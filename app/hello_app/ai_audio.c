/****************************************************************************
 * AI Audio Module Implementation
 * 智爱陪伴 - AI老人陪伴守护终端
 * 音频模块 - 麦克风录音与音频播放
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/audio/audio.h>     /* AUDIOIOC_* / struct audio_caps_desc_s */

/* nuttx/audio/audio.h 里也有个 AUDIO_VOLUME_MAX，但那是驱动侧的 0..1000；
 * ai_audio.h 里的同名宏是本模块对外的 0..100。两个头文件都改不得，
 * 所以先 include 平台头、把名字让回本模块（下面的 ai_audio.h 会重新定义它）。 */

#undef AUDIO_VOLUME_MAX
#undef AUDIO_VOLUME_MIN

#include "ai_audio.h"
#include "sf32lb52_audio_in.h"     /* 板级录音封装 audio_in_start/read/stop */

/* 板级录音封装还有第三个入口：**放弃**当前会话 —— 只清理本层自己的状态并关掉
 * 本层那个 fd，绝不发 AUDIOIOC_STOP。板级头里也会有这条声明，这里再声明一次
 * 只是不让本文件绑在它的落地时间上（两处签名一致，不冲突）。
 * 用法只有一处：录音线程收尾时，这一代已经**不是我们的**了，见那里。 */

int audio_in_abandon(void);

/* 任务四：把每一帧 PCM **旁路**一份给声音门控（实现和门限在
 * ai_sound_detect.c，头文件声明见 ai_sound_detect.h）。
 * 这里只声明、不 include 那个头，和上面 audio_in_abandon() 同一考虑：
 * 本文件不绑死对方的落地时间，两处签名一致即可。
 * 契约：**非阻塞**，只往有界环形缓冲 memcpy，队列满丢最旧的窗；
 * 绝不能在录音线程里等门控线程 —— 等就是丢麦克风数据。 */

void sound_detect_pcm_tap(const int16_t *pcm, size_t nsamples);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <pthread.h>
#include <math.h>
#include <time.h>
#include <syslog.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 播放设备路径。
 * 本板录音/播放是**同一个节点**（驱动 audio_register("audio0")，路径带
 * audio/ 子目录），而且是**半双工**：AUDIOIOC_STOP 会同时停掉两条通路。
 * 录音不直接开设备，走板级封装 sf32lb52_audio_in（它自带路径）。 */

#define AUDIO_PLAY_DEVICE      "/dev/audio/audio0"

/* 播放分块：250 ms 一块。驱动里的 write() 是**同步**的（阻塞到这块放完），
 * 分块后 audio_play_stop() 最多等一块 + 驱动的等待余量就能生效。
 * 播放缓冲本身仍由 ctx->play_buf 承担（AUDIO_PLAY_BUFFER_MS）。
 *
 * 块大小是"听感 vs 打断跟手"的取舍：块边界要 DMAStop 再重新起传输，而驱动里
 * 那块 TX DMA 是 DMA_CIRCULAR（见 board 的 sf32lb52_audio.c），边界越密越容易
 * 听到"啪"。从 100 ms 抬到 250 ms，边界从 10 次/秒降到 4 次/秒。
 * 代价是打断变慢，数字见 audio_play_stop() 的注释；要更跟手就改回 100。 */

#define AUDIO_PLAY_CHUNK_MS    250

/* 每个分块首尾各做多长的淡入/淡出（ms）。这是消除块边界爆音的主要手段，
 * 原理和代价见 audio_apply_chunk_fade()。 */

#define AUDIO_PLAY_FADE_MS     3

/* 驱动侧音量值域 0..1000（nuttx/audio/audio.h 的 AUDIO_VOLUME_MAX，
 * 也就是 AUDIOIOC_CONFIGURE + AUDIO_TYPE_FEATURE + AUDIO_FU_VOLUME 的取值），
 * 本模块对外是 0..100（ai_audio.h 的 AUDIO_VOLUME_MAX），换算时乘 10。
 * 这里写常量而不是引平台宏，免得上面那两个同名宏再串味。 */

#define AUDIO_HW_VOLUME_MAX    1000

/* 录音线程能**连续容忍**多少次"读超时"（下层一次等待满 5 秒都没等到数据）。
 *
 * 为什么要有这个容忍：驱动把"这一次没等到数据"和"会话结束"分开返回了
 * （超时 = 负值且 errno = ETIMEDOUT，会话结束 = 返回 0），而真机上大约 10% 的读
 * 会超时（驱动现场 irq=448 read=479 timeout=47）。原来两者都返回 0、都被上层
 * 当成"会话死了"，于是一次抖动就能拆掉整场录音 —— 永远攒不齐一句话。
 *
 * 取 5 的理由（建议区间 5~10 的下限）：
 *   - 一次超时要等 5 秒（驱动那 50 片 × 100ms），所以 5 次 = 最坏 25 秒才告诉
 *     上层"会话死了"；这是"容忍抖动"要付的代价，取下限就是为了把它压到最小；
 *   - 单次读独立超时的概率按 10% 算，连续 5 次都超时约 1e-5（0.1 的 5 次方），
 *     不会把偶发抖动误判成死；真死则最多 25 秒被发现；
 *   - 真机那种超时"粘住"（连着若干次健康读之后卡住、再也不给数据）的可能性很大，
 *     粘住时容忍次数越多聋得越久，所以不往 10 那头靠；
 *   - 上层不会替这里兜底：上层只看会话标志（录音不活跃就重开），"会话看着还在、
 *     设备其实已经不给数据"那种形状它看不见（2026-09-15 试过在应用层加"数据流
 *     像死了"的判据，结果那道判据把重开永久挡死）。所以"连续超时到什么时候算
 *     会话死了"只能由本宏定，别再指望别人。 */

#define AUDIO_RECORD_TIMEOUT_TOLERANCE  5

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void *audio_record_thread(void *arg);
static void *audio_play_thread(void *arg);
static uint32_t audio_now_ms(void);
static uint32_t audio_now_ms_nonzero(void);
static int audio_since_ms(uint32_t since);
static bool audio_record_thread_is_current(const audio_context_t *ctx);
static uint32_t audio_calc_frame_energy(const int16_t *data, size_t frames);
static size_t audio_play_chunk_bytes(const audio_context_t *ctx);
static void audio_apply_chunk_fade(audio_context_t *ctx, size_t frames);
static int audio_open_play_device(audio_context_t *ctx);
static void audio_close_play_device(int fd);
static int audio_apply_volume(int fd, uint8_t volume);
static int audio_configure_output(int fd, const audio_context_t *ctx);
static int audio_hw_set_volume(const audio_context_t *ctx, uint8_t volume);
static void audio_prepare_output(audio_context_t *ctx);
static void audio_resume_record(audio_context_t *ctx);
static void audio_reap_play_thread(audio_context_t *ctx, const char *who);
static int audio_play_begin(audio_context_t *ctx, size_t frames,
                            audio_play_complete_cb_t callback,
                            void *user_data);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 状态名称表 */
