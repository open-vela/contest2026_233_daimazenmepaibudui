/****************************************************************************
 * app/hello_app/prompt_ack.c
 *
 * 实现见 prompt_ack.h。设计取舍在那个头文件里，这里只记怎么算的。
 *
 * PCM 口径和整条语音链路一致：16 kHz / 单声道 / s16le 裸数据
 * （MIMO_PCM_RATE 同档，喂给 audio_play_start() 的 frames 是**样本数**，
 * 所以字节数要 / sizeof(int16_t)）。
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "prompt_ack.h"
#include "tts_cache.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 想让提示音变成说人话，就在 PC 上把这两句合成进 ROMFS（见头文件）。
 * 文案在这里只当查表的 key 用，板子自己不出声念它。 */

#define PROMPT_RATE_HZ         16000

/* 语气 A「嗯，我在」：两声降调，暖、长一点 —— 听感是"收到了，在想"。
 * 频率挑在 500~700 Hz —— 老人高频听力最先退化，挑 2 kHz 以上的铃声
 * 他们可能压根听不见。 */
#define PROMPT_ACK_N1_HZ       659   /* E5 */
#define PROMPT_ACK_N1_MS       160
#define PROMPT_ACK_GAP_MS      30
#define PROMPT_ACK_N2_HZ       523   /* C5 */
#define PROMPT_ACK_N2_MS       240

/* 语气 B「没听清」：更低、更短、更急。低频降调听感是"没成，再来一次"，
 * 和 A 一定要能分辨开 —— 两声听成一个意思就白做了。 */
#define PROMPT_MISS_N1_HZ      440   /* A4 */
#define PROMPT_MISS_N1_MS      110
#define PROMPT_MISS_GAP_MS     25
#define PROMPT_MISS_N2_HZ      330   /* E4 */
#define PROMPT_MISS_N2_MS      190

#define PROMPT_MS_TO_N(m)      (PROMPT_RATE_HZ * (m) / 1000)

/* 峰值 7200 ≈ 满量程的 22%。够响又不刺耳，也不会把麦克风旁边的人吓一跳。 */
#define PROMPT_PEAK            7200

/* 两种语气里更长的那一种（语气 A），临时堆按它开 */
#define PROMPT_MAX_SAMPLES     (PROMPT_MS_TO_N(PROMPT_ACK_N1_MS) + \
                                PROMPT_MS_TO_N(PROMPT_ACK_GAP_MS) + \
                                PROMPT_MS_TO_N(PROMPT_ACK_N2_MS))

/* 起振 6 ms / 收尾 8 ms，都是防"咔哒"的（波形从非零值突变到 0 就是咔哒）。 */
#define PROMPT_ATTACK_SAMPLES  (PROMPT_RATE_HZ * 6 / 1000)
#define PROMPT_RELEASE_SAMPLES (PROMPT_RATE_HZ * 8 / 1000)

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 四分之一周期正弦表，0..π/2 共 65 点、幅值 ±32767。
 * 用整表而不是调 sin()：不引 libm，也省得浮点在这条近实时的路上跑。 */
static const int16_t s_qsin[65] =
{
       0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
    6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
   12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
   18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
   23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
   27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
   30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
   32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
   32767,
};

/* 提示音 PCM 走**临时堆**，不放静态缓冲：6880 样本 ≈ 13.4 KB，静态的是
 * BSS、要占 SRAM —— 这块板子 SRAM 吃紧过（play_buf 曾常驻 2.24 MB 把开机
 * 水位挤爆、hello_app 崩溃重启死亡螺旋，见 ai_audio.h 的
 * AUDIO_PLAY_BUFFER_MS 注释）。audio_play_start() 起播时会 memcpy 进它自己
 * 的播放缓冲，所以这段内存起播一返回就还掉，和预缓存那条路同一套做法。
 * 堆里的大块会落到 PSRAM，不跟内核抢 SRAM。 */

/* 提示音此刻在不在响。**不是**"设备在不在播"（那要问 audio_is_playing()）：
 * 真答复出声时设备也在播，但那不算提示音。 */
static volatile bool s_active = false;

/* 当前在响的是哪一种语气（-1 = 没有）。用来分"同一句重复请求"和
 * "换一句话要说"：前者去重，后者把前一声换掉。 */
static volatile int  s_active_kind = -1;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * @brief  一个相位（0..1023 表示一个完整周期）上的正弦值，幅值 ±32767
 */

