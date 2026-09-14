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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <pthread.h>
#include <math.h>
#include <syslog.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 播放设备路径。
 * 本板录音/播放是**同一个节点**（驱动 audio_register("audio0")，路径带
 * audio/ 子目录），而且是**半双工**：AUDIOIOC_STOP 会同时停掉两条通路。
 * 录音不直接开设备，走板级封装 sf32lb52_audio_in（它自带路径）。 */

#define AUDIO_PLAY_DEVICE      "/dev/audio/audio0"

/* 播放分块：100 ms 一块。驱动里的 write() 是**同步**的（阻塞到这块放完），
 * 分块后 audio_play_stop() 最多等一块 + 驱动的等待余量就能生效。
 * 播放缓冲本身仍由 ctx->play_buf 承担（AUDIO_PLAY_BUFFER_MS）。 */

#define AUDIO_PLAY_CHUNK_MS    100

/* 驱动侧音量值域 0..1000（nuttx/audio/audio.h 的 AUDIO_VOLUME_MAX，
 * 也就是 AUDIOIOC_CONFIGURE + AUDIO_TYPE_FEATURE + AUDIO_FU_VOLUME 的取值），
 * 本模块对外是 0..100（ai_audio.h 的 AUDIO_VOLUME_MAX），换算时乘 10。
 * 这里写常量而不是引平台宏，免得上面那两个同名宏再串味。 */

#define AUDIO_HW_VOLUME_MAX    1000

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void *audio_record_thread(void *arg);
static void *audio_play_thread(void *arg);
static uint32_t audio_calc_frame_energy(const int16_t *data, size_t frames);
static int audio_open_play_device(audio_context_t *ctx);
static void audio_close_play_device(int fd);
static int audio_apply_volume(int fd, uint8_t volume);
static int audio_configure_output(int fd, const audio_context_t *ctx);
static int audio_hw_set_volume(const audio_context_t *ctx, uint8_t volume);
static void audio_prepare_output(audio_context_t *ctx);
static void audio_resume_record(audio_context_t *ctx);
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
 */

static int audio_open_play_device(audio_context_t *ctx)
{
  int fd;

  fd = open(AUDIO_PLAY_DEVICE, O_WRONLY);
  if (fd < 0)
    {
      int errcode = errno;

      AUDIO_DEBUG("打开播放设备 %s 失败: %d", AUDIO_PLAY_DEVICE, errcode);
      return -errcode;
    }

  if (audio_configure_output(fd, ctx) != OK)
    {
      close(fd);
      return -EIO;
    }

  audio_apply_volume(fd, ctx->config.volume);

  if (ioctl(fd, AUDIOIOC_START, 0) < 0)
    {
      int errcode = errno;

      AUDIO_DEBUG("播放 AUDIOIOC_START 失败: %d", errcode);
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
       * 它阻塞到读满 want 字节，被 AUDIOIOC_STOP 打断或下层 5 秒超时返回 0。 */

      nbytes = audio_in_read(ctx->record_buf, want);

      /* 0 = 被 STOP 打断（audio_record_stop() 的正常路径）或超时；
       * 负值 = 没 start / fd 已被收掉。**两者都必须跳出**，
       * 不能当成"这次没数据、再读一次就有"（那样是死循环）。 */

      if (nbytes > 0)
        {
          size_t frames_read = (size_t)nbytes / frame_bytes;
          size_t samples_read = frames_read * ctx->config.channels;

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
          AUDIO_DEBUG("录音结束/读取失败: %zd", nbytes);
          break;
        }
    }

  /* 线程自己收尾时也要把设备还回去。audio_in_stop() 是幂等的：
   * 正常 audio_record_stop() 路径下这里已经是空操作。
   * 不能留一个 START 过的设备没人关，否则下次 audio_in_start() 给的是 -EBUSY。 */

  audio_in_stop();

  AUDIO_DEBUG("录音线程退出");
  ctx->recording = false;
  ctx->state = ctx->playing ? AUDIO_STATE_PLAYING : AUDIO_STATE_IDLE;

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
 */

static void *audio_play_thread(void *arg)
{
  audio_context_t *ctx = (audio_context_t *)arg;
  size_t frame_bytes = ctx->config.channels * sizeof(int16_t);
  size_t total_bytes = ctx->play_frames * frame_bytes;
  size_t chunk_bytes =
    (size_t)ctx->config.sample_rate * AUDIO_PLAY_CHUNK_MS / 1000 * frame_bytes;
  size_t done = 0;
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
       * 把上层状态机推下去，别让它等一个永远不会来的完成事件。 */

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
              AUDIO_DEBUG("播放写入失败: %zd", written);
              break;
            }

          done += (size_t)written;
        }
    }

  /* 先停设备再退出：AUDIOIOC_STOP 会同时停掉录音通路（半双工），
   * 所以它必须是播放的最后一步。fd 也是在本线程里关的。 */

  audio_close_play_device(fd);

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

  /* 播放完成回调（在播放线程里回调，调用者可以放心紧跟着开录音） */

  if (!ctx->play_stop && ctx->play_cb)
    {
      ctx->play_cb(ctx->play_user_data);
    }

  AUDIO_DEBUG("播放线程退出");
  return NULL;
}

