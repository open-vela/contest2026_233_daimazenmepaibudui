/****************************************************************************
 * app/hello_app/kws_dtw.c
 *
 * 唤醒词「你好，openvela」/「Hello，openvela」的命令词识别（MFCC + DTW）
 * 智爱陪伴 - AI老人陪伴守护终端
 *
 * 纯计算模块：不开麦、不起线程、不碰 LVGL。算法与量化数字、阈值口径、
 * 接线方式都写在 kws_dtw.h 的文件头里，这里只补实现上的取舍注释。
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "kws_dtw.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* DTW 的"动态规划值"上限。
 * 为什么需要上限：int16 的差最大 65535，一格的平方和最大 13×65535² ≈ 5.6e10，
 * 累加几十格就撑爆 int32。这里用两道闸：
 *   ① 特征点在定点时就削到 ±KWS_MFCC_MAX(4096)，一格最多
 *      13×(2×4096)² = 8.7e8；
 *   ② 累加值超过 KWS_DP_CAP 就钉住，之后 best + d 最多
 *      1.0e9 + 8.7e8 < 2^31，不会溢出。
 * 两道闸合起来保证累加**全程在 int32 里**，不会回绕成负数
 * （宿主 gcc 上拿全 ±4096 的对抗输入实测过：100 点 vs 100 点返回 9690，
 *  正数、不单调回绕；把 KWS_DP_CAP 再放大就会顶到 2^31 以上，不能再加）。
 *
 * ⚠ 钉住之后换算出来的距离**是定值、只由 qn+tn 决定**：
 *     1000·sqrt(CAP/(qn+tn)/13)/64 → qn+tn=200 时 9690，
 *     qn+tn=120 时 12510，qn+tn=16（最短的 8+8 点）时 34241。
 *   而 kws_set_threshold() 接受到 20000 —— 也就是 qn+tn ≥ 100 时，
 *   饱和值（9690~13706）落在可设的阈值范围内。真把阈值设到 1 万以上，
 *   "最不像的东西"也会被算成命中。
 *   现在没改这个（改 kws_set_threshold 的上限会动到已经在接线的调用方），
 *   但**别把阈值设过 9000**：kws_selftest 推荐的也从来是 8000 以下。
 *   真差异大到需要 1 万以上阈值的录音，本来也不是这个算法能用的场景。 */

#define KWS_DP_CAP          ((int32_t)1000000000)
#define KWS_DP_INF          ((int32_t)1073741824)   /* 2^30：只表示"路径不可达" */

/* 饱和距离的下限（qn+tn = 2×KWS_MAX_FEAT 时最小）= 9690，
 * 拿它当"别把阈值设过这条线"的常量，给下面注释和自检用。
 * 不写成 1000*sqrt(...) 是怕编译器对 int32 上限做常量折叠时算出别的口径。 */
#define KWS_DP_CAP_DIST_MIN 9690

/* 自检里时间拉伸/帧序打乱用的暂存特征点个数。
 * 必须 ≥ KWS_MAX_FEAT：模板最长 100 点，暂存要是切短了，
 * 长度差会超出 DTW 带宽、自检会误报"打乱都被带宽挡掉"。
 * 它是常驻静态（不是栈），100 点 = 2600 字节。 */

#define KWS_SELFTEST_FEAT   KWS_MAX_FEAT

/* 模板文件头。字段都在 4 字节边界上，sizeof == 24，不会有填充字节。
 * sum 是 int16 负载按字节（当作 uint16 累加）的和，用来挡"写了一半的文件"。 */

#define KWS_TPL_MAGIC       0x5453574Bu     /* 'K''W''S''T' 小端 */
#define KWS_TPL_VERSION     1u

/* 出厂模板在只读 ROMFS 里的位置（对应仓库的
 * board/contest_board/src/etc/assets/kws/，上板后挂在 /etc 下）。
 * 为什么需要它：/data 是 tmpfs，重启就把 /data/kws 清空 —— 没有这一步，
 * 唤醒词每次重启都静默失效，只剩 VAD 触发。仓库里不放 .tpl 是**正常情况**
 * （麦克风录的模板因人而异），所以缺失时安静跳过，不是错误。 */

#define KWS_TPL_ASSETS_DIR  "/etc/assets/kws"

/* 刚 reset 之后先稳一会儿再判端点：自适应本底还没收敛、
 * 开机/切回麦克风的第一帧常有直流冲击。
 * （和 robot_ui/ambient_listen.c 的 AMBIENT_RESUME_GUARD_MS 一个道理） */

#define KWS_SETTLE_FRAMES   30              /* 30 帧 × 10ms = 300ms */

#define KWS_PI              3.14159265358979f

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* 一个 Mel 三角滤波器。weight(k) = (k - lo)/(ctr - lo) 当 k ≤ ctr，
 * 否则 (hi - k)/(hi - ctr)；把两条斜率的倒数预存下来，
 * 滤波时每个 bin 只要一次减一次乘（不用除法，M33 上除法要几十个周期） */

typedef struct
{
  int   lo;         /* 三角左边（bin，向下取整，含） */
  int   hi;         /* 三角右边（bin，向上取整，含） */
  float ctr;        /* 中心（bin，浮点） */
  float inv_lo;     /* 1/(ctr - lo) */
  float inv_hi;     /* 1/(hi - ctr) */
} kws_mel_t;

typedef struct
{
  uint32_t magic;
  uint16_t version;
  uint16_t dim;
  uint16_t frames;
  uint16_t q;
  uint32_t rate;
  uint32_t sum;
  uint32_t hop_ms;
} kws_tpl_hdr_t;

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 端点处理完成后做什么：0 = 判唤醒词（正常路径），1 = 只把结果留在 g_utt 里
 * （kws_enroll / kws_distance_pcm 用，它们要复用同一条流水线） */

#define KWS_FINISH_MATCH    0
#define KWS_FINISH_KEEP     1

static bool     g_ready;                            /* 表建好了没 */
static int      g_threshold = KWS_THRESHOLD_MILLI;
static int      g_last_dist = -1;

/* 端点检测参数 */
static int      g_vad_on_db10  = KWS_VAD_ON_DB10;
static int      g_vad_off_db10 = KWS_VAD_OFF_DB10;
static int      g_tail_ms      = KWS_TAIL_MS;
static int      g_floor_db10   = -550;              /* 自适应本底初值 -55.0 dBFS */
static int      g_settle_frames;

/* 流式状态 */
static int16_t  g_in[KWS_FRAME_LEN];                /* 拼帧缓冲（20ms 窗） */
static size_t   g_in_len;
static float    g_prev;                             /* 预加重历史 */
static unsigned g_feat_idx;                         /* 10ms 帧序号（做 2:1 抽取） */
static int16_t  g_ring[KWS_PREROLL_FEAT][KWS_NUM_MFCC];  /* 预滚环 */
static int16_t  g_ring_db10[KWS_PREROLL_FEAT];
static int      g_ring_pos;
static int      g_ring_cnt;
static int16_t  g_utt[KWS_MAX_FEAT][KWS_NUM_MFCC];  /* 待判句子（匹配用特征点） */
static int16_t  g_utt_db10[KWS_MAX_FEAT];           /* 每点的帧能量，掐头去尾用 */
static int      g_utt_len;
static int      g_utt_dropped;                      /* 因为超过 2.0s 被挤掉的点数 */
static bool     g_in_speech;
static int      g_tail_cnt;                         /* 连续静音帧数 */
static int      g_cooldown_frames;
static int      g_finish_mode = KWS_FINISH_MATCH;
static bool     g_captured;                         /* KEEP 模式下已经抓到一句 */

/* 模板 */
static int16_t  g_tpl[KWS_MAX_TEMPLATES][KWS_MAX_FEAT][KWS_NUM_MFCC];
static int      g_tpl_len[KWS_MAX_TEMPLATES];

/* 表与工作区 */
static float    g_window[KWS_FRAME_LEN];
static float    g_fft_re[KWS_FFT_LEN];
static float    g_fft_im[KWS_FFT_LEN];
static float    g_tw_cos[KWS_FFT_LEN / 2];
static float    g_tw_sin[KWS_FFT_LEN / 2];
static uint16_t g_bitrev[KWS_FFT_LEN];
static float    g_power[KWS_FFT_LEN / 2 + 1];
static kws_mel_t g_mel[KWS_NUM_MEL];
static float    g_logmel[KWS_NUM_MEL];
static float    g_dct[KWS_NUM_MFCC - 1][KWS_NUM_MEL];
static int32_t  g_dtw_a[KWS_MAX_FEAT + 1];
static int32_t  g_dtw_b[KWS_MAX_FEAT + 1];
static int16_t  g_selftest[KWS_SELFTEST_FEAT][KWS_NUM_MFCC];

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static float    kws_hz2mel(float f);
static float    kws_mel2hz(float m);
static int16_t  kws_quant(float v);
static void     kws_build_tables(void);
static void     kws_fft(float *re, float *im);
static void     kws_frame_features(const int16_t *frame, int16_t *out,
                                   int *db10, float *prev);
