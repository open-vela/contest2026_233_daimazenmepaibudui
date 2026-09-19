/****************************************************************************
 * board/contest_board/src/sf32lb52_alarm.c
 *
 * SF32LB52 设备级报警模块（只出声，持续到解除）
 *
 * 组成：
 *   - 一个工作线程（栈 4096，优先级 115）：负责播报警音、自动超时解除；
 *     所有回调都在这个线程里发。
 *   - alarm_trigger()/alarm_clear() 只改状态 + 唤醒工作线程，非阻塞。
 *
 * 声音走标准 NuttX 音频设备 /dev/audio/audio0（16k 单声道 16bit），
 * 每 50ms 写一块（静态缓冲，绝不放在栈上），块与块之间检查状态，
 * 这样解除报警最迟 ~50ms 生效。
 *
 * 本模块**不驱动任何指示灯**：板上唯一能由软件控制的用户 GPIO 输出脚是
 * PA26（/dev/gpio1），留给板级/应用做状态指示比绑在报警上更有价值；
 * 原理图上标着 RGB LED 的 PA32 在这块板子实物上并不存在，驱动它没有
 * 可见效果。所以报警的"设备级动作"只保留声音。
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
#include <math.h>

#include <nuttx/audio/audio.h>
#include <nuttx/clock.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/sched.h>
#include <nuttx/wdog.h>    /* 私有心跳：等一次新触发/解除，见 alarm_wait_wake */

#include "sf32lb52_alarm.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ALARM_AUDIO_DEV       "/dev/audio/audio0"
#define ALARM_SAMPLE_RATE     16000
#define ALARM_CHANNELS        1
#define ALARM_BITS            16

#define ALARM_SAMPLES_PER_MS  (ALARM_SAMPLE_RATE / 1000)          /* 16 */
#define ALARM_CHUNK_MS        50                                  /* 每块 50ms */
#define ALARM_CHUNK_SAMPLES   (ALARM_CHUNK_MS * ALARM_SAMPLES_PER_MS)
#define ALARM_TONE_AMP        0.25                                /* 约 -12dBFS */

/* NOTICE：一声短鸣 */

#define ALARM_NOTICE_MS          200
#define ALARM_NOTICE_FREQ        1000
#define ALARM_NOTICE_INTERVAL_MS 0          /* 0 = 只响一次，不重复 */

/* WARNING：三声短鸣，每 10 秒一轮 */

#define ALARM_WARNING_BEEP_MS     150
#define ALARM_WARNING_GAP_MS      120
#define ALARM_WARNING_CYCLE_MS    (ALARM_WARNING_BEEP_MS + ALARM_WARNING_GAP_MS)
#define ALARM_WARNING_BEEPS       3
#define ALARM_WARNING_MS          (ALARM_WARNING_CYCLE_MS * ALARM_WARNING_BEEPS)
#define ALARM_WARNING_FREQ        1000
#define ALARM_WARNING_INTERVAL_MS 10000

/* EMERGENCY：高低交替，每 5 秒一轮 */

#define ALARM_EMERG_TONE_MS       250
#define ALARM_EMERG_CYCLE_MS      (2 * ALARM_EMERG_TONE_MS)
#define ALARM_EMERG_CYCLES        2
#define ALARM_EMERG_MS            (ALARM_EMERG_CYCLE_MS * ALARM_EMERG_CYCLES)
#define ALARM_EMERG_HIGH_FREQ     1200
#define ALARM_EMERG_LOW_FREQ      700
#define ALARM_EMERG_INTERVAL_MS   5000

/* 工作线程。
 *
 * 优先级 115 是刻意选的：NuttX 里**数值越小优先级越高**，所以它低于
 * robot_ui(110)、高于 lpwork(120)。报警时不能反过来抢在 lpwork 前面 ——
 * lpwork 跑的是 USB RNDIS 收发，而报警恰恰是最需要把 MQTT 上报发出去的
 * 时候。报警线程绝大部分时间阻塞在 DMA/休眠上，低一点不影响出声。 */

#define ALARM_WORKER_NAME     "alarm"
#define ALARM_WORKER_PRIORITY 115
#define ALARM_WORKER_STACK    4096

/* 两轮之间等一等一次的粒度（毫秒）：原来的 nxsem_tickwait 用的就是这个数，
 * 换成私有心跳之后语义一个字都没变（见 alarm_wait_wake）。 */
