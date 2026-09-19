/****************************************************************************
 * app/drvtest/main.c
 *
 * 独立的「驱动测试固件」应用 —— NSH 命令 drvtest
 *
 * 目的：把驱动层（音频 / 显示 / 触摸 / RTC / 按键 / I2C / 内存）在
 * **没有 LVGL、没有网络、没有两个 app 抢麦克风**的环境里测清楚。
 * 本文件只依赖板级驱动封装（board/contest_board/src 下的那几个 .h），
 * 不 include robot_ui / hello_app 的任何东西，也不开机自启 ——
 * 一律从 NSH 敲命令跑，方便控制。
 *
 * 子命令一览（`drvtest help` 也会打）：
 *   drvtest audio   [轮数] [每轮秒数]   反复 起录/读/停录，逐轮统计
 *   drvtest audiolong [分钟]            长跑：连录，每 5 秒一行统计
 *   drvtest play    [次数]              播放正弦 + 录-放-录 半双工交替
 *   drvtest lcd     [亮度 0-100]        亮度设定 + 回读（不带参数就扫几档）
 *   drvtest lcdfill                     整屏刷色（红绿蓝白黑）
 *   drvtest touch   [秒]                读 /dev/input0 + FT6146 的 I2C 探针
 *   drvtest rtc                         读时间 + 设 N 秒后的 alarm 等信号
 *   drvtest btn     [秒]                板级按键事件（GPIO 轮询，不走 /dev/buttons）
 *   drvtest heap                        mallinfo（free / 峰值）+ 大块分配探底
 *   drvtest i2c                         扫 /dev/i2c0 和 /dev/i2c1
 *   drvtest all                         顺序跑上面这些并汇总
 *
 * ---------------------------------------------------------------------------
 * 几条刻意的设计（都是被真机的坑教出来的）
 * ---------------------------------------------------------------------------
 * 1. **阻塞的 read 一律放独立任务，主任务带超时等它**：
 *    audio_in_read() 会一直阻塞到读满 / 被 STOP 打断 / 下层超时。
 *    如果直接在 NSH 任务里读，一旦驱动那侧出问题，整条命令行就永远回不来。
 * 2. **读任务用的缓冲是静态的，绝不 free**：
 *    读任务万一卡在驱动里、我们放弃了它，那块内存如果被释放就是
 *    use-after-free（驱动醒来还会往里写）。静态缓冲从根上避免这件事。
 * 3. **收尾顺序永远是先让读任务停下、再发设备级 STOP**：
 *    AUDIOIOC_STOP 是设备级的（同时停录和放），但本命令的会话就是自己开的，
 *    所以这里用 audio_in_stop() 是对的（被抢走那种情形才要用 abandon）。
 * 4. **每个测试都打数字**，失败要说清 errno 的名字。
 *
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/compiler.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <malloc.h>
#include <signal.h>
#include <poll.h>

#include <nuttx/audio/audio.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/lcd/lcd_dev.h>
#include <nuttx/video/fb.h>
#include <nuttx/input/touchscreen.h>
#include <nuttx/timers/rtc.h>
#include <nuttx/clock.h>          /* clock_systime_ticks() + TICK2MSEC() */
#include <nuttx/sched.h>          /* task_create() */

#include "sf32lb52_audio_in.h"     /* audio_in_start / read / stop + rx_stats */
#include "sf32lb52_backlight.h"    /* backlight_set / get */
#include "sf32lb52_rtc_alarm.h"    /* rtc_alarm_now / time_valid */
#include "sf32lb52_boardbtn.h"     /* 板级按键：GPIO 轮询 + 回调，不走 /dev/buttons */

/* ---- 只声明、不 include 的三个板级/芯片符号（touchinit 与 tpreg 要用）----
 *
 * BSP_TP_PowerUp() / BSP_TP_Reset()：实现在 board/contest_board/src/bsp_lcd_tp.c，
 * 原型在 board/contest_board/include/drv_io.h:75-77。那个 include/ 目录不在本
 * app 的 -I 里（app 只加 ${NUTTX_BOARD_ABS_DIR}/src），所以照原型再声明一遍，
 * 签名与 drv_io.h 完全一致。
 *
 * sifli_gpio_read()：原型在 vendor/sifli/chips/sf32lb52/sifli_gpio.h:168
 *     bool sifli_gpio_read(uint32_t pin);
 * 那份头会拉进 chip.h / bf0_hal.h（要 SOC_BF0_HCPU 一整套 HAL 宏），本 app
 * 不引它，只按同样签名声明一次。
 * pin 号口径：GPIO1 上的脚就是脚号本身 —— sifli_gpio.h 的
 * GET_PIN_2(hwp_gpio1, N) 展开就是 N（boardbtn 用 GET_PIN_2(hwp_gpio1,11) 读
 * PA11、sifli_ap.c 用 GET_PIN_2(hwp_gpio1, CONFIG_TOUCH_IRQ_PIN) 读 PA31），
 * 所以 PA09 = 9、PA31 = CONFIG_TOUCH_IRQ_PIN。
 */

extern void BSP_TP_PowerUp(void);
extern void BSP_TP_Reset(uint8_t high1_low0);
extern bool sifli_gpio_read(uint32_t pin);

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define AUDIO_DEV        "/dev/audio/audio0"
#define LCD_DEV          "/dev/lcd0"
#define INPUT_DEV        "/dev/input0"
#define RTC_DEV          "/dev/rtc0"
#define I2C0_DEV         "/dev/i2c0"
#define I2C1_DEV         "/dev/i2c1"

#define AUDIO_RATE       16000
#define AUDIO_CHANNELS   1
#define AUDIO_BITS       16

/* 下层一次 read 内部只等 5 秒，所以单次 read 取 1 秒的量最稳 */

#define REC_CHUNK_BYTES  (AUDIO_RATE * 2)

#define REC_TASK_STACK   4096
#define REC_TASK_PRIO    100

#define REC_STOP_WAIT_MS 1500     /* 发了 STOP 之后最多再等读任务多久 */

#define AUDIO_DEF_ROUNDS 3
#define AUDIO_DEF_SECS   2

#define LONG_DEF_MINUTES 5
#define LONG_STAT_MS     5000     /* 长跑统计行间隔 */
#define LONG_STALL_MS    15000    /* 连续这么久没读到任何字节就报可疑 */

#define PLAY_DEF_COUNT   2
#define PLAY_MS          500      /* 每次播放的正弦时长（ms） */
#define PLAY_TONE_HZ     1000
#define PLAY_VOLUME      700      /* 驱动的 0..1000 口径，700 = 70% */

#define LCD_BAND_ROWS    60       /* 刷色时每次提交多少行 */
#define LCD_DEF_PERCENT  100

#define RGB565_RED       0xf800
#define RGB565_GREEN     0x07e0
#define RGB565_BLUE      0x001f
#define RGB565_WHITE     0xffff
#define RGB565_BLACK     0x0000

#define TOUCH_MAX_POINTS 16
#define TOUCH_DEF_SECS   10
#define TOUCH_POLL_MS    200
#define TOUCH_WANT_PTS   20       /* 读满这么多点就提前收 */

#define RTC_DEF_ALARM_SEC 3
#define RTC_SLACK_SEC     2

#define BTN_DEF_SECS     15
#define BTN_POLL_MS      200

#define FT6146_ADDR      0x38
#define FT6146_REG_MODE  0x00     /* DEVICE_MODE */
#define FT6146_REG_TD    0x02     /* TD_STATUS（低 4 位 = 触点数） */
#define FT6146_REG_ID_H  0xa3
#define FT6146_REG_ID_L  0x9f
#define I2C_PROBE_FREQ   400000

#define TD_READ_TIMES    5        /* 0x02 连读几次看稳不稳 */

/* 触摸的两个控制脚（sifli_gpio_read 的 pin 号，GPIO1 上就是脚号本身，见文件头的声明注释） */

#define TP_RESET_PIN     9        /* PA09 = CTP_RESET（bsp_lcd_tp.c 的 TP_RESET） */
#define TP_INT_PIN       CONFIG_TOUCH_IRQ_PIN  /* PA31 = CTP_INT（INT 是开漏，要上拉） */

/* touchinit 的手册时序：复位拉低 5ms 以上、放开后等 80ms 再访问 I2C */

#define TP_RESET_LOW_MS  5
#define TP_RESET_BOOT_MS 80

#define TPREG_DEF_ADDR   FT6146_ADDR
#define TPREG_DEF_BUS    0        /* /dev/i2c0 */

static FAR const char *g_node_list[] =
{
  "/dev/audio/audio0",
  "/dev/lcd0",
  "/dev/fb0",
  "/dev/input0",
  "/dev/rtc0",
  "/dev/i2c0",
  "/dev/i2c1",
  "/dev/buttons",
  "/dev/gpio0",
  "/dev/timer0",
  "/dev/watchdog0",
};

#define NNODES ((int)(sizeof(g_node_list) / sizeof(g_node_list[0])))

/****************************************************************************
 * Private Data
 ****************************************************************************/

static int g_pass;
static int g_total;

/* ---- 录音读任务与主任务之间的交接状态（全是 volatile，无锁） ---- */

static volatile int  g_rec_stop;      /* 主任务要求读任务收尾 */
static volatile int  g_rec_done;      /* 读任务已经退出循环 */
static volatile long g_rec_bytes;     /* 累计读到的字节 */
static volatile int  g_rec_reads;     /* audio_in_read() 调用次数 */
static volatile int  g_rec_timeout;   /* errno = ETIMEDOUT 的次数 */
static volatile int  g_rec_eof;       /* read 返回 0 的次数 */
static volatile int  g_rec_err;       /* 其它负值次数 */
static volatile int  g_rec_ret;       /* 最后一次 read 的原始返回值 */
static volatile int  g_rec_errno;     /* 最后一次 read 的 errno */
static volatile int  g_rec_peak;      /* 读到的最大绝对幅度 */

/* 读任务的块缓冲：**静态**。理由见文件头第 2 条。 */