/**
 * @brief  启动播放线程（数据已经在 ctx->play_buf 里了）
 * @return 0成功, 负值失败
 *
 * 设备由播放线程自己 open/START/write/STOP/close，这里只负责：
 * 回收上一次的播放线程（它自己会关 fd）、记下参数、起线程。
 */

static int audio_play_begin(audio_context_t *ctx, size_t frames,
                            audio_play_complete_cb_t callback,
                            void *user_data)
{
  int ret;

  /* 上一次的播放线程需要回收。正常情况下 audio_play_stop() 已经 join 过了，
   * 这里是兜底（比如播放线程自己跑完、没被 stop 过）。
   * 如果本函数就是在播放线程里被调的（播放完成回调里又开一段），不能 join 自己：
   * 线程已经写完了缓冲、回调返回后就退出，直接起新的即可。 */

  if (ctx->play_thread_valid)
    {
      if (pthread_equal(pthread_self(), ctx->play_thread))
        {
          AUDIO_DEBUG("在播放线程里重开播放，跳过 join");
        }
      else
        {
          pthread_join(ctx->play_thread, NULL);
        }

      ctx->play_thread_valid = false;
    }

  ctx->play_cb = callback;
  ctx->play_user_data = user_data;
  ctx->play_frames = frames;
  ctx->play_stop = false;
  ctx->playing = true;

  ret = pthread_create(&ctx->play_thread, NULL, audio_play_thread, ctx);
  if (ret != 0)
    {
      AUDIO_DEBUG("创建播放线程失败: %d", ret);
      ctx->playing = false;

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

  /* 停止录音和播放。两个 stop 都会等到各自的线程退出（设备 fd 由录音封装
   * 和播放线程自己关），所以下面 free 缓冲是安全的。 */

  audio_record_stop(ctx);
  audio_play_stop(ctx);

  /* 释放缓冲区 */

  if (ctx->record_buf != NULL)
    {
      free(ctx->record_buf);
      ctx->record_buf = NULL;
    }

  if (ctx->play_buf != NULL)
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

  ret = pthread_create(&ctx->record_thread, NULL,
                       audio_record_thread, ctx);
  if (ret != 0)
    {
      AUDIO_DEBUG("创建录音线程失败: %d", ret);
      ctx->recording = false;
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
 * 做法：先自己轮询 ctx->record_exited（usleep 自旋，**不依赖任何内核超时**：
 * 那次连 nxsem_tickwait 的 5 秒超时都没回来，见 sf32lb52_audio.c 的 read），
 * 最多 300ms。线程确实退出后再 join 回收 TCB（此时立刻返回）。真超时就打印
 * 错误、放弃 join，把 record_thread_valid 留着让下一次收尾 —— 无论如何，
 * 界面不许被它拖死。
 */

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

  for (i = 0; i < 30 && !ctx->record_exited; i++)
    {
      usleep(10000);                    /* 10ms × 30 = 300ms 上限 */
    }

  if (!ctx->record_exited)
    {
      syslog(LOG_ERR,
             "[AUDIO] %s: 录音线程 300ms 内没退出，放弃 join（界面优先，"
             "线程会在 read 返回后自己收尾）\n", who);
      return;                           /* record_thread_valid 保持 true */
    }

  pthread_join(ctx->record_thread, NULL);
  ctx->record_thread_valid = false;
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

  if (ctx->record_thread_valid)
    {
      audio_in_stop();
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

  /* 复制音频数据到缓冲区（必须在播放线程起来之前复制完） */

  AUDIO_DEBUG("开始播放 (%zu 帧)", frames);

  memcpy(ctx->play_buf, data, frames * frame_bytes);

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
   * 最坏等一块 + 驱动内部的等待余量（约 600ms），不会长时间卡死。 */

  ctx->play_stop = true;

  if (ctx->play_thread_valid)
    {
      /* 和录音一样：如果是从播放线程自己里调的（播放完成回调里又调 stop），
       * 不能 join 自己；线程返回前会自己判断 play_stop 决定是否再回调。 */

      if (pthread_equal(pthread_self(), ctx->play_thread))
        {
          AUDIO_DEBUG("在播放线程里调 stop，跳过 join");
        }
      else
        {
          pthread_join(ctx->play_thread, NULL);
          ctx->play_thread_valid = false;
        }
    }

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
