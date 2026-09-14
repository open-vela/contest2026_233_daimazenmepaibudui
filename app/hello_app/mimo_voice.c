/****************************************************************************
 * app/hello_app/mimo_voice.c
 *
 * 小米 MiMo 云端后端（ASR + TTS + 对话 chat），普通 HTTPS POST 实现。
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
 *   对话: mimo_chat()（给别的 app 用的同步接口，不经过 ai_agent 进程）
 *        请求体在运行时拼，带 3 轮上限的工具循环；两个工具各自独立开关
 *        （add_reminder 默认开、get_weather 默认关，见 MIMO_CFG_KEY_*）：
 *        两个都关（默认单轮）
 *        {"model":"<配置里的 model>",
 *         "messages":[{"role":"system","content":"<当前时间 + 说话要求>"},
 *                     {"role":"user","content":"<转义文本>"}],
 *         "max_tokens":1024}
 *        至少开一个时多一段 tools（数组里的成员按开关拼，见 MIMO_TOOL_*_JSON）：
 *        "tools":[{"type":"function","function":{"name":"add_reminder",...}},
 *                 {"type":"function","function":{"name":"get_weather",...}}]
 *        -> finish_reason="tool_calls" 时，取 message.tool_calls[0] 的
 *           function.name / .arguments 按名字分派：
 *             add_reminder(time,title) -> 交给 app 注册的
 *               mimo_set_reminder_hook() 实现落地（robot_ui 那边写进提醒列表
 *               并重挂 RTC 闹钟），工具结果是"已建好：每天 08:00 提醒用户吃药"；
 *             get_weather(city) -> 板子替它打 open-meteo 查
 *               （geocode + forecast 两个免 key 的 HTTPS GET），工具结果是
 *               中文实况；
 *           把 {"role":"assistant","content":null,"tool_calls":[...]} 和
 *           {"role":"tool","tool_call_id":"<id>","content":"<工具结果>"}
 *           两条消息追加进 messages 再发一轮，让模型用口语复述给用户；
 *        -> 模型不问工具时第 1 轮就出正文。
 *           choices[0].message.content（**只取 content**，reasoning_content
 *           是推理模型的思维链）
 *        为什么要有这个循环：模型不知道实时天气、也没有改板子状态的手，
 *        不把工具结果喂回去，"今天天气怎么样"就只能瞎编，"八点提醒我吃药"
 *        就只能回一句"我记住了"（实际什么都没发生）。
 *        天气默认关掉是因为 open-meteo 的实况和用户所在地经常对不上（还不如
 *        不说），而且多一轮请求更慢；"今天几号 / 现在几点"靠 system prompt
 *        里的时间就能答，不需要工具。
 *
 * 凭据：
 *   从 claw_config_get() 读 llm_host / api_key / model（开机由板级代码把
 *   /etc/assets/agent_config.json 拷进 /data/ai_agent/config/config.json）。
 *   本文件没有硬编码 key；日志只报长度 / 状态 / host / model，绝不打
 *   Authorization 头或 base64 音频（出错时才打响应体前 200 字节）。
 *
 * 内存：
 *   请求体、响应体、编解码缓冲全部走堆（大块分配会落到 PSRAM），**用完立刻 free**。
 *   每次 malloc/calloc/free 都会打一条带字节数的日志（mimo_alloc_logged 等），
 *   上板串口日志里可以直接对着看每步的块大小和峰值。
 *   三处的峰值（一轮对话里依次发生，不是同时）：
 *     ASR   : pcm(调用方) + wav(44+n) + body(约 1.34*n) + resp(16 KB)
 *     chat  : msgs(约 10 KB，system+user+每轮追加的 tool_calls/tool 两条)
 *             + body(约 msgs + 1 KB) + resp(64 KB)；问天气时中间还会过两个
 *             8 KB 的天气响应（用完立刻还，不叠加）
 *     TTS   : body(小) + resp(MIMO_TTS_RESP_CAP，就地 base64 解码 → 重采样直接
 *             写进调用方的 PCM 缓冲，不再额外要 decoded / pcm16 两块)
 *   TTS 单次可合成的字数由 MIMO_TTS_TEXT_MAX / MIMO_TTS_RESP_CAP 决定，见宏注释。
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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
 * 也就是每字约 11 KB base64。于是：
 *
 *   MIMO_TTS_RESP_CAP = 1536 KB / 11 KB ≈ 139 字，
 *   而 MIMO_TTS_TEXT_MAX 只送 120 个字（≈ 1.32 MB，占 cap 的 86%），留出余量。
 *
 * 为什么不是 1 MB：406 字节的回复（约 130 汉字）要 1.4 MB 左右，1 MB 装不下，
 * JSON 会被 tls 层截成半个、白跑一趟还把整轮对话卡死（真机现场）。
 * 喂进去之前先按 MIMO_TTS_TEXT_MAX 截断，见 tts_truncate()。
 *
 * 峰值：以前是 resp(cap) + decoded(cap*3/4) + pcm16(完整时长) 三层叠在一起，
 * 现在 base64 就地解码（省掉 decoded）、重采样直接写进调用方的 PCM 缓冲
 * （省掉 pcm16），峰值 ≈ resp + 调用方缓冲，比原来少 1 MB 以上。 */
#define MIMO_ASR_RESP_CAP (16 * 1024)
#define MIMO_TTS_RESP_CAP (1536 * 1024)

/* 一次最多送多少个字去合成（UTF-8 码点，不是字节）。见上面 11 KB/字的实测值。 */
#define MIMO_TTS_TEXT_MAX 120

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

/* 对话（chat/completions）：和 ASR/TTS 共用 host / path / api_key，只有
 * model 不同。请求体在运行时拼（model 从配置的 "model" 键读，不写死）：
 *   {"model":"<model>",
 *    "messages":[{"role":"system","content":"<时间 [+ 默认城市] + 说话要求>"},
 *                {"role":"user","content":"<转义文本>"},
 *                ... 开了工具才有后续轮次：assistant(tool_calls) / tool ...],
 *    "tools":[{...get_weather...}],        <- enable_weather_tool 才带这段
 *    "max_tokens":1024}
 * 默认不带 tools（用户反馈查出来的天气不准），messages 就 system + user 两条，
 * 一轮就完事；要开就在配置里把 enable_weather_tool 设成 1。
 * max_tokens 给 1024 —— 这个模型是推理模型，思维链会吃掉一部分 token，
 * 给太小正文（content）就会是空的。
 * 开了工具时，模型要查天气会先回 finish_reason="tool_calls"（正文为空），
 * 板子替它查完再把结果作为 role:"tool" 的消息追加进去问一轮；最多
 * MIMO_CHAT_MAX_ROUNDS 轮。
 * 应答只取 choices[0].message.content：reasoning_content 是思维链，不要。
 * 响应是纯文本 JSON（含思维链），几十 KB 足够；缓冲走堆。 */

/* 请求体的三段：前缀（含 model）+ messages 数组 + 尾巴（tools 表 + max_tokens）。
 * messages 由 mimo_chat() 拼好放在中间，见那里的 sjoin_add*。
 * 尾巴分两份：带 tools 的 / 不带的 —— 不能用一份再删，否则不是多一个逗号
 * 就是留下空的 "tools":[]，服务端 json 解析直接报错。 */

#define MIMO_CHAT_JSON_FMT "{\"model\":\"%s\",\"messages\":["

/* tools 表：两个工具，运行时按开关拼成数组（见 mimo_chat 里的 tools_json）。
 * 这里每个宏只是**一个函数对象**（不带数组方括号），拼的时候用 "," 连 ——
 * 这样"天气开不开""提醒开不开"四种组合都能拼出合法 JSON，不用为每种组合
 * 各写一份数组字面量（那份"拼了再删"的老写法会留尾逗号，服务端直接报错）。
 * description 是写给模型看的，里面的括号 / 逗号 / 引号一律用中文全角 ——
 * 这是手写的 JSON 字面量，一个 ASCII 引号或反斜杠就能把它拼坏。 */

#define MIMO_TOOL_WEATHER_JSON                                           \
  "{\"type\":\"function\",\"function\":{\"name\":\"get_weather\","      \
  "\"description\":\"查询中国某个城市的实时天气：天气现象、气温、体感温度、"                 \
  "湿度、风速。用户问天气、气温、冷不冷、热不热、要不要带伞时调用。\","                      \
  "\"parameters\":{\"type\":\"object\",\"properties\":{\"city\":"         \
  "{\"type\":\"string\",\"description\":\"中文城市名，例如 北京/上海/深圳；"             \
  "用户没说城市时用默认城市\"}},\"required\":[\"city\"]}}}"

/* add_reminder：口语时间由模型负责折算成 24 小时制 —— system prompt 里带着
 * 当前日期时间，所以"明天早上八点"它能算对（见 chat_build_system_prompt）。 */

#define MIMO_TOOL_REMINDER_JSON                                          \
  "{\"type\":\"function\",\"function\":{\"name\":\"add_reminder\","      \
  "\"description\":\"给用户建一条每天重复的提醒。用户说让我几点几分提醒他做"           \
  "某件事时调用，例如明天早上八点提醒我吃药、下午三点提醒我量血压。"                    \
  "时间必须你自己换算成 24 小时制的 HH:MM 再传：早上八点是 08:00，"                  \
  "下午三点是 15:00，晚上八点是 20:00。\","                                  \
  "\"parameters\":{\"type\":\"object\",\"properties\":{"                  \
  "\"time\":{\"type\":\"string\",\"description\":\"24 小时制时刻，格式 "     \
  "HH:MM，例如 08:00 / 15:30。必须换算好，不要传明天早上这种词。\"},"             \
  "\"title\":{\"type\":\"string\",\"description\":\"提醒做什么，简短中文，"    \
  "例如 吃药 / 量血压 / 喝水 / 给儿子打电话\"}},"                             \
  "\"required\":[\"time\",\"title\"]}}}"

/* 默认（工具关）：messages 收尾 + max_tokens，没有 tools 字段 */

#define MIMO_CHAT_JSON_TAIL_NO_TOOLS "],\"max_tokens\":1024}"

/* 带工具时尾巴分三段拼：HEAD + 运行时拼好的 tools 数组 + END */