static int16_t g_rec_buf[AUDIO_RATE];           /* 1 秒 16k 单声道 */
static int16_t g_play_buf[AUDIO_RATE / 2];      /* 500ms 正弦 */

/* ---- rtc：alarm 到点由信号处理函数置位 ---- */

static volatile sig_atomic_t g_rtc_fired;

/* ---- btn：回调跑在板级按键轮询任务里，只写 volatile ---- */

static volatile int                     g_btn_seq;
static volatile enum board_btn_e        g_btn_which;
static volatile enum board_btn_event_e  g_btn_type;
static volatile uint32_t                g_btn_held;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: mono_ms
 *
 * Description:
 *   单调毫秒计时。用 clock_systime_ticks() + TICK2MSEC()，不要自己累加
 *   固定步长 —— 板子一忙（初始化 / 刷屏）循环体开销就被漏掉，读数会系统性偏小。
 ****************************************************************************/

static uint32_t mono_ms(void)
{
  return (uint32_t)TICK2MSEC(clock_systime_ticks());
}

/****************************************************************************
 * Name: errname
 ****************************************************************************/

static FAR const char *errname(int err)
{
  switch (err)
    {
      case 0:      return "OK";
      case EBUSY:  return "EBUSY";
      case ETIMEDOUT: return "ETIMEDOUT";
      case EINVAL: return "EINVAL";
      case ENODEV: return "ENODEV";
      case EPERM:  return "EPERM";
      case ENOENT: return "ENOENT";
      case ENOSYS: return "ENOSYS";
      case EAGAIN: return "EAGAIN";
      case EIO:    return "EIO";
      default:     break;
    }

  return "?";
}

/****************************************************************************
 * Name: report
 ****************************************************************************/

static void report(FAR const char *name, int ok, FAR const char *detail)
{
  g_total++;
  if (ok)
    {
      g_pass++;
    }

  printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", name,
         detail != NULL ? " - " : "", detail != NULL ? detail : "");
}

/****************************************************************************
 * Name: touch_mode_ok
 *
 * Description:
 *   打印模式字节并判断是否像一个正常工作的 FT6146（DEVICE_MODE 期望 0x10）。
 ****************************************************************************/

static FAR const char *describe_mode(uint8_t mode)
{
  if (mode == 0x10)
    {
      return "正常（0x10 = 工作模式）";
    }

  if (mode == 0x00)
    {
      return "0x00 = 芯片在但没进工作模式（或 I2C 时序问题）";
    }

  return "非预期值";
}

/****************************************************************************
 * Name: set_audio_volume
 *
 * Description:
 *   走标准音量接口：AUDIOIOC_CONFIGURE + AUDIO_TYPE_FEATURE + AUDIO_FU_VOLUME。
 *   驱动口径 0..1000（0 = -36dB，1000 = +6dB）。
 ****************************************************************************/

static int set_audio_volume(int vol)
{
  struct audio_caps_desc_s capdesc;
  int fd;
  int ret;

  fd = open(AUDIO_DEV, O_WRONLY);
  if (fd < 0)
    {
      return -errno;
    }

  memset(&capdesc, 0, sizeof(capdesc));
  capdesc.caps.ac_len            = sizeof(struct audio_caps_s);
  capdesc.caps.ac_type           = AUDIO_TYPE_FEATURE;
  capdesc.caps.ac_format.hw      = AUDIO_FU_VOLUME;
  capdesc.caps.ac_controls.hw[0] = (uint16_t)vol;

  ret = ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&capdesc);
  if (ret >= 0)
    {
      ret = 0;
    }
  else
    {
      ret = -errno;
    }

  close(fd);
  return ret;
}

/****************************************************************************
 * Name: audio_start_path / audio_stop_path
 *
 * Description:
 *   播放通路的 open + CONFIGURE + START 与 STOP + close。
 *   录音通路走板级封装（audio_pin_start），这里只留播放这一条。
 ****************************************************************************/

static int audio_play_start(FAR int *fdp)
{
  struct audio_caps_desc_s capdesc;
  int fd;
  int ret;

  fd = open(AUDIO_DEV, O_WRONLY);
  if (fd < 0)
    {
      return -errno;
    }

  memset(&capdesc, 0, sizeof(capdesc));
  capdesc.caps.ac_len            = sizeof(struct audio_caps_s);
  capdesc.caps.ac_type           = AUDIO_TYPE_OUTPUT;
  capdesc.caps.ac_channels       = AUDIO_CHANNELS;
  capdesc.caps.ac_controls.hw[0] = AUDIO_RATE;
  capdesc.caps.ac_controls.b[2]  = AUDIO_BITS;

  ret = ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&capdesc);
  if (ret < 0)
    {
      ret = -errno;
      close(fd);
      return ret;
    }

  ret = ioctl(fd, AUDIOIOC_START, 0);
  if (ret < 0)
    {
      ret = -errno;
      close(fd);
      return ret;
    }

  *fdp = fd;
  return 0;
}

static void audio_play_stop(int fd)
{
  ioctl(fd, AUDIOIOC_STOP, 0);
  close(fd);
}

/****************************************************************************
 * Name: gen_sine
 ****************************************************************************/

static void gen_sine(FAR int16_t *buf, int nsamples, int freq, double amp)
{
  int i;

  for (i = 0; i < nsamples; i++)
    {
      buf[i] = (int16_t)(32767.0 * amp *
                         sin(2.0 * M_PI * (double)freq *
                             (double)i / (double)AUDIO_RATE));
    }
}

/****************************************************************************
 * Name: play_pcm
 *
 * Description:
 *   把一段 16k / 单声道 / 16bit PCM 整段写进播放通路。返回写成功的字节数，
 *   负数 = 负 errno。
 ****************************************************************************/

static long play_pcm(FAR const void *buf, size_t len)
{
  int fd = -1;
  int ret;
  ssize_t n;

  ret = audio_play_start(&fd);
  if (ret < 0)
    {
      printf("      播放通路打不开：%d (%s)\n", ret, errname(-ret));
      return ret;
    }

  n = write(fd, buf, len);
  if (n < 0)
    {
      ret = -errno;
      n = ret;
      printf("      write 失败：%d (%s)\n", ret, errname(-ret));
    }

  audio_play_stop(fd);
  return (long)n;
}

/****************************************************************************
 * Name: rec_reset
 ****************************************************************************/

static void rec_reset(void)
{
  g_rec_stop    = 0;
  g_rec_done    = 0;
  g_rec_bytes   = 0;
  g_rec_reads   = 0;
  g_rec_timeout = 0;
  g_rec_eof     = 0;
  g_rec_err     = 0;
  g_rec_ret     = 0;
  g_rec_errno   = 0;
  g_rec_peak    = 0;
}

/****************************************************************************
 * Name: rec_reader_task
 *
 * Description:
 *   录音读任务：按 1 秒一块反复 audio_in_read()，直到
 *     - 主任务置 g_rec_stop
 *     - read 返回 0（EOF：被 STOP 打断 / 设备没在跑 / 会话换代）
 *     - read 返回 ETIMEDOUT 之外的错误
 *   ETIMEDOUT **不退出**：那是"这一帧没数据"，会话还活着，跳过接着读
 *   （这是 audio_in_read 头文件里明确写的处理方式）。
 ****************************************************************************/

static int rec_reader_task(int argc, FAR char *argv[])
{
  (void)argc;
  (void)argv;

  while (!g_rec_stop)
    {
      ssize_t n;
      int i;

      errno = 0;
      n = audio_in_read(g_rec_buf, REC_CHUNK_BYTES);
      g_rec_reads++;
      g_rec_ret = (int)n;

      if (n > 0)
        {
          int ns = (int)(n / 2);

          g_rec_bytes += n;
          for (i = 0; i < ns; i++)
            {
              int v = g_rec_buf[i];

              if (v < 0)
                {
                  v = -v;
                }

              if (v > g_rec_peak)
                {
                  g_rec_peak = v;
                }
            }
        }
      else if (n == 0)
        {
          g_rec_eof++;
          break;
        }
      else
        {
          g_rec_errno = errno;
          if (errno == ETIMEDOUT)
            {
              g_rec_timeout++;
            }
          else
            {
              g_rec_err++;
              break;
            }
        }
    }

  g_rec_done = 1;
  return 0;
}

/****************************************************************************
 * Name: rec_wait_done
 ****************************************************************************/

static void rec_wait_done(uint32_t ms)
{
  uint32_t t0 = mono_ms();

  while (!g_rec_done && (uint32_t)(mono_ms() - t0) < ms)
    {
      usleep(20 * 1000);
    }
}

/****************************************************************************
 * Name: print_rx_stats
 *
 * Description:
 *   打一行 sf32lb52_audio_rx_stats() 的读数（6 个计数 + armed/busy）。
 *   六个计数器是"开机以来"的口径，只看趋势；要判断"刚刚这 5 秒流没流"，
 *   得靠调用方做差值。
 ****************************************************************************/

static int rx_stats_snapshot(FAR struct sf32lb52_audio_rx_stats_s *out)
{
  memset(out, 0, sizeof(*out));
  return sf32lb52_audio_rx_stats(out);
}

static void print_rx_stats(FAR const struct sf32lb52_audio_rx_stats_s *s,
                           FAR const struct sf32lb52_audio_rx_stats_s *prev)
{
  if (prev != NULL)
    {
      printf("      rx: irq=%lu(+%ld) half=%lu(+%ld) read=%lu(+%ld) "
             "timeout=%lu(+%ld) dma_err=%lu(+%ld) lost=%lu(+%ld) "
             "armed=%d busy=%d\n",
             (unsigned long)s->irq, (long)(s->irq - prev->irq),
             (unsigned long)s->half, (long)(s->half - prev->half),
             (unsigned long)s->read, (long)(s->read - prev->read),
             (unsigned long)s->timeout, (long)(s->timeout - prev->timeout),
             (unsigned long)s->dma_err, (long)(s->dma_err - prev->dma_err),
             (unsigned long)s->lost, (long)(s->lost - prev->lost),
             s->armed, s->busy);
    }
  else
    {
      printf("      rx: irq=%lu half=%lu read=%lu timeout=%lu dma_err=%lu "
             "lost=%lu armed=%d busy=%d\n",
             (unsigned long)s->irq, (unsigned long)s->half,
             (unsigned long)s->read, (unsigned long)s->timeout,
             (unsigned long)s->dma_err, (unsigned long)s->lost,
             s->armed, s->busy);
    }
}