static const char *g_state_names[] =
{
  [AUDIO_STATE_UNINIT]     = "UNINIT",
  [AUDIO_STATE_IDLE]       = "IDLE",
  [AUDIO_STATE_RECORDING]  = "RECORDING",
  [AUDIO_STATE_PLAYING]    = "PLAYING",
  [AUDIO_STATE_BOTH]       = "BOTH"
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * @brief  单调时钟的毫秒数（本模块记时间戳用）
 *
 * 为什么不用 CLOCK_REALTIME：它会被 NTP 校时/手动改时间跳走，算出来的间隔
 * 可能是负的、也可能一次跳几小时。算"过了多久"只能用 CLOCK_MONOTONIC
 * （ai_companion_main.c 的 main_now_ms 也是这么取的）。
 *
 * 返回值只有"刚上电那一毫秒"才可能是 0，而本模块拿 0 当
 * "还没记过时间戳"的哨兵，所以写时间戳的地方一律用 audio_now_ms_nonzero()。
 */

static uint32_t audio_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static uint32_t audio_now_ms_nonzero(void)
{
  uint32_t now = audio_now_ms();

  /* 0 是"从没记过"的哨兵，真拿到 0 就退一格用 1：代价是那一毫秒少算 1ms，
   * 好过让"刚启动"和"从没启动"在访问器里长得一样。 */

  return (now != 0) ? now : 1;
}

/**
 * @brief  "从 since 到现在过了多少毫秒"；since 是 0（哨兵）时返回 -1
 *
 * 有符号差值算的间隔：uint32 绕一圈（约 49 天）也不会算反，不用管回绕。
 * 取时间戳和取时钟之间有一瞬间的差，所以理论上能算出负数（时间戳刚被别的
 * 线程刷新、这次读到的却是刷新前的值），夹成 0 —— 那种情况下"刚好有数据来了"
 * 才是实话，返个负数会让上层以为接口坏了。
 */

static int audio_since_ms(uint32_t since)
{
  int32_t elapsed;

  if (since == 0)
    {
      return -1;
    }

  elapsed = (int32_t)(audio_now_ms() - since);
  return (elapsed > 0) ? (int)elapsed : 0;
}

/**
 * @brief  本线程还是不是"当前这一代录音会话"的线程
 *
 * audio_record_start() 起新一代时会覆盖 ctx->record_thread。上一次的线程仍然
 * 可能在"上层已经起了新一代"之后才醒过来/才收尾：
 *   - 回收是在**录音线程自己里**被调的那种玩法（见 audio_reap_record_thread
 *     里 pthread_equal 那一支：不能 join 自己，TCB 留给下次回收）；
 *   - 以及"线程自己判定被抢走、自己退出"的那条路（stolen）。
 * 这种"上一代的线程"再去收尾就是踩别人：
 *   - audio_in_stop() / audio_in_abandon() 动到的都是**全局那一份**录音会话
 *     （前者发设备级 STOP、后者关掉那个 fd，设备/ fd 都只有一份）；
 *   - recording / record_exited / record_died 会被写成新一代的状态，
 *     于是"线程在跑、标志说没在录"，监听守护据此反复重开，永远好不了。
 * 判据就是"当前会话记着的那个线程还是不是我"；正常情况下它一定是 true
 * （线程本来就是 audio_record_start() 起的那个）。
 * NuttX 的 pthread_create() 是先写好这个输出参数、再激活新线程的，所以新线程
 * 第一行就能看到自己的 tid，不用担心"起得太快还没写进去"。
 */

static bool audio_record_thread_is_current(const audio_context_t *ctx)
{
  return pthread_equal(ctx->record_thread, pthread_self()) != 0;
}

/**
 * @brief  计算单帧能量
 */

static uint32_t audio_calc_frame_energy(const int16_t *data, size_t frames)
{
  uint64_t sum = 0;

  if (data == NULL || frames == 0)
    {
      return 0;                       /* 不做除零 */
    }

  for (size_t i = 0; i < frames; i++)
    {
      /* 单个采样峰值 32768^2 = 2^30，一帧 320 个采样也才 ~2^38，
       * 用 uint64 累加不会溢出；返回值是平均能量，最大 2^30，uint32 装得下。 */

      sum += (int64_t)data[i] * data[i];
    }

  return (uint32_t)(sum / frames);
}

/**
 * @brief  一个播放分块的字节数
 *
 * 播放线程按它切块，淡入淡出也按它算块边界 —— 只能有**一份**算法，
 * 否则淡化的位置和实际 write() 的分块位置对不上，等于白做。
 */

static size_t audio_play_chunk_bytes(const audio_context_t *ctx)
{
  return (size_t)ctx->config.sample_rate * AUDIO_PLAY_CHUNK_MS / 1000 *
         (size_t)ctx->config.channels * sizeof(int16_t);
}

/**
 * @brief  给每个分块的首尾各加一段淡入/淡出（就地改 ctx->play_buf）
 *
 * 为什么加：驱动那边 HAL_AUDCODEC_Transmit_DMA() 把 TX DMA 设成 DMA_CIRCULAR
 * （见 board/contest_board/src/sf32lb52_audio.c 的注释），这一块放完的同时
 * 硬件会**立刻从块首再播一遍**；要等 DMA 完成中断把 write() 唤醒、走到
 * HAL_AUDCODEC_DMAStop() 才停得下来。所以每个块边界上，波形都是
 * "块尾 → 块首"来回跳两下 —— 块尾和块首的幅度差多少，就是多响的一声"啪"。
 * 每块 100 ms 时一秒十声，就是同事听到的"每个字都有爆音"。
 *
 * 怎么解决：把每块的头尾各压成 AUDIO_PLAY_FADE_MS 的斜坡，让块尾和块首
 * 都落在**静音附近**。这样一来重播的那几个采样是"从 0 慢慢起来"的，
 * 停 DMA 那一下也是"停在 0 附近"，两边都没有跳变。驱动侧另外做了配套：
 * 在 DMA 完成中断里就把 DMA 掐掉（sf32lb52_audio_tx_freeze），把重播长度
 * 压到一个 32bit 字以内，斜坡盖得住。
 *
 * 代价：每块首尾共 2 × AUDIO_PLAY_FADE_MS = 6 ms 的样本被压低，250 ms 一块
 * 算下来只占 2.4%，是听感上基本听不出来的轻微调幅；不改时长、不改音高、
 * 不多占内存（就地改本模块自己的播放缓冲，不碰调用者的数据）。
 */

static void audio_apply_chunk_fade(audio_context_t *ctx, size_t frames)
{
  size_t channels = (size_t)ctx->config.channels;
  size_t frame_bytes = channels * sizeof(int16_t);
  size_t chunk_frames = audio_play_chunk_bytes(ctx) / frame_bytes;
  size_t fade_frames = (size_t)ctx->config.sample_rate * AUDIO_PLAY_FADE_MS / 1000;
  size_t begin;

  if (chunk_frames == 0 || fade_frames == 0)
    {
      return;
    }

  for (begin = 0; begin < frames; begin += chunk_frames)
    {
      size_t len = frames - begin;     /* 最后一块可能不足一块 */
      size_t fade = fade_frames;
      size_t i;
      size_t c;

      if (len > chunk_frames)
        {
          len = chunk_frames;
        }

      /* 块比两段斜坡还短（尾部残块、很短的提示音）：两侧各分一半，
       * 保证两段不重叠，也不会除零。 */

      if (fade * 2 > len)
        {
          fade = len / 2;
        }

      if (fade == 0)
        {
          continue;
        }

      for (i = 0; i < fade; i++)
        {
          size_t f_in  = begin + i;                 /* 从块首往后数 */
          size_t f_out = begin + len - 1 - i;       /* 从块尾往前数 */
          int32_t g = (int32_t)i;                   /* 0 → 1 的线性斜坡 */

          for (c = 0; c < channels; c++)
            {
              ctx->play_buf[f_in * channels + c] =
                (int16_t)((int32_t)ctx->play_buf[f_in * channels + c] *
                          g / (int32_t)fade);
              ctx->play_buf[f_out * channels + c] =
                (int16_t)((int32_t)ctx->play_buf[f_out * channels + c] *
                          g / (int32_t)fade);
            }
        }
    }
}

/**
 * @brief  把 0..100 的软件音量换算成驱动的 0..1000 下发
 * @param  fd: 已经 open 的音频设备 fd（必须在本线程里 open 的）
 * @param  volume: 音量 0-100
 * @return 0成功, 负值失败
 */

static int audio_apply_volume(int fd, uint8_t volume)
{
  struct audio_caps_desc_s capdesc;

  /* 驱动只认这个标准接口：AUDIOIOC_CONFIGURE + AUDIO_TYPE_FEATURE +
   * AUDIO_FU_VOLUME，值域 0..AUDIO_VOLUME_MAX(1000)。
   * NuttX 的 audio 上层**没有**实现 AUDIOIOC_SETVOLUME（发下去是 -ENOTTY），
   * 板级 alarm 模块设音量走的也是这一套。 */

  memset(&capdesc, 0, sizeof(capdesc));
  capdesc.caps.ac_len            = sizeof(struct audio_caps_s);
  capdesc.caps.ac_type           = AUDIO_TYPE_FEATURE;
  capdesc.caps.ac_format.hw      = AUDIO_FU_VOLUME;
  capdesc.caps.ac_controls.hw[0] = (uint16_t)volume * AUDIO_HW_VOLUME_MAX / 100;

  if (ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&capdesc) < 0)
    {
      int errcode = errno;

      AUDIO_DEBUG("设置硬件音量失败: %d", errcode);
      return -errcode;
    }

  return OK;
}

/**
 * @brief  把一个刚 open 的 fd 配成"16k/单声道 输出"方向
 * @return 0成功, 负值失败
 *
 * 单独抽出来是因为它不光建播放会话要用，**设音量也必须先走一遍**：
 * 驱动是按"当前方向"决定 AUDIO_FU_VOLUME 改的是 DAC 音量还是麦克风增益的
 * （见 sf32lb52_audio_configure），上一次会话如果是录音（priv->playback 还是
 * false），不标一次 OUTPUT 音量就下到 ADC 增益上去了。
 */

static int audio_configure_output(int fd, const audio_context_t *ctx)
{
  struct audio_caps_desc_s capdesc;

  memset(&capdesc, 0, sizeof(capdesc));
  capdesc.caps.ac_len            = sizeof(struct audio_caps_s);
  capdesc.caps.ac_type           = AUDIO_TYPE_OUTPUT;
  capdesc.caps.ac_channels       = (uint8_t)ctx->config.channels;
  capdesc.caps.ac_controls.hw[0] = (uint16_t)ctx->config.sample_rate;
  capdesc.caps.ac_controls.b[2]  = AUDIO_DEFAULT_BITS_PER_SAMPLE;

  if (ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&capdesc) < 0)
    {
      int errcode = errno;

      AUDIO_DEBUG("播放 AUDIOIOC_CONFIGURE 失败: %d", errcode);
      return -errcode;
    }

  return OK;
}

/**
 * @brief  临时开一个 fd 把音量下到硬件
 * @return 0成功, 负值失败
 *
 * 不碰播放线程的 fd：NuttX 的 fd 属于 task group，只能在本任务里 open/close。
 * 设备没在跑时 close() 会走驱动的 shutdown 路径，那条路径已修成
 * "未 running 就直接返回"的最小化版本（见 docs/audio_driver_usage.md 第 8 节）。
 */

static int audio_hw_set_volume(const audio_context_t *ctx, uint8_t volume)
{
  int fd = open(AUDIO_PLAY_DEVICE, O_WRONLY);
  int ret;

  if (fd < 0)
    {
      int errcode = errno;

      AUDIO_DEBUG("打开 %s 失败: %d", AUDIO_PLAY_DEVICE, errcode);
      return -errcode;
    }

  /* 先标方向（否则音量可能被下到麦克风增益上），再下发 */

  ret = audio_configure_output(fd, ctx);
  if (ret == OK)
    {
      ret = audio_apply_volume(fd, volume);
    }

  close(fd);
  return ret;
}

/**
 * @brief  打开播放设备（**只能在播放线程里调用**）
 * @return 成功返回 fd, 失败返回负 errno
 *
 * 顺序照 app/audio_test 与板级 alarm 模块真机验证过的那套：
 * open -> CONFIGURE(AUDIO_TYPE_OUTPUT) -> CONFIGURE(AUDIO_FU_VOLUME) -> START。
 *
 * 这里的失败一律 `printf` 一行（**不能只走 AUDIO_DEBUG**）：本构建没开
 * CONFIG_DEBUG_AI_AUDIO，那个宏整体编译成空，于是"START 被驱动 -EBUSY 拒掉"
 * 这种"一声都没出"的事在串口上一点痕迹都没有，上层还当它放完了。
 * 每次失败播放只会有这一行（下面播放线程里最多再有一行），不会刷屏。
 */

static int audio_open_play_device(audio_context_t *ctx)
{
  int fd;
  int ret;

  fd = open(AUDIO_PLAY_DEVICE, O_WRONLY);
  if (fd < 0)
    {
      int errcode = errno;

      printf("[AUDIO] 播放没出声：打不开 %s (%d)\n", AUDIO_PLAY_DEVICE, errcode);
      return -errcode;
    }

  /* 配置失败就把它自己的错误码透出去（原来是笼统的 -EIO，上层看不出是
   * 参数/驱动哪一步坏的）。AUDIOIOC_CONFIGURE 那一步失败时 errno 已经被
   * audio_configure_output() 翻成了负 errno。 */

  ret = audio_configure_output(fd, ctx);
  if (ret != OK)
    {
      close(fd);
      printf("[AUDIO] 播放没出声：配输出方向失败 (%d)\n", ret);
      return ret;
    }

  /* 音量下发失败是非致命的（驱动自己有一份默认音量），但"提示音听不见"
   * 也可能是这个原因，所以照样留一行。 */

  ret = audio_apply_volume(fd, ctx->config.volume);
  if (ret != OK)
    {
      printf("[AUDIO] 播放音量下发失败 (%d)：这段可能偏小甚至听不见\n", ret);
    }

  if (ioctl(fd, AUDIOIOC_START, 0) < 0)
    {
      int errcode = errno;

      /* 这就是"半双工被对方占着"的落点：驱动在方向冲突时（录音通路的模拟级
       * 还开着、本次要起播放）返回 -EBUSY，一声都不会出。 */

      printf("[AUDIO] 播放没出声：AUDIOIOC_START 被拒 (%d)%s\n", errcode,
             (errcode == EBUSY) ? "：设备被另一方向的会话占着" : "");
      close(fd);
      return -errcode;
    }

  AUDIO_DEBUG("播放设备已就绪 (fd=%d)", fd);
  return fd;
}

/**
 * @brief  停止并关闭播放设备（**只能在持有该 fd 的播放线程里调用**）
 */

static void audio_close_play_device(int fd)
{
  if (fd < 0)
    {
      return;
    }

  /* 顺序和录音一样：先 AUDIOIOC_STOP 再 close。
   * 注意这个 STOP 会同时停掉录音通路（半双工），所以它必须是播放的最后一步。 */

  ioctl(fd, AUDIOIOC_STOP, 0);
  close(fd);
}

/**
 * @brief  半双工准备：要出声了，先把正在录的停下来
 *
 * 本板 AUDIOIOC_STOP 会同时停掉录放两条通路，录和放必须串行。
 * 麻烦的是 ai_companion 的录音是"开机起一个常开监听线程一直录"的模式
 * （ai_companion_main.c 只在启动时调一次 audio_record_start，之后靠录音线程里的
 * VAD 回调推状态机）。播放如果只是把它停掉就不管，播完就再也没人开麦了，
 * 所以这里记一个标志，播完由播放线程按原配置把录音恢复起来。
 * 想让上层自己管录音（比如在 sm_listening_enter 里重启监听），
 * 把播放线程末尾那段"恢复录音"删掉即可。
 */