static int16_t tone_sin(uint32_t phase)
{
  uint32_t quad = (phase >> 8) & 3u;
  int      idx  = (int)((phase & 255u) >> 2);   /* 0..63 */

  switch (quad)
    {
      case 0:  return s_qsin[idx];
      case 1:  return s_qsin[64 - idx];
      case 2:  return (int16_t)(-s_qsin[idx]);
      default: return (int16_t)(-s_qsin[64 - idx]);
    }
}

/**
 * @brief  往 out 里写一个音：起振 → 保持 → 收尾，整段用一条包络乘上去
 *
 * 包络是"线性起振 6ms → 线性衰到 25% → 线性收 8ms 到 0"。
 * 衰到 25% 而不是衰到 0，是为了让音有"身段"：纯三角包络听起来像敲了一下
 * 就没了，老人可能还没反应过来。
 */

static void tone_note(int16_t *out, size_t samples, uint32_t hz)
{
  /* 相位以 1/1024 周期为单位累加（见 tone_sin）。
   * hz * 1024 / 16000 用整数算，误差不到 0.5%（659 → 42.17 取 42）。 */
  uint32_t step = (uint32_t)(((uint64_t)hz * 1024u) / PROMPT_RATE_HZ);
  uint32_t phase = 0;
  size_t   i;

  for (i = 0; i < samples; i++)
    {
      int32_t amp;
      int32_t body_end = (int32_t)samples - (int32_t)PROMPT_RELEASE_SAMPLES;
      int32_t v;

      if (i < PROMPT_ATTACK_SAMPLES)
        {
          amp = (int32_t)PROMPT_PEAK * (int32_t)i / (int32_t)PROMPT_ATTACK_SAMPLES;
        }
      else if (i < (size_t)body_end)
        {
          /* 从 PEAK 线性衰到 PEAK/4 */
          int32_t span = body_end - (int32_t)PROMPT_ATTACK_SAMPLES;
          int32_t gone = (int32_t)i - (int32_t)PROMPT_ATTACK_SAMPLES;

          amp = PROMPT_PEAK - (int32_t)(PROMPT_PEAK * 3 / 4) * gone /
                              (span > 0 ? span : 1);
        }
      else
        {
          int32_t span = (int32_t)PROMPT_RELEASE_SAMPLES;
          int32_t gone = (int32_t)i - body_end;

          amp = (int32_t)(PROMPT_PEAK / 4) * (span - gone) / (span > 0 ? span : 1);
        }

      v = ((int32_t)tone_sin(phase) * amp) / 32767;
      if (v > 32767)
        {
          v = 32767;
        }
      else if (v < -32768)
        {
          v = -32768;
        }

      out[i] = (int16_t)v;
      phase += step;
    }
}

/**
 * @brief  合成整段提示音（两个音 + 中间空档）写进 out，返回总样本数
 *
 * out 至少要 PROMPT_MAX_SAMPLES 个样本。每次起播现算一遍：几千个样本的
 * 整数乘加，微秒级，比省这 13 KB 的常驻内存划算得多。
 */

static size_t tone_build(int16_t *out, prompt_ack_kind_t kind)
{
  uint32_t n1_hz;
  uint32_t n2_hz;
  size_t   n1;
  size_t   gap;
  size_t   n2;

  if (kind == PROMPT_ACK_KIND_MISS)
    {
      n1_hz = PROMPT_MISS_N1_HZ;
      n2_hz = PROMPT_MISS_N2_HZ;
      n1    = PROMPT_MS_TO_N(PROMPT_MISS_N1_MS);
      gap   = PROMPT_MS_TO_N(PROMPT_MISS_GAP_MS);
      n2    = PROMPT_MS_TO_N(PROMPT_MISS_N2_MS);
    }
  else
    {
      n1_hz = PROMPT_ACK_N1_HZ;
      n2_hz = PROMPT_ACK_N2_HZ;
      n1    = PROMPT_MS_TO_N(PROMPT_ACK_N1_MS);
      gap   = PROMPT_MS_TO_N(PROMPT_ACK_GAP_MS);
      n2    = PROMPT_MS_TO_N(PROMPT_ACK_N2_MS);
    }

  tone_note(&out[0], n1, n1_hz);
  memset(&out[n1], 0, gap * sizeof(int16_t));
  tone_note(&out[n1 + gap], n2, n2_hz);

  return n1 + gap + n2;
}