/****************************************************************************
 * Name: audio_one_round
 *
 * Description:
 *   跑一轮：audio_in_start -> 起读任务读 seconds 秒 -> 收尾。
 *   返回 0 = 这一轮成功；负数 = 负 errno（-EBUSY 表示设备被别人占着）。
 ****************************************************************************/

static int audio_one_round(int seconds, FAR long *bytes, FAR int *frames,
                           FAR int *peak, FAR int *timeouts, FAR int *eofs)
{
  uint32_t t0;
  int ret;

  rec_reset();

  ret = audio_in_start(AUDIO_RATE, AUDIO_CHANNELS, AUDIO_BITS);
  if (ret < 0)
    {
      return ret;
    }

  if (task_create("drv_rec", REC_TASK_PRIO, REC_TASK_STACK,
                  rec_reader_task, NULL) < 0)
    {
      int e = errno;

      audio_in_stop();
      return -e;
    }

  /* 等它读够 seconds 秒；宽限 2 秒，超了就收尾 */

  t0 = mono_ms();
  while (!g_rec_done && (uint32_t)(mono_ms() - t0) < (uint32_t)(seconds + 2) * 1000)
    {
      usleep(20 * 1000);
    }

  if (!g_rec_done)
    {
      printf("      到了 %d 秒 + 宽限还没读完，主动收尾\n", seconds);
    }

  /* 先让读任务停，再发设备级 STOP（这个会话是我们自己开的，STOP 是对的） */

  g_rec_stop = 1;
  ret = audio_in_stop();
  if (ret < 0)
    {
      printf("      audio_in_stop 返回 %d (%s)\n", ret, errname(-ret));
    }

  rec_wait_done(REC_STOP_WAIT_MS);

  *bytes    = g_rec_bytes;
  *frames   = (int)(g_rec_bytes / 2);
  *peak     = g_rec_peak;
  *timeouts = g_rec_timeout;
  *eofs     = g_rec_eof;

  if (!g_rec_done)
    {
      /* 读任务还卡在驱动里：静态缓冲不怕 use-after-free，但这一轮算失败 */
      return -ETIMEDOUT;
    }

  return 0;
}

/****************************************************************************
 * Name: test_audio
 ****************************************************************************/

static int test_audio(int rounds, int seconds)
{
  int n_ok = 0;
  int n_busy = 0;
  int n_tmo = 0;
  int n_other = 0;
  long total_bytes = 0;
  int r;
  char detail[160];

  printf("[audio] %s 连跑 %d 轮，每轮读 %d 秒\n", AUDIO_DEV, rounds, seconds);

  for (r = 1; r <= rounds; r++)
    {
      long bytes = 0;
      int frames = 0;
      int peak = 0;
      int tmo = 0;
      int eofs = 0;
      uint32_t t0 = mono_ms();
      int ret;

      ret = audio_one_round(seconds, &bytes, &frames, &peak, &tmo, &eofs);
      if (ret == -EBUSY)
        {
          n_busy++;
          printf("  第 %d/%d 轮: audio_in_start 返回 -EBUSY（设备被活着的会话占着）\n",
                 r, rounds);
          usleep(200 * 1000);
          continue;
        }

      if (ret == -ETIMEDOUT)
        {
          n_tmo++;
          printf("  第 %d/%d 轮: -ETIMEDOUT —— 发了 STOP 之后读任务 %d ms 还没退出来"
                 "（读卡在驱动里），本轮读了 %ld 字节\n",
                 r, rounds, REC_STOP_WAIT_MS, bytes);
          continue;
        }

      if (ret < 0)
        {
          n_other++;
          printf("  第 %d/%d 轮: 失败 ret=%d (%s) 读了 %ld 字节\n",
                 r, rounds, ret, errname(-ret), bytes);
          continue;
        }

      n_ok++;
      total_bytes += bytes;
      printf("  第 %d/%d 轮: %ld ms, %ld 字节 (%d 帧 = %d ms), peak=%d, "
             "read=%d 超时=%d EOF=%d\n",
             r, rounds, (long)(mono_ms() - t0), bytes, frames,
             frames * 1000 / AUDIO_RATE, peak,
             (int)g_rec_reads, tmo, eofs);

      {
        struct sf32lb52_audio_rx_stats_s rx;

        rx_stats_snapshot(&rx);
        print_rx_stats(&rx, NULL);
      }
    }

  snprintf(detail, sizeof(detail),
           "%d/%d 轮成功, -EBUSY %d 次, -ETIMEDOUT %d 次, 其它失败 %d 次, "
           "共读 %ld 字节",
           n_ok, rounds, n_busy, n_tmo, n_other, total_bytes);
  report("audio 多轮录音", n_ok == rounds, detail);

  return n_ok == rounds ? 0 : -1;
}

/****************************************************************************
 * Name: test_audiolong
 *
 * Description:
 *   长跑：一次 audio_in_start，然后一直录，每 5 秒打一行统计。
 *   这是抓「读永久阻塞 / 断流」的关键 —— 短测看不见的两种形态：
 *     - bytes 停止增长且 armed=1 busy=1，rx.irq/half 也一个都不涨
 *       => 通路武装着、就是不来数据（ADC/AUDPRC 侧）；
 *     - bytes 停止增长但 rx.half 在涨
 *       => 数据在流，只有"完成通知"那一路丢了（等待侧的问题）。
 ****************************************************************************/

static int test_audiolong(int minutes)
{
  struct sf32lb52_audio_rx_stats_s prev;
  struct sf32lb52_audio_rx_stats_s cur;
  uint32_t t_start;
  uint32_t t_last;
  uint32_t limit_ms;
  long last_bytes = 0;
  long last_reads = 0;
  uint32_t last_progress_ms;
  int stalls = 0;
  int ret;

  printf("[audiolong] 连续录音 %d 分钟，每 %d 秒一行统计"
         "（分钟数用满会自己收尾；中途按 Ctrl-C 只会杀掉本命令，"
         "读任务还占着麦克风，要再录得先重启）\n", minutes, LONG_STAT_MS / 1000);

  rec_reset();

  ret = audio_in_start(AUDIO_RATE, AUDIO_CHANNELS, AUDIO_BITS);
  if (ret < 0)
    {
      printf("  audio_in_start 失败：%d (%s)\n", ret, errname(-ret));
      report("audiolong 启动录音", 0, errname(-ret));
      return -1;
    }

  if (task_create("drv_lrec", REC_TASK_PRIO, REC_TASK_STACK,
                  rec_reader_task, NULL) < 0)
    {
      printf("  task_create 失败：%d\n", errno);
      audio_in_stop();
      report("audiolong 启动录音", 0, "起读任务失败");
      return -1;
    }

  rx_stats_snapshot(&prev);

  t_start        = mono_ms();
  t_last         = t_start;
  last_progress_ms = t_start;
  limit_ms       = (uint32_t)minutes * 60 * 1000;

  while ((uint32_t)(mono_ms() - t_start) < limit_ms)
    {
      usleep(100 * 1000);

      if ((uint32_t)(mono_ms() - t_last) < LONG_STAT_MS)
        {
          continue;
        }

      {
        uint32_t now   = mono_ms();
        uint32_t dt    = now - t_last;
        long     bytes = g_rec_bytes;

        rx_stats_snapshot(&cur);

        printf("  [long] t=%lus 间隔 %lums: 本段 %ld 字节 (%.1f kB/s) "
               "累计 %ld 字节 peak=%d\n",
               (unsigned long)((now - t_start) / 1000), (long)dt,
               bytes - last_bytes,
               (double)(bytes - last_bytes) * 1000.0 / (double)dt / 1024.0,
               bytes, g_rec_peak);
        printf("      read=%ld(+%ld) 超时=%d EOF=%d 错=%d 最后 ret=%d errno=%d(%s)\n",
               (long)g_rec_reads, (long)g_rec_reads - last_reads,
               g_rec_timeout, g_rec_eof, g_rec_err,
               g_rec_ret, g_rec_errno, errname(g_rec_errno));
        print_rx_stats(&cur, &prev);

        if (bytes != last_bytes)
          {
            last_progress_ms = now;
          }
        else if ((uint32_t)(now - last_progress_ms) >= LONG_STALL_MS)
          {
            stalls++;
            printf("      *** 警告：已经 %lu ms 没有新数据了"
                   "（armed=%d busy=%d, irq 本段 +%ld, half 本段 +%ld）***\n",
                   (unsigned long)(now - last_progress_ms),
                   cur.armed, cur.busy,
                   (long)(cur.irq - prev.irq), (long)(cur.half - prev.half));
          }

        last_bytes = bytes;
        last_reads = g_rec_reads;
        prev       = cur;
        t_last     = now;
      }

      if (g_rec_done)
        {
          printf("  *** 读任务自己退出了（EOF=%d 错=%d 最后 ret=%d errno=%d/%s）"
                 "——会话提前结束 ***\n",
                 g_rec_eof, g_rec_err, g_rec_ret, g_rec_errno,
                 errname(g_rec_errno));
          break;
        }
    }

  g_rec_stop = 1;
  audio_in_stop();
  rec_wait_done(REC_STOP_WAIT_MS);

  printf("  [long] 结束：累计 %ld 字节 (%ld 帧 = %ld 秒音频), read=%ld, "
         "超时=%d, EOF=%d, 错=%d, peak=%d\n",
         g_rec_bytes, g_rec_bytes / 2,
         (long)(g_rec_bytes / 2) / AUDIO_RATE,
         (long)g_rec_reads, g_rec_timeout, g_rec_eof, g_rec_err, g_rec_peak);
  rx_stats_snapshot(&cur);
  print_rx_stats(&cur, &prev);

  {
    char detail[128];

    snprintf(detail, sizeof(detail),
             "%ld 字节, 超时 %d, 错 %d, 可疑停顿 %d 次, 读任务%s",
             g_rec_bytes, g_rec_timeout, g_rec_err, stalls,
             g_rec_done ? "正常退出" : "卡住");
    report("audiolong 长跑", stalls == 0 && g_rec_done, detail);
  }

  return (stalls == 0 && g_rec_done) ? 0 : -1;
}