#define ALARM_WAKE_SLICE_MS   50

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct alarm_priv_s
{
  mutex_t               lock;        /* 保护 status / cb / 序号 */
  sem_t                 wake;        /* 触发/解除时唤醒工作线程 */
  struct wdog_s         wake_wdog;   /* 等一次新触发/解除那片等待的私有心跳 */
  volatile bool         wake_to;     /* 这次醒是心跳到点叫的（不是有人 post）*/
  bool                  initialized;
  pid_t                 worker;
  alarm_cb_t            cb;
  FAR void             *cb_arg;
  struct alarm_status_s status;
  uint32_t              seq;         /* 每次 trigger() 自增 */
  uint32_t              handled_seq; /* 工作线程已回调过的 seq */
  enum alarm_event_e    exit_event;  /* 待回调的解除/超时事件，0 = 无 */
  bool                  audio_warned;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 锁和信号量静态初始化，这样 alarm_get_status() 在模块初始化前也能安全用 */

static struct alarm_priv_s g_alarm =
{
  .lock = NXMUTEX_INITIALIZER,
  .wake = SEM_INITIALIZER(0),
};

/* 报警音缓冲：必须是静态的（工程踩过"大局部数组被编译到函数序言 → hardfault"）。
 * 只按 50ms 一块生成，1600 字节足够。 */

static int16_t g_alarm_tone[ALARM_CHUNK_SAMPLES];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t alarm_now_ms(void)
{
  return (uint32_t)TICK2MSEC(clock_systime_ticks());
}

static void alarm_set_string(FAR char *dst, size_t size, FAR const char *src)
{
  if (src == NULL)
    {
      src = "";
    }

  strncpy(dst, src, size - 1);
  dst[size - 1] = '\0';
}

/****************************************************************************
 * Name: alarm_round_ms / alarm_round_interval_ms
 *
 * Description: 各报警级别的时间参数
 *
 ****************************************************************************/

static uint32_t alarm_round_ms(enum alarm_level_e level)
{
  switch (level)
    {
      case ALARM_LEVEL_NOTICE:
        return ALARM_NOTICE_MS;

      case ALARM_LEVEL_WARNING:
        return ALARM_WARNING_MS;

      case ALARM_LEVEL_EMERGENCY:
        return ALARM_EMERG_MS;

      default:
        return 0;
    }
}

static uint32_t alarm_round_interval_ms(enum alarm_level_e level)
{
  switch (level)
    {
      case ALARM_LEVEL_WARNING:
        return ALARM_WARNING_INTERVAL_MS;

      case ALARM_LEVEL_EMERGENCY:
        return ALARM_EMERG_INTERVAL_MS;

      default:
        return 0;    /* NOTICE：只响一次 */
    }
}

static int alarm_round_samples(enum alarm_level_e level)
{
  return (int)(alarm_round_ms(level) * ALARM_SAMPLES_PER_MS);
}

/****************************************************************************
 * Name: alarm_tone_freq
 *
 * Description: 给定轮内采样偏移，返回该点应有的音调频率；0 = 静音。
 *   偏移用绝对值，保证分块写入时相位连续、块与块之间不会"咔"一声。
 *
 ****************************************************************************/

static int alarm_tone_freq(enum alarm_level_e level, int offset)
{
  int cycle;
  int pos;

  switch (level)
    {
      case ALARM_LEVEL_NOTICE:
        return (offset < ALARM_NOTICE_MS * ALARM_SAMPLES_PER_MS) ?
               ALARM_NOTICE_FREQ : 0;

      case ALARM_LEVEL_WARNING:
        cycle = ALARM_WARNING_CYCLE_MS * ALARM_SAMPLES_PER_MS;
        pos   = offset % cycle;
        return (pos < ALARM_WARNING_BEEP_MS * ALARM_SAMPLES_PER_MS) ?
               ALARM_WARNING_FREQ : 0;

      case ALARM_LEVEL_EMERGENCY:
        cycle = ALARM_EMERG_CYCLE_MS * ALARM_SAMPLES_PER_MS;
        pos   = offset % cycle;
        return (pos < ALARM_EMERG_TONE_MS * ALARM_SAMPLES_PER_MS) ?
               ALARM_EMERG_HIGH_FREQ : ALARM_EMERG_LOW_FREQ;

      default:
        return 0;
    }
}

static void alarm_fill_chunk(enum alarm_level_e level, int offset, int count)
{
  int i;

  for (i = 0; i < count; i++)
    {
      int off  = offset + i;
      int freq = alarm_tone_freq(level, off);

      if (freq == 0)
        {
          g_alarm_tone[i] = 0;
        }
      else
        {
          g_alarm_tone[i] = (int16_t)(32767.0 * ALARM_TONE_AMP *
                            sin(2.0 * M_PI * (double)freq * (double)off /
                                (double)ALARM_SAMPLE_RATE));
        }
    }
}

/****************************************************************************
 * Name: alarm_audio_open
 *
 * Description:
 *   按 audio_test 已验证的顺序打开报警音通路：
 *   open -> CONFIGURE(output 16k mono 16bit) -> CONFIGURE(volume) -> START
 *
 * Returned Value:
 *   成功返回 fd，失败返回 -1。
 *
 ****************************************************************************/

static int alarm_audio_open(void)
{
  struct audio_caps_desc_s capdesc;
  int fd;
  int ret;

  fd = open(ALARM_AUDIO_DEV, O_WRONLY);
  if (fd < 0)
    {
      return -1;
    }

  memset(&capdesc, 0, sizeof(capdesc));
  capdesc.caps.ac_len            = sizeof(struct audio_caps_s);
  capdesc.caps.ac_type           = AUDIO_TYPE_OUTPUT;
  capdesc.caps.ac_channels       = ALARM_CHANNELS;
  capdesc.caps.ac_controls.hw[0] = ALARM_SAMPLE_RATE;
  capdesc.caps.ac_controls.b[2]  = ALARM_BITS;

  ret = ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&capdesc);
  if (ret < 0)
    {
      close(fd);
      return -1;
    }

  /* 标准音量接口：AUDIOIOC_CONFIGURE + AUDIO_TYPE_FEATURE + AUDIO_FU_VOLUME */

  memset(&capdesc, 0, sizeof(capdesc));
  capdesc.caps.ac_len            = sizeof(struct audio_caps_s);
  capdesc.caps.ac_type           = AUDIO_TYPE_FEATURE;
  capdesc.caps.ac_format.hw      = AUDIO_FU_VOLUME;
  capdesc.caps.ac_controls.hw[0] = ALARM_PLAYBACK_VOLUME;
  ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&capdesc);

  if (ioctl(fd, AUDIOIOC_START, 0) < 0)
    {
      close(fd);
      return -1;
    }

  return fd;
}

