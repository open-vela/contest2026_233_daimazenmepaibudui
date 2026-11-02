/****************************************************************************
 * apps/hello_app/sound_event.c
 *
 * 本地声音事件识别：mel 前端 + DSCNN-lite + 投票后处理，纯 C / 零依赖。
 * 刻意不用 CMSIS-DSP、不用 tflite-micro：主机侧能直接 gcc -lm 编译，
 * 和 numpy 参考实现对拍（见 _sound_event_host_test/）。
 *
 * ---- 和训练侧严格对齐的前端参数（对照 tools/audio_event/mel.py）----
 *   采样率 16000，单声道 s16le
 *   帧长 400 样本(25ms)，帧跳 160(10ms)，**symmetric** Hann 窗（np.hanning(400)，
 *   即 0.5-0.5cos(2*pi*n/(400-1))）
 *   512 点 FFT（不足补零）→ 功率谱 257 点 |X|^2（不除 N；log 后归一化会把常数消掉）
 *   40 个三角 mel，f_min=0 Hz，f_max=8000 Hz；mel(f)=2595*log10(1+f/700)，
 *   顶点归一化到 1（**在 Hz 域线性插值**，不做面积归一化）：
 *     第 k 个 bin 的中心频率 fk = k*SR/NFFT，权重 = clip(min((fk-lo)/(ce-lo),
 *     (hi-fk)/(hi-ce)), 0, +inf)
 *   取 log(max(x, 1e-10))（自然对数）
 *   窗口 = 连续 64 帧(0.64s)，滑窗步长 20 帧(200ms)
 *   归一化：整窗 2560 个值做 (x-mean)/(std+1e-5)（总体标准差，ddof=0）
 *   存放 mel[m*64 + t]
 *
 * ---- 网络（float32，BN 已折进权重，C 侧没有 BN）----
 *   c1  conv 3x3 pad1 s2 1->16   -> 16x20x32  +ReLU
 *   dw2 depthwise 3x3 pad1 s1    -> 16x20x32  +ReLU
 *   pw2 conv 1x1 16->32          -> 32x20x32  +ReLU
 *   dw3 depthwise 3x3 pad1 s1    -> 32x20x32  +ReLU
 *   pw3 conv 1x1 32->32          -> 32x20x32  +ReLU
 *   gap 空间全局平均池化          -> 32
 *   fc  32->4
 *
 * ---- 内存取舍 ----
 * 板子 SRAM 紧（.bss 已经 211KB / 512KB），所以不整层落中间张量，
 * 卷积按“行”流水，每一级只留 3 行环形缓冲：静态量约 52KB。
 * 卷积是最朴素的循环，只求正确不求快（约 2.4e7 MAC/s 量级）。
 ****************************************************************************/

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sound_event.h"
#include "sound_event_model.h"

/****************************************************************************
 * 编译期常量
 ****************************************************************************/

#define SE_SR          16000
#define SE_FLEN        400                  /* 帧长 */
#define SE_FHOP        160                  /* 帧跳 = 10ms */
#define SE_NFFT        512
#define SE_NBIN        (SE_NFFT / 2 + 1)    /* 257 */
#define SE_MEL         40                   /* mel 维 */
#define SE_FRAME       64                   /* 一个窗口的帧数 */
#define SE_CLASS       4
#define SE_WINHOP      20                   /* 滑窗步长（帧）= 200ms */
#define SE_PI          3.14159265358979323846

/* 网络空间尺寸 */
#define SE_H0          SE_MEL               /* 输入高 40 */
#define SE_W0          SE_FRAME             /* 输入宽 64 */
#define SE_H1          20                   /* 降采样后高 */
#define SE_W1          32                   /* 降采样后宽 */
#define SE_CH1         16                   /* c1 / dw2 通道 */
#define SE_CH2         32                   /* pw2 / dw3 / pw3 通道 */

/* 权重段长度（和导出脚本的拼接顺序一一对应） */
#define SE_C1_N        144
#define SE_B1_N        16
#define SE_DW2_N       144
#define SE_BDW2_N      16
#define SE_PW2_N       512
#define SE_BPW2_N      32
#define SE_DW3_N       288
#define SE_BDW3_N      32
#define SE_PW3_N       1024
#define SE_BPW3_N      32
#define SE_FC_N        128
#define SE_BFC_N       4
#define SE_WEIGHTS     (SE_C1_N + SE_B1_N + SE_DW2_N + SE_BDW2_N + SE_PW2_N + \
                        SE_BPW2_N + SE_DW3_N + SE_BDW3_N + SE_PW3_N +          \
                        SE_BPW3_N + SE_FC_N + SE_BFC_N)

/* 权重段在 g_sound_event_w[] 里的偏移 */
#define SE_O_C1        0
#define SE_O_B1        (SE_O_C1   + SE_C1_N)
#define SE_O_DW2       (SE_O_B1   + SE_B1_N)
#define SE_O_BDW2      (SE_O_DW2  + SE_DW2_N)
#define SE_O_PW2       (SE_O_BDW2 + SE_BDW2_N)
#define SE_O_BPW2      (SE_O_PW2  + SE_PW2_N)
#define SE_O_DW3       (SE_O_BPW2 + SE_BPW2_N)
#define SE_O_BDW3      (SE_O_DW3  + SE_DW3_N)
#define SE_O_PW3       (SE_O_BDW3 + SE_BDW3_N)
#define SE_O_BPW3      (SE_O_PW3  + SE_PW3_N)
#define SE_O_FC        (SE_O_BPW3 + SE_BPW3_N)
#define SE_O_BFC       (SE_O_FC   + SE_FC_N)