/****************************************************************************
 * Name: test_play
 ****************************************************************************/

static int test_play(int count)
{
  int i;
  int ok = 0;
  int ret;

  printf("[play] 先设音量 %d/1000\n", PLAY_VOLUME);
  ret = set_audio_volume(PLAY_VOLUME);
  printf("  SETVOLUME ret=%d (%s)\n", ret, errname(-ret));
  report("播放音量下发", ret == 0, ret == 0 ? NULL : errname(-ret));

  gen_sine(g_play_buf, AUDIO_RATE / 2, PLAY_TONE_HZ, 0.2);

  for (i = 1; i <= count; i++)
    {
      uint32_t t0 = mono_ms();
      long n;
      long ms;

      n = play_pcm(g_play_buf, sizeof(g_play_buf));
      ms = (long)(mono_ms() - t0);

      printf("  第 %d/%d 次: 写 %ld/%u 字节, %ld ms（期望约 %d ms）\n",
             i, count, n, (unsigned)sizeof(g_play_buf), ms, PLAY_MS);

      if (n == (long)sizeof(g_play_buf))
        {
          ok++;
        }
    }

  {
    char detail[96];

    snprintf(detail, sizeof(detail), "%d/%d 次写满（%dHz %dms 正弦）",
             ok, count, PLAY_TONE_HZ, PLAY_MS);
    report("播放正弦", ok == count, detail);
  }

  /* ---- 半双工交替：录 1 秒 -> 放这 1 秒 -> 再录 1 秒 ---- */

  printf("[halfduplex] 录 -> 放 -> 录\n");

  {
    long bytes = 0;
    int frames = 0;
    int peak = 0;
    int tmo = 0;
    int eofs = 0;
    long played;

    ret = audio_one_round(1, &bytes, &frames, &peak, &tmo, &eofs);
    printf("  录1: ret=%d 字节=%ld peak=%d 超时=%d EOF=%d\n",
           ret, bytes, peak, tmo, eofs);
    report("半双工 录1", ret == 0 && bytes > 0, errname(-ret));

    played = play_pcm(g_rec_buf, (size_t)(bytes > 0 ? bytes : 0));
    printf("  放1: 写 %ld/%ld 字节（就是刚录到的那段）\n", played, bytes);
    report("半双工 放1", played == bytes && bytes > 0, NULL);

    bytes = 0;
    ret = audio_one_round(1, &bytes, &frames, &peak, &tmo, &eofs);
    printf("  录2: ret=%d 字节=%ld peak=%d 超时=%d EOF=%d\n",
           ret, bytes, peak, tmo, eofs);
    report("半双工 录2（放完之后还能录）", ret == 0 && bytes > 0, errname(-ret));
  }

  return ok == count ? 0 : -1;
}

/****************************************************************************
 * Name: lcd_info
 ****************************************************************************/

static int lcd_info(int fd, FAR struct fb_videoinfo_s *vinfo)
{
  memset(vinfo, 0, sizeof(*vinfo));

  if (ioctl(fd, LCDDEVIO_GETVIDEOINFO, (unsigned long)vinfo) < 0)
    {
      return -errno;
    }

  printf("  面板: %ux%u fmt=%u nplanes=%u\n",
         vinfo->xres, vinfo->yres, vinfo->fmt, vinfo->nplanes);
  return 0;
}

/****************************************************************************
 * Name: test_lcd
 ****************************************************************************/

static int test_lcd(int percent, int percent_given)
{
  struct fb_videoinfo_s vinfo;
  static const int sweep[] = { 0, 30, 60, 100 };
  int fd;
  int ret;
  unsigned i;
  int ok = 1;

  printf("[lcd] %s 亮度\n", LCD_DEV);

  fd = open(LCD_DEV, O_RDWR);
  if (fd < 0)
    {
      printf("  open 失败: %d (%s)\n", errno, errname(errno));
      report("打开 LCD", 0, errname(errno));
      return -1;
    }

  ret = lcd_info(fd, &vinfo);
  report("LCD 面板信息", ret == 0, ret == 0 ? NULL : errname(-ret));
  close(fd);

  if (percent_given)
    {
      printf("  设定亮度 %d%%:\n", percent);
      ret = backlight_set(percent);
      usleep(200 * 1000);
      printf("    backlight_set(%d) = %d (%s), 回读 = %d\n",
             percent, ret, errname(-ret), backlight_get());
      ok = (ret == 0) && (backlight_get() == percent);
      report("亮度设定与回读", ok, ok ? NULL : "回读与设定不一致");

      /* 别把屏幕留在 0% 上，否则后面几项什么都看不见 */

      if (percent == 0)
        {
          backlight_set(LCD_DEF_PERCENT);
        }
    }
  else
    {
      for (i = 0; i < sizeof(sweep) / sizeof(sweep[0]); i++)
        {
          int got;

          ret = backlight_set(sweep[i]);
          usleep(200 * 1000);
          got = backlight_get();
          printf("    设 %3d%% -> set ret=%d (%s), 回读 %d%%\n",
                 sweep[i], ret, errname(-ret), got);

          if (ret != 0 || got != sweep[i])
            {
              ok = 0;
            }
        }

      backlight_set(LCD_DEF_PERCENT);
      report("亮度全档扫描 0/30/60/100", ok, ok ? NULL : "有档位没兑现");
    }

  return ok ? 0 : -1;
}

/****************************************************************************
 * Name: lcd_fill
 *
 * Description:
 *   用 LCDDEVIO_PUTAREA 把 [row0, row1] 刷成一个 RGB565 颜色。
 *   一次只提交 LCD_BAND_ROWS 行，避免 malloc 整屏（390x450x2 = 351KB）。
 *   小端：低字节在前（与 hw_test 的刷色写法一致）。
 ****************************************************************************/

static int lcd_fill(int fd, uint16_t xres, uint16_t yres,
                    uint16_t row0, uint16_t row1, uint16_t color,
                    FAR uint8_t *buf, size_t bufsize)
{
  struct lcddev_area_s area;
  uint32_t row;

  if (row1 > yres)
    {
      row1 = yres;
    }

  for (row = row0; row < row1; row += LCD_BAND_ROWS)
    {
      uint32_t rows = (uint32_t)(row1 - row);
      size_t npixel;
      size_t need;
      size_t i;
      int ret;

      if (rows > LCD_BAND_ROWS)
        {
          rows = LCD_BAND_ROWS;
        }

      npixel = (size_t)xres * rows;
      need   = npixel * 2;
      if (need > bufsize)
        {
          return -ENOMEM;
        }

      for (i = 0; i < npixel; i++)
        {
          buf[i * 2]     = (uint8_t)(color & 0xff);
          buf[i * 2 + 1] = (uint8_t)(color >> 8);
        }

      memset(&area, 0, sizeof(area));
      area.row_start = (fb_coord_t)row;
      area.row_end   = (fb_coord_t)(row + rows - 1);
      area.col_start = 0;
      area.col_end   = (fb_coord_t)(xres - 1);
      area.stride    = (fb_coord_t)((uint32_t)xres * 2);
      area.data      = buf;

      ret = ioctl(fd, LCDDEVIO_PUTAREA, (unsigned long)&area);
      if (ret < 0)
        {
          printf("    PUTAREA(row %u..%u) 失败: %d (%s)\n",
                 (unsigned)row, (unsigned)(row + rows - 1),
                 errno, errname(errno));
          return -errno;
        }
    }

  return 0;
}

/****************************************************************************
 * Name: test_lcdfill
 ****************************************************************************/

static int test_lcdfill(void)
{
  struct fb_videoinfo_s vinfo;
  FAR uint8_t *buf;
  size_t bufsize;
  int fd;
  int ret;
  int ok = 1;
  static const struct
  {
    uint16_t    color;
    FAR const char *name;
  } bands[] =
  {
    { RGB565_RED,   "红" },
    { RGB565_GREEN, "绿" },
    { RGB565_BLUE,  "蓝" },
    { RGB565_WHITE, "白" },
    { RGB565_BLACK, "黑（清屏）" },
  };
  unsigned i;

  printf("[lcdfill] 整屏刷色（RGB565，分 %d 行一块）\n", LCD_BAND_ROWS);

  fd = open(LCD_DEV, O_RDWR);
  if (fd < 0)
    {
      printf("  open 失败: %d (%s)\n", errno, errname(errno));
      report("lcdfill 打开 LCD", 0, errname(errno));
      return -1;
    }

  ret = lcd_info(fd, &vinfo);
  if (ret < 0 || vinfo.xres == 0 || vinfo.yres == 0)
    {
      printf("  GETVIDEOINFO 失败: %d (%s)\n", ret, errname(-ret));
      report("lcdfill 面板信息", 0, errname(-ret));
      close(fd);
      return -1;
    }

  bufsize = (size_t)vinfo.xres * LCD_BAND_ROWS * 2;
  buf = (FAR uint8_t *)malloc(bufsize);
  if (buf == NULL)
    {
      printf("  malloc %u 失败\n", (unsigned)bufsize);
      report("lcdfill 缓冲", 0, "内存不足");
      close(fd);
      return -1;
    }

  for (i = 0; i < sizeof(bands) / sizeof(bands[0]); i++)
    {
      uint32_t t0 = mono_ms();

      ret = lcd_fill(fd, vinfo.xres, vinfo.yres, 0, vinfo.yres,
                     bands[i].color, buf, bufsize);
      printf("  %s 0x%04x: ret=%d, %ld ms\n", bands[i].name, bands[i].color,
             ret, (long)(mono_ms() - t0));

      if (ret < 0)
        {
          ok = 0;
        }

      usleep(400 * 1000);       /* 让肉眼能看清每一色 */
    }

  free(buf);
  close(fd);

  report("整屏刷色 红绿蓝白黑", ok, ok ? NULL : "有颜色没刷上");
  return ok ? 0 : -1;
}