static void audio_prepare_output(audio_context_t *ctx)
{
  if (!ctx->recording)
    {
      ctx->record_resume_on_play_end = false;
      return;
    }

  AUDIO_DEBUG("正在录音，先停止录音（半双工），播完再恢复");

  ctx->record_resume_on_play_end = true;
  audio_record_stop(ctx);
}

/**
 * @brief  按原配置把录音恢复起来（只为播放让路而停的才恢复）
 *
 * 必须在 ctx->playing 已经是 false 的时候调，否则 audio_record_start() 的
 * 半双工逻辑会反过来再把播放停一次。
 */

static void audio_resume_record(audio_context_t *ctx)
{
  audio_record_config_t cfg;

  if (!ctx->record_resume_on_play_end)
    {
      return;
    }

  ctx->record_resume_on_play_end = false;
  cfg = ctx->record_cfg;               /* 用副本：audio_record_start 会写回 record_cfg */

  if (audio_record_start(ctx, &cfg) < 0)
    {
      AUDIO_DEBUG("恢复录音失败");
    }
}

/**
 * @brief  录音线程
 */

static void *audio_record_thread(void *arg)
{
  audio_context_t *ctx = (audio_context_t *)arg;
  size_t frame_bytes = ctx->config.channels * sizeof(int16_t);
  size_t frames_per_read;
  size_t want;
  ssize_t nbytes;
  int read_errno = 0;          /* 这一次 audio_in_read() 之后 errno 的快照 */
  bool stale = false;          /* 醒过来才发现自己已经被新一代会话接管 */

  /* 这一代是怎么死的（两个都是"不是本层让它停的"那一类，本层让停的都不置）。
   * 必须分成两个量：死因决定了**线程末尾怎么把设备还回去**，而这两种死因的
   * 正确收尾正好相反（abandon vs stop，理由见发现死亡那一刻和线程末尾两处
   * 注释）。在发现死亡的那一拍就定下来，之后不再读任何会变的标志。 */

  bool stolen = false;         /* 被别人抢走：read 返回 0/EOF（对家发了设备级 STOP） */
  bool starved = false;        /* 自己断粮：连续读超时到上限，或其它真错误 */

  AUDIO_DEBUG("录音线程启动");

  /* 每次读一帧（config.frame_ms 毫秒）的数据。
   * record_buf 在 audio_init 里就是按一帧分配的，这里再夹一次上限防越界。 */

  frames_per_read = ctx->config.sample_rate * ctx->config.frame_ms / 1000;
  want = frames_per_read * frame_bytes;

  if (want == 0 || want > ctx->record_buf_size)
    {
      want = ctx->record_buf_size;
    }

  while (!ctx->record_stop && ctx->recording && want > 0)
    {
      /* 真录音：走板级封装 audio_in_read()（设备 open/CONFIGURE/START 都在
       * audio_in_start() 里，由 audio_record_start() 在本线程外先调好）。
       * 它阻塞到读满 want 字节：被 AUDIOIOC_STOP 打断返回 0（EOF，会话结束），
       * 下层等待超时则返回负值（errno = ETIMEDOUT，只是这一帧没数据）。 */

      /* 记下"这一次 read 是什么时候开始等的"：它答的是"线程此刻在不在等设备、
       * 等了多久"，只进 diag 快照（见 ai_audio.h 的 audio_record_wait_ms）——
       * 不做判据，理由写在 ai_audio.h 那边。
       * 0 是"没在等"的哨兵，所以用非零的那个取值。 */

      ctx->record_read_start_ms = audio_now_ms_nonzero();

      /* 先清 errno 再读：驱动现在会区分"这次读超时"（负值 + errno = ETIMEDOUT）
       * 和"会话结束"（返回 0），而那个负值经 POSIX read() 那层会被翻成
       * -1 + errno，所以判"是不是超时"只能看 errno —— 不清 0 的话会读到上一次
       * 调用留下的旧值，把别的错误当成超时、白容忍好几帧。 */

      errno = 0;
      nbytes = audio_in_read(ctx->record_buf, want);
      read_errno = errno;

      ctx->record_read_start_ms = 0;   /* read 回来了，不再"在等" */

      /* 醒来第一件事：确认自己还是"当前这一代"的线程。不是的话（上一代的线程
       * 这会儿才从设备调用里醒过来，而上层已经起了新一代，见
       * audio_record_thread_is_current 的说明），这批数据**绝不能**处理：
       * 它和新一代用的是同一块 record_buf，处理一遍等于把同一段音频喂两次
       * VAD/KWS/ASR。本次唯一的正确动作就是退出去。 */

      if (!audio_record_thread_is_current(ctx))
        {
          stale = true;
          break;
        }

      /* 返回值怎么读（处理方式都在下面）：
       *   > 0                      → 真读到了这么多字节，走数据通路；
       *   = 0                      → EOF：被 STOP 打断 / 会话换代（别人把设备拿走了）。
       *                              维持原行为（按 record_stop 分岔，跳出循环）；
       *   < 0 且 errno = ENODEV    → 驱动侧说"设备没在跑"（**不是**被抢走，2026-09-20
       *                              晚从 EOF 里拆出来的）：这一支收尾必须 STOP + close，
       *                              理由见下面 -ENODEV 那一支的注释；
       *   < 0 且 errno = ETIMEDOUT → **只是这一次没等到数据**（下层那一次等待
       *                              5 秒超时）。跳过这一帧接着读，连续超到
       *                              AUDIO_RECORD_TIMEOUT_TOLERANCE 次才当会话死；
       *   < 0 其它                 → 真错误（未 start 的 -EINVAL、fd 失效等），
       *                              按"会话死了"处理。
       * 后两种都**不能**当成"这次没数据、再读一次就有"，区别在于：超时只跳一帧、
       * 错误要收摊（设备已经不能用了）。 */

      if (nbytes > 0)
        {
          size_t frames_read = (size_t)nbytes / frame_bytes;
          size_t samples_read = frames_read * ctx->config.channels;

          /* 数据流活跃度先记账，再谈别的：
           *   - 读到数据是"通路还活着"的**唯一**实证（那几个标志都只是软件
           *     状态，设备不出数据时它们照样全是健康的）；
           *   - 必须记在 VAD 和数据回调**之前**。后面那些回调可能跑很久
           *     （ASR/大模型同步跑在 VAD 回调里，几十秒），记晚了就变成
           *     "这几十秒没数据流"被算进去，判据自己就误报了。 */

          ctx->record_last_data_ms = audio_now_ms_nonzero();

          /* 上一帧之后又读到数据了：这就是"读已经恢复"的唯一凭据，打一行
           * （正常录音时一次都不会打 —— 只有超时那一段之后的第一帧会走到这里）。
           * 节流配对见下面那段"连续超时容忍"的注释。 */

          if (ctx->record_empty_reads > 0)
            {
              printf("[录音] 读已恢复：上一帧之后又有数据了"
                     "（清掉 %u 次连续空读）\n",
                     (unsigned)ctx->record_empty_reads);
            }

          ctx->record_empty_reads = 0;
          ctx->record_last_result = 0;

          /* 任务四：把这一帧原样旁路给声音门控（人声/非人声）。
           * 放在 VAD 和 data_callback **之前**：门控要的是最原始的麦克风数据，
           * 而且这是录音线程里离 read() 最近的一处。它不阻塞、不做判断，
           * 只挪一块内存进有界队列；判断全在门控线程里做。 */

          sound_detect_pcm_tap(ctx->record_buf, samples_read);

          /* VAD检测 */

          if (ctx->vad_enabled)
            {
              uint32_t energy = audio_calc_frame_energy(
                ctx->record_buf, samples_read);

              if (energy > ctx->vad_energy_threshold)
                {
                  /* 检测到语音 */

                  ctx->vad_silence_frames = 0;
                  ctx->vad_speech_frames++;

                  uint32_t min_speech_frames =
                    (ctx->record_cfg.min_speech_ms +
                     ctx->config.frame_ms - 1) / ctx->config.frame_ms;

                  if (!ctx->vad_speech_active &&
                      ctx->vad_speech_frames >= min_speech_frames)
                    {
                      AUDIO_DEBUG("VAD: 检测到语音开始 (能量=%lu)",
                                  (unsigned long)energy);
                      ctx->vad_speech_active = true;

                      if (ctx->vad_callback)
                        {
                          ctx->vad_callback(true, ctx->vad_user_data);
                        }
                    }
                }
              else
                {
                  /* 静音 */

                  ctx->vad_speech_frames = 0;
                  ctx->vad_silence_frames++;

                  uint32_t silence_frames =
                    (ctx->record_cfg.silence_timeout_ms +
                     ctx->config.frame_ms - 1) / ctx->config.frame_ms;

                  if (ctx->vad_speech_active &&
                      ctx->vad_silence_frames >= silence_frames)
                    {
                      AUDIO_DEBUG("VAD: 语音结束 (静音超时)");
                      ctx->vad_speech_active = false;

                      if (ctx->vad_callback)
                        {
                          ctx->vad_callback(false, ctx->vad_user_data);
                        }
                    }
                }
            }

          /* 这里不再 usleep：audio_in_read() 本身就要等 DMA 采满这一帧
           * （16k/16bit 下 640 字节 = 20ms），节奏已经由设备定住；
           * 再 sleep 一个 frame_ms 只会让循环变成 40ms 一次、白丢一半音频。 */

          /* 调用数据回调（真数据：ASR 累积、声音检测都吃这里） */

          if (ctx->record_cfg.data_callback)
            {
              ctx->record_cfg.data_callback(ctx->record_buf,
                                            frames_read,
                                            ctx->record_cfg.user_data);
            }
        }
      else
        {
          /* 读了却一个字节都没拿到。这里必须先分清是哪一种"没拿到" ——
           * 上层的收尾判据（record_died）完全建立在"会话真的没了"之上，
           * 所以三种情况不能混在一起：
           *
           *   1) **下层那一次等待超时**（驱动返回 -ETIMEDOUT，经 POSIX read() 那层
           *      变成 -1 且 errno = ETIMEDOUT）：会话还活着，只是这一帧没等到数据。
           *      容忍 —— 跳过这一帧、接着读下一帧，只有**连续**超时超过
           *      AUDIO_RECORD_TIMEOUT_TOLERANCE 次才当会话死了收摊。
           *      真机上大约 10% 的读会落到这一类（驱动现场 irq=448 read=479
           *      timeout=47），原来它和"会话结束"都返回 0、被一律当成会话死 ——
           *      于是每 5 秒拆一次设备重录，一断一续，永远攒不齐一句完整的话，
           *      VAD 不触发、ASR 不开始，界面上一个字都不动。
           *   2) **read 返回 0（EOF）**：audio_record_stop() 让我们停（正常收尾），
           *      或者驱动那边通路被停 / 设备没在跑 / 会话换代（异常中断）。
           *      维持原来的行为：按 record_stop 分岔，该收尾的收尾、该标记
           *      record_died 的标记，然后跳出循环。
           *   3) **其它负值**：未 start 的 -EINVAL、fd 失效等真错误，
           *      同样按"会话死了"处理（和原来一致）。
           *
           * "是谁让这次录音结束的"仍然要分清楚 —— 上层（ai_companion 的监听
           * 守护）正是靠这一点区分"正常收尾"和"会话死了"：
           *
           *   - ctx->record_stop 已经置位：audio_record_stop() 让我们停的，
           *     正常收尾。放音前的半双工让路（audio_prepare_output）走的也是
           *     这条路，它播完会自己把录音恢复起来，不需要谁去救。
           *   - record_stop 还是假：**没人**要求停，是驱动 AUDIOIOC_STOP 了
           *     通路 / 连续读超时到了上限 / 设备被别的会话（半双工）抢走了。
           *     这一代录音到此为止，而且不会有 VAD 回调来收尾 —— 上层要是不知道，
           *     状态机就会一直停在"正在听"直到自己超时，用户体感就是"卡死了"。
           *     所以置上 record_died，让上层下一拍（100ms）就能重开。
           *     同一个判据还要**顺手把死因定下来**（stolen / starved），因为
           *     死因决定了线程末尾怎么把设备还回去，而两种死因的正确收尾正好
           *     相反（被抢走 → 只 close；自己断粮 → 先 STOP 再 close，
           *     理由见线程末尾那段）。在这里定、不在收尾处再读一次 record_stop：
           *     中间那几微秒里上层完全可能正好调 audio_record_stop()，
           *     那样"这一次是异常死亡"就会被读成"本层在停"，收尾方式正好反了。
           *
           * 注意这里**不能**去补一次 vad_callback(false) 冒充"语音结束"：
           * VAD 报 false 的意思是"人说完了一句完整的话，去送 ASR"，
           * 和"设备没了"完全不是一回事（补了会把半截音频送去识别）。
           * 异常中断只值一个标志，怎么处理是上层的事（见 ai_audio.h）。 */

          bool timed_out = (nbytes < 0 && read_errno == ETIMEDOUT);

          /* 数据流活跃度：这次是"读了，但一个字节都没拿到"，如实记下来。
           * 连续空读计数只有超时这条路才可能大于 1（另外两条都会立刻跳出循环）。
           * 记下的结果码：超时 -ETIMEDOUT、EOF -ECANCELED、其它负值原样 ——
           * diag 快照的 lres 一栏会把它打出来，一眼看出"为什么断的"。 */

          ctx->record_empty_reads++;
          ctx->record_last_result = timed_out ? -ETIMEDOUT :
                                    ((nbytes == 0) ? -ECANCELED : (int)nbytes);

          if (timed_out &&
              ctx->record_empty_reads <= AUDIO_RECORD_TIMEOUT_TOLERANCE)
            {
              /* 还在容忍范围内：跳过这一帧，接着读下一帧。
               *
               * 日志节流：这一段连续超时只在第 1 次和第 2 次各打一行
               * （一次超时要等 5 秒，连着来就会刷屏）；"恢复"那一行在数据通路里
               * 打。第 2 行之后到上限之前都不打 —— 连续到第几次在
               * record_empty_reads 里，真需要时读得到（audio_record_empty_reads）。 */

              if (ctx->record_empty_reads == 1)
                {
                  printf("[录音] 读超时（第 1 次，连续超时容忍 %d 次）："
                         "跳过这一帧，会话继续\n", AUDIO_RECORD_TIMEOUT_TOLERANCE);
                }
              else if (ctx->record_empty_reads == 2)
                {
                  printf("[录音] 读连续超时（已 %u 次，容忍 %d 次）："
                         "仍在读下一帧，不结束会话\n",
                         (unsigned)ctx->record_empty_reads,
                         AUDIO_RECORD_TIMEOUT_TOLERANCE);
                }

              /* 缓冲**不动**：这一帧没有新数据，绝不能把上一帧的样本再喂一遍
               * VAD / 数据回调（那等于把同一段音频喂两次）。
               * 下一帧读成功时会整帧覆盖 record_buf，所以也用不着清零。 */

              continue;
            }

          /* 超时到上限：这一代会话不再给数据了。先打一行和"被 stop / EOF"
           * 分得开的日志，再落到下面共用的收尾。 */

          if (timed_out)
            {
              printf("[录音] 读连续超时 %u 次（每次等 5 秒都没数据），"
                     "超过容忍上限 %d，判定会话已死\n",
                     (unsigned)ctx->record_empty_reads,
                     AUDIO_RECORD_TIMEOUT_TOLERANCE);
            }

          if (!ctx->record_stop)
            {
              /* 死因在**发现死亡的这一刻**就落地，线程末尾据此选收尾方式。
               * 两种死因的正确收尾正好相反，所以这里不能只置一个"死了"：
               *
               *   - nbytes == 0（EOF）：是**别人**把这一代停掉的。本层没让它停
               *     （上面的 record_stop 已经把自己排除了），而 0 只有两个来源
               *     —— 被 AUDIOIOC_STOP 打断 / 会话换代 —— 两个都说明设备已经
               *     不在我们手里了。此时发设备级 STOP 只会把刚接管设备的那个
               *     会话打死（半双工），所以走 stolen → 只 close。
               *     （2026-09-20 晚修正：原来这里把"**设备没在跑**"也算作 0 的
               *       来源之一，于是把"需要 STOP 才能收干净"的那一类归进了
               *       "绝不能 STOP"那一类 —— 现场那个"1.8 秒一轮、永远起不来"的
               *       自锁就是这么来的。现在驱动把"设备没在跑"单独用 -ENODEV
               *       说出来，见下面那一支。）
               *   - 其余（连续读超时到上限，-ENODEV "设备没在跑"，或 -EINVAL/fd
               *     失效这类真错误）：**没有任何人发过 STOP**，这一代就是我们自己
               *     在用，走 starved → 先 STOP 再 close，把设备收干净。
               *     这几种归到一类是因为判据一致：都没有"别人把我停了"的实证，
               *     而那正是 stolen 唯一成立的理由。 */

              if (nbytes == 0)
                {
                  stolen = true;

                  printf("[录音] 录音被外部停掉（read 返回 0/EOF，不是本层在停）："
                         "设备已被别人接手，收尾只关自己的 fd、不发 STOP\n");
                }
              else if (nbytes == -ENODEV)
                {
                  /* 驱动明确说"设备压根没在跑"（不是被抢走）。这一支必须**发 STOP**：
                   * 框架那份共享 status（可能正钉在 DRAINING）只有 STOP 或"最后一个
                   * fd 被关"才放得掉；不发它，下一个会话的 CONFIGURE/START 会被
                   * 上层静默吞掉（返回 OK、驱动一次没被调），于是每次 read 又立刻
                   * -ENODEV —— 现场那个自锁。 */

                  starved = true;

                  printf("[录音] 设备没在跑（read 返回 -ENODEV，驱动侧确认，"
                         "不是被抢走）：按最后使用者停掉设备\n");
                }
              else
                {
                  starved = true;

                  printf("[录音] 录音异常中断（read 返回 %ld，errno=%d，"
                         "不是本层在停）：没人发过 STOP，收尾按最后使用者"
                         "停掉设备\n", (long)nbytes, read_errno);
                }
            }
          else
            {
              AUDIO_DEBUG("录音被 stop 打断，正常收尾（不置 record_died）");
            }

          break;
        }
    }

  /* 收尾前再确认一次"我还是当前这一代"：上面那次判断之后（处理一帧 + 打印的
   * 工夫里）上层完全可能已经起了新一代会话，这时本线程已经是个"上一代的
   * 残影"。再往后每一句都是错的：
   *   - audio_in_stop() / audio_in_abandon() 会把新一代刚 START 好的会话带走
   *     （设备与那份 fd 都是全局一份）；
   *   - recording / record_exited 会被写到新一代头上，于是"线程在跑、标志说
   *     没在录、还报 record_exited"，监听守护据此反复重开，永远好不了。
   * 所以这时候什么都不碰，直接退出。 */

  if (stale || !audio_record_thread_is_current(ctx))
    {
      printf("[录音] 上一代录音线程退出（当前会话已由新一代接管）："
             "不碰设备，也不动任何标志\n");
      return NULL;
    }

  /* 线程自己收尾时也要把设备还回去，但**怎么还**取决于这一代是怎么死的
   * （stolen / starved，在发现死亡那一刻定下来）。
   *
   * 没有一个"本层让停"的分支单独写：那种情况下两个量都是假，
   * 照旧走 audio_in_stop()。它幂等，audio_record_stop() 那条路到这里通常已经是
   * 空操作（设备早停了），只有"那次 STOP 被驱动拒了"（现场见过 ENOTTY）时才真的
   * 有用 —— 那是把设备还回去的最后一次机会，不能省。而"线程压根没进循环"
   * （want 为 0，帧长配置异常）也落在这一支：设备是 audio_record_start() 刚
   * START 好的，只有本层能动它。
   *
   * **被抢走的（stolen）只能 abandon（只 close）**：
   * 此刻握住设备的正是刚接管它的那个会话 —— 现场多半是正在播报的 TTS ——
   * 收尾时再发一次**设备级** AUDIOIOC_STOP 就会连它一起打死（现场老串口里
   * "[录音] 录音异常中断…会话已死"后面紧跟播报半路哑掉，就是这条路径，
   * 而且在现场反复出现；那行日志现在按死因拆成了"被外部停掉"和"异常中断"两行，
   * 走本支的是前一行）。abandon 不会漏设备：它只关本层那个 fd，而关 fd 是否真去动
   * 硬件由 NuttX 上层决定 —— 上层在**这个设备上最后一个 fd 被关**时才调驱动的
   * hw_shutdown（nuttx/audio/audio.c:263-275，upper->head 变空才调），
   * hw_shutdown 自己会停 DMA、DISABLE AUDPRC、关功放、把卡在 read 里的会话作废。
   * 所以有人接管时对方毫发无伤；万一没人接管，我们就是最后一个 fd，驱动照常
   * 收尾，设备照样被放回去。
   *
   * **自己断粮的（starved）要走 stop，不能图省事一律 abandon** —— 这是本轮
   * 复核定下来的分岔，两边的理由都写在这里：
   *   · abandon 的清理效果不是本层说了算：它只 close 自己的 fd，而"这一 close
   *     到底动不动硬件、什么时候动"取决于这个设备上还有没有别的 file 对象
   *     （上层内部的条件，本层看不见）；而且它**不唤醒**任何阻塞中的 read
   *     （见 sf32lb52_audio_in.h 里 abandon 那段边界：同一个 file 对象还被
   *     阻塞的 read 持着时，close 只是减引用计数，真正的 close 和上层 shutdown
   *     要等那次 read 返回）。starved 已经判定"设备是我们自己在用"，
   *     这种场合要的是**主动、确定**地把它收掉，那就只有 STOP：它明确停驱动
   *     （清 running、停 DMA、关模拟通路/功放），随后的 close 在关掉上层
   *     status 时会连还可能钉着的 DRAINING 一起释放。
   *     （stop 万一被驱动拒了，行为与本层让停那一支完全一样：板级会留一行
   *      AUDIOIOC_STOP failed 的 ERR，不关 fd 也不清全局，下一次
   *      audio_record_start() 开头会先重试一次 stop，成功了就自然恢复。
   *      这里不额外兜底，免得把板级"STOP 失败就别动设备"的设计绕过去。）
   *   · 而"被抢走"那一支又绝不能跟着走 stop：那时 STOP 到不了驱动
   *     （audio.c:685-688 要求自己的 status 是 RUNNING/PAUSED **且**除自己以外
   *     所有 openpriv 都是 OPEN；对家只要成功 CONFIGURE 过一次，
   *     priv->state 就 ≥ PREPARED，条件不成立），却照样会白踩下面这个共享状态。
   *   · STOP 真被转下去时的副作用（正是 starved 需要它、stolen 必须躲开它的
   *     原因）：audio.c:692 会把**共享的** upper->status->state 置成 DRAINING，
   *     而且驱动成功返回也**不还原**。本固件没有任何路径会发
   *     AUDIO_CALLBACK_COMPLETE / AUDIO_APB_FINAL（板级只在队列模式回调里发，
   *     而全仓库没人置那一位），而 DRAINING 只在两种情况下才会消失
   *     （audio.c:1556-1561 的 COMPLETE 回调，或最后一个 fd 被关时连
   *     upper->status 一起释放）—— 也就是说它会一直钉着，期间每个新会话的
   *     CONFIGURE（audio.c:479）和 START（audio.c:621-658）都被上层静默吞掉：
   *     返回 OK、驱动一次都没被调用，"看着在录、一个字节都没有"。
   *     starved 这一支是主动收自己这一代（stop + close），DRAINING 不会留到下一代；
   *     stolen 那一支要是也发 STOP，受害的却是后面每一个会话。
   * "先探测一下有没有别人在用、再决定发不发 STOP" 这条路走不通：别人的
   * openpriv / state 只在上层内部，本层没有任何接口问得出来。
   * 也不能靠"线程末尾再读一次 record_stop"来分岔：那几微秒里上层完全可能
   * 正好调 audio_record_stop()（所以死因在发现死亡那一刻就定了）。 */

  if (stolen)
    {
      audio_in_abandon();
    }
  else
    {
      audio_in_stop();
    }

  AUDIO_DEBUG("录音线程退出");
  ctx->recording = false;
  ctx->state = ctx->playing ? AUDIO_STATE_PLAYING : AUDIO_STATE_IDLE;

  /* 异常死亡的标记**在这里**才对上层可见，而不是在发现死亡那一刻。
   * 判死处就置位的话，监听守护可能正好在"还 active、却已经 died"的那半拍里
   * 进来（它只在会话不活跃时才清标记），把这一代的死亡标记顺手清掉，于是
   * 死亡失去"下一拍就重开"的资格。放在 recording 已清、record_exited 未置的
   * 窗口里，守护只可能看到"已经不活跃 + 已经 died"这个自洽组合。
   * 这一支的收尾方式（上面）不影响这个时机的选择：无论 abandon 还是 stop，
   * 标记都在 recording 清掉之后才置位。 */

  if (stolen || starved)
    {
      ctx->record_died = true;
    }

  /* 最后一步：告诉 audio_reap_record_thread() "我已经跑完了"。
   * 必须在所有收尾动作**之后**置位，否则别人会立刻 join 走 TCB。 */

  ctx->record_exited = true;
  return NULL;
}