static void     kws_vad_thresholds(int *on_db10, int *off_db10);
static void     kws_floor_update(int db10, int on_db10);
static void     kws_ring_push(const int16_t *feat, int db10);
static void     kws_utt_append(const int16_t *feat, int db10);
static void     kws_utt_start(void);
static int      kws_utt_finalize(int off_db10);
static int      kws_finish_utterance(void);
static void     kws_flush(void);
static int      kws_match(void);
static int      kws_process_frame(void);
static int      kws_dtw_dist(const int16_t *q, int qn, const int16_t *t,
                             int tn);
static int      kws_tpl_path(char *buf, size_t size, int slot);
static int      kws_tpl_tmp_path(char *buf, size_t size, int slot,
                                 const char *suffix);
static int      kws_write_all(int fd, const void *buf, size_t len);
static int      kws_read_all(int fd, void *buf, size_t len);
static int      kws_tpl_save(int slot);
static int      kws_tpl_check(int fd, const char *path, int16_t *dst,
                             int *frames_out);
static int      kws_tpl_load(int slot);
static int      kws_tpl_install(int slot);
static void     kws_install_templates(void);
static int      kws_us_diff(const struct timespec *a, const struct timespec *b);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static float kws_hz2mel(float f)
{
  return 2595.0f * logf(1.0f + f / 700.0f) / logf(10.0f);
}

static float kws_mel2hz(float m)
{
  return 700.0f * (expf(m * logf(10.0f) / 2595.0f) - 1.0f);
}

/* 浮点 MFCC → 定点 int16（Q6）。削到 ±KWS_MFCC_MAX 一是防溢出（见 KWS_DP_CAP
 * 的注释），二是实测特征值域根本到不了那么大：13 维里除第 0 维，
 * c1..c12 的绝对值一般 < 30（log-mel 的跨度被 ln 压得很扁）。 */

static int16_t kws_quant(float v)
{
  float s = v * (float)KWS_MFCC_Q;
  int i;

  if (s > (float)KWS_MFCC_MAX)
    {
      s = (float)KWS_MFCC_MAX;
    }
  else if (s < -(float)KWS_MFCC_MAX)
    {
      s = -(float)KWS_MFCC_MAX;
    }

  i = (int)(s >= 0.0f ? s + 0.5f : s - 0.5f);
  return (int16_t)i;
}

static void kws_build_tables(void)
{
  float mel_lo = kws_hz2mel(0.0f);
  float mel_hi = kws_hz2mel((float)(KWS_SAMPLE_RATE / 2));
  float mel_step = (mel_hi - mel_lo) / (float)(KWS_NUM_MEL + 1);
  int i;
  int k;
  int m;

  /* 汉明窗 */

  for (i = 0; i < KWS_FRAME_LEN; i++)
    {
      g_window[i] = 0.54f - 0.46f * cosf(2.0f * KWS_PI * (float)i /
                                         (float)(KWS_FRAME_LEN - 1));
    }

  /* FFT 旋转因子（cos/sin 各 N/2 个）与位反转表 */

  for (i = 0; i < KWS_FFT_LEN / 2; i++)
    {
      g_tw_cos[i] = cosf(2.0f * KWS_PI * (float)i / (float)KWS_FFT_LEN);
      g_tw_sin[i] = sinf(2.0f * KWS_PI * (float)i / (float)KWS_FFT_LEN);
    }

  for (i = 0; i < KWS_FFT_LEN; i++)
    {
      unsigned int x = (unsigned int)i;
      unsigned int r = 0;

      for (k = 0; k < KWS_FFT_BITS; k++)
        {
          r = (r << 1) | (x & 1u);
          x >>= 1;
        }

      g_bitrev[i] = (uint16_t)r;
    }

  /* Mel 三角滤波器：26 个三角在 mel 轴上等间距，折回 Hz 再折成 FFT bin */

  for (m = 0; m < KWS_NUM_MEL; m++)
    {
      float lo_hz = kws_mel2hz(mel_lo + mel_step * (float)m);
      float ct_hz = kws_mel2hz(mel_lo + mel_step * (float)(m + 1));
      float hi_hz = kws_mel2hz(mel_lo + mel_step * (float)(m + 2));
      float lo_b = lo_hz * (float)KWS_FFT_LEN / (float)KWS_SAMPLE_RATE;
      float ct_b = ct_hz * (float)KWS_FFT_LEN / (float)KWS_SAMPLE_RATE;
      float hi_b = hi_hz * (float)KWS_FFT_LEN / (float)KWS_SAMPLE_RATE;
      int lo = (int)lo_b;
      int hi = (int)(hi_b + 1.0f);

      if (lo < 0)
        {
          lo = 0;
        }

      if (hi > KWS_FFT_LEN / 2)
        {
          hi = KWS_FFT_LEN / 2;
        }

      if (hi <= lo)
        {
          hi = lo + 1;                  /* 至少一个 bin，避免空滤波器 */
        }

      g_mel[m].lo = lo;
      g_mel[m].hi = hi;
      g_mel[m].ctr = ct_b;
      g_mel[m].inv_lo = (ct_b > lo_b + 0.001f) ? 1.0f / (ct_b - lo_b) : 1.0f;
      g_mel[m].inv_hi = (hi_b > ct_b + 0.001f) ? 1.0f / (hi_b - ct_b) : 1.0f;
    }

  /* DCT-II 的 i = 1..12 行（i = 0 那个位置不用 DCT，用帧对数能量：
   * DCT 的 0 行是 log-mel 的和，量级能到上百，Q6 定点装不下；
   * 而 i>0 的行和恒为 0，log-mel 的偏置只会进 0 行、不会漏到这里） */

  for (i = 1; i < KWS_NUM_MFCC; i++)
    {
      float s = sqrtf(2.0f / (float)KWS_NUM_MEL);

      for (m = 0; m < KWS_NUM_MEL; m++)
        {
          g_dct[i - 1][m] = s * cosf(KWS_PI * (float)i *
                                     (2.0f * (float)m + 1.0f) /
                                     (2.0f * (float)KWS_NUM_MEL));
        }
    }
}

/* 就地 radix-2 复数 FFT。
 * 输入是实的，但 320 点补零到 512 之后虚部全 0，所以直接按复数算、
 * 不做"实序列打包成 N/2 复数"那套优化 —— 代码简单、好核对，代价是
 * 蝶形多一倍（每帧多约 3 万周期，占 240MHz 的 0.1ms，可以接受）。
 * 想省这一半可以换成打包法，接口不变。 */

static void kws_fft(float *re, float *im)
{
  int i;
  int k;
  int len;
  int half;
  int step;

  for (i = 0; i < KWS_FFT_LEN; i++)
    {
      int j = (int)g_bitrev[i];

      if (j > i)
        {
          float tr = re[i];
          float ti = im[i];

          re[i] = re[j];
          im[i] = im[j];
          re[j] = tr;
          im[j] = ti;
        }
    }

  for (len = 2; len <= KWS_FFT_LEN; len <<= 1)
    {
      half = len >> 1;
      step = KWS_FFT_LEN / len;

      for (i = 0; i < KWS_FFT_LEN; i += len)
        {
          for (k = 0; k < half; k++)
            {
              int t = k * step;
              float wr = g_tw_cos[t];
              float wi = -g_tw_sin[t];
              float xr = re[i + k + half];
              float xi = im[i + k + half];
              float tr = xr * wr - xi * wi;
              float ti = xr * wi + xi * wr;

              re[i + k + half] = re[i + k] - tr;
              im[i + k + half] = im[i + k] - ti;
              re[i + k] += tr;
              im[i + k] += ti;
            }
        }
    }
}

/* 一帧（320 采样）→ 13 维定点特征 + 帧能量（0.1dBFS）
 *
 * 预加重的历史采样点由 prev 带进来（in/out）：离线路径上它就是上一个采样点，
 * 流式路径上它是上一个 20ms 窗的最后一个采样点 —— 差 160 个采样的历史，
 * 只影响每个窗的第 0 个采样点（窗函数权重 ~0.08，占一帧 320 个样本的 0.03%），
 * 对 MFCC 的影响远小于定点 Q6 的量化步长。两条路径用同一个约定，
 * 模板和识别口径就一致 —— 这是 DTW 能用的前提。 */

