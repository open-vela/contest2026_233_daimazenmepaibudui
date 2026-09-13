/****************************************************************************
 * board/contest_board/src/sf32lb52_audio_in.c
 *
 * SF32LB52 录音通路封装（麦克风输入）
 *
 * 只做三件事：start（open + CONFIGURE + START）、read、stop（STOP + close）。
 * 内部用一个全局 fd 表示"当前这次录音会话"，用一把 nxmutex 保护 fd 与状态。
 *
 * 线程约定（重要）：
 *   - audio_in_read() 是阻塞的，**读的时候不持锁**，只短暂取锁拿 fd 快照。
 *     否则"一个任务在读、另一个任务调 audio_in_stop() 发 STOP 救场"的
 *     经典用法会直接死锁（stop 拿不到锁 -> 发不出 STOP -> read 永远不返回）。
 *   - 只有 fd 的创建/销毁（start/stop）和状态读写在锁里。
 *
 * 录音参数固定 16 kHz / 单声道 / 16bit：这是驱动与本板麦克风一路验证过的组合。
 * 详细说明与完整示例见 docs/audio_driver_usage.md 第 9 节。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/audio/audio.h>
#include <nuttx/mutex.h>

#include "sf32lb52_audio_in.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 驱动 codec 时钟表里有的档位（sf32lb52_audio.c:167-186 的
 * codec_dac_clk_config / codec_adc_clk_config）。
 * 其余采样率驱动会在 CONFIGURE 时返回 -EINVAL，这里提前拦掉。 */

#define AUDIO_IN_RATE_8K      8000
#define AUDIO_IN_RATE_16K     16000
#define AUDIO_IN_RATE_44K1    44100
#define AUDIO_IN_RATE_48K     48000

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 当前录音会话的 fd，-1 = 未打开 */

static int  g_audio_in_fd = -1;

/* fd 与状态的保护锁。只保护 start/stop 的临界区，不覆盖阻塞的 read。 */

static mutex_t g_audio_in_lock = NXMUTEX_INITIALIZER;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * @brief  采样率是否是驱动支持的档位
 */

static bool audio_in_rate_supported(int sample_rate)
{
  return sample_rate == AUDIO_IN_RATE_8K  ||
         sample_rate == AUDIO_IN_RATE_16K ||
         sample_rate == AUDIO_IN_RATE_44K1 ||
         sample_rate == AUDIO_IN_RATE_48K;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: audio_in_start
 ****************************************************************************/

int audio_in_start(int sample_rate, int channels, int bits)
{
  struct audio_caps_desc_s capdesc;
  int fd;

  /* 参数校验：不做隐式纠正，非法就直接拒绝，免得录出格式不对的数据 */

  if (!audio_in_rate_supported(sample_rate) ||
      channels != 1 || bits != AUDIO_IN_DEFAULT_BITS)
    {
      syslog(LOG_ERR,
             "AUDIO_IN: bad params (rate=%d ch=%d bits=%d), "
             "only 8k/16k/44.1k/48k, mono, 16bit\n",
             sample_rate, channels, bits);
      return -EINVAL;
    }

  if (nxmutex_lock(&g_audio_in_lock) < 0)
    {
      return -EINVAL;                 /* 锁坏掉了，属于不可能的状态 */
    }

  /* 已经在录音中：返回 -EBUSY，不打断当前会话。
   * 想重录请先 audio_in_stop()。 */

  if (g_audio_in_fd >= 0)
    {
      nxmutex_unlock(&g_audio_in_lock);
      return -EBUSY;
    }

  fd = open(AUDIO_IN_DEV, O_RDONLY);
  if (fd < 0)
    {
      int errcode = errno;

      nxmutex_unlock(&g_audio_in_lock);
      syslog(LOG_ERR, "AUDIO_IN: open %s failed: %d\n",
             AUDIO_IN_DEV, errcode);
      return -errcode;
    }

  memset(&capdesc, 0, sizeof(capdesc));
  capdesc.caps.ac_len            = sizeof(struct audio_caps_s);
  capdesc.caps.ac_type           = AUDIO_TYPE_INPUT;
  capdesc.caps.ac_channels       = (uint8_t)channels;
  capdesc.caps.ac_controls.hw[0] = (uint16_t)sample_rate;
  capdesc.caps.ac_controls.b[2]  = (uint8_t)bits;

  if (ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&capdesc) < 0)
    {
      int errcode = errno;

      close(fd);
      nxmutex_unlock(&g_audio_in_lock);
      syslog(LOG_ERR, "AUDIO_IN: AUDIOIOC_CONFIGURE failed: %d\n", errcode);
      return -errcode;
    }

  if (ioctl(fd, AUDIOIOC_START, 0) < 0)
    {
      int errcode = errno;

      close(fd);
      nxmutex_unlock(&g_audio_in_lock);
      syslog(LOG_ERR, "AUDIO_IN: AUDIOIOC_START failed: %d\n", errcode);
      return -errcode;
    }

  /* 最后一步才发布 fd：中途失败时全局状态始终是"未打开"，
   * 别人不会拿到一个半配置好的会话。 */

  g_audio_in_fd = fd;

  nxmutex_unlock(&g_audio_in_lock);

  syslog(LOG_INFO, "AUDIO_IN: started (%d Hz, %d ch, %d bit)\n",
         sample_rate, channels, bits);
  return OK;
}

/****************************************************************************
 * Name: audio_in_read
 ****************************************************************************/

ssize_t audio_in_read(FAR void *buf, size_t len)
{
  int fd;
  ssize_t n;

  if (buf == NULL || len == 0)
    {
      return -EINVAL;
    }

  /* 只拿一个 fd 快照，**不持锁**做阻塞读：否则别的任务没法调
   * audio_in_stop() 来发 STOP 唤醒这里的 read()。 */

  if (nxmutex_lock(&g_audio_in_lock) < 0)
    {
      return -EINVAL;
    }

  fd = g_audio_in_fd;

  nxmutex_unlock(&g_audio_in_lock);

  if (fd < 0)
    {
      return -EINVAL;                 /* 没 start，或者已经被 stop 了 */
    }

  n = read(fd, buf, len);

  /* 注意 read() 的语义：0 = 被 STOP 打断或下层 5 秒超时，
   * 不是错误码也不是"读到 0 字节数据"。正数才是读到的字节数。
   * 负值说明 fd 已失效（比如刚被另一个任务 close 掉）。 */

  return n;
}

/****************************************************************************
 * Name: audio_in_stop
 ****************************************************************************/

int audio_in_stop(void)
{
  int fd;

  if (nxmutex_lock(&g_audio_in_lock) < 0)
    {
      return -EINVAL;
    }

  fd = g_audio_in_fd;
  g_audio_in_fd = -1;                 /* 先摘掉 fd：后来者立刻看到"已停" */

  nxmutex_unlock(&g_audio_in_lock);

  if (fd < 0)
    {
      return OK;                      /* 没在录音，安全空操作 */
    }

  /* 顺序很关键：先 STOP（唤醒可能正阻塞在 read() 里的任务），再 close。
   * close() 在最外层 fd 上不会走到驱动的 shutdown()；只有本模块是设备上
   * 唯一使用者时才会，而那条路径已修成"关中断上下文里安全"的最小化版本
   * （见 docs/audio_driver_usage.md 第 8 节）。 */

  if (ioctl(fd, AUDIOIOC_STOP, 0) < 0)
    {
      syslog(LOG_ERR, "AUDIO_IN: AUDIOIOC_STOP failed: %d\n", errno);
    }

  close(fd);

  syslog(LOG_INFO, "AUDIO_IN: stopped\n");
  return OK;
}