#define MIMO_CHAT_JSON_TAIL_TOOLS_HEAD "],\"tools\":"
#define MIMO_CHAT_JSON_TAIL_TOOLS_END  ",\"max_tokens\":1024}"

/* tools 数组（两个工具对象的 JSON 大约 600 字节，1 KB 给足） */

#define MIMO_CHAT_TOOLS_BUF_MAX 1024

#define MIMO_CHAT_RESP_CAP (64 * 1024)

/* 工具循环最多几轮：开了 get_weather 时正常问一次天气是 2 轮（第 1 轮要工具、
 * 第 2 轮给正文），多出来的 1 轮留给模型追问；到上限还在要工具就按 mimo_chat
 * 里的兜底走。工具关着时用不到这个上限 —— 模型手里没有工具，第 1 轮就给正文。 */

#define MIMO_CHAT_MAX_ROUNDS 3

/* get_weather 工具：open-meteo 的两个免 key 接口（都是 HTTPS GET）。
 * geocode 拿经纬度，forecast 拿实况；weather_code 是 WMO 码，由 wmo_text()
 * 在板子上映射成中文短句再喂给模型（模型不认识数字码）。
 * 注意 forecast 那个路径要过 snprintf：URL 里的 % 必须写成 %%，否则
 * "Asia%2FShanghai" 会被 printf 当成 %2F 转换符（编译器也会告警）。 */

#define MIMO_GEO_HOST     "geocoding-api.open-meteo.com"
#define MIMO_GEO_PATH_FMT "/v1/search?name=%s&count=1&language=zh"

#define MIMO_WEATHER_HOST "api.open-meteo.com"
#define MIMO_WEATHER_PATH_FMT                                            \
  "/v1/forecast?latitude=%s&longitude=%s&current="                       \
  "temperature_2m,apparent_temperature,relative_humidity_2m,weather_code," \
  "wind_speed_10m&timezone=Asia%%2FShanghai"

/* 天气响应就几百字节，8 KB 足够（截断了会解析失败，宁大勿小）；
 * 城市名先进 URL：中文一个字 percent-encode 成 9 个字符，256 字节能装 28 个汉字。 */

#define MIMO_WEATHER_RESP_CAP (8 * 1024)
#define MIMO_CITY_BUF_MAX     64
#define MIMO_CITY_URL_MAX     256

/* 用户没说城市时用哪个：先读配置库的 default_city，读不到用这个。
 * 只在 enable_weather_tool 开着（真的会用 get_weather）时才读。 */

#define MIMO_DEFAULT_CITY "北京"

/* 天气工具的开关（配置键，字符串 "1" / "true" / "on"，大小写不敏感；
 * 缺省、空串、别的值一律算关）。**默认关**：open-meteo 查出来的实况跟用户
 * 所在地常常对不上，用户宁可不要；关掉后请求体里没有 "tools" 字段，也就
 * 不会多跑工具循环。"今天几号 / 现在几点"不靠这个 —— 时间在 system prompt 里。 */

#define MIMO_CFG_KEY_WEATHER_TOOL "enable_weather_tool"

/* 建提醒工具的开关，键名 enable_reminder_tool。**默认开**（配成 "0" / "false" /
 * "off" 才关）：这是老人说"八点提醒我吃药"就要生效的功能，不该让用户先去改
 * 配置。真正决定它出不出现在 tools 表里的还有一条：robot_ui 有没有注册
 * mimo_set_reminder_hook()（没注册就没地方落地，工具干脆不给模型看）。 */

#define MIMO_CFG_KEY_REMINDER_TOOL "enable_reminder_tool"

/* add_reminder 的 title 最长留几个字（按 UTF-8 整字截，尾部补省略号）。
 * 提醒列表本身是 64 字节，20 个汉字 = 60 字节，正好放得下。 */

#define MIMO_REMINDER_TITLE_CHARS 20

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
static char s_model[64];

/* 建提醒的落地实现，由 robot_ui 在启动时用 mimo_set_reminder_hook() 注册。
 * 为 NULL 时 add_reminder 不出现在 tools 表里（见 chat_reminder_tool_enabled）。 */

static mimo_reminder_hook_t s_reminder_hook = NULL;

/****************************************************************************
 * Name: mimo_set_reminder_hook
 *
 * Description:
 *   注册"替用户建一条提醒"的实现（声明与约定见 mimo_voice.h）。传 NULL 注销。
 *   只写一个函数指针，任何线程都能调；真正的回调发生在语音工作线程里。
 *
 ****************************************************************************/