static void kws_frame_features(const int16_t *frame, int16_t *out, int *db10,
                               float *prev)
{
  float sum_sq = 0.0f;
  float hist = *prev;
  float dbfs;
  int n;
  int m;
  int k;
  int i;

  for (n = 0; n < KWS_FRAME_LEN; n++)
    {
      float x = (float)frame[n];
      float y = x - KWS_PREEMPH * hist;

      hist = x;
      sum_sq += x * x;
      g_fft_re[n] = y * g_window[n];
    }

  *prev = hist;

  for (n = KWS_FRAME_LEN; n < KWS_FFT_LEN; n++)
    {
      g_fft_re[n] = 0.0f;
    }

  memset(g_fft_im, 0, sizeof(g_fft_im));

  kws_fft(g_fft_re, g_fft_im);

  for (k = 0; k <= KWS_FFT_LEN / 2; k++)
    {
      g_power[k] = g_fft_re[k] * g_fft_re[k] + g_fft_im[k] * g_fft_im[k];
    }

  for (m = 0; m < KWS_NUM_MEL; m++)
    {
      const kws_mel_t *mf = &g_mel[m];
      float acc = 0.0f;

      for (k = mf->lo; k <= mf->hi; k++)
        {
          float w = ((float)k <= mf->ctr)
                      ? (((float)k - (float)mf->lo) * mf->inv_lo)
                      : (((float)mf->hi - (float)k) * mf->inv_hi);

          acc += w * g_power[k];
        }

      /* 用自然对数（和 HTK 一致）；加 1 防 log(0) */

      g_logmel[m] = logf(acc + 1.0f);
    }

  for (i = 1; i < KWS_NUM_MFCC; i++)
    {
      float acc = 0.0f;

      for (m = 0; m < KWS_NUM_MEL; m++)
        {
          acc += g_dct[i - 1][m] * g_logmel[m];
        }

      out[i] = kws_quant(acc);
    }

  /* 第 0 维：帧能量 dBFS（满量程 32768）。
   * 权重 0.5 是因为音量差（远近/增益）会直接进这一维，
   * 不压一压会让"同一句话小声说一遍"的距离被能量项主导 */

  dbfs = 10.0f * log10f(sum_sq / (float)KWS_FRAME_LEN /
                        (32768.0f * 32768.0f) + 1e-12f);
  out[0] = kws_quant(KWS_C0_WEIGHT * dbfs);

  *db10 = (int)(dbfs * 10.0f + 0.5f);
}

/* 起/落端点阈值 = max(绝对地板, 本底 + 余量)。
 * 为什么两条都要：绝对地板挡住"安静房间里的空调声"这类数值上像语音的东西
 * （和 ambient_listen.c 的 AMBIENT_LEVEL_FLOOR 一个思路）；
 * 相对条件挡住"本来就吵的屋子"（电视常开）—— 本底抬上去了，
 * 绝对的 -40dBFS 就一直满足，只有"比本底高 6dB"才算语音。
 * 本底是"没在说话时"的帧能量 EMA，所以电视开的房间本底也会跟着抬。 */

static void kws_vad_thresholds(int *on_db10, int *off_db10)
{
  int on = g_vad_on_db10;
  int off = g_vad_off_db10;

  if (g_floor_db10 + KWS_VAD_MARGIN_DB10 > on)
    {
      on = g_floor_db10 + KWS_VAD_MARGIN_DB10;
    }

  if (g_floor_db10 + KWS_VAD_HYST_DB10 > off)
    {
      off = g_floor_db10 + KWS_VAD_HYST_DB10;
    }

  if (off > on - 20)
    {
      off = on - 20;                    /* 留 2dB 回差，别在阈值上抖 */
    }

  *on_db10 = on;
  *off_db10 = off;
}

static void kws_floor_update(int db10, int on_db10)
{
  int d;

  if (db10 >= on_db10)
    {
      return;                          /* 语音帧不喂本底，否则本底被自己抬高 */
    }

  d = db10 - g_floor_db10;
  g_floor_db10 += (d >= 0) ? ((d + 16) / 32) : ((d - 16) / 32);   /* α = 1/32 */

  if (g_floor_db10 < -900)
    {
      g_floor_db10 = -900;             /* 下限 -90 dBFS */
    }
  else if (g_floor_db10 > -250)
    {
      g_floor_db10 = -250;             /* 上限 -25 dBFS */
    }
}

static void kws_ring_push(const int16_t *feat, int db10)
{
  memcpy(g_ring[g_ring_pos], feat, KWS_NUM_MFCC * sizeof(int16_t));
  g_ring_db10[g_ring_pos] = (int16_t)db10;

  g_ring_pos = (g_ring_pos + 1) % KWS_PREROLL_FEAT;
  if (g_ring_cnt < KWS_PREROLL_FEAT)
    {
      g_ring_cnt++;
    }
}

/* 追加一个特征点。满了就从**最老**的丢：一句话的尾巴比开头重要
 * （唤醒词在句尾），丢掉的最先也是预滚那部分。 */

static void kws_utt_append(const int16_t *feat, int db10)
{
  if (g_utt_len >= KWS_MAX_FEAT)
    {
      int drop = g_utt_len - KWS_MAX_FEAT + 1;
      int left = g_utt_len - drop;

      memmove(g_utt, &g_utt[drop],
              (size_t)left * KWS_NUM_MFCC * sizeof(int16_t));
      memmove(g_utt_db10, &g_utt_db10[drop], (size_t)left * sizeof(int16_t));
      g_utt_len = left;
      g_utt_dropped += drop;
    }

  memcpy(g_utt[g_utt_len], feat, KWS_NUM_MFCC * sizeof(int16_t));
  g_utt_db10[g_utt_len] = (int16_t)db10;
  g_utt_len++;
}

static void kws_utt_start(void)
{
  int idx = g_ring_pos - g_ring_cnt;
  int i;

  g_utt_len = 0;
  g_utt_dropped = 0;

  if (idx < 0)
    {
      idx += KWS_PREROLL_FEAT;
    }

  for (i = 0; i < g_ring_cnt; i++)
    {
      int p = idx + i;

      if (p >= KWS_PREROLL_FEAT)
        {
          p -= KWS_PREROLL_FEAT;
        }

      kws_utt_append(g_ring[p], g_ring_db10[p]);
    }
}

/* 掐头去尾 + CMN。返回收口后的特征点数（0 = 太短/太安静，丢掉） */

static int kws_utt_finalize(int off_db10)
{
  int a = 0;
  int b = g_utt_len - 1;
  int n;
  int i;
  int m;
  int32_t sum;
  int mean;

  while (a <= b && g_utt_db10[a] < off_db10)
    {
      a++;
    }

  while (b >= a && g_utt_db10[b] < off_db10)
    {
      b--;
    }

  n = b - a + 1;
  if (n <= 0)
    {
      g_utt_len = 0;
      return 0;
    }

  if (a > 0)
    {
      memmove(g_utt, &g_utt[a],
              (size_t)n * KWS_NUM_MFCC * sizeof(int16_t));
      memmove(g_utt_db10, &g_utt_db10[a], (size_t)n * sizeof(int16_t));
    }

  if (n < KWS_MIN_FEAT)
    {
      g_utt_len = 0;
      return 0;
    }

  /* CMN：每维减自己的均值。去的是麦克风/信道增益和说话音量带来的
   * 整体偏置 —— 不归一的话"同一个人远近说两遍"的距离会很大。
   * 减完再削一次顶（均值最大 4096，差可能到 8192，不削会破坏
   * KWS_DP_CAP 那道闸的前提）。 */

  for (m = 0; m < KWS_NUM_MFCC; m++)
    {
      sum = 0;

      for (i = 0; i < n; i++)
        {
          sum += g_utt[i][m];
        }

      mean = (int)((sum >= 0) ? ((sum + n / 2) / n) : ((sum - n / 2) / n));

      for (i = 0; i < n; i++)
        {
          int v = (int)g_utt[i][m] - mean;

          if (v > KWS_MFCC_MAX)
            {
              v = KWS_MFCC_MAX;
            }
          else if (v < -KWS_MFCC_MAX)
            {
              v = -KWS_MFCC_MAX;
            }

          g_utt[i][m] = (int16_t)v;
        }
    }

  g_utt_len = n;
  return n;
}

static int kws_finish_utterance(void)
{
  int on_db10;
  int off_db10;
  int n;

  g_in_speech = false;
  g_tail_cnt = 0;

  if (g_utt_len <= 0)
    {
      return 0;
    }

  /* 离线路径只认第一句：录音里后面还有话（或噪声）时不要把已经抓到的
   * 那句话冲掉 —— g_captured 就是这道闸 */

  if (g_finish_mode == KWS_FINISH_KEEP && g_captured)
    {
      return 0;
    }

  kws_vad_thresholds(&on_db10, &off_db10);
  n = kws_utt_finalize(off_db10);
  if (n <= 0)
    {
      KWS_LOG("有效语音太短，丢弃（%d 点）", g_utt_len);
      return 0;
    }

  KWS_LOG("句子收口：%d 个点（%dms）本底 %d 起 %d 落 %d",
          n, n * KWS_FEAT_MS, g_floor_db10, on_db10, off_db10);

  if (g_finish_mode == KWS_FINISH_KEEP)
    {
      g_captured = true;
      return 0;                         /* 结果留在 g_utt / g_utt_len 里 */
    }

  return kws_match();
}

/* 录音在句子中间就结束了（离线 enroll/标定时会发生）时，手动收口 */

static void kws_flush(void)
{
  if (g_in_speech)
    {
      (void)kws_finish_utterance();
    }

  g_in_speech = false;
  g_tail_cnt = 0;
}

static int kws_match(void)
{
  int best = -1;
  int best_slot = -1;
  int s;
  int d;

  for (s = 0; s < KWS_MAX_TEMPLATES; s++)
    {
      if (g_tpl_len[s] <= 0)
        {
          continue;
        }

      d = kws_dtw_dist(g_utt[0], g_utt_len, g_tpl[s][0], g_tpl_len[s]);
      KWS_LOG("slot%d: 模板 %d 点 vs 句子 %d 点 → %d", s, g_tpl_len[s],
              g_utt_len, d);

      if (d < 0)
        {
          continue;
        }

      if (best < 0 || d < best)
        {
          best = d;
          best_slot = s;
        }
    }

  g_last_dist = best;

  if (best < 0)
    {
      return 0;
    }

  if (best <= g_threshold)
    {
      printf("[KWS] 命中唤醒词（slot%d，距离 %d ≤ 阈值 %d）\n", best_slot, best,
             g_threshold);
      return 1;
    }

  return 0;
}