static void alarm_audio_close(int fd)
{
  if (fd >= 0)
    {
      ioctl(fd, AUDIOIOC_STOP, 0);
      close(fd);
    }
}

/****************************************************************************
 * Name: alarm_emit
 *
 * Description: 在**工作线程**上下文里调用上层注册的回调。
 *
 ****************************************************************************/

static void alarm_emit(enum alarm_event_e event,
                       FAR const struct alarm_status_s *status)
{
  alarm_cb_t cb;
  FAR void  *arg;

  nxmutex_lock(&g_alarm.lock);
  cb  = g_alarm.cb;
  arg = g_alarm.cb_arg;
  nxmutex_unlock(&g_alarm.lock);

  if (cb != NULL)
    {
      cb(event, status, arg);
    }
}

/****************************************************************************
 * Name: alarm_recheck
 *
 * Description:
 *   工作线程的"状态检查点"：派发待处理的事件，并告诉调用者要不要中止
 *   当前这一轮。
 *
 * Returned Value:
 *   0 = 继续；1 = 中止（报警已解除）；2 = 中止并按新的更高级别重来。
 *
 ****************************************************************************/

static int alarm_recheck(enum alarm_level_e cur_level)
{
  struct alarm_status_s snapshot;
  enum alarm_event_e    event = (enum alarm_event_e)0;
  int                   result = 0;

  nxmutex_lock(&g_alarm.lock);

  if (g_alarm.status.active && g_alarm.seq != g_alarm.handled_seq)
    {
      /* 触发（含已经在报警中又被触发）：回调 TRIGGERED */

      g_alarm.handled_seq = g_alarm.seq;
      snapshot = g_alarm.status;
      event    = ALARM_EVENT_TRIGGERED;

      if (g_alarm.status.level > cur_level)
        {
          result = 2;                 /* 级别升高 → 重来 */
        }
    }
  else if (!g_alarm.status.active)
    {
      if (g_alarm.exit_event != (enum alarm_event_e)0)
        {
          event = g_alarm.exit_event;
          g_alarm.exit_event = (enum alarm_event_e)0;
          g_alarm.handled_seq = g_alarm.seq;
          snapshot = g_alarm.status;
        }

      result = 1;
    }

  nxmutex_unlock(&g_alarm.lock);

  if (event != (enum alarm_event_e)0)
    {
      alarm_emit(event, &snapshot);
    }

  return result;
}

