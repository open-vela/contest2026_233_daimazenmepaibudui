/****************************************************************************
 * app/hello_app/mimo_voice.c
 *
 * 小米 MiMo 云端语音后端（ASR + TTS），普通 HTTPS POST 实现。
 *
 * 为什么另写一个后端：
 *   packages/ai_agent/src/voice/ 下的 volc_asr.c / volc_tts.c 是上游 openvela
 *   的实现（ASR 自搓 WebSocket 二进制协议，TTS 走 V3 单向流），凭据要求火山
 *   引擎的 appkey / token。本工程的凭据是 token-plan 的 MiMo key，所以这里按
 *   voice_asr_ops_t / voice_tts_ops_t 的接口另写一份，用 mimo_asr_register() /
 *   mimo_tts_register() 挂进同一个分发层（voice_asr.c / voice_tts.c），
 *   上游那些文件一行都不用改。
 *
 * 接口（和 LLM 同一个 host + path，只有 model 不同；PC 上实测 200）：
 *   ASR: POST <path>
 *        {"model":"mimo-v2.5-asr","messages":[{"role":"user","content":[
 *           {"type":"input_audio","input_audio":
 *              {"data":"<base64(整个 WAV，含 44 字节头)>","format":"wav"}}]}]}
 *        -> choices[0].message.content
 *   TTS: POST <path>
 *        {"model":"mimo-v2.5-tts","messages":[{"role":"assistant",
 *           "content":"<要合成的文本>"}]}
 *        （**必须 assistant 角色**；写成 user 服务端会报
 *          "messages must contain an assistant role for TTS model"）
 *        -> choices[0].message.audio.data = base64(WAV)
 *           这个 WAV 是 24 kHz / 单声道 / 16 bit，而本工程整条链是
 *           16 kHz / 单声道 / s16le，所以这里线性插值重采样到 16k 再交出去。
 *
 * 凭据：
 *   从 claw_config_get() 读 llm_host / api_key（开机由板级代码把
 *   /etc/assets/agent_config.json 拷进 /data/ai_agent/config/config.json）。
 *   本文件没有硬编码 key；日志只报长度 / 状态 / host，绝不打 Authorization 头
 *   或 base64 音频（出错时才打响应体前 200 字节）。
 *
 * 内存：
 *   请求体、响应体、base64 编解码缓冲全部走堆（大块分配会落到 PSRAM）。
 *   TTS 单次可合成的时长由 MIMO_TTS_RESP_CAP 决定，见该宏注释。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "mimo_voice.h"

#include "agent_compat.h"
#include "agent_config.h"
#include "infra/config_store.h"
#include "infra/vela_tls.h"
#include "voice/voice_asr.h"
#include "voice/voice_tts.h"

#include "mbedtls/base64.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 后端名字（voice_asr_set_backend("mimo") / voice_tts_set_backend("mimo")） */

#define MIMO_BACKEND_NAME "mimo"

#define MIMO_ASR_MODEL    "mimo-v2.5-asr"
#define MIMO_TTS_MODEL    "mimo-v2.5-tts"

/* host / path 都从配置读（和 LLM 共用）；这两个只是兜底默认值，
 * 配置里没有 llm_path / llm_port 时用得上。host 没有默认值：
 * 读不到 llm_host 直接返回错误，绝不硬编码任何 host/key。 */

#define MIMO_DEFAULT_PATH "/v1/chat/completions"
#define MIMO_DEFAULT_PORT "443"

/* 整条语音链路的格式：16 kHz / 单声道 / 16 bit */

#define MIMO_PCM_RATE     16000
#define MIMO_PCM_CHANNELS 1
#define MIMO_PCM_BITS     16
#define MIMO_WAV_HDR_LEN  44

/* ASR 响应很小（就是一段文字），TTS 响应是一条 base64 音频。
 * 实测（PC 上真打这个接口）：13 个汉字的 MiMo TTS 回包 143907 字节
 * （base64 143420 字符 -> WAV 107564 字节 = 24 kHz 的 2.24 秒），
 * 也就是每字约 11 KB。所以：
 *   512 KB ≈ 46 字 / 8 秒，1 MB ≈ 90 字 / 16 秒。
 * 这里给 1 MB，覆盖"AI 说一两句话"的常见长度；文本再长响应就会被这个上限
 * 截断（JSON 不完整 -> 报 -EPROTO，不会放出半截声音，日志里有明确提示）。
 * 堆里有 8 MB PSRAM：峰值是 resp 1 MB + base64 解码 ~750 KB + 16k PCM ~500 KB
 * ≈ 2.3 MB，用完即 free。 */

#define MIMO_ASR_RESP_CAP (16 * 1024)
#define MIMO_TTS_RESP_CAP (1024 * 1024)

/* 应答里最多往串口打多少字节（只在出错时打） */

#define MIMO_ERR_DUMP_LEN 200