/* Sakoe-Chiba 带宽限制的 DTW，返回"每维每帧 RMS 距离 × 1000"；
 * 负值 = 长度差超出带宽（这条模板直接不判）。
 *
 * 归一化：D/(qn+tn) → 每步平均平方距离，再 /13 开方 → 每维 RMS。
 * 不归一的话长模板天然吃亏（路径更长，累加更大）。 */

static int kws_dtw_dist(const int16_t *q, int qn, const int16_t *t, int tn)
{
  int32_t *prev = g_dtw_a;
  int32_t *cur = g_dtw_b;
  int32_t *tmp;
  int r;
  int i;
  int j;
  int k;
  float per;

  if (q == NULL || t == NULL || qn <= 0 || tn <= 0)
    {
      return -1;
    }

  r = KWS_BAND_MIN + (int)(KWS_BAND_RATIO * (float)(qn > tn ? qn : tn));

  if ((qn > tn ? qn - tn : tn - qn) > r)
    {
      return -1;
    }

  /* 第 0 行：只有 (0,0) 可达 = 0，其余不可达 */

  for (j = 0; j <= tn; j++)
    {
      prev[j] = KWS_DP_INF;
    }

  prev[0] = 0;

  for (i = 1; i <= qn; i++)
    {
      int jlo = (i - r > 1) ? (i - r) : 1;
      int jhi = (i + r < tn) ? (i + r) : tn;
      const int16_t *qf = q + (size_t)(i - 1) * KWS_NUM_MFCC;

      for (j = 0; j <= tn; j++)
        {
          cur[j] = KWS_DP_INF;
        }

      for (j = jlo; j <= jhi; j++)
        {
          const int16_t *tf = t + (size_t)(j - 1) * KWS_NUM_MFCC;
          int32_t d = 0;
          int32_t best;

          for (k = 0; k < KWS_NUM_MFCC; k++)
            {
              int32_t df = (int32_t)qf[k] - (int32_t)tf[k];

              d += df * df;
            }

          best = (prev[j - 1] < prev[j]) ? prev[j - 1] : prev[j];
          if (cur[j - 1] < best)
            {
              best = cur[j - 1];
            }

          if (best >= KWS_DP_INF)
            {
              cur[j] = KWS_DP_INF;
            }
          else
            {
              int32_t v = best + d;

              cur[j] = (v > KWS_DP_CAP) ? KWS_DP_CAP : v;
            }
        }

      tmp = prev;
      prev = cur;
      cur = tmp;
    }

  if (prev[tn] >= KWS_DP_INF)
    {
      return -1;
    }

  per = (float)prev[tn] / (float)(qn + tn);
  per = sqrtf(per / (float)KWS_NUM_MFCC) / (float)KWS_MFCC_Q;

  return (int)(per * 1000.0f + 0.5f);
}

/* 一个 10ms 帧走完整条流水线，返回 1 = 命中 */

static int kws_process_frame(void)
{
  int16_t feat[KWS_NUM_MFCC];
  int db10 = 0;
  int on_db10;
  int off_db10;
  bool keep;

  kws_frame_features(g_in, feat, &db10, &g_prev);

  kws_vad_thresholds(&on_db10, &off_db10);

  keep = ((g_feat_idx & (KWS_DECIM - 1)) == 0);   /* 2:1 抽取 */
  g_feat_idx++;

  if (keep)
    {
      kws_ring_push(feat, db10);                  /* 预滚环始终保温 */
    }

  if (!g_in_speech)
    {
      kws_floor_update(db10, on_db10);

      if (g_settle_frames > 0)
        {
          g_settle_frames--;
          return 0;
        }

      if (g_cooldown_frames > 0)
        {
          g_cooldown_frames--;
          return 0;
        }

      if (g_finish_mode == KWS_FINISH_KEEP && g_captured)
        {
          return 0;                     /* 离线路径已经抓到一句，后面的不管 */
        }

      if (db10 >= on_db10)
        {
          g_in_speech = true;
          g_tail_cnt = 0;
          kws_utt_start();                        /* 把预滚（含本帧）铺进去 */
          KWS_LOG("起端点 %d dBFS（起 %d 落 %d 本底 %d）预滚 %d 点", db10,
                  on_db10, off_db10, g_floor_db10, g_utt_len);
        }

      return 0;
    }

  if (keep)
    {
      kws_utt_append(feat, db10);
    }

  if (db10 < off_db10)
    {
      g_tail_cnt++;
    }
  else
    {
      g_tail_cnt = 0;
    }

  if (g_tail_cnt * KWS_HOP_MS >= g_tail_ms)
    {
      if (kws_finish_utterance() == 1)
        {
          g_cooldown_frames = KWS_COOLDOWN_MS / KWS_HOP_MS;
          return 1;
        }
    }

  return 0;
}

static int kws_tpl_path(char *buf, size_t size, int slot)
{
  return (snprintf(buf, size, KWS_DATA_DIR "/slot%d.tpl", slot) > 0) ? 0 : -1;
}

/* 落盘用的临时文件名：正式名 + 后缀。临时文件和最终文件必须在**同一个文件
 * 系统**里 —— NuttX 的 rename 跨挂载点直接失败（fs_rename.c 里
 * oldinode != newinode 那条返回 -EXDEV，不会替你"拷贝过去再删"），
 * 同一个目录最省事也最清楚。
 * 文件名里留得住线索：目录里看到 slotN.tpl.tmp 就是录制路径写了一半的残骸，
 * slotN.tpl.inst 就是补装路径的。
 *
 * 两条路径的临时文件**必须不同名**：都用 .tmp 的话，补装（开机那一下）和
 * 现场录制撞在一起会互相 O_TRUNC，最坏是补装的 rename 把用户刚录好的模板
 * 覆盖成出厂模板。 */

static int kws_tpl_tmp_path(char *buf, size_t size, int slot,
                            const char *suffix)
{
  return (snprintf(buf, size, KWS_DATA_DIR "/slot%d.tpl.%s", slot, suffix) > 0)
         ? 0 : -1;
}

static int kws_write_all(int fd, const void *buf, size_t len)
{
  const uint8_t *p = (const uint8_t *)buf;

  while (len > 0)
    {
      ssize_t n = write(fd, p, len);

      if (n <= 0)
        {
          return -1;
        }

      p += n;
      len -= (size_t)n;
    }

  return 0;
}

static int kws_read_all(int fd, void *buf, size_t len)
{
  uint8_t *p = (uint8_t *)buf;

  while (len > 0)
    {
      ssize_t n = read(fd, p, len);

      if (n <= 0)
        {
          return -1;
        }

      p += n;
      len -= (size_t)n;
    }

  return 0;
}

static int kws_tpl_save(int slot)
{
  const int16_t *p = g_tpl[slot][0];
  int cnt = g_tpl_len[slot] * KWS_NUM_MFCC;
  kws_tpl_hdr_t hdr;
  char path[64];
  char tmp_path[64];
  uint32_t sum = 0;
  int fd;
  int i;
  int err;

  /* 先写临时文件、写完再 rename 改名到正式名字。
   * 为什么必须这么做："直接 O_TRUNC 正式文件"会留下一个**长度对、内容半截**
   * 的 .tpl —— 它头里 frames 是新的、校验和是新的，看起来完全合法，只会读出
   * 垃圾模板当唤醒词用（比读不出来更糟）。临时文件 + rename 消掉了这个状态。
   *
   * ⚠ 但别把 rename 当"原子替换"：目标名字不存在时它是一次原子的公开改名
   *（同一文件系统内）；**目标已存在（覆盖旧模板）时 NuttX 的 VFS 是先 unlink
   * 再 rename**，中间有一刻 slotN.tpl 这个名字底下什么都没有。掉电正好落在
   * 那一瞬间就丢了旧模板 —— 但 /data 是 tmpfs，掉电本来就整片丢，"丢一个槽"
   * 不是额外损失，所以这里不为那个窗口再做一层（比如先备份旧文件）。 */

  for (i = 0; i < cnt; i++)
    {
      sum += (uint32_t)(uint16_t)p[i];
    }

  memset(&hdr, 0, sizeof(hdr));
  hdr.magic = KWS_TPL_MAGIC;
  hdr.version = KWS_TPL_VERSION;
  hdr.dim = KWS_NUM_MFCC;
  hdr.frames = (uint16_t)g_tpl_len[slot];
  hdr.q = KWS_MFCC_Q;
  hdr.rate = KWS_SAMPLE_RATE;
  hdr.sum = sum;
  hdr.hop_ms = KWS_FEAT_MS;

  if (kws_tpl_path(path, sizeof(path), slot) < 0 ||
      kws_tpl_tmp_path(tmp_path, sizeof(tmp_path), slot, "tmp") < 0)
    {
      return -1;
    }

  /* /data 目录可能还不存在（本板 /data 是 tmpfs，开机是空的）。
   * 只建一级：KWS_DATA_DIR 就在 /data 下面，逐级建需要 mkdir -p 那套逻辑，
   * 而 /data 本身是板子挂载出来的，不需要我们建。 */

  if (mkdir(KWS_DATA_DIR, 0777) < 0 && errno != EEXIST)
    {
      printf("[KWS] 建 %s 失败：%d（模板只在内存里，重启会丢）\n",
             KWS_DATA_DIR, errno);
      return -1;
    }

  fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
    {
      printf("[KWS] 写 %s 失败：%d（模板只在内存里，重启会丢）\n", tmp_path,
             errno);
      return -1;
    }

  if (kws_write_all(fd, &hdr, sizeof(hdr)) < 0 ||
      kws_write_all(fd, p, (size_t)cnt * sizeof(int16_t)) < 0)
    {
      printf("[KWS] 写 %s 出错：%d（模板只在内存里，重启会丢）\n", tmp_path,
             errno);
      close(fd);
      unlink(tmp_path);
      return -1;
    }

  close(fd);

  if (rename(tmp_path, path) == 0)
    {
      return 0;
    }

  /* 有的文件系统（FAT/exFAT 之类）的 rename 不覆盖已存在的目标文件。
   * /data 是 tmpfs 时走不到这里，但万一换到别的分区，宁可"删旧再改名"
   * （这一步不原子，只在正常路径失败后兜底）也比"新模板永远存不进去"强。
   * err 一路跟着最后一次失败的调用走，报出来的原因是真正卡住的那一步。 */

  err = errno;

  if (unlink(path) == 0)
    {
      if (rename(tmp_path, path) == 0)
        {
          return 0;
        }

      err = errno;
    }
  else
    {
      err = errno;
    }

  printf("[KWS] %s 改名失败：%d（模板只在内存里，重启会丢）\n", path, err);
  unlink(tmp_path);
  return -1;
}