/****************************************************************************
 * Name: alarm_request_exit
 *
 * Description: 工作线程自己判定超时/解除，清 active 并留下待回调事件。
 *
 ****************************************************************************/

static void alarm_request_exit(enum alarm_event_e event)
{
  nxmutex_lock(&g_alarm.lock);
  g_alarm.status.active = false;
  g_alarm.exit_event    = event;
  nxmutex_unlock(&g_alarm.lock);

  nxsem_post(&g_alarm.wake);
}

/****************************************************************************
 * Name: alarm_wake_timeout
 *
 * Description:
 *   两轮之间等一等那片等待的心跳到点了。
 *
 *   与音频驱动里那两记心跳（sf32lb52_audio.c 的 rx/tx_wait_wdog）逐条对应：
 *   跑在 systick 中断里，只做两件**与 TCB 无关**的事 —— 立 wake_to 旗（告诉
 *   等待方这一次是到点了）＋ **无条件** post 一次 wake 信号量。
 *   ★ 必须无条件：这记心跳就是到点叫醒回去看一眼状态的那一下，一旦加上
 *   `if (xxx) 才 post` 这类条件，只要判据不为真就叫不醒，等待就没有上界了。
 *   **不打日志**（中断里碰串口会抢控制台锁把整机挂住）。
 *
 *   为什么不再用内核的 nxsem_tickwait_uninterruptible（2026-09-19 定案）：
 *   那条路是内核定时等待的超时与别人的 nxsem_post 抢同一份 TCB 字段
 *   （rtcb->waitdog / rtcb->waitobj），本板临界区是 BASEPRI 型（dump 里
 *   BASEPRI=0x80 = 只挡优先级大于等于 8 的异常）挡不住它，真机上抓到的断言正是：
 *       ASSERT sem_waitirq.c:137  task robot_ui
 *       nxsem_wait_irq <- nxsem_timeout <- wd_timer <- timer_callback
 *                      <- systick_interrupt
 *   （dump 全文在 _flash/gate_status.txt）同一块板同一天在音频 read/write 那两条
 *   等待上各中过一次，那两处已换成同一套私有心跳。报警的出声线程每 50ms 走一次
 *   这条等待，而 alarm_trigger()/alarm_clear() **会从别的线程 post 同一个信号量**
 *   —— 形状完全一样，所以这里也换掉：心跳从不读也不写 TCB，那条断言路径在这条
 *   等待上不会被走到；而心跳是从本模块 priv 里起的一记普通看门狗，别人 post 取消
 *   不了它，所以最多等 50ms 是真的上界。
 ****************************************************************************/

static void alarm_wake_timeout(wdparm_t arg)
{
  FAR struct alarm_priv_s *priv = (FAR struct alarm_priv_s *)(uintptr_t)arg;

  priv->wake_to = true;

  nxsem_post(&priv->wake);
}

/****************************************************************************
 * Name: alarm_wait_wake
 *
 * Description:
 *   等一次新触发/解除，最多 wait_ms 毫秒。
 *
 *   返回值语义**与原来那句 nxsem_tickwait_uninterruptible 完全一致**：
 *     OK         = 确实有人登记了新触发/解除（调用方回主循环看一眼）；
 *     -ETIMEDOUT = 到点了、没人登记（本模块自己的心跳叫醒的），调用方接着等下一片。
 *
 *   nxsem_reset(&wake, 0) 收掉上一次心跳多出来的那一次 post：不清的话下一次等待
 *   会立刻返回、gap 里就变成空转（报警线程优先级 115，空转会饿着 lpwork）。
 *   它最多让一次新触发晚 50ms 被看见 —— 与原来每片醒来先 alarm_recheck() 的
 *   粒度一致（alarm_clear() 之后最迟 ~50ms 生效这条承诺本来就是这样）。
 ****************************************************************************/

