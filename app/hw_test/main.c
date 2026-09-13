/****************************************************************************
 * app/hw_test/main.c
 *
 * SF32LB52-DevKit-LCD 硬件自检（显示 / 触摸 / 按键 / GPIO /
 *                                  IMU / RTC / 音频输入）
 *
 * 用法：
 *   hw_test                 只读自检：不动屏幕、不拉 GPIO 电平
 *   hw_test touch <秒>       指定触摸观察时长（0 = 跳过触摸步骤）
 *   hw_test lcdcolor        额外做一次刷色测试（会改屏，退出前清屏）
 *   hw_test gpio            额外翻转一次板级 GPIO 输出脚 PA26（/dev/gpio1）
 *   hw_test imu [帧数]       读 LSM6DS3 加速度/陀螺（默认 10 帧）—— 单独运行
 *   hw_test rtc [秒]         读 RTC 时间 + 设一个 N 秒后的 alarm（默认 3 秒）—— 单独运行
 *   hw_test rtcday [时] [分]  每日定时提醒：到点回调一次（默认 = 当前时间 + 1 分钟）
 *                            —— 单独运行，最多等 90 秒
 *   hw_test audio [秒]       录 N 秒到内存，打印 peak/avg（默认 2 秒）—— 单独运行
 *   hw_test alarm [级别] [秒] 触发报警（1=提示 2=警告 3=紧急，默认 3），
 *                            持续 N 秒（默认 3）后解除 —— 单独运行
 *   hw_test lcd [0..100]     设屏幕亮度（默认 100）并回读 —— 单独运行；
 *                            本板只有 0 / 100 是真的，中间值会 FAIL
 *   hw_test button [秒]      等按键按下（默认 15 秒）：按到才 PASS，
 *                            超时是 FAIL —— 单独运行
 *   hw_test status           打印统一外设状态（board_status_get）：网络拿
 *                            到 IPv4 地址才算 PASS，其它设备只提示 —— 单独运行
 *
 * 设计约定：
 *   - 每一步失败都只打印 FAIL，不中断后面的步骤，也不会卡死
 *     （所有 open/ioctl/read 都判返回值，read 前先用 poll 等超时）
 *   - 默认（不带参数）不碰屏幕、不拉 GPIO 电平，只做只读自检
 *   - imu / rtc / audio / alarm / button 都会真的开外设（START 转换、
 *     设 alarm、开麦克风、响喇叭、等按键），所以放在子命令里；而且它们
 *     **不跑**上面那套 5 步自检，只跑自己，免得每次验 IMU 还要先等
 *     10 秒触摸 + 5 秒按键
 *   - 默认自检里的按键步骤是"非交互"的（没人按也算 PASS，只证明能读）；
 *     要真的验证按键，跑 `hw_test button`，它超时会 FAIL
 *   - **按键一律走板级 GPIO 模块 `sf32lb52_boardbtn`，不碰 /dev/buttons**：
 *     那个节点的 poll()/read() 在真机上会让整机静默卡死（零输出、USB 设备
 *     消失、只能重烧），现象和原因写在 `sf32lb52_boardbtn.h` 文件头
 *
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/compiler.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <poll.h>

#include <nuttx/lcd/lcd_dev.h>
#include <nuttx/video/fb.h>
#include <nuttx/input/touchscreen.h>
#include <nuttx/input/buttons.h>
#include <nuttx/ioexpander/gpio.h>
#include <nuttx/timers/rtc.h>
#include <nuttx/sensors/ioctl.h>       /* SNIOC_START / SNIOC_STOP / ... */
#include <nuttx/clock.h>               /* clock_systime_ticks() + TICK2MSEC() */
#include <nuttx/sched.h>               /* task_create() */
#include <signal.h>                    /* SIGUSR1 + sigaction() */

/* 音频不再直接开 fd：录音走板级封装 sf32lb52_audio_in（它自己 include
 * <nuttx/audio/audio.h>），这里只保留设备路径用于打印。 */
#include "sf32lb52_alarm.h"            /* 板级报警模块（只出声，不驱灯） */
#include "sf32lb52_rtc_alarm.h"        /* 板级 RTC 每日定时提醒模块 */
#include "sf32lb52_audio_in.h"         /* 板级录音封装 audio_in_start/read/stop */
#include "sf32lb52_backlight.h"        /* 板级亮度封装 backlight_set/get */
#include "sf32lb52_boardbtn.h"         /* 板级按键：GPIO 轮询 + 回调（不用 /dev/buttons） */
#include "sf32lb52_status.h"           /* 板级统一外设状态 board_status_get/dump */

/* IMU：本板的 LSM6DS3 走的是 NuttX **老式字符驱动**（不是 uORB），
 * 节点是 /dev/lsm6dsl0，接口是 read() + ioctl(SNIOC_*)。
 * 头文件本身被 CONFIG_SENSORS_LSM6DSL 包着，所以这里也要判一下，
 * 否则编不过（拿不到 struct lsm6dsl_sensor_data_s）。
 */

#if defined(CONFIG_I2C) && defined(CONFIG_SENSORS_LSM6DSL)
#  include <nuttx/sensors/lsm6dsl.h>
#  define HW_TEST_HAS_IMU 1
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define LCD_DEV        "/dev/lcd0"
#define INPUT_DEV      "/dev/input0"
#define GPIO_IN_DEV    "/dev/gpio0"
#define GPIO_OUT_DEV   "/dev/gpio1"
#define GPIO_INT_DEV   "/dev/gpio2"
#define IMU_DEV        "/dev/lsm6dsl0"
#define RTC_DEV        "/dev/rtc0"
#define AUDIO_DEV      "/dev/audio/audio0"   /* 注意：带 audio/ 子目录 */

#define TOUCH_MAX_POINTS   16    /* FT6146 注册上限，见 touch_register(...,16) */
#define TOUCH_SAMPLES_WANT 20    /* 读满这么多点就提前结束 */
#define TOUCH_DEFAULT_SEC  10    /* 默认观察时长 */
#define TOUCH_POLL_MS      200
#define BTN_TIMEOUT_MS     5000  /* 没人按键时的等待上限 */
#define BTN_POLL_MS        200   /* 主任务查"按键模块报了新事件没"的间隔 */
#define LCD_BAND_ROWS      60    /* 刷色时每次写多少行（与 LVGL 分块缓冲同量级） */

#define RGB565_RED         0xf800
#define RGB565_GREEN       0x07e0
#define RGB565_BLUE        0x001f
#define RGB565_WHITE       0xffff
#define RGB565_BLACK       0x0000

/* imu 子命令 */

#define IMU_DEFAULT_FRAMES   10    /* 默认打印帧数 */
#define IMU_FRAME_MS         100   /* 帧间隔（ms）；10 帧约 1 秒 */

/* rtc 子命令 */

#define RTC_DEFAULT_ALARM_SEC 3    /* 默认 alarm 延时（秒） */
#define RTC_WAIT_SLACK_SEC    2    /* 等 alarm 的额外宽限；超时就 FAIL 退出 */

/* audio 子命令（参数与板级封装 sf32lb52_audio_in 的约定一致：16k 单声道 16bit） */

#define AUDIO_SAMPLE_RATE     16000
#define AUDIO_CHANNELS        1
#define AUDIO_BITS            16
#define AUDIO_DEFAULT_SEC     2    /* 默认录音秒数 */
#define AUDIO_WAIT_SLACK_SEC  2    /* read 没在预期时间内返回就发 STOP 救场 */
#define AUDIO_SOUND_PEAK      500  /* peak 超过它算“检测到声音”（同 audio_test） */
#define AUDIO_READ_TASK_STACK 4096

/* 驱动下层一次 read 只等 5 秒（sf32lb52_audio.c:1130 的 rx_sem 超时），
 * 所以超过 1 秒的录音要拆成 1 秒一块地读（audio_in_read 的约定），
 * 否则 6 秒的录音必然返回 0。
 * 32000 字节 = 16k 单声道 16bit × 1 秒，正是 audio_test 验证过的大小。
 */
#define AUDIO_CHUNK_BYTES     (AUDIO_SAMPLE_RATE * 2)

/* alarm 子命令（设备级报警模块：响喇叭，持续到解除或超时；不驱指示灯） */

#define ALARM_DEFAULT_LEVEL   3     /* 1=NOTICE 2=WARNING 3=EMERGENCY */
#define ALARM_DEFAULT_SEC     3     /* 默认保持报警的秒数 */
#define ALARM_POLL_MS         100   /* 等报警结束的轮询间隔 */
#define ALARM_SETTLE_MS       500   /* 解除后等一小会，让工作线程停音频/灭灯 */

/* lcd 子命令（板级亮度封装 sf32lb52_backlight）。
 * 打了 patches/vendor_sifli-lcd-brightness.patch 之后 0..100 都能 PASS
 * （中间值走面板亮度寄存器）；没打补丁的树只有 0 和 100 能 PASS，
 * 中间值 backlight_set() 会返回 -ENOSYS（本子命令判为 FAIL）。 */

#define BACKLIGHT_DEFAULT_PCT 100   /* 不带参数时设成"全亮" */
#define BACKLIGHT_SETTLE_MS   200   /* 下发后等一小会，再回读 */
#define BACKLIGHT_POLL_MS     50

/* rtcday 子命令（RTC 每日定时提醒模块；只有一个 alarm 槽） */

#define RTCDAY_MAX_WAIT_MS    (90 * 1000)  /* 等回调的上限，绝不永久卡住 */
#define RTCDAY_POLL_MS        200          /* 等回调的轮询间隔 */

/* button 子命令：真的等一次按下，超时算 FAIL */

