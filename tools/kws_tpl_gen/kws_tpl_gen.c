/* kws_tpl_gen —— 在 **PC 上**生成唤醒词出厂模板（slotN.tpl）。
 *
 * ============================================================================
 * 为什么要有它
 * ============================================================================
 * 板子上录模板的唯一入口是 `hw_test kws enroll`，但那要麦克风，而 hello_app
 * 常开麦一直占着它（且按 2026-09-19 的决定设计上停不下来）→ -EBUSY，过不去。
 *
 * 而 kws_dtw.h 里本来就写明模板有两条来源，其中 **(a) 出厂模板**是随 ROMFS
 * 打包、开机 kws_init() 自动补装的，压根不需要板子的麦：
 *
 *     (a) 出厂模板：放进 ROMFS 的 /etc/assets/kws/slotN.tpl 随固件打包，
 *         开机时 kws_init() 自动把它补到 /data/kws（目标已存在则不覆盖，
 *         现场录的优先）
 *
 * 本工具就是把 (a) 这条路在 PC 上跑通：把 kws_dtw.c **原样**编到 x86，喂一段
 * 你自己录的 WAV，跑的就是板上那一份 kws_enroll() —— 特征口径、文件格式、
 * 校验和完全同一份代码，不存在"PC 算出来的板子读不懂"。
 *
 * ============================================================================
 * 怎么用
 * ============================================================================
 *     ./build.sh
 *     ./kws_tpl_gen 你好openvela.wav                 → slot0（你好，openvela）
 *     ./kws_tpl_gen 你好openvela.wav Hello.wav       → slot0 + slot1
 *
 * 录音要求（**不满足会直接失败，看它打印的那行**）：
 *   · 16 kHz / 单声道 / 16 bit —— 这三条是 kws_dtw 的固定口径，写进模板头校验
 *   · 一句完整短语，**前后各留 0.3~0.5 秒安静**（端点检测靠它起停）
 *   · 有效语音 ≤ 2.0 秒（KWS_MAX_FEAT = 100 点 × 20ms），"你好，openvela"
 *     正常念是 1.5~2.0 秒，念太慢会被截成残模板
 *   · 用**准备在板子上对着说话的那个位置/距离**录最好；条件实在不允许就先用
 *     PC/手机录一版，等串口写入修好之后再用 `hw_test kws enroll` 上板补录
 *     （现场录的优先级高于出厂模板，补录会自动盖掉它）
 *
 * 产物：out 目录下的 slot0.tpl / slot1.tpl，直接拷进
 *   board/contest_board/src/etc/assets/kws/  重新打包烧录即可。
 *
 * ============================================================================
 * 诚实预期（阈值这块别指望一次到位）
 * ============================================================================
 * kws_dtw.h 里实测：类内距离 620~660、类外 2450~3100，默认阈值 1800。
 * 但同音节换序的负例能压到 1806 —— margin 比看起来小。所以：
 *   · 换说话人/换口音（目标用户是老人、可能说冀鲁官话）必须**上板实测类内距离**
 *   · 想要余量就把阈值收到 1100~1300；**绝不能超过 9000**（超了等于全命中）
 *   · 这些都得等唤醒链路真跑起来、从串口读到命中/不命中才能标定
 * 本工具只负责把模板做对，**不负责把阈值调对**。
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kws_dtw.h"

/* ---- WAV 读取（只认 16k / mono / s16le，其余明确拒绝，不做隐式转换）---- */

typedef struct
{
  int16_t *pcm;
  size_t   nsamples;
} wav_t;

