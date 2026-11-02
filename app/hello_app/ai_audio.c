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
static bool audio_record_thread_is_current(const audio_context_t *ctx,
                                           uint32_t my_gen);
static uint32_t audio_calc_frame_energy(const int16_t *data, size_t frames);
static size_t audio_play_chunk_bytes(const audio_context_t *ctx);
static void audio_apply_chunk_fade(audio_context_t *ctx, size_t frames);
static int audio_open_play_device(audio_context_t *ctx);
static void audio_close_play_device(audio_context_t *ctx, int fd);
static void audio_drop_play_device(audio_context_t *ctx);
static int audio_apply_volume(int fd, uint8_t volume);
static int audio_configure_output(int fd, const audio_context_t *ctx);
static int audio_hw_set_volume(const audio_context_t *ctx, uint8_t volume);
static void audio_prepare_output(audio_context_t *ctx);
static void audio_resume_record(audio_context_t *ctx);
static void audio_reap_play_thread(audio_context_t *ctx, const char *who);
static int audio_play_begin(audio_context_t *ctx, const int16_t *src,
                            size_t frames,
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

/* 用户设定的播放音量（0..100），**模块级单值**，不能记在某个 audio_context_t 上。
 *
 * 一份镜像里有两套 audio_context_t：hello_app 常开麦一套（ai_companion_main.c）
 * 和 robot_ui 面板一套（main.c），而设置页的音量滑块只调得到 robot_ui 那个
 * （volume_set_handler 里 audio_set_volume(&g_audio_ctx, ...)）。
 *
 * 起播时下发的音量以前读的是 ctx->config.volume —— 于是常开麦那条每次起播都
 * 把自己那套的默认值 70 写回硬件（audio_apply_volume 在 audio_open_play_device
 * 里每次都跑），把滑块设的值冲掉。表现就是「拖音量对 AI 回话没用」：拖到 30
 * 显示 30，老人听到的还是 70 的响度。而那条常开麦恰恰是老人日常用的那条。
 *
 * 记成模块级单值后，谁起播都用同一个用户音量。顺带修掉另一个隐患：
 * robot_ui 的 audio_config 是按字段初始化的，.volume 没写 = 0，原来起播前
 * 会把「0」当成音量下发（最小档），全靠 touch_ui_set_volume_cb 注册时补一下
 * 才没露馅 —— 现在两者都对齐到这个单值。
 *
 * 起播前每次都重新下发（而不是设一次就不管）是**故意**的：板级报警模块
 * （sf32lb52_alarm.c 的 alarm_audio_open）响铃时会把硬件音量强制写成
 * ALARM_PLAYBACK_VOLUME(800) 并且不还回来，只有下一次起播重新下发才能把它
 * 复位到用户值。 */

static uint8_t g_play_volume = AUDIO_VOLUME_DEFAULT;

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
 * 2026-10-25 改成比**代际号**，不再比 pthread_t。原来的写法是
 * `pthread_equal(ctx->record_thread, pthread_self())`，判据落在一个会被下一次
 * pthread_create() 覆盖的共享字段上；真机上它出现过对**刚创建出来的线程**
 * 判假，于是新一代一进函数就走"上一代录音线程退出…不动任何标志"那条早退，
 * record_exited 永远没人置，audio_record_is_active() 永远报"活着"，
 * 监听守护每 5 秒重开一次、永不升级 —— 一次抖动被放大成永久性"不能对话"
 * （现场证据与读数记在 ai_audio.h 的 record_gen / record_claimed 头上）。
 *
 * 代际号为什么可靠：它由 audio_record_start() 在 pthread_create() **之前**
 * 自增，线程把自己那一代的号从**自己的参数**里抄进局部变量 —— 参数是这一个
 * 线程私有的，之后谁再起新一代都改不到它。比出来的是"我这一代还在不在当班"，
 * 语义正好是这里要的：
 *
 *   - 上一代的线程（回收时放弃 join 被 detach 的那条、或自己判被抢走正在收尾
 *     的那条）在"上层已经起了新一代"之后才醒过来：代际号对不上 → 什么都不碰，
 *     直接退出。该踩的别人才不会被踩：
 *       · audio_in_stop() / audio_in_abandon() 动到的都是**全局那一份**录音会话
 *         （前者发设备级 STOP、后者关掉那个 fd，设备/fd 都只有一份）；
 *       · recording / record_exited / record_died 会被写成新一代的状态。
 *   - 当前这一代：号对得上，正常干活、正常收尾。
 */

static bool audio_record_thread_is_current(const audio_context_t *ctx,
                                           uint32_t my_gen)
{
  return my_gen == ctx->record_gen;
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
 * 只给**建播放会话**用（open_play_device）。
 *
 * ⚠️ 设音量**不要**走这条路（audio_hw_set_volume 现在只发 FEATURE）：
 * 这记 CONFIGURE(OUTPUT) 是 2026-10-21 那次「开机就聋、越修越坏」的毒源。
 * 两条毒：① 上层半双工抢占会把它当"要播放"，把正在跑的录音会话直接 stop 掉；
 * ② 驱动记下 pending_playback=true，而录音重启的 CONFIGURE(INPUT) 又被上层
 * 状态机（state=PREPARED≠OPEN）静默吞掉，于是录音会话带着 playback=1 的标签
 * 启动（串口铁证 "hw_start 真跑了：playback=1"），L5 恢复守卫看着"健康的播放"
 * 永久跳过、RX 从此冻结。
 *
 * 音量落哪条通路由驱动自己判（sf32lb52_audio_configure 的 rec_dir 身份判据：
 * 只有**正在读这条录音通路的那条线程**来设才算录音增益，其余一律记播放音量），
 * 不需要上层标方向 —— 标方向的代价就是上面那两条毒。
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
 *
 * **只发 AUDIO_TYPE_FEATURE / AUDIO_FU_VOLUME，绝不先 CONFIGURE(OUTPUT)。**
 * FEATURE 在上层是无条件放行的（audio.c 的转发门只拦 INPUT/OUTPUT），也不会
 * 触发抢占、不会写 pending_playback；落 DAC 还是 ADC 由驱动按身份判据自己定
 * （见 audio_configure_output 头上那段）。原来那句 CONFIGURE(OUTPUT) 是
 * 「音量滑块一动、麦克风就聋」的整条毒链起点，2026-10-21 删掉。
 */

static int audio_hw_set_volume(const audio_context_t *ctx, uint8_t volume)
{
  int fd;
  int ret;

  (void)ctx; /* 方向改由驱动的身份判据决定，这里不再需要采样率/channels */

  fd = open(AUDIO_PLAY_DEVICE, O_WRONLY);
  if (fd < 0)
    {
      int errcode = errno;

      AUDIO_DEBUG("打开 %s 失败: %d", AUDIO_PLAY_DEVICE, errcode);
      return -errcode;
    }

  ret = audio_apply_volume(fd, volume);
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
  uint32_t t0 = audio_now_ms_nonzero();
  uint32_t t_cfg = 0;     /* 三段拆解计时（2026-10-30）：重配 / 音量 / START */
  uint32_t t_vol = 0;
  uint32_t t_start = 0;

  /* ★ 默认「不是保温取来的」，只有下面那条保温分支返回前才会置真。
   * 判据用途见 ai_audio.h 的 play_open_warm：写循环靠它决定 write 失败时
   * 允不允许补一次完整热启。放在这里置假，是因为保温分支紧跟着就 return，
   * 另外两条路（复用 / 全开）都从这儿往下走、不会再改它。 */

  ctx->play_open_warm = false;

#if AUDIO_PLAY_DEVICE_CACHE
  /* 上一块留下的 fd 还在：设备和方向都配好了，这一块只重配一下方向再起。
   *
   * 省掉的是 open() + close() 一整圈。START/STOP 照旧，会话记账每块都回
   * OPEN，半双工的让路/还麦一字未动。
   *
   * ⚠ 判据别再按"省掉约 600 ms"算：实测「全开」280~803 ms 里的大头在
   * START 里面那段 hw_start 上电时序（开 ADC 通路 / 开 DAC 通路 / 开功放），
   * 那个无论如何每块都要做。这里省的只是 open+close，几十毫秒量级。
   *
   * 判据：串口看 [AUDIO] 播放设备 那行，块 2 起应该是「复用」。 */

#if AUDIO_PLAY_KEEP_WARM
  /* ★ 保温直取（2026-10-30，用户拍板「要动」）：上一块收尾时**没有** STOP，
   * 设备这会儿还是 RUNNING —— 这一块连 CONFIGURE / START 都不用发，只把音量
   * 再下发一次就直接写。
   *
   * 砍掉的是每块 391~522 ms 的「重配 + 音量 + START」，其中大头在 START 里
   * 那段 hw_start 上电时序（开 ADC 模拟通路 / 开 DAC 模拟通路 / 开功放 /
   * 30ms + 20ms 建立时间）。同时功放不再每块重开一次，那声「啪」也没了。
   *
   * 音量**照样每块重发**：板级报警（sf32lb52_alarm.c）会把设备级的
   * playback_db 强制写成 800 且不还回来，报警若夹在两块之间，不重发就会按
   * 800 响。AUDIO_TYPE_FEATURE 不属于 INPUT/OUTPUT，audio.c 的转发门永远
   * 放行它，running 时发也是纯寄存器写（见 hw_start 里 hw_set_volume 那段），
   * 几毫秒的事。
   *
   * 判据：串口看 [AUDIO] 播放设备 那行，一轮里块 2 起应该是「保温直取」，
   * 拆解计时里的「共 X ms」应该只剩音量那一段（几 ms）。 */
  if (ctx->play_fd >= 0 && ctx->play_fd_running)
    {
      uint32_t t_warm0 = audio_now_ms_nonzero();
      uint32_t t_warmv;

      ret = audio_apply_volume(ctx->play_fd, g_play_volume);
      t_warmv = audio_now_ms_nonzero();
      if (ret != OK)
        {
          printf("[AUDIO] 播放音量下发失败 (%d)：这段可能偏小甚至听不见\n",
                 ret);
        }

      ctx->play_open_warm = true;   /* ★ 写循环据此允许「write 失败就补热启」 */
      printf("[AUDIO] 播放设备：保温直取（音量 %lu ms，省掉重配+START，"
             "连功放都不断 → 没有「啪」）\n",
             (unsigned long)(t_warmv - t_warm0));
      return ctx->play_fd;
    }
#endif

  if (ctx->play_fd >= 0)
    {
      /* ★ 2026-10-26：复用分支必须**先 CONFIGURE(OUTPUT) 再 START**，
       * 否则 START 恒失败，fd 复用从来没生效过。
       *
       * 根因在驱动（nuttx/audio/audio.c 的 audio_start）：
       *     if (upper->status->state == AUDIO_STATE_OPEN) return -EPERM;
       * 上一块收尾时 audio_close_play_device() 发的 AUDIOIOC_STOP 会把
       * upper->status->state 带回 OPEN，而复用分支原来只发 音量+START ——
       * 状态停在 OPEN，START 必然吃 -EPERM。真机铁证就是每块都打
       *     [AUDIO] 缓存的播放设备起不动 (1)，摘掉重开
       * （那个 (1) 就是 EPERM），一次「播放设备：复用」都没有。
       *
       * CONFIGURE(OUTPUT) 正是把它从 OPEN 推到 PREPARED 的那一步
       * （audio.c 里 audio_setstate(upper, AUDIO_STATE_PREPARED)）。驱动的
       * 转发门对 OPEN / PREPARED 都放行，所以在 PREPARED 重配是**合法**的；
       * 半双工抢占那条支路也踩不到（它要求 status==RUNNING）。 */
      ret = audio_configure_output(ctx->play_fd, ctx);
      t_cfg = audio_now_ms_nonzero();
      if (ret != OK)
        {
          printf("[AUDIO] 缓存的播放设备重配方向失败 (%d)，摘掉重开\n", ret);
          close(ctx->play_fd);      /* 见下面「摘掉时必须 close」 */
          ctx->play_fd = -1;
          ctx->play_fd_running = false;
        }
      else
        {
          /* 音量**每块都重发**，不跟着 open 一起省掉。
           *
           * 为什么不能省：板级报警（sf32lb52_alarm.c 的 alarm_audio_open）
           * 响铃时会把驱动的 playback_db 强制写成 ALARM_PLAYBACK_VOLUME(800)
           * 并且不还回来，而 playback_db 是**设备级**的（priv 就是 dev）。
           * 轮内要是复用了不重发的 fd，报警之后那几块就会按 800 响，滑块设的
           * 值又不管用了 —— 那正是上一版刚修好的 bug，不能在这里复辟。
           *
           * 代价只是一个 ioctl：AUDIO_TYPE_FEATURE 不属于 INPUT/OUTPUT，
           * audio.c 的转发门永远放行它，几毫秒的事。 */
          ret = audio_apply_volume(ctx->play_fd, g_play_volume);
          t_vol = audio_now_ms_nonzero();
          if (ret != OK)
            {
              printf("[AUDIO] 播放音量下发失败 (%d)：这段可能偏小甚至听不见\n",
                     ret);
            }

          if (ioctl(ctx->play_fd, AUDIOIOC_START, 0) == 0)
            {
              t_start = audio_now_ms_nonzero();
              ctx->play_fd_running = true;   /* ★ 保温用：这一下真的起来了 */

              /* ★ 2026-10-30 拆解计时：这行原来只打一个总数（391~522 ms），
               * 归不到实处就只能猜。板级 hw_start 里**刻意**的上电延时只有
               * 30+20 = 50 ms（sf32lb52_audio.c 的 SETTLE_MS + 等功放），
               * 所以大头不在那儿。现在把三段分开打，下一轮照着数决定砍哪儿。
               * 顺带把 START 之后这段（回到调用方之前）也量进去 —— 它同样在
               * 「尽快出声」的关键路径上。
               * 不改任何行为，只把一行日志拆细。 */
              printf("[AUDIO] 播放设备：复用（重配 %lu ms + 音量 %lu ms + "
                     "START %lu ms，共 %lu ms，省掉 open/close）\n",
                     (unsigned long)(t_cfg - t0),
                     (unsigned long)(t_vol - t_cfg),
                     (unsigned long)(t_start - t_vol),
                     (unsigned long)(t_start - t0));
              return ctx->play_fd;
            }

          /* ★ 摘掉时**必须 close**（2026-10-26 修，原来是不 close 的）。
           *
           * 原注释的理由是"上一代播放线程收尾时会认领并关掉它"，但那条路
           * 根本走不到：audio_close_play_device() 里那句
           *     if (ctx->play_fd == fd) return;
           * 就是"留给缓存"——上一代**已经**在它自己的收尾里把这个 fd 交给
           * 缓存、跳过了 close。
           * 于是在这里摘掉而不关 = **每块漏一个 fd**（一轮七八块就漏七八个），
           * 早晚把 fd 表耗干 —— 那正是"玩一会儿就不能对话了"的一种形状。
           *
           * 为什么在这里关是安全的（不会双重 close）：能走到 audio_open_play_device
           * 的新播放线程，一定是在 audio_play_begin() 里 reap 掉上一代之后才
           * 创建的（reap 放弃 join 时它直接返回 -EBUSY、压根不建新线程）；而
           * 上一代若被换代顶掉，它自己那句 audio_close_play_device 因
           * `play_gen != my_gen` **根本不会执行**，也不会来关。所以此刻这个 fd
           * 没有第二个人会关，只有我们能关。 */
          printf("[AUDIO] 缓存的播放设备起不动 (%d)，摘掉重开\n", errno);
          close(ctx->play_fd);
          ctx->play_fd = -1;
          ctx->play_fd_running = false;
        }
    }
#endif

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
   * 也可能是这个原因，所以照样留一行。
   *
   * 这里读的是模块级 g_play_volume，不是 ctx->config.volume —— 理由见它
   * 定义处：两套 ctx 各存一份必然漂，滑块只管得到 robot_ui 那套。
   *
   * ★ 全开和复用**两条路都重发**这一下（见上面复用分支里的说明：报警会把
   * 设备级的 playback_db 写成 800 不还回来）。 */

  ret = audio_apply_volume(fd, g_play_volume);
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

  /* ★ 保温标记：START 真成功了才算 RUNNING（和上面复用分支同一条判据）。 */
  ctx->play_fd_running = true;

#if AUDIO_PLAY_DEVICE_CACHE
  /* 只有**一路顺风**才缓存：上面任何一步失败都直接 close 退回了，
   * 不会留下一个配了一半的 fd 让下一块去复用。 */
  ctx->play_fd = fd;
#endif

  printf("[AUDIO] 播放设备：全开（open+CONFIGURE+音量+START 共 %lu ms）\n",
         (unsigned long)(audio_now_ms_nonzero() - t0));
  return fd;
}

/**
 * @brief  一块放完了：停设备，fd 按 AUDIO_PLAY_DEVICE_CACHE / AUDIO_PLAY_KEEP_WARM
 *         决定留还是关
 *
 * **STOP 在"整轮播报中"可以省**（2026-10-30，AUDIO_PLAY_KEEP_WARM）：块与块
 * 之间没人要这台设备 —— audio_resume_record() 在 record_hold 为真时本来就
 * 早退（"整轮播报还没结束，暂不还麦"），所以这一刻举着 RUNNING 挡不住任何人。
 * 省掉 STOP 之后下一块走「保温直取」，砍掉 391~522 ms 的重配+START，还免了
 * 功放重开那声「啪」。
 *
 * **STOP 不能无条件省**：不停的话这个 fd 的会话一直举着 RUNNING，而
 * audio_stop() 的放行判据是"别的 fd 的 nstate <= PREPARED"（nuttx/audio/
 * audio.c），它一个人就能把所有 STOP 挡在门外 —— 录音侧从此钉死，那正是
 * 2026-10-26 那次「AUDIO_IN: started 打了 22 次、hw_start 一次都没有」的
 * 形状。所以保温只在 record_hold 为真时做；一出整轮播报（record_hold 被
 * round_hold_give 清掉、或空档超 8 秒提前还麦），这里就照旧 STOP，
 * priv->state 落回 OPEN，录音侧的放行判据永远不变。
 *
 * 只能在持有该 fd 的播放线程里调用。
 */

static void audio_close_play_device(audio_context_t *ctx, int fd)
{
  if (fd < 0)
    {
      return;
    }

#if AUDIO_PLAY_KEEP_WARM
  /* 保温：整轮播报还没结束，而且这个 fd 就是缓存那个 → 不 STOP，留给下一块。
   * 三个条件缺一不可：
   *   - record_hold：块间没人要设备，举着 RUNNING 不挡谁（上面那段）；
   *   - ctx->play_fd == fd：不是缓存那个就说明这一代已被顶掉，别人的设备
   *     不能替它保温；
   *   - play_fd_running：本来就没起来就别记账，免得下一块「以为在跑」。 */
  if (ctx->record_hold && ctx->play_fd == fd && ctx->play_fd_running)
    {
      /* 不打每块一行：一轮七八块会刷屏。真正的判据是上层那行
       * 「播放设备：保温直取」和变短的块间空档。 */
      AUDIO_DEBUG("整轮播报中，播放设备保温不 STOP");
      return;
    }
#endif

  /* 顺序和录音一样：先 AUDIOIOC_STOP 再 close。
   * 注意这个 STOP 会同时停掉录音通路（半双工），所以它必须是播放的最后一步。 */

  ioctl(fd, AUDIOIOC_STOP, 0);
  ctx->play_fd_running = false;   /* ★ 这一下真的停了 */

#if AUDIO_PLAY_DEVICE_CACHE
  /* 还是当前缓存的那个 → 留给下一块。此刻它在 audio.c 里已经是 OPEN 态，
   * 和"关掉再打开"的会话状态完全一致，不会变成陈旧记账。 */
  if (ctx->play_fd == fd)
    {
      return;
    }

  /* 不是当前缓存（这一代已经被顶掉、缓存早换人了）→ 这个 fd 只能自己关。
   * 走到这里说明有人把缓存摘掉了，见 audio_open_play_device 里那条注释。 */
#endif

  close(fd);
}

/**
 * @brief  真关缓存的播放设备（收摊用）
 *
 * 调用点只有两个：audio_play_stop()（上层明确掐掉这一轮）与 audio_deinit()。
 * **不能在还有播放线程活着的时候调** —— 那条线程可能正在 write 这个 fd，
 * 跨线程 close 它就是踩 fd（本项目被这条咬过）。所以调用方先 reap，确认
 * play_exited 之后才来。
 */

static void audio_drop_play_device(audio_context_t *ctx)
{
  int fd;

  if (ctx == NULL)
    {
      return;
    }

  fd = ctx->play_fd;
  if (fd < 0)
    {
      /* 没有缓存的 fd 就没有保温可言 —— 顺手把可能残留的标记清掉，
       * 免得下一次 audio_open_play_device 读到「以为在跑」。 */
      ctx->play_fd_running = false;
      return;
    }

  /* 先摘再关：万一有人正好在复用它，看到 -1 就会走全新 open，不会用到
   * 一个正在被关掉的 fd。 */
  ctx->play_fd = -1;
  ctx->play_fd_running = false;

  ioctl(fd, AUDIOIOC_STOP, 0);
  close(fd);

  printf("[AUDIO] 播放设备已关闭（缓存清空）\n");
}

/**
 * @brief  强制回收被陈旧记账钉死的音频上半部（2026-10-26）
 *
 * **症状**：某个 fd 的 priv->state 卡在 RUNNING，而设备上其实一个会话都没有
 * （串口判据：`armed=0 busy=0`）。它一个人就能把 audio_stop() 的放行判据
 * （`audio_getnstate(其它 fd) <= PREPARED`，见 nuttx/audio/audio.c）恒判为假：
 * 硬件永远停不下来、COMPLETE 永远不发、上层状态机钉在 RUNNING，此后每一次
 * CONFIGURE/START 都被那道门**静默吞掉却照样返回 OK**。真机指纹就是
 * 「AUDIO_IN: started」刷屏而「hw_start 真跑了」一次都没有，监听守护每 5 秒
 * 判定一次僵尸会话重开一圈，**麦克风永久失聪** → 没轮次 → 没回复也没提示音。
 *
 * **内核里对症的药早就有了**：audio_configure() 的半双工抢占路径会
 * `lower->ops->stop()` + `audio_setstate(PREPARED)` + `audio_forget_sessions()`，
 * 把其它 fd 的陈旧记账清回 OPEN。**但它只在「OUTPUT 抢 INPUT」时触发**，
 * 而这个死锁里播放永远不会发生（麦聋了 → 不起轮次 → 不播），于是那条自愈
 * 一辈子等不到 —— 这里就是替它把那一下主动打出去：开一个**一次性**的
 * OUTPUT fd、发一次 CONFIGURE(OUTPUT)，正好踩进那条自愈路径，然后立刻收摊。
 *
 * **幂等**：设备本来就好好的时候，这一下相当于正常的半双工抢占（无害），
 * 随后 STOP + close 归还，不会留下任何常驻 fd。所以可以每轮僵尸自愈都调。
 *
 * @param why  只用来打日志，说明是哪条判据触发的回收
 *
 * @return 0 表示这一下打出去了（不保证回收成功，看随后串口有没有
 *         `audio: 清掉 N 个陈旧的会话记账`）；<0 表示设备都打不开。
 */

int audio_reclaim_stale(const char *why)
{
  struct audio_caps_desc_s capdesc;
  int fd;
  int ret;

  fd = open(AUDIO_PLAY_DEVICE, O_WRONLY);
  if (fd < 0)
    {
      printf("[AUDIO] 强制回收陈旧会话（%s）：打不开 %s (%d)\n",
             why != NULL ? why : "?", AUDIO_PLAY_DEVICE, errno);
      return -errno;
    }

  /* 只发一次 CONFIGURE(OUTPUT) —— 这一步才是踩响内核自愈的那一下。
   * 参数不用精：抢占分支只看 ac_type，转发过来的 CONFIGURE 由驱动自己
   * 忽略也无所谓，我们紧接着就收摊。 */

  memset(&capdesc, 0, sizeof(capdesc));
  capdesc.caps.ac_len            = sizeof(struct audio_caps_s);
  capdesc.caps.ac_type           = AUDIO_TYPE_OUTPUT;
  capdesc.caps.ac_channels       = 1;
  capdesc.caps.ac_controls.hw[0] = 16000;
  capdesc.caps.ac_controls.b[2]  = 16;

  ret = ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&capdesc);

  printf("[AUDIO] 强制回收陈旧会话（%s）：CONFIGURE(OUTPUT)=%d，"
         "看上面有没有「清掉 N 个陈旧的会话记账」\n",
         why != NULL ? why : "?", ret);

  ioctl(fd, AUDIOIOC_STOP, 0);
  close(fd);
  return ret;
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
      /* 已经没在录：没什么可停的。★ record_resume_on_play_end **不清** ——
       * 它可能正是上一段让路欠下的那次恢复（流水线 TTS 的首句→尾段空档被
       * record_hold 住时就是这个形状，见 audio_resume_record）。清掉就等于
       * 把常开监听永久丢掉，再也没人开麦。 */
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

  /* ★ 整轮播报还没收尾（流水线 TTS 的首句→尾段空档）：这一段让路要留着。
   * record_resume_on_play_end 保持 true = "还欠一次还麦"，等轮子收尾由上层
   * audio_record_hold_end() 补上。这里**不清**这一位是关键 —— 清了就再也没
   * 人开麦，常开监听直接哑掉。 */

  if (ctx->record_hold)
    {
      AUDIO_DEBUG("整轮播报未结束，暂不还麦");
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
 * @brief  解除"整轮播报"的让路锁，并把欠着的那次还麦补上
 *
 * 用法与设计动机见 ai_audio.h 的声明处。幂等：没在 hold 时就是 no-op。
 */

void audio_record_hold_end(audio_context_t *ctx)
{
  if (ctx == NULL || !ctx->record_hold)
    {
      return;
    }

  ctx->record_hold = false;

  /* ★ 保温兜底（2026-10-30）：让路锁到此为止，那台一直举着 RUNNING 的播放
   * 设备必须收掉 —— 不收的话它要么挡住接下来的录音 START，要么就带着功放
   * 一直开着（PA 常通会发热、也会把底噪送进喇叭）。
   *
   * audio_resume_record → audio_record_start 一路走到那里也会做同一件事
   * （见那里的说明），这里是第二道：覆盖 audio_resume_record 早退的那两种
   * 情况（没欠还麦 / 还麦失败）。幂等，多调无害。
   *
   * 只在没在播的时候动：playing 为真说明播放线程还在写这个 fd，跨线程
   * STOP/close 就是踩 fd（本项目被这条咬过）。那种情况由 audio_record_start
   * 的 ctx->playing 分支先 reap 再丢。 */
  if (!ctx->playing && ctx->play_fd_running)
    {
      audio_drop_play_device(ctx);
    }

  audio_resume_record(ctx);
}

/**
 * @brief  录音线程的启动参数（堆上一份，线程起跑即刻抄进局部变量后释放）
 *
 * 为什么不直接把 audio_context_t* 当参数传：线程还要知道自己是**第几代**。
 * 代际号必须是这个线程私有的，不能是"醒来之后再去读共享字段"——共享字段会被
 * 下一次 audio_record_start() 改掉，那正是 2026-10-25 那次"新一代被判成上一代"
 * 的形状（详见 audio_record_thread_is_current 头上）。
 */

typedef struct
{
  audio_context_t *ctx;
  uint32_t         gen;
} record_thread_arg_t;

/**
 * @brief  录音线程
 */

static void *audio_record_thread(void *arg)
{
  record_thread_arg_t *ta  = (record_thread_arg_t *)arg;
  audio_context_t     *ctx = ta->ctx;
  uint32_t             my_gen = ta->gen;   /* 抄成私有，之后不再看 ta */
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

  free(ta);                    /* 参数用完就还，之后所有退出路径都不用再管它 */

  /* 起跑认领：只有**确实是当前这一代**才置 record_claimed。
   * 这一步放在进读循环之前，是为了让"这一代到底有没有真跑起来"有个不依赖
   * 线程收尾的实证 —— 没认领就退的那一代，audio_record_is_active() 报假，
   * 监听守护立刻按"要常听却没在录"重开，自愈；而不是像 2026-10-25 现场那样
   * 把假健康一直挂着、每 5 秒空转一圈。 */

  if (!audio_record_thread_is_current(ctx, my_gen))
    {
      printf("[录音] ★起跑即被换代（我这代 %lu，当前 %lu）：不碰设备、"
             "不动任何标志，直接退出\n",
             (unsigned long)my_gen, (unsigned long)ctx->record_gen);
      return NULL;
    }

  ctx->record_claimed = true;

  printf("[录音] 录音线程起跑并认领（代=%lu）\n", (unsigned long)my_gen);

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

      if (!audio_record_thread_is_current(ctx, my_gen))
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
              uint32_t thr;

              /* ===== 自适应噪声门（2026-10-25）=====
               *
               * 先学底噪、再算门槛、最后才判有声/无声。为什么必须有这一段
               * （以及 2026-10-24 把阈值写死成 300 是怎么把静音判停变成死代码的）
               * 全记在 ai_audio.h 的「VAD 参数」那一段 —— 那里是唯一真相，
               * 这里只写怎么算，不再复述。
               *
               * 两条纪律：
               *   ① 只有**明确不是人声**的帧才进统计（energy <= LEARN_BLOCK）。
               *      人声/突发噪声混进来会把门槛抬到人声量级，之后小声说话
               *      全被自己挡掉 —— 自适应门最常见的自杀方式。
               *   ② 只在「还没判成说话」时更新。判成说话之后再学，学到的
               *      就是这一句人声本身。
               *
               * 用 int64 中转是因为 energy 与底噪都是 uint32，直接相减会在
               * energy < 底噪时下溢成天文数字。 */

              if (energy <= VAD_NOISE_LEARN_BLOCK && !ctx->vad_speech_active)
                {
                  int64_t f = (int64_t)ctx->vad_noise_floor_e;

                  f += ((int64_t)energy - f) / 32;
                  if (f < 0)
                    {
                      f = 0;
                    }

                  ctx->vad_noise_floor_e = (uint32_t)f;
                }

              thr = ctx->vad_noise_floor_e * VAD_NOISE_FLOOR_MULT;
              if (thr < AUDIO_VAD_ENERGY_THRESHOLD)
                {
                  thr = AUDIO_VAD_ENERGY_THRESHOLD;
                }

              ctx->vad_energy_threshold = thr;

              if (energy > thr)
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
                      /* 这一行**故意走 printf 而不是 AUDIO_DEBUG**：本构建没开
                       * AUDIO_DEBUG（见本文件 457 行附近），而门槛/底噪这两个数
                       * 是「自适应门调得对不对」的唯一判据 —— 换个屋子要是不灵，
                       * 串口上得能一眼看出是门槛飘了还是人声真小。每段语音一行，
                       * 密度合适。 */

                      printf("[VAD] 判到说话（帧能量 %lu > 门槛 %lu，"
                             "底噪 %lu）\n",
                             (unsigned long)energy, (unsigned long)thr,
                             (unsigned long)ctx->vad_noise_floor_e);

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
                      /* 同上走 printf：这是「自动断句到底有没有触发」的唯一凭据。
                       * 2026-10-25 之前这句是 AUDIO_DEBUG，所以那条死代码在串口上
                       * 连痕迹都没留 —— 一直以为是"等 8 秒太慢"，其实是从来没判过。 */

                      printf("[VAD] 判到说完（静音 %u 帧 ≥ %u，即 %u ms）\n",
                             (unsigned)ctx->vad_silence_frames,
                             (unsigned)silence_frames,
                             (unsigned)ctx->record_cfg.silence_timeout_ms);

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

  if (stale || !audio_record_thread_is_current(ctx, my_gen))
    {
      /* 这一行带**代际号**：2026-10-25 现场只有一句"上一代录音线程退出"，
       * 分不清打它的是真正上一代的残影、还是刚起跑就被判负的新一代 ——
       * 而这两件事的处置完全不同（前者是正常收场，后者是根因）。带上号，
       * 下次一眼分清。 */

      printf("[录音] 上一代录音线程退出（当前会话已由新一代接管）："
             "不碰设备，也不动任何标志（我这代 %lu，当前 %lu%s）\n",
             (unsigned long)my_gen, (unsigned long)ctx->record_gen,
             ctx->record_claimed ? "" : "，且我从未认领");

      /* 没认领就退的那一代：把"没真跑起来"如实留成假账本里那个洞被堵上的样子。
       * record_claimed 由 audio_record_start() 起手清成 false，所以这里不用改它
       * —— 它本来就是 false。真正要防的是**认领过**却中途被判换代：那时账本
       * 归新一代，同样不能碰。 */
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

  /* 死因再细分一层：被抢走的（stolen）是**半双工的正常让路**，不是故障 ——
   * 对面多半是正在播报的那一路按设计接管了 /dev/audio/audio0。
   * 监听守护靠这一位决定"要不要去抢设备"（见 ai_audio.h 的 record_stolen：
   * 2026-10-29 报警按钮那一场 28 次「异常中断」就是没分这一层，
   * 把"让路"当成"死了"去紧急重开抢回来，把正在放的报警声打死了）。 */

  if (stolen)
    {
      ctx->record_stolen = true;
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
  uint32_t my_gen = ctx->play_gen;  /* 本线程属于哪一代，见 play_gen 说明 */
  size_t frame_bytes = ctx->config.channels * sizeof(int16_t);
  size_t total_bytes = ctx->play_frames * frame_bytes;
  size_t chunk_bytes = audio_play_chunk_bytes(ctx);
  size_t done = 0;
  int fail = 0;                  /* 0=没失败；负值=没出声的原因，最后写进 ctx */
  int healed = 0;                /* ★ 保温自愈只补一次热启（2026-10-30），见下面 */
  int fd;

  /* 数据源只认 play_src（拷贝路径＝play_buf，零拷贝路径＝调用方缓冲，
   * 两条路都在起播前设好）。play_src 万一没设（防御），退回 play_buf。 */

  const char *src = (const char *)(ctx->play_src != NULL
                                   ? ctx->play_src : ctx->play_buf);

  if (chunk_bytes == 0)
    {
      chunk_bytes = total_bytes;   /* 防御：别写出 0 字节的空块 */
    }

  AUDIO_DEBUG("播放线程启动 (%zu 帧)", ctx->play_frames);

  ctx->play_stage = PLAY_STAGE_OPEN;   /* 诊断标记，见 play_stage 说明 */
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
      ctx->play_stage = PLAY_STAGE_WRITE;  /* 设备开好了，进写数据阶段 */

      /* 代数不等 = 已经被下一代作废（audio_reap_play_thread 解闩放行过）：
       * 立刻停手，别再往设备里灌数据、也别走后面的完成回调。 */
      while (!ctx->play_stop && ctx->playing && done < total_bytes &&
             ctx->play_gen == my_gen)
        {
          size_t chunk = total_bytes - done;
          ssize_t written;

          if (chunk > chunk_bytes)
            {
              chunk = chunk_bytes;
            }

          written = write(fd, src + done, chunk);

          /* write() 同步：返回 <= 0 说明设备被别的会话占了或出错
           * （板级 alarm 同一招：关掉 fd，剩下的静默走完）。 */

          if (written <= 0)
            {
              /* ★ 保温直取的自愈（2026-10-30）：write 立刻返回 0 多半不是
               * 坏了，而是**这个 fd 在块间被别的会话停了**。play_fd_running
               * 是 ctx 自己记的账，而设备是两个 ctx 共享一台 —— robot_ui 的
               * 提醒铃/报警会在块间空档里抢走它并发 AUDIOIOC_STOP，账上
               * 「在跑」和硬件上「已停」就此脱节，sf32lb52_audio_write() 一看
               * !priv->running 直接 return 0，整段一声没出。
               * 真机铁证（2026-10-30 17:37 那一轮）：
               *     [AUDIO] 播放设备：保温直取（音量 1 ms…）
               *     [AUDIO] 播放没出声：write 返回 0（已写 0/286720 字节…）
               * 往前 20 行正是 [Reminder] 语音播报那一路的 hw_stop。
               *
               * 补救：摘掉这个陈旧 fd，补一次**完整热启**，重写同一块。
               * 只在「这次是保温取来的」（play_open_warm）时候补 ——
               * 冷路（复用 / 全开）刚 START 过还写不进就是真故障，重开救不
               * 回来，照旧报「播放没出声」走人，和改动前一字不差。
               *
               * 代价账（这是它敢动的原因）：撞上这一次就补付 ~455 ms 的全开，
               * 正是改动前**每一块**都要付的那个数；撞不上就白赚 454 ms 的
               * START。**任何情况下都不劣于现状**，符合「更稳定更流畅」。
               *
               * 只补一次：补完的设备是刚 START 起来的，再写不进就是真坏了。 */

              if (ctx->play_open_warm && !healed)
                {
                  healed = 1;
                  printf("[AUDIO] 保温的播放设备其实已停（write 返回 %zd，"
                         "已写 %zu/%zu）——块间被别的会话停了，摘掉补一次"
                         "完整热启后重写这一块\n",
                         written, done, total_bytes);

                  /* 摘这个陈旧 fd：**先把两个标记清干净**再走
                   * audio_close_play_device —— 不清的话它那条「整轮播报中
                   * 保温不 STOP」的分支会连 close 一起跳过，就漏 fd 了
                   * （本项目被漏 fd 咬过，见 audio_open_play_device 里那段
                   * 「每块漏一个 fd」的注释）。play_fd 置 -1 之后，它下面
                   * 那句「还是缓存那个就留给下一块」也不成立，于是走真
                   * STOP + close，一步不漏。 */

                  ctx->play_fd = -1;
                  ctx->play_fd_running = false;
                  audio_close_play_device(ctx, fd);
                  fd = -1;

                  fd = audio_open_play_device(ctx);  /* 这次一定走全开 */
                  if (fd >= 0)
                    {
                      continue;   /* 重写同一块，done 一动不动 */
                    }

                  /* 起不来：audio_open_play_device 已经打了「播放没出声：
                   * 打不开 / 配输出方向失败 / START 被拒」那一行，这里不重复。 */
                  fail = fd;
                  break;
                }

              fail = -EIO;
              printf("[AUDIO] 播放没出声：write 返回 %zd"
                     "（已写 %zu/%zu 字节，这段只出了一部分或一声没出）\n",
                     written, done, total_bytes);
              break;
            }

          done += (size_t)written;

          /* 推进打卡：每写完一块盖一章，audio_play_watchdog_tick() 靠它判
           * "这条线程还活着"。卡在 write() 里就盖不了章，看门狗自然会响。 */

          ctx->play_progress_ms = audio_now_ms_nonzero();
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
   * 所以它必须是播放的最后一步。
   *
   * ★ 2026-10-26：fd 不再在这里 close，改由 AUDIO_PLAY_DEVICE_CACHE 决定
   * 留给下一块（见 audio_close_play_device）。而**被换代顶掉的那一代什么都不
   * 动** —— play_begin 起新代之前会 reap 我，正常情况下我早就跑完收尾了；
   * 走到这里还换了代，只可能是 reap 放弃了有界 join（我卡在设备里）。
   * 那种情况下设备和 ctx->play_fd 都已经归下一代了，我再 STOP/close 就是
   * 和它抢同一台设备、甚至双重 close 同一个 fd。静默交出去即可。 */

  ctx->play_stage = PLAY_STAGE_CLOSE;  /* 诊断标记：进收尾（STOP/close） */

  if (ctx->play_gen == my_gen)
    {
      audio_close_play_device(ctx, fd);
    }

  /* 结果落定：必须在 audio_resume_record() / play_cb **之前** ——
   * 回调里（或回调返回后立刻）读到的要是最终值，不能是中转值。
   *
   * 判据是"整段有没有写进设备"，不是"线程收没收尾"：
   *   - 有失败码（打不开 / START 被拒 / 写失败）→ 原样记下，就是"没出声"；
   *   - 没失败码但没写满 → 只可能是被 audio_play_stop() 砍断的（写失败那条
   *     上面已经排除了），记 -ECANCELED：不算放完，也不算"出错"；
   *   - 写满了 → OK（**即**播放线程收尾时 play_stop 才置位，也照样是"整段
   *     都进了设备"，不该因为一个收尾竞态被报成"被打断"）。 */

  /* ★ 2026-10-29：下面这几个共享字段只许**当前那条**播放线程写。
   *
   * 原来这里是无条件的：被换代作废的那条 T1 一退出，就把 T2 的
   * `playing` 清成 false、`play_last_result` 覆盖成自己的、`state` 也改掉。
   * T2 下一次块间隙一看 `playing==false`，自己也退出了 —— 于是**整段只写出
   * 一个块**，串口就是那句「播放没出声：写了 8000/76800 字节就退出循环了」。
   * 2026-10-29 真机一次里连着两条，两次的 total 还都不一样（81920 / 76800），
   * 那就是 T1 读到的 play_frames 已经被 T2 覆盖过 —— 两条线程在并发。
   *
   * 同文件 1617 行的 `play_exited` 早就因为**同一个坑**修过一次（见那里的长
   * 注释），当时用 pthread_equal 做"我是不是当前那条"的判据。这里照抄那个
   * 判据：pthread_create 覆盖 ctx->play_thread 就是天然的换代信号，语义统一。
   *
   * 被换代的那条该做的只有：不碰共享状态、不恢复录音（T2 正在放）、不回调
   * （下面本来就有判据）、别置 play_exited（下面本来就有判据）—— 静默走完。 */

  if (pthread_equal(pthread_self(), ctx->play_thread))
    {
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
       * 顺序要点：必须先把 playing 置 false —— 否则 audio_record_start() 的
       * 半双工会反过来再停一次播放，连带把下面的 play_cb 也吞掉。
       * 被 audio_play_stop() 打断的播放不恢复：那是上层主动要停。 */

      if (!ctx->play_stop)
        {
          audio_resume_record(ctx);
        }
    }

  /* 播放完成回调（在播放线程里回调，调用者可以放心紧跟着开录音）。
   * 语义不变：失败也回调 —— 上层（hello_app 的 TTS 状态机）靠它收尾，
   * 吞掉它会让人一直卡在"正在说话"。想区分成败请读 play_last_result。 */

  /* 作废代（已被下一代顶掉）不许回调：那会把状态机再推一遍，
   * 表现是"没人说话它自己响一下 / 状态乱跳"。 */
  if (!ctx->play_stop && ctx->play_gen == my_gen && ctx->play_cb)
    {
      ctx->play_cb(ctx->play_user_data);
    }

  /* 最后一步：告诉 audio_reap_play_thread() "我已经跑完了"。
   * 必须在所有收尾动作（关播放设备 / 恢复录音 / 完成回调）**之后**置位 ——
   * 回收的人看到它就是直接 join 走 TCB，早置一位等于把线程剩下的动作甩给一个
   * 随时可能被释放的上下文。和录音线程末尾那个 record_exited 是同一套语义。
   *
   * ★ 只在自己仍是"当前那条播放线程"时才置位（2026-10-24 修）。
   *
   * 流水线 TTS 的首句→尾段是**在播放线程里重开一段播放**的（play_cb 里起尾段，
   * 走 audio_play_begin 的"自己调自己"分支）：那时 ctx->play_thread 已经被
   * 换成了**新线程 T2** 的 id，而本线程 T1 是它的上一代。
   *
   * 旧写法无条件写 play_exited = true，等于 T1 一退出就把 T2 标成"已退出"，
   * 可 T2 还在放尾段。此后任何 audio_play_stop() / 下一轮 audio_play_begin()
   * 去 reap，看到 play_exited 为真就**跳过有界等待直接 pthread_join**，join 的
   * 却是正在播的 T2 —— 轻则调用者被钉住整个尾段时长，重则和"设备动作权锁"
   * 互锁，表现就是跑了几轮之后整机静默（连 [ui] 慢统计 都不打了）。
   *
   * 判据用 pthread_equal 而不是加代数计数：pthread_create 覆盖 ctx->play_thread
   * 就是天然的换代信号，T2 自己退出时 equal 成立、照常置位，语义不变。 */

  if (pthread_equal(pthread_self(), ctx->play_thread))
    {
      ctx->play_exited = true;
    }

  ctx->play_stage = PLAY_STAGE_DONE;   /* 诊断标记：走完了 */

  AUDIO_DEBUG("播放线程退出");
  return NULL;
}

/**
 * @brief  启动播放线程（数据源由调用方备好：拷贝路径在 play_buf，零拷贝路径
 *         在调用方自己的缓冲；本函数只把它挂到 ctx->play_src 上）
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

static int audio_play_begin(audio_context_t *ctx, const int16_t *src,
                            size_t frames,
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
  ctx->play_src = src;                  /* 播放线程只从这里读（拷贝/零拷贝两条路
                                         * 都在进本函数前把数据准备好了） */
  ctx->play_frames = frames;
  ctx->play_gen++;                      /* ★ 换代：上一代从此作废（见 play_gen） */
  ctx->play_stage = PLAY_STAGE_NONE;    /* 新一代从头走，见 play_stage */
  ctx->play_stop = false;
  ctx->playing = true;
  ctx->play_progress_ms = audio_now_ms_nonzero();  /* 起播先盖一章（停滞看门狗） */
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
    }

  /* 音量这一项无条件对齐到模块级的 g_play_volume（用户设定值），不吃调用方
   * 传进来的 config.volume。
   *
   * 两个理由：
   *   - 起播下发的就是 g_play_volume（见 audio_open_play_device），ctx 里这份
   *     只是给 audio_get_volume 看的读数，两边不一致等于读数说谎；
   *   - robot_ui 传的 audio_config 是按字段初始化的，.volume 根本没写 = 0，
   *     不对齐就是「起播前把音量下发成 0」（最小档），全靠后面
   *     touch_ui_set_volume_cb 注册时补一下才没露馅。
   * 真要给某一套单独的音量，得先想清楚它和共享值谁说了算 —— 现在没有这种需求。 */

  ctx->config.volume = g_play_volume;

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

  /* 初始化VAD参数
   *
   * 底噪估计的**种子**是 AUDIO_VAD_ENERGY_THRESHOLD（300）—— 不是 0。
   * 起手门槛因此是 max(300, 300×4) = 1200，实测房间底噪能量 729 < 1200，
   * 开机第一帧就被判成「无声」，不会误触发，同时底噪往真实值收敛。
   * 写 0 的话起手门槛就是 300，正好是 2026-10-25 之前那个死代码的形状。 */

  ctx->vad_energy_threshold = AUDIO_VAD_ENERGY_THRESHOLD;
  ctx->vad_noise_floor_e    = AUDIO_VAD_ENERGY_THRESHOLD;

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
  record_thread_arg_t *ta;
  int ret;

  if (ctx == NULL || !ctx->initialized)
    {
      return -EINVAL;
    }

  /* 有人明确要开麦（开机常开监听 / 监听守护 / 手动接管）：让路锁到此为止。
   * 这是"锁不会永久挂住"的第二条兜底 —— 万一上层漏了解锁，谁要开麦谁就把它
   * 冲掉，代价只是多还一次麦，比常开监听永久哑掉强得多。 */

  ctx->record_hold = false;

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

  /* ★ 保温的播放设备必须在这里丢掉（2026-10-30，配 AUDIO_PLAY_KEEP_WARM）。
   *
   * 它此刻还举着 RUNNING（DAC 模拟通路 + 功放都开着），而本板录放共用一台
   * 设备、半双工：不 STOP 就开麦，录音的 START 会被驱动的方���冲突判据拒掉
   * （-EBUSY，见 audio_open_play_device 里那条注释），或者更糟 —— 抢下来了
   * 但方向标签还是播放（sf32lb52_audio.c 的 pending_playback 毒链）。
   *
   * 为什么 ctx->playing 的判据不够：让路锁被冲掉的那几条路
   * （round_hold_give 的「空档超 8 秒提前还麦」、守护/看门狗强制交还、
   * 本函数自己那句 record_hold = false）都不经过 audio_play_stop，
   * playing 早就是 false，可设备还保温着。
   *
   * audio_drop_play_device 幂等，没保温时就是个空操作。 */
  if (ctx->play_fd_running)
    {
      printf("[AUDIO] 开麦前丢掉保温的播放设备（半双工，不丢录音起不来）\n");
      audio_drop_play_device(ctx);
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

      /* **不清 vad_noise_floor_e**：本函数在每一轮播报结束后都会被调一次
       * （半双工还麦 → 重新开录），清了就等于每次答完话都要重新热身 2 秒，
       * 老人刚问完下一句正赶在这 2 秒里说，门槛还没稳就可能被误判。
       * 已经学到的底噪直接接着用。只在它还是 0（ctx 被 memset 过）时补种子。 */

      if (ctx->vad_noise_floor_e == 0)
        {
          ctx->vad_noise_floor_e = AUDIO_VAD_ENERGY_THRESHOLD;
        }
    }

  /* 启动录音线程：设备已经 START 好，线程里只管 audio_in_read() */

  ctx->record_stop = false;
  ctx->recording = true;
  ctx->record_exited = false;         /* 新一代录音线程的收尾标记 */
  ctx->record_died = false;           /* 上一代的"异常中断"标记到此作废 */
  ctx->record_stolen = false;         /* 同上：上一代的"被抢走"死因到此作废 */

  /* ★ 代际号 + 认领握手（2026-10-25）。顺序很关键：
   *   ① 代际号必须在 pthread_create **之前**自增 —— 线程起跑后读到的必须是
   *      "我已经是最新的那一代"，否则它一上来就判负；
   *   ② record_claimed 先清成 false，等线程自己起跑认领 —— 这样"创建成功了
   *      但线程从没真跑起来"不会被记成"会话活着"，守护下一拍就能看见并重开。
   * 详见 ai_audio.h 那两个字段头上的现场读数。 */

  ctx->record_gen++;
  ctx->record_claimed = false;

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

  /* 线程参数走临时堆：代际号必须是这一个线程私有的（见 record_thread_arg_t）。
   * 这里 8~12 字节、起跑即还，不值得进 SRAM 静态区。 */

  ta = (record_thread_arg_t *)malloc(sizeof(*ta));
  if (ta == NULL)
    {
      ctx->recording = false;
      ctx->record_last_data_ms = 0;
      audio_in_stop();
      return -ENOMEM;
    }

  ta->ctx = ctx;
  ta->gen = ctx->record_gen;

  ret = pthread_create(&ctx->record_thread, NULL,
                       audio_record_thread, ta);
  if (ret != 0)
    {
      AUDIO_DEBUG("创建录音线程失败: %d", ret);
      free(ta);
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

/* 播放停滞多久算"这条线程卡死了"。判据是**推进**（每写完一块盖一次章），
 * 不是总时长 —— 合法的长回复能连着念 60 秒以上，按总时长一刀切会把好好的
 * 长播报掐断。
 *
 * 余量怎么算的：块是 AUDIO_PLAY_CHUNK_MS = 250ms，驱动单次 write 的上界是
 * "一块 + 500ms"（board/contest_board/src/sf32lb52_audio.c 的
 * sf32lb52_audio_tx_wait_slice，由 wr_wait_wdog 私有心跳收口），也就是单块
 * 最多 750ms。5 秒 = 6~7 块的余量，正常路径绝对够，卡死路径又不至于让人
 * 等太久。
 *
 * 2026-10-25 真机基线：那一次卡死整整 80 秒才被监听守护兜住，期间「提交」
 * 全被判"正忙"、新一轮起不来 —— 这个值就是要把那 80 秒压成 5 秒。 */

#define AUDIO_PLAY_STALL_MAX_MS  5000

/* play_stage 的人话（只给日志用）。见 ai_audio.h 里 play_stage 的说明。 */
static const char *audio_play_stage_name(uint8_t stage)
{
  switch (stage)
    {
      case PLAY_STAGE_OPEN:  return "open/CONFIGURE/START";
      case PLAY_STAGE_WRITE: return "write";
      case PLAY_STAGE_CLOSE: return "STOP/close";
      case PLAY_STAGE_DONE:  return "已收尾";
      default:               return "还没开工";
    }
}

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
      static uint32_t s_reap_giveups;   /* 本开机累计放弃回收的次数 */

      s_reap_giveups++;

      /* ★ 2026-10-25 真机教训：这里原来打完日志就 return，play_thread_valid
       * 留 true。那是**单向闩锁** —— 只要有一条播放线程卡在设备调用里
       * （open / write / ioctl(STOP) / close，这几处都没有等待上界），
       * 此后每一次 audio_play_begin() 都走「上一次还没收尾」分支返回 -EBUSY，
       * 整机**永久哑巴**：能听、能识别、文字上屏，就是不出声，只有断电能好。
       * 14:46 现场连续两轮 100% 复现（[TTS] 首句播放失败: -16）。
       *
       * 代码原意是"宁可永久哑巴，也不要两条线程抢一个设备 / 写同一块缓冲"。
       * 但永久哑巴正中"别又不能对话"这条第一优先级，必须反过来取舍：
       *
       *   ① 换代 ctx->play_gen++（在 audio_play_begin 里做）让被甩下的那条线程
       *      自己作废 —— 它哪天从设备调用里醒过来，会在下一个块间隙看到代数
       *      不等、立刻停手退出，也不会再调 play_cb 推状态机。抢设备的风险
       *      因此被压到"它醒来的那一瞬间"，而不是永远。
       *   ② 照旧置 play_stop，双保险。
       *   ③ **解闩**：play_thread_valid 放回 false，让下一次起播有机会重新
       *      open 设备。它若还占着 fd，audio_open_play_device() 会失败并走
       *      正常收尾（play_exited 照常置位、TCB 照常回收），下一轮就恢复了
       *      —— 自己会好，不用断电。这一步把"永久哑巴"降级成"可能掉一轮"。
       *
       * TCB 仍然不 join（它还活着，join 会把调用方钉死），交给内核回收。 */

      syslog(LOG_ERR,
             "[AUDIO] %s: 播放线程 %dms 内没退出，放弃 join 并**解闩放行**"
             "（累计 %u 次；卡在【%s】（阶段 %u，代 %lu）。"
             "下一次起播若还被它占着设备会报 START 被拒，那一轮掉、下一轮恢复）\n",
             who, AUDIO_PLAY_REAP_MS, (unsigned)s_reap_giveups,
             audio_play_stage_name(ctx->play_stage),
             (unsigned)ctx->play_stage, (unsigned long)ctx->play_gen);

      ctx->play_stop = true;
      ctx->play_thread_valid = false;
      return;
    }

  pthread_join(ctx->play_thread, NULL);
  ctx->play_thread_valid = false;
}

/**
 * @brief  播放停滞看门狗（上层每 100ms 调一次，**与轮次无关**）
 *
 * 2026-10-25 真机定案的那 80 秒：一次播报卡在 write 里，playing 一直为真，
 * 「提交」全被判"正忙"、新一轮起不来。自愈的「解闩放行」是对的，但它只在
 * **别人调 audio_play_stop()** 时才触发 —— 而那 80 秒里没人调：轮次看门狗
 * 在 ROUND_PHASE_IDLE 就早退，追问/报警提示音压根不在"轮"里。于是只能等
 * 监听守护 80 秒后兜底，用户看到的就是"AI 不回答"。
 *
 * 所以这条判据必须放在音频层，只看"这块有没有往前走"，不看是谁在放、
 * 放的是哪一轮。
 */

void audio_play_watchdog_tick(audio_context_t *ctx)
{
  int idle;

  if (ctx == NULL || !ctx->playing)
    {
      return;
    }

  idle = audio_since_ms(ctx->play_progress_ms);
  if (idle < 0 || idle < (int)AUDIO_PLAY_STALL_MAX_MS)
    {
      return;
    }

  /* 判定为卡死。把现场一次说清：卡在哪一步、哪一代、多久没动 ——
   * 这几项正是上次只拿到"卡在【write】"之后还要再挖一遍的东西。 */

  printf("[音频] ★★ 播放卡死：%d ms 没有任何一块往前走（阶段=%s/%u，代=%lu）"
         "—— 强制收摊放行，本段作废、下一轮自愈\n",
         idle, audio_play_stage_name(ctx->play_stage),
         (unsigned)ctx->play_stage, (unsigned long)ctx->play_gen);

  /* audio_play_stop() 内部就是那条已经验过的收摊路：置 play_stop →
   * audio_reap_play_thread（等不到就换代 + 解闩放行）→ playing=false。
   * 它会连带把让路锁放掉、把欠着的还麦补上，所以只调它就够。 */

  audio_play_stop(ctx);
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

  /* ★ record_claimed 是 2026-10-25 加的那一道：上面三个 + 下面两个都是
   * "**软件**认为在录"的证据，没有一个能证明"这一代的线程真的跑起来过"。
   * 现场那种形状（线程起跑就被判负、走了"不动任何标志"的早退）正是靠这五个
   * 全为健康骗过了守护 —— 于是每 5 秒空转一圈重开、永不升级，一次抖动变成
   * 永久性"不能对话"。认领是线程自己在进读循环之前盖的章，盖不了就说明这一代
   * 压根没干活，必须如实报假，让守护走"要常听却没在录"那条重开路自愈。 */

  if (!ctx->record_claimed)
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

  return audio_play_begin(ctx, ctx->play_buf, frames, callback, user_data);
}

/**
 * @brief  开始播放音频数据（零拷贝：直读调用方缓冲）
 *
 * 和 audio_play_start 的唯一区别是数据怎么进播放线程：
 *   - 这里不 memcpy、不淡入淡出（淡入淡出会改数据，而调用方的缓冲不许碰）、
 *     不受 AUDIO_PLAY_BUFFER_MS 容量限制 —— 数据压根不进 play_buf；
 *   - 换来的义务写在头文件的原型注释里：调用方必须保证 data 从本函数返回 0
 *     起一直有效，直到完成回调被调用（或 audio_play_stop() 返回）。
 *
 * 为什么加它（2026-10-19 真机取证）：TTS 全段最长 2 MB ≈ 64 秒，原来必须
 * memcpy 进 play_buf 才能播，play_buf 因此被迫常驻 2.24 MB × 两份
 * （hello_app / robot_ui 各一个 ctx），开机水位被挤到 606 KB、主循环任务
 * 都建不起来，hello_app 进崩溃重启死亡螺旋。零拷贝之后 play_buf 缩回
 * 512 KB（只服务短提示音），TTS 容量与播放缓冲彻底解耦。
 *
 * 生命周期契约为什么在这条链上天然成立：g_tts_buf / s_tts_buf 都是常驻
 * 缓冲、从不 free；下一次 voice_tts_speak 合成必然排在这一轮播放完成之后
 * （状态机一轮对话串行），所以"覆写"也不会发生在播放期间。
 */

int audio_play_start_ref(audio_context_t *ctx,
                         const int16_t *data, size_t frames,
                         audio_play_complete_cb_t callback,
                         void *user_data)
{
  if (ctx == NULL || !ctx->initialized || data == NULL || frames == 0)
    {
      return -EINVAL;
    }

  if (ctx->playing)
    {
      AUDIO_DEBUG("已在播放中");
      return -EBUSY;
    }

  /* 半双工切割和拷贝路径完全一致（audio_prepare_output：停录音，播完自动恢复） */

  audio_prepare_output(ctx);

  AUDIO_DEBUG("开始播放（零拷贝）(%zu 帧)", frames);

  /* 不 memcpy、不淡入淡出、不查 play_buf 容量：播放线程直读 data */

  return audio_play_begin(ctx, data, frames, callback, user_data);
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

  return audio_play_begin(ctx, ctx->play_buf, frames, callback, user_data);
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

  /* 上层明确掐掉这一轮播报：让路锁一并解除，别把麦一直扣在手里
   * （"锁不会永久挂住"的第三条兜底，见 ai_audio.h 的 record_hold）。 */

  ctx->record_hold = false;

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

  /* 缓存的播放设备在这里真关（2026-10-26，配 AUDIO_PLAY_DEVICE_CACHE）。
   *
   * 上层明确掐掉了这一轮，下一轮要走全新的一圈，留着那个 fd 没意义。
   *
   * ⚠ 判据**只能**是 play_exited，不能拿 play_thread_valid 一起 or：
   * audio_reap_play_thread() 放弃 join 时会"解闩放行"、把 play_thread_valid
   * 置回 false，可那条线程**还活着**、可能正在 write 这个 fd。跨线程 close
   * 它就是踩 fd（本项目被这条咬过三次）。那种情况留给下一次 stop / deinit 收。
   *
   * play_exited 由播放线程退出前自己置位（audio_play_begin 起新代时清零），
   * 是"这条线程真的跑完了"的唯一可信信号。 */

  if (ctx->play_exited)
    {
      audio_drop_play_device(ctx);
    }
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
   * 设完立刻 close —— 不去碰播放线程/录音封装的 fd。
   *
   * 真正的"用户音量"记在模块级 g_play_volume 上：本函数由设置页调进来时
   * 传的是 robot_ui 那套 ctx，而下一次起播的可能是 hello_app 那套 ——
   * 只记进调用方那个 ctx 的话，另一套起播就把音量写回它自己的旧值了。 */

  g_play_volume = volume;
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

  /* 返回模块级那份：它才是"现在放出来的音量是多少"的答案。
   * ctx->config.volume 只是某一套自己的缓存，两套之间会漂。 */

  return g_play_volume;
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