void mimo_set_reminder_hook(mimo_reminder_hook_t fn)
{
  s_reminder_hook = fn;

  syslog(LOG_INFO, "[%s] 建提醒工具: %s\n", TAG,
         fn != NULL ? "已注册（add_reminder 可用）" : "未注册（工具不下发）");
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: mimo_alloc_logged / mimo_calloc_logged / mimo_free_logged
 *
 * Description:
 *   语音链路里的大块内存都从这里要、也从这里还，"每步要了多少、还了多少"
 *   直接打到串口上（上板复现内存峰值时对着看即可）。free 的字节数由调用方
 *   传 —— 这里不记账，只是把同一份数字打出来。
 *
 *   失败一定打 LOG_ERR（这是真正会把整轮对话搞挂的路径）。
 *
 ****************************************************************************/

static void *mimo_alloc_logged(const char *what, size_t bytes)
{
  void *p = malloc(bytes);

  if (p == NULL)
    {
      syslog(LOG_ERR, "[%s] 分配失败: %s %zu 字节\n", TAG, what, bytes);
    }
  else
    {
      syslog(LOG_INFO, "[%s] alloc  %-14s %zu 字节\n", TAG, what, bytes);
    }

  return p;
}

static void *mimo_calloc_logged(const char *what, size_t bytes)
{
  void *p = calloc(1, bytes);

  if (p == NULL)
    {
      syslog(LOG_ERR, "[%s] 分配失败: %s %zu 字节\n", TAG, what, bytes);
    }
  else
    {
      syslog(LOG_INFO, "[%s] alloc  %-14s %zu 字节\n", TAG, what, bytes);
    }

  return p;
}

static void mimo_free_logged(const char *what, void *p, size_t bytes)
{
  if (p == NULL)
    {
      return;
    }

  syslog(LOG_INFO, "[%s] free   %-14s %zu 字节\n", TAG, what, bytes);
  free(p);
}

/****************************************************************************
 * Name: tts_truncate
 *
 * Description:
 *   按"字"（UTF-8 码点）把要合成的文本截到 max_chars 个，超长时在末尾补一个
 *   省略号（U+2026，"…"）。返回写进 dst 的字节数，*truncated 记下有没有截。
 *
 *   为什么要截：MiMo TTS 回的是音频文件的 base64，每字约 11 KB。回复一长
 *   （真机那次是 406 字节 ≈ 130 个汉字），响应就超过 MIMO_TTS_RESP_CAP，
 *   tls 读缓冲被塞满、JSON 少了尾巴，白等一场还把整轮对话卡住。
 *   让 TTS 只念前面一段，比什么都不出声强得多。
 *
 *   ⚠️ 只截 TTS 这一路。对话区显示和 ASR 都不截（显示当然要完整）。
 *
 ****************************************************************************/

static size_t tts_truncate(const char *src, char *dst, size_t dst_cap,
                           size_t max_chars, bool *truncated)
{
  size_t i = 0;
  size_t o = 0;
  size_t chars = 0;
  size_t len;

  *truncated = false;

  if (src == NULL || dst == NULL || dst_cap == 0)
    {
      return 0;
    }

  len = strlen(src);

  while (i < len)
    {
      unsigned char c = (unsigned char)src[i];
      size_t used;

      if (c < 0x80)
        {
          used = 1;
        }
      else if ((c & 0xe0) == 0xc0)
        {
          used = 2;
        }
      else if ((c & 0xf0) == 0xe0)
        {
          used = 3;
        }
      else if ((c & 0xf8) == 0xf0)
        {
          used = 4;
        }
      else
        {
          used = 1;              /* 非法首字节：吃掉一个，别把后面全带偏 */
        }

      if (used > len - i)
        {
          used = 1;
        }

      if (chars >= max_chars || o + used + 1 > dst_cap)
        {
          *truncated = true;
          break;
        }

      memcpy(dst + o, src + i, used);
      o += used;
      i += used;
      chars++;
    }

  /* 省略号：U+2026 的 UTF-8 是 E2 80 A6，字库里有这个字形 */

  if (*truncated && o + 4 <= dst_cap)
    {
      memcpy(dst + o, "\xe2\x80\xa6", 3);
      o += 3;
    }

  dst[o] = '\0';
  return o;
}


/****************************************************************************
 * Name: mimo_load_config
 *
 * Description:
 *   把 llm_host / api_key / llm_path / llm_port / model 读进静态快照。
 *   host 和 key 缺一个都算不可用（不给默认值，避免用错凭据去打别人的服务）。
 *   model 只有对话（mimo_chat）用，缺了不算整体不可用，由 mimo_chat 自己报错。
 *
 ****************************************************************************/

static int mimo_load_config(void)
{
  s_host[0] = '\0';
  s_key[0]  = '\0';
  s_path[0] = '\0';
  s_port[0] = '\0';
  s_model[0] = '\0';

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

  /* 对话模型：从配置的 model 键读（不写死） */

  claw_config_get(AGENT_CFG_KEY_MODEL, s_model, sizeof(s_model));

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

  /* 请求前清掉 ai_agent TLS 连接池里的旧连接，强制走"全新连接"。
   *
   * 为什么：池子的复用路径（vela_tls.c 里 pool 那段）在排空旧连接残包时用的是 10ms 超时，
   * 读到 WANT_READ 时它**不重连**，而是把死连接当好的继续用 —— 写进去之后在
   * tls_read_response() 上等满 120 秒才失败、再重连重发。表现就是界面长时间停在
   * "正在识别"（不是崩溃，是慢得离谱）。丢一次连接、重做一次 TLS 握手（几百毫秒）
   * 比这个划算。 */
  vela_tls_pool_cleanup();

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
 * Name: trim_whitespace
 *
 * Description:
 *   去掉字符串首尾的空格 / 制表符 / 换行（就地）。模型回复常带一个尾换行。
 *
 ****************************************************************************/

static void trim_whitespace(char *s)
{
  size_t i = 0;
  size_t len;

  if (s == NULL)
    {
      return;
    }

  while (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')
    {
      i++;
    }

  if (i > 0)
    {
      memmove(s, s + i, strlen(s + i) + 1);
    }

  len = strlen(s);

  while (len > 0)
    {
      char c = s[len - 1];

      if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
        {
          break;
        }

      s[--len] = '\0';
    }
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
 * Name: wav_extract_16k_into
 *
 * Description:
 *   从一段 WAV 内存里取出 PCM，转成 16 kHz / 单声道 / s16le，**直接写进
 *   调用方给的缓冲**（不再自己分配一份"完整时长"的 pcm16）。
 *   多声道只取第 0 声道（本工程的链路就是单声道）。
 *
 *   dst_frames 是目标缓冲能放多少个采样点：TTS 那条路上调用方的缓冲只有
 *   VOICE_TTS_BUF_BYTES（8 秒），音频更长时这里只填满前一段（语义和原来
 *   "装得下多少给多少"一致），*full_frames 回的是"完整放得下会有多少"，
 *   调用方据此判断有没有被截。
 *
 ****************************************************************************/

static int wav_extract_16k_into(const unsigned char *buf, size_t len,
                                int16_t *dst, size_t dst_frames,
                                size_t *out_frames, size_t *full_frames)
{
  struct wav_info_s info;
  size_t frames;
  size_t total;
  int ret;

  *out_frames = 0;

  if (full_frames != NULL)
    {
      *full_frames = 0;
    }

  if (dst == NULL || dst_frames == 0)
    {
      return -EINVAL;
    }

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

  if (info.rate == 0)
    {
      return -EINVAL;
    }

  total = (size_t)(((uint64_t)frames * MIMO_PCM_RATE) / info.rate);
  if (total == 0)
    {
      return -EINVAL;
    }

  if (full_frames != NULL)
    {
      *full_frames = total;
    }

  *out_frames = resample_to_16k(buf + info.data_off, frames, info.rate,
                                info.channels, dst, dst_frames);

  return (*out_frames == 0) ? -EINVAL : OK;
}

/****************************************************************************
 * Name: wav_extract_16k
 *
 * Description:
 *   同上，只是输出缓冲由本函数在堆上分配（mimo_wav_load_16k 用，
 *   调用方负责 free）。
 *
 ****************************************************************************/

static int wav_extract_16k(const unsigned char *buf, size_t len,
                           unsigned char **pcm_out, size_t *pcm_len)
{
  struct wav_info_s info;
  size_t frames;
  size_t out_frames;
  size_t total = 0;
  int16_t *out;
  size_t got = 0;
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

  ret = wav_extract_16k_into(buf, len, out, out_frames, &got, &total);
  if (ret != OK)
    {
      free(out);
      return ret;
    }

  *pcm_out = (unsigned char *)out;
  *pcm_len = got * sizeof(int16_t);
  return OK;
}

/****************************************************************************
 * Name: b64_decode_inplace
 *
 * Description:
 *   base64 **就地**解码：dst 和 src 是同一块内存。
 *
 *   为什么能就地：mbedtls_base64_decode() 是顺序解码，每吃 4 个输入字节才吐
 *   3 个输出字节，写指针永远落后于读指针（mbedtls/library/base64.c 的第二遍
 *   循环就是这个形状），所以原地解码不会踩到还没读的数据。
 *
 *   为什么值得：TTS 响应是几十万到一两 MB 的 base64，原来要再额外 malloc 一份
 *   3/4 大小的解码缓冲（1.5 MB 的响应就是 ~1.1 MB），是这条链路上第二大的块。
 *
 * Returned Value:
 *   OK（*out 指向 buf 自己）；-EINVAL（base64 非法或解出来是空的）
 *
 ****************************************************************************/

static int b64_decode_inplace(char *buf, size_t b64_len,
                              unsigned char **out, size_t *out_len)
{
  size_t got = 0;
  int ret;

  *out = NULL;
  *out_len = 0;

  if (buf == NULL || b64_len == 0)
    {
      return -EINVAL;
    }

  ret = mbedtls_base64_decode((unsigned char *)buf, b64_len, &got,
                              (const unsigned char *)buf, b64_len);
  if (ret != 0 || got == 0)
    {
      return -EINVAL;
    }

  *out = (unsigned char *)buf;
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
  wav = mimo_alloc_logged("asr.wav", wav_len);
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
      mimo_free_logged("asr.wav", wav, wav_len);
      return -EIO;
    }

  body = mimo_alloc_logged("asr.body", pfx_len + b64_need + sfx_len + 1);
  if (body == NULL)
    {
      mimo_free_logged("asr.wav", wav, wav_len);
      return -ENOMEM;
    }

  memcpy(body, prefix, pfx_len);

  ret = mbedtls_base64_encode((unsigned char *)body + pfx_len,
                              b64_need, &b64_len, wav, wav_len);
  mimo_free_logged("asr.wav", wav, wav_len);   /* 只活到编码完这一次 */

  if (ret != 0)
    {
      mimo_free_logged("asr.body", body, pfx_len + b64_need + sfx_len + 1);
      return -EIO;
    }

  memcpy(body + pfx_len + b64_len, suffix, sfx_len + 1);

  syslog(LOG_INFO, "[%s] ASR: PCM %zu 字节 -> WAV %zu 字节 -> base64 %zu 字节\n",
         TAG, pcm_len, wav_len, b64_len);

  /* 3. 请求。只报长度，不报 body（里面是音频）。
   *    ASR 的响应就是一段识别文字，16 KB 足够（别按 TTS 那个量级开）。 */

  resp = mimo_calloc_logged("asr.resp", MIMO_ASR_RESP_CAP);
  if (resp == NULL)
    {
      mimo_free_logged("asr.body", body, pfx_len + b64_need + sfx_len + 1);
      return -ENOMEM;
    }

  status = mimo_post(body, resp, MIMO_ASR_RESP_CAP);

  /* body 是 16k*2*10 秒 PCM 的 base64（几百 KB），请求一回来就还给堆 ——
   * 后面解析响应、进 chat/TTS 都不再需要它。 */
  mimo_free_logged("asr.body", body, pfx_len + b64_need + sfx_len + 1);

  if (status != 200)
    {
      mimo_log_http_error("ASR", status, resp);
      mimo_free_logged("asr.resp", resp, MIMO_ASR_RESP_CAP);
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
      mimo_free_logged("asr.resp", resp, MIMO_ASR_RESP_CAP);
      return -EPROTO;
    }

  out_len = json_unescape(content, content_len, text_out, text_cap - 1);
  text_out[out_len] = '\0';

  syslog(LOG_INFO, "[%s] ASR: 识别结果 %zu 字节\n", TAG, out_len);

  mimo_free_logged("asr.resp", resp, MIMO_ASR_RESP_CAP);
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
 *   内存上的三处收紧（见文件头）：
 *     - 送出去的文本先按 MIMO_TTS_TEXT_MAX 截断（响应大小 ≈ 11 KB/字）；
 *     - base64 就地解码，不再额外要一份 3/4 大小的解码缓冲；
 *     - 重采样直接写进 pcm_out，不再先建一份"完整时长"的 pcm16 再 memcpy。
 *   每一步的块大小都打日志（mimo_alloc_logged / mimo_free_logged）。
 *
 ****************************************************************************/

static int mimo_tts_synthesize(const char *text, unsigned char *pcm_out,
                               size_t pcm_cap, size_t *pcm_len)
{
  const char *prefix = MIMO_TTS_JSON_PREFIX;
  const char *suffix = MIMO_TTS_JSON_SUFFIX;
  size_t pfx_len = strlen(prefix);
  size_t sfx_len = strlen(suffix);
  char tts_text[MIMO_TTS_TEXT_MAX * 4 + 8];   /* 一个字最多 4 字节 + 省略号 */
  size_t text_len;
  size_t body_len;
  bool truncated = false;
  size_t esc_cap;
  size_t esc_len;
  char *body;
  char *resp;
  const char *audio;
  const char *b64;
  size_t b64_len = 0;
  unsigned char *decoded = NULL;
  size_t decoded_len = 0;
  size_t out_frames = 0;
  size_t full_frames = 0;
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

  /* 0. 太长就只念前面一段：TTS 回的是 base64 音频，响应大小和字数成正比
   *    （实测约 11 KB/字），130 个字的回复会超过 MIMO_TTS_RESP_CAP，
   *    卡满一整轮对话。对话区显示的还是完整回复，这里只截送去合成的那份。 */

  text_len = tts_truncate(text, tts_text, sizeof(tts_text),
                          MIMO_TTS_TEXT_MAX, &truncated);
  if (truncated)
    {
      syslog(LOG_WARNING,
             "[%s] TTS: 合成文本过长（%zu 字节），只念前 %d 个字，后面省略\n",
             TAG, strlen(text), MIMO_TTS_TEXT_MAX);
    }

  if (text_len == 0)
    {
      return -EINVAL;
    }

  /* 1. 文本 JSON 转义后拼请求体（role 必须是 assistant）。
   *    转义直接写进 body 里，不再先 malloc 一份 esc 再 copy 一遍。 */

  esc_cap = text_len * 6;
  body_len = pfx_len + esc_cap + sfx_len + 1;

  body = mimo_alloc_logged("tts.body", body_len);
  if (body == NULL)
    {
      return -ENOMEM;
    }

  memcpy(body, prefix, pfx_len);
  esc_len = json_escape(tts_text, text_len, body + pfx_len, esc_cap);
  memcpy(body + pfx_len + esc_len, suffix, sfx_len + 1);

  syslog(LOG_INFO, "[%s] TTS: 请求 %zu 字节文本（原文 %zu 字节%s）\n", TAG,
         text_len, strlen(text), truncated ? "，已截断" : "");

  /* 2. 请求（响应可能有几十万到一两 MB，缓冲走堆） */

  resp = mimo_calloc_logged("tts.resp", MIMO_TTS_RESP_CAP);
  if (resp == NULL)
    {
      mimo_free_logged("tts.body", body, body_len);
      return -ENOMEM;
    }

  status = mimo_post(body, resp, MIMO_TTS_RESP_CAP);
  mimo_free_logged("tts.body", body, body_len);

  if (status != 200)
    {
      mimo_log_http_error("TTS", status, resp);
      mimo_free_logged("tts.resp", resp, MIMO_TTS_RESP_CAP);
      return -EIO;
    }

  /* 3. 取 message.audio.data。先在 "audio" 之后再找 "data"，
   *    免得命中响应里别的同名字段。
   *    json_find_string() 要求配对结束引号，找不到就是响应被 cap 截断了 ——
   *    这里必须**明确失败返回**，不能让调用方以为还在合成。 */

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
             "[%s] TTS: 响应里没有完整的 audio.data（合成文本太长、响应超过 "
             "%d 字节被截断？）: %.*s\n",
             TAG, MIMO_TTS_RESP_CAP, MIMO_ERR_DUMP_LEN,
             resp[0] != '\0' ? resp : "(空)");
      mimo_free_logged("tts.resp", resp, MIMO_TTS_RESP_CAP);
      return -EPROTO;
    }

  syslog(LOG_INFO, "[%s] TTS: 收到 base64 音频 %zu 字节\n", TAG, b64_len);

  /* 4. base64 **就地**解码（decoded 就在 resp 里），省掉一份 3/4 大小的
   *    解码缓冲。resp 必须活到重采样结束，之后才能 free。 */

  ret = b64_decode_inplace((char *)b64, b64_len, &decoded, &decoded_len);
  if (ret != OK)
    {
      syslog(LOG_ERR, "[%s] TTS: base64 解码失败 %d（响应被截断？）\n", TAG,
             ret);
      mimo_free_logged("tts.resp", resp, MIMO_TTS_RESP_CAP);
      return ret;
    }

  /* 5. WAV -> 16k 单声道 s16le（MiMo 回的是 24 kHz），直接写进调用方的
   *    pcm_out：装不下就只填满前面一段（和原来"装得下多少给多少"一致，
   *    调用方看 *pcm_len 就知道被截了）。 */

  ret = wav_extract_16k_into(decoded, decoded_len, (int16_t *)pcm_out,
                             pcm_cap / sizeof(int16_t),
                             &out_frames, &full_frames);
  mimo_free_logged("tts.resp", resp, MIMO_TTS_RESP_CAP);

  if (ret != OK || out_frames == 0)
    {
      syslog(LOG_ERR, "[%s] TTS: WAV 解析/重采样失败 %d\n", TAG, ret);
      return (ret != OK) ? ret : -EINVAL;
    }

  if (full_frames > pcm_cap / sizeof(int16_t))
    {
      syslog(LOG_WARNING,
             "[%s] TTS: 输出缓冲只有 %zu 字节，只放得下 %zu/%zu 字节\n",
             TAG, pcm_cap, out_frames * sizeof(int16_t),
             full_frames * sizeof(int16_t));
    }

  *pcm_len = out_frames * sizeof(int16_t);

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

/****************************************************************************
 * Name: sjoin_add / sjoin_addstr / sjoin_add_escaped / str_last
 *
 * Description:
 *   拼请求体用的小工具（请求体是手拼的，没引 cJSON）。缓冲区一旦装不下就
 *   置 ovf 并停止写入，调用方拼完检查一次 ovf 决定成败 —— 比每处手算剩余
 *   长度省心，也不会写出半个 JSON。
 *   每块的余量沿用文件里"最坏放大 6 倍"的老规矩（一个字节可能变成 \u00XX）。
 *
 ****************************************************************************/

struct sjoin_s
{
  char  *buf;    /* 输出缓冲 */
  size_t cap;    /* 含结尾 '\0' */
  size_t len;    /* 已写入字节数（不含结尾 '\0'） */
  bool   ovf;    /* true = 溢出，内容已不可信 */
};

static void sjoin_add(struct sjoin_s *sj, const char *src, size_t len)
{
  if (sj->ovf || len > sj->cap - 1 - sj->len)
    {
      sj->ovf = true;
      return;
    }

  memcpy(sj->buf + sj->len, src, len);
  sj->len += len;
  sj->buf[sj->len] = '\0';
}

static void sjoin_addstr(struct sjoin_s *sj, const char *src)
{
  sjoin_add(sj, src, strlen(src));
}

/* 把 src 当 JSON 字符串内容转义后追加（两边的引号由调用方拼） */

static void sjoin_add_escaped(struct sjoin_s *sj, const char *src, size_t len)
{
  size_t room;

  if (sj->ovf)
    {
      return;
    }

  room = sj->cap - 1 - sj->len;

  /* json_escape() 装不下时是静默停手（会拼出"看着合法但内容不全"的 JSON），
   * 所以先按最坏放大 6 倍确认装得下 */

  if (len > room / 6)
    {
      sj->ovf = true;
      return;
    }

  sj->len += json_escape(src, len, sj->buf + sj->len, room);
  sj->buf[sj->len] = '\0';
}

/* 最后一次出现的位置：模型的思维链里也可能出现要找的词（比如 tool_calls），
 * 而 JSON 里真正的那个键在 message 的最后 */

static const char *str_last(const char *hay, const char *needle)
{
  const char *hit = NULL;
  const char *p = hay;

  while ((p = strstr(p, needle)) != NULL)
    {
      hit = p;
      p++;
    }

  return hit;
}

/****************************************************************************
 * Name: json_find_number_text
 *
 * Description:
 *   取 <key> 后面的 JSON 数字**原文**（"21.5" / "39.9075"），不解析成 double：
 *   这版 NuttX 的 printf 不一定编进了 %f，而 open-meteo 给的就是一两位小数，
 *   原文直接拼进中文句子既好看又省事。
 *   键后面不是数字（字符串 / null / 对象）也算没找到 —— 比如 weather_code 的
 *   单位在 "current_units" 里是个字符串。
 *
 ****************************************************************************/

static int json_find_number_text(const char *json, const char *key,
                                 char *out, size_t out_cap)
{
  char pat[64];
  const char *p;
  size_t n = 0;

  if (json == NULL || key == NULL || out == NULL || out_cap < 2
      || strlen(key) + 3 > sizeof(pat))
    {
      return -EINVAL;
    }

  out[0] = '\0';
  snprintf(pat, sizeof(pat), "\"%s\"", key);

  p = strstr(json, pat);
  if (p == NULL)
    {
      return -ENOENT;
    }

  p += strlen(pat);

  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    {
      p++;
    }

  if (*p != ':')
    {
      return -ENOENT;
    }

  p++;

  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    {
      p++;
    }

  if (*p != '-' && *p != '+' && (*p < '0' || *p > '9'))
    {
      return -ENOENT;
    }

  while ((*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '+'
         || *p == 'e' || *p == 'E')
    {
      if (n + 1 >= out_cap)
        {
          break;
        }

      out[n++] = *p++;
    }

  out[n] = '\0';
  return n > 0 ? OK : -ENOENT;
}

/* "20.0" -> "20"，"21.50" -> "21.5"（纯文本剪尾巴，不做数值转换） */

static void num_trim_tail_zeros(char *s)
{
  size_t len = strlen(s);

  if (strchr(s, '.') == NULL)
    {
      return;
    }

  while (len > 0 && s[len - 1] == '0')
    {
      s[--len] = '\0';
    }

  if (len > 0 && s[len - 1] == '.')
    {
      s[--len] = '\0';
    }
}

/****************************************************************************
 * Name: url_encode
 *
 * Description:
 *   把城市名 percent-encode 进 URL query（中文一个字 3 字节 -> 9 个字符）。
 *   只放过 RFC 3986 的 unreserved（字母数字和 - _ . ~），其它一律 %XX：
 *   城市名里可能有空格、斜杠、&，不编码会把 query 拆坏。
 *
 ****************************************************************************/

static int url_encode(const char *src, char *dst, size_t dst_cap)
{
  static const char hexd[] = "0123456789ABCDEF";
  size_t o = 0;

  if (src == NULL || dst == NULL || dst_cap < 4)
    {
      return -EINVAL;
    }

  for (; *src != '\0'; src++)
    {
      unsigned char c = (unsigned char)*src;
      bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                   || (c >= '0' && c <= '9')
                   || c == '-' || c == '_' || c == '.' || c == '~';

      if (plain)
        {
          if (o + 1 >= dst_cap)
            {
              return -ENOSPC;
            }

          dst[o++] = (char)c;
        }
      else
        {
          if (o + 3 >= dst_cap)
            {
              return -ENOSPC;
            }

          dst[o++] = '%';
          dst[o++] = hexd[(c >> 4) & 0xf];
          dst[o++] = hexd[c & 0xf];
        }
    }

  dst[o] = '\0';
  return OK;
}

/****************************************************************************
 * Name: wmo_text
 *
 * Description:
 *   WMO weather code -> 中文短句。模型不认识数字码（问它 61 它不知道是雨），
 *   所以在板子上先翻译好再作为工具结果喂给它。
 *
 ****************************************************************************/

static const char *wmo_text(int code)
{
  if (code == 0)                return "晴";
  if (code == 1 || code == 2)   return "多云";
  if (code == 3)                return "阴";
  if (code == 45 || code == 48) return "有雾";
  if (code >= 51 && code <= 57) return "毛毛雨";
  if (code >= 61 && code <= 67) return "下雨";
  if (code >= 71 && code <= 77) return "下雪";
  if (code >= 80 && code <= 82) return "阵雨";
  if (code == 85 || code == 86) return "阵雪";
  if (code >= 95 && code <= 99) return "雷雨";
  return "天气状况未知";
}

/****************************************************************************
 * Name: weather_http_get
 *
 * Description:
 *   打一次 open-meteo 的 HTTPS GET（443）。请求前清一次连接池，理由和
 *   mimo_post() 里那段一样：池子复用死连接时排空只用 10ms 超时、不重连，
 *   真踩上要等满 120 秒的读超时。天气接口是另外两台机器、平时用不上池子，
 *   每次多花一次握手（几百毫秒）换掉这个风险更划算。
 *
 ****************************************************************************/

static int weather_http_get(const char *host, const char *path, char *resp,
                            size_t resp_cap)
{
  vela_tls_pool_cleanup();
  return vela_https_get(host, "443", path, resp, resp_cap);
}

/****************************************************************************
 * Name: weather_lookup
 *
 * Description:
 *   get_weather 工具的实现：geocode 拿经纬度 -> forecast 拿实况 -> 拼一句
 *   中文（例："北京：晴，气温 21.5℃，体感 20℃，湿度 25%，风速 7.3 km/h"）。
 *   两个接口都是 open-meteo 的免 key HTTPS GET，中文城市名直接进 query。
 *
 * Returned Value:
 *   OK  = out 里是给模型看的结果，**包含"没找到这个城市"这类失败原因**
 *         （让模型自己跟用户解释，不直接报错给用户）；
 *   <0  = 负 errno，只有网络层失败（DNS/TLS/连接）和内存不足才会走到这里。
 *
 ****************************************************************************/

static int weather_lookup(const char *city, char *out, size_t out_cap)
{
  char enc[MIMO_CITY_URL_MAX];
  char path[512];
  char lat[24];
  char lon[24];
  char name[MIMO_CITY_BUF_MAX];
  char temp[24];
  char feels[24];
  char humid[24];
  char wind[24];
  char code[24];
  const char *p;
  const char *val;
  char *resp;
  size_t n;
  int status;

  if (city == NULL || city[0] == '\0' || out == NULL || out_cap == 0)
    {
      return -EINVAL;
    }

  out[0]  = '\0';
  name[0] = '\0';

  if (url_encode(city, enc, sizeof(enc)) != OK)
    {
      snprintf(out, out_cap, "城市名太长，查不了天气");
      return OK;
    }

  /* 1. 地理编码：城市名 -> 经纬度 */

  snprintf(path, sizeof(path), MIMO_GEO_PATH_FMT, enc);

  resp = mimo_calloc_logged("chat.geo", MIMO_WEATHER_RESP_CAP);
  if (resp == NULL)
    {
      return -ENOMEM;
    }

  status = weather_http_get(MIMO_GEO_HOST, path, resp, MIMO_WEATHER_RESP_CAP);

  if (status < 0)
    {
      mimo_free_logged("chat.geo", resp, MIMO_WEATHER_RESP_CAP);
      return -EIO;
    }

  if (status != 200)
    {
      syslog(LOG_ERR, "[%s] 天气: 地理编码 HTTP %d\n", TAG, status);
      snprintf(out, out_cap, "天气服务暂时不可用（HTTP %d）", status);
      mimo_free_logged("chat.geo", resp, MIMO_WEATHER_RESP_CAP);
      return OK;
    }

  /* 只在 "results" 之后找：顶层还有 generationtime_ms 之类的数字键 */

  p = strstr(resp, "\"results\"");
  if (p == NULL
      || json_find_number_text(p, "latitude", lat, sizeof(lat)) != OK
      || json_find_number_text(p, "longitude", lon, sizeof(lon)) != OK)
    {
      syslog(LOG_INFO, "[%s] 天气: 没查到城市 %s\n", TAG, city);
      snprintf(out, out_cap, "没找到%s这个城市", city);
      mimo_free_logged("chat.geo", resp, MIMO_WEATHER_RESP_CAP);
      return OK;
    }

  val = json_find_string(p, "name", &n);
  if (val != NULL && n > 0)
    {
      n = json_unescape(val, n, name, sizeof(name) - 1);
      name[n] = '\0';
    }

  /* resp 在这里还，后面只用抄出来的经纬度和城市名 */

  mimo_free_logged("chat.geo", resp, MIMO_WEATHER_RESP_CAP);

  if (name[0] == '\0')
    {
      snprintf(name, sizeof(name), "%s", city);
    }

  /* 2. 实况：经纬度 -> 天气现象 / 气温 / 体感 / 湿度 / 风速 */

  snprintf(path, sizeof(path), MIMO_WEATHER_PATH_FMT, lat, lon);

  resp = mimo_calloc_logged("chat.wx", MIMO_WEATHER_RESP_CAP);
  if (resp == NULL)
    {
      return -ENOMEM;
    }

  status = weather_http_get(MIMO_WEATHER_HOST, path, resp,
                            MIMO_WEATHER_RESP_CAP);

  if (status < 0)
    {
      mimo_free_logged("chat.wx", resp, MIMO_WEATHER_RESP_CAP);
      return -EIO;
    }

  if (status != 200)
    {
      syslog(LOG_ERR, "[%s] 天气: 实况 HTTP %d\n", TAG, status);
      snprintf(out, out_cap, "天气服务暂时不可用（HTTP %d）", status);
      mimo_free_logged("chat.wx", resp, MIMO_WEATHER_RESP_CAP);
      return OK;
    }

  /* 必须在 "current" 之后找：前面 "current_units" 里同名的键是单位字符串
   * （"°C" / "%"），先找会拿到非数字 */

  p = strstr(resp, "\"current\":");
  if (p == NULL
      || json_find_number_text(p, "temperature_2m", temp, sizeof(temp)) != OK
      || json_find_number_text(p, "apparent_temperature", feels,
                               sizeof(feels)) != OK
      || json_find_number_text(p, "relative_humidity_2m", humid,
                               sizeof(humid)) != OK
      || json_find_number_text(p, "weather_code", code, sizeof(code)) != OK
      || json_find_number_text(p, "wind_speed_10m", wind, sizeof(wind)) != OK)
    {
      syslog(LOG_ERR, "[%s] 天气: 实况响应解析失败: %.*s\n", TAG,
             MIMO_ERR_DUMP_LEN, resp);
      snprintf(out, out_cap, "天气数据没拿到，请稍后再试");
      mimo_free_logged("chat.wx", resp, MIMO_WEATHER_RESP_CAP);
      return OK;
    }

  mimo_free_logged("chat.wx", resp, MIMO_WEATHER_RESP_CAP);

  num_trim_tail_zeros(temp);
  num_trim_tail_zeros(feels);
  num_trim_tail_zeros(humid);
  num_trim_tail_zeros(wind);

  snprintf(out, out_cap, "%s：%s，气温 %s℃，体感 %s℃，湿度 %s%%，风速 %s km/h",
           name, wmo_text(atoi(code)), temp, feels, humid, wind);

  syslog(LOG_INFO, "[%s] 天气: %s lat=%s lon=%s\n", TAG, name, lat, lon);
  return OK;
}

/****************************************************************************
 * Name: chat_config_flag_on
 *
 * Description:
 *   读配置里一个开关键：值是 "1" / "true" / "on"（大小写不敏感，前后空白先
 *   去掉）算开，别的值算关。键读不到时返回 missing_default —— 两个工具的
 *   默认值不一样（天气默认关、提醒默认开），差别就靠这个参数表达。
 *
 *   claw_config_get() 失败时不动缓冲区，所以先自己置空。
 *
 * Returned Value:
 *   true = 开；false = 关。
 *
 ****************************************************************************/

static bool chat_config_flag_on(const char *key, bool missing_default)
{
  char val[16];
  size_t i;

  val[0] = '\0';

  if (claw_config_get(key, val, sizeof(val)) != OK)
    {
      return missing_default;
    }

  trim_whitespace(val);

  /* 先折成小写再比：不引 strings.h，不动 strcasecmp，只处理 ASCII 字母
   * （这几个字面量里也没有非 ASCII） */

  for (i = 0; val[i] != '\0'; i++)
    {
      if (val[i] >= 'A' && val[i] <= 'Z')
        {
          val[i] = (char)(val[i] - 'A' + 'a');
        }
    }

  return strcmp(val, "1") == 0 || strcmp(val, "true") == 0
         || strcmp(val, "on") == 0;
}

/****************************************************************************
 * Name: chat_weather_tool_enabled
 *
 * Description:
 *   天气工具（get_weather）开不开：配置键 enable_weather_tool，**键缺省算关**。
 *   没开的时候请求体里没有这个工具，system prompt 里也不提城市和工具，
 *   模型拿不到就只能好好聊天（用户反馈 open-meteo 查出来的天气不准）。
 *
 * Returned Value:
 *   true = 带 get_weather 工具；false = 不带（默认）。
 *
 ****************************************************************************/

static bool chat_weather_tool_enabled(void)
{
  return chat_config_flag_on(MIMO_CFG_KEY_WEATHER_TOOL, false);
}

/****************************************************************************
 * Name: chat_reminder_tool_enabled
 *
 * Description:
 *   建提醒工具（add_reminder）开不开：配置键 enable_reminder_tool，
 *   **键缺省算开**（配成 "0" / "false" / "off" 才关）。
 *
 *   还有一条硬条件：必须有 app 注册了 mimo_set_reminder_hook()。没注册就
 *   没地方落地，这时**不能**把工具下发给模型 —— 模型调了也只能回它一句
 *   "建不了"，还不如让它直接回答"我记不住提醒"。
 *
 * Returned Value:
 *   true = 带 add_reminder 工具（默认，前提是 hook 已注册）。
 *
 ****************************************************************************/

static bool chat_reminder_tool_enabled(void)
{
  if (s_reminder_hook == NULL)
    {
      return false;
    }

  return chat_config_flag_on(MIMO_CFG_KEY_REMINDER_TOOL, true);
}

/****************************************************************************
 * Name: chat_default_city / chat_build_system_prompt
 *
 * Description:
 *   系统提示是"让模型靠谱"的关键，由四段拼成（prompt_append 逐段追加）：
 *     1) 说话要求 —— 口语化中文、不要 emoji / markdown（屏幕字库没有 emoji，
 *        TTS 也念不出来）。跟工具开不开无关；
 *     2) 当前北京时间 —— 本构建里 localtime 就是 gmtime（romfs 没有 zoneinfo），
 *        所以自己 +8 小时再格式化（星期也一并给）。时间写在提示里而不是做成
 *        工具："今天几号 / 现在几点 / 今天星期几"照抄就行，不用再打一次网络；
 *     3) 提醒工具（use_reminder）—— 让模型把口语时间折算成 24 小时制再调
 *        add_reminder，并复述一遍确认。没给这个工具时这段必须拿掉；
 *     4) 默认城市 + 天气工具（use_weather）—— 只有开了 get_weather 才加配置库
 *        的 default_city（读不到用 MIMO_DEFAULT_CITY），并写明"实时天气必须查
 *        工具"；没开时改成"这类你答不了，别编"。
 *
 ****************************************************************************/

static void chat_default_city(char *out, size_t out_cap)
{
  if (out == NULL || out_cap == 0)
    {
      return;
    }

  /* claw_config_get() 失败时不动缓冲区，所以先自己置空 */

  out[0] = '\0';

  if (claw_config_get("default_city", out, out_cap) != OK || out[0] == '\0')
    {
      snprintf(out, out_cap, MIMO_DEFAULT_CITY);
    }
}

/* 往提示串尾部追加一段（纯字符串，不做格式化 —— 要格式化先自己 snprintf 进
 * tmp 再传进来）。装不下就停下不再追加：提示短一点只是"说得不那么细"，
 * 不能因为截断把 out 写成非 '\0' 结尾。返回新的已用长度。 */

static size_t prompt_append(char *out, size_t out_cap, size_t used,
                            const char *src)
{
  size_t n = strlen(src);

  if (used + n >= out_cap)
    {
      return used;
    }

  memcpy(out + used, src, n);
  used += n;
  out[used] = '\0';
  return used;
}

static void chat_build_system_prompt(bool use_weather, bool use_reminder,
                                     const char *city, char *out,
                                     size_t out_cap)
{
  static const char *WEEKDAY[] = { "日", "一", "二", "三", "四", "五", "六" };
  struct tm tm_bj;
  time_t bj = time(NULL) + 8 * 3600;
  char now[64];
  char tmp[512];
  size_t used = 0;

  if (out == NULL || out_cap == 0)
    {
      return;
    }

  gmtime_r(&bj, &tm_bj);
  snprintf(now, sizeof(now), "%04d-%02d-%02d %02d:%02d",
           tm_bj.tm_year + 1900, tm_bj.tm_mon + 1, tm_bj.tm_mday,
           tm_bj.tm_hour, tm_bj.tm_min);

  out[0] = '\0';

  /* 说话要求：口语化中文、不要 emoji / markdown（屏幕字库没有 emoji，
   * TTS 也念不出来）。这段跟工具开不开无关，每轮都在。 */

  used = prompt_append(out, out_cap, used,
                       "你是老人陪伴机器人里的语音助手，用口语化的中文说话，"
                       "简短直接，一般一两句话就够。不要用 emoji，也不要用"
                       "星号或者 markdown 列表，因为屏幕字库和语音都不支持"
                       "这些符号。");

  /* 当前北京时间：放提示里而不是做成工具 —— "今天几号 / 现在几点 /
   * 今天星期几"照抄就行，不用再打一次网络 */

  snprintf(tmp, sizeof(tmp),
           "现在是 %s（北京时间，星期%s）。用户问今天几号、现在几点、"
           "今天星期几时，直接用上面给你的时间回答，不要说不知道。",
           now, WEEKDAY[tm_bj.tm_wday]);
  used = prompt_append(out, out_cap, used, tmp);

  /* 提醒工具：让模型自己把口语时间折算成 24 小时制（提示里刚给了当前日期
   * 时间，"明天早上八点"它能算）。不提工具时这段必须拿掉，否则模型会去调
   * 一个 tools 表里根本没有的函数。 */

  if (use_reminder)
    {
      used = prompt_append(out, out_cap, used,
                           "用户让你在某个时间提醒他做某件事时，必须调用 "
                           "add_reminder 工具把提醒建好：时间用 24 小时制，"
                           "例如“明天早上八点吃药”就是 08:00 加 吃药，"
                           "“下午三点量血压”就是 15:00 加 量血压。"
                           "建好之后在回复里简短复述一遍让用户确认，例如"
                           "“好，8 点提醒你吃药”。不要只说记住了而不调用"
                           "工具，也不要说自己做不到。");
    }

  /* 天气工具：开着才提默认城市和"必须查工具"；关着就明确告诉它这类问题
   * 答不了（否则会编一个数字给用户）。 */

  if (use_weather)
    {
      snprintf(tmp, sizeof(tmp),
               "用户问天气、气温、冷不冷、热不热、要不要带伞这类实时信息时，"
               "必须调用 get_weather 工具去查，再把它查回来的内容用自然口语"
               "讲给用户听，不要把 JSON、字段名或者数字天气码念出来。"
               "用户没说城市时就用默认城市%s。"
               "工具查不到或者查询失败时，就老实说没查到，不要编。", city);
      prompt_append(out, out_cap, used, tmp);
    }
  else
    {
      prompt_append(out, out_cap, used,
                    "用户问实时天气这类你答不了的信息时，就老实说你查不了，"
                    "不要编一个数字给他。");
    }
}

/****************************************************************************
 * Name: chat_tool_city
 *
 * Description:
 *   从 tool_calls 的 arguments 原文里取 city。arguments 是"JSON 里的字符串"，
 *   里面是转义过的 JSON 文本（形如 "{\"city\": \"北京\"}"），所以先 unescape
 *   再找键。取不到（模型没给 / 形状不对）返回 -ENOENT，调用方改用默认城市。
 *
 ****************************************************************************/

static int chat_tool_city(const char *args_escaped, char *out, size_t out_cap)
{
  char plain[256];
  const char *val;
  size_t n;

  if (args_escaped == NULL || args_escaped[0] == '\0' || out == NULL
      || out_cap == 0)
    {
      return -ENOENT;
    }

  n = json_unescape(args_escaped, strlen(args_escaped), plain,
                    sizeof(plain) - 1);
  plain[n] = '\0';

  val = json_find_string(plain, "city", &n);
  if (val == NULL || n == 0)
    {
      return -ENOENT;
    }

  n = json_unescape(val, n, out, out_cap - 1);
  out[n] = '\0';
  trim_whitespace(out);

  return out[0] != '\0' ? OK : -ENOENT;
}

/****************************************************************************
 * Name: chat_tool_add_reminder
 *
 * Description:
 *   执行 add_reminder 工具：从 arguments 里取出 time / title，校验并规范化
 *   时间，再交给 app 注册进来的 mimo_reminder_hook_t 落地（robot_ui 注册的
 *   实现会写进提醒列表并重挂 RTC 闹钟、刷新界面）。
 *
 *   **失败不返回负值**：把中文原因写进 out 当工具结果喂回模型，让模型自己跟
 *   用户解释（"提醒满了""时间没算对"这些让它说出来比界面弹窗自然，模型还能
 *   自己改正重试）。只有 out 本身不够长这种编程错误才回 -ENOMEM。
 *
 *   时间不合法时**绝不自己猜**：模型传 "8点" / "晚上八点" 就让它换算重试 ——
 *   猜错了就是到点不响，用户会以为提醒功能坏了。
 *
 * Returned Value:
 *   OK（out 里是工具结果文本）; -ENOMEM（out 装不下）。
 *
 ****************************************************************************/

static int chat_tool_add_reminder(const char *args_escaped, char *out,
                                  size_t out_cap)
{
  char plain[640];
  char time_raw[32];
  char title_raw[256];
  char title[64];               /* 提醒列表每条标题就是 64 字节 */
  char hhmm[8];
  const char *val;
  size_t n;
  int hh;
  int mm;
  int ret;
  bool truncated = false;

  if (out == NULL || out_cap == 0)
    {
      return -ENOMEM;
    }

  out[0] = '\0';

  /* arguments 是"JSON 里的 JSON 字符串"，先反转义出真正的 JSON 文本 */

  n = json_unescape(args_escaped, strlen(args_escaped), plain,
                    sizeof(plain) - 1);
  plain[n] = '\0';

  val = json_find_string(plain, "time", &n);
  if (val == NULL || n == 0)
    {
      snprintf(out, out_cap,
               "调用失败：缺少 time 参数。请用 24 小时制的 HH:MM（例如 08:00）"
               "重新调用一次。");
      return OK;
    }

  n = json_unescape(val, n, time_raw, sizeof(time_raw) - 1);
  time_raw[n] = '\0';
  trim_whitespace(time_raw);

  val = json_find_string(plain, "title", &n);
  if (val == NULL || n == 0)
    {
      snprintf(out, out_cap,
               "调用失败：缺少 title 参数。请用简短中文写清楚提醒做什么"
               "（例如 吃药）重新调用一次。");
      return OK;
    }

  n = json_unescape(val, n, title_raw, sizeof(title_raw) - 1);
  title_raw[n] = '\0';
  trim_whitespace(title_raw);

  /* 只认 24 小时制的 H:MM / HH:MM —— "8:00" 和 "08:00" 都收，别的一律打回 */

  if (sscanf(time_raw, "%d:%d", &hh, &mm) != 2 || hh < 0 || hh > 23
      || mm < 0 || mm > 59)
    {
      snprintf(out, out_cap,
               "调用失败：time 必须是 24 小时制的 HH:MM，收到的是“%.40s”。"
               "请把口语时间自己换算好再调用一次，例如下午三点写成 15:00。",
               time_raw);
      return OK;
    }

  /* 标题按整字截到 20 字：提醒列表只有 64 字节，截了比让 hook 拒收好 */

  tts_truncate(title_raw, title, sizeof(title), MIMO_REMINDER_TITLE_CHARS,
               &truncated);

  snprintf(hhmm, sizeof(hhmm), "%02d:%02d", hh, mm);

  if (s_reminder_hook == NULL)
    {
      /* 工具能下发就说明注册过（见 chat_reminder_tool_enabled），这里是兜底 */

      snprintf(out, out_cap, "调用失败：本机没有可用的提醒功能。");
      return OK;
    }

  ret = s_reminder_hook(hhmm, title);

  if (ret < 0)
    {
      syslog(LOG_WARNING, "[%s] add_reminder(%s, %s) 失败: %d\n", TAG, hhmm,
             title, ret);
      snprintf(out, out_cap,
               "调用失败：%s 提醒%s 没能建起来（错误码 %d）。请跟用户说一声"
               "这次没记住，稍后再试。", hhmm, title, ret);
      return OK;
    }

  syslog(LOG_INFO, "[%s] add_reminder 成功: %s %s%s\n", TAG, hhmm, title,
         truncated ? "（标题已截短）" : "");

  snprintf(out, out_cap,
           "已建好：每天 %s 提醒用户%s。请在回复里简短复述一遍确认。",
           hhmm, title);

  return OK;
}

/* 一轮应答里解析出来的工具调用（都是响应里的原文） */

struct chat_toolcall_s
{
  char id[64];     /* tool_call id：回填下一轮 role:"tool" 的 tool_call_id */
  char name[32];   /* function.name */
  char args[512];  /* function.arguments 原文：已是 JSON 转义过的文本 */
};

/****************************************************************************
 * Name: chat_round
 *
 * Description:
 *   发一轮请求并解析应答：
 *     finish_reason="tool_calls" -> 返回 1，把 tool_calls[0] 的 id / name /
 *       arguments 抄进 tc（响应缓冲随后就还了，所以必须抄出来）；
 *     其它 -> 返回 0，正文已 unescape 进 reply_out；
 *     失败 -> 负 errno（-EIO HTTP 失败 / -EPROTO 没有可用的正文或解析不出
 *       工具调用 / -ENOMEM）。
 *
 *   "tool_calls" 用 str_last() 取最后一处：模型的 reasoning_content 里也常
 *   出现这个词，而 JSON 里真正的那个键在 message 的最后。
 *
 ****************************************************************************/

static int chat_round(int round, const char *body, char *reply_out,
                      size_t reply_cap, struct chat_toolcall_s *tc)
{
  char extra[64];
  bool is_tool;
  const char *reason;
  const char *val;
  const char *tc_json;
  char *resp;
  size_t len = 0;
  size_t n;
  size_t out_len;
  int status;

  memset(tc, 0, sizeof(*tc));

  resp = mimo_calloc_logged("chat.resp", MIMO_CHAT_RESP_CAP);
  if (resp == NULL)
    {
      return -ENOMEM;
    }

  status = mimo_post(body, resp, MIMO_CHAT_RESP_CAP);

  if (status != 200)
    {
      mimo_log_http_error("chat", status, resp);
      mimo_free_logged("chat.resp", resp, MIMO_CHAT_RESP_CAP);
      return -EIO;
    }

  /* finish_reason 决定这一轮是"要调工具"还是"给正文" */

  reason = json_find_string(resp, "finish_reason", &len);
  if (reason == NULL)
    {
      syslog(LOG_ERR, "[%s] chat: 第 %d 轮响应里没有 finish_reason: %.*s\n",
             TAG, round, MIMO_ERR_DUMP_LEN, resp);
      mimo_free_logged("chat.resp", resp, MIMO_CHAT_RESP_CAP);
      return -EPROTO;
    }

  extra[0] = '\0';
  is_tool = (len == 10 && memcmp(reason, "tool_calls", 10) == 0);

  if (is_tool)
    {
      tc_json = str_last(resp, "\"tool_calls\"");
      if (tc_json != NULL)
        {
          val = json_find_string(tc_json, "id", &n);
          if (val != NULL && n < sizeof(tc->id))
            {
              memcpy(tc->id, val, n);
              tc->id[n] = '\0';
            }

          val = json_find_string(tc_json, "name", &n);
          if (val != NULL && n < sizeof(tc->name))
            {
              memcpy(tc->name, val, n);
              tc->name[n] = '\0';
            }

          /* arguments 是"JSON 里的 JSON 字符串"：这里拿到的是**已经转义好**
           * 的原文，下一轮直接抄进请求体就行（不用先反转义再转义回来） */

          val = json_find_string(tc_json, "arguments", &n);
          if (val != NULL && n < sizeof(tc->args))
            {
              memcpy(tc->args, val, n);
              tc->args[n] = '\0';
            }
        }

      snprintf(extra, sizeof(extra), ", 工具=%s",
               tc->name[0] != '\0' ? tc->name : "(没解析出来)");
    }

  syslog(LOG_INFO, "[%s] chat: 第 %d 轮, finish=%.*s%s\n", TAG, round,
         (int)len, reason, extra);

  if (is_tool)
    {
      if (tc->name[0] == '\0')
        {
          /* 要工具但连函数名都没解析出来，拼不出下一轮。模型偶尔会既写正文
           * 又要工具，有正文就用正文，没有就当这一轮失败。 */

          val = json_find_string(resp, "content", &n);
          if (val != NULL && n > 0)
            {
              out_len = json_unescape(val, n, reply_out, reply_cap - 1);
              reply_out[out_len] = '\0';
              trim_whitespace(reply_out);
              mimo_free_logged("chat.resp", resp, MIMO_CHAT_RESP_CAP);
              return reply_out[0] != '\0' ? 0 : -EPROTO;
            }

          syslog(LOG_ERR, "[%s] chat: tool_calls 解析不出函数名\n", TAG);
          mimo_free_logged("chat.resp", resp, MIMO_CHAT_RESP_CAP);
          return -EPROTO;
        }

      /* tool_call_id 理论上一定有；真没有就自己造一个，别让这一轮白费 */

      if (tc->id[0] == '\0')
        {
          syslog(LOG_WARNING, "[%s] chat: tool_calls 里没有 id，用 call_0 顶上\n",
                 TAG);
          strcpy(tc->id, "call_0");
        }

      mimo_free_logged("chat.resp", resp, MIMO_CHAT_RESP_CAP);
      return 1;
    }

  /* 只取 choices[0].message.content。json_find_string() 找的是字面量
   * "content"，不会命中 "reasoning_content"（那里 content 前面是下划线，
   * 不是引号）。 */

  val = json_find_string(resp, "content", &n);
  if (val == NULL || n == 0)
    {
      syslog(LOG_ERR,
             "[%s] chat: 响应里没有 message.content（思维链把 token 占满了？）"
             ": %.*s\n",
             TAG, MIMO_ERR_DUMP_LEN, resp[0] != '\0' ? resp : "(空)");
      mimo_free_logged("chat.resp", resp, MIMO_CHAT_RESP_CAP);
      return -EPROTO;
    }

  out_len = json_unescape(val, n, reply_out, reply_cap - 1);
  reply_out[out_len] = '\0';
  mimo_free_logged("chat.resp", resp, MIMO_CHAT_RESP_CAP);

  trim_whitespace(reply_out);

  if (reply_out[0] == '\0')
    {
      syslog(LOG_ERR, "[%s] chat: 回复正文为空（max_tokens 被思维链吃光？）\n",
             TAG);
      return -EPROTO;
    }

  return 0;
}

/****************************************************************************
 * Name: mimo_chat
 *
 * Description:
 *   同步跑完一次对话（POST <llm_host>:<llm_port><llm_path>），把回复正文
 *   写进 reply_out。和 ASR/TTS 共用 host / path / api_key，只有 model 从
 *   配置的 "model" 键读（不写死）。
 *
 *   默认是**单轮对话**：请求体里不带 "tools"，messages 就 system + user 两条，
 *   模型没有工具可要，第 1 轮直接出正文。system prompt 里带着当前北京时间
 *   （自己 +8 小时算，本构建没有 zoneinfo），所以"今天几号 / 现在几点 /
 *   今天星期几"这类问题不用再打一次网络。
 *
 *   两个工具，各自独立开关，请求体里的 tools 表是运行时按开关拼的（见
 *   MIMO_CHAT_TOOLS_BUF_MAX / tools_json）：
 *     add_reminder —— **默认开**（enable_reminder_tool 配成 0/false/off 才关），
 *       前提是 app 已经注册了 mimo_set_reminder_hook()。模型要调它时，板子把
 *       time/title 交给那个回调落地（robot_ui 会写进提醒列表 + 重挂 RTC 闹钟），
 *       结果作为 role:"tool" 追加进去再问一轮，让模型复述确认；
 *     get_weather —— **默认关**（enable_weather_tool 设成 1 才开）：模型要调它时
 *       板子替它打 open-meteo 查，同样追加两条消息再问一轮。
 *   messages 会长，最多 MIMO_CHAT_MAX_ROUNDS 轮。开天气工具时系统提示里才提
 *   默认城市。
 *   工具查不到 / 服务端 HTTP 出错不改语义：把"没查到"喂回给模型，让它自己跟
 *   用户解释；只有网络层失败（TLS/DNS/连接）才回负值。
 *
 *   这是给**别的 app**（robot_ui）用的跨 app 接口：凭据自己读、请求自己发，
 *   不依赖 ai_agent 进程的任何全局状态（消息总线队列等）。相比之下 ai_agent
 *   的 llm_send_text() / velaclaw_* 只能在本进程里用 —— 别的 app 调会把消息
 *   投进一个从没初始化过的队列，pthread_mutex_lock 直接撞
 *   NXSEM_IS_MUTEX 断言，把整个 app 打死。
 *
 *   注意：内部要跑 TLS 握手 + 云端推理，会阻塞几十秒，只能在非 LVGL 线程里调。
 *
 * Input Parameters:
 *   text      - 用户输入文本（UTF-8，内部做 JSON 转义）；
 *   reply_out - 输出缓冲（调用方提供，栈或堆都行）；
 *   reply_cap - 输出缓冲大小（含结尾 '\0'）。
 *
 * Returned Value:
 *   回复正文长度（字节，去掉首尾空白后）；<0 为负 errno
 *   （-ENOENT 未配置 / -EIO HTTP 或网络失败 / -EPROTO 响应里没有正文或
 *   解析不出工具调用）。
 *
 ****************************************************************************/

int mimo_chat(const char *text, char *reply_out, size_t reply_cap)
{
  char sys[2048];
  char city[MIMO_CITY_BUF_MAX];
  char city_hint[MIMO_CITY_BUF_MAX];
  char tool_result[384];
  char prefix[128];
  char tools_json[MIMO_CHAT_TOOLS_BUF_MAX];
  struct chat_toolcall_s tc;
  struct sjoin_s msj;
  char *msgs;
  size_t msgs_cap;
  size_t sys_len;
  size_t text_len;
  size_t body_cap;
  int pfx_len;
  int round;
  int ret;
  bool use_weather;
  bool use_reminder;
  bool use_tools;

  if (text == NULL || reply_out == NULL || reply_cap == 0)
    {
      return -EINVAL;
    }

  reply_out[0] = '\0';

  if (mimo_load_config() != OK)
    {
      syslog(LOG_ERR, "[%s] chat: 配置里缺 llm_host / api_key，MiMo 不可用\n",
             TAG);
      return -ENOENT;
    }

  if (s_model[0] == '\0')
    {
      syslog(LOG_ERR, "[%s] chat: 配置里缺 model，无法发起对话\n", TAG);
      return -ENOENT;
    }

  text_len = strlen(text);
  if (text_len == 0)
    {
      return -EINVAL;
    }

  pfx_len = snprintf(prefix, sizeof(prefix), MIMO_CHAT_JSON_FMT, s_model);
  if (pfx_len < 0 || (size_t)pfx_len >= sizeof(prefix))
    {
      return -EINVAL;
    }

  /* 两个工具各自独立开关：
   *   天气默认关（用户反馈查得不准），要开在配置里设 enable_weather_tool=1；
   *   提醒默认开（"八点提醒我吃药"就要生效），前提是 app 注册了建提醒回调。
   * 关掉的工具不进 tools 表，系统提示里也不提，模型就看不到它。 */

  use_weather  = chat_weather_tool_enabled();
  use_reminder = chat_reminder_tool_enabled();
  use_tools    = use_weather || use_reminder;

  if (use_weather)
    {
      chat_default_city(city, sizeof(city));
    }
  else
    {
      city[0] = '\0';
    }

  chat_build_system_prompt(use_weather, use_reminder, city, sys, sizeof(sys));
  sys_len = strlen(sys);

  /* tools 表：单个工具是"一个函数对象"（见 MIMO_TOOL_*_JSON），这里用一个
   * 逗号连成数组。关掉的那个不参与拼接，所以四种组合都是合法 JSON ——
   * 不能用"先拼全再删"那套，删不干净就是尾逗号，服务端 json 解析直接报错。 */

  tools_json[0] = '\0';

  if (use_tools)
    {
      int tj = snprintf(tools_json, sizeof(tools_json), "[%s%s%s]",
                        use_weather ? MIMO_TOOL_WEATHER_JSON : "",
                        (use_weather && use_reminder) ? "," : "",
                        use_reminder ? MIMO_TOOL_REMINDER_JSON : "");

      if (tj < 0 || (size_t)tj >= sizeof(tools_json))
        {
          syslog(LOG_ERR, "[%s] chat: tools 表缓冲 %d 字节不够\n", TAG,
                 (int)sizeof(tools_json));
          return -ENOMEM;
        }
    }

  /* 1. 先拼 messages 数组：system（当前时间 [+ 默认城市] + 说话要求）+ user。
   *    开了工具时之后每轮最多再追加 assistant(tool_calls) + tool 两条
   *    （1 KB 量级），余量按最多 3 轮 + 最坏 6 倍放大留，装不下由 sjoin 的
   *    ovf 兜住；工具关着就固定两条消息。 */

  msgs_cap = (sys_len + text_len) * 6 + 4096;
  msgs = mimo_alloc_logged("chat.msgs", msgs_cap);
  if (msgs == NULL)
    {
      return -ENOMEM;
    }

  msgs[0] = '\0';
  msj = (struct sjoin_s) { msgs, msgs_cap, 0, false };

  sjoin_addstr(&msj, "{\"role\":\"system\",\"content\":\"");
  sjoin_add_escaped(&msj, sys, sys_len);
  sjoin_addstr(&msj, "\"},{\"role\":\"user\",\"content\":\"");
  sjoin_add_escaped(&msj, text, text_len);
  sjoin_addstr(&msj, "\"}");

  if (msj.ovf)
    {
      syslog(LOG_ERR, "[%s] chat: messages 缓冲 %zu 字节不够（文本太长？）\n",
             TAG, msgs_cap);
      mimo_free_logged("chat.msgs", msgs, msgs_cap);
      return -ENOMEM;
    }

  body_cap = msgs_cap + 1024 + sizeof(tools_json);  /* + model / tools 表 / max_tokens 骨架 */

  /* 日志里只说这一轮带哪些工具，key 一个字节都不打 */

  syslog(LOG_INFO,
         "[%s] chat: 工具=[提醒=%s 天气=%s], 请求 %zu 字节文本, model=%s%s%s\n",
         TAG, use_reminder ? "开" : "关", use_weather ? "开" : "关",
         text_len, s_model,
         use_weather ? ", 默认城市=" : "", use_weather ? city : "");

  ret = -EPROTO;

  /* 2. 工具循环：每轮把当前的 messages 拼成完整请求体发出去。
   *    模型要工具 -> 板子执行一次（建提醒 / 查天气）-> 追加两条消息 -> 再来
   *    一轮；模型直接给正文 -> 那就是回复（工具都关着时必然走这条，一轮结束）。 */

  for (round = 1; round <= MIMO_CHAT_MAX_ROUNDS; round++)
    {
      struct sjoin_s bj;
      char *body;

      body = mimo_alloc_logged("chat.body", body_cap);
      if (body == NULL)
        {
          ret = -ENOMEM;
          break;
        }

      body[0] = '\0';
      bj = (struct sjoin_s) { body, body_cap, 0, false };
      sjoin_add(&bj, prefix, (size_t)pfx_len);
      sjoin_add(&bj, msgs, msj.len);

      /* 尾巴两份，二选一：不带 tools 的整段，或者" ] , tools : [...] , "
       * 三段拼（tools 数组是上面按开关拼好的）—— 关掉的工具不进这个数组 */

      if (use_tools)
        {
          sjoin_addstr(&bj, MIMO_CHAT_JSON_TAIL_TOOLS_HEAD);
          sjoin_addstr(&bj, tools_json);
          sjoin_addstr(&bj, MIMO_CHAT_JSON_TAIL_TOOLS_END);
        }
      else
        {
          sjoin_addstr(&bj, MIMO_CHAT_JSON_TAIL_NO_TOOLS);
        }

      if (bj.ovf)
        {
          mimo_free_logged("chat.body", body, body_cap);
          ret = -ENOMEM;
          break;
        }

      ret = chat_round(round, body, reply_out, reply_cap, &tc);
      mimo_free_logged("chat.body", body, body_cap);

      if (ret < 0)
        {
          break;                     /* 网络 / 解析失败 */
        }

      if (ret == 0)
        {
          break;                     /* 拿到正文，已经在 reply_out 里 */
        }

      /* ret == 1：模型要调工具。按**函数名**分派 —— tools 表里有两个工具，
       * 名字对不上（模型幻觉出来的）也不能把整轮对话打死：回一句"没这个工具"
       * 喂回去，让它下一轮改用正文回答用户。 */

      if (strcmp(tc.name, "add_reminder") == 0 && use_reminder)
        {
          /* 建提醒：交给 app 注册进来的回调落地（robot_ui 注册的实现会写进
           * 提醒列表、重挂 RTC 闹钟、刷新界面）。失败原因由它写成中文工具
           * 结果，不回负值 —— 让模型自己跟用户解释。 */

          ret = chat_tool_add_reminder(tc.args, tool_result,
                                       sizeof(tool_result));
          if (ret < 0)
            {
              break;
            }
        }
      else if (strcmp(tc.name, "get_weather") == 0 && use_weather)
        {
          if (chat_tool_city(tc.args, city_hint, sizeof(city_hint)) != OK)
            {
              /* 模型没给城市（或者 arguments 不是我们预期的形状）：用默认城市 */

              snprintf(city_hint, sizeof(city_hint), "%s", city);
            }

          ret = weather_lookup(city_hint, tool_result, sizeof(tool_result));

          if (ret < 0)
            {
              syslog(LOG_ERR, "[%s] chat: get_weather(%s) 网络失败 %d\n", TAG,
                     city_hint, ret);
              break;
            }

          syslog(LOG_INFO, "[%s] chat: get_weather(%s) 结果 %zu 字节\n", TAG,
                 city_hint, strlen(tool_result));
        }
      else
        {
          /* 名字不认识，或者这个工具这一轮根本不在 tools 表里（模型不该
           * 知道它）。当成工具结果喂回去，别用负值收场。 */

          syslog(LOG_WARNING,
                 "[%s] chat: 收到不可用的工具 %s（提醒=%s 天气=%s）\n", TAG,
                 tc.name[0] != '\0' ? tc.name : "(空名)",
                 use_reminder ? "开" : "关", use_weather ? "开" : "关");

          snprintf(tool_result, sizeof(tool_result),
                   "本机没有名为 %s 的工具，请直接用中文回答用户。", tc.name);
          ret = OK;
        }

      if (round == MIMO_CHAT_MAX_ROUNDS)
        {
          /* 轮次用完模型还在要工具：不再发请求，把工具结果直接当回复给用户
           * （总比让界面报"AI 请求失败"强） */

          syslog(LOG_WARNING,
                 "[%s] chat: %d 轮上限仍在要工具，直接用工具结果兜底\n", TAG,
                 MIMO_CHAT_MAX_ROUNDS);
          snprintf(reply_out, reply_cap, "%s", tool_result);
          ret = (int)strlen(reply_out);
          break;
        }

      /* 把这一轮的 assistant(tool_calls) 和 tool(结果) 两条消息追加进
       * messages，下一轮再问一次。arguments 直接抄响应里的原文（它本来就是
       * 转义好的 JSON 文本）。 */

      sjoin_addstr(&msj,
                   ",{\"role\":\"assistant\",\"content\":null,\"tool_calls\":"
                   "[{\"id\":\"");
      sjoin_add_escaped(&msj, tc.id, strlen(tc.id));
      sjoin_addstr(&msj, "\",\"type\":\"function\",\"function\":{\"name\":\"");
      sjoin_add_escaped(&msj, tc.name, strlen(tc.name));
      sjoin_addstr(&msj, "\",\"arguments\":\"");
      sjoin_add(&msj, tc.args, strlen(tc.args));
      sjoin_addstr(&msj, "\"}}]},{\"role\":\"tool\",\"tool_call_id\":\"");
      sjoin_add_escaped(&msj, tc.id, strlen(tc.id));
      sjoin_addstr(&msj, "\",\"content\":\"");
      sjoin_add_escaped(&msj, tool_result, strlen(tool_result));
      sjoin_addstr(&msj, "\"}");

      if (msj.ovf)
        {
          syslog(LOG_ERR, "[%s] chat: messages 追加不下（缓冲 %zu 字节）\n",
                 TAG, msgs_cap);
          ret = -ENOMEM;
          break;
        }
    }

  mimo_free_logged("chat.msgs", msgs, msgs_cap);

  if (ret < 0)
    {
      return ret;
    }

  trim_whitespace(reply_out);

  if (reply_out[0] == '\0')
    {
      syslog(LOG_ERR, "[%s] chat: 回复正文为空（max_tokens 被思维链吃光？）\n",
             TAG);
      return -EPROTO;
    }

  ret = (int)strlen(reply_out);
  syslog(LOG_INFO, "[%s] chat: 回复 %d 字节\n", TAG, ret);
  return ret;
}