/* 导出头文件必须和本文件的契约一致，不一致就编译报错（别默默算错维度） */
#if defined(SE_NMEL) && (SE_NMEL != SE_MEL)
#error "sound_event_model.h: SE_NMEL 必须是 40"
#endif
#if defined(SE_NFRAME) && (SE_NFRAME != SE_FRAME)
#error "sound_event_model.h: SE_NFRAME 必须是 64"
#endif
#if defined(SE_NCLASS) && (SE_NCLASS != SE_CLASS)
#error "sound_event_model.h: SE_NCLASS 必须是 4"
#endif
#if defined(SE_W_LEN) && (SE_W_LEN != SE_WEIGHTS)
#error "sound_event_model.h: SE_W_LEN 必须是 2372"
#endif

/* 后处理参数 */
#define SE_NMELBIN     42                   /* mel 边界点数 = 40+2 */
#define SE_BINHZ       ((float)SE_SR / (float)SE_NFFT)   /* 每个 FFT bin 的频宽 31.25Hz */
#define SE_PCMBUF      2048                 /* PCM 残余缓冲（样本） */
#define SE_VOTE_N      4                    /* 看最近 4 个窗口 */
#define SE_VOTE_NEED   3                    /* 至少 3 票 */
#define SE_REFRACT_WIN 40                   /* scream 不应期 8s = 40 个窗口步 */
/* ★ 2026-11-01：fall/knock 都属于"撞击"，不应期给 2 秒（10 窗）。
 *
 *   "敲击求救"是连着敲；而且真机把敲击判成 fall 的情况比判成 knock 还多
 *   （用户连敲时 top1=fall 0.57 出现多次，knock 一次都没有）。如果 fall 还
 *   保持 8 秒，第二下会被直接吞掉，"15 秒内两下"永远凑不成。
 *   代价：持续噪声能更频繁地产生 hit —— 由上层"两次确认 + 单次 ≥0.80"
 *   和门控的"持续声不报/人声余温"兜着。 */
#define SE_REFRACT_WIN_IMPACT 10

/* 每类阈值（softmax 概率），索引 0 是 other，不触发所以填 0。
 *
 * ★ 2026-10-31 从 {0, 0.60, 0.70, 0.70} 整体下调到 0.55 —— 这是真机日志逼出来的，
 *   不是拍脑袋：
 *
 *   用户 2026-10-31 傍晚**大声叫喊**，板子一声没响。串口里小模型其实一直在判：
 *       #16 top1=knock 0.72 | fall=0 knock=3 scream=0 | ⚠票够了但平均置信度低于阈值
 *       #18 top1=knock 0.70 | fall=0 knock=4 scream=0 | ⚠票够了但平均置信度低于阈值
 *   票数早够了（3/4、4/4），卡的全是**置信度**这一关：4 票的均值实测落在
 *   0.49~0.63 之间，而 knock/scream 的阈值是 0.70 —— 在真机流式音频上根本
 *   够不着。等于这两类**永远不可能触发**，用户喊破喉咙也没用。
 *
 *   0.55 这个数是拿实测分布定的：真命中那一档（fall 0.67、knock 均值 0.62/0.63）
 *   都在 0.55 之上，纯噪声那一档（top1=other 0.50、knock 0.36~0.46）都在之下。
 *
 *   下调阈值涨的是**误报**，不是漏报；而误报的代价已经被下面两道兜住：
 *     · 3/4 投票 + 8 秒同类不应期（连着几个窗都要判成同一类才算数）；
 *     · 报警起铃后有 **10 秒说「没事」就能取消**的确认窗（voice_emerg）。
 *   反过来漏报的代价是老人倒地没人知道 —— 这笔账怎么算都该往下调。 */
static const float g_threshold[SE_CLASS] =
{
  0.0f, 0.55f, 0.55f, 0.55f
};

/* ★ 2026-11-01：fall/knock/scream 都回到 0.55，**误报交给上层判据**。
 *
 *   真机数据：
 *     · fall 的误报（开机持续环境噪声）0.74；真敲被模型判成 fall 时
 *       0.57~0.63 —— 单靠 0.80 会把真敲也挡掉（用户实测"敲击没反应"）；
 *     · knock 的真事件和误报完全重叠（真敲 0.64~0.73 / 误报 0.57~0.71）；
 *     · scream 几乎不出分（0.00~0.11），喊叫主要靠云端 ASR 关键词。
 *
 *   所以阈值统一 0.55，改由 ai_companion 的**撞击两次确认**兜误报：
 *     · 单次置信度 ≥ 0.80 → 立刻报（非常像的真事件）；
 *     · 否则 15 秒内第二次撞击（fall 或 knock 任意组合）→ 报；
 *     · 单次 0.55~0.80 → 只记时间，不报。
 *   再加门控的人声余温/持续声 veto，挡住"说话/持续噪声"。 */

/****************************************************************************
 * 只读表 + 流式状态（全部 static，单实例）
 ****************************************************************************/

static float    g_hann[SE_FLEN];
static float    g_tw_re[SE_NFFT / 2];       /* cos(-2*pi*k/N) */
static float    g_tw_im[SE_NFFT / 2];       /* sin(-2*pi*k/N) */
static uint16_t g_bitrev[SE_NFFT];
static float    g_melhz[SE_NMELBIN];        /* 42 个 mel 边界频率（Hz） */
static int      g_mbin_lo[SE_MEL];          /* 每个滤波器起作用的 bin 区间（含端） */
static int      g_mbin_hi[SE_MEL];