/* ASR 请求的 JSON 骨架：base64 的 WAV 直接填在中间的空档里（不经过 cJSON，
 * 省掉一整份字符串拷贝 —— hello_app 的编译参数里也没有 cJSON 的 include 路径）。 */

#define MIMO_ASR_JSON_PREFIX                                             \
  "{\"model\":\"" MIMO_ASR_MODEL "\",\"messages\":[{\"role\":\"user\"," \
  "\"content\":[{\"type\":\"input_audio\",\"input_audio\":{\"data\":\""

#define MIMO_ASR_JSON_SUFFIX "\",\"format\":\"wav\"}}]}]}"

#define MIMO_TTS_JSON_PREFIX                                             \
  "{\"model\":\"" MIMO_TTS_MODEL "\",\"messages\":[{\"role\":"          \
  "\"assistant\",\"content\":\""

#define MIMO_TTS_JSON_SUFFIX "\"}]}"

/* 单次读进来的 WAV 文件上限（16k 单声道 16bit 的 32 秒） */

#define MIMO_WAV_MAX_FILE (1024 * 1024)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct wav_info_s
{
  unsigned int rate;
  unsigned int channels;
  unsigned int bits;
  size_t       data_off;
  size_t       data_len;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char *TAG = "mimo_voice";

/* 配置快照（每次调用前重新读一遍，配置改了不用重启） */

static char s_host[128];
static char s_key[128];
static char s_path[128];
static char s_port[8];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: mimo_load_config
 *
 * Description:
 *   把 llm_host / api_key / llm_path / llm_port 读进静态快照。
 *   host 和 key 缺一个都算不可用（不给默认值，避免用错凭据去打别人的服务）。
 *
 ****************************************************************************/

static int mimo_load_config(void)
{
  s_host[0] = '\0';
  s_key[0]  = '\0';
  s_path[0] = '\0';
  s_port[0] = '\0';

  if (claw_config_get(AGENT_CFG_KEY_LLM_HOST, s_host, sizeof(s_host)) != OK
      || s_host[0] == '\0')
    {
      return -ENOENT;
    }

  if (claw_config_get(AGENT_CFG_KEY_API_KEY, s_key, sizeof(s_key)) != OK
      || s_key[0] == '\0')
    {
      return -ENOENT;
    }

  if (claw_config_get(AGENT_CFG_KEY_LLM_PATH, s_path, sizeof(s_path)) != OK
      || s_path[0] == '\0')
    {
      strncpy(s_path, MIMO_DEFAULT_PATH, sizeof(s_path) - 1);
    }

  /* llm_port 没有独立的宏，llm_proxy.c 也是直接写 "llm_port" */

  if (claw_config_get("llm_port", s_port, sizeof(s_port)) != OK
      || s_port[0] == '\0')
    {
      strncpy(s_port, MIMO_DEFAULT_PORT, sizeof(s_port) - 1);
    }

  return OK;
}

/****************************************************************************
 * Name: mimo_post
 *
 * Description:
 *   POST 一段 JSON 到 <llm_host>:<llm_port><llm_path>，复用 ai_agent 的
 *   vela_https_post_json()（它自己加 Content-Type: application/json）。
 *   这里只补 Authorization: Bearer <api_key>。
 *
 *   不加 Connection 头：ai_agent 的 vela_tls.c 在 tls_write_request() 里已经
 *   写死了 "Connection: keep-alive"，再传一个会出现重复头（volc_tts.c 就是这么
 *   传的，能通，但没必要冒这个险）。
 *
 ****************************************************************************/

static int mimo_post(const char *body, char *resp, size_t resp_cap)
{
  char auth[192];

  /* 只在本函数里用，绝不打印 */

  snprintf(auth, sizeof(auth), "Bearer %s", s_key);

  vela_header_t hdrs[] = {
    { "Authorization", auth },
    { NULL, NULL }
  };

  return vela_https_post_json(s_host, s_port, s_path, hdrs, body,
                              resp, resp_cap);
}

/****************************************************************************
 * Name: mimo_log_http_error
 *
 * Description:
 *   出错时打状态码 + 响应体前 200 字节。响应体可能是服务端的错误 JSON，
 *   截断打印足够定位问题；成功路径（含 base64 音频）不打。
 *
 ****************************************************************************/

static void mimo_log_http_error(const char *what, int status,
                                const char *resp)
{
  if (status < 0)
    {
      syslog(LOG_ERR, "[%s] %s: HTTPS 失败 %d\n", TAG, what, status);
      return;
    }

  syslog(LOG_ERR, "[%s] %s: HTTP %d, 响应 %.*s\n", TAG, what, status,
         MIMO_ERR_DUMP_LEN,
         (resp != NULL && resp[0] != '\0') ? resp : "(空)");
}

/****************************************************************************
 * Name: hex4 / utf8_put / json_unescape / json_escape
 *
 * Description:
 *   极简 JSON 字符串编解码：不引 cJSON（hello_app 的编译参数里没有它的
 *   include 路径，而且大 base64 过一遍 cJSON 会多一整份拷贝，板子上不划算）。
 *
 *   json_find_string() 只做"找键 -> 拿引号内的原始字节"，不复制、不反转义
 *   （base64 直接原地解码）；文本类字段再用 json_unescape() 还原 UTF-8。
 *
 ****************************************************************************/

static int hex4(const char *p, unsigned int *out)
{
  unsigned int v = 0;
  int i;

  for (i = 0; i < 4; i++)
    {
      char c = p[i];

      v <<= 4;

      if (c >= '0' && c <= '9')
        {
          v |= (unsigned int)(c - '0');
        }
      else if (c >= 'a' && c <= 'f')
        {
          v |= (unsigned int)(c - 'a' + 10);
        }
      else if (c >= 'A' && c <= 'F')
        {
          v |= (unsigned int)(c - 'A' + 10);
        }
      else
        {
          return -1;
        }
    }

  *out = v;
  return 0;
}

static size_t utf8_put(unsigned int cp, char *dst)
{
  if (cp < 0x80)
    {
      dst[0] = (char)cp;
      return 1;
    }

  if (cp < 0x800)
    {
      dst[0] = (char)(0xc0 | (cp >> 6));
      dst[1] = (char)(0x80 | (cp & 0x3f));
      return 2;
    }

  if (cp < 0x10000)
    {
      dst[0] = (char)(0xe0 | (cp >> 12));
      dst[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
      dst[2] = (char)(0x80 | (cp & 0x3f));
      return 3;
    }

  dst[0] = (char)(0xf0 | (cp >> 18));
  dst[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
  dst[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
  dst[3] = (char)(0x80 | (cp & 0x3f));
  return 4;
}

static const char *json_find_string(const char *json, const char *key,
                                    size_t *out_len)
{
  char pat[64];
  const char *p;

  if (json == NULL || key == NULL || out_len == NULL)
    {
      return NULL;
    }

  if (strlen(key) + 3 > sizeof(pat))
    {
      return NULL;
    }

  snprintf(pat, sizeof(pat), "\"%s\"", key);

  p = strstr(json, pat);
  if (p == NULL)
    {
      return NULL;
    }

  p += strlen(pat);

  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    {
      p++;
    }

  if (*p != ':')
    {
      return NULL;
    }

  p++;

  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    {
      p++;
    }

  if (*p != '"')
    {
      return NULL;
    }

  p++;

  /* 扫到配对的结束引号（跳过 \"）。扫不到引号 = 响应被截断了。 */

  {
    const char *start = p;

    while (*p != '\0' && *p != '"')
      {
        if (*p == '\\' && p[1] != '\0')
          {
            p++;
          }

        p++;
      }

    if (*p != '"')
      {
        return NULL;
      }

    *out_len = (size_t)(p - start);
    return start;
  }
}

static size_t json_unescape(const char *src, size_t len, char *dst,
                            size_t dst_cap)
{
  size_t i = 0;
  size_t o = 0;

  while (i < len && o < dst_cap)
    {
      char c = src[i++];

      if (c != '\\' || i >= len)
        {
          dst[o++] = c;
          continue;
        }

      c = src[i++];

      switch (c)
        {
          case '"':  dst[o++] = '"';  break;
          case '\\': dst[o++] = '\\'; break;
          case '/':  dst[o++] = '/';  break;
          case 'b':  dst[o++] = '\b'; break;
          case 'f':  dst[o++] = '\f'; break;
          case 'n':  dst[o++] = '\n'; break;
          case 'r':  dst[o++] = '\r'; break;
          case 't':  dst[o++] = '\t'; break;

          case 'u':
            {
              unsigned int cp;

              if (i + 4 > len || hex4(src + i, &cp) != 0)
                {
                  dst[o++] = '?';
                  break;
                }

              i += 4;

              /* UTF-16 代理对：高代理 + \uXXXX 低代理合成一个码点 */

              if (cp >= 0xd800 && cp <= 0xdbff && i + 6 <= len
                  && src[i] == '\\' && src[i + 1] == 'u')
                {
                  unsigned int lo;

                  if (hex4(src + i + 2, &lo) == 0
                      && lo >= 0xdc00 && lo <= 0xdfff)
                    {
                      cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                      i += 6;
                    }
                }

              if (o + 4 > dst_cap)
                {
                  break;
                }

              o += utf8_put(cp, dst + o);
            }
            break;

          default:
            dst[o++] = c;
            break;
        }
    }

  return o;
}

static size_t json_escape(const char *src, size_t len, char *dst,
                          size_t dst_cap)
{
  size_t i;
  size_t o = 0;

  for (i = 0; i < len; i++)
    {
      unsigned char c = (unsigned char)src[i];
      char tmp[8];
      size_t n;

      switch (c)
        {
          case '"':  tmp[0] = '\\'; tmp[1] = '"';  n = 2; break;
          case '\\': tmp[0] = '\\'; tmp[1] = '\\'; n = 2; break;
          case '\n': tmp[0] = '\\'; tmp[1] = 'n';  n = 2; break;
          case '\r': tmp[0] = '\\'; tmp[1] = 'r';  n = 2; break;
          case '\t': tmp[0] = '\\'; tmp[1] = 't';  n = 2; break;

          default:
            if (c < 0x20)
              {
                static const char hexd[] = "0123456789abcdef";

                tmp[0] = '\\';
                tmp[1] = 'u';
                tmp[2] = '0';
                tmp[3] = '0';
                tmp[4] = hexd[(c >> 4) & 0xf];
                tmp[5] = hexd[c & 0xf];
                n = 6;
              }
            else
              {
                tmp[0] = (char)c;
                n = 1;
              }
            break;
        }

      if (o + n > dst_cap)
        {
          break;
        }

      memcpy(dst + o, tmp, n);
      o += n;
    }

  return o;
}

/****************************************************************************
 * Name: rd_le16 / rd_le32 / wr_le16 / wr_le32
 *
 * Description:
 *   小端读写。WAV 头按字节拼，避开"把任意偏移强转成 int16_t*"的对齐问题。
 *
 ****************************************************************************/

static unsigned int rd_le16(const unsigned char *p)
{
  return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static unsigned long rd_le32(const unsigned char *p)
{
  return (unsigned long)p[0] | ((unsigned long)p[1] << 8)
         | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static void wr_le16(unsigned char *p, unsigned int v)
{
  p[0] = (unsigned char)(v & 0xff);
  p[1] = (unsigned char)((v >> 8) & 0xff);
}

static void wr_le32(unsigned char *p, unsigned long v)
{
  p[0] = (unsigned char)(v & 0xff);
  p[1] = (unsigned char)((v >> 8) & 0xff);
  p[2] = (unsigned char)((v >> 16) & 0xff);
  p[3] = (unsigned char)((v >> 24) & 0xff);
}

/****************************************************************************
 * Name: wav_write_header
 *
 * Description:
 *   写 44 字节标准 PCM WAV 头（ASR 要把整段 WAV 传上去，服务端按 fmt 块解析）。
 *
 ****************************************************************************/

static void wav_write_header(unsigned char *hdr, size_t pcm_len)
{
  unsigned int block = MIMO_PCM_CHANNELS * MIMO_PCM_BITS / 8;

  memcpy(hdr, "RIFF", 4);
  wr_le32(hdr + 4, (unsigned long)(36 + pcm_len));
  memcpy(hdr + 8, "WAVEfmt ", 8);
  wr_le32(hdr + 16, 16);                            /* fmt 块长度 */
  wr_le16(hdr + 20, 1);                             /* 1 = 未压缩 PCM */
  wr_le16(hdr + 22, MIMO_PCM_CHANNELS);
  wr_le32(hdr + 24, MIMO_PCM_RATE);
  wr_le32(hdr + 28, (unsigned long)MIMO_PCM_RATE * block);
  wr_le16(hdr + 32, block);
  wr_le16(hdr + 34, MIMO_PCM_BITS);
  memcpy(hdr + 36, "data", 4);
  wr_le32(hdr + 40, (unsigned long)pcm_len);
}

/****************************************************************************
 * Name: wav_parse
 *
 * Description:
 *   扫 RIFF 块，找 fmt / data。只认未压缩 PCM（fmt=1）16bit。
 *
 ****************************************************************************/

static int wav_parse(const unsigned char *buf, size_t len,
                     struct wav_info_s *info)
{
  size_t pos = 12;

  if (len < MIMO_WAV_HDR_LEN)
    {
      return -EINVAL;
    }

  if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0)
    {
      return -EINVAL;
    }

  memset(info, 0, sizeof(*info));

  while (pos + 8 <= len)
    {
      const unsigned char *id = buf + pos;
      unsigned long sz = rd_le32(buf + pos + 4);
      size_t body = pos + 8;

      if (memcmp(id, "fmt ", 4) == 0)
        {
          unsigned int fmt_tag;

          if (sz < 16 || body + 16 > len)
            {
              return -EINVAL;
            }

          fmt_tag = rd_le16(buf + body);

          if (fmt_tag != 1)
            {
              /* WAVE_FORMAT_EXTENSIBLE(0xFFFE)：真正的格式在扩展块开头
               * 那 2 字节（PCM 就是 1），后面各字段偏移不变。
               * 扩展块布局：cbSize(16) / valid bits(18) / channel mask(20) /
               * SubFormat GUID(24)，所以 SubFormat 的 tag 在 body+24。 */

              if (fmt_tag != 0xfffe || sz < 26 || body + 26 > len
                  || rd_le16(buf + body + 24) != 1)
                {
                  return -ENOSYS;   /* 非 PCM（浮点/ADPCM）不支持 */
                }
            }

          info->channels = rd_le16(buf + body + 2);
          info->rate     = (unsigned int)rd_le32(buf + body + 4);
          info->bits     = rd_le16(buf + body + 14);

          /* data 块可能出现在 fmt 前面，反过来再找一遍 */

          if (info->data_len != 0)
            {
              return OK;
            }
        }
      else if (memcmp(id, "data", 4) == 0)
        {
          info->data_off = body;
          info->data_len = (size_t)sz;

          if (info->data_off + info->data_len > len)
            {
              info->data_len = len - info->data_off;   /* 文件被截短 */
            }

          if (info->rate != 0 && info->data_len != 0)
            {
              return OK;
            }
        }
      else if ((size_t)sz > len - body)
        {
          break;                 /* 块长度超出文件，别把 pos 算溢出 */
        }

      pos = body + (size_t)sz + (sz & 1);   /* 块按偶数字节对齐 */
    }

  if (info->rate == 0 || info->data_len == 0)
    {
      return -EINVAL;
    }

  return OK;
}

/****************************************************************************
 * Name: resample_to_16k
 *
 * Description:
 *   int16 线性插值重采样到 16 kHz（24k -> 16k 是 3:2），多声道只取第 0 声道。
 *   源缓冲按字节读（WAV 的 data 段偏移不保证 2 字节对齐），所以这里收
 *   unsigned char* + channels，而不是 int16_t*。
 *   采样率已经是 16k 且单声道时直接拷贝。返回写进 dst 的采样点数。
 *
 ****************************************************************************/

static size_t resample_to_16k(const unsigned char *src, size_t frames,
                              unsigned int rate, unsigned int channels,
                              int16_t *dst, size_t dst_frames)
{
  size_t stride = (size_t)channels * 2;
  uint64_t step;
  uint64_t pos = 0;
  size_t n = 0;

  if (src == NULL || dst == NULL || frames == 0 || dst_frames == 0
      || channels == 0)
    {
      return 0;
    }

  if (rate == MIMO_PCM_RATE)
    {
      if (frames > dst_frames)
        {
          frames = dst_frames;
        }

      if (channels == 1)
        {
          memcpy(dst, src, frames * sizeof(int16_t));
        }
      else
        {
          size_t i;

          for (i = 0; i < frames; i++)
            {
              dst[i] = (int16_t)rd_le16(src + i * stride);
            }
        }

      return frames;
    }

  if (rate == 0)
    {
      return 0;
    }

  /* 定点：pos 的单位是 1/65536 个输入采样，step = rate/16000 * 65536 */

  step = ((uint64_t)rate << 16) / MIMO_PCM_RATE;

  if (step == 0)
    {
      return 0;                            /* 输入采样率低于 1 Hz，防死循环 */
    }

  while (n < dst_frames)
    {
      size_t idx = (size_t)(pos >> 16);
      unsigned int frac;
      int32_t a;
      int32_t b;

      if (idx + 1 >= frames)
        {
          break;                           /* 最后一个采样没有右邻居，收尾 */
        }

      frac = (unsigned int)(pos & 0xffff);
      a = (int16_t)rd_le16(src + idx * stride);
      b = (int16_t)rd_le16(src + (idx + 1) * stride);
      dst[n++] = (int16_t)(a + ((int32_t)(((int64_t)(b - a) * frac) >> 16)));
      pos += step;
    }

  return n;
}

/****************************************************************************
 * Name: wav_extract_16k
 *
 * Description:
 *   从一段 WAV 内存里取出 PCM，转成 16 kHz / 单声道 / s16le。
 *   多声道只取第 0 声道（本工程的链路就是单声道）。
 *   只分配输出缓冲（不额外建 mono 中转），TTS 那种 24k 大 WAV 省一份内存。
 *
 ****************************************************************************/

static int wav_extract_16k(const unsigned char *buf, size_t len,
                           unsigned char **pcm_out, size_t *pcm_len)
{
  struct wav_info_s info;
  size_t frames;
  size_t out_frames;
  int16_t *out;
  size_t got;
  int ret;

  *pcm_out = NULL;
  *pcm_len = 0;

  ret = wav_parse(buf, len, &info);
  if (ret != OK)
    {
      return ret;
    }

  if (info.bits != 16)
    {
      return -ENOSYS;                      /* 只支持 16bit */
    }

  if (info.channels < 1 || info.channels > 2)
    {
      return -ENOSYS;
    }

  frames = info.data_len / ((size_t)info.channels * 2);
  if (frames == 0)
    {
      return -EINVAL;
    }

  out_frames = (size_t)(((uint64_t)frames * MIMO_PCM_RATE) / info.rate);
  if (out_frames == 0)
    {
      return -EINVAL;
    }

  out = malloc((out_frames + 1) * sizeof(int16_t));
  if (out == NULL)
    {
      return -ENOMEM;
    }

  got = resample_to_16k(buf + info.data_off, frames, info.rate,
                        info.channels, out, out_frames);

  if (got == 0)
    {
      free(out);
      return -EINVAL;
    }

  *pcm_out = (unsigned char *)out;
  *pcm_len = got * sizeof(int16_t);
  return OK;
}

/****************************************************************************
 * Name: b64_decode_alloc
 *
 * Description:
 *   base64 解码到新分配的缓冲（先用 dlen=0 问 mbedtls 要精确长度，
 *   不多占一字节 —— 几十万字节的响应下这点很重要）。
 *
 ****************************************************************************/

static int b64_decode_alloc(const char *b64, size_t b64_len,
                            unsigned char **out, size_t *out_len)
{
  unsigned char *buf;
  size_t need = 0;
  size_t got = 0;
  int ret;

  *out = NULL;
  *out_len = 0;

  ret = mbedtls_base64_decode(NULL, 0, &need,
                              (const unsigned char *)b64, b64_len);
  if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || need == 0)
    {
      return -EINVAL;
    }

  buf = malloc(need);
  if (buf == NULL)
    {
      return -ENOMEM;
    }

  ret = mbedtls_base64_decode(buf, need, &got,
                              (const unsigned char *)b64, b64_len);
  if (ret != 0 || got == 0)
    {
      free(buf);
      return -EINVAL;
    }

  *out = buf;
  *out_len = got;
  return OK;
}

/****************************************************************************
 * Name: mimo_asr_recognize
 *
 * Description:
 *   voice_asr_ops_t.recognize：把 16k 单声道 s16le 的 PCM 包成 WAV、
 *   base64 后 POST 给 MiMo ASR，取 choices[0].message.content。
 *
 ****************************************************************************/

static int mimo_asr_recognize(const unsigned char *pcm_data, size_t pcm_len,
                              char *text_out, size_t text_cap)
{
  const char *prefix = MIMO_ASR_JSON_PREFIX;
  const char *suffix = MIMO_ASR_JSON_SUFFIX;
  size_t pfx_len = strlen(prefix);
  size_t sfx_len = strlen(suffix);
  size_t wav_len;
  size_t b64_need = 0;
  size_t b64_len = 0;
  unsigned char *wav;
  char *body;
  char *resp;
  const char *content;
  size_t content_len = 0;
  size_t out_len;
  int status;
  int ret;

  if (pcm_data == NULL || pcm_len == 0 || text_out == NULL
      || text_cap == 0)
    {
      return -EINVAL;
    }

  text_out[0] = '\0';

  if (mimo_load_config() != OK)
    {
      syslog(LOG_ERR, "[%s] ASR: 配置里缺 llm_host / api_key，MiMo 不可用\n",
             TAG);
      return -ENOENT;
    }

  /* 1. PCM -> WAV（服务端要的就是"整个 WAV 文件"，含 44 字节头） */

  wav_len = MIMO_WAV_HDR_LEN + pcm_len;
  wav = malloc(wav_len);
  if (wav == NULL)
    {
      return -ENOMEM;
    }

  wav_write_header(wav, pcm_len);
  memcpy(wav + MIMO_WAV_HDR_LEN, pcm_data, pcm_len);

  /* 2. 直接拼请求体：前缀 + base64 + 后缀，省掉中间那份 base64 字符串 */

  ret = mbedtls_base64_encode(NULL, 0, &b64_need, wav, wav_len);
  if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || b64_need == 0)
    {
      free(wav);
      return -EIO;
    }

  body = malloc(pfx_len + b64_need + sfx_len + 1);
  if (body == NULL)
    {
      free(wav);
      return -ENOMEM;
    }

  memcpy(body, prefix, pfx_len);

  ret = mbedtls_base64_encode((unsigned char *)body + pfx_len,
                              b64_need, &b64_len, wav, wav_len);
  free(wav);

  if (ret != 0)
    {
      free(body);
      return -EIO;
    }

  memcpy(body + pfx_len + b64_len, suffix, sfx_len + 1);

  syslog(LOG_INFO, "[%s] ASR: PCM %zu 字节 -> WAV %zu 字节 -> base64 %zu 字节\n",
         TAG, pcm_len, wav_len, b64_len);

  /* 3. 请求。只报长度，不报 body（里面是音频） */

  resp = calloc(1, MIMO_ASR_RESP_CAP);
  if (resp == NULL)
    {
      free(body);
      return -ENOMEM;
    }

  status = mimo_post(body, resp, MIMO_ASR_RESP_CAP);
  free(body);

  if (status != 200)
    {
      mimo_log_http_error("ASR", status, resp);
      free(resp);
      return -EIO;
    }

  content = json_find_string(resp, "content", &content_len);

  if (content == NULL)
    {
      content = json_find_string(resp, "text", &content_len);
    }

  if (content == NULL)
    {
      syslog(LOG_ERR, "[%s] ASR: 响应里没有 content/text: %.*s\n", TAG,
             MIMO_ERR_DUMP_LEN, resp[0] != '\0' ? resp : "(空)");
      free(resp);
      return -EPROTO;
    }

  out_len = json_unescape(content, content_len, text_out, text_cap - 1);
  text_out[out_len] = '\0';

  syslog(LOG_INFO, "[%s] ASR: 识别结果 %zu 字节\n", TAG, out_len);

  free(resp);
  return OK;
}

/****************************************************************************
 * Name: mimo_tts_synthesize
 *
 * Description:
 *   voice_tts_ops_t.synthesize：POST 文本给 MiMo TTS，取
 *   choices[0].message.audio.data（base64 WAV，24 kHz），
 *   解码 + 重采样成 16 kHz / 单声道 / s16le 写进 pcm_out。
 *
 ****************************************************************************/

static int mimo_tts_synthesize(const char *text, unsigned char *pcm_out,
                               size_t pcm_cap, size_t *pcm_len)
{
  const char *prefix = MIMO_TTS_JSON_PREFIX;
  const char *suffix = MIMO_TTS_JSON_SUFFIX;
  size_t pfx_len = strlen(prefix);
  size_t sfx_len = strlen(suffix);
  size_t text_len;
  char *esc;
  size_t esc_len;
  char *body;
  char *resp;
  const char *audio;
  const char *b64;
  size_t b64_len = 0;
  unsigned char *decoded = NULL;
  size_t decoded_len = 0;
  unsigned char *pcm16 = NULL;
  size_t pcm16_len = 0;
  size_t copy;
  int status;
  int ret;

  if (text == NULL || pcm_out == NULL || pcm_len == NULL || pcm_cap == 0)
    {
      return -EINVAL;
    }

  *pcm_len = 0;

  if (mimo_load_config() != OK)
    {
      syslog(LOG_ERR, "[%s] TTS: 配置里缺 llm_host / api_key，MiMo 不可用\n",
             TAG);
      return -ENOENT;
    }

  text_len = strlen(text);
  if (text_len == 0)
    {
      return -EINVAL;
    }

  /* 1. 文本 JSON 转义后拼请求体（role 必须是 assistant） */

  esc = malloc(text_len * 6 + 1);
  if (esc == NULL)
    {
      return -ENOMEM;
    }

  esc_len = json_escape(text, text_len, esc, text_len * 6);
  esc[esc_len] = '\0';

  body = malloc(pfx_len + esc_len + sfx_len + 1);
  if (body == NULL)
    {
      free(esc);
      return -ENOMEM;
    }

  memcpy(body, prefix, pfx_len);
  memcpy(body + pfx_len, esc, esc_len);
  memcpy(body + pfx_len + esc_len, suffix, sfx_len + 1);
  free(esc);

  syslog(LOG_INFO, "[%s] TTS: 请求 %zu 字节文本\n", TAG, text_len);

  /* 2. 请求（响应可能有几十万字节，缓冲走堆） */

  resp = calloc(1, MIMO_TTS_RESP_CAP);
  if (resp == NULL)
    {
      free(body);
      return -ENOMEM;
    }

  status = mimo_post(body, resp, MIMO_TTS_RESP_CAP);
  free(body);

  if (status != 200)
    {
      mimo_log_http_error("TTS", status, resp);
      free(resp);
      return -EIO;
    }

  /* 3. 取 message.audio.data。先在 "audio" 之后再找 "data"，
   *    免得命中响应里别的同名字段。找不到多半是响应被 resp_cap 截断。 */

  audio = strstr(resp, "\"audio\"");
  if (audio != NULL)
    {
      b64 = json_find_string(audio, "data", &b64_len);
    }
  else
    {
      b64 = NULL;
    }

  if (b64 == NULL)
    {
      syslog(LOG_ERR,
             "[%s] TTS: 响应里没有 audio.data（合成文本太长、响应超过 %d 字节"
             "被截断？）: %.*s\n",
             TAG, MIMO_TTS_RESP_CAP, MIMO_ERR_DUMP_LEN,
             resp[0] != '\0' ? resp : "(空)");
      free(resp);
      return -EPROTO;
    }

  syslog(LOG_INFO, "[%s] TTS: 收到 base64 音频 %zu 字节\n", TAG, b64_len);

  ret = b64_decode_alloc(b64, b64_len, &decoded, &decoded_len);
  free(resp);
  resp = NULL;

  if (ret != OK)
    {
      syslog(LOG_ERR, "[%s] TTS: base64 解码失败 %d\n", TAG, ret);
      return ret;
    }

  /* 4. WAV -> 16k 单声道 s16le（MiMo 回的是 24 kHz） */

  ret = wav_extract_16k(decoded, decoded_len, &pcm16, &pcm16_len);
  free(decoded);

  if (ret != OK)
    {
      syslog(LOG_ERR, "[%s] TTS: WAV 解析失败 %d\n", TAG, ret);
      return ret;
    }

  copy = pcm16_len;
  if (copy > pcm_cap)
    {
      /* 不整段丢弃：装得下多少给多少（和 volc_tts.c 的 PCM buffer full
       * 处理一致），调用方看 *pcm_len 就知道被截了。 */

      syslog(LOG_WARNING,
             "[%s] TTS: 输出缓冲只有 %zu 字节，只放得下 %zu/%zu 字节\n",
             TAG, pcm_cap, pcm_cap, pcm16_len);
      copy = pcm_cap;
    }

  memcpy(pcm_out, pcm16, copy);
  free(pcm16);

  *pcm_len = copy;

  syslog(LOG_INFO, "[%s] TTS: 输出 16k PCM %zu 字节（约 %zu ms）\n", TAG,
         *pcm_len, *pcm_len * 1000 / (MIMO_PCM_RATE * 2));
  return OK;
}

/****************************************************************************
 * Name: mimo_init
 *
 * Description:
 *   后端 init 回调：只做一次配置检查 + 打日志。失败不算注册失败
 *   （分发层忽略返回值），真正调用时还会再检查一遍。
 *
 ****************************************************************************/

static int mimo_init(void)
{
  int ret = mimo_load_config();

  if (ret != OK)
    {
      syslog(LOG_WARNING,
             "[%s] 未配置 llm_host / api_key，MiMo 语音后端调用会返回 -ENOENT\n",
             TAG);
      return ret;
    }

  /* 只打 host / path，不打 key */

  syslog(LOG_INFO, "[%s] MiMo 后端就绪: host=%s path=%s port=%s\n", TAG,
         s_host, s_path, s_port);
  return OK;
}

/****************************************************************************
 * Private Data（后端 ops）
 ****************************************************************************/

static const voice_asr_ops_t s_mimo_asr_ops = {
  .name      = MIMO_BACKEND_NAME,
  .init      = mimo_init,
  .recognize = mimo_asr_recognize,
  .deinit    = NULL,
};

static const voice_tts_ops_t s_mimo_tts_ops = {
  .name       = MIMO_BACKEND_NAME,
  .init       = mimo_init,
  .synthesize = mimo_tts_synthesize,
  .deinit     = NULL,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int mimo_asr_register(void)
{
  return voice_asr_register(&s_mimo_asr_ops);
}

int mimo_tts_register(void)
{
  return voice_tts_register(&s_mimo_tts_ops);
}

int mimo_voice_available(void)
{
  char host[128];
  char key[128];

  host[0] = '\0';
  key[0] = '\0';

  if (claw_config_get(AGENT_CFG_KEY_LLM_HOST, host, sizeof(host)) != OK
      || host[0] == '\0')
    {
      return 0;
    }

  if (claw_config_get(AGENT_CFG_KEY_API_KEY, key, sizeof(key)) != OK
      || key[0] == '\0')
    {
      return 0;
    }

  return 1;
}

int mimo_wav_load_16k(const char *path, unsigned char **pcm_out,
                      size_t *pcm_len)
{
  FILE *fp;
  long size;
  unsigned char *raw;
  size_t got;
  int ret;

  if (path == NULL || pcm_out == NULL || pcm_len == NULL)
    {
      return -EINVAL;
    }

  *pcm_out = NULL;
  *pcm_len = 0;

  fp = fopen(path, "rb");
  if (fp == NULL)
    {
      syslog(LOG_ERR, "[%s] 打不开 WAV: %s (%d)\n", TAG, path, errno);
      return -ENOENT;
    }

  if (fseek(fp, 0, SEEK_END) != 0)
    {
      fclose(fp);
      return -EIO;
    }

  size = ftell(fp);
  if (size <= MIMO_WAV_HDR_LEN || size > MIMO_WAV_MAX_FILE)
    {
      syslog(LOG_ERR, "[%s] WAV 大小不合适: %ld 字节（上限 %d）\n", TAG,
             size, MIMO_WAV_MAX_FILE);
      fclose(fp);
      return -EINVAL;
    }

  rewind(fp);

  raw = malloc((size_t)size);
  if (raw == NULL)
    {
      fclose(fp);
      return -ENOMEM;
    }

  got = fread(raw, 1, (size_t)size, fp);
  fclose(fp);

  if (got != (size_t)size)
    {
      syslog(LOG_ERR, "[%s] 读 WAV 只拿到 %zu/%ld 字节\n", TAG, got, size);
      free(raw);
      return -EIO;
    }

  ret = wav_extract_16k(raw, got, pcm_out, pcm_len);
  free(raw);

  if (ret != OK)
    {
      syslog(LOG_ERR, "[%s] WAV 解析/重采样失败: %d\n", TAG, ret);
      return ret;
    }

  syslog(LOG_INFO, "[%s] %s -> %zu 字节 16k 单声道 PCM\n", TAG, path,
         *pcm_len);
  return OK;
}