#define BTN_DEFAULT_WAIT_SEC  15    /* 默认等待秒数 */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct dev_node_s
{
  FAR const char *path;       /* 设备节点 */
  FAR const char *desc;       /* 用途 */
  FAR const char *optional;   /* 非 NULL：本配置下可能不注册，缺失不算故障 */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static int g_pass;
static int g_total;

/* rtc 子命令：alarm 到点后由信号处理函数置位 */

static volatile sig_atomic_t g_rtc_alarm;

/* audio 子命令：阻塞的 read() 放在独立任务里，主任务带超时等它。
 * fd 由板级封装 sf32lb52_audio_in 自己持有，这里不再存一份。 */

static volatile int     g_arec_done;
static volatile ssize_t g_arec_n;
static FAR int16_t     *g_arec_buf;
static int              g_arec_len;

/* alarm 子命令：模块工作线程里回调，这里只累计事件给主任务打印 */

static volatile int g_alarm_events;
static volatile int g_alarm_last_level;

/* rtcday 子命令：模块工作线程里回调，这里只置标志给主任务打印 */

static volatile sig_atomic_t g_rtcday_fired;
static volatile int          g_rtcday_calls;

/* 按键（button 子命令 + 默认自检第 4 步）：回调跑在板级按键模块的轮询
 * 任务里，只能写 volatile 变量；主任务靠 g_btn_ev_seq 发现"有新事件"，
 * 再把最近一次事件的字段打印出来（中间的事件可能被合并，自检够用）。 */

static volatile int      g_btn_ev_seq;
static volatile enum board_btn_e       g_btn_ev_btn;
static volatile enum board_btn_event_e g_btn_ev_type;
static volatile uint32_t g_btn_ev_held;
static volatile int      g_btn_press_seen;
static volatile int      g_btn_lp_seen;
static volatile enum board_btn_e g_btn_press_which;
static volatile uint32_t g_btn_press_held;

/* 与本板硬件相关的设备节点（枚举顺序 = 打印顺序）。
 * optional 非 NULL 的条目：本配置下就是不注册，缺失**不**计入"节点缺失"。 */

static const struct dev_node_s g_nodes[] =
{
  { "/dev/lcd0",         "AMOLED (CO5300), NuttX LCD dev", NULL },
  { "/dev/fb0",          "framebuffer (LCD 的 fb 前端)",   NULL },
  { "/dev/input0",       "touchscreen (FT6146)",           NULL },
  { "/dev/buttons",      "buttons upper half (Key2 = PA11) —— 节点还在，"
                         "但 poll/read 会卡死整机，按键别用它",  NULL },
  { "/dev/gpio0",        "GPIO input  (PA34, Key1/power key)", NULL },
  { "/dev/gpio1",        "GPIO output (PA26, 板级用户输出)",   NULL },
  { "/dev/gpio2",        "GPIO interrupt (PA34)",          NULL },
  { "/dev/timer0",       "timer",                          NULL },
  /* /dev/pwm0 由 sifli_ap.c 在 #ifdef CONFIG_PWM 里注册。本配置
   * CONFIG_PWM=n，节点必然不存在 —— 这是配置选择，不是故障。 */
#if defined(CONFIG_PWM)
  { "/dev/pwm0",         "pwm (背光 PA01 = GPTIM1_CH4)",   NULL },
#else
  { "/dev/pwm0",         "pwm (背光 PA01 = GPTIM1_CH4)",   "本配置未启用（CONFIG_PWM=n），不是故障" },
#endif
  { "/dev/i2c0",         "i2c master (触摸 FT6146: SCL=PA30 SDA=PA33)", NULL },
  { "/dev/i2c1",         "i2c master (I2C2)",              NULL },
  { "/dev/adc0",         "adc",                            NULL },
  { "/dev/audio/audio0", "audio (NS4150B 功放 + MEMS MIC)", NULL },
  /* 注意：网络接口**不是** /dev 节点。NuttX 的网卡是 netdev，
   * 走 socket + ioctl(SIOCGIF*) 访问，标准做法见下面 netdev_probe()。
   * 之前这里写成 "/dev/eth0" 是错的；eth0 在不在取决于 USB RNDIS
   * 有没有枚举起来，所以那条结论也不计入 PASS/FAIL。 */
};

#define NNODES ((int)(sizeof(g_nodes) / sizeof(g_nodes[0])))

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t mono_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void report(FAR const char *name, int ok, FAR const char *detail)
{
  g_total++;
  if (ok)
    {
      g_pass++;
    }

  printf("      [%s] %s", ok ? "PASS" : "FAIL", name);
  if (detail != NULL && detail[0] != '\0')
    {
      printf("  (%s)", detail);
    }

  printf("\n");
}

static void usage(void)
{
  printf("用法:\n");
  printf("  hw_test              只读自检（不动屏幕、不拉 GPIO 电平）\n");
  printf("  hw_test touch <秒>    指定触摸观察时长，0 = 跳过\n");
  printf("  hw_test lcdcolor     额外刷色测试（会改屏，退出前清屏）\n");
  printf("  hw_test gpio         额外翻转一次 /dev/gpio1 (PA26)\n");
  printf("  hw_test imu [帧数]   IMU(LSM6DS3) 加速度/陀螺，默认 10 帧（单独运行）\n");
  printf("  hw_test rtc [秒]     RTC 时间 + N 秒后的 alarm，默认 3 秒（单独运行）\n");
  printf("  hw_test rtcday [时] [分]  每日定时提醒，默认当前时间 + 1 分钟，"
         "最多等 90 秒（单独运行）\n");
  printf("  hw_test audio [秒]   录音电平 peak/avg，默认 2 秒（单独运行）\n");
  printf("  hw_test alarm [级别] [秒]  报警：1=提示 2=警告 3=紧急(默认)，"
         "持续秒数默认 3（单独运行）\n");
  printf("  hw_test lcd [0..100]  设屏幕亮度并回读，默认 100（单独运行）；"
         "中间值走面板亮度寄存器（需 vendor 补丁，见 patches/README.md）\n");
  printf("  hw_test button [秒]  等按键按下（板级 GPIO：PA11=KEY / PA34=HOME），"
         "默认 15 秒，超时算 FAIL（单独运行）\n");
  printf("  hw_test status       打印统一外设状态（board_status_get/dump）；"
         "网络没拿到 IPv4 地址算 FAIL，其它设备只提示（单独运行）\n");
}

/****************************************************************************
 * Name: node_exists
 ****************************************************************************/

static int node_exists(FAR const char *path)
{
  struct stat st;

  return stat(path, &st) == 0;
}

/****************************************************************************
 * Name: netdev_probe
 *
 * Description:
 *   用标准 netdev API 查一个网络接口的 IP / 网关。
 *   NuttX 的网卡是 netdev，不是 /dev 节点，必须用 socket + ioctl(SIOCGIF*)
 *   访问（这也是标准 openvela/NuttX 的用法）。
 *
 *   返回 0 表示接口已配置好，-1 表示没有这个接口或没配 IP。
 *   注意：这条**不计入 PASS/FAIL**——eth0 存不存在取决于 USB RNDIS
 *   有没有在主机侧枚举起来，没插 USB 时接口不存在是正常的，不是故障。
 *
 ****************************************************************************/

static int netdev_probe(FAR const char *ifname)
{
  struct ifreq ifr;
  struct sockaddr_in *sin;
  int fd;
  int ret = -1;

  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    {
      printf("      %-8s --    创建 socket 失败: %d\n", ifname, errno);
      return -1;
    }

  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

  if (ioctl(fd, SIOCGIFADDR, (unsigned long)&ifr) < 0)
    {
      printf("      %-8s --    接口未配置（USB RNDIS 未枚举，不是故障）\n",
             ifname);
      close(fd);
      return -1;
    }

  sin = (struct sockaddr_in *)&ifr.ifr_addr;
  printf("      %-8s OK    IP = %s", ifname, inet_ntoa(sin->sin_addr));
  ret = 0;

  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
  if (ioctl(fd, SIOCGIFDSTADDR, (unsigned long)&ifr) == 0)
    {
      sin = (struct sockaddr_in *)&ifr.ifr_dstaddr;
      printf("  网关 = %s", inet_ntoa(sin->sin_addr));
    }

  printf("\n");
  close(fd);
  return ret;
}

/****************************************************************************
 * Name: step_nodes
 *
 * Description:
 *   第 1 步：枚举与本板硬件相关的设备节点，打印存在性。
 *   网络接口用 netdev API 单独查（它不是 /dev 节点）。
 *
 *   optional 条目（如 CONFIG_PWM=n 时的 /dev/pwm0）缺失是正常配置差异，
 *   不计入"节点缺失"，打印时也会明确标出。
 *
 ****************************************************************************/

static int step_nodes(void)
{
  int missing = 0;
  int i;

  printf("[1/6] 设备节点枚举\n");

  for (i = 0; i < NNODES; i++)
    {
      int ok = node_exists(g_nodes[i].path);

      if (!ok && g_nodes[i].optional == NULL)
        {
          missing++;
        }

      printf("      %-18s %-3s  %s", g_nodes[i].path, ok ? "OK" : "--",
             g_nodes[i].desc);

      if (!ok && g_nodes[i].optional != NULL)
        {
          printf("（%s）", g_nodes[i].optional);
        }

      printf("\n");
    }

  /* 网卡不是 /dev 节点，单独用 netdev API 查（不计入 PASS/FAIL） */
  netdev_probe("eth0");

  if (missing == 0)
    {
      report("必需节点存在", 1, NULL);
    }
  else
    {
      char detail[32];

      snprintf(detail, sizeof(detail), "%d 个节点缺失", missing);
      report("必需节点存在", 0, detail);
    }

  return missing == 0 ? OK : -1;
}

/****************************************************************************
 * Name: print_touch_point
 ****************************************************************************/

static void print_touch_point(int idx, FAR const struct touch_point_s *pt)
{
  printf("      #%-2d id=%d flags=0x%02x x=%d y=%d h=%d%s%s%s\n",
         idx, pt->id, pt->flags, pt->x, pt->y, pt->h,
         (pt->flags & TOUCH_DOWN) != 0 ? " DOWN" : "",
         (pt->flags & TOUCH_MOVE) != 0 ? " MOVE" : "",
         (pt->flags & TOUCH_UP) != 0 ? " UP" : "");
}

/****************************************************************************
 * Name: step_touch
 *
 * Description:
 *   第 2 步：非阻塞读 /dev/input0，轮询 seconds 秒或读满 TOUCH_SAMPLES_WANT 个
 *   样点就退出。没摸屏幕不算失败（没人碰而已），打印提示后正常结束。
 *
 ****************************************************************************/

static int step_touch(int seconds)
{
  uint8_t buf[sizeof(struct touch_sample_s) +
              TOUCH_MAX_POINTS * sizeof(struct touch_point_s)];
  uint32_t t0;
  int samples = 0;
  int fd;

  printf("[2/6] 触摸 %s\n", INPUT_DEV);

  if (seconds <= 0)
    {
      report("触摸观察", 1, "已跳过");
      return 0;
    }

  fd = open(INPUT_DEV, O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    {
      printf("      open 失败: %d\n", errno);
      report("打开触摸设备", 0, "open /dev/input0 失败");
      return -1;
    }

  printf("      观察 %d 秒（最多 %d 个样点），请用手指点一下屏幕...\n",
         seconds, TOUCH_SAMPLES_WANT);

  t0 = mono_ms();
  while (samples < TOUCH_SAMPLES_WANT)
    {
      struct pollfd pfd;
      ssize_t n;
      int ret;
      int i;

      if ((int)(mono_ms() - t0) >= seconds * 1000)
        {
          break;
        }

      pfd.fd      = fd;
      pfd.events  = POLLIN;
      pfd.revents = 0;

      ret = poll(&pfd, 1, TOUCH_POLL_MS);
      if (ret < 0)
        {
          usleep(TOUCH_POLL_MS * 1000);   /* poll 不可用时退化成定时轮询 */
        }
      else if (ret == 0)
        {
          continue;
        }

      memset(buf, 0, sizeof(buf));
      n = read(fd, buf, sizeof(buf));
      if (n < (ssize_t)sizeof(struct touch_sample_s))
        {
          /* EAGAIN / 被别的 reader 抢走：继续等 */
          continue;
        }

      {
        FAR struct touch_sample_s *sample = (FAR struct touch_sample_s *)buf;
        int npoints = sample->npoints;

        if (npoints > TOUCH_MAX_POINTS)
          {
            npoints = TOUCH_MAX_POINTS;
          }

        for (i = 0; i < npoints && samples < TOUCH_SAMPLES_WANT; i++)
          {
            print_touch_point(samples + 1, &sample->point[i]);
            samples++;
          }
      }
    }

  close(fd);

  if (samples == 0)
    {
      printf("      未检测到触摸，请用手指点一下屏幕\n");
      report("触摸读样点", 1, "0 个样点（这不算失败）");
    }
  else
    {
      char detail[32];

      snprintf(detail, sizeof(detail), "%d 个样点", samples);
      report("触摸读样点", 1, detail);
    }

  return 0;
}

/****************************************************************************
 * Name: lcd_fill
 *
 * Description:
 *   用 LCDDEVIO_PUTAREA 把 [row0, row1] 行刷成一个颜色（RGB565）。
 *   每次只提交 LCD_BAND_ROWS 行，避免一次 malloc 整屏 351KB。
 *
 ****************************************************************************/

static int lcd_fill(int fd, uint8_t fmt, uint16_t xres, uint16_t yres,
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

      /* RGB565 小端：低字节在前 */

      for (i = 0; i < npixel; i++)
        {
          buf[i * 2]     = (uint8_t)(color & 0xff);
          buf[i * 2 + 1] = (uint8_t)(color >> 8);
        }

      memset(&area, 0, sizeof(area));
      area.row_start = (uint16_t)row;
      area.row_end   = (uint16_t)(row + rows - 1);
      area.col_start = 0;
      area.col_end   = (uint16_t)(xres - 1);
      area.stride    = (uint32_t)xres * 2;
      area.data      = buf;

      /* 注意：struct lcddev_area_s 里**没有** fmt 字段
       * （只有 row_start/row_end/col_start/col_end/stride/data），
       * 像素格式由驱动自己的 bpp 决定，所以这里不需要传 fmt。 */
      (void)fmt;

      ret = ioctl(fd, LCDDEVIO_PUTAREA, (unsigned long)&area);
      if (ret < 0)
        {
          printf("      PUTAREA(row %u..%u) 失败: %d\n",
                 (unsigned)row, (unsigned)(row + rows - 1), ret);
          return ret;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: lcd_color_test
 *
 * Description:
 *   刷色测试：4 条横向色带（红/绿/蓝/白）-> 停一下 -> 清屏成纯黑。
 *   本程序不启动 LVGL，退出前只保证"清屏"，界面恢复靠重启或重跑 robot_ui。
 *
 ****************************************************************************/

static int lcd_color_test(int fd, FAR const struct fb_videoinfo_s *vinfo)
{
  uint16_t xres = vinfo->xres;
  uint16_t yres = vinfo->yres;
  uint16_t q    = (uint16_t)(yres / 4);
  FAR uint8_t *buf;
  size_t bufsize;
  int ret = OK;

  if (xres == 0 || yres < 4)
    {
      printf("      分辨率异常(%dx%d)，跳过刷色\n", xres, yres);
      return -1;
    }

  bufsize = (size_t)xres * LCD_BAND_ROWS * 2;
  buf = (FAR uint8_t *)malloc(bufsize);
  if (buf == NULL)
    {
      printf("      malloc %u 字节失败，跳过刷色\n", (unsigned)bufsize);
      return -ENOMEM;
    }

  printf("      刷 4 条横向色带：红/绿/蓝/白（每次 %d 行）\n",
         LCD_BAND_ROWS);

  if (lcd_fill(fd, vinfo->fmt, xres, yres, 0,
               q, RGB565_RED, buf, bufsize) < 0 ||
      lcd_fill(fd, vinfo->fmt, xres, yres, q,
               (uint16_t)(2 * q), RGB565_GREEN, buf, bufsize) < 0 ||
      lcd_fill(fd, vinfo->fmt, xres, yres, (uint16_t)(2 * q),
               (uint16_t)(3 * q), RGB565_BLUE, buf, bufsize) < 0 ||
      lcd_fill(fd, vinfo->fmt, xres, yres, (uint16_t)(3 * q),
               yres, RGB565_WHITE, buf, bufsize) < 0)
    {
      ret = -1;
    }

  sleep(1);

  printf("      清屏（纯黑）...\n");
  if (lcd_fill(fd, vinfo->fmt, xres, yres, 0, yres,
               RGB565_BLACK, buf, bufsize) < 0)
    {
      ret = -1;
    }

  free(buf);

  printf("      注意：本程序不启动 LVGL；屏幕已被直接改写。\n");
  printf("      要恢复界面请复位板子，或重新跑 robot_ui。\n");
  return ret;
}

/****************************************************************************
 * Name: step_lcd
 *
 * Description:
 *   第 3 步：读 /dev/lcd0 的显示信息（分辨率/格式/对齐要求）；
 *   带 lcdcolor 参数时再做一次刷色测试。
 *
 ****************************************************************************/

static int step_lcd(int do_color)
{
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  struct lcddev_area_align_s align;
  int ok = 1;
  int fd;
  int ret;

  printf("[3/6] 显示 %s\n", LCD_DEV);

  fd = open(LCD_DEV, O_RDWR);
  if (fd < 0)
    {
      printf("      open 失败: %d\n", errno);
      report("打开显示设备", 0, "open /dev/lcd0 失败");
      return -1;
    }

  /* 显示信息：分辨率 / 像素格式 / 平面数 */

  memset(&vinfo, 0, sizeof(vinfo));
  ret = ioctl(fd, LCDDEVIO_GETVIDEOINFO, (unsigned long)&vinfo);
  if (ret < 0)
    {
      printf("      GETVIDEOINFO 失败: %d\n", ret);
      report("GETVIDEOINFO", 0, "ioctl 失败");
      ok = 0;
    }
  else
    {
      printf("      分辨率   : %dx%d, fmt=%d, planes=%d\n",
             vinfo.xres, vinfo.yres, vinfo.fmt, vinfo.nplanes);
      report("GETVIDEOINFO", 1, NULL);
    }

  /* 当前平面信息：framebuffer 指针 / 行跨距 / 位深 */

  memset(&pinfo, 0, sizeof(pinfo));
  ret = ioctl(fd, LCDDEVIO_GETPLANEINFO, (unsigned long)&pinfo);
  if (ret < 0)
    {
      printf("      GETPLANEINFO 失败: %d\n", ret);
      report("GETPLANEINFO", 0, "ioctl 失败");
      ok = 0;
    }
  else
    {
      printf("      平面     : bpp=%d stride=%u fblen=%u fbmem=%p\n",
             pinfo.bpp, (unsigned)pinfo.stride,
             (unsigned)pinfo.fblen, pinfo.fbmem);
      report("GETPLANEINFO", 1, NULL);
    }

  /* 区域对齐要求（PUTAREA 的 row/col/buf 对齐） */

  memset(&align, 0, sizeof(align));
  ret = ioctl(fd, LCDDEVIO_GETAREAALIGN, (unsigned long)&align);
  if (ret < 0)
    {
      printf("      GETAREAALIGN 失败: %d\n", ret);
      report("GETAREAALIGN", 0, "ioctl 失败");
      ok = 0;
    }
  else
    {
      printf("      对齐要求 : row_start_align=%u height_align=%u "
             "width_align=%u buf_align=%u\n",
             align.row_start_align, align.height_align,
             align.width_align, align.buf_align);
      report("GETAREAALIGN", 1, NULL);
    }

  if (do_color)
    {
      if (vinfo.xres == 0 || vinfo.yres == 0)
        {
          report("刷色测试", 0, "拿不到分辨率");
          ok = 0;
        }
      else if (lcd_color_test(fd, &vinfo) < 0)
        {
          report("刷色测试", 0, "PUTAREA 失败");
          ok = 0;
        }
      else
        {
          report("刷色测试", 1, "已清屏");
        }
    }

  close(fd);
  return ok ? OK : -1;
}

/****************************************************************************
 * Name: hw_test_btn_reset
 *
 * Description:
 *   把按键模块的"事件信箱"清空（两个按键用例在注册回调之前各调一次）。
 *
 ****************************************************************************/

static void hw_test_btn_reset(void)
{
  g_btn_ev_seq      = 0;
  g_btn_ev_btn      = BOARD_BTN_KEY;
  g_btn_ev_type     = BOARD_BTN_PRESS;
  g_btn_ev_held     = 0;
  g_btn_press_seen  = 0;
  g_btn_lp_seen     = 0;
  g_btn_press_which = BOARD_BTN_KEY;
  g_btn_press_held  = 0;
}

/****************************************************************************
 * Name: hw_test_btn_cb
 *
 * Description:
 *   按键事件回调。**跑在板级按键模块的轮询任务里**（任务的优先级 115），
 *   所以这里只写几个 volatile 变量：不 printf、不 sleep、不碰 LVGL。
 *   打印留给主任务 —— 下面两个函数靠 g_btn_ev_seq 发现"有新事件"。
 *
 ****************************************************************************/

static void hw_test_btn_cb(enum board_btn_e btn, enum board_btn_event_e ev,
                           uint32_t held_ms, FAR void *arg)
{
  (void)arg;

  g_btn_ev_btn  = btn;
  g_btn_ev_type = ev;
  g_btn_ev_held = held_ms;

  if (ev == BOARD_BTN_PRESS)
    {
      g_btn_press_seen  = 1;
      g_btn_press_which = btn;
    }
  else if (ev == BOARD_BTN_LONGPRESS)
    {
      g_btn_lp_seen = 1;
    }
  else if (ev == BOARD_BTN_RELEASE)
    {
      g_btn_press_held = held_ms;
    }

  g_btn_ev_seq++;
}

/****************************************************************************
 * Name: step_buttons
 *
 * Description:
 *   第 4 步：看板级按键模块（PA11=KEY / PA34=HOME）的事件，按了就打印
 *   事件类型和按住时长；等到超时正常结束 —— 没人按也算 PASS
 *   （这一步只证明按键**能读**，不能证明它没坏）。
 *
 *   以前这里读的是 /dev/buttons（poll + read），那个路径在真机上会让整机
 *   静默卡死；现在走 sf32lb52_boardbtn 的 GPIO 轮询，见它的头文件说明。
 *
 ****************************************************************************/

static int step_buttons(int timeout_ms)
{
  uint32_t t0;
  int seen = 0;
  int events;
  int ret;

  printf("[4/6] 按键 %s / %s（板级 GPIO，不走 /dev/buttons）\n",
         board_btn_name(BOARD_BTN_KEY), board_btn_name(BOARD_BTN_HOME));

  ret = board_btn_init();
  if (ret < 0)
    {
      printf("      board_btn_init 失败: %d\n", ret);
      report("按键读取", 0, "board_btn_init 失败（起不了轮询任务）");
      return -1;
    }

  hw_test_btn_reset();
  if (board_btn_set_callback(hw_test_btn_cb, NULL) < 0)
    {
      report("按键读取", 0, "board_btn_set_callback 失败");
      return -1;
    }

  printf("      最多等 %d 秒，按一下 %s 就能看到事件（没人按也算 PASS）\n",
         timeout_ms / 1000, board_btn_name(BOARD_BTN_KEY));

  t0 = mono_ms();
  while ((int)(mono_ms() - t0) < timeout_ms)
    {
      if (seen < g_btn_ev_seq)
        {
          seen = g_btn_ev_seq;
          printf("      事件 %d: %s %s  held=%u ms\n",
                 seen, board_btn_name(g_btn_ev_btn),
                 board_btn_event_name(g_btn_ev_type),
                 (unsigned)g_btn_ev_held);
        }

      usleep(BTN_POLL_MS * 1000);
    }

  events = g_btn_ev_seq;
  board_btn_set_callback(NULL, NULL);

  if (events == 0)
    {
      report("按键读取", 1, "超时未按键（这不算失败）");
    }
  else
    {
      char detail[32];

      snprintf(detail, sizeof(detail), "%d 个事件", events);
      report("按键读取", 1, detail);
    }

  return 0;
}

/****************************************************************************
 * Name: step_button_wait
 *
 * Description:
 *   button 子命令：真的等一次按键按下（默认 15 秒）。
 *
 *   与默认自检里的 step_buttons() 不同：这里"没按到"是**失败**。
 *   默认自检那一步只证明"按键能读"，所以没人按也算 PASS；要验证按键
 *   本身必须用本命令，按到才 PASS。
 *
 *   按下由板级按键模块的回调通知（回调跑在按键任务上下文里，只置标志），
 *   主任务每 BTN_POLL_MS 醒一次看有没有新事件，所以这里**不碰任何设备节点**。
 *
 ****************************************************************************/

static int step_button_wait(int seconds)
{
  uint32_t t0;
  int seen = 0;
  int ret;

  if (seconds < 1)
    {
      seconds = BTN_DEFAULT_WAIT_SEC;
    }

  printf("[BUTTON] 板级 GPIO 按键（%s / %s）等一次按下（最多 %d 秒）\n",
         board_btn_name(BOARD_BTN_KEY), board_btn_name(BOARD_BTN_HOME),
         seconds);

  ret = board_btn_init();
  if (ret < 0)
    {
      printf("      board_btn_init 失败: %d\n", ret);
      report("按键", 0, "board_btn_init 失败（起不了轮询任务）");
      return -1;
    }

  hw_test_btn_reset();
  if (board_btn_set_callback(hw_test_btn_cb, NULL) < 0)
    {
      report("按键", 0, "board_btn_set_callback 失败");
      return -1;
    }

  printf("      请按一下 %s 或 %s ...\n",
         board_btn_name(BOARD_BTN_KEY), board_btn_name(BOARD_BTN_HOME));

  t0 = mono_ms();

  while ((int)(mono_ms() - t0) < seconds * 1000)
    {
      if (seen < g_btn_ev_seq)
        {
          seen = g_btn_ev_seq;
          printf("      事件 %d: %s %s  held=%u ms\n",
                 seen, board_btn_name(g_btn_ev_btn),
                 board_btn_event_name(g_btn_ev_type),
                 (unsigned)g_btn_ev_held);
        }

      if (g_btn_press_seen)
        {
          char detail[96];

          snprintf(detail, sizeof(detail), "%s 按下（%d ms 后检测到%s）",
                   board_btn_name(g_btn_press_which), (int)(mono_ms() - t0),
                   g_btn_lp_seen ? "，含长按" : "");
          report("按键", 1, detail);

          board_btn_set_callback(NULL, NULL);
          return OK;
        }

      usleep(BTN_POLL_MS * 1000);
    }

  board_btn_set_callback(NULL, NULL);

  printf("      等了 %d ms 没检测到按下（两个脚一直是松开）\n",
         (int)(mono_ms() - t0));
  report("按键", 0, "超时，没检测到按下");
  return -1;
}

/****************************************************************************
 * Name: gpio_pintype
 *
 * Description:
 *   读 GPIOIOC_GETPINTYPE，失败返回 -1。宏名随 NuttX 版本略有不同，
 *   这里做兼容判断。
 *
 ****************************************************************************/

static int gpio_pintype(int fd)
{
#if defined(GPIOIOC_GETPINTYPE)
  enum gpio_pintype_e pintype = (enum gpio_pintype_e)-1;

  if (ioctl(fd, GPIOIOC_GETPINTYPE, (unsigned long)&pintype) < 0)
    {
      return -1;
    }

  return (int)pintype;
#else
  /* 旧内核只有 GPIOIOC_CONFIG/GET/SET，没有 pintype 查询 */

  return -1;
#endif
}

static FAR const char *gpio_pintype_name(int pintype)
{
  switch (pintype)
    {
      case GPIO_INPUT_PIN:
        return "input";
      case GPIO_OUTPUT_PIN:
        return "output";
      case GPIO_INTERRUPT_BOTH_PIN:
        return "interrupt-both";
      default:
        return "unknown";
    }
}

static int gpio_read_value(int fd, FAR bool *value)
{
  return read(fd, value, 1) == 1 ? OK : -1;
}

/****************************************************************************
 * Name: step_gpio
 *
 * Description:
 *   第 5 步：枚举 /dev/gpio0..2，打印引脚类型和当前电平（只读）。
 *   带 gpio 参数时对 /dev/gpio1（PA26，板级输出脚）做一次电平翻转。
 *
 ****************************************************************************/

static int step_gpio(int do_toggle)
{
  static FAR const char *const paths[3] =
  {
    GPIO_IN_DEV, GPIO_OUT_DEV, GPIO_INT_DEV
  };
  int ok = 1;
  int i;

  printf("[5/6] GPIO %s %s %s\n", GPIO_IN_DEV, GPIO_OUT_DEV, GPIO_INT_DEV);

  for (i = 0; i < 3; i++)
    {
      bool value = false;
      int pintype;
      int fd;

      fd = open(paths[i], O_RDONLY);
      if (fd < 0)
        {
          printf("      %-12s open 失败: %d\n", paths[i], errno);
          ok = 0;
          continue;
        }

      pintype = gpio_pintype(fd);
      if (gpio_read_value(fd, &value) == OK)
        {
          printf("      %-12s pintype=%s(%d) value=%d\n", paths[i],
                 gpio_pintype_name(pintype), pintype, (int)value);
        }
      else
        {
          printf("      %-12s pintype=%s(%d) value=读取失败\n", paths[i],
                 gpio_pintype_name(pintype), pintype);
          ok = 0;
        }

      close(fd);
    }

  if (ok)
    {
      report("GPIO 只读检查", 1, NULL);
    }
  else
    {
      report("GPIO 只读检查", 0, "见上面的失败项");
    }

  if (!do_toggle)
    {
      printf("      提示：要测输出脚电平翻转请跑 `hw_test gpio`\n");
      return ok ? OK : -1;
    }

  /* 输出脚电平翻转：只碰 /dev/gpio1（PA26，板级唯一的 GPIO 输出脚） */

  {
    bool value = false;
    int fd = open(GPIO_OUT_DEV, O_RDWR);

    if (fd < 0)
      {
        printf("      %s open 失败: %d\n", GPIO_OUT_DEV, errno);
        report("GPIO 输出翻转", 0, "open 失败");
        return -1;
      }

#if defined(GPIOIOC_SETPINTYPE)
    if (ioctl(fd, GPIOIOC_SETPINTYPE, (unsigned long)GPIO_OUTPUT_PIN) < 0)
      {
        printf("      SETPINTYPE(output) 失败（板级 bringup 已配成输出，继续）\n");
      }
#elif defined(GPIOIOC_CONFIG)
    if (ioctl(fd, GPIOIOC_CONFIG, (unsigned long)GPIO_OUTPUT_PIN) < 0)
      {
        printf("      CONFIG(output) 失败（板级 bringup 已配成输出，继续）\n");
      }
#endif

    value = true;
    if (write(fd, &value, 1) != 1)
      {
        printf("      写 1 失败: %d\n", errno);
        close(fd);
        report("GPIO 输出翻转", 0, "write 失败");
        return -1;
      }

    if (gpio_read_value(fd, &value) == OK)
      {
        printf("      %s 置 1 -> 回读 %d\n", GPIO_OUT_DEV, (int)value);
      }
    else
      {
        printf("      %s 置 1（回读失败）\n", GPIO_OUT_DEV);
      }

    value = false;
    if (write(fd, &value, 1) != 1)
      {
        printf("      写 0 失败: %d，电平可能停在 1\n", errno);
        close(fd);
        report("GPIO 输出翻转", 0, "写 0 失败");
        return -1;
      }

    if (gpio_read_value(fd, &value) == OK)
      {
        printf("      %s 置 0 -> 回读 %d\n", GPIO_OUT_DEV, (int)value);
      }
    else
      {
        printf("      %s 置 0（回读失败）\n", GPIO_OUT_DEV);
      }

    close(fd);
    report("GPIO 输出翻转", 1, "PA26 已回到 0");
  }

  return OK;
}

/****************************************************************************
 * Name: step_imu
 *
 * Description:
 *   imu 子命令：读 LSM6DS3 的加速度 / 陀螺 / 温度。
 *
 *   本板在树里的 LSM6DSL 驱动是 **NuttX 老式字符驱动**
 *   （nuttx/drivers/sensors/lsm6dsl.c，走 register_driver(devpath, ...)），
 *   注册出来的节点就是 "/dev/lsm6dsl0"，接口是：
 *     ioctl(fd, SNIOC_START)                 -- 开始转换（写 CTRL1_XL/CTRL2_G）
 *     ioctl(fd, SNIOC_LSM6DSLSENSORREAD, &s) -- 一次拿到 acc/gyro/temp/timestamp
 *     read(fd, buf, len)                     -- 只要 acc，len/6 个 int16 xyz 原始码
 *     ioctl(fd, SNIOC_STOP)                  -- 停转换
 *   它**不是** uORB 传感器（没有 /dev/uorb/sensor_accel0），
 *   所以这里没有用 orb_subscribe()。详见 docs/sensor_rtc_usage.md。
 *
 ****************************************************************************/

static int step_imu(int frames)
{
  printf("[IMU] LSM6DS3 %s\n", IMU_DEV);

#ifndef HW_TEST_HAS_IMU
  (void)frames;
  printf("      本板**没有加速度计/陀螺**：模组 SF32LB52-MOD-1 的 BOM 里\n");
  printf("      只有 MCU + 128Mb NOR Flash + 晶振 + 天线，没有任何 IMU 器件。\n");
  printf("      所以这不是\"驱动没编\"，是硬件不存在，开了也读不到。\n");
  printf("      /dev/lsm6dsl0 不会出现；详见 docs/sensor_rtc_usage.md。\n");
  report("IMU 读数", 0, "本板无 IMU 硬件");
  return -1;
#else
  struct lsm6dsl_sensor_data_s sdata;
  int got = 0;
  int fd;
  int i;

  if (frames <= 0)
    {
      frames = IMU_DEFAULT_FRAMES;
    }

  fd = open(IMU_DEV, O_RDONLY);
  if (fd < 0)
    {
      printf("      open 失败: %d\n", errno);
      report("IMU 读数", 0, "open /dev/lsm6dsl0 失败");
      return -1;
    }

  /* 注册时驱动只做了 WHO_AM_I 校验；不 START 就是 POWER_DOWN，读出来全是 0 */

  if (ioctl(fd, SNIOC_START, 0) < 0)
    {
      printf("      SNIOC_START 失败: %d（IMU 没焊 / 地址不对？）\n", errno);
      close(fd);
      report("IMU 读数", 0, "SNIOC_START 失败");
      return -1;
    }

  for (i = 0; i < frames; i++)
    {
      memset(&sdata, 0, sizeof(sdata));

      if (ioctl(fd, SNIOC_LSM6DSLSENSORREAD, (unsigned long)&sdata) < 0)
        {
          printf("      第 %d 帧读取失败: %d\n", i + 1, errno);
          break;
        }

      /* 单位：acc 是 mg（驱动内部已按 ±16g 的 0.488 mg/LSB 换算好），
       *       gyro 是 mdps（±2000dps 的 70 mdps/LSB），
       *       temp 是摄氏度，timestamp 是传感器自己的计数器（不是毫秒）。
       */

      printf("      #%-2d acc=(%5d,%5d,%5d) mg  gyro=(%6d,%6d,%6d) mdps  "
             "temp=%d C  ts=%u\n",
             i + 1,
             (int)sdata.x_data, (int)sdata.y_data, (int)sdata.z_data,
             (int)sdata.g_x_data, (int)sdata.g_y_data, (int)sdata.g_z_data,
             (int)sdata.temperature, (unsigned)sdata.timestamp);
      got++;
      usleep(IMU_FRAME_MS * 1000);
    }

  ioctl(fd, SNIOC_STOP, 0);
  close(fd);

  if (got == frames)
    {
      char detail[32];

      snprintf(detail, sizeof(detail), "%d 帧", got);
      report("IMU 读数", 1, detail);
      return OK;
    }

  printf("      只读到 %d/%d 帧\n", got, frames);
  report("IMU 读数", 0, "读取中途失败，见上面");
  return -1;
#endif
}

/****************************************************************************
 * Name: rtc_alarm_handler
 *
 * Description:
 *   RTC alarm 到点后，rtc upper half 用 nxsig_notification() 给任务发信号，
 *   这里只置一个标志位，真正的等待在主循环里带超时做。
 *
 ****************************************************************************/

static void rtc_alarm_handler(int signo)
{
  (void)signo;
  g_rtc_alarm = 1;
}

/****************************************************************************
 * Name: step_rtc
 *
 * Description:
 *   rtc 子命令：读当前时间 -> 设一个 N 秒后的 alarm -> 等它触发（带超时）。
 *
 *   关键事实（都从源码核实过，别按 Linux 直觉写）：
 *   - /dev/rtc0 由 board 的 rtc_initialize(0, ...) 建出来
 *     （sifli_ap.c:391 -> nuttx/drivers/timers/rtc.c:853）。
 *   - rtc upper half 的 read() 直接 return 0（EOF），**不会阻塞等 alarm**
 *     （rtc.c:318-321）；而且 g_rtc_fops 里 poll 是 NULL（rtc.c:136）。
 *   - alarm 到点走的是 **信号**：ioctl 参数里带 struct sigevent，
 *     upper half 用 nxsig_notification() 通知 pid（rtc.c:198-199）。
 *
 ****************************************************************************/

static int step_rtc(int alarm_sec)
{
  struct rtc_setrelative_s rel;
  struct rtc_rdalarm_s query;
  struct sigaction sa;
  struct rtc_time rt;
  clock_t t0;
  uint32_t elapsed_ms;
  int fd;

  printf("[RTC] %s\n", RTC_DEV);

  if (alarm_sec < 1)
    {
      alarm_sec = RTC_DEFAULT_ALARM_SEC;
    }

  fd = open(RTC_DEV, O_RDONLY);
  if (fd < 0)
    {
      printf("      open 失败: %d\n", errno);
      report("打开 RTC", 0, "open /dev/rtc0 失败");
      return -1;
    }

  memset(&rt, 0, sizeof(rt));
  if (ioctl(fd, RTC_RD_TIME, (unsigned long)&rt) < 0)
    {
      printf("      RTC_RD_TIME 失败: %d\n", errno);
      report("读 RTC 时间", 0, "RTC_RD_TIME 失败");
      close(fd);
      return -1;
    }

  /* tm_year 是从 1900 起的年数，tm_mon 是 0..11（和 struct tm 完全一样） */

  printf("      当前时间 : %04d-%02d-%02d %02d:%02d:%02d\n",
         rt.tm_year + 1900, rt.tm_mon + 1, rt.tm_mday,
         rt.tm_hour, rt.tm_min, rt.tm_sec);

  if (rt.tm_year < 100)
    {
      printf("      提示：RTC 时间看着没设过（年 %d），"
             "先用 RTC_SET_TIME 或 NSH 的 `date -s` 对时\n",
             rt.tm_year + 1900);
    }

  report("读 RTC 时间", 1, NULL);

  /* 用 SIGUSR1 + 标志位等 alarm；主循环有超时，绝不永久卡住 */

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = rtc_alarm_handler;
  sigemptyset(&sa.sa_mask);

  if (sigaction(SIGUSR1, &sa, NULL) < 0)
    {
      printf("      sigaction(SIGUSR1) 失败: %d\n", errno);
      report("RTC alarm", 0, "装信号处理失败");
      close(fd);
      return -1;
    }

  g_rtc_alarm = 0;

  memset(&rel, 0, sizeof(rel));
  rel.id                 = 0;
  rel.pid                = 0;          /* 0 = 通知调用者自己 */
  rel.event.sigev_notify = SIGEV_SIGNAL;
  rel.event.sigev_signo  = SIGUSR1;
  rel.reltime            = alarm_sec;  /* 相对当前 RTC 时间的秒数 */

  if (ioctl(fd, RTC_SET_RELATIVE, (unsigned long)&rel) < 0)
    {
      printf("      RTC_SET_RELATIVE(%d 秒) 失败: %d\n", alarm_sec, errno);
      report("RTC alarm", 0, "RTC_SET_RELATIVE 失败");
      close(fd);
      return -1;
    }

  printf("      已设 %d 秒后的 alarm，最多等 %d 秒...\n",
         alarm_sec, alarm_sec + RTC_WAIT_SLACK_SEC);

  /* 计时必须用单调时钟。原来这里是"每轮 usleep(50ms) 就把 waited_ms 加 50"，
   * 循环体本身的开销不计入，板子一忙（app 初始化/网络重连）就系统性偏小
   * （实测：设 3 秒报 1600ms、设 5 秒报 3500ms），这种读数会让人误以为
   * "alarm 提前触发了"。 */

  t0 = clock_systime_ticks();
  elapsed_ms = 0;

  while (!g_rtc_alarm &&
         elapsed_ms < (alarm_sec + RTC_WAIT_SLACK_SEC) * 1000)
    {
      usleep(50 * 1000);
      elapsed_ms = (uint32_t)TICK2MSEC(clock_systime_ticks() - t0);
    }

  if (!g_rtc_alarm)
    {
      ioctl(fd, RTC_CANCEL_ALARM, 0);
      printf("      超时：等了 %u ms 没收到 SIGUSR1\n",
             (unsigned)elapsed_ms);
      report("RTC alarm", 0, "超时未收到 alarm");
      close(fd);
      return -1;
    }

  printf("      收到 SIGUSR1：alarm 触发了（%u ms，设定 %d 秒）\n",
         (unsigned)elapsed_ms, alarm_sec);

  /* 注意：RTC_RD_ALARM 只能拿到**时间**字段。HAL_RTC_GetAlarm() 只读 ALRMTR、
   * 不读 ALRMDR（bf0_hal_rtc.c 的 GetAlarm 实现），所以闹钟的日/月/年
   * 永远是结构体里的 0，打出来是 "2000-00-00"。这里只打时间，别误导。 */

  memset(&query, 0, sizeof(query));
  query.id = 0;
  if (ioctl(fd, RTC_RD_ALARM, (unsigned long)&query) == 0)
    {
      printf("      RTC_RD_ALARM: active=%d 时间 %02d:%02d:%02d"
             "（日期字段 HAL 不填，见源码注释）\n",
             (int)query.active,
             query.time.tm_hour, query.time.tm_min, query.time.tm_sec);
    }

  report("RTC alarm", 1, "alarm 已触发");
  close(fd);
  return OK;
}

/****************************************************************************
 * Name: rtcday_cb
 *
 * Description:
 *   rtcday 子命令的回调。**它在每日提醒模块的工作线程里执行**，
 *   所以这里只置标志 + 打印，绝不做 sleep / 等锁之类的阻塞动作，
 *   更不能在这里播 TTS 或碰 LVGL（这是模块头文件里明确写的约束）。
 *
 ****************************************************************************/

static void rtcday_cb(FAR void *arg)
{
  (void)arg;

  g_rtcday_calls++;
  g_rtcday_fired = 1;

  printf("      [RTCDAY] 每日提醒回调触发（模块工作线程上下文）\n");
}

/****************************************************************************
 * Name: step_rtcday
 *
 * Description:
 *   rtcday 子命令：注册一个"每天 hh:mm 到点回调一次"的提醒，然后带超时
 *   地等一次回调（最多 RTCDAY_MAX_WAIT_MS，绝不永久卡住）。不传参数时用
 *   "当前时间 + 1 分钟"，这样不用等到明天也能验。
 *
 *   注意：全板只有 1 个 RTC alarm 槽，所以这和 `hw_test rtc` 不能同时跑；
 *   本命令结束前一定会 rtc_alarm_cancel() 收尾，把槽还回去。
 *
 ****************************************************************************/

static int step_rtcday(int hour, int minute, int time_given)
{
  struct rtc_time now;
  clock_t   t0;
  uint32_t  elapsed_ms;
  int       valid;
  int       ret;

  printf("[RTCDAY] 每日 RTC 提醒 %s\n", RTC_DEV);

  memset(&now, 0, sizeof(now));
  ret = rtc_alarm_now(&now);
  if (ret < 0)
    {
      printf("      读 RTC 时间失败: %d\n", ret);
      report("读 RTC 时间", 0, "rtc_alarm_now 失败");
      return -1;
    }

  printf("      当前时间 : %04d-%02d-%02d %02d:%02d:%02d\n",
         now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
         now.tm_hour, now.tm_min, now.tm_sec);
  report("读 RTC 时间", 1, NULL);

  /* 只打印"有没有对时"，不计 PASS/FAIL：没对时不是本命令的错，
   * 而是现场必须先 `date -s`（NSH 只认 "MMM DD HH:MM:SS YYYY" 格式）。 */

  valid = rtc_alarm_time_valid();
  printf("      时间已对时 : %s（rtc_alarm_time_valid()=%d）\n",
         valid ? "是" : "否，RTC 还没对时；模块会每 30 秒重试等对时", valid);

  if (!time_given)
    {
      hour   = now.tm_hour;
      minute = now.tm_min + 1;
      if (minute >= 60)
        {
          minute -= 60;
          hour = (hour + 1) % 24;
        }

      printf("      未指定时刻：用当前时间 + 1 分钟 = %02d:%02d\n",
             hour, minute);
    }

  if (hour < 0 || hour > 23 || minute < 0 || minute > 59)
    {
      printf("      时刻非法: %02d:%02d（应在 00:00..23:59）\n", hour, minute);
      report("每日提醒", 0, "时刻参数非法");
      return -1;
    }

  if (valid)
    {
      int now_min  = now.tm_hour * 60 + now.tm_min;
      int want_min = hour * 60 + minute;

      printf("      下一个提醒 : %s %02d:%02d\n",
             now_min >= want_min ? "明天" : "今天", hour, minute);
    }
  else
    {
      printf("      下一个提醒 : 待 RTC 对时后才能确定（对时前模块不排 alarm）\n");
    }

  g_rtcday_fired = 0;
  g_rtcday_calls = 0;

  ret = rtc_alarm_at_daily(hour, minute, rtcday_cb, NULL);
  if (ret != OK)
    {
      printf("      rtc_alarm_at_daily 失败: %d\n", ret);
      report("注册每日提醒", 0, "rtc_alarm_at_daily 失败");
      return -1;
    }

  report("注册每日提醒", 1, "已交给模块工作线程排 alarm");
  printf("      等回调（最多 %d 秒）...\n", RTCDAY_MAX_WAIT_MS / 1000);

  /* 计时用单调时钟：clock_systime_ticks() + TICK2MSEC()，
   * 不用"每轮 sleep 就累加固定值"（工程刚修过这个计时 bug）。 */

  t0 = clock_systime_ticks();
  elapsed_ms = 0;

  while (!g_rtcday_fired && elapsed_ms < RTCDAY_MAX_WAIT_MS)
    {
      usleep(RTCDAY_POLL_MS * 1000);
      elapsed_ms = (uint32_t)TICK2MSEC(clock_systime_ticks() - t0);
    }

  /* 不管成功还是超时都收尾：别让 worker 一直占着唯一的 alarm 槽 */

  (void)rtc_alarm_cancel();

  if (!g_rtcday_fired)
    {
      printf("      超时：等了 %u ms 没收到回调\n", (unsigned)elapsed_ms);
      report("每日提醒", 0, "超时未回调（已 rtc_alarm_cancel 收尾）");
      return -1;
    }

  printf("      等了 %u ms 收到回调（共 %d 次）\n",
         (unsigned)elapsed_ms, g_rtcday_calls);
  report("每日提醒", 1, "回调已触发");
  return OK;
}

/****************************************************************************
 * Name: audio_reader_task
 *
 * Description:
 *   audio 子命令的读任务：audio_in_read() 会阻塞到读满，或被
 *   audio_in_stop() 发出的 AUDIOIOC_STOP 唤醒。
 *   按 AUDIO_CHUNK_BYTES（1 秒）分块读，避免踩驱动下层 5 秒的 read 超时。
 *
 ****************************************************************************/

static int audio_reader_task(int argc, FAR char *argv)
{
  int offset = 0;

  (void)argc;
  (void)argv;

  while (offset < g_arec_len)
    {
      int chunk = g_arec_len - offset;
      ssize_t n;

      if (chunk > AUDIO_CHUNK_BYTES)
        {
          chunk = AUDIO_CHUNK_BYTES;
        }

      n = audio_in_read((FAR char *)g_arec_buf + offset, chunk);
      if (n <= 0)
        {
          /* <=0 必须跳出：0 = 被 STOP 打断或下层 5 秒超时（不是
           * "再读一次就有数据"），负值 = 未 start / fd 已失效。 */
          break;
        }

      offset += (int)n;
    }

  g_arec_n    = offset;
  g_arec_done = 1;
  return 0;
}

/****************************************************************************
 * Name: audio_level
 *
 * Description:
 *   统计 peak / avg，判断有没有声音。返回 peak。
 *
 ****************************************************************************/

static int audio_level(FAR const int16_t *buf, int nsamples)
{
  int peak = 0;
  long sum = 0;
  int i;

  for (i = 0; i < nsamples; i++)
    {
      int v = buf[i];

      if (v < 0)
        {
          v = -v;
        }

      if (v > peak)
        {
          peak = v;
        }

      sum += v;
    }

  printf("      peak=%d avg=%ld (16k mono 16bit)\n",
         peak, nsamples > 0 ? sum / nsamples : 0);
  printf("      声音检测 : %s\n",
         peak > AUDIO_SOUND_PEAK ? "有声音" : "静音（麦克风没信号/没说话）");
  return peak;
}

/****************************************************************************
 * Name: step_audio
 *
 * Description:
 *   audio 子命令：录 N 秒到内存，打印 peak/avg，不写文件。
 *
 *   走的是板级封装 sf32lb52_audio_in（audio_in_start/read/stop），它内部
 *   就是 app/audio_test 真机验证过的那套接口：
 *     open(/dev/audio/audio0, O_RDONLY)
 *     ioctl(AUDIOIOC_CONFIGURE, AUDIO_TYPE_INPUT 16k mono 16bit)
 *     ioctl(AUDIOIOC_START) -> read() 阻塞读满 -> ioctl(AUDIOIOC_STOP)
 *   所以这一步同时也是对那个封装的自检。
 *
 *   阻塞的 audio_in_read() 放独立任务，主任务带超时；超时就调
 *   audio_in_stop()（内部发 AUDIOIOC_STOP，驱动已修：STOP 能唤醒阻塞中的
 *   read），保证不永久卡住。
 *
 ****************************************************************************/

static int step_audio(int seconds)
{
  FAR int16_t *buf;
  int nsamples;
  clock_t t0;
  uint32_t elapsed_ms;
  int ret;

  printf("[AUDIO] %s 录音 %d 秒\n", AUDIO_DEV, seconds);

  if (seconds < 1)
    {
      seconds = AUDIO_DEFAULT_SEC;
    }

  nsamples = AUDIO_SAMPLE_RATE * seconds;
  buf = (FAR int16_t *)malloc((size_t)nsamples * sizeof(int16_t));
  if (buf == NULL)
    {
      printf("      malloc %d 字节失败\n", nsamples * 2);
      report("录音", 0, "内存不足");
      return -1;
    }

  /* open + CONFIGURE + START 都收在封装里，失败看返回的负 errno */

  ret = audio_in_start(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BITS);
  if (ret < 0)
    {
      printf("      audio_in_start(%d, %d, %d) 失败: %d\n",
             AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BITS, ret);
      printf("      （录音封装的用法见 docs/audio_driver_usage.md 第 9 节）\n");
      report("启动录音", 0, "audio_in_start 失败");
      free(buf);
      return -1;
    }

  g_arec_buf  = buf;
  g_arec_len  = nsamples * 2;
  g_arec_done = 0;
  g_arec_n    = -999;

  if (task_create("hwtest_rec", 100, AUDIO_READ_TASK_STACK,
                  (main_t)audio_reader_task, NULL) < 0)
    {
      printf("      task_create 失败: %d\n", errno);
      audio_in_stop();
      report("录音", 0, "起读任务失败");
      free(buf);
      return -1;
    }

  /* 计时同样用单调时钟（理由见 step_rtc 里的注释） */

  t0 = clock_systime_ticks();
  elapsed_ms = 0;

  while (!g_arec_done &&
         elapsed_ms < (uint32_t)(seconds + AUDIO_WAIT_SLACK_SEC) * 1000)
    {
      usleep(100 * 1000);
      elapsed_ms = (uint32_t)TICK2MSEC(clock_systime_ticks() - t0);
    }

  if (!g_arec_done)
    {
      /* audio_in_stop() 里的 AUDIOIOC_STOP 能让阻塞在 read() 里的任务返回 */

      audio_in_stop();
      t0 = clock_systime_ticks();
      while (!g_arec_done &&
             (uint32_t)TICK2MSEC(clock_systime_ticks() - t0) < 1000)
        {
          usleep(100 * 1000);
        }

      printf("      read 没在 %d 秒内返回，已调 audio_in_stop()\n",
             seconds + AUDIO_WAIT_SLACK_SEC);
      report("录音", 0, g_arec_done ? "read 被 STOP 唤醒" : "read 卡住");

      if (g_arec_done)
        {
          free(buf);
        }
      else
        {
          /* 读任务还活着：**故意不释放缓冲**。它可能仍挂在驱动的 read()
           * 里持有这块地址，一 free 就是 use-after-free。
           * fd 已由 audio_in_stop() 关闭，读任务下次进 audio_in_read()
           * 会直接拿到 -EINVAL 跳出，不会再往这块内存写。
           * 一次失败的自检，漏一块缓冲是可以接受的代价。 */
          printf("      （读任务还活着，故意不释放缓冲，避免它醒来写已释放内存）\n");
        }

      return -1;
    }

  printf("      read 返回 %zd 字节（期望 %d）\n", g_arec_n, nsamples * 2);
  audio_in_stop();

  if (g_arec_n <= 0)
    {
      report("录音", 0, "read 返回 <= 0");
      free(buf);
      return -1;
    }

  {
    int peak = audio_level(buf, (int)(g_arec_n / 2));
    char detail[64];

    snprintf(detail, sizeof(detail), "%d 字节, peak=%d%s",
             (int)g_arec_n, peak,
             peak > AUDIO_SOUND_PEAK ? " 有声音" : " 静音");
    report("录音", 1, detail);
  }

  free(buf);
  return OK;
}

/****************************************************************************
 * Name: alarm_level_name / alarm_event_name
 ****************************************************************************/

static FAR const char *alarm_level_name(enum alarm_level_e level)
{
  switch (level)
    {
      case ALARM_LEVEL_NOTICE:    return "NOTICE";
      case ALARM_LEVEL_WARNING:   return "WARNING";
      case ALARM_LEVEL_EMERGENCY: return "EMERGENCY";
      default:                    return "NONE";
    }
}

static FAR const char *alarm_event_name(enum alarm_event_e event)
{
  switch (event)
    {
      case ALARM_EVENT_TRIGGERED: return "TRIGGERED";
      case ALARM_EVENT_CLEARED:   return "CLEARED";
      case ALARM_EVENT_TIMEOUT:   return "TIMEOUT";
      default:                    return "?";
    }
}

/****************************************************************************
 * Name: alarm_test_cb
 *
 * Description:
 *   alarm 子命令的回调。**它在报警模块的工作线程里执行**，
 *   所以这里只打印 + 计数，绝不做 sleep / 等锁之类的阻塞动作。
 *
 ****************************************************************************/

static void alarm_test_cb(enum alarm_event_e event,
                          FAR const struct alarm_status_s *status,
                          FAR void *arg)
{
  (void)arg;

  g_alarm_events++;
  if (status != NULL)
    {
      g_alarm_last_level = (int)status->level;
    }

  printf("      [ALARM] event=%s level=%s reason=%s\n",
         alarm_event_name(event),
         alarm_level_name(status != NULL ? status->level : ALARM_LEVEL_NONE),
         status != NULL ? status->reason : "");
}

/****************************************************************************
 * Name: step_alarm
 *
 * Description:
 *   alarm 子命令：注册回调 -> 触发报警 -> 保持 N 秒（期间能听到声音）
 *   -> alarm_clear() -> 打印状态并用 report() 给 PASS/FAIL。
 *
 *   计时用 clock_systime_ticks() + TICK2MSEC()，不用"每轮累加固定值"。
 *
 ****************************************************************************/

static int step_alarm(int level_num, int seconds)
{
  struct alarm_status_s st;
  enum alarm_level_e    level;
  clock_t               t0;
  uint32_t              elapsed_ms;

  switch (level_num)
    {
      case 1:  level = ALARM_LEVEL_NOTICE;    break;
      case 2:  level = ALARM_LEVEL_WARNING;   break;
      case 3:  level = ALARM_LEVEL_EMERGENCY; break;
      default: level = ALARM_LEVEL_EMERGENCY; break;
    }

  if (seconds < 1)
    {
      seconds = ALARM_DEFAULT_SEC;
    }

  printf("[ALARM] 级别=%d(%s) 保持 %d 秒\n",
         (int)level, alarm_level_name(level), seconds);

  g_alarm_events     = 0;
  g_alarm_last_level = 0;

  if (alarm_set_callback(alarm_test_cb, NULL) != OK)
    {
      report("注册报警回调", 0, "alarm_set_callback 失败");
      return -1;
    }

  if (alarm_trigger(level, "hw_test", "hw_test alarm 子命令") != OK)
    {
      report("触发报警", 0, "alarm_trigger 失败");
      alarm_set_callback(NULL, NULL);
      return -1;
    }

  report("触发报警", 1, alarm_level_name(level));

  /* 单调时钟计时（同 step_rtc / step_audio，别自己累加固定值） */

  t0 = clock_systime_ticks();
  elapsed_ms = 0;
  while (elapsed_ms < (uint32_t)seconds * 1000)
    {
      usleep(ALARM_POLL_MS * 1000);
      elapsed_ms = (uint32_t)TICK2MSEC(clock_systime_ticks() - t0);
    }

  memset(&st, 0, sizeof(st));
  if (alarm_get_status(&st) == OK)
    {
      printf("      状态: active=%d level=%s repeat=%u reason=%s text=%s\n",
             (int)st.active, alarm_level_name(st.level),
             (unsigned)st.repeat_count, st.reason, st.text);
      report("报警持续", st.active ? 1 : 0,
             st.active ? "仍在报警中" : "提前结束");
    }
  else
    {
      report("读报警状态", 0, "alarm_get_status 失败");
    }

  alarm_clear();

  /* 等一小会，让工作线程把音频 STOP 掉，并回调 CLEARED */

  t0 = clock_systime_ticks();
  while ((uint32_t)TICK2MSEC(clock_systime_ticks() - t0) < ALARM_SETTLE_MS)
    {
      usleep(ALARM_POLL_MS * 1000);
    }

  memset(&st, 0, sizeof(st));
  alarm_get_status(&st);
  printf("      解除后: active=%d level=%s 回调事件=%d 次\n",
         (int)st.active, alarm_level_name(st.level), g_alarm_events);
  report("解除报警", st.active ? 0 : 1, st.active ? "仍在报警" : "已解除");

  report("报警回调", g_alarm_events >= 1 ? 1 : 0,
         g_alarm_events >= 1 ? "收到回调" : "没有收到任何回调");

  alarm_set_callback(NULL, NULL);
  return OK;
}

/****************************************************************************
 * Name: arg_is_number
 *
 * Description:
 *   判断一个命令行参数是不是纯十进制数字。
 *   子命令的可选数值参数用它来决定"要不要吃掉下一个 argv"—— 否则
 *   `hw_test lcd gpio` 会把 atoi("gpio") = 0 当成亮度 0。
 *
 ****************************************************************************/

static bool arg_is_number(FAR const char *s)
{
  if (s == NULL || *s == '\0')
    {
      return false;
    }

  if (*s == '-' || *s == '+')
    {
      s++;
    }

  if (*s == '\0')
    {
      return false;
    }

  for (; *s != '\0'; s++)
    {
      if (*s < '0' || *s > '9')
        {
          return false;
        }
    }

  return true;
}

/****************************************************************************
 * Name: step_backlight
 *
 * Description:
 *   lcd 子命令：读当前亮度 -> backlight_set(percent) -> 等一小会 -> 回读，
 *   每步都打印，并用 report() 给 PASS/FAIL。
 *
 *   计时用 clock_systime_ticks() + TICK2MSEC()（同 step_alarm / step_rtc，
 *   别自己累加固定值）。
 *
 *   打了 vendor 补丁后 0..100 都能 PASS；没打补丁的树上 SETCONTRAST 是
 *   -ENOSYS，只有 0 和 100 能 PASS，中间值这里如实报 FAIL（不掩盖）。
 *   详见 docs/display_touch_gpio_usage.md 第 3.3 节。
 *
 ****************************************************************************/

static int step_backlight(int percent)
{
  clock_t  t0;
  uint32_t elapsed_ms;
  int      before;
  int      after;
  int      ret;

  printf("[LCD ] 屏幕亮度 /dev/lcd0，目标 = %d%%\n", percent);

  before = backlight_get();
  if (before < 0)
    {
      printf("      设置前读亮度失败: %d\n", before);
    }
  else
    {
      printf("      设置前: %d%%\n", before);
    }

  t0 = clock_systime_ticks();
  ret = backlight_set(percent);
  elapsed_ms = (uint32_t)TICK2MSEC(clock_systime_ticks() - t0);

  printf("      backlight_set(%d) -> %d，耗时 %u ms\n",
         percent, ret, (unsigned)elapsed_ms);

  if (ret != OK)
    {
      report("设置亮度", 0,
             ret == -ENOSYS
                 ? "本树不支持百分比亮度（vendor 补丁没打？）"
                 : "backlight_set 失败");
      return -1;
    }

  if (percent == 0)
    {
      report("设置亮度", 1, "面板已关（最暗）");
    }
  else if (percent >= BACKLIGHT_MAX_PERCENT)
    {
      report("设置亮度", 1, "面板全亮");
    }
  else
    {
      report("设置亮度", 1, "已下发百分比亮度");
    }

  /* 等一下再回读，别刚下发就去要状态 */

  t0 = clock_systime_ticks();
  while ((uint32_t)TICK2MSEC(clock_systime_ticks() - t0) < BACKLIGHT_SETTLE_MS)
    {
      usleep(BACKLIGHT_POLL_MS * 1000);
    }

  after = backlight_get();
  if (after < 0)
    {
      report("回读亮度", 0, "backlight_get 失败");
      return -1;
    }

  printf("      设置后: %d%%\n", after);
  report("回读亮度", after == percent ? 1 : 0,
         after == percent ? NULL : "回读值与设置值不一致");

  return OK;
}

/****************************************************************************
 * Name: step_status
 *
 * Description:
 *   status 子命令：打一份统一外设状态（board_status_get/dump），
 *   并判 PASS/FAIL。
 *
 *   判定规则（和默认自检里 optional 节点的思路一致）：
 *     - **必需**只有一条：网络拿到非回环 IPv4 地址。没地址是 FAIL ——
 *       板子的 MQTT / AI 全链路都依赖它，是"真故障"；
 *     - 其它（/etc/assets、/data、音频、LCD、触摸、RTC、MQTT）**只提示**，
 *       不计入 PASS/FAIL：它们可能被别的模块占着、也可能是配置差异，
 *       拿来判死刑会误报（和 eth0 未枚举、CONFIG_PWM=n 的处理一样）。
 *
 *   本子命令**只读**：board_status_get() 只做 open/close、stat、
 *   写一个 0 字节探针文件（立刻删）、读一次 RTC，不初始化任何设备。
 *
 ****************************************************************************/

static int step_status(void)
{
  struct board_status_s st;
  int ret;

  printf("[STATUS] 统一外设状态（board_status_get）\n\n");

  ret = board_status_get(&st);
  if (ret < 0)
    {
      printf("      board_status_get 失败: %d\n", ret);
      report("状态查询", 0, "board_status_get 失败");
      return -1;
    }

  board_status_dump();
  printf("\n");

  /* 必需项：网络 */

  if (st.net_link_up)
    {
      char detail[24];

      snprintf(detail, sizeof(detail), "%s", st.net_ip);
      report("网络已获取 IPv4 地址", 1, detail);
    }
  else
    {
      report("网络已获取 IPv4 地址", 0,
             "没有非回环 IPv4 地址（USB RNDIS 没枚举起来？）");
    }

  /* 其余只提示，不影响 PASS/FAIL */

  printf("      [提示] ROM 素材 /etc/assets : %s（不计入判定）\n",
         st.rom_assets_ready ? "可读" : "读不到");
  printf("      [提示] /data 可写           : %s（不计入判定）\n",
         st.data_ready ? "是" : "否");
  printf("      [提示] 音频播放 / 录音      : %s / %s（不计入判定）\n",
         st.audio_play_ready ? "可打开" : "打不开",
         st.audio_rec_ready ? "可打开" : "打不开");
  printf("      [提示] 显示 / 触摸          : %s / %s（不计入判定）\n",
         st.lcd_ready ? "可打开" : "打不开",
         st.touch_ready ? "可打开" : "打不开");
  printf("      [提示] 按键 PA11            : %s（不计入判定）\n",
         st.btn_key_pressed ? "按下" : "松开");
  printf("      [提示] RTC 时间             : %s（不计入判定）\n",
         st.rtc_valid ? "有效" : "无效（年份 < 2000）");
  printf("      [提示] MQTT                 : %s（不计入判定）\n",
         st.mqtt_connected ? "已连接"
                           : "未连接（要由网络模块 board_status_set_mqtt 喂）");
  printf("      [提示] 运行时间             : %u s\n", (unsigned)st.uptime_s);

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  int do_color      = 0;
  int do_gpio       = 0;
  int do_imu        = 0;
  int do_rtc        = 0;
  int do_rtcday     = 0;
  int do_audio      = 0;
  int do_alarm      = 0;
  int do_button     = 0;
  int do_backlight  = 0;
  int do_status     = 0;
  int standalone;
  int imu_frames    = IMU_DEFAULT_FRAMES;
  int rtc_sec       = RTC_DEFAULT_ALARM_SEC;
  int rtcday_hour   = 0;
  int rtcday_min    = 0;
  int rtcday_set    = 0;
  int audio_sec     = AUDIO_DEFAULT_SEC;
  int alarm_level   = ALARM_DEFAULT_LEVEL;
  int alarm_sec     = ALARM_DEFAULT_SEC;
  int button_sec    = BTN_DEFAULT_WAIT_SEC;
  int lcd_percent   = BACKLIGHT_DEFAULT_PCT;
  int touch_sec     = TOUCH_DEFAULT_SEC;
  int touch_set     = 0;
  int i;

  g_pass  = 0;
  g_total = 0;

  for (i = 1; i < argc; i++)
    {
      if (strcmp(argv[i], "lcdcolor") == 0)
        {
          do_color = 1;
        }
      else if (strcmp(argv[i], "gpio") == 0)
        {
          do_gpio = 1;
        }
      else if (strcmp(argv[i], "touch") == 0)
        {
          touch_sec = (i + 1 < argc) ? atoi(argv[++i]) : TOUCH_DEFAULT_SEC;
          touch_set = 1;
        }
      else if (strcmp(argv[i], "imu") == 0)
        {
          do_imu     = 1;
          imu_frames = (i + 1 < argc) ? atoi(argv[++i]) : IMU_DEFAULT_FRAMES;
        }
      else if (strcmp(argv[i], "rtc") == 0)
        {
          do_rtc  = 1;
          rtc_sec = (i + 1 < argc) ? atoi(argv[++i]) : RTC_DEFAULT_ALARM_SEC;
        }
      else if (strcmp(argv[i], "rtcday") == 0)
        {
          do_rtcday = 1;
          if (i + 1 < argc)
            {
              rtcday_hour = atoi(argv[++i]);
              rtcday_set  = 1;
              rtcday_min  = (i + 1 < argc) ? atoi(argv[++i]) : 0;
            }
        }
      else if (strcmp(argv[i], "audio") == 0)
        {
          do_audio  = 1;
          audio_sec = (i + 1 < argc) ? atoi(argv[++i]) : AUDIO_DEFAULT_SEC;
        }
      else if (strcmp(argv[i], "alarm") == 0)
        {
          do_alarm    = 1;
          alarm_level = (i + 1 < argc) ? atoi(argv[++i]) : ALARM_DEFAULT_LEVEL;
          alarm_sec   = (i + 1 < argc) ? atoi(argv[++i]) : ALARM_DEFAULT_SEC;
        }
      else if (strcmp(argv[i], "lcd") == 0)
        {
          /* 只有下一个参数确实是数字才吃掉它，免得把 `hw_test lcd gpio`
           * 里的 "gpio" 当成亮度 0（atoi("gpio") == 0）。 */

          do_backlight = 1;
          if (i + 1 < argc && arg_is_number(argv[i + 1]))
            {
              lcd_percent = atoi(argv[++i]);
            }
        }
      else if (strcmp(argv[i], "button") == 0)
        {
          do_button  = 1;
          button_sec = (i + 1 < argc) ? atoi(argv[++i]) : BTN_DEFAULT_WAIT_SEC;
        }
      else if (strcmp(argv[i], "status") == 0)
        {
          do_status = 1;
        }
      else
        {
          printf("hw_test: 未知参数 '%s'\n", argv[i]);
          usage();
          return EXIT_FAILURE;
        }
    }

  /* imu / rtc / rtcday / audio / alarm / lcd / button / status 是各自独立的
   * 外设子命令：只跑自己，不跑那套 5 步自检 */

  standalone = do_imu || do_rtc || do_rtcday || do_audio || do_alarm ||
               do_backlight || do_button || do_status;

  /* lcdcolor 只是刷个屏，别让它再干等 10 秒触摸 */

  if (do_color && !touch_set)
    {
      touch_sec = 0;
    }

  printf("\n");
  printf("========================================\n");
  printf("   SF32LB52-DevKit-LCD 硬件自检 (hw_test)\n");
  if (standalone)
    {
      printf("   子命令模式：只跑 imu/rtc/rtcday/audio/alarm/lcd/button/status，"
             "不做 5 步自检\n");
    }
  else
    {
      printf("   默认只做只读检查；lcdcolor/gpio 才会改硬件\n");
    }

  printf("========================================\n\n");

  if (standalone)
    {
      if (do_imu)
        {
          step_imu(imu_frames);
          printf("\n");
        }

      if (do_rtc)
        {
          step_rtc(rtc_sec);
          printf("\n");
        }

      if (do_rtcday)
        {
          step_rtcday(rtcday_hour, rtcday_min, rtcday_set);
          printf("\n");
        }

      if (do_audio)
        {
          step_audio(audio_sec);
          printf("\n");
        }

      if (do_alarm)
        {
          step_alarm(alarm_level, alarm_sec);
          printf("\n");
        }

      if (do_backlight)
        {
          step_backlight(lcd_percent);
          printf("\n");
        }

      if (do_button)
        {
          step_button_wait(button_sec);
          printf("\n");
        }

      if (do_status)
        {
          step_status();
          printf("\n");
        }
    }
  else
    {
      step_nodes();
      printf("\n");
      step_touch(touch_sec);
      printf("\n");
      step_lcd(do_color);
      printf("\n");
      step_buttons(BTN_TIMEOUT_MS);
      printf("\n");
      step_gpio(do_gpio);
    }

  printf("\n========================================\n");
  printf("   结果: %d/%d PASS", g_pass, g_total);
  if (g_pass == g_total)
    {
      printf("   >>> 硬件自检通过 <<<\n");
    }
  else
    {
      printf("   >>> 有 FAIL 项，见上面标记 <<<\n");
    }

  printf("========================================\n\n");

  return g_pass == g_total ? EXIT_SUCCESS : EXIT_FAILURE;
}