/**
 * @brief  播放线程
 *
 * 播放的 fd **只在本线程里** open/configure/write/close：NuttX 的 fd 属于
 * task group，跨任务用别人的 fd 会炸机（这项目已经炸过三次）。
 * 驱动里的 write() 是同步的（阻塞到这块 DMA 放完），所以分块写，
 * 每块之间看一眼 play_stop，audio_play_stop() / audio_deinit() 才能及时生效。
 *
 * 本线程是 ctx->play_last_result 的**唯一**写入者，写入点在完成回调之前 ——
 * 让"这次到底出没出声"在回调那一刻就是确定的（读法见 ai_audio.h 的
 * audio_play_last_result()）。完成回调本身语义不变：设备打不开、START 被拒、
 * 写失败也照样回调，好让上层状态机不会等一个永远不来的完成事件。
 */

static void *audio_play_thread(void *arg)
{
  audio_context_t *ctx = (audio_context_t *)arg;
  size_t frame_bytes = ctx->config.channels * sizeof(int16_t);
  size_t total_bytes = ctx->play_frames * frame_bytes;
  size_t chunk_bytes = audio_play_chunk_bytes(ctx);
  size_t done = 0;
  int fail = 0;                  /* 0=没失败；负值=没出声的原因，最后写进 ctx */
  int fd;

  if (chunk_bytes == 0)
    {
      chunk_bytes = total_bytes;   /* 防御：别写出 0 字节的空块 */
    }

  AUDIO_DEBUG("播放线程启动 (%zu 帧)", ctx->play_frames);

  fd = audio_open_play_device(ctx);
  if (fd < 0)
    {
      /* 打不开设备就不假装在放：直接走完流程，让 play_complete 回调
       * 把上层状态机推下去，别让它等一个永远不会来的完成事件。
       * 但"没出声"这件事必须记下来（下面写进 play_last_result），
       * 否则上层读到回调就以为放完了 —— 那正是"一声没响、日志说放完了"的根因。
       * 具体原因（open / CONFIGURE / START 哪一步、什么错误码）已经由
       * audio_open_play_device() 那行 printf 打出串口了，这里不重复。 */

      fail = fd;
      AUDIO_DEBUG("播放设备不可用: %d", fd);
    }
  else
    {
      while (!ctx->play_stop && ctx->playing && done < total_bytes)
        {
          size_t chunk = total_bytes - done;
          ssize_t written;

          if (chunk > chunk_bytes)
            {
              chunk = chunk_bytes;
            }

          written = write(fd, (const char *)ctx->play_buf + done, chunk);

          /* write() 同步：返回 <= 0 说明设备被别的会话占了或出错
           * （板级 alarm 同一招：关掉 fd，剩下的静默走完）。 */

          if (written <= 0)
            {
              fail = -EIO;
              printf("[AUDIO] 播放没出声：write 返回 %zd"
                     "（已写 %zu/%zu 字节，这段只出了一部分或一声没出）\n",
                     written, done, total_bytes);
              break;
            }

          done += (size_t)written;
        }

      /* "写满但没走完"：被上层 stop 打断不算失败（那是调用方要停），
       * 除此之外只要没写满就是没放完 —— 不记下来的话，上层会把它当成
       * "放完了"（这也是上面那行日志之后的兜底，正常路径到不了）。 */

      if (fail == 0 && !ctx->play_stop && done < total_bytes)
        {
          fail = -EIO;
          printf("[AUDIO] 播放没出声：写了 %zu/%zu 字节就退出循环了\n",
                 done, total_bytes);
        }
    }

  /* 先停设备再退出：AUDIOIOC_STOP 会同时停掉录音通路（半双工），
   * 所以它必须是播放的最后一步。fd 也是在本线程里关的。 */

  audio_close_play_device(fd);

  /* 结果落定：必须在 audio_resume_record() / play_cb **之前** ——
   * 回调里（或回调返回后立刻）读到的要是最终值，不能是中转值。
   *
   * 判据是"整段有没有写进设备"，不是"线程收没收尾"：
   *   - 有失败码（打不开 / START 被拒 / 写失败）→ 原样记下，就是"没出声"；
   *   - 没失败码但没写满 → 只可能是被 audio_play_stop() 砍断的（写失败那条
   *     上面已经排除了），记 -ECANCELED：不算放完，也不算"出错"；
   *   - 写满了 → OK（**即**播放线程收尾时 play_stop 才置位，也照样是"整段
   *     都进了设备"，不该因为一个收尾竞态被报成"被打断"）。 */

  if (fail != 0)
    {
      ctx->play_last_result = fail;
    }
  else if (done < total_bytes)
    {
      ctx->play_last_result = -ECANCELED;
    }
  else
    {
      ctx->play_last_result = OK;
    }

  ctx->playing = false;
  ctx->state = ctx->recording ? AUDIO_STATE_RECORDING : AUDIO_STATE_IDLE;

  /* 是播放把录音停掉的，播完按原配置恢复（理由见 audio_prepare_output）。
   * 顺序要点：必须先把 playing 置 false —— 否则 audio_record_start() 的半双工
   * 逻辑会反过来再停一次播放，连带把下面的 play_cb 也吞掉。
   * 被 audio_play_stop() 打断的播放不恢复：那是上层主动要停。 */

  if (!ctx->play_stop)
    {
      audio_resume_record(ctx);
    }

  /* 播放完成回调（在播放线程里回调，调用者可以放心紧跟着开录音）。
   * 语义不变：失败也回调 —— 上层（hello_app 的 TTS 状态机）靠它收尾，
   * 吞掉它会让人一直卡在"正在说话"。想区分成败请读 play_last_result。 */

  if (!ctx->play_stop && ctx->play_cb)
    {
      ctx->play_cb(ctx->play_user_data);
    }

  /* 最后一步：告诉 audio_reap_play_thread() "我已经跑完了"。
   * 必须在所有收尾动作（关播放设备 / 恢复录音 / 完成回调）**之后**置位 ——
   * 回收的人看到它就是直接 join 走 TCB，早置一位等于把线程剩下的动作甩给一个
   * 随时可能被释放的上下文。和录音线程末尾那个 record_exited 是同一套语义。 */

  ctx->play_exited = true;

  AUDIO_DEBUG("播放线程退出");
  return NULL;
}

