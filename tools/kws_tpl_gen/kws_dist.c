/* kws_dist —— 量「这段录音」离「某条模板」有多远（kws_dtw 的 DTW 距离）。
 *
 * ============================================================================
 * 为什么要有它
 * ============================================================================
 * kws_dtw **只有命中才打日志**（`[KWS] 命中唤醒词（slot%d，距离 %d ≤ 阈值 %d）`），
 * 近失是静默 return 0。所以板子上「喊了没反应」这件事，光看串口**差多远都不知道**
 * —— 阈值该往哪边调完全没抓手。
 *
 * 本工具把 kws_distance_pcm() 那条路搬到 PC 上跑，于是可以：
 *   · 拿同一句话的不同版本（变音量/加噪声/轻微变速）量类内距离，
 *     看默认阈值 1800 到底有多紧；
 *   · 拿别的句子/别的人量类外距离，看误唤醒风险；
 *   · 全部离线，不占板子的麦克风（那玩意儿被 hello_app 常开麦占着）。
 *
 * 距离口径见 kws_dtw.h：「每维每帧 RMS 距离 × 1000」。合成信号实测
 * 类内 620~660、类外 2450~3100、默认阈值 1800，但同音节换序能压到 1806。
 *
 * 用法：
 *   ./kws_dist <slot0.tpl 路径> 被测.wav [被测2.wav ...]
 *   ./kws_dist --dir out 被测.wav [...]     # out/ 就是 kws_tpl_gen -o 的那个目录
 *
 * ⚠ --dir 给的是**输出目录**（里面是 kws_out/slotN.tpl），不是 kws_out 本身 ——
 *   跟 kws_tpl_gen -o out 的落盘布局对齐，省得记两套路径。
 *
 * 输出一行一个文件：
 *   文件名  端点后 N 点(=N×20ms)  →  slot0 距离 = XXXX  （≤1800 会命中 / >1800 不命中）
 *
 * ⚠「超带宽」= 长度差超出 Sakoe-Chiba 带宽，kws_dtw 直接判"不是这句话"。
 *   那不是"距离很大"，是"压根没比"。两种情况在板子上都是不命中，但含义不同。
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kws_dtw.h"

/* ---- WAV 读取：和 kws_tpl_gen.c 同一套（只认 16k/mono/s16le）---- */