/****************************************************************************
 * Name: i2c_xfer
 *
 * Description:
 *   /dev/i2cX 是**字符设备**（nuttx/drivers/i2c/i2c_driver.c），发事务要
 *   ioctl(fd, I2CIOC_TRANSFER, &struct i2c_transfer_s)，不能直接用
 *   I2C_TRANSFER() —— 那个宏是给手里已经有 struct i2c_master_s * 的
 *   板级代码用的，拿 fd（int）去点它的 ->ops 编都编不过。
 *   返回 0 成功，负值 = 负 errno。
 ****************************************************************************/

static int i2c_xfer(int fd, FAR struct i2c_msg_s *msg, int nmsg)
{
  struct i2c_transfer_s xfer;

  xfer.msgv = msg;
  xfer.msgc = (size_t)nmsg;

  return ioctl(fd, I2CIOC_TRANSFER, (unsigned long)&xfer);
}

/****************************************************************************
 * Name: i2c_read_regs
 *
 * Description:
 *   标准的两段式读：先写 1 字节寄存器号（无 STOP），再读 len 字节
 *   （无 START）。和 ft6146.c 里的 ft6146_i2c_read() 是同一套写法。
 ****************************************************************************/

static int i2c_read_regs(int fd, uint8_t addr, uint8_t reg,
                         FAR uint8_t *buf, int len)
{
  struct i2c_msg_s msg[2];

  memset(msg, 0, sizeof(msg));

  msg[0].frequency = I2C_PROBE_FREQ;
  msg[0].addr      = addr;
  msg[0].flags     = I2C_M_NOSTOP;
  msg[0].buffer    = &reg;
  msg[0].length    = 1;

  msg[1].frequency = I2C_PROBE_FREQ;
  msg[1].addr      = addr;
  msg[1].flags     = I2C_M_READ | I2C_M_NOSTART;
  msg[1].buffer    = buf;
  msg[1].length    = len;

  return i2c_xfer(fd, msg, 2);
}

/****************************************************************************
 * Name: i2c_probe
 *
 * Description:
 *   只读探测一个地址：发一次 1 字节的读。芯片在就会 ACK（返回 0），
 *   不在就 NAK（返回负值）。**不做任何写操作**，扫到陌生地址也不会改它的状态。
 ****************************************************************************/

static int i2c_probe(int fd, uint8_t addr)
{
  struct i2c_msg_s msg;
  uint8_t byte = 0;

  memset(&msg, 0, sizeof(msg));
  msg.frequency = I2C_PROBE_FREQ;
  msg.addr      = addr;
  msg.flags     = I2C_M_READ;
  msg.buffer    = &byte;
  msg.length    = 1;

  return i2c_xfer(fd, &msg, 1);
}

/****************************************************************************
 * Name: i2c_errname
 *
 * Description:
 *   I2C_TRANSFER 的负值既可能是"下层直接给的负 errno"，也可能是
 *   "经上层翻成 -1 + errno"。两种都认，免得日志里出现没用的问号。
 ****************************************************************************/

static FAR const char *i2c_errname(int ret)
{
  if (ret >= 0)
    {
      return "OK";
    }

  return errname(ret == -1 ? errno : -ret);
}

/****************************************************************************
 * Name: test_i2c_bus
 ****************************************************************************/

static int test_i2c_bus(FAR const char *dev)
{
  int fd;
  int found = 0;
  int addr;

  printf("[i2c] 扫描 %s（只读探测，地址 0x08-0x77）\n", dev);

  fd = open(dev, O_RDWR);
  if (fd < 0)
    {
      printf("  open 失败: %d (%s)\n", errno, errname(errno));
      report("打开 I2C 总线", 0, errname(errno));
      return -1;
    }

  for (addr = 0x08; addr <= 0x77; addr++)
    {
      if (i2c_probe(fd, (uint8_t)addr) == 0)
        {
          printf("  发现设备 0x%02x%s\n", addr,
                 addr == FT6146_ADDR ? "  <- FT6146 触摸屏" : "");
          found++;
        }
    }

  close(fd);

  printf("  %s: 共发现 %d 个设备\n", dev, found);

  {
    char detail[64];

    snprintf(detail, sizeof(detail), "%s 上发现 %d 个设备", dev, found);
    report("I2C 总线扫描", found > 0, detail);
  }

  return found > 0 ? 0 : -1;
}

/****************************************************************************
 * Name: test_touch
 ****************************************************************************/

static int touch_i2c_probe_at(FAR const char *dev, uint8_t addr)
{
  uint8_t mode = 0;
  uint8_t td[TD_READ_TIMES];
  uint8_t idh  = 0;
  uint8_t idl  = 0;
  int fd;
  int ret;
  int ok;
  int i;
  int td_ok      = 0;
  int td_nonzero = 0;

  printf("[touch] FT6146 I2C 探针：%s @ 0x%02x\n", dev, addr);

  fd = open(dev, O_RDWR);
  if (fd < 0)
    {
      printf("  open %s 失败: %d (%s)\n", dev, errno, errname(errno));
      report("FT6146 探针", 0, errname(errno));
      return -1;
    }

  /* 0x00 = DEVICE_MODE：正常工作的 FT 芯片是 0x10。
   * 这一项就是"PA31(INT) 上拉"那条修复的对照读数 —— 只有芯片真的在
   * I2C 上应答、进了工作模式，INT 才可能有意义。 */

  ret = i2c_read_regs(fd, addr, FT6146_REG_MODE, &mode, 1);
  printf("  reg 0x00 DEVICE_MODE = 0x%02x（ret=%d/%s） %s\n",
         mode, ret, i2c_errname(ret), describe_mode(mode));

  /* 0x02 = TD_STATUS：低 4 位是当前触点数。**连读 5 次** —— 只读一次分不清
   * "这一拍刚好没摸"和"这条读通路一直回 0"。 */

  printf("  reg 0x02 TD_STATUS（连读 %d 次，低 4 位 = 触点数）:",
         TD_READ_TIMES);

  for (i = 0; i < TD_READ_TIMES; i++)
    {
      td[i] = 0xff;
      ret = i2c_read_regs(fd, addr, FT6146_REG_TD, &td[i], 1);
      printf(" [%d]0x%02x/%d", i + 1, td[i], td[i] & 0x0f);

      if (ret == 0)
        {
          td_ok++;
        }

      if (td[i] != 0)
        {
          td_nonzero++;
        }
    }

  printf("\n");

  if (td_nonzero == 0)
    {
      printf("      %d 次全是 0x00（成功 %d 次）—— 手指按住屏幕也是 0 的话，"
             "说明要么 IC 没在扫（0x00/0x06 那套配置没生效），要么这条读通路"
             "本身就回不来数据。\n"
             "      下一步：drvtest tpreg 0x80 5a 看能不能读回刚写的值"
             "（能读回 = 读通路没问题，是 IC 侧），再 drvtest touchinit。\n",
             TD_READ_TIMES, td_ok);
    }
  else if (td_ok < TD_READ_TIMES)
    {
      printf("      有 %d 次读失败（%d 次成功）—— 读通路不稳，重点看这个。\n",
             TD_READ_TIMES - td_ok, td_ok);
    }

  /* 顺手读芯片 ID，区分"这是个真 FT 芯片"和"总线上有别的东西应答" */

  ret = i2c_read_regs(fd, addr, FT6146_REG_ID_H, &idh, 1);
  if (ret == 0)
    {
      ret = i2c_read_regs(fd, addr, FT6146_REG_ID_L, &idl, 1);
    }

  printf("  reg 0xa3/0x9f 芯片 ID = 0x%02x%02x（ret=%d/%s）\n",
         idh, idl, ret, i2c_errname(ret));

  close(fd);

  ok = (mode == 0x10);
  report("FT6146 I2C 探针（0x00/0x02）", ok,
         ok ? NULL : "DEVICE_MODE 不是 0x10");

  return ok ? 0 : -1;
}

static int touch_i2c_probe(void)
{
  return touch_i2c_probe_at(I2C0_DEV, FT6146_ADDR);
}