/**
 * @brief  播放完成回调：只清标志，**什么都不做**
 *
 * 不能复用 light_speak 那个 play_complete_callback()：那个会置"这轮出过声"
 * 并发 AI_RESPONSE 让状态机收尾。提示音不是这一轮的答复，它一响完就让
 * 状态机收尾的话，真答复还没回来界面就回到待机了 —— 听感是"叮一声就没
 * 下文"。反过来，真答复自己的播放走它自己的回调，两边不串。
 */

static void prompt_ack_done(void *user_data)
{
  (void)user_data;
  s_active = false;
  s_active_kind = -1;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int prompt_ack_play_kind(audio_context_t *ctx, prompt_ack_kind_t kind)
{
  const char    *text = (kind == PROMPT_ACK_KIND_MISS) ? PROMPT_MISS_TEXT
                                                       : PROMPT_ACK_TEXT;
  const char    *what = (kind == PROMPT_ACK_KIND_MISS) ? "没听清" : "我听见了";
  unsigned char *cached = NULL;
  size_t         cached_len = 0;
  int16_t       *tone;
  size_t         tone_n;
  int            ret;

  if (ctx == NULL)
    {
      return -EINVAL;
    }

  /* 同一种已经在响 → 去重（叠出来的相位不连续，听着像破音）。
   * 不同语气 → **换掉**前一声：后到的那句更具体，留着前一声反而说错话。
   * 真实情形是 ASR 秒败（-ENOENT / 网络立刻回错）时"没听清"要顶掉
   * 还在响的"我听见了" —— 不换就是"它说听见了然后装死"。 */

  if (s_active)
    {
      if ((int)kind == s_active_kind)
        {
          return 0;
        }

      s_active = false;
      s_active_kind = -1;
      audio_play_stop(ctx);
      printf("[提示音] 换一句话（%s），前一声收掉\n", what);
    }

  /* ---- ① 预缓存命中就说人话 ---- */

  if (tts_cache_load(text, &cached, &cached_len) == 0)
    {
      s_active = true;
      s_active_kind = (int)kind;
      ret = audio_play_start(ctx, (const int16_t *)cached,
                             cached_len / sizeof(int16_t),
                             prompt_ack_done, NULL);

      /* 起播已经 memcpy 进播放缓冲了，这里可以立刻还掉（同 light_speak）。 */
      free(cached);

      if (ret < 0)
        {
          s_active = false;
          s_active_kind = -1;
          return ret;
        }

      printf("[提示音] 起播（%s，预缓存语音 %zu 字节 ≈ %zu ms）\n",
             what, cached_len, cached_len * 1000 / (PROMPT_RATE_HZ * 2));
      return 0;
    }

  /* ---- ② 没预生成：程序合成的短提示音 ---- */

  tone = (int16_t *)malloc(PROMPT_MAX_SAMPLES * sizeof(int16_t));
  if (tone == NULL)
    {
      /* 要不到 13 KB 就没有这一声 —— 不出声也比不出话强，本轮照走。 */
      return -ENOMEM;
    }

  tone_n = tone_build(tone, kind);

  s_active = true;
  s_active_kind = (int)kind;
  ret = audio_play_start(ctx, tone, tone_n, prompt_ack_done, NULL);

  /* 起播已 memcpy 进播放缓冲，还掉（同上面预缓存那条路）。 */
  free(tone);

  if (ret < 0)
    {
      s_active = false;
      s_active_kind = -1;

      /* -EBUSY = 真答复已经先出声了。这是**正常**竞态，不是错误。 */
      return ret;
    }

  printf("[提示音] 起播（%s，合成提示音 %zu 样本 ≈ %zu ms，没预缓存「%s」）\n",
         what, tone_n, tone_n * 1000 / PROMPT_RATE_HZ, text);
  return 0;
}

int prompt_ack_play(audio_context_t *ctx)
{
  return prompt_ack_play_kind(ctx, PROMPT_ACK_KIND_ACK);
}

void prompt_ack_stop(audio_context_t *ctx)
{
  if (ctx == NULL || !s_active)
    {
      return;
    }

  /* audio_play_stop() 是 ai_audio 已经验过的收摊路：置 play_stop → 有界
   * join 播放线程 → playing=false。被打断的播放不会触发完成回调，所以
   * s_active 必须在这里自己清。 */

  s_active = false;
  s_active_kind = -1;
  audio_play_stop(ctx);

  printf("[提示音] 真答复要出声，提示音收掉\n");
}

bool prompt_ack_busy(void)
{
  return s_active;
}