static int alarm_wait_wake(uint32_t wait_ms)
{
  int ret;

  g_alarm.wake_to = false;

  (void)nxsem_reset(&g_alarm.wake, 0);

  (void)wd_start(&g_alarm.wake_wdog, MSEC2TICK(wait_ms),
                 alarm_wake_timeout, (wdparm_t)&g_alarm);

  ret = nxsem_wait_uninterruptible(&g_alarm.wake);

  /* wd_cancel 对已经到点的心跳只返回 -EINVAL：那正是 wake_to 为真的那一刻，
   * 下面按到点报（调用方照常回主循环重判一次状态）。 */
  if (wd_cancel(&g_alarm.wake_wdog) != OK)
    {
      g_alarm.wake_to = true;
    }

  return (ret == OK && !g_alarm.wake_to) ? OK : -ETIMEDOUT;
}

/****************************************************************************
 * Name: alarm_wait_gap
 *
 * Description:
 *   两轮播报之间的等待。每 50ms 醒一次，并在每次醒来时调用
 *   alarm_recheck() 派发事件；被 trigger/clear 用信号量唤醒时立刻返回。
 *   所以 alarm_clear() 之后，工作线程最迟 ~50ms 就会看到解除并停下
 *   （和写音频块时的检查粒度一致）。
 *
 ****************************************************************************/

static void alarm_wait_gap(enum alarm_level_e level, uint32_t gap_ms)
{
  uint32_t start = alarm_now_ms();

  while ((alarm_now_ms() - start) < gap_ms)
    {
      if (alarm_recheck(level) != 0)
        {
          return;
        }

      if (alarm_wait_wake(ALARM_WAKE_SLICE_MS) == OK)
        {
          return;   /* 有新触发/解除，回主循环立刻处理 */
        }
    }
}

/****************************************************************************
 * Name: alarm_worker
 *
 * Description: 报警工作线程主循环（唯一做声音和回调的地方）。
 *
 ****************************************************************************/