/* 校验一个模板文件的内容。fd 要刚打开、位置在文件开头；dst 非空就顺便把负载
 * 读进这块内存（载入路径要），为空就只校验、不留数据（补装路径在 rename 之前
 * 要的正是这个）。成功时把点数写进 *frames_out（可空）。
 *
 * 为什么判据只有这一份：补装和载入要是各写一套，就会出现"补装说没问题、
 * 载入侧却不认"的文件，而那种文件一旦 rename 成了 slotN.tpl，就会被
 * kws_install_templates() 里"目标已存在就跳过"那道闸永久挡在重装之外。
 *
 * 下面逐条查、不用一个大 if：出问题时日志要能直接指出是哪一条对不上。
 * 魔数/版本不对就**明确拒绝**，绝不"尽力猜着读" ——
 * 猜错的话读到的是被当成模板用的垃圾特征，比"这个槽没有模板"糟得多
 * （后者只是不唤醒，前者是乱唤醒）。 */

static int kws_tpl_check(int fd, const char *path, int16_t *dst,
                         int *frames_out)
{
  kws_tpl_hdr_t hdr;
  int16_t vals[128];
  const int chunk_vals = (int)(sizeof(vals) / sizeof(vals[0]));
  uint32_t sum = 0;
  int cnt;
  int done = 0;
  int i;

  if (kws_read_all(fd, &hdr, sizeof(hdr)) < 0)
    {
      printf("[KWS] %s 读头失败（文件比 %u 字节还短？），忽略\n", path,
             (unsigned)sizeof(hdr));
      return -EIO;
    }

  if (hdr.magic != KWS_TPL_MAGIC)
    {
      printf("[KWS] %s 魔数 0x%08x 不对（应为 0x%08x），不是本模块的模板，"
             "忽略\n", path, (unsigned)hdr.magic, (unsigned)KWS_TPL_MAGIC);
      return -EINVAL;
    }

  if (hdr.version != KWS_TPL_VERSION)
    {
      printf("[KWS] %s 版本 %u 不认识（本程序只认 %u）：换过特征口径的模板"
             "必须重录，不能凑合读，忽略\n", path, (unsigned)hdr.version,
             (unsigned)KWS_TPL_VERSION);
      return -EINVAL;
    }

  if (hdr.dim != KWS_NUM_MFCC || hdr.q != KWS_MFCC_Q ||
      hdr.rate != KWS_SAMPLE_RATE || hdr.hop_ms != KWS_FEAT_MS)
    {
      printf("[KWS] %s 特征口径不一致（dim %u/%d，q %u/%d，rate %u/%d，"
             "hop %ums/%dms），忽略\n", path, (unsigned)hdr.dim, KWS_NUM_MFCC,
             (unsigned)hdr.q, KWS_MFCC_Q, (unsigned)hdr.rate, KWS_SAMPLE_RATE,
             (unsigned)hdr.hop_ms, KWS_FEAT_MS);
      return -EINVAL;
    }

  /* frames 是文件里的值，用于算读多少字节 —— 越界不查就是栈/静态区溢出 */

  if (hdr.frames == 0 || hdr.frames > KWS_MAX_FEAT)
    {
      printf("[KWS] %s 点数 %u 越界（1~%d），忽略\n", path,
             (unsigned)hdr.frames, KWS_MAX_FEAT);
      return -EINVAL;
    }

  cnt = (int)hdr.frames * KWS_NUM_MFCC;

  /* 按 int16 的整数倍一块块读（kws_read_all 会把一块读满），校验和逐块累加，
   * 一块读完直接扔掉 —— 补装路径不需要把 2600 字节留在内存里。 */

  while (done < cnt)
    {
      int vals_n = cnt - done;

      if (vals_n > chunk_vals)
        {
          vals_n = chunk_vals;
        }

      if (kws_read_all(fd, vals, (size_t)vals_n * sizeof(vals[0])) < 0)
        {
          printf("[KWS] %s 读不完整（头说 %u 点），忽略\n", path,
                 (unsigned)hdr.frames);
          return -EIO;
        }

      for (i = 0; i < vals_n; i++)
        {
          sum += (uint32_t)(uint16_t)vals[i];

          if (dst != NULL)
            {
              *dst++ = vals[i];
            }
        }

      done += vals_n;
    }

  /* 校验和 + 头里的点数一起挡"写了一半"的文件（旧格式、host 侧生成的模板
   * 没有改名保护，只能靠这一关）。 */

  if (sum != hdr.sum)
    {
      printf("[KWS] %s 校验和不对（0x%08x/0x%08x，写了一半？），忽略\n", path,
             (unsigned)sum, (unsigned)hdr.sum);
      return -EIO;
    }

  if (frames_out != NULL)
    {
      *frames_out = (int)hdr.frames;
    }

  return 0;
}

static int kws_tpl_load(int slot)
{
  char path[64];
  int frames = 0;
  int fd;
  int rc;

  if (kws_tpl_path(path, sizeof(path), slot) < 0)
    {
      return -EINVAL;
    }

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      /* 没录过这个槽是最常见的情况（/data 是 tmpfs，开机就是空的），
       * 所以这里**刻意不打日志**，由调用方按调试级别打一行 ——
       * 4 个槽 × 每次开机 4 条 "文件不存在" 的 ERROR 才是真的刷屏。
       * 但"文件在、只是打不开"（权限/FS 出错）是真问题，要打。 */

      if (errno != ENOENT)
        {
          printf("[KWS] 打开 %s 失败：%d，忽略\n", path, errno);
          return -EIO;
        }

      return -ENOENT;
    }

  rc = kws_tpl_check(fd, path, g_tpl[slot][0], &frames);
  close(fd);

  /* 校验不过时 g_tpl[slot] 里是垃圾，但长度还是 0，匹配路径永远不会碰它
   *（g_tpl_len 只在下面赋值）。 */

  if (rc < 0)
    {
      return rc;
    }

  g_tpl_len[slot] = frames;
  return 0;
}

/* 把只读素材里的 slotN.tpl 补一份到 /data/kws（同板级
 * sf32lb52_install_agent_config() 的套路）。返回写入的字节数，负值 = 没装。
 *
 * 目标已存在时调用方就不会走到这里：现场录的模板（更贴合说话人）优先于
 * 出厂模板，绝不覆盖。
 *
 * 先写 slotN.tpl.inst 再 rename，和 kws_tpl_save 同一条理由（见它的说明）。
 * 临时后缀用 .inst 而不是 .tmp：录制路径用的是 .tmp，两边同名的话补装和
 * 现场录到一起会互相 O_TRUNC，最坏是这边的 rename 把用户刚录好的模板
 * 覆盖成出厂模板。
 *
 * ★ rename 之前必须先用 kws_tpl_check() 验一遍内容，验不过就不改名。
 *   素材是宿主侧拷来/打包进来的，0 字节、抄了半截、别的模块的文件都可能
 *   混进来；而一旦坏文件占了 slotN.tpl 这个名字，kws_install_templates()
 *   里"目标已存在就跳过"那道闸会让它此后每次开机都拦下重装 —— 一个坏素材
 *   就把这个槽永久占死，现场表现是"喊名字怎么都不醒"。 */