static uint32_t rd_u32(const unsigned char *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16(const unsigned char *p)
{
  return (uint16_t)(p[0] | (p[1] << 8));
}

static int wav_read(const char *path, int16_t **pcm, size_t *nsamples)
{
  FILE         *f;
  long          flen;
  unsigned char hdr[12], ch[8];
  uint16_t      fmt = 0, chans = 0, bits = 0;
  uint32_t      rate = 0, data_off = 0, data_len = 0;
  int           have_fmt = 0, have_data = 0;

  *pcm = NULL;
  *nsamples = 0;

  f = fopen(path, "rb");
  if (f == NULL)
    {
      printf("  ✗ %s：打不开（%s）\n", path, strerror(errno));
      return -1;
    }

  if (fread(hdr, 1, 12, f) != 12 ||
      memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
    {
      printf("  ✗ %s：不是 RIFF/WAVE\n", path);
      fclose(f);
      return -1;
    }

  fseek(f, 0, SEEK_END);
  flen = ftell(f);
  fseek(f, 12, SEEK_SET);       /* ★ 取完长度必须跳回块区，否则循环一次都不跑 */

  while (ftell(f) + 8 <= flen)
    {
      uint32_t clen;
      long     body;

      if (fread(ch, 1, 8, f) != 8)
        {
          break;
        }

      clen = rd_u32(ch + 4);
      body = ftell(f);

      if (memcmp(ch, "fmt ", 4) == 0)
        {
          unsigned char b[32];
          size_t want = clen > sizeof(b) ? sizeof(b) : clen;

          if (fread(b, 1, want, f) != want || clen < 16)
            {
              break;
            }

          fmt   = rd_u16(b);
          chans = rd_u16(b + 2);
          rate  = rd_u32(b + 4);
          bits  = rd_u16(b + 14);
          if (fmt == 0xfffe && clen >= 40)
            {
              fmt = rd_u16(b + 24);
            }
          have_fmt = 1;
        }
      else if (memcmp(ch, "data", 4) == 0)
        {
          data_off = (uint32_t)body;
          data_len = clen;
          have_data = 1;
        }

      if (fseek(f, body + (long)clen + (long)(clen & 1), SEEK_SET) != 0)
        {
          break;
        }
    }

  if (!have_fmt || !have_data || fmt != 1 || chans != 1 || bits != 16 ||
      rate != KWS_SAMPLE_RATE || data_len < 4 || (data_len & 1))
    {
      printf("  ✗ %s：%s（只认 %dHz/单声道/16bit PCM）\n", path,
             !have_fmt ? "缺 fmt" : !have_data ? "缺 data" :
             fmt != 1 ? "不是 PCM" :
             (chans != 1 || bits != 16 || rate != KWS_SAMPLE_RATE) ?
             "格式不对" : "data 长度不合法", KWS_SAMPLE_RATE);
      fclose(f);
      return -1;
    }

  *pcm = (int16_t *)malloc(data_len);
  if (*pcm == NULL)
    {
      fclose(f);
      return -1;
    }

  fseek(f, (long)data_off, SEEK_SET);
  if (fread(*pcm, 1, data_len, f) != data_len)
    {
      free(*pcm);
      *pcm = NULL;
      fclose(f);
      return -1;
    }

  *nsamples = data_len / 2;
  fclose(f);
  return 0;
}

int main(int argc, char **argv)
{
  const char *tpl_dir = NULL;
  const char *tpl_file = NULL;
  int         i, first;

  if (argc < 3)
    {
      printf("用法：kws_dist <slotN.tpl> 被测.wav [被测2.wav ...]\n");
      printf("      kws_dist --dir 模板目录 被测.wav [...]\n");
      return 2;
    }

  if (strcmp(argv[1], "--dir") == 0)
    {
      if (argc < 4)
        {
          printf("--dir 要给目录和至少一个 wav\n");
          return 2;
        }
      tpl_dir = argv[2];
      first = 3;
    }
  else
    {
      tpl_file = argv[1];
      first = 2;
    }

  /* ★ 先把 wav 路径钉成绝对的 —— 下面会 chdir 进模板目录，
   *   届时相对路径（尤其 --dir 那种）会解析到完全另一个文件去。 */
  for (i = first; i < argc; i++)
    {
      char *abs = realpath(argv[i], NULL);

      if (abs == NULL)
        {
          printf("★ 找不到 %s（%s）\n", argv[i], strerror(errno));
          return 2;
        }
      argv[i] = abs;
    }

  if (kws_init() < 0)
    {
      printf("★ kws_init() 失败\n");
      return 1;
    }

  /* 模板从哪来：--dir 时把工作目录切过去，kws_template_load_all() 会去
   * KWS_DATA_DIR 读；单文件时把它拷成 KWS_DATA_DIR/slot0.tpl 再载入。 */

  if (tpl_dir != NULL)
    {
      if (chdir(tpl_dir) != 0)
        {
          printf("★ 进不去模板目录 %s（%s）\n", tpl_dir, strerror(errno));
          return 2;
        }
    }
  else
    {
      char   buf[65536];
      FILE  *in, *out;
      size_t n;

      if (mkdir(KWS_DATA_DIR, 0777) < 0 && errno != EEXIST)
        {
          printf("★ 建 %s 失败（%s）\n", KWS_DATA_DIR, strerror(errno));
          return 2;
        }

      in = fopen(tpl_file, "rb");
      if (in == NULL)
        {
          printf("★ 打不开模板 %s\n", tpl_file);
          return 2;
        }
      out = fopen(KWS_DATA_DIR "/slot0.tpl", "wb");
      if (out == NULL)
        {
          printf("★ 写不了 %s/slot0.tpl\n", KWS_DATA_DIR);
          fclose(in);
          return 2;
        }
      while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        {
          fwrite(buf, 1, n, out);
        }
      fclose(in);
      fclose(out);
    }

  if (kws_template_load_all() <= 0)
    {
      printf("★ 一条模板都没载入 —— 查 %s/slot*.tpl\n", KWS_DATA_DIR);
      return 1;
    }

  printf("阈值 %d，已载入 %d 条模板\n", kws_get_threshold(), kws_ready_count());
  printf("（距离 = 每维每帧 RMS × 1000；≤ 阈值才会命中）\n\n");

  for (i = first; i < argc; i++)
    {
      int16_t *pcm;
      size_t   ns;
      int      s;

      if (wav_read(argv[i], &pcm, &ns) != 0)
        {
          continue;
        }

      {
        const char *nm = strrchr(argv[i], '/');

        printf("%-34s %5.2fs", nm ? nm + 1 : argv[i], (double)ns / KWS_SAMPLE_RATE);
      }

      for (s = 0; s < KWS_MAX_TEMPLATES; s++)
        {
          int d = 0;
          int rc;

          if (!kws_slot_ready(s))
            {
              continue;
            }

          /* ★ 距离是出参，返回值只报"能不能比"。
           *   rc<0 分三种：太短 / 超带宽 / 参数非法 —— 都是"没比成"，
           *   不是"距离很大"。 */
          rc = kws_distance_pcm(pcm, ns, s, &d);

          if (rc != 0)
            {
              printf("   slot%d=  没比成(rc=%d: %s)", s, rc,
                     rc == -EINVAL ? "太短或超带宽" : "未初始化");
            }
          else if (d <= kws_get_threshold())
            {
              printf("   slot%d=%5d ✓命中", s, d);
            }
          else
            {
              printf("   slot%d=%5d ✗", s, d);
            }
        }

      printf("\n");
      free(pcm);
    }

  printf("\n判读：\n");
  printf("  同一个人换口气/变音量/加点噪声，距离通常在 500~1200 —— 这是类内。\n");
  printf("  别的句子/别的人应该 2000 以上 —— 这是类外。\n");
  printf("  阈值要落在两者中间偏类内一侧（老人/方言的类内会更散）。\n");
  printf("  ⚠ 别超过 9000（距离被 DTW 的 DP 上限钉住，超了等于全命中）。\n");
  return 0;
}