static int alarm_worker(int argc, FAR char *argv[])
{
  (void)argc;
  (void)argv;

  for (;;)
    {
      enum alarm_level_e level;
      uint32_t triggered;
      uint32_t elapsed;
      uint32_t round_ms;
      uint32_t interval;
      uint32_t remain;
      uint32_t gap;
      bool     active;
      int      fd;
      int      pos;
      int      total;
      int      decision = 0;

      /* 派发上一次遗留的事件（触发/解除/超时） */

      alarm_recheck(ALARM_LEVEL_NONE);

      nxmutex_lock(&g_alarm.lock);
      active    = g_alarm.status.active;
      level     = g_alarm.status.level;
      triggered = g_alarm.status.triggered_ms;
      nxmutex_unlock(&g_alarm.lock);

      if (!active)
        {
          nxsem_wait_uninterruptible(&g_alarm.wake);
          continue;
        }

      elapsed = alarm_now_ms() - triggered;
      if (elapsed >= ALARM_AUTO_TIMEOUT_MS)
        {
          alarm_request_exit(ALARM_EVENT_TIMEOUT);
          continue;
        }

      /* ---- 播报一轮：50ms 一块，块间检查状态 ---- */

      total = alarm_round_samples(level);
      pos   = 0;
      fd    = alarm_audio_open();

      if (fd < 0 && !g_alarm.audio_warned)
        {
          g_alarm.audio_warned = true;
          syslog(LOG_ERR,
                 "ALARM: audio open failed, this alarm has no output\n");
        }

      while (pos < total)
        {
          int chunk = total - pos;

          if (chunk > ALARM_CHUNK_SAMPLES)
            {
              chunk = ALARM_CHUNK_SAMPLES;
            }

          decision = alarm_recheck(level);
          if (decision != 0)
            {
              break;
            }

          if (fd >= 0)
            {
              alarm_fill_chunk(level, pos, chunk);

              if (write(fd, g_alarm_tone,
                        (size_t)chunk * sizeof(int16_t)) <= 0)
                {
                  /* 设备被占用（比如 TTS 正拿着）或出错：关掉它，
                   * 本轮剩下的时间静默走完（仍然每 50ms 检查一次状态），
                   * 下一轮重新 open 再试。 */

                  alarm_audio_close(fd);
                  fd = -1;
                }
            }
          else
            {
              /* 没有音频 fd（open 失败/写失败）：照样按 50ms 一块的节奏
               * 等待并检查状态，避免空转，也保证解除/超时仍然及时。 */

              usleep((useconds_t)(chunk * 1000000 / ALARM_SAMPLE_RATE));
            }

          pos += chunk;
        }

      alarm_audio_close(fd);

      if (decision != 0)
        {
          continue;   /* 已解除 / 需按新级别重来 */
        }

      nxmutex_lock(&g_alarm.lock);
      g_alarm.status.repeat_count++;
      nxmutex_unlock(&g_alarm.lock);

      /* ---- 两轮之间 ---- */

      round_ms = alarm_round_ms(level);
      interval = alarm_round_interval_ms(level);
      remain   = (ALARM_AUTO_TIMEOUT_MS > (alarm_now_ms() - triggered)) ?
                 (ALARM_AUTO_TIMEOUT_MS - (alarm_now_ms() - triggered)) : 0;

      if (interval == 0)
        {
          gap = remain;      /* NOTICE：只响一次，等自动超时/解除 */
        }
      else
        {
          gap = (interval > round_ms) ? (interval - round_ms) : 0;
          if (gap > remain)
            {
              gap = remain;
            }
        }

      if (gap > 0)
        {
          alarm_wait_gap(level, gap);
        }
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int alarm_init(void)
{
  pid_t pid;

  nxmutex_lock(&g_alarm.lock);
  if (g_alarm.initialized)
    {
      nxmutex_unlock(&g_alarm.lock);
      return OK;
    }

  g_alarm.initialized = true;
  nxmutex_unlock(&g_alarm.lock);

  pid = task_create(ALARM_WORKER_NAME, ALARM_WORKER_PRIORITY,
                    ALARM_WORKER_STACK, (main_t)alarm_worker, NULL);
  if (pid < 0)
    {
      nxmutex_lock(&g_alarm.lock);
      g_alarm.initialized = false;
      nxmutex_unlock(&g_alarm.lock);

      syslog(LOG_ERR, "ALARM: task_create failed: %d\n", (int)pid);
      return (int)pid;
    }

  g_alarm.worker = pid;
  syslog(LOG_INFO, "ALARM: worker started (pid=%d)\n", (int)pid);
  return OK;
}

int alarm_trigger(enum alarm_level_e level, FAR const char *reason,
                  FAR const char *text)
{
  int ret;

  if (level < ALARM_LEVEL_NOTICE || level > ALARM_LEVEL_EMERGENCY)
    {
      return -EINVAL;
    }

  ret = alarm_init();          /* 懒初始化：调用方不必先 init */
  if (ret < 0)
    {
      return ret;
    }

  nxmutex_lock(&g_alarm.lock);

  if (!g_alarm.status.active || level > g_alarm.status.level)
    {
      /* 新报警，或级别升高：按新级别重来 */

      g_alarm.status.level        = level;
      g_alarm.status.triggered_ms = alarm_now_ms();
      g_alarm.status.repeat_count = 0;
    }

  g_alarm.status.active = true;
  g_alarm.exit_event    = (enum alarm_event_e)0;

  alarm_set_string(g_alarm.status.reason, sizeof(g_alarm.status.reason),
                   reason);
  alarm_set_string(g_alarm.status.text, sizeof(g_alarm.status.text),
                   text);

  g_alarm.seq++;
  nxmutex_unlock(&g_alarm.lock);

  nxsem_post(&g_alarm.wake);
  return OK;
}

int alarm_clear(void)
{
  bool was_active;

  nxmutex_lock(&g_alarm.lock);
  was_active = g_alarm.status.active;

  if (was_active)
    {
      g_alarm.status.active = false;
      g_alarm.exit_event    = ALARM_EVENT_CLEARED;
    }

  nxmutex_unlock(&g_alarm.lock);

  if (was_active)
    {
      nxsem_post(&g_alarm.wake);
    }

  return OK;
}

int alarm_get_status(FAR struct alarm_status_s *out)
{
  if (out == NULL)
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_alarm.lock);
  *out = g_alarm.status;
  nxmutex_unlock(&g_alarm.lock);

  return OK;
}

int alarm_set_callback(alarm_cb_t cb, FAR void *arg)
{
  nxmutex_lock(&g_alarm.lock);
  g_alarm.cb     = cb;
  g_alarm.cb_arg = arg;
  nxmutex_unlock(&g_alarm.lock);

  return OK;
}