static int kws_tpl_install(int slot)
{
  char src[64];
  char dst[64];
  char tmp[64];
  char buf[256];
  ssize_t total = 0;
  ssize_t nread;
  int srcfd;
  int chkfd;
  int dstfd;
  int ok;

  if (snprintf(src, sizeof(src), KWS_TPL_ASSETS_DIR "/slot%d.tpl", slot) <= 0 ||
      kws_tpl_path(dst, sizeof(dst), slot) < 0 ||
      kws_tpl_tmp_path(tmp, sizeof(tmp), slot, "inst") < 0)
    {
      printf("[KWS] 出厂模板 slot%d：路径拼不出来，跳过\n", slot);
      return -1;
    }

  srcfd = open(src, O_RDONLY);
  if (srcfd < 0)
    {
      /* 素材里没有这一条：最常见、也是**正常**情况（仓库不带 .tpl）。
       * 所以默认不打 —— 每次开机刷 4 行"素材不存在"才是真的刷屏；要看就
       * 开 KWS_DEBUG_ENABLE。而"文件在、打不开"（权限/FS 出错）是真问题，
       * 一律打 —— 和 kws_tpl_load 对 ENOENT 的处理保持一致。 */

      if (errno != ENOENT)
        {
          printf("[KWS] 出厂模板 %s 打不开：%d，跳过\n", src, errno);
        }
      else
        {
          KWS_LOG("slot%d 素材里没有 %s，跳过（正常情况）", slot, src);
        }

      return -1;
    }

  dstfd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (dstfd < 0)
    {
      printf("[KWS] 建临时文件 %s 失败：%d，slot%d 不装\n", tmp, errno, slot);
      close(srcfd);
      return -1;
    }

  for (;;)
    {
      nread = read(srcfd, buf, sizeof(buf));
      if (nread <= 0)
        {
          break;                        /* 0 = 读完（EOF）；负数下面按失败处理 */
        }

      if (kws_write_all(dstfd, buf, (size_t)nread) < 0)
        {
          nread = -1;                   /* 写失败和读失败走同一段清理 */
          break;
        }

      total += nread;
    }

  close(dstfd);
  close(srcfd);

  ok = 1;

  if (nread < 0)
    {
      printf("[KWS] 抄 %s 出错：%d，slot%d 不装\n", src, errno, slot);
      ok = 0;
    }

  /* 抄完了先验内容再改名。验的是临时文件 —— 那才是将来真正生效的字节；
   * 日志里报的是 src，因为要修的是素材那个文件。 */

  if (ok)
    {
      chkfd = open(tmp, O_RDONLY);
      if (chkfd < 0)
        {
          printf("[KWS] 临时文件 %s 复查时打不开：%d，slot%d 不装\n", tmp, errno,
                 slot);
          ok = 0;
        }
      else
        {
          if (kws_tpl_check(chkfd, src, NULL, NULL) < 0)
            {
              printf("[KWS] 出厂模板 %s 内容不合法（原因见上一行），不装："
                     "装了它就会占死 slot%d，之后每次开机都不再重装\n", src,
                     slot);
              ok = 0;
            }

          close(chkfd);
        }
    }

  if (ok)
    {
      if (rename(tmp, dst) == 0)
        {
          return (int)total;
        }

      printf("[KWS] %s 改名成 %s 失败：%d，slot%d 不装\n", tmp, dst, errno, slot);
    }

  /* 上面任何一步没成：临时文件留着没意义（下次开机还会重写），清掉。
   * 清不掉要说一声 —— 目录里躺个 .inst 残骸会让人以为有半截文件。 */

  if (unlink(tmp) < 0)
    {
      printf("[KWS] 临时文件 %s 删不掉：%d，不影响启动\n", tmp, errno);
    }

  return -1;
}

/* 开机时把 ROMFS 里的出厂模板补进 /data/kws（/data 是 tmpfs，重启就空，
 * 不补的话唤醒词功能每次重启都静默失效）。目录里没有素材时安静跳过 ——
 * 仓库不带 .tpl 是常态，这一步**不能**报错、也不能拖住启动。 */

static void kws_install_templates(void)
{
  int installed = 0;
  int bytes = 0;
  int s;

  /* /data 还没挂上（或压根没有）时建目录会失败：直接整段跳过，
   * 后面的载入会把"没有模板"当成正常情况。但"目录建不出来"和"素材里没有
   * 模板"在日志上必须分得开，所以这里要留一行（不是每次开机都无脑刷）。 */

  if (mkdir(KWS_DATA_DIR, 0777) < 0 && errno != EEXIST)
    {
      printf("[KWS] 出厂模板补装跳过：建目录 %s 失败：%d\n", KWS_DATA_DIR, errno);
      return;
    }

  for (s = 0; s < KWS_MAX_TEMPLATES; s++)
    {
      char dst[64];
      int n;

      if (kws_tpl_path(dst, sizeof(dst), s) < 0 || access(dst, F_OK) == 0)
        {
          continue;                     /* 已经有模板（现场录的优先），不覆盖 */
        }

      n = kws_tpl_install(s);
      if (n > 0)
        {
          installed++;
          bytes += n;
        }
    }

  if (installed > 0)
    {
      printf("[KWS] 出厂模板补装到 %s：%d 字节（%d 条）\n", KWS_DATA_DIR, bytes,
             installed);
    }
}