/* FFT / 功率谱的工作缓冲。做成 static 而不是局部变量：
 * se_frame_mel() 是在麦克风线程里被调用的，4KB 的 re/im 放栈上太危险。 */
static float    g_fft_re[SE_NFFT];
static float    g_fft_im[SE_NFFT];
static double   g_fft_ps[SE_NBIN];

/* 原始 log-mel 滑窗历史：mel[m*SE_FRAME + t]，始终按时间顺序 */
static float    g_melhist[SE_MEL * SE_FRAME];
static float    g_win[SE_MEL * SE_FRAME];   /* 归一化后的卷积输入（整窗） */
static int      g_win_fill;                 /* 已写入的帧数：首窗 0..64 / 稳态 0..20 */
static int      g_win_first;                /* 1 = 还在攒第一个窗口 */

static int16_t  g_pcm[SE_PCMBUF];           /* 待切帧样本，起点永远是帧边界 */
static size_t   g_pcm_len;

/* 网络流水行缓冲 */
static float    g_c1ring[3][SE_CH1][SE_W1]; /* c1 输出最近 3 行 */
static int      g_c1_hi;                    /* 已算到第几行 c1（-1 = 一行都还没有） */
static float    g_pw2ring[3][SE_CH2][SE_W1];/* pw2 输出最近 3 行 */
static float    g_dw2row[SE_CH1][SE_W1];
static float    g_dw3row[SE_CH2][SE_W1];
static float    g_gap[SE_CH2];

/* 输出 + 后处理状态 */
static float    g_logit[SE_CLASS];
static float    g_softmax[SE_CLASS];
static float    g_last_conf;
static int      g_nwindows;                 /* 已推理窗口数；1 个窗口 = 200ms 音频 */
static int      g_votes[SE_VOTE_N];
static float    g_voteconf[SE_VOTE_N];
static int      g_vote_pos;
static int      g_fire_win[SE_CLASS];       /* 每类上次触发的窗口序号，-1 = 从未 */

static sound_event_cb_t g_cb;
static void            *g_cb_arg;
static int              g_inited;

/* 本次 feed 的来源窗口是不是"人声"（门控判的）。
 *
 * ★ 2026-11-01 真机定案：v12 为了让尖叫类能触发，把**人声窗也喂给模型**，
 *   结果模型把正常说话/喊叫全判成 knock/fall —— 11 分钟里 5 次误报报警
 *   （fall 0.71 / knock 0.56/0.57/0.71/0.67，全部来自 ac=0.66~0.90 的人声窗）。
 *   语义上"敲击/跌倒"本来就不该由人声触发，所以人声窗**只允许 SCREAM 类投票**；
 *   其余类别一律按 other 记。尖叫仍然走本地模型，模型认不出来时由门控把这段
 *   人声送云端 ASR，走「救命/救我/着火」关键词那条路兜底。 */
static int              g_voice_hint;
static int              g_loud_hint;       /* 本次喂入的窗**现在仍然很响** */
static int              se_voice_dbg_count;

/****************************************************************************
 * 小工具
 ****************************************************************************/

static float se_hz2mel(float f)
{
  return 2595.0f * log10f(1.0f + f / 700.0f);
}

static float se_mel2hz(float m)
{
  return 700.0f * (powf(10.0f, m / 2595.0f) - 1.0f);
}