static int touch_read_events(int seconds)
{
  uint8_t buf[sizeof(struct touch_sample_s) +
              TOUCH_MAX_POINTS * sizeof(struct touch_point_s)];
  uint32_t t0;
  int samples = 0;
  int fd;

  printf("  读 %s，观察 %d 秒（最多 %d 个点），请点一下屏幕\n",
         INPUT_DEV, seconds, TOUCH_WANT_PTS);

  fd = open(INPUT_DEV, O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    {
      printf("  open 失败: %d (%s)\n", errno, errname(errno));
      report("打开触摸设备", 0, errname(errno));
      return -1;
    }

  t0 = mono_ms();
  while (samples < TOUCH_WANT_PTS &&
         (uint32_t)(mono_ms() - t0) < (uint32_t)seconds * 1000)
    {
      struct pollfd pfd;
      ssize_t n;
      int pret;
      int i;

      pfd.fd      = fd;
      pfd.events  = POLLIN;
      pfd.revents = 0;

      pret = poll(&pfd, 1, TOUCH_POLL_MS);
      if (pret < 0)
        {
          usleep(TOUCH_POLL_MS * 1000);
        }
      else if (pret == 0)
        {
          continue;
        }

      memset(buf, 0, sizeof(buf));
      n = read(fd, buf, sizeof(buf));
      if (n < (ssize_t)sizeof(struct touch_sample_s))
        {
          continue;      /* EAGAIN / 被别的 reader 抢走 */
        }

      {
        FAR struct touch_sample_s *sample = (FAR struct touch_sample_s *)buf;
        int npoints = sample->npoints;

        if (npoints > TOUCH_MAX_POINTS)
          {
            npoints = TOUCH_MAX_POINTS;
          }

        for (i = 0; i < npoints && samples < TOUCH_WANT_PTS; i++)
          {
            FAR struct touch_point_s *pt = &sample->point[i];

            samples++;
            printf("  #%-2d id=%d flags=0x%02x x=%d y=%d h=%d%s%s%s\n",
                   samples, pt->id, pt->flags, pt->x, pt->y, pt->h,
                   (pt->flags & TOUCH_DOWN) != 0 ? " DOWN" : "",
                   (pt->flags & TOUCH_MOVE) != 0 ? " MOVE" : "",
                   (pt->flags & TOUCH_UP) != 0 ? " UP" : "");
          }
      }
    }

  close(fd);

  printf("  共读到 %d 个样点\n", samples);

  /* 没人碰屏幕不算失败（和 hw_test 同一个口径） */

  {
    char detail[64];

    snprintf(detail, sizeof(detail), "%d 个样点", samples);
    report("触摸读样点", 1, detail);
  }

  return samples;
}

/****************************************************************************
 * Name: test_touch
 ****************************************************************************/

static int test_touch(int seconds)
{
  int ret = touch_i2c_probe();

  touch_read_events(seconds);
  return ret;
}

/****************************************************************************
 * Name: print_pins
 *
 * Description:
 *   打一行 CTP_RESET(PA09) / CTP_INT(PA31) 的**实际电平**。
 *   用 sifli_gpio_read()（就是 HAL_GPIO_ReadPin() 的直接封装）：读不到就是 0，
 *   所以"复位前后两个脚一直是 0、没有变化"本身也是一条有用的证据。
 *
 *   注意这里一次把两个脚都打出来：**不要**写成返回 static 缓冲的
 *   pin_str() 再在一个 printf 里调两次 —— 两次调用共用同一块缓冲，
 *   两次 %s 会打出同一个值。
 ****************************************************************************/

static void print_pins(FAR const char *tag)
{
  char rst[24];
  char int_pin[24];

  snprintf(rst, sizeof(rst), "PA%02u=%d", (unsigned)TP_RESET_PIN,
           (int)sifli_gpio_read(TP_RESET_PIN));
  snprintf(int_pin, sizeof(int_pin), "PA%02u=%d", (unsigned)TP_INT_PIN,
           (int)sifli_gpio_read(TP_INT_PIN));

  printf("  %-14s: CTP_RESET(%s)  CTP_INT(%s)\n", tag, rst, int_pin);
}

/****************************************************************************
 * Name: test_touchinit
 *
 * Description:
 *   `drvtest touchinit [秒]` —— 在**运行时**照厂商 SDK 的顺序把触摸重做一遍
 *   上电 + 复位，每一步打日志，然后再打探针：
 *
 *     BSP_TP_PowerUp() -> BSP_TP_Reset(0) -> 5ms -> BSP_TP_Reset(1) -> 80ms
 *
 *   判读（这条命令存在的意义就在这里）：
 *     - 重做之后 0x00 变成 0x10、ID 读得出 => **开机那次初始化没生效**
 *       （上电/复位时序问题），不是 IC 坏；
 *     - 重做之后还是全 0 => **跟有没有复位无关**，往读通路（配合
 *       `drvtest tpreg 0x80 5a` 的写回读）/ IC / 硬件那边查。
 *
 *   PA09(CTP_RESET) 和 PA31(CTP_INT) 每步都打实际电平。PA31 是开漏 INT，
 *   空闲时应当被上拉到 1；一直 0 要么上拉没生效，要么芯片把 INT 拉着不放。
 ****************************************************************************/

static int test_touchinit(int seconds)
{
  int ret;

  printf("[touchinit] 运行时重做触摸上电 + 复位\n");
  print_pins("复位前");

  /* bsp_lcd_tp.c 的 BSP_TP_PowerUp = BSP_PIN_Touch() + CTP_RESET=1 */

  printf("  BSP_TP_PowerUp()\n");
  BSP_TP_PowerUp();
  usleep(5 * 1000);
  print_pins("PowerUp 之后");

  printf("  BSP_TP_Reset(0) ... 等 %d ms\n", TP_RESET_LOW_MS);
  BSP_TP_Reset(0);
  usleep(TP_RESET_LOW_MS * 1000);
  print_pins("复位拉低时");

  printf("  BSP_TP_Reset(1) ... 等 %d ms\n", TP_RESET_BOOT_MS);
  BSP_TP_Reset(1);
  usleep(TP_RESET_BOOT_MS * 1000);
  print_pins("复位放开后");

  usleep(20 * 1000);
  print_pins("再等 20 ms 后");

  printf("  ---- 复位后探针 ----\n");
  ret = touch_i2c_probe();

  if (seconds > 0)
    {
      printf("  ---- 复位后读 /dev/input0 ----\n");
      touch_read_events(seconds);
    }

  if (ret == 0)
    {
      printf("  => 重做初始化之后探针正常：开机那次初始化有问题（上电/复位"
             "时序），不是 IC 坏。\n");
    }
  else
    {
      printf("  => 重做初始化之后仍然不正常：跟'有没有复位'无关，继续查读通路"
             "（drvtest tpreg <reg> 5a）和硬件。\n");
    }

  report("touchinit 重做上电+复位", ret == 0,
         ret == 0 ? "复位后探针正常" : "复位后仍然全 0");
  return ret;
}

/****************************************************************************
 * Name: tpreg_write
 *
 * Description:
 *   写一个寄存器：一次发 2 字节（寄存器号 + 值），正常带 STOP。
 ****************************************************************************/

static int tpreg_write(int fd, uint8_t addr, uint8_t reg, uint8_t val)
{
  uint8_t buf[2];
  struct i2c_msg_s msg;

  buf[0] = reg;
  buf[1] = val;

  memset(&msg, 0, sizeof(msg));
  msg.frequency = I2C_PROBE_FREQ;
  msg.addr      = addr;
  msg.flags     = 0;
  msg.buffer    = buf;
  msg.length    = 2;

  return i2c_xfer(fd, &msg, 1);
}

/****************************************************************************
 * Name: test_tpreg
 *
 * Description:
 *   `drvtest tpreg <reg_hex> [value_hex] [addr_hex] [bus]`
 *
 *   不给 value：读该寄存器 3 次（看这一路读数稳不稳）。
 *   给了 value：**先写后读** —— 判定"读通路本身是否有效"的关键实验：
 *     · 读回 == 刚写的值 => 读通路没问题（写进去的能原样读出来），
 *       那么 0x00/0xa3 读回 0x00 就是 IC 侧的事，别再查 SDA 上拉；
 *     · 写返回 OK（从机 ACK）但读回 0x00 => **读通路有问题**：从机能拉低 SDA
 *       应答，主机却一个 1 都读不到 —— 典型是 SDA 拉不上去 / 上拉没生效 /
 *       SDA 与 SCL 接反 / 速率太高；
 *     · 写就失败 => 连写都发不出去，先修总线本身。
 *
 *   ⚠ 写寄存器会真的改芯片行为：
 *     · 0xa3/0x9f 是**只读**芯片 ID、0x02 是**只读** TD_STATUS —— 拿它们做
 *       写回读实验，读回来一定"不符"，判不出读通路好坏（本函数会先提醒）；
 *     · 0x00 是**模式**寄存器（0x5A 不是合法模式，芯片可能压根不保存它），
 *       可以试但不是干净的证据；
 *     · 想判读通路，用 **0x80（THGROUP）** 这类参数寄存器最干净：写 0x5A
 *       （0x5A = 0101_1010，1 和 0 都有），能原样读回就说明读通路能正确
 *       采到 1 和 0。再补一次 0xa5 更稳（两个互补模式各测一遍）。
 ****************************************************************************/

static int test_tpreg(uint8_t reg, int has_val, uint8_t val,
                      uint8_t addr, int bus)
{
  FAR const char *dev = (bus == 1) ? I2C1_DEV : I2C0_DEV;
  uint8_t rd = 0;
  uint8_t last_rd = 0;
  int fd;
  int ret;
  int i;
  int zero_count = 0;
  int ok_reads   = 0;

  printf("[tpreg] %s @ 0x%02x，寄存器 0x%02x%s\n", dev, addr, reg,
         has_val ? "（先写后读）" : "（只读 3 次）");

  fd = open(dev, O_RDWR);
  if (fd < 0)
    {
      printf("  open %s 失败: %d (%s)\n", dev, errno, errname(errno));
      report("tpreg 打开 I2C", 0, errname(errno));
      return -1;
    }

  if (has_val)
    {
      /* 提醒一句：写寄存器是真的改芯片行为。0xa3/0x9f 是只读芯片 ID、
       * 0x02 是只读 TD_STATUS，拿它们做写实验读回来一定"不符"，容易误判。 */

      if (reg == FT6146_REG_ID_H || reg == FT6146_REG_ID_L ||
          reg == FT6146_REG_TD)
        {
          printf("  ⚠ 0x%02x 是只读寄存器（芯片 ID / TD_STATUS），写它读回来的"
                 "值不会等于写进去的值，别拿它判读通路；\n"
                 "    建议换 0x80 / 0x86 / 0x88 这类参数寄存器。\n", reg);
        }

      printf("  写 0x%02x <- 0x%02x ...\n", reg, val);
      ret = tpreg_write(fd, addr, reg, val);
      printf("  写返回 %d (%s)%s\n", ret, i2c_errname(ret),
             ret == 0 ? "  <- 从机 ACK 了这次写" : "");
      report("tpreg 写寄存器", ret == 0, i2c_errname(ret));

      if (ret != 0)
        {
          printf("  => 连写都发不出去，先修总线（上拉 / 引脚 / 速率）。\n");
          close(fd);
          return -1;
        }

      usleep(10 * 1000);       /* 给芯片一点时间把值吃进去 */
    }

  for (i = 0; i < 3; i++)
    {
      rd  = 0xff;
      ret = i2c_read_regs(fd, addr, reg, &rd, 1);
      printf("  读[%d] 0x%02x = 0x%02x（ret=%d/%s）\n",
             i + 1, reg, rd, ret, i2c_errname(ret));

      if (ret == 0)
        {
          ok_reads++;
          last_rd = rd;

          if (rd == 0)
            {
              zero_count++;
            }
        }
    }

  close(fd);

  if (ok_reads == 0)
    {
      printf("  => 3 次读全部失败（ret 见上）—— 这不是「读回 0」，是根本没读成，"
             "先修总线/引脚再谈别的。\n");
      report("tpreg 读寄存器", 0, "3 次读全部失败");
      return -1;
    }

  if (has_val)
    {
      if (ok_reads < 3)
        {
          printf("  ⚠ 只有 %d/%d 次读成功，下面的判读用最后一次成功的读数"
                 "（0x%02x）。\n", ok_reads, 3, last_rd);
        }

      if (last_rd == val)
        {
          printf("  => **读通路没问题**：写进去的 0x%02x 原样读回来了。"
                 "0x00/0xa3 读回 0x00 是 IC 侧的事（没工作 / 没配置 / 坏），"
                 "不用再查 SDA 上拉。\n", val);
          report("tpreg 先写后读", 1, "读回值等于写入值");
          return 0;
        }

      if (last_rd == 0x00)
        {
          printf("  => **读通路有问题**：写 ACK 了（SDA 能被从机拉低），但读回来"
                 "是 0x00（主机侧一个 1 都读不到）。重点查 SDA 上拉"
                 "（PA33 的 PIN_PULLUP 有没有生效 / 外部电阻）、SDA 与 SCL "
                 "是否接反、把速率从 %d 降到 100k 再试。\n", I2C_PROBE_FREQ);
          report("tpreg 先写后读", 0, "写成功但读回 0x00（读通路问题）");
          return -1;
        }

      printf("  => 读回来的 0x%02x 既不等于写入值也不是 0x00：这个寄存器可能"
             "只读，或者被芯片自己改写了。换一个参数寄存器再试。\n", last_rd);
      report("tpreg 先写后读", 0, "读回值与写入值不符");
      return -1;
    }

  if (zero_count == ok_reads && ok_reads == 3)
    {
      printf("  => 3 次全是 0x00：这一路读不出东西（配合先写后读一起判读）。\n");
    }

  report("tpreg 读寄存器", 1, NULL);
  return 0;
}

/****************************************************************************
 * Name: rtc_sig_handler
 ****************************************************************************/

static void rtc_sig_handler(int signo)
{
  (void)signo;
  g_rtc_fired = 1;
}

/****************************************************************************
 * Name: test_rtc
 ****************************************************************************/

static int test_rtc(int alarm_sec)
{
  struct rtc_setrelative_s rel;
  struct rtc_rdalarm_s query;
  struct sigaction sa;
  struct rtc_time rt;
  uint32_t t0;
  uint32_t elapsed;
  int fd;
  int ok;

  printf("[rtc] %s\n", RTC_DEV);

  fd = open(RTC_DEV, O_RDONLY);
  if (fd < 0)
    {
      printf("  open 失败: %d (%s)\n", errno, errname(errno));
      report("打开 RTC", 0, errname(errno));
      return -1;
    }

  memset(&rt, 0, sizeof(rt));
  if (ioctl(fd, RTC_RD_TIME, (unsigned long)&rt) < 0)
    {
      printf("  RTC_RD_TIME 失败: %d (%s)\n", errno, errname(errno));
      report("读 RTC 时间", 0, errname(errno));
      close(fd);
      return -1;
    }

  printf("  当前时间: %04d-%02d-%02d %02d:%02d:%02d（time_valid=%d）\n",
         rt.tm_year + 1900, rt.tm_mon + 1, rt.tm_mday,
         rt.tm_hour, rt.tm_min, rt.tm_sec, rtc_alarm_time_valid());
  report("读 RTC 时间", 1, NULL);

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = rtc_sig_handler;
  sigemptyset(&sa.sa_mask);

  if (sigaction(SIGUSR1, &sa, NULL) < 0)
    {
      printf("  sigaction(SIGUSR1) 失败: %d (%s)\n", errno, errname(errno));
      report("RTC alarm", 0, "装信号处理失败");
      close(fd);
      return -1;
    }

  g_rtc_fired = 0;

  memset(&rel, 0, sizeof(rel));
  rel.id                 = 0;
  rel.pid                = 0;            /* 0 = 通知调用者自己 */
  rel.event.sigev_notify = SIGEV_SIGNAL;
  rel.event.sigev_signo  = SIGUSR1;
  rel.reltime            = alarm_sec;

  if (ioctl(fd, RTC_SET_RELATIVE, (unsigned long)&rel) < 0)
    {
      printf("  RTC_SET_RELATIVE(%d 秒) 失败: %d (%s)\n",
             alarm_sec, errno, errname(errno));
      report("RTC alarm", 0, errname(errno));
      close(fd);
      return -1;
    }

  printf("  已设 %d 秒后的 alarm，最多等 %d 秒...\n",
         alarm_sec, alarm_sec + RTC_SLACK_SEC);

  t0 = mono_ms();
  while (!g_rtc_fired &&
         (uint32_t)(mono_ms() - t0) < (uint32_t)(alarm_sec + RTC_SLACK_SEC) * 1000)
    {
      usleep(50 * 1000);
    }

  elapsed = mono_ms() - t0;
  ok = (g_rtc_fired != 0);

  if (!ok)
    {
      ioctl(fd, RTC_CANCEL_ALARM, 0);
      printf("  超时：等了 %lu ms 没收到 SIGUSR1\n", (unsigned long)elapsed);
    }
  else
    {
      printf("  收到 SIGUSR1：alarm 在 %lu ms 触发（设定 %d 秒）\n",
             (unsigned long)elapsed, alarm_sec);

      memset(&query, 0, sizeof(query));
      query.id = 0;
      if (ioctl(fd, RTC_RD_ALARM, (unsigned long)&query) == 0)
        {
          printf("  RTC_RD_ALARM: active=%d 时间 %02d:%02d:%02d"
                 "（日期字段 HAL 不填）\n",
                 (int)query.active, query.time.tm_hour,
                 query.time.tm_min, query.time.tm_sec);
        }
    }

  close(fd);
  report("RTC alarm（相对 %d 秒）", ok, ok ? NULL : "超时未收到信号");
  return ok ? 0 : -1;
}

/****************************************************************************
 * Name: btn_cb
 *
 * Description:
 *   板级按键回调。**跑在按键模块的轮询任务里**，只能更新 volatile 变量，
 *   不阻塞、不碰 LVGL、不开文件。
 ****************************************************************************/

static void btn_cb(enum board_btn_e btn, enum board_btn_event_e ev,
                   uint32_t held_ms, FAR void *arg)
{
  (void)arg;

  g_btn_which = btn;
  g_btn_type  = ev;
  g_btn_held  = held_ms;
  g_btn_seq++;
}

/****************************************************************************
 * Name: test_btn
 ****************************************************************************/

static int test_btn(int seconds)
{
  uint32_t t0;
  int last = 0;
  int events = 0;
  int i;
  int ret;

  printf("[btn] 板级按键（GPIO 轮询 + 回调，不走 /dev/buttons）\n");
  printf("  观察 %d 秒，请按 KEY(PA11)\n", seconds);

  ret = board_btn_set_callback(btn_cb, NULL);
  if (ret < 0)
    {
      printf("  board_btn_set_callback 失败: %d (%s)\n", ret, errname(-ret));
      report("按键模块初始化", 0, errname(-ret));
      return -1;
    }

  t0 = mono_ms();
  while ((uint32_t)(mono_ms() - t0) < (uint32_t)seconds * 1000)
    {
      usleep(BTN_POLL_MS * 1000);

      while (last != g_btn_seq)
        {
          last = g_btn_seq;
          events++;
          printf("  #%-2d %s %s held=%lums\n", events,
                 board_btn_name(g_btn_which), board_btn_event_name(g_btn_type),
                 (unsigned long)g_btn_held);
        }
    }

  board_btn_set_callback(NULL, NULL);

  printf("  共收到 %d 个按键事件\n", events);

  for (i = 0; i < BOARD_BTN_COUNT; i++)
    {
      printf("  %s 当前电平: %d\n", board_btn_name((enum board_btn_e)i),
             board_btn_is_pressed((enum board_btn_e)i));
    }

  {
    char detail[80];

    snprintf(detail, sizeof(detail), "%d 个事件", events);
    report("按键事件回调", events > 0, detail);
  }

  return events > 0 ? 0 : -1;
}

/****************************************************************************
 * Name: test_heap
 ****************************************************************************/

static int test_heap(void)
{
  struct mallinfo mi;
  size_t probe = 0;
  FAR void *p;

  printf("[heap] 堆使用情况\n");

  mi = mallinfo();

  printf("  arena    = %u 字节（堆总大小）\n", mi.arena);
  printf("  uordblks = %u 字节（在用，%u 块）\n", mi.uordblks, mi.aordblks);
  printf("  fordblks = %u 字节（空闲，%u 块）\n", mi.fordblks, mi.ordblks);
  printf("  mxordblk = %u 字节（最大单块空闲）\n", mi.mxordblk);
  printf("  usmblks  = %u 字节（**开机以来的堆峰值**）\n", mi.usmblks);

  /* 探底：一块到底能要多大。加一个"下一次要更大的会不会失败"的边界读数。 */

  for (probe = mi.mxordblk; probe > 1024; probe -= 1024)
    {
      p = malloc(probe);
      if (p != NULL)
        {
          free(p);
          break;
        }
    }

  printf("  最大一次能 malloc %u 字节（请求 %u，成功水位 %u）\n",
         (unsigned)probe, mi.mxordblk, (unsigned)probe);

  {
    char detail[96];

    snprintf(detail, sizeof(detail), "free=%u 峰值=%u 最大块=%u",
             mi.fordblks, mi.usmblks, mi.mxordblk);
    report("堆状态", mi.fordblks > 0, detail);
  }

  return 0;
}

/****************************************************************************
 * Name: test_nodes
 ****************************************************************************/

static int test_nodes(void)
{
  int missing = 0;
  int optional = 0;
  int i;

  printf("[nodes] 驱动设备节点\n");

  for (i = 0; i < NNODES; i++)
    {
      struct stat st;

      if (stat(g_node_list[i], &st) == 0)
        {
          printf("  OK  %-20s\n", g_node_list[i]);
        }
      else
        {
          /* /dev/buttons 与 /dev/timer0 不是本测试的必需项 */

          printf("  --  %-20s 不存在（%s）\n", g_node_list[i],
                 errname(errno));
          if (strcmp(g_node_list[i], "/dev/buttons") == 0 ||
              strcmp(g_node_list[i], "/dev/timer0") == 0)
            {
              optional++;
            }
          else
            {
              missing++;
            }
        }
    }

  {
    char detail[96];

    snprintf(detail, sizeof(detail), "缺 %d 个必需节点，%d 个可选项没注册",
             missing, optional);
    report("必需设备节点存在", missing == 0, detail);
  }

  return missing == 0 ? 0 : -1;
}

/****************************************************************************
 * Name: test_i2c
 ****************************************************************************/

static int test_i2c(void)
{
  int a = test_i2c_bus(I2C0_DEV);
  int b = test_i2c_bus(I2C1_DEV);

  return (a == 0 && b == 0) ? 0 : -1;
}

/****************************************************************************
 * Name: test_all
 ****************************************************************************/

static int test_all(void)
{
  int before_pass = g_pass;
  int before_total = g_total;

  printf("============ drvtest all ============\n");

  test_nodes();
  test_heap();
  test_i2c();
  test_lcd(LCD_DEF_PERCENT, 0);
  test_lcdfill();
  test_touch(5);
  test_rtc(RTC_DEF_ALARM_SEC);
  test_btn(5);
  test_audio(3, 2);
  test_play(1);

  printf("============ 汇总 ============\n");
  printf("  %d/%d 项 PASS，%d 项 FAIL\n",
         g_pass - before_pass, g_total - before_total,
         (g_total - before_total) - (g_pass - before_pass));
  printf("  （触摸若有异常，接着跑：drvtest touchinit；"
         "读通路存疑就跑 drvtest tpreg 0x80 5a）\n");

  return (g_pass == g_total) ? 0 : 1;
}

/****************************************************************************
 * Name: usage
 ****************************************************************************/

static void usage(void)
{
  printf("drvtest - 独立驱动测试固件（NSH 命令）\n"
         "\n"
         "  drvtest audio    [轮数] [每轮秒数]   反复 起录/读/停录，逐轮统计\n"
         "  drvtest audiolong [分钟]            长跑连续录音，每 5 秒一行统计\n"
         "  drvtest play     [次数]             播放正弦 + 录-放-录 半双工交替\n"
         "  drvtest lcd      [亮度 0-100]       亮度设定 + 回读\n"
         "  drvtest lcdfill                     整屏刷色（红绿蓝白黑）\n"
         "  drvtest touch    [秒]               /dev/input0 读点 + FT6146 I2C 探针\n"
         "  drvtest touchinit [秒]              运行时重做触摸上电+复位，再打探针\n"
         "  drvtest tpreg    <reg> [value] [addr] [bus]\n"
         "                                      读/写 FT6146 寄存器（给了 value 就\n"
         "                                      先写后读，判「读通路是否有效」）\n"
         "  drvtest rtc                         读时间 + 设 N 秒后的 alarm\n"
         "  drvtest btn      [秒]               板级按键事件（GPIO 轮询）\n"
         "  drvtest heap                        mallinfo + 大块分配探底\n"
         "  drvtest i2c                         扫 /dev/i2c0 与 /dev/i2c1\n"
         "  drvtest all                         顺序跑全部并汇总\n"
         "\n"
         "  默认值：audio %d %d / play %d / touch %d / btn %d / rtc %d\n"
         "  tpreg 的 reg/value/addr 都按十六进制认（0x 前缀可省），\n"
         "  addr 默认 0x%02x，bus 默认 %d（0 -> /dev/i2c0，1 -> /dev/i2c1）\n",
         AUDIO_DEF_ROUNDS, AUDIO_DEF_SECS, PLAY_DEF_COUNT,
         TOUCH_DEF_SECS, BTN_DEF_SECS, RTC_DEF_ALARM_SEC,
         TPREG_DEF_ADDR, TPREG_DEF_BUS);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  FAR const char *cmd;

  g_pass  = 0;
  g_total = 0;

  if (argc < 2)
    {
      usage();
      return 0;
    }

  cmd = argv[1];

  if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0)
    {
      usage();
      return 0;
    }

  if (strcmp(cmd, "audio") == 0)
    {
      int rounds  = (argc > 2) ? atoi(argv[2]) : AUDIO_DEF_ROUNDS;
      int seconds = (argc > 3) ? atoi(argv[3]) : AUDIO_DEF_SECS;

      if (rounds < 1)
        {
          rounds = AUDIO_DEF_ROUNDS;
        }

      if (seconds < 1)
        {
          seconds = AUDIO_DEF_SECS;
        }

      return test_audio(rounds, seconds);
    }

  if (strcmp(cmd, "audiolong") == 0)
    {
      int minutes = (argc > 2) ? atoi(argv[2]) : LONG_DEF_MINUTES;

      if (minutes < 1)
        {
          minutes = LONG_DEF_MINUTES;
        }

      return test_audiolong(minutes);
    }

  if (strcmp(cmd, "play") == 0)
    {
      int count = (argc > 2) ? atoi(argv[2]) : PLAY_DEF_COUNT;

      if (count < 1)
        {
          count = PLAY_DEF_COUNT;
        }

      return test_play(count);
    }

  if (strcmp(cmd, "lcd") == 0)
    {
      int given   = 0;
      int percent = LCD_DEF_PERCENT;

      /* 只有下一个参数确实是数字才当亮度，免得把别的字面量 atoi 成 0 */

      if (argc > 2)
        {
          char *end = NULL;
          long v = strtol(argv[2], &end, 10);

          if (end != NULL && *end == '\0')
            {
              percent = (int)v;
              given   = 1;
            }
        }

      if (percent < BACKLIGHT_MIN_PERCENT || percent > BACKLIGHT_MAX_PERCENT)
        {
          printf("亮度要 0..100，给了 %d\n", percent);
          return 1;
        }

      return test_lcd(percent, given);
    }

  if (strcmp(cmd, "lcdfill") == 0)
    {
      return test_lcdfill();
    }

  if (strcmp(cmd, "touch") == 0)
    {
      int seconds = (argc > 2) ? atoi(argv[2]) : TOUCH_DEF_SECS;

      if (seconds < 1)
        {
          seconds = TOUCH_DEF_SECS;
        }

      return test_touch(seconds);
    }

  if (strcmp(cmd, "touchinit") == 0)
    {
      /* 秒数可以给 0：那就只做上电+复位+探针，不去读事件 */

      int seconds = (argc > 2) ? atoi(argv[2]) : 0;

      if (seconds < 0)
        {
          seconds = 0;
        }

      return test_touchinit(seconds);
    }

  if (strcmp(cmd, "tpreg") == 0)
    {
      char *end = NULL;
      long  v;
      uint8_t reg;
      uint8_t val = 0;
      uint8_t addr = TPREG_DEF_ADDR;
      int bus  = TPREG_DEF_BUS;
      int have = 0;

      if (argc < 3)
        {
          printf("用法: drvtest tpreg <reg_hex> [value_hex] [addr_hex] [bus]\n");
          usage();
          return 1;
        }

      /* 三个数字都按十六进制认（"5a" / "0x5a" 都行），给了 value 才写 */

      v = strtol(argv[2], &end, 16);
      if (end == NULL || *end != '\0' || v < 0 || v > 0xff)
        {
          printf("tpreg: 寄存器号要 0x00..0xff，给了 '%s'\n", argv[2]);
          return 1;
        }

      reg = (uint8_t)v;

      if (argc > 3)
        {
          v = strtol(argv[3], &end, 16);
          if (end == NULL || *end != '\0' || v < 0 || v > 0xff)
            {
              printf("tpreg: 值要 0x00..0xff，给了 '%s'\n", argv[3]);
              return 1;
            }

          val  = (uint8_t)v;
          have = 1;
        }

      if (argc > 4)
        {
          v = strtol(argv[4], &end, 16);
          if (end == NULL || *end != '\0' || v < 0x08 || v > 0x77)
            {
              printf("tpreg: 从机地址要 0x08..0x77，给了 '%s'\n", argv[4]);
              return 1;
            }

          addr = (uint8_t)v;
        }

      if (argc > 5)
        {
          bus = atoi(argv[5]);
          if (bus != 0 && bus != 1)
            {
              printf("tpreg: bus 只能是 0 或 1，给了 '%s'（0 -> %s，1 -> %s）\n",
                     argv[5], I2C0_DEV, I2C1_DEV);
              return 1;
            }
        }

      return test_tpreg(reg, have, val, addr, bus);
    }

  if (strcmp(cmd, "rtc") == 0)
    {
      int sec = (argc > 2) ? atoi(argv[2]) : RTC_DEF_ALARM_SEC;

      if (sec < 1)
        {
          sec = RTC_DEF_ALARM_SEC;
        }

      return test_rtc(sec);
    }

  if (strcmp(cmd, "btn") == 0 || strcmp(cmd, "button") == 0)
    {
      int seconds = (argc > 2) ? atoi(argv[2]) : BTN_DEF_SECS;

      if (seconds < 1)
        {
          seconds = BTN_DEF_SECS;
        }

      return test_btn(seconds);
    }

  if (strcmp(cmd, "heap") == 0)
    {
      return test_heap();
    }

  if (strcmp(cmd, "i2c") == 0)
    {
      return test_i2c();
    }

  if (strcmp(cmd, "all") == 0)
    {
      return test_all();
    }

  printf("drvtest: 不认识的子命令 '%s'\n\n", cmd);
  usage();
  return 1;
}