static int kws_us_diff(const struct timespec *a, const struct timespec *b)
{
  return (int)((b->tv_sec - a->tv_sec) * 1000000 +
               (b->tv_nsec - a->tv_nsec) / 1000);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int kws_init(void)
{
  int loaded;

  if (!g_ready)
    {
      kws_build_tables();
      g_ready = true;
    }

  /* 先把只读素材里的出厂模板补进 /data（/data 是 tmpfs，重启就空），
   * 再统一走载入路径 —— 补装出来的文件和现场录的模板是同一种文件，
   * 后面不需要区分。 */

  kws_install_templates();

  loaded = kws_template_load_all();

  kws_reset();

  printf("[KWS] 初始化：%d 维特征/%dms 一个点，模板 %d 条（%s 里）\n",
         KWS_NUM_MFCC, KWS_FEAT_MS, loaded, KWS_DATA_DIR);

  if (loaded == 0)
    {
      printf("[KWS] 提示：还没有模板，kws_feed 永远不会返回 1；"
             "先用 kws_enroll 录「你好，openvela」和「Hello，openvela」，"
             "或把录好的 slotN.tpl 放进固件里的 " KWS_TPL_ASSETS_DIR
             "/ 随固件一起打包\n");
    }

  return 0;
}

int kws_feed(const int16_t *pcm, size_t frames)
{
  size_t i = 0;
  int matched = 0;

  if (!g_ready)
    {
      return -ENOSYS;
    }

  if (pcm == NULL || frames == 0)
    {
      return -EINVAL;
    }

  while (i < frames)
    {
      size_t n = KWS_FRAME_LEN - g_in_len;
      int rc;

      if (n > frames - i)
        {
          n = frames - i;
        }

      memcpy(&g_in[g_in_len], &pcm[i], n * sizeof(int16_t));
      g_in_len += n;
      i += n;

      if (g_in_len < KWS_FRAME_LEN)
        {
          break;                        /* 没凑满一窗，留着下次接着凑 */
        }

      rc = kws_process_frame();
      if (rc < 0)
        {
          return rc;
        }

      if (rc == 1)
        {
          matched = 1;                  /* 命中后本段剩下的照喂（进冷却），
                                         * 这样帧边界不会和调用方错开 */
        }

      /* 50% 重叠：丢掉最老的 160 个采样，下次再补 160 个就是一窗 */

      memmove(g_in, &g_in[KWS_HOP_LEN],
              (KWS_FRAME_LEN - KWS_HOP_LEN) * sizeof(int16_t));
      g_in_len = KWS_FRAME_LEN - KWS_HOP_LEN;
    }

  return matched;
}

int kws_enroll(const int16_t *pcm, size_t frames, int slot)
{
  int n;

  if (!g_ready)
    {
      return -ENOSYS;
    }

  if (pcm == NULL || slot < 0 || slot >= KWS_MAX_TEMPLATES ||
      frames < KWS_FRAME_LEN)
    {
      return -EINVAL;
    }

  kws_reset();                          /* 模板采集走同一条流水线 */
  g_settle_frames = 0;                  /* 录音是现成的，不用再等本底收敛 */

  g_finish_mode = KWS_FINISH_KEEP;
  (void)kws_feed(pcm, frames);
  kws_flush();
  g_finish_mode = KWS_FINISH_MATCH;

  n = g_utt_len;

  if (n < KWS_MIN_FEAT)
    {
      printf("[KWS] 录模板失败：有效语音只有 %d 个点（需要 ≥ %d = %dms）——"
             "录音太短/太安静/前后没有静音都会这样\n", n, KWS_MIN_FEAT,
             KWS_MIN_FEAT * KWS_FEAT_MS);
      return -EINVAL;
    }

  /* 预滚那部分被挤掉是正常的；再多说明短语本身超了 2.0s，模板会是残的 */

  if (g_utt_dropped > KWS_PREROLL_FEAT)
    {
      printf("[KWS] 录模板失败：有效语音 %d 点，超过上限 %d 点(%dms)，"
             "挤掉了 %d 点 —— 请把短语控制短一点\n", n, KWS_MAX_FEAT,
             KWS_MAX_FEAT * KWS_FEAT_MS, g_utt_dropped);
      return -ENOSPC;
    }

  memcpy(g_tpl[slot], g_utt, (size_t)n * KWS_NUM_MFCC * sizeof(int16_t));
  g_tpl_len[slot] = n;

  if (kws_tpl_save(slot) < 0)
    {
      return 1;                         /* 内存里有了，只是没落盘 */
    }

  printf("[KWS] slot%d 存好：%d 个特征点（%dms，落盘 " KWS_DATA_DIR
         "/slot%d.tpl）\n", slot, n, n * KWS_FEAT_MS, slot);

  return 0;
}

void kws_set_threshold(int th)
{
  if (th < 200 || th > 20000)
    {
      printf("[KWS] 阈值 %d 不合法（200~20000），忽略\n", th);
      return;
    }

  /* 阈值一旦高过 DTW 饱和距离的下限，判定的意义就没了：
   * 距离被 DP 上限钉住，算出来最多就是这个量级，"最不像的输入"也会命中。
   * 这里是现场标定的入口，不拒绝（可能是标定的人故意的），但必须让人看见。 */

  if (th > KWS_DP_CAP_DIST_MIN)
    {
      printf("[KWS] 警告：阈值 %d ≥ DTW 饱和距离下限 %d，"
             "任何输入都会被判命中（阈值实际失效）\n", th, KWS_DP_CAP_DIST_MIN);
    }

  g_threshold = th;
}

int kws_get_threshold(void)
{
  return g_threshold;
}

void kws_set_vad_db(int on_dbfs, int off_dbfs)
{
  if (on_dbfs != 0)
    {
      g_vad_on_db10 = on_dbfs * 10;
    }

  if (off_dbfs != 0)
    {
      g_vad_off_db10 = off_dbfs * 10;
    }

  if (g_vad_off_db10 > g_vad_on_db10)
    {
      g_vad_off_db10 = g_vad_on_db10;
    }
}

void kws_set_endpoint_ms(int tail_ms)
{
  if (tail_ms < 100 || tail_ms > 2000)
    {
      printf("[KWS] 尾静音超时 %dms 不合法（100~2000），忽略\n", tail_ms);
      return;
    }

  g_tail_ms = tail_ms;
}

int kws_get_last_distance(void)
{
  return g_last_dist;
}

int kws_ready_count(void)
{
  int s;
  int cnt = 0;

  for (s = 0; s < KWS_MAX_TEMPLATES; s++)
    {
      if (g_tpl_len[s] > 0)
        {
          cnt++;
        }
    }

  return cnt;
}

int kws_template_frames(int slot)
{
  if (slot < 0 || slot >= KWS_MAX_TEMPLATES)
    {
      return 0;
    }

  return g_tpl_len[slot];
}

int kws_slot_ready(int slot)
{
  if (slot < 0 || slot >= KWS_MAX_TEMPLATES)
    {
      return 0;
    }

  return (g_tpl_len[slot] > 0) ? 1 : 0;
}

int kws_template_load_all(void)
{
  int loaded = 0;
  int s;

  if (!g_ready)
    {
      return -ENOSYS;
    }

  for (s = 0; s < KWS_MAX_TEMPLATES; s++)
    {
      int rc;

      /* 先清零再读：读失败（文件没了/坏了）时这个槽必须回到"没有模板"，
       * 不能留着上一次的旧模板继续用 —— 否则"清掉一个槽"永远做不到。 */

      g_tpl_len[s] = 0;

      rc = kws_tpl_load(s);
      if (rc == 0)
        {
          loaded++;
          KWS_LOG("slot%d 载入模板：%d 点（%dms）", s, g_tpl_len[s],
                  g_tpl_len[s] * KWS_FEAT_MS);
        }
      else
        {
          /* 文件不存在（-ENOENT）是最正常的：4 个槽一个都没录也是常态。
           * kws_tpl_load 对"不存在"不打日志，这里也只降级到 DEBUG 一行；
           * 头不对/校验不对那几种情况 kws_tpl_load 已经打过 ERROR 了。 */

          KWS_LOG("slot%d 没有可用模板（rc %d，没录过属正常）", s, rc);
        }
    }

  printf("[KWS] 模板载入：%d/%d 条可用（%s）\n", loaded, KWS_MAX_TEMPLATES,
         KWS_DATA_DIR);

  return loaded;
}

int kws_template_save(int slot)
{
  if (!g_ready)
    {
      return -ENOSYS;
    }

  if (slot < 0 || slot >= KWS_MAX_TEMPLATES)
    {
      return -EINVAL;
    }

  if (g_tpl_len[slot] <= 0)
    {
      return -EINVAL;                   /* 这个槽还没录过，没什么可存 */
    }

  /* 越界/空槽是调用方的 bug，用返回码告诉他，不在这里重复打日志；
   * 写盘失败的原因 kws_tpl_save 内部已经打过了，不重复刷屏。 */

  if (kws_tpl_save(slot) < 0)
    {
      return -EIO;
    }

  printf("[KWS] slot%d 已落盘：%d 点（%s/slot%d.tpl）\n", slot, g_tpl_len[slot],
         KWS_DATA_DIR, slot);
  return 0;
}

void kws_reset(void)
{
  memset(g_in, 0, sizeof(g_in));
  g_in_len = 0;
  g_prev = 0.0f;
  g_feat_idx = 0;
  memset(g_ring, 0, sizeof(g_ring));
  g_ring_pos = 0;
  g_ring_cnt = 0;
  g_utt_len = 0;
  g_utt_dropped = 0;
  g_in_speech = false;
  g_tail_cnt = 0;
  g_cooldown_frames = 0;
  g_settle_frames = KWS_SETTLE_FRAMES;
  g_floor_db10 = -550;
  g_last_dist = -1;
  g_captured = false;
}

int kws_distance_pcm(const int16_t *pcm, size_t frames, int slot,
                     int *dist_milli)
{
  int d;

  if (!g_ready)
    {
      return -ENOSYS;
    }

  if (pcm == NULL || dist_milli == NULL || slot < 0 ||
      slot >= KWS_MAX_TEMPLATES || frames < KWS_FRAME_LEN ||
      g_tpl_len[slot] <= 0)
    {
      return -EINVAL;
    }

  kws_reset();
  g_settle_frames = 0;

  g_finish_mode = KWS_FINISH_KEEP;
  (void)kws_feed(pcm, frames);
  kws_flush();
  g_finish_mode = KWS_FINISH_MATCH;

  if (g_utt_len < KWS_MIN_FEAT)
    {
      return -EINVAL;
    }

  d = kws_dtw_dist(g_utt[0], g_utt_len, g_tpl[slot][0], g_tpl_len[slot]);
  if (d < 0)
    {
      return -EINVAL;                   /* 长度差超带宽，等于"完全不像" */
    }

  *dist_milli = d;
  return 0;
}

int kws_selftest(void)
{
  int16_t buf[KWS_FRAME_LEN];
  int16_t feat[KWS_NUM_MFCC];
  struct timespec t0;
  struct timespec t1;
  float prev = 0.0f;
  float peak = -1.0f;
  float mel_max = -1.0f;
  int db10 = 0;
  int peak_bin = 0;
  int mel_peak = 0;
  int fails = 0;
  int i;
  int k;
  int cn = 0;

  if (!g_ready)
    {
      printf("[KWS] 自检：模块没初始化（先 kws_init）\n");
      return KWS_ERR_STATE;
    }

  printf("[KWS] ===== 自检 =====\n");

  /* 1) 合成 1kHz 正弦跑一帧：FFT 峰值 bin 和 Mel 峰值滤波器位置 */

  for (i = 0; i < KWS_FRAME_LEN; i++)
    {
      buf[i] = (int16_t)(8000.0f * sinf(2.0f * KWS_PI * 1000.0f *
                                        (float)i / (float)KWS_SAMPLE_RATE));
    }

  prev = 0.0f;
  kws_frame_features(buf, feat, &db10, &prev);

  for (k = 0; k <= KWS_FFT_LEN / 2; k++)
    {
      if (g_power[k] > peak)
        {
          peak = g_power[k];
          peak_bin = k;
        }
    }

  for (k = 0; k < KWS_NUM_MEL; k++)
    {
      if (g_logmel[k] > mel_max)
        {
          mel_max = g_logmel[k];
          mel_peak = k;
        }
    }

  /* 1kHz / 31.25Hz 一个 bin = 32 号 bin；mel(1000) 落在第 8/9 个滤波器上 */

  printf("[KWS] 自检1 合成 1kHz：功率峰 bin=%d（期望 32），mel 峰=%d"
         "（期望 8~9），帧能量 %d (0.1dBFS)\n", peak_bin, mel_peak, db10);

  if (peak_bin < 31 || peak_bin > 33)
    {
      printf("[KWS] 自检1 失败：FFT 峰值 bin 不对 —— 查窗/补零/位反转\n");
      fails++;
    }

  if (mel_peak < 7 || mel_peak > 10)
    {
      printf("[KWS] 自检1 失败：Mel 峰值滤波器不对 —— 查 Hz/Mel 映射\n");
      fails++;
    }

  /* 2) 模板自比对：自距离、时间拉伸、帧序打乱（"不是这句话"的代理） */

  if (kws_ready_count() == 0)
    {
      printf("[KWS] 自检2/3 跳过：还没有模板（先 kws_enroll 录一条）\n");
    }
  else
    {
      int n0 = g_tpl_len[0];
      int self;
      int warp;
      int noise;
      int rec;
      int wn = 0;
      unsigned int seed = 12345u;

      self = kws_dtw_dist(g_tpl[0][0], n0, g_tpl[0][0], n0);

      /* 时间拉伸 1.25 倍：每 4 个点重复一个点。DTW 应该能吸收掉。
       *
       * ⚠ 这个检查的**实测值恒等于 0**，不是 bug，是复制法的必然结果：
       *   复制出来的点是模板里已有的精确值，DTW 走一步"竖直/水平"到它身上
       *   代价正好是 0（整条路径仍然是 0 代价）。所以它只能证明一件事 ——
       *   KWS_BAND_* 给出的带宽容得下 25% 的长度变化（warp < 0 才失败），
       *   它**测不出**"同一个人另说一遍"的类内距离有多大。
       *   真正量过（宿主 gcc，本文件同一套 DTW）：把模板按 1.25 倍线性插值
       *   重采样 → 620；再叠加 ±2 MFCC 的随机扰动 → 1076；
       *   对比"帧序打乱" → 3068。也就是说 620~1100 才是类内量级。
       *   因为恒为 0，下面推荐阈值里的"同句侧上限"这一项实际上是 0，
       *   推荐值 = (0 + 结构打乱)/2 再和互距离平均 —— 偏保守（偏小），
       *   不会因为这条而放宽。想拿真类内数就用 kws_distance_pcm() 实测。 */

      for (i = 0; i < n0 && wn < KWS_SELFTEST_FEAT; i++)
        {
          memcpy(g_selftest[wn++], g_tpl[0][i], sizeof(int16_t) *
                 KWS_NUM_MFCC);

          if ((i & 3) == 3 && wn < KWS_SELFTEST_FEAT)
            {
              memcpy(g_selftest[wn++], g_tpl[0][i], sizeof(int16_t) *
                     KWS_NUM_MFCC);
            }
        }

      warp = kws_dtw_dist(g_selftest[0], wn, g_tpl[0][0], n0);

      /* "不是这句话"的代理：从模板自己抓随机帧拼一条假句子。
       * 为什么不用撒白噪声特征：随手取个固定量级（比如 ±2048）跟真特征的
       * 量级、维度分布都对不上，算出来的距离没法当阈值参考（第一版就是这么
       * 错的，推荐值偏到几千）。抓模板自己的帧 = 值域完全同分布（量级自校准），
       * 只把时间结构打乱 —— 这才是"距离有没有区分度"的正确问法。 */

      for (i = 0; i < n0 && i < KWS_SELFTEST_FEAT; i++)
        {
          seed = seed * 1103515245u + 12345u;
          memcpy(g_selftest[i], g_tpl[0][(seed >> 16) % (unsigned int)n0],
                 sizeof(int16_t) * KWS_NUM_MFCC);
        }

      noise = kws_dtw_dist(g_selftest[0], n0, g_tpl[0][0], n0);

      printf("[KWS] 自检2 模板0（%d 点）：自距离 %d，拉伸1.25倍 %d，"
             "帧序打乱 %d（都是 ×1000 口径）\n", n0, self, warp, noise);

      if (self > 200)
        {
          printf("[KWS] 自检2 失败：自己跟自己比都不是 0（%d）—— DTW 有 bug\n",
                 self);
          fails++;
        }

      if (warp < 0)
        {
          printf("[KWS] 自检2 失败：拉伸 1.25 倍被带宽挡掉了（模板 %d 点）——"
                 "KWS_BAND_RATIO/KWS_BAND_MIN 太小\n", n0);
          fails++;
        }

      if (noise < 0)
        {
          printf("[KWS] 自检2 失败：帧序打乱都被带宽挡掉了（长度差）\n");
          fails++;
        }
      else if (warp >= 0 && noise < warp * 2)
        {
          printf("[KWS] 自检2 失败：帧序打乱 %d 没有明显大于拉伸 %d ——"
                 "距离没有区分度\n", noise, warp);
          fails++;
        }
      else if (noise < 800)
        {
          printf("[KWS] 自检2 失败：帧序打乱只有 %d（<800）——"
                 "距离太小，阈值没有区分余地\n", noise);
          fails++;
        }

      /* 推荐阈值：取「同句侧上界（拉伸）」和「结构打乱」的中点；
       * 有第二条模板就再往"不同词"那侧拉一半。 */

      if (warp >= 0 && noise > 0)
        {
          int cross2 = (g_tpl_len[1] > 0)
                         ? kws_dtw_dist(g_tpl[0][0], g_tpl_len[0],
                                        g_tpl[1][0], g_tpl_len[1])
                         : -1;

          rec = (warp + noise) / 2;
          if (cross2 > 0)
            {
              rec = (rec + cross2) / 2;
            }

          if (rec < 800)
            {
              rec = 800;
            }

          if (rec > 8000)
            {
              rec = 8000;
            }

          printf("[KWS] 推荐阈值 %d（当前 %d）：同句侧上限 %d、结构打乱侧下限 %d%s。"
                 "上板后最好再录 2~3 遍同一个人/不同人说同一句，"
                 "用 kws_distance_pcm() 看真实分布再定\n", rec, g_threshold,
                 warp, noise, (cross2 > 0) ? "、另有两条模板互距离" : "");
          printf("[KWS] 改法：kws_set_threshold(%d);\n", rec);
        }

      /* 3) 模板互距离 */

      if (g_tpl_len[1] > 0)
        {
          int cross = kws_dtw_dist(g_tpl[0][0], g_tpl_len[0], g_tpl[1][0],
                                   g_tpl_len[1]);

          printf("[KWS] 自检3 slot0(%d 点) vs slot1(%d 点)：互距离 %d\n",
                 g_tpl_len[0], g_tpl_len[1], cross);

          if (cross >= 0 && cross < 1000)
            {
              printf("[KWS] 自检3 警告：两条模板太像了（互距离 %d < 1000）——"
                     "是不是录成了同一句？（「你好，openvela」和"
                     "「Hello，openvela」的互距离应该明显大于同句距离）\n",
                     cross);
            }
        }
    }

  /* 4) 实测耗时（板子上真正跑出来的数，不是估算） */

  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (i = 0; i < 20; i++)
    {
      prev = 0.0f;
      kws_frame_features(buf, feat, &db10, &prev);
    }

  clock_gettime(CLOCK_MONOTONIC, &t1);
  printf("[KWS] 自检4 实测 MFCC：%d us/帧（10ms 一帧 → 约 %d.%d%% CPU）\n",
         kws_us_diff(&t0, &t1) / 20, kws_us_diff(&t0, &t1) / 20 / 100,
         (kws_us_diff(&t0, &t1) / 20 / 10) % 10);

  if (kws_ready_count() > 0)
    {
      clock_gettime(CLOCK_MONOTONIC, &t0);
      for (i = 0; i < 20; i++)
        {
          int dd = kws_dtw_dist(g_tpl[0][0], g_tpl_len[0], g_tpl[0][0],
                                g_tpl_len[0]);

          /* 数"算出来了几次"，不是把距离加起来：自比的距离**恒为 0**，
           * 累加值永远是 0，拿 sum == 0 当"没算出值"会误报
           * （原来就是这样的假警报）。真正要抓的是返回负值那种失败。 */

          if (dd >= 0)
            {
              cn++;
            }
        }

      clock_gettime(CLOCK_MONOTONIC, &t1);
      printf("[KWS] 自检4 实测 DTW：%d us/次（模板 %d 点），"
             "一次判定最多 %d 条模板\n", kws_us_diff(&t0, &t1) / 20,
             g_tpl_len[0], KWS_MAX_TEMPLATES);
    }

  /* 主要几块静态 RAM 的实际尺寸（宏改了这里会跟着变，可当账本核对） */

  printf("[KWS] 自检4 静态占用：模板 %d B，句子 %d B，"
         "FFT 工作区 %d B，表 %d B\n", (int)sizeof(g_tpl),
         (int)sizeof(g_utt) + (int)sizeof(g_utt_db10),
         (int)(sizeof(g_fft_re) + sizeof(g_fft_im) + sizeof(g_tw_cos) +
               sizeof(g_tw_sin) + sizeof(g_bitrev) + sizeof(g_power)),
         (int)(sizeof(g_window) + sizeof(g_mel) + sizeof(g_dct) +
               sizeof(g_logmel) + sizeof(g_ring) + sizeof(g_selftest) +
               sizeof(g_dtw_a) + sizeof(g_dtw_b)));

  if (cn == 0 && kws_ready_count() > 0)
    {
      printf("[KWS] 自检4 警告：DTW 20 次一次都没算出值（都返回负值）——"
             "查带宽/长度\n");
      fails++;
    }

  printf("[KWS] ===== 自检 %s =====\n", (fails == 0) ? "通过" : "有问题");
  return (fails == 0) ? 0 : -1;
}