static int se_clamp_int(int v, int lo, int hi)
{
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

/* 迭代式 radix-2 Cooley-Tukey，原地，长度固定 512。
 * 旋转因子和位反转表都在 init() 里预先算好。 */
static void se_fft512(float *re, float *im)
{
  int i;
  int len;

  for (i = 0; i < SE_NFFT; i++)
    {
      int j = g_bitrev[i];

      if (j > i)
        {
          float tr = re[i]; re[i] = re[j]; re[j] = tr;
          float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }

  for (len = 2; len <= SE_NFFT; len <<= 1)
    {
      int half = len >> 1;
      int step = SE_NFFT / len;
      int blk;

      for (blk = 0; blk < SE_NFFT; blk += len)
        {
          int j;

          for (j = 0; j < half; j++)
            {
              int   k  = j * step;
              float wr = g_tw_re[k];
              float wi = g_tw_im[k];
              float ur = re[blk + j];
              float ui = im[blk + j];
              float xr = re[blk + j + half];
              float xi = im[blk + j + half];
              float vr = xr * wr - xi * wi;
              float vi = xr * wi + xi * wr;

              re[blk + j]        = ur + vr;
              im[blk + j]        = ui + vi;
              re[blk + j + half] = ur - vr;
              im[blk + j + half] = ui - vi;
            }
        }
    }
}

/* 一帧 PCM(400 样本) → 40 维 log-mel。
 * 三角滤波器的权值现算（每个滤波器只有十几个非零 bin），省掉 41KB 的滤波器组表。 */
static void se_frame_mel(const int16_t *pcm, float *out)
{
  float *re = g_fft_re;
  float *im = g_fft_im;
  double *ps = g_fft_ps;
  int    m;
  int    n;

  for (n = 0; n < SE_FLEN; n++)
    {
      re[n] = (float)pcm[n] * g_hann[n];
      im[n] = 0.0f;
    }

  for (n = SE_FLEN; n < SE_NFFT; n++)
    {
      re[n] = 0.0f;
      im[n] = 0.0f;
    }

  se_fft512(re, im);

  for (n = 0; n < SE_NBIN; n++)
    {
      ps[n] = (double)re[n] * (double)re[n] + (double)im[n] * (double)im[n];
    }

  for (m = 0; m < SE_MEL; m++)
    {
      float  lo    = g_melhz[m];
      float  ce    = g_melhz[m + 1];
      float  hi    = g_melhz[m + 2];
      float  invup = (ce > lo) ? 1.0f / (ce - lo) : 0.0f;
      float  invdn = (hi > ce) ? 1.0f / (hi - ce) : 0.0f;
      double acc   = 0.0;
      int    k;

      for (k = g_mbin_lo[m]; k <= g_mbin_hi[m]; k++)
        {
          float f  = (float)k * SE_BINHZ;
          float up = (f - lo) * invup;
          float dn = (hi - f) * invdn;
          float w  = up < dn ? up : dn;

          if (w > 0.0f)
            {
              acc += (double)w * ps[k];
            }
        }

      if (acc < 1e-10)
        {
          acc = 1e-10;
        }

      out[m] = (float)log(acc);
    }
}

/****************************************************************************
 * 网络前向：按行流水，一级一级往下推
 * 行号 r 一律指“降采样后”的 20 行（c1 / dw2 / pw2 / dw3 / pw3 都是 20 行）
 ****************************************************************************/

/* c1 第 oh 行 → g_c1ring[oh%3]；输入是 mel 行 2*oh-1 / 2*oh / 2*oh+1 */
static void se_c1_row(int oh)
{
  float *dst = &g_c1ring[oh % 3][0][0];
  int    oc;

  for (oc = 0; oc < SE_CH1; oc++)
    {
      const float *w = &g_sound_event_w[SE_O_C1 + oc * 9];
      float        b = g_sound_event_w[SE_O_B1 + oc];
      int          ow;

      for (ow = 0; ow < SE_W1; ow++)
        {
          float acc = b;
          int   dh;

          for (dh = 0; dh < 3; dh++)
            {
              int ih = 2 * oh - 1 + dh;
              int dw;

              if (ih < 0 || ih >= SE_H0) continue;

              for (dw = 0; dw < 3; dw++)
                {
                  int iw = 2 * ow - 1 + dw;

                  if (iw < 0 || iw >= SE_W0) continue;
                  acc += w[dh * 3 + dw] * g_win[ih * SE_FRAME + iw];
                }
            }

          dst[oc * SE_W1 + ow] = acc > 0.0f ? acc : 0.0f;
        }
    }
}

/* dw2 第 r 行：读 c1 的 r-1 / r / r+1 行（越界行按零填充跳过） */
static void se_dw2_row(int r)
{
  const float *up = &g_c1ring[(r + 2) % 3][0][0];   /* 行 r-1 */
  const float *md = &g_c1ring[r % 3][0][0];         /* 行 r   */
  const float *dn = &g_c1ring[(r + 1) % 3][0][0];   /* 行 r+1 */
  int          ch;

  for (ch = 0; ch < SE_CH1; ch++)
    {
      const float *w = &g_sound_event_w[SE_O_DW2 + ch * 9];
      float        b = g_sound_event_w[SE_O_BDW2 + ch];
      int          ow;

      for (ow = 0; ow < SE_W1; ow++)
        {
          float acc = b;
          int   dh;

          for (dh = 0; dh < 3; dh++)
            {
              int          ih = r - 1 + dh;
              const float *src;
              int          dw;

              if (ih < 0 || ih >= SE_H1) continue;
              src = (ih == r) ? md : ((ih < r) ? up : dn);

              for (dw = 0; dw < 3; dw++)
                {
                  int iw = ow - 1 + dw;

                  if (iw < 0 || iw >= SE_W1) continue;
                  acc += w[dh * 3 + dw] * src[ch * SE_W1 + iw];
                }
            }

          g_dw2row[ch][ow] = acc > 0.0f ? acc : 0.0f;
        }
    }
}

/* pw2 第 r 行（1x1，16→32）→ g_pw2ring[r%3] */
static void se_pw2_row(int r)
{
  float *dst = &g_pw2ring[r % 3][0][0];
  int    oc;

  for (oc = 0; oc < SE_CH2; oc++)
    {
      const float *w = &g_sound_event_w[SE_O_PW2 + oc * SE_CH1];
      float        b = g_sound_event_w[SE_O_BPW2 + oc];
      int          ow;

      for (ow = 0; ow < SE_W1; ow++)
        {
          float acc = b;
          int   ic;

          for (ic = 0; ic < SE_CH1; ic++)
            {
              acc += w[ic] * g_dw2row[ic][ow];
            }

          dst[oc * SE_W1 + ow] = acc > 0.0f ? acc : 0.0f;
        }
    }
}

/* dw3 第 r 行：读 pw2 的 r-1 / r / r+1 行 */
static void se_dw3_row(int r)
{
  const float *up = &g_pw2ring[(r + 2) % 3][0][0];
  const float *md = &g_pw2ring[r % 3][0][0];
  const float *dn = &g_pw2ring[(r + 1) % 3][0][0];
  int          ch;

  for (ch = 0; ch < SE_CH2; ch++)
    {
      const float *w = &g_sound_event_w[SE_O_DW3 + ch * 9];
      float        b = g_sound_event_w[SE_O_BDW3 + ch];
      int          ow;

      for (ow = 0; ow < SE_W1; ow++)
        {
          float acc = b;
          int   dh;

          for (dh = 0; dh < 3; dh++)
            {
              int          ih = r - 1 + dh;
              const float *src;
              int          dw;

              if (ih < 0 || ih >= SE_H1) continue;
              src = (ih == r) ? md : ((ih < r) ? up : dn);

              for (dw = 0; dw < 3; dw++)
                {
                  int iw = ow - 1 + dw;

                  if (iw < 0 || iw >= SE_W1) continue;
                  acc += w[dh * 3 + dw] * src[ch * SE_W1 + iw];
                }
            }

          g_dw3row[ch][ow] = acc > 0.0f ? acc : 0.0f;
        }
    }
}

/* pw3 一行（1x1，32→32）+ReLU，直接累加进 GAP（省掉一整层的 20x32x32） */
static void se_pw3_row(void)
{
  int oc;

  for (oc = 0; oc < SE_CH2; oc++)
    {
      const float *w = &g_sound_event_w[SE_O_PW3 + oc * SE_CH2];
      float        b = g_sound_event_w[SE_O_BPW3 + oc];
      int          ow;

      for (ow = 0; ow < SE_W1; ow++)
        {
          float acc = b;
          int   ic;

          for (ic = 0; ic < SE_CH2; ic++)
            {
              acc += w[ic] * g_dw3row[ic][ow];
            }

          acc = acc > 0.0f ? acc : 0.0f;
          g_gap[oc] += acc;
        }
    }
}

static void se_forward(void)
{
  int r;

  g_c1_hi = -1;
  memset(g_gap, 0, sizeof(g_gap));

  for (r = 0; r < SE_H1; r++)
    {
      /* 先把 c1 行 r+1 算出来（dw2 行 r 需要 c1 的 r-1/r/r+1） */
      while (g_c1_hi < r + 1 && g_c1_hi + 1 < SE_H1)
        {
          g_c1_hi++;
          se_c1_row(g_c1_hi);
        }

      se_dw2_row(r);
      se_pw2_row(r);

      /* 此时 dw3 行 r-1 需要的 pw2 行 r-2/r-1/r 都齐了 */
      if (r >= 1)
        {
          se_dw3_row(r - 1);
          se_pw3_row();
        }
    }

  se_dw3_row(SE_H1 - 1);
  se_pw3_row();

  for (r = 0; r < SE_CH2; r++)
    {
      g_gap[r] /= (float)(SE_H1 * SE_W1);
    }

  {
    int oc;

    for (oc = 0; oc < SE_CLASS; oc++)
      {
        const float *w = &g_sound_event_w[SE_O_FC + oc * SE_CH2];
        float        acc = g_sound_event_w[SE_O_BFC + oc];
        int          ic;

        for (ic = 0; ic < SE_CH2; ic++)
          {
            acc += w[ic] * g_gap[ic];
          }

        g_logit[oc] = acc;
      }
  }
}

/****************************************************************************
 * 窗口级：归一化 + 推理 + 投票后处理
 ****************************************************************************/

/* 把 g_melhist 整窗归一化到 g_win（g_melhist 保持原始值，供下一窗复用） */
static void se_normalize(void)
{
  const int n = SE_MEL * SE_FRAME;
  double    sum = 0.0;
  double    var = 0.0;
  double    mean;
  double    inv;
  int       i;

  for (i = 0; i < n; i++)
    {
      sum += (double)g_melhist[i];
    }

  mean = sum / (double)n;

  for (i = 0; i < n; i++)
    {
      double d = (double)g_melhist[i] - mean;

      var += d * d;
    }

  var /= (double)n;                 /* 总体标准差，和 numpy 的 .std() 一致 */
  inv  = 1.0 / (sqrt(var) + 1e-5);

  for (i = 0; i < n; i++)
    {
      g_win[i] = (float)(((double)g_melhist[i] - mean) * inv);
    }
}

static void se_softmax(void)
{
  float  mx = g_logit[0];
  double sum = 0.0;
  int    i;

  for (i = 1; i < SE_CLASS; i++)
    {
      if (g_logit[i] > mx) mx = g_logit[i];
    }

  for (i = 0; i < SE_CLASS; i++)
    {
      double e = exp((double)(g_logit[i] - mx));

      g_softmax[i] = (float)e;
      sum += e;
    }

  for (i = 0; i < SE_CLASS; i++)
    {
      g_softmax[i] = (float)((double)g_softmax[i] / sum);
    }
}

/* 3/4 窗口投票 + 每类阈值 + 同类 8s 不应期。返回触发的类别，-1 = 没触发。 */

/* ★ 后处理可观测（2026-10-31）。
 *
 * 排查"摔倒检测一点反应都没有"时，小模型这边是完全黑的：喂了几个窗、
 * 每窗 top-1 是什么、票攒到几票、是被阈值挡的还是被不应期挡的 —— 一行都
 * 没有。于是"门控没喂"和"喂了但投票凑不齐"这两种病因根本分不开。
 *
 * 限速规则：top-1 不是 other 就**每一窗都打**（真事件只有几窗，一窗都不能漏）；
 * top-1 是 other 就 25 窗打一次（5 秒），别把串口灌满。 */

static int se_dbg_count;

static void se_dbg(const char *why, int best, float bestp,
                   const int *cnt, int nwindows)
{
  int interesting = (best != SOUND_EVENT_OTHER);

  if (!interesting)
    {
      if ((se_dbg_count++ % 25) != 0)
        {
          return;
        }
    }

  printf("[小模型] #%-5d top1=%s %.2f | fall=%d knock=%d scream=%d | %s\n",
         nwindows, sound_event_class_name(best), bestp,
         cnt[SOUND_EVENT_FALL], cnt[SOUND_EVENT_KNOCK], cnt[SOUND_EVENT_SCREAM],
         why);
}

static int se_postprocess(void)
{
  int   best  = 0;
  float bestp = g_softmax[0];
  int   cnt[SE_CLASS];
  float acc[SE_CLASS];
  float pk[SE_CLASS];       /* 该类几张票里**最高**的那一票（2026-10-31 加） */
  int   i;
  int   c;

  for (i = 1; i < SE_CLASS; i++)
    {
      if (g_softmax[i] > bestp)
        {
          bestp = g_softmax[i];
          best  = i;
        }
    }

  /* ★ 人声窗只允许 SCREAM 类投票（见 g_voice_hint 的注释）。
   *   这里不能只看 top-1：真实尖叫时 knock 的 softmax 可能更高，
   *   但 scream 那一维仍然有分数，所以直接看 scream 这一维。
   *   阈值 0.20 只是"够格投一票"，真正能不能触发仍由 3/4 投票 +
   *   g_threshold[SCREAM]（0.55）把关。 */
  if (g_voice_hint)
    {
      float sp = g_softmax[SOUND_EVENT_SCREAM];

      if (sp >= 0.20f)
        {
          best  = SOUND_EVENT_SCREAM;
          bestp = sp;
        }
      else
        {
          best  = SOUND_EVENT_OTHER;
          bestp = 1.0f - sp;
        }

      /* 每 5 个人声窗打一行四类分数：下一版调 scream 阈值/判断
       * "模型到底有没有尖叫检测"就看这行。 */
      if ((se_voice_dbg_count++ % 5) == 0)
        {
          printf("[小模型] 人声窗 scores other=%.2f fall=%.2f knock=%.2f "
                 "scream=%.2f -> 投 %s\n",
                 (double)g_softmax[SOUND_EVENT_OTHER],
                 (double)g_softmax[SOUND_EVENT_FALL],
                 (double)g_softmax[SOUND_EVENT_KNOCK],
                 (double)g_softmax[SOUND_EVENT_SCREAM],
                 sound_event_class_name(best));
        }
    }

  g_last_conf = bestp;

  g_votes[g_vote_pos]    = best;
  g_voteconf[g_vote_pos] = bestp;
  g_vote_pos             = (g_vote_pos + 1) % SE_VOTE_N;

  memset(cnt, 0, sizeof(cnt));
  for (i = 0; i < SE_CLASS; i++)
    {
      acc[i] = 0.0f;
      pk[i]  = 0.0f;
    }

  for (i = 0; i < SE_VOTE_N; i++)
    {
      int v = g_votes[i];

      if (v < 0 || v >= SE_CLASS) continue;
      cnt[v]++;
      acc[v] += g_voteconf[i];
      if (g_voteconf[i] > pk[v])
        {
          pk[v] = g_voteconf[i];
        }
    }

  /* 票数不够（前两个窗口）直接不判：3/4 投票至少要攒到 3 个真实窗口 */
  if (g_nwindows < SE_VOTE_NEED)
    {
      se_dbg("首窗攒票中，还没得判", best, bestp, cnt, g_nwindows);
      return -1;
    }

  for (c = 1; c < SE_CLASS; c++)   /* other 不触发 */
    {
      float conf;

      /* 人声窗这一票只允许尖叫触发：非尖叫类即便靠前面几窗的票凑够了，
       * 也不在这一窗放行（否则话音一响就把上一声撞击的票补成报警）。 */
      if (g_voice_hint && c != SOUND_EVENT_SCREAM)
        {
          continue;
        }

      if (cnt[c] < SE_VOTE_NEED)
        {
          if (best == c)
            {
              se_dbg("⚠这一窗投给它了，但最近 4 窗里票不够（要 3 票）—— "
                     "典型的是\"撞击太短，只够一两窗\"",
                     best, bestp, cnt, g_nwindows);
            }

          continue;
        }

      /* ★ 2026-10-31：判据从「均值」放宽成「均值和最高一票取大」。
       *
       *   真机日志里最常见的形状是"三票里两票一般、一票很亮"：
       *       #16 knock 0.59 / 0.56 / 0.72  -> 均值 0.62
       *       #18 knock 0.56 / 0.50 / 0.70  -> 均值 0.59
       *   流式音频每一窗只吃到事件的 1/3 左右，本来就不可能窗窗都亮；
       *   拿均值去卡，等于要求"整段都像"，而短促撞击/一声叫喊天然是
       *   "中间一两窗特别像"。取大之后这类就能过。
       *
       *   误报仍然有 3/4 投票兜着 —— 必须**最近 4 窗里有 3 窗**都判成同一类，
       *   单单一窗亮不算数。 */
      {
        float mean = acc[c] / (float)cnt[c];
        float peak = pk[c];

        conf = (mean > peak) ? mean : peak;

        if (conf < g_threshold[c])
          {
            if (best == c)
              {
                se_dbg("⚠票够了但置信度低于阈值（均值和最高一票都不够）",
                       best, bestp, cnt, g_nwindows);
              }

            continue;
          }

        printf("[小模型] 判据 %s：均值 %.2f / 最高一票 %.2f -> 用 %.2f（阈值 %.2f）\n",
               sound_event_class_name(c), (double)mean, (double)peak,
               (double)conf, (double)g_threshold[c]);
      }

      /* ★ 2026-11-01：**声音还在持续（这一窗仍然很响）时，fall/knock 先不报**。
       *
       *   真撞击/摔倒是一声短响，能量很快就会回落；风扇、摩擦、持续环境声
       *   却能连续好几窗都过能量门槛，模型照样能在这几窗里攒够 fall 票。
       *   真机开机 50 秒、用户什么都没做，就靠一段 rms≈0.05 的持续声报了
       *   「跌倒 74%」—— 就是这条。
       *
       *   处理：这一窗仍然响 → 只记票、不触发；等它回落以后，票的窗口
       *   （最近 4 窗）大概率已经过期，持续声自然报不出来。短促真事件
       *   在 0.6~1 秒内就安静了，不受影响。SCREAM 不受此限（尖叫本来就
       *   是持续的浊音）。 */
      if (g_loud_hint && c != SOUND_EVENT_SCREAM)
        {
          printf("[小模型] ⚠%s 票够了（%.2f），但声音还在持续（未回落），先不报\n",
                 sound_event_class_name(c), (double)conf);
          continue;
        }

      {
        int refract = (c == SOUND_EVENT_SCREAM) ? SE_REFRACT_WIN
                                                : SE_REFRACT_WIN_IMPACT;

        if (g_fire_win[c] >= 0 && (g_nwindows - g_fire_win[c]) < refract)
          {
            se_dbg("⚠判出来了，但撞上同类不应期", best, bestp, cnt,
                   g_nwindows);
            continue;
          }
      }

      g_fire_win[c] = g_nwindows;

      printf("[小模型] ★命中 %s（置信度 %.2f，第 %d 窗）\n",
             sound_event_class_name(c), conf, g_nwindows);

      if (g_cb != NULL)
        {
          g_cb(c, conf, g_cb_arg);
        }

      return c;
    }

  /* top-1 不是 other 的三种"差一点"上面已经各自打了；这里只兜 top-1=other。 */

  if (best == SOUND_EVENT_OTHER)
    {
      se_dbg("没触发", best, bestp, cnt, g_nwindows);
    }

  return -1;
}

static void se_run_window(void)
{
  se_normalize();
  se_forward();
  se_softmax();
  g_nwindows++;
  se_postprocess();
}

/* 把一帧 PCM 的 40 维 log-mel 散列写入 g_melhist 的第 col 列 */
static void se_store_frame(const int16_t *pcm, int col)
{
  float mel[SE_MEL];
  int   m;

  se_frame_mel(pcm, mel);

  for (m = 0; m < SE_MEL; m++)
    {
      g_melhist[m * SE_FRAME + col] = mel[m];
    }
}

/****************************************************************************
 * 公开 API
 ****************************************************************************/

int sound_event_init(void)
{
  double mel_lo = (double)se_hz2mel(0.0f);
  double mel_hi = (double)se_hz2mel(8000.0f);
  int    i;

  if (SE_WEIGHTS != SE_W_LEN)
    {
      return -1;
    }

  /* symmetric Hann 窗：np.hanning(400) = 0.5-0.5cos(2*pi*n/(400-1))。
   * 注意分母是 N-1（不是 N），和训练侧 mel.py 的 hann_window() 一致。 */
  for (i = 0; i < SE_FLEN; i++)
    {
      g_hann[i] = 0.5f - 0.5f * cosf(2.0f * (float)SE_PI * (float)i /
                                    (float)(SE_FLEN - 1));
    }

  /* FFT 旋转因子 */
  for (i = 0; i < SE_NFFT / 2; i++)
    {
      double ang = -2.0 * SE_PI * (double)i / (double)SE_NFFT;

      g_tw_re[i] = (float)cos(ang);
      g_tw_im[i] = (float)sin(ang);
    }

  /* 位反转表（512 = 2^9） */
  for (i = 0; i < SE_NFFT; i++)
    {
      int v = i;
      int r = 0;
      int b;

      for (b = 0; b < 9; b++)
        {
          r = (r << 1) | (v & 1);
          v >>= 1;
        }

      g_bitrev[i] = (uint16_t)r;
    }

  /* mel 滤波器组：42 个边界频率由等间隔 mel 点反变换得到（mel.py 的做法），
   * 每个滤波器的实际权重在 se_frame_mel() 里按 Hz 域线性插值现算。
   * 这里只预算出每个滤波器真正起作用的 bin 区间，省掉整张 40x257 的表。 */
  for (i = 0; i < SE_NMELBIN; i++)
    {
      double t = mel_lo + (mel_hi - mel_lo) * (double)i /
                 (double)(SE_NMELBIN - 1);

      g_melhz[i] = (float)se_mel2hz((float)t);
    }

  for (i = 0; i < SE_MEL; i++)
    {
      int klo = (int)floor((double)g_melhz[i] / (double)SE_BINHZ);
      int khi = (int)ceil((double)g_melhz[i + 2] / (double)SE_BINHZ);

      klo = se_clamp_int(klo, 0, SE_NBIN - 1);
      khi = se_clamp_int(khi, 0, SE_NBIN - 1);
      if (khi < klo)
        {
          khi = klo;
        }

      g_mbin_lo[i] = klo;
      g_mbin_hi[i] = khi;
    }

  sound_event_reset();
  g_inited = 1;
  return 0;
}

void sound_event_reset(void)
{
  int i;

  memset(g_melhist, 0, sizeof(g_melhist));
  memset(g_win, 0, sizeof(g_win));
  memset(g_pcm, 0, sizeof(g_pcm));
  memset(g_c1ring, 0, sizeof(g_c1ring));
  memset(g_pw2ring, 0, sizeof(g_pw2ring));
  memset(g_dw2row, 0, sizeof(g_dw2row));
  memset(g_dw3row, 0, sizeof(g_dw3row));
  memset(g_gap, 0, sizeof(g_gap));
  memset(g_logit, 0, sizeof(g_logit));
  memset(g_softmax, 0, sizeof(g_softmax));
  memset(g_votes, 0, sizeof(g_votes));
  memset(g_voteconf, 0, sizeof(g_voteconf));

  g_pcm_len     = 0;
  g_win_fill    = 0;
  g_win_first   = 1;
  g_c1_hi       = -1;
  g_last_conf   = 0.0f;
  g_nwindows    = 0;
  g_vote_pos    = 0;

  for (i = 0; i < SE_CLASS; i++)
    {
      g_fire_win[i] = -1;
    }
}

void sound_event_set_callback(sound_event_cb_t cb, void *arg)
{
  g_cb     = cb;
  g_cb_arg = arg;
}

const char *sound_event_class_name(int cls)
{
  switch (cls)
    {
      case SOUND_EVENT_OTHER:  return "other";
      case SOUND_EVENT_FALL:   return "fall";
      case SOUND_EVENT_KNOCK:  return "knock";
      case SOUND_EVENT_SCREAM: return "scream";
      default:                 return "unknown";
    }
}

float sound_event_last_confidence(void)
{
  return g_last_conf;
}

int sound_event_feed_ex(const int16_t *pcm, size_t nsamples, int voice_hint,
                        int loud_hint)
{
  size_t off  = 0;
  int    wins = 0;
  int    saved_hint;
  int    saved_loud;

  if (!g_inited || (pcm == NULL && nsamples > 0))
    {
      return -1;
    }

  saved_hint   = g_voice_hint;
  saved_loud   = g_loud_hint;
  g_voice_hint = voice_hint ? 1 : 0;
  g_loud_hint  = loud_hint ? 1 : 0;

  while (off < nsamples)
    {
      size_t room = (size_t)SE_PCMBUF - g_pcm_len;
      size_t take = nsamples - off;

      if (take > room)
        {
          take = room;
        }

      if (take == 0)
        {
          break;   /* 防御分支：正常情况下 g_pcm_len 每轮都会被吃回 < 400 */
        }

      memcpy(&g_pcm[g_pcm_len], &pcm[off], take * sizeof(int16_t));
      g_pcm_len += take;
      off       += take;

      while (g_pcm_len >= SE_FLEN)
        {
          if (g_win_first)
            {
              /* 首窗：第 t 帧就放第 t 列 */
              se_store_frame(g_pcm, g_win_fill);
              g_win_fill++;

              if (g_win_fill >= SE_FRAME)
                {
                  se_run_window();
                  g_win_first = 0;
                  g_win_fill  = 0;
                  wins++;
                }
            }
          else
            {
              /* 稳态：新帧先放到第 44+fill 列，攒够 20 帧就推理，然后整窗左移 20 列，
               * 这样第 44..63 列（正好是这 20 帧）落到第 24..43 列，接在旧数据后面。 */
              se_store_frame(g_pcm, SE_FRAME - SE_WINHOP + g_win_fill);
              g_win_fill++;

              if (g_win_fill >= SE_WINHOP)
                {
                  se_run_window();

                  {
                    int m;

                    for (m = 0; m < SE_MEL; m++)
                      {
                        memmove(&g_melhist[m * SE_FRAME],
                                &g_melhist[m * SE_FRAME + SE_WINHOP],
                                (size_t)(SE_FRAME - SE_WINHOP) * sizeof(float));
                      }
                  }

                  g_win_fill = 0;
                  wins++;
                }
            }

          memmove(g_pcm, &g_pcm[SE_FHOP],
                  (g_pcm_len - SE_FHOP) * sizeof(int16_t));
          g_pcm_len -= SE_FHOP;
        }
    }

  g_voice_hint = saved_hint;
  g_loud_hint  = saved_loud;
  return wins;
}

int sound_event_feed(const int16_t *pcm, size_t nsamples)
{
  return sound_event_feed_ex(pcm, nsamples, 0, 0);
}

/****************************************************************************
 * 主机对拍专用钩子（板端编译不会带 SOUND_EVENT_HOST_TEST，等于不存在）
 ****************************************************************************/

#ifdef SOUND_EVENT_HOST_TEST
/* 最近一次推理用的整窗 mel（已按 (x-mean)/(std+1e-5) 归一化，mel[m*64+t]） */
const float *sound_event_test_window(void)
{
  return g_win;
}

/* 原始 log-mel 滑窗历史（未归一化，mel[m*64+t]） */
const float *sound_event_test_rawmel(void)
{
  return g_melhist;
}

const float *sound_event_test_logit(void)
{
  return g_logit;
}

const float *sound_event_test_softmax(void)
{
  return g_softmax;
}

int sound_event_test_windows(void)
{
  return g_nwindows;
}

/* 跳过前端，直接灌一组 logit 走 softmax + 投票 + 不应期，
 * 用来确定性地验后处理（真实音频很难造出稳定的 3/4 投票）。返回触发的类别。 */
int sound_event_test_inject_logit(const float *logit)
{
  int i;

  if (!g_inited || logit == NULL)
    {
      return -1;
    }

  for (i = 0; i < SE_CLASS; i++)
    {
      g_logit[i] = logit[i];
    }

  se_softmax();
  g_nwindows++;
  return se_postprocess();
}
#endif /* SOUND_EVENT_HOST_TEST */