/**
 * @brief  启动播放线程（数据已经在 ctx->play_buf 里了）
 * @return 0成功, 负值失败
 *
 * 设备由播放线程自己 open/START/write/STOP/close，这里只负责：
 * 回收上一次的播放线程（它自己会关 fd）、记下参数、起线程。
 *
 * 起线程前后各写一次 play_last_result（见 ai_audio.h 的 audio_play_last_result）：
 *   - 起线程前写 -EINPROGRESS：让"上一次的结果"在这次还没出结果时就先作废，
 *     否则读的人可能读到上一次的 0 而以为这次放完了；
 *   - 线程起不来时写 -ret：这也是一次"没出声"的播放尝试，得如实记下来。
 */

static int audio_play_begin(audio_context_t *ctx, size_t frames,
                            audio_play_complete_cb_t callback,
                            void *user_data)
{
  int ret;

  /* 上一次的播放线程需要回收。正常情况下 audio_play_stop() 已经 join 过了，
   * 这里是兜底（比如播放线程自己跑完、没被 stop 过）。
   * 如果本函数就是在播放线程里被调的（播放完成回调里又开一段），不能 join 自己：
   * 线程已经写完了缓冲、回调返回后就退出，直接起新的即可。
   *
   * 回收是**有界**的（见 audio_reap_play_thread）。它放弃 join（线程卡在设备里
   * 没退出来）时不能再往下走：那条线程还在用同一个 ctx->play_buf，也还占着音频
   * 设备，再起一条播放线程就是"两条线程抢一个设备 + 一起写同一块缓冲"。
   * 报 -EBUSY 让上层自己收尾（上层本来就有负返回值的处理），好过在这里死等。 */

  if (ctx->play_thread_valid)
    {
      if (pthread_equal(pthread_self(), ctx->play_thread))
        {
          AUDIO_DEBUG("在播放线程里重开播放，跳过 join");
          ctx->play_thread_valid = false;
        }
      else
        {
          audio_reap_play_thread(ctx, "play_begin");

          if (ctx->play_thread_valid)
            {
              AUDIO_DEBUG("上一次播放线程还没收尾，本次起播放弃");

              /* 录音已经为这次播放让过路了（audio_prepare_output），
               * 而这次没播成：赶紧把录音恢复起来，别让上层一直聋着。
               * playing 此刻还是 false（调用方在 audio_play_start 里刚判过），
               * 恢复逻辑不会反过来再停一次播放 —— 和下面 pthread_create
               * 失败那条路同一个处理。 */

              audio_resume_record(ctx);
              return -EBUSY;
            }
        }
    }

  ctx->play_cb = callback;
  ctx->play_user_data = user_data;
  ctx->play_frames = frames;
  ctx->play_stop = false;
  ctx->playing = true;
  ctx->play_exited = false;               /* 新一代播放线程的收尾标记 */
  ctx->play_last_result = -EINPROGRESS;   /* 这次的结果还没落定 */

  ret = pthread_create(&ctx->play_thread, NULL, audio_play_thread, ctx);
  if (ret != 0)
    {
      AUDIO_DEBUG("创建播放线程失败: %d", ret);
      ctx->playing = false;
      ctx->play_last_result = -ret;       /* 这次也没出声 */

      /* 录音已经为这次播放让过路了，但播放没起来：赶紧恢复，
       * 别让上层一直聋着（playing 已经置 false，恢复逻辑不会再停播放）。 */

      audio_resume_record(ctx);
      return -ret;
    }

  ctx->play_thread_valid = true;

  /* 更新状态 */

  ctx->state = ctx->recording ? AUDIO_STATE_BOTH : AUDIO_STATE_PLAYING;

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/**
 * @brief  初始化音频模块
 */

int audio_init(audio_context_t *ctx, const audio_config_t *config)
{
  if (ctx == NULL)
    {
      return -EINVAL;
    }

  AUDIO_DEBUG("初始化音频模块");

  /* 清零上下文 */

  memset(ctx, 0, sizeof(audio_context_t));

  /* 设置默认配置 */

  if (config != NULL)
    {
      memcpy(&ctx->config, config, sizeof(audio_config_t));
    }
  else
    {
      ctx->config.sample_rate = AUDIO_DEFAULT_SAMPLE_RATE;
      ctx->config.channels = AUDIO_DEFAULT_CHANNELS;
      ctx->config.format = AUDIO_FORMAT_S16_LE;
      ctx->config.frame_ms = AUDIO_DEFAULT_FRAME_MS;
      ctx->config.volume = AUDIO_VOLUME_DEFAULT;
    }

  if (ctx->config.sample_rate <= 0 ||
      (ctx->config.channels != AUDIO_CH_MONO &&
       ctx->config.channels != AUDIO_CH_STEREO) ||
      (ctx->config.format != AUDIO_FORMAT_S16_LE &&
       ctx->config.format != AUDIO_FORMAT_S16_BE) ||
      ctx->config.frame_ms == 0)
    {
      return -EINVAL;
    }

  /* 设备 fd 不存在 ctx 里用（NuttX 的 fd 属于 task group，谁用谁开）：
   * 录音 fd 由板级封装 sf32lb52_audio_in 持有，播放 fd 由播放线程持有。
   * 这两个字段只保留占位，不要往里存 fd 跨任务用。 */

  ctx->record_fd = -1;
  ctx->play_fd = -1;

  /* 初始化VAD参数 */

  ctx->vad_energy_threshold = AUDIO_VAD_ENERGY_THRESHOLD;

  /* 分配缓冲区 */

  size_t frame_bytes = ctx->config.channels * sizeof(int16_t);
  size_t frames_per_period =
    (size_t)ctx->config.sample_rate * ctx->config.frame_ms / 1000;

  ctx->record_buf_size = frames_per_period * frame_bytes;
  ctx->record_buf = (int16_t *)calloc(1, ctx->record_buf_size);
  if (ctx->record_buf == NULL)
    {
      AUDIO_DEBUG("分配录音缓冲区失败");
      return -ENOMEM;
    }

  ctx->play_buf_size =
    (size_t)ctx->config.sample_rate * AUDIO_PLAY_BUFFER_MS / 1000 *
    frame_bytes;
  ctx->play_buf = (int16_t *)calloc(1, ctx->play_buf_size);
  if (ctx->play_buf == NULL)
    {
      AUDIO_DEBUG("分配播放缓冲区失败");
      free(ctx->record_buf);
      return -ENOMEM;
    }

  ctx->state = AUDIO_STATE_IDLE;
  ctx->initialized = true;

  AUDIO_DEBUG("音频模块初始化完成");
  AUDIO_DEBUG("  采样率: %d Hz", ctx->config.sample_rate);
  AUDIO_DEBUG("  通道数: %d", ctx->config.channels);
  AUDIO_DEBUG("  帧长: %d ms", ctx->config.frame_ms);

  return OK;
}

/**
 * @brief  反初始化音频模块
 */

void audio_deinit(audio_context_t *ctx)
{
  if (ctx == NULL || !ctx->initialized)
    {
      return;
    }

  AUDIO_DEBUG("反初始化音频模块");

  /* 停止录音和播放。两个 stop 都会等各自的线程退出（设备 fd 由录音封装
   * 和播放线程自己关），正常路径下等到这里就可以安全 free 缓冲了。
   *
   * 录音那条回收这次改成"一定会 join"（驱动 read 有 5 秒硬上界，见
   * audio_reap_record_thread），所以它退出时一定已经收干净；播放那条仍然是
   * **有界**的（设备卡住时会放弃 join、留一行日志），那时线程还活着、还在用
   * play_buf。那种情况下**绝不能** free —— 线程会踩到已释放的缓冲（比漏一点
   * 内存严重得多，而且现场只会是一个莫名其妙的崩溃）。所以这里逐个判：
   * 只要线程还没退干净就留着缓冲不释放，反正本函数只在收摊时跑一次。 */

  audio_record_stop(ctx);
  audio_play_stop(ctx);

  /* 释放缓冲区。
   *
   * **不置 NULL**：指针要留给那条还在跑的线程用 —— 播放线程下一轮还会
   * `write(fd, ctx->play_buf + done, chunk)`，把它清成 NULL 就成了"空指针 + 偏移"
   * 的一个非法地址（驱动那边只判 buffer == NULL，判不出这种），比漏内存危险得多。
   * 这里只是不 free，缓冲区本身仍是有效的堆块。 */

  if (ctx->record_thread_valid && !ctx->record_exited)
    {
      syslog(LOG_ERR, "[AUDIO] 录音线程还没退干净，保留录音缓冲不释放"
                      "（避免线程踩已释放内存）\n");
    }
  else if (ctx->record_buf != NULL)
    {
      free(ctx->record_buf);
      ctx->record_buf = NULL;
    }

  if (ctx->play_thread_valid && !ctx->play_exited)
    {
      syslog(LOG_ERR, "[AUDIO] 播放线程还没退干净，保留播放缓冲不释放"
                      "（避免线程踩已释放内存）\n");
    }
  else if (ctx->play_buf != NULL)
    {
      free(ctx->play_buf);
      ctx->play_buf = NULL;
    }

  ctx->initialized = false;
  ctx->state = AUDIO_STATE_UNINIT;

  AUDIO_DEBUG("音频模块已反初始化");
}

/**
 * @brief  开始录音
 */

int audio_record_start(audio_context_t *ctx,
                       const audio_record_config_t *config)
{
  int ret;

  if (ctx == NULL || !ctx->initialized)
    {
      return -EINVAL;
    }

  if (ctx->recording)
    {
      AUDIO_DEBUG("已在录音中");
      return -EBUSY;
    }

  /* 半双工：要录就先别放（AUDIOIOC_STOP 会把录放两条通路一起停，
   * 同时开着必然互相打断）。audio_play_stop() 会 join 播放线程，
   * 由播放线程自己关掉设备 fd，所以往下走是安全的。 */

  if (ctx->playing)
    {
      AUDIO_DEBUG("正在播放，先停止播放（半双工）");
      audio_play_stop(ctx);
    }

  /* 上一次的录音线程还没回收：**不能直接 pthread_join**。
   * 那个线程可能正阻塞在 audio_in_read() 里，只有设备先停下来它才会返回，
   * 所以必须先走 audio_record_stop()（它内部先 audio_in_stop() 再 join）。 */

  if (ctx->record_thread_valid)
    {
      audio_record_stop(ctx);
    }

  AUDIO_DEBUG("开始录音");

  /* 保存录音配置 */

  if (config != NULL)
    {
      memcpy(&ctx->record_cfg, config, sizeof(audio_record_config_t));
    }
  else
    {
      memset(&ctx->record_cfg, 0, sizeof(audio_record_config_t));
    }

  if (ctx->record_cfg.silence_timeout_ms == 0)
    {
      ctx->record_cfg.silence_timeout_ms = AUDIO_VAD_SILENCE_TIMEOUT_MS;
    }

  if (ctx->record_cfg.min_speech_ms == 0)
    {
      ctx->record_cfg.min_speech_ms = AUDIO_VAD_MIN_SPEECH_MS;
    }

  /* 打开 + 配置 + 启动录音通路：走板级封装 sf32lb52_audio_in。
   * 本板全链路只有 16bit 小端，格式不对就别去开设备。 */

  if (ctx->config.format != AUDIO_FORMAT_S16_LE)
    {
      AUDIO_DEBUG("只支持 s16le 录音 (format=%d)", ctx->config.format);
      return -EINVAL;
    }

  ret = audio_in_start(ctx->config.sample_rate, ctx->config.channels,
                       AUDIO_DEFAULT_BITS_PER_SAMPLE);
  if (ret < 0)
    {
      AUDIO_DEBUG("启动录音通路失败: %d", ret);
      return ret;
    }

  /* 启用VAD */

  if (ctx->record_cfg.enable_vad)
    {
      ctx->vad_enabled = true;
      ctx->vad_speech_active = false;
      ctx->vad_silence_frames = 0;
      ctx->vad_speech_frames = 0;
    }

  /* 启动录音线程：设备已经 START 好，线程里只管 audio_in_read() */

  ctx->record_stop = false;
  ctx->recording = true;
  ctx->record_exited = false;         /* 新一代录音线程的收尾标记 */
  ctx->record_died = false;           /* 上一代的"异常中断"标记到此作废 */

  /* 数据流活跃度也重新起算：上一代攒下的时间戳/计数/错误码到这里作废。
   * 时间戳先种成"现在"（设备已经 START 好了，线程马上就开读）——
   * 这样"会话起来了却一个字都没读到"也算"安静得越来越久"，
   * 正是 audio_record_idle_ms() 要能看见的形态之一。
   * 还没有任何一次 read 的结果，所以记 -EINPROGRESS（和 play_last_result
   * 起播时同一个口径）。 */

  ctx->record_last_data_ms = audio_now_ms_nonzero();
  ctx->record_read_start_ms = 0;
  ctx->record_empty_reads = 0;
  ctx->record_last_result = -EINPROGRESS;

  ret = pthread_create(&ctx->record_thread, NULL,
                       audio_record_thread, ctx);
  if (ret != 0)
    {
      AUDIO_DEBUG("创建录音线程失败: %d", ret);
      ctx->recording = false;
      ctx->record_last_data_ms = 0;   /* 没起成，"从没启动过录音"才是实话 */
      audio_in_stop();                /* 把刚 START 的设备还回去 */
      return -ret;
    }

  ctx->record_thread_valid = true;

  /* 更新状态 */

  ctx->state = AUDIO_STATE_RECORDING;

  return OK;
}

/**
 * @brief  回收录音线程（有界等待，界面优先）
 *
 * 为什么不用裸 pthread_join：本函数是在**别的任务**里被调的，而最典型的那
 * 个调用者就是 LVGL 线程（「提交」按钮回调 → voice_submit_handler →
 * audio_record_stop）。一旦录音线程因为驱动 read 没返回而出不来，join 就把
 * 界面整块挂住 —— 实测过一次：提交按钮一直绿着不变色、整机像死机。
 *
 * 做法：先自己轮询 ctx->record_exited（usleep 自旋，**不依赖任何内核超时**），
 * 最多 AUDIO_RECORD_REAP_MS；正常路径（设备已经停了、驱动那次 read 已经返回、
 * 线程正在收尾）到这里就等到了，随后 join 立刻返回。
 *
 * ★ 2026-09-20 真机定案：**等不到就放弃 join，绝不在这里无限期等下去。**
 *
 * 上一版注释（2026-09-19 写的）的前提是"驱动录音 read 有 5 秒硬上界，所以这个
 * join 确定会返回"，据此把原来那条"放弃 join"的兜底删掉了。当天现场就把这个
 * 前提否掉 —— 板子卡死时抓的 dumpstack（串口 `dumpstack 55/56/51`）：
 *
 *   main_loop_task     → audio_record_stop → pthread_join → nxsem_wait_slow
 *   care_announce_task → audio_play_start → audio_record_stop → pthread_join
 *   audio_record_thread→ sf32lb52_audio_read+0x5d3 → nxsem_wait_slow
 *                        （落在 sf32lb52_audio_rx_wait_slice 那次
 *                          nxsem_wait_uninterruptible 上，而驱动的 timeout
 *                          计数恒 0 ⇒ 那记"私有心跳"根本没把它叫醒）
 *
 * 后果不是"收尾慢"，而是 **join 把调用它的整条线程一起永久钉住**：
 * main_loop_task 一死，监听守护 / 状态机 / 追问流程全停；而界面、MQTT、关怀
 * 这些别的任务照常打日志 —— 从外面看像"app 还活着、只是说话没反应"，
 * 只能断电重来。
 *
 * 所以恢复那条兜底，形状与播放线程的 audio_reap_play_thread 逐条对齐：等不到就
 * 把它 detach 掉（内核在它真退出时回收 TCB）、清掉 record_thread_valid
 * （= 我们不再持有它），重开会话的事交回上层（监听守护）。
 * 代价是可能留下一个"上一代"的 reader —— 本文件里那整套 stale / record_died /
 * audio_record_thread_is_current 的判据本来就是为它写的，它退出时自己安静收场。
 */

/* 放弃 join 的门槛。比原来那个 300ms 宽：300ms 是"收尾慢了要报一行"的诊断线，
 * 不是"可以放弃它"的线，拿它当门槛会把正常但稍慢的收尾误判成卡死
 * （播放那条 AUDIO_PLAY_REAP_MS 也是同样的取值理由）。 */

#define AUDIO_RECORD_REAP_MS   1000

static void audio_reap_record_thread(audio_context_t *ctx, const char *who)
{
  int i;

  if (!ctx->record_thread_valid)
    {
      return;
    }

  /* 在录音线程自己里调（数据回调里触发状态切换）：不能 join 自己。
   * 线程会在回到循环判据时看到 record_stop 自己跳出，TCB 留给下次回收。 */

  if (pthread_equal(pthread_self(), ctx->record_thread))
    {
      AUDIO_DEBUG("在录音线程里调 stop，跳过 join");
      return;
    }

  for (i = 0; i < AUDIO_RECORD_REAP_MS / 10 && !ctx->record_exited; i++)
    {
      usleep(10000);                    /* 10ms × 100 = 1000ms 上限 */
    }

  if (!ctx->record_exited)
    {
      /* 还没退出 = 线程此刻还卡在设备调用里（理由与现场证据见上面那段）。
       * **放弃 join**：继续等下去就是把调用者一起钉死，而调用者里包括主循环。 */

      syslog(LOG_ERR,
             "[AUDIO] %s: 录音线程 %dms 内没退出，放弃 join"
             "（它卡在设备调用里；detach 交给内核回收，账本交回上层）\n",
             who, AUDIO_RECORD_REAP_MS);

      (void)pthread_detach(ctx->record_thread);
      ctx->record_thread_valid = false;
      return;
    }

  pthread_join(ctx->record_thread, NULL);
  ctx->record_thread_valid = false;
}

/**
 * @brief  回收播放线程（有界等待，理由同 audio_reap_record_thread）
 *
 * 和录音那条是同一套做法：先轮询 ctx->play_exited（usleep 自旋，不依赖任何内核
 * 超时），退出后再 join 回收 TCB；等不到就放弃并留一行日志，play_thread_valid
 * 保持 true 让下一次收尾再来。
 *
 * 为什么必须有上界（这次才加的）：hello_app 起播和监听守护的停开麦都会走到
 * audio_play_stop / audio_play_begin。如果播放线程正卡在设备调用里
 * （真机上确实出现过：驱动里 hw_start / 那次 STOP 的现场，见 ai_companion_req.h
 * 头上那次跨 app 停设备的事故），裸的 pthread_join 就是永久等待 —— 于是主循环里
 * 下一次起播、下一次重开录音全部跟着钉死，整个 app（连带等它出声的
 * robot_ui 提醒链路）一起静默。放弃 join 的代价只是 TCB 晚一点回收，
 * 设备那边的残局本来就得由驱动 / 上层的守护去收拾，不该由一个应用线程无期限地等。
 *
 * 上限为什么比录音那 300ms 宽：播放线程被 stop 时可能正阻塞在一次同步 write()
 * 里，而驱动那一块的大小是 AUDIO_PLAY_CHUNK_MS=250ms、它自己的等待上限是
 * "一块 + 500ms 余量"（见 ai_audio.c 的 audio_play_stop 注释与
 * board/contest_board/src/sf32lb52_audio.c 的 write）。300ms 会在完全正常的
 * 路径上误判成"没退出来"，所以这里给 1000ms：正常收尾一定能在这之内退干净。
 */

#define AUDIO_PLAY_REAP_MS   1000

static void audio_reap_play_thread(audio_context_t *ctx, const char *who)
{
  int i;

  if (!ctx->play_thread_valid)
    {
      return;
    }

  /* 在播放线程自己里调（完成回调里又停一次）：不能 join 自己。
   * 线程回调返回后就会退出，TCB 留给下次回收。 */

  if (pthread_equal(pthread_self(), ctx->play_thread))
    {
      AUDIO_DEBUG("在播放线程里调 stop，跳过 join");
      return;
    }

  for (i = 0; i < AUDIO_PLAY_REAP_MS / 10 && !ctx->play_exited; i++)
    {
      usleep(10000);                    /* 10ms × 100 = 1000ms 上限 */
    }

  if (!ctx->play_exited)
    {
      syslog(LOG_ERR,
             "[AUDIO] %s: 播放线程 %dms 内没退出，放弃 join"
             "（多半卡在音频设备调用里；TCB 留给下一次回收）\n",
             who, AUDIO_PLAY_REAP_MS);
      return;                           /* play_thread_valid 保持 true */
    }

  pthread_join(ctx->play_thread, NULL);
  ctx->play_thread_valid = false;
}

/**
 * @brief  停止录音
 */

void audio_record_stop(audio_context_t *ctx)
{
  if (ctx == NULL)
    {
      return;
    }

  AUDIO_DEBUG("停止录音");

  /* 顺序很关键：**先让设备停下来，再收线程**。
   * audio_in_stop() 内部的 AUDIOIOC_STOP 会把阻塞在 audio_in_read() 里的
   * 录音线程唤醒（驱动已修：read 返回 0），线程随后跳出循环自己收尾。
   * 反过来（先等线程再关设备）就会一直卡着，最坏要等驱动下层的 read 超时。
   */

  ctx->record_stop = true;

  /* 设备那一层与线程那一层**分开**收（2026-09-20 真机定案）。
   *
   * 原来 audio_in_stop() 被关在 `if (record_thread_valid)` 里：只有当"还有一条线程
   * 等着回收"时才会给设备发 AUDIOIOC_STOP。而现场那种死循环恰好是反过来的 ——
   * 每次重开的会话**立刻** EOF、录音线程当场自己退干净并被回收（valid 变假），
   * 于是**一次 STOP 都没发出去**，设备一直停在"没在跑"的状态，下一轮重开照样
   * 起不来：串口里 60 多次循环、一条 `AUDIO: 通路位 adc_path_on -> 0` 都没有，
   * 表现就是"说话永远没反应"（只能断电）。
   *
   * 所以 STOP 无条件发：设备那一层必须有人收干净，跟"有没有线程要 join"无关。
   * 线程那一层依旧只在 valid 时才回收（没线程就不用 join）。 */

  audio_in_stop();

  if (ctx->record_thread_valid)
    {
      audio_reap_record_thread(ctx, "stop");
    }

  /* 走到这里设备已经停了：不管线程有没有收尾，对上层来说都"不在录音"了。
   * 不能让 audio_is_recording() 一直报 true —— 上层会以为麦克风还被占着。 */

  ctx->recording = false;

  ctx->recording = false;

  /* 更新状态 */

  ctx->state = ctx->playing ? AUDIO_STATE_PLAYING : AUDIO_STATE_IDLE;
}

/**
 * @brief  是否正在录音
 */

bool audio_is_recording(audio_context_t *ctx)
{
  return (ctx != NULL) ? ctx->recording : false;
}

/**
 * @brief  录音这条链路是不是真的还活着
 *
 * 四个字段缺一不可（理由见 ai_audio.h 的声明）：
 *   recording            —— 设备 START 过、线程被期望在跑
 *   record_thread_valid  —— 线程 TCB 还没被回收
 *   record_stop          —— 有人在停它（停的过程中不该被当成"健康的常听"）
 *   record_exited        —— 线程已经跑到最后一行（收尾标记，见录音线程末尾）
 *
 * 它只回答"还活着吗"，不回答"为什么死的"：后者看 ctx->record_died
 * （录音异常中断的标记，上层认领后自己清，见 ai_audio.h 那张说明表）。
 *
 * ⚠️ "活着"≠"还听得见"：设备不再给数据（DMA 完成中断不来了）时线程照样
 * 卡在 read 里，这四个字段全是健康的 —— 那种形状只有 diag 快照里的
 * idle / wait 两栏看得见（audio_record_idle_ms / audio_record_wait_ms），
 * 本函数管不了、也不该管：上层的恢复路径只看它（不活跃就重开）。
 */

bool audio_record_is_active(const audio_context_t *ctx)
{
  if (ctx == NULL || !ctx->recording || !ctx->record_thread_valid)
    {
      return false;
    }

  return !(ctx->record_stop || ctx->record_exited);
}

/**
 * @brief  录音这条链路"安静"了多久（详见 ai_audio.h 的声明）
 */

int audio_record_idle_ms(const audio_context_t *ctx)
{
  if (ctx == NULL)
    {
      return -EINVAL;
    }

  /* -1 有两个来源，对调用方是同一件事"没参照点"：从未启动过录音（哨兵 0），
   * 或者 ctx 是 NULL（那是调用方的 bug，用 -EINVAL 和它区分开）。 */

  return audio_since_ms(ctx->record_last_data_ms);
}

/**
 * @brief  录音线程"等设备给数据"等了多久（详见 ai_audio.h 的声明）
 *
 * 实现就是读录音线程写下的那个起等时间戳，不需要额外加锁：
 * 写的人（同一线程）先写时间戳再去阻塞 read，读的人拿到的只会是"稍微偏大"
 * 的等待时长（时间戳先写、时钟后读），偏大在判据那边是安全的方向
 * （阈值 8 秒，偏的是一微秒级）。
 */

int audio_record_wait_ms(const audio_context_t *ctx)
{
  if (ctx == NULL)
    {
      return -EINVAL;
    }

  return audio_since_ms(ctx->record_read_start_ms);
}

/**
 * @brief  最近一次录音 read 的结果（详见 ai_audio.h 的声明）
 *
 * 和 audio_play_last_result() 是同一套做法：只由录音线程写，读的人不加锁，
 * volatile 保证每次都真去读内存。
 */

int audio_record_last_result(const audio_context_t *ctx)
{
  if (ctx == NULL)
    {
      return -EINVAL;
    }

  return ctx->record_last_result;
}

/**
 * @brief  连续空读次数（详见 ai_audio.h 的声明）
 */

uint32_t audio_record_empty_reads(const audio_context_t *ctx)
{
  return (ctx != NULL) ? ctx->record_empty_reads : 0;
}

/**
 * @brief  开始播放音频数据
 */

int audio_play_start(audio_context_t *ctx,
                     const int16_t *data, size_t frames,
                     audio_play_complete_cb_t callback,
                     void *user_data)
{
  size_t frame_bytes;
  size_t capacity_frames;

  if (ctx == NULL || !ctx->initialized || data == NULL || frames == 0)
    {
      return -EINVAL;
    }

  if (ctx->playing)
    {
      AUDIO_DEBUG("已在播放中");
      return -EBUSY;
    }

  /* 先做不需要设备的校验/复制准备，再做半双工切割 —— 免得参数不对时
   * 已经把录音停了、却又没播成（那样麦克风就没人再打开了）。 */

  frame_bytes = ctx->config.channels * sizeof(int16_t);
  capacity_frames = ctx->play_buf_size / frame_bytes;
  if (frames > capacity_frames)
    {
      /* 播放缓冲就 AUDIO_PLAY_BUFFER_MS 这么大。塞不下就明确报错，
       * 不做静默截断（截断会让上层以为整段都放完了）。 */

      AUDIO_DEBUG("音频太长: %zu 帧 > 缓冲 %zu 帧", frames, capacity_frames);
      return -ENOSPC;
    }

  /* 半双工：把正在录的停了（播完会自动恢复，见 audio_prepare_output） */

  audio_prepare_output(ctx);

  /* 复制音频数据到缓冲区（必须在播放线程起来之前复制完）。
   * 复制完立刻做分块淡入淡出 —— 只改我们自己的副本，不碰调用者的 data。 */

  AUDIO_DEBUG("开始播放 (%zu 帧)", frames);

  memcpy(ctx->play_buf, data, frames * frame_bytes);
  audio_apply_chunk_fade(ctx, frames);

  return audio_play_begin(ctx, frames, callback, user_data);
}

/**
 * @brief  从文件播放音频
 */

int audio_play_file(audio_context_t *ctx,
                    const char *filepath,
                    audio_play_complete_cb_t callback,
                    void *user_data)
{
  size_t frame_bytes;
  size_t capacity_frames;
  size_t want_bytes;
  size_t got;
  size_t frames;
  FILE *fp;
  long size;

  if (ctx == NULL || !ctx->initialized || filepath == NULL)
    {
      return -EINVAL;
    }

  if (ctx->playing)
    {
      AUDIO_DEBUG("已在播放中");
      return -EBUSY;
    }

  /* 文件内容是**裸 PCM**（16k / 单声道 / s16le），和 `audio_test record`
   * 存出来的一样；带 WAV/MP3 头的容器格式不在这里解析（本板没有解码器）。
   * 先把文件读进缓冲，再动设备 —— 读文件失败时不该把录音停了。 */

  fp = fopen(filepath, "rb");
  if (fp == NULL)
    {
      int errcode = errno;

      AUDIO_DEBUG("打开音频文件失败: %s (%d)", filepath, errcode);
      return -errcode;
    }

  if (fseek(fp, 0, SEEK_END) != 0)
    {
      fclose(fp);
      return -EIO;
    }

  size = ftell(fp);
  rewind(fp);
  if (size <= 0)
    {
      fclose(fp);
      AUDIO_DEBUG("音频文件是空的: %s", filepath);
      return -EINVAL;
    }

  frame_bytes = ctx->config.channels * sizeof(int16_t);
  capacity_frames = ctx->play_buf_size / frame_bytes;

  /* 塞不下就报 -ENOSPC，不截断 */

  want_bytes = (size_t)size;
  if (want_bytes / frame_bytes > capacity_frames)
    {
      fclose(fp);
      AUDIO_DEBUG("音频文件太长: %ld 字节 > 缓冲 %zu 字节",
                  size, capacity_frames * frame_bytes);
      return -ENOSPC;
    }

  got = fread(ctx->play_buf, 1, want_bytes, fp);
  fclose(fp);

  frames = got / frame_bytes;           /* 末尾不足一帧的丢掉 */
  if (frames == 0)
    {
      AUDIO_DEBUG("音频文件没有完整的一帧: %s", filepath);
      return -EINVAL;
    }

  /* 和 audio_play_start 一样：分块边界上做淡入淡出，消除"每块一个啪" */

  audio_apply_chunk_fade(ctx, frames);

  /* 半双工：确认文件能放了，再停录音（播完会自动恢复） */

  audio_prepare_output(ctx);

  AUDIO_DEBUG("播放文件: %s (%zu 帧)", filepath, frames);

  return audio_play_begin(ctx, frames, callback, user_data);
}

/**
 * @brief  停止播放
 */

void audio_play_stop(audio_context_t *ctx)
{
  if (ctx == NULL)
    {
      return;
    }

  AUDIO_DEBUG("停止播放");

  /* 播放线程自己持有 fd（open/write/close 全在它自己的 task group 里），
   * 这里**不能**替它 ioctl(STOP)/close —— 那是跨任务碰 fd。
   * 让设备停下来的办法是置 play_stop：线程每 AUDIO_PLAY_CHUNK_MS 一块
   * 同步 write，写完一块就会看到标志、由它自己 ioctl(STOP)+close 后退出。
   *
   * 阻塞时长（AUDIO_PLAY_CHUNK_MS = 250 时）：
   *   - 常规：等到当前这块放完为止，≤ 250 ms，平均约 125 ms；
   *   - 驱动异常（DMA 完成中断没来）：write() 自己的等待上限
   *     一块 + 500 ms = 750 ms，然后照样退出来。
   * 真等不到（设备那一侧卡住了）由 audio_reap_play_thread() 兜底：放弃 join、
   * 留一行日志，不改"等不到就永远等"那条路。（本函数有两条路径是在
   * hello_app 的设备动作权锁里被调的，见那个函数的说明。）
   * 把块改小（100 ms）打断更跟手，代价是块边界的爆音更密，
   * 取舍说明见文件头部 AUDIO_PLAY_CHUNK_MS 那里。 */

  ctx->play_stop = true;

  /* 和录音一样：如果是从播放线程自己里调的（播放完成回调里又调 stop），
   * 不能 join 自己；线程返回前会自己判断 play_stop 决定是否再回调。
   * 这两种情况都在 audio_reap_play_thread() 里判。 */

  audio_reap_play_thread(ctx, "stop");

  ctx->playing = false;

  /* 更新状态 */

  ctx->state = ctx->recording ? AUDIO_STATE_RECORDING : AUDIO_STATE_IDLE;
}

/**
 * @brief  是否正在播放
 */

bool audio_is_playing(audio_context_t *ctx)
{
  return (ctx != NULL) ? ctx->playing : false;
}

/**
 * @brief  上一次播放到底出没出声（见 ai_audio.h 的完整说明）
 *
 * 实现就是读播放线程写下的那个字段：写的人在完成回调之前写定，读的人在回调
 * 之后读（robot_ui/main.c 的 reminder_play_exclusive 就是这么用的），所以不需要
 * 额外加锁；volatile 只保证编译器每次都真去读内存（不要缓存进寄存器）。
 */

int audio_play_last_result(const audio_context_t *ctx)
{
  if (ctx == NULL)
    {
      return -EINVAL;
    }

  return ctx->play_last_result;
}

/**
 * @brief  设置音量
 */

int audio_set_volume(audio_context_t *ctx, uint8_t volume)
{
  int ret;

  if (ctx == NULL || !ctx->initialized)
    {
      return -EINVAL;
    }

  if (volume > AUDIO_VOLUME_MAX)
    {
      volume = AUDIO_VOLUME_MAX;
    }

  AUDIO_DEBUG("设置音量: %d", volume);

  /* 软件字段照旧记下来（audio_get_volume 用），再真正下到硬件。
   * 设备 fd 属于 task group，所以这里在本任务里临时 open 一个专门设音量，
   * 设完立刻 close —— 不去碰播放线程/录音封装的 fd。 */

  ctx->config.volume = volume;

  ret = audio_hw_set_volume(ctx, volume);
  if (ret < 0)
    {
      AUDIO_DEBUG("下发硬件音量失败: %d（软件音量已更新）", ret);
    }

  return ret;
}

/**
 * @brief  获取音量
 */

uint8_t audio_get_volume(audio_context_t *ctx)
{
  if (ctx == NULL || !ctx->initialized)
    {
      return 0;
    }

  return ctx->config.volume;
}

/**
 * @brief  启用VAD检测
 */

void audio_vad_enable(audio_context_t *ctx,
                      audio_vad_cb_t callback,
                      void *user_data)
{
  if (ctx == NULL)
    {
      return;
    }

  AUDIO_DEBUG("启用VAD检测");

  ctx->vad_enabled = true;
  ctx->vad_callback = callback;
  ctx->vad_user_data = user_data;
  ctx->vad_speech_active = false;
  ctx->vad_silence_frames = 0;
  ctx->vad_speech_frames = 0;
}

/**
 * @brief  禁用VAD检测
 */

void audio_vad_disable(audio_context_t *ctx)
{
  if (ctx == NULL)
    {
      return;
    }

  AUDIO_DEBUG("禁用VAD检测");

  ctx->vad_enabled = false;
  ctx->vad_callback = NULL;
  ctx->vad_user_data = NULL;
}

/**
 * @brief  设置VAD能量阈值
 */

void audio_vad_set_threshold(audio_context_t *ctx, uint32_t threshold)
{
  if (ctx == NULL)
    {
      return;
    }

  AUDIO_DEBUG("设置VAD阈值: %lu", (unsigned long)threshold);

  ctx->vad_energy_threshold = threshold;
}

/**
 * @brief  把 VAD 里"正在说一句话"的相位收掉（外部提前收尾时用）
 *
 * 说明写在 ai_audio.h 的声明处（为什么要调、为什么只复位相位不回调上层、
 * 为什么只能在录音线程里调）。
 */

void audio_vad_end_speech(audio_context_t *ctx)
{
  if (ctx == NULL)
    {
      return;
    }

  ctx->vad_speech_active = false;
  ctx->vad_silence_frames = 0;
  ctx->vad_speech_frames = 0;
}

/**
 * @brief  计算音频帧能量
 */

uint32_t audio_calc_energy(const int16_t *data, size_t frames)
{
  if (data == NULL || frames == 0)
    {
      return 0;
    }

  return audio_calc_frame_energy(data, frames);
}

/**
 * @brief  获取音频模块状态
 */

audio_state_t audio_get_state(audio_context_t *ctx)
{
  if (ctx == NULL)
    {
      return AUDIO_STATE_UNINIT;
    }

  return ctx->state;
}

/**
 * @brief  获取状态名称字符串
 */

const char *audio_get_state_name(audio_state_t state)
{
  if ((int)state >= 0 && state <= AUDIO_STATE_BOTH)
    {
      return g_state_names[state];
    }

  return "UNKNOWN";
}