static uint32_t rd_u32(const unsigned char *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16(const unsigned char *p)
{
  return (uint16_t)(p[0] | (p[1] << 8));
}

/* 读一个 RIFF/WAVE。成功返回 0；失败打印原因并返回 -1（不 exit，好让调用方
 * 一次性报完所有坏文件）。 */
static int wav_read(const char *path, wav_t *out)
{
  FILE         *f;
  long          flen;
  unsigned char hdr[12];
  unsigned char ch[8];
  uint16_t      fmt = 0, chans = 0, bits = 0;
  uint32_t      rate = 0;
  int           have_fmt = 0, have_data = 0;
  uint32_t      data_off = 0, data_len = 0;

  memset(out, 0, sizeof(*out));

  f = fopen(path, "rb");
  if (f == NULL)
    {
      printf("  ✗ %s：打不开（%s）\n", path, strerror(errno));
      return -1;
    }

  if (fread(hdr, 1, 12, f) != 12 ||
      memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
    {
      printf("  ✗ %s：不是 RIFF/WAVE 文件\n", path);
      fclose(f);
      return -1;
    }

  /* 逐块走，别假设 fmt 就在第 12 字节 —— 现代录音工具常在中间塞 LIST/cue。
   *
   * ⚠ 取完文件长度**必须 seek 回块区起点**：fseek(SEEK_END) 把位置留在了
   * EOF，下面 `ftell(f) + 8 <= flen` 第一拍就是假，整个块循环一次都不跑，
   * 于是每个文件都报"缺 fmt 块"。2026-10-30 自检第一版就栽在这儿 ——
   * 连"纯静音必须失败"那条负例都是因为同一个原因才"过了"，等于没测。 */

  fseek(f, 0, SEEK_END);
  flen = ftell(f);
  fseek(f, 12, SEEK_SET);

  while (ftell(f) + 8 <= flen)
    {
      uint32_t clen;
      long     body;

      if (fread(ch, 1, 8, f) != 8)
        {
          break;
        }

      clen = rd_u32(ch + 4);
      body = ftell(f);          /* 本块载荷起点 —— 循环末尾统一从这儿跳 */

      if (memcmp(ch, "fmt ", 4) == 0)
        {
          unsigned char fmtbuf[32];
          size_t        want = clen > sizeof(fmtbuf) ? sizeof(fmtbuf) : clen;

          if (fread(fmtbuf, 1, want, f) != want)
            {
              break;
            }

          if (clen < 16)
            {
              printf("  ✗ %s：fmt 块太短（%u 字节）\n", path, (unsigned)clen);
              fclose(f);
              return -1;
            }

          fmt   = rd_u16(fmtbuf);
          chans = rd_u16(fmtbuf + 2);
          rate  = rd_u32(fmtbuf + 4);
          bits  = rd_u16(fmtbuf + 14);

          /* WAVE_FORMAT_EXTENSIBLE 的真实格式在 SubFormat GUID 的头 2 字节 */

          if (fmt == 0xfffe && clen >= 40)
            {
              fmt = rd_u16(fmtbuf + 24);
            }

          have_fmt = 1;
        }
      else if (memcmp(ch, "data", 4) == 0)
        {
          data_off = (uint32_t)body;
          data_len = clen;
          have_data = 1;
        }

      /* 统一跳到下一块：载荷 + 奇数长度的 1 字节对齐填充。
       * 三个分支都不自己 fseek，免得哪一路漏了对齐字节、后面的块就全错位
       * （那会以"不是 RIFF/WAVE 文件"或读出垃圾的方式表现出来，极难查）。 */

      if (fseek(f, body + (long)clen + (long)(clen & 1), SEEK_SET) != 0)
        {
          break;
        }
    }

  if (!have_fmt || !have_data)
    {
      printf("  ✗ %s：缺 %s 块\n", path, !have_fmt ? "fmt" : "data");
      fclose(f);
      return -1;
    }

  if (fmt != 1)
    {
      printf("  ✗ %s：不是 PCM（format=%u），不支持压缩格式\n", path, fmt);
      fclose(f);
      return -1;
    }

  if (chans != 1 || bits != 16 || rate != KWS_SAMPLE_RATE)
    {
      printf("  ✗ %s：%u Hz / %u 声道 / %u bit —— kws 只认 "
             "%d Hz / 1 声道 / 16 bit\n",
             path, (unsigned)rate, (unsigned)chans, (unsigned)bits,
             KWS_SAMPLE_RATE);
      printf("        转换：ffmpeg -i 输入 -ac 1 -ar %d -sample_fmt s16 输出.wav\n",
             KWS_SAMPLE_RATE);
      fclose(f);
      return -1;
    }

  if (data_len < 4 || (data_len & 1) != 0)
    {
      printf("  ✗ %s：data 块长度 %u 不合法\n", path, (unsigned)data_len);
      fclose(f);
      return -1;
    }

  out->pcm = (int16_t *)malloc(data_len);
  if (out->pcm == NULL)
    {
      printf("  ✗ %s：%u 字节分配不出来\n", path, (unsigned)data_len);
      fclose(f);
      return -1;
    }

  fseek(f, (long)data_off, SEEK_SET);
  if (fread(out->pcm, 1, data_len, f) != data_len)
    {
      printf("  ✗ %s：data 块读不全\n", path);
      free(out->pcm);
      out->pcm = NULL;
      fclose(f);
      return -1;
    }

  out->nsamples = data_len / 2;
  fclose(f);
  return 0;
}

/* 前后补静音。默认开：端点检测靠"前后有静音"起停，录得太贴边就会被判成
 * "有效语音 0 点"。补的是纯零，对已经有静音的录音没有副作用。 */
static int wav_pad(const wav_t *w, size_t pad_samples, wav_t *out)
{
  size_t n = w->nsamples + 2 * pad_samples;

  out->pcm = (int16_t *)calloc(n, sizeof(int16_t));
  if (out->pcm == NULL)
    {
      return -1;
    }

  memcpy(out->pcm + pad_samples, w->pcm, w->nsamples * sizeof(int16_t));
  out->nsamples = n;
  return 0;
}

/* 量一下这段录音的电平，好在失败时给出"是没声音还是念太长"的分辨 */
static void report_level(const wav_t *w, double *peak, double *rms)
{
  long   sum2 = 0;
  int    p = 0;
  size_t i;

  for (i = 0; i < w->nsamples; i++)
    {
      int v = w->pcm[i];
      if (v < 0)
        {
          v = -v;
        }
      if (v > p)
        {
          p = v;
        }
      sum2 += (long)w->pcm[i] * w->pcm[i];
    }

  *peak = p / 32768.0;
  *rms  = w->nsamples ? sqrt((double)sum2 / (double)w->nsamples) / 32768.0 : 0.0;
}

static void usage(void)
{
  printf("用法：kws_tpl_gen [-o 输出目录] [-p 补静音毫秒] 文件1.wav [文件2.wav ...]\n");
  printf("  按给定顺序装进 slot0、slot1…（slot0 =「你好，openvela」）\n");
  printf("  -o  输出目录（默认当前目录）\n");
  printf("  -p  前后各补多少毫秒纯静音（默认 300；录音本来就带静音可给 0）\n");
  printf("  录音必须是 16kHz / 单声道 / 16bit，一句 ≤2.0 秒，前后带静音。\n");
}

int main(int argc, char **argv)
{
  const char *outdir = ".";
  int         pad_ms = 300;
  int         first;
  int         n_ok   = 0;
  int         i;

  for (i = 1; i < argc; i++)
    {
      if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
          usage();
          return 0;
        }
      else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
        {
          outdir = argv[++i];
        }
      else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
        {
          pad_ms = atoi(argv[++i]);
          if (pad_ms < 0 || pad_ms > 2000)
            {
              printf("-p 取 0~2000 毫秒\n");
              return 2;
            }
        }
      else if (argv[i][0] == '-')
        {
          printf("不认识的选项：%s\n", argv[i]);
          usage();
          return 2;
        }
      else
        {
          break;
        }
    }

  first = i;

  if (first >= argc)
    {
      usage();
      return 2;
    }

  if (argc - first > KWS_MAX_TEMPLATES)
    {
      printf("★ 最多 %d 条模板（KWS_MAX_TEMPLATES），给了 %d 个文件\n",
             KWS_MAX_TEMPLATES, argc - first);
      return 2;
    }

  /* kws_tpl_save() 写的是编译期定死的 KWS_DATA_DIR（相对路径 "kws_out"），
   * 所以靠 chdir 落到你指定的输出目录。这样不用在板子那份代码里加运行时
   * 配置项 —— 那份是要进固件的，越少动越好。 */

  if (chdir(outdir) != 0)
    {
      printf("★ 进不去输出目录 %s（%s）—— 先建好它\n", outdir, strerror(errno));
      return 2;
    }

  printf("输出目录：%s    补静音：%d ms\n", outdir, pad_ms);
  printf("特征口径：%d 维 Q%d  一个点 %dms  最长 %d 点\n\n",
         KWS_NUM_MFCC, KWS_MFCC_Q, KWS_FEAT_MS, KWS_MAX_FEAT);

  if (kws_init() < 0)
    {
      printf("★ kws_init() 失败\n");
      return 1;
    }

  for (i = first; i < argc; i++)
    {
      wav_t   raw, padded;
      int     slot = i - first;
      double  peak, rms;
      int     rc;

      printf("── slot%d ← %s\n", slot, argv[i]);

      if (wav_read(argv[i], &raw) != 0)
        {
          continue;
        }

      report_level(&raw, &peak, &rms);
      printf("     %zu 采样 = %.2f 秒，峰值 %.0f%%，RMS %.1f%%\n",
             raw.nsamples, (double)raw.nsamples / KWS_SAMPLE_RATE,
             peak * 100.0, rms * 100.0);

      if (peak < 0.02)
        {
          printf("  ✗ 峰值只有 %.1f%%，基本是静音 —— 检查是不是录错通道/没开麦\n",
                 peak * 100.0);
          free(raw.pcm);
          continue;
        }

      if (raw.nsamples > (size_t)KWS_SAMPLE_RATE * 4)
        {
          printf("  ⚠ 有 %.1f 秒，超过 4 秒 —— 端点检测可能会截错，"
                 "建议只留那句话本身\n",
                 (double)raw.nsamples / KWS_SAMPLE_RATE);
        }

      if (wav_pad(&raw, (size_t)KWS_SAMPLE_RATE * pad_ms / 1000, &padded) != 0)
        {
          printf("  ✗ 补静音时分配不出来\n");
          free(raw.pcm);
          continue;
        }
      free(raw.pcm);

      rc = kws_enroll(padded.pcm, padded.nsamples, slot);
      free(padded.pcm);

      if (rc == 0)
        {
          printf("  ✓ 好了：%d 个特征点（%dms）→ %s/slot%d.tpl\n",
                 kws_template_frames(slot),
                 kws_template_frames(slot) * KWS_FEAT_MS,
                 KWS_DATA_DIR, slot);
          n_ok++;
        }
      else if (rc == 1)
        {
          printf("  ✗ 特征算出来了但**没写进盘** —— 看上面 kws_tpl_save 的报错\n");
        }
      else if (rc == -ENOSPC)
        {
          printf("  ✗ 这句话太长（有效语音超过 %d 点 = %dms），念快一点或删掉"
                 "前后的拖音\n", KWS_MAX_FEAT, KWS_MAX_FEAT * KWS_FEAT_MS);
        }
      else if (rc == -EINVAL)
        {
          printf("  ✗ 端点检测没找到有效语音 —— 八成是前后没静音，或声音太小。\n");
          printf("        用 -p 800 多补点静音试试，或者重录时前后各留半秒不动。\n");
        }
      else
        {
          printf("  ✗ kws_enroll 返回 %d（%s）\n", rc, strerror(-rc));
        }
    }

  printf("\n");
  if (n_ok == 0)
    {
      printf("★ 一条都没成。\n");
      return 1;
    }

  printf("★ %d 条模板已生成。下一步：\n", n_ok);
  printf("   1) 拷进 ROMFS：cp %s/%s/slot*.tpl \\\n"
         "        board/contest_board/src/etc/assets/kws/\n", outdir, KWS_DATA_DIR);
  printf("   2) 重新打包烧录（ROMFS 要删了再编才会重生成）\n");
  printf("   3) 板子起来看串口：[KWS] 唤醒词模板 %d 条 —— 变成 %d 就装上了\n",
         n_ok, n_ok);
  printf("   4) 对板子说「你好，openvela」，串口看唤醒命中\n");
  printf("\n");
  printf("⚠ 阈值没在这一步定。kws_dtw.h 实测类内 620~660 / 类外 2450~3100，\n");
  printf("  默认 1800；老人/冀鲁官话的类内距离**必须上板实测**才能定。\n");
  printf("  想要余量可试 1100~1300，**绝不要超过 9000**（超了等于全命中）。\n");
  return 0;
}
