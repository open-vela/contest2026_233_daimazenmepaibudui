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
 *   hw_test audio [秒]       录 N 秒到内存，打印 peak/avg（默认 2 秒）—— 单独运行
 *
 * 设计约定：
 *   - 每一步失败都只打印 FAIL，不中断后面的步骤，也不会卡死
 *     （所有 open/ioctl/read 都判返回值，read 前先用 poll 等超时）
 *   - 默认（不带参数）不碰屏幕、不拉 GPIO 电平，只做只读自检
 *   - imu / rtc / audio 都会真的开外设（START 转换、设 alarm、开麦克风），
 *     所以放在子命令里；而且它们**不跑**上面那套 5 步自检，只跑自己，
 *     免得每次验 IMU 还要先等 10 秒触摸 + 5 秒按键
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
#include <nuttx/audio/audio.h>
#include <nuttx/clock.h>               /* clock_systime_ticks() + TICK2MSEC() */
#include <nuttx/sched.h>               /* task_create() */
#include <signal.h>                    /* SIGUSR1 + sigaction() */

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
#define BTN_DEV        "/dev/buttons"
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
#define BTN_POLL_MS        200
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

/* audio 子命令（与 app/audio_test 保持同一套参数：16k 单声道 16bit） */

#define AUDIO_SAMPLE_RATE     16000
#define AUDIO_CHANNELS        1
#define AUDIO_BITS            16
#define AUDIO_DEFAULT_SEC     2    /* 默认录音秒数 */
#define AUDIO_WAIT_SLACK_SEC  2    /* read 没在预期时间内返回就发 STOP 救场 */
#define AUDIO_SOUND_PEAK      500  /* peak 超过它算“检测到声音”（同 audio_test） */
#define AUDIO_READ_TASK_STACK 4096

/* 驱动下层一次 read 只等 5 秒（sf32lb52_audio.c:1130 的 rx_sem 超时），
 * 所以超过 1 秒的录音要拆成 1 秒一块地读，否则 6 秒的录音必然返回 0。
 * 32000 字节 = 16k 单声道 16bit × 1 秒，正是 audio_test 验证过的大小。
 */
#define AUDIO_CHUNK_BYTES     (AUDIO_SAMPLE_RATE * 2)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct dev_node_s
{
  FAR const char *path;   /* 设备节点 */
  FAR const char *desc;   /* 用途 */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static int g_pass;
static int g_total;

/* rtc 子命令：alarm 到点后由信号处理函数置位 */

static volatile sig_atomic_t g_rtc_alarm;

/* audio 子命令：阻塞的 read() 放在独立任务里，主任务带超时等它 */

static volatile int     g_arec_done;
static volatile ssize_t g_arec_n;
static int              g_arec_fd;
static FAR int16_t     *g_arec_buf;
static int              g_arec_len;

/* 与本板硬件相关的设备节点（枚举顺序 = 打印顺序） */

static const struct dev_node_s g_nodes[] =
{
  { "/dev/lcd0",         "AMOLED (CO5300), NuttX LCD dev" },
  { "/dev/fb0",          "framebuffer (LCD 的 fb 前端)" },
  { "/dev/input0",       "touchscreen (FT6146)" },
  { "/dev/buttons",      "buttons (Key2 = PA11)" },
  { "/dev/gpio0",        "GPIO input  (PA34, Key1/power key)" },
  { "/dev/gpio1",        "GPIO output (PA26, 板级用户输出)" },
  { "/dev/gpio2",        "GPIO interrupt (PA34)" },
  { "/dev/timer0",       "timer" },
  { "/dev/pwm0",         "pwm (背光 PA01 = GPTIM1_CH4)" },
  { "/dev/i2c0",         "i2c master (触摸 FT6146: SCL=PA30 SDA=PA33)" },
  { "/dev/i2c1",         "i2c master (I2C2)" },
  { "/dev/adc0",         "adc" },
  { "/dev/audio/audio0", "audio (NS4150B 功放 + MEMS MIC)" },
  /* 注意：网络接口**不是** /dev 节点。NuttX 的网卡是 netdev，
   * 走 socket + ioctl(SIOCGIF*) 访问，标准做法见下面 netdev_probe()。
   * 之前这里写成 "/dev/eth0" 是错的。 */
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
  printf("  hw_test audio [秒]   录音电平 peak/avg，默认 2 秒（单独运行）\n");
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
 *   注意：这条**不计入 PASS/FAIL**——USB RNDIS 需要主机侧插好并完成枚举，
 *   没插 USB 时接口不存在是正常的。
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
      printf("      %-8s --    接口未配置（USB 已插好并完成枚举？）\n", ifname);
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
 ****************************************************************************/

static int step_nodes(void)
{
  int missing = 0;
  int i;

  printf("[1/6] 设备节点枚举\n");

  for (i = 0; i < NNODES; i++)
    {
      int ok = node_exists(g_nodes[i].path);

      if (!ok)
        {
          missing++;
        }

      printf("      %-18s %-3s  %s\n", g_nodes[i].path, ok ? "OK" : "--",
             g_nodes[i].desc);
    }

  /* 网卡不是 /dev 节点，单独用 netdev API 查（不计入 PASS/FAIL） */
  netdev_probe("eth0");

  if (missing == 0)
    {
      report("全部节点存在", 1, NULL);
    }
  else
    {
      char detail[32];

      snprintf(detail, sizeof(detail), "%d 个节点缺失", missing);
      report("全部节点存在", 0, detail);
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
 * Name: step_buttons
 *
 * Description:
 *   第 4 步：非阻塞读 /dev/buttons，按键就打印键值；等到超时正常结束。
 *
 ****************************************************************************/

static int step_buttons(int timeout_ms)
{
  uint32_t t0;
  int events = 0;
  int fd;

  printf("[4/6] 按键 %s\n", BTN_DEV);

  fd = open(BTN_DEV, O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    {
      printf("      open 失败: %d\n", errno);
      report("打开按键设备", 0, "open /dev/buttons 失败");
      return -1;
    }

  printf("      最多等 %d 秒，请按一下 Key2（PA11）...\n", timeout_ms / 1000);

  t0 = mono_ms();
  while ((int)(mono_ms() - t0) < timeout_ms)
    {
      struct pollfd pfd;
      uint8_t buttons = 0;      /* btn_buttonset_t，按位表示按键集合 */
      ssize_t n;
      int ret;

      pfd.fd      = fd;
      pfd.events  = POLLIN;
      pfd.revents = 0;

      ret = poll(&pfd, 1, BTN_POLL_MS);
      if (ret < 0)
        {
          usleep(BTN_POLL_MS * 1000);   /* poll 不可用时退化成定时轮询 */
        }
      else if (ret == 0)
        {
          continue;
        }

      n = read(fd, &buttons, sizeof(buttons));
      if (n < (ssize_t)sizeof(buttons))
        {
          continue;
        }

      printf("      按键事件: buttonset=0x%02x (Key2 %s)\n",
             (unsigned)buttons, (buttons & 1) ? "按下" : "松开");
      events++;
    }

  close(fd);

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
 * Name: audio_reader_task
 *
 * Description:
 *   audio 子命令的读任务：read() 会阻塞到读满，或被 AUDIOIOC_STOP 唤醒。
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

      n = read(g_arec_fd, (FAR char *)g_arec_buf + offset, chunk);
      if (n <= 0)
        {
          break;                    /* 出错 / 被 STOP 打断（驱动按 0 返回） */
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
 *   走的完全是 app/audio_test 真机验证过的那套接口：
 *     open(/dev/audio/audio0, O_RDONLY)
 *     ioctl(AUDIOIOC_CONFIGURE, AUDIO_TYPE_INPUT 16k mono 16bit)
 *     ioctl(AUDIOIOC_START) -> read() 阻塞读满 -> ioctl(AUDIOIOC_STOP)
 *   阻塞的 read 放独立任务，主任务带超时；超时就 STOP（驱动已修：
 *   STOP 能唤醒阻塞中的 read），保证不永久卡住。
 *
 ****************************************************************************/

static int step_audio(int seconds)
{
  struct audio_caps_desc_s capdesc;
  FAR int16_t *buf;
  int nsamples;
  clock_t t0;
  uint32_t elapsed_ms;
  int fd;

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

  fd = open(AUDIO_DEV, O_RDONLY);
  if (fd < 0)
    {
      printf("      open 失败: %d（节点带 audio/ 子目录，别写成 /dev/audio0）\n",
             errno);
      report("打开音频设备", 0, "open /dev/audio/audio0 失败");
      free(buf);
      return -1;
    }

  memset(&capdesc, 0, sizeof(capdesc));
  capdesc.caps.ac_len            = sizeof(struct audio_caps_s);
  capdesc.caps.ac_type           = AUDIO_TYPE_INPUT;
  capdesc.caps.ac_channels       = AUDIO_CHANNELS;
  capdesc.caps.ac_controls.hw[0] = AUDIO_SAMPLE_RATE;
  capdesc.caps.ac_controls.b[2]  = AUDIO_BITS;

  if (ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&capdesc) < 0)
    {
      printf("      AUDIOIOC_CONFIGURE(input) 失败: %d\n", errno);
      report("配置录音通路", 0, "CONFIGURE 失败");
      close(fd);
      free(buf);
      return -1;
    }

  if (ioctl(fd, AUDIOIOC_START, 0) < 0)
    {
      printf("      AUDIOIOC_START 失败: %d\n", errno);
      report("启动录音", 0, "START 失败");
      close(fd);
      free(buf);
      return -1;
    }

  g_arec_fd   = fd;
  g_arec_buf  = buf;
  g_arec_len  = nsamples * 2;
  g_arec_done = 0;
  g_arec_n    = -999;

  if (task_create("hwtest_rec", 100, AUDIO_READ_TASK_STACK,
                  (main_t)audio_reader_task, NULL) < 0)
    {
      printf("      task_create 失败: %d\n", errno);
      ioctl(fd, AUDIOIOC_STOP, 0);
      report("录音", 0, "起读任务失败");
      close(fd);
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
      /* 驱动已修：AUDIOIOC_STOP 能让阻塞在 read() 里的任务返回 */

      ioctl(fd, AUDIOIOC_STOP, 0);
      t0 = clock_systime_ticks();
      while (!g_arec_done &&
             (uint32_t)TICK2MSEC(clock_systime_ticks() - t0) < 1000)
        {
          usleep(100 * 1000);
        }

      printf("      read 没在 %d 秒内返回，已发 AUDIOIOC_STOP\n",
             seconds + AUDIO_WAIT_SLACK_SEC);
      report("录音", 0, g_arec_done ? "read 被 STOP 唤醒" : "read 卡住");

      if (g_arec_done)
        {
          close(fd);
          free(buf);
        }
      else
        {
          /* 读任务还卡在 read() 里：这里**故意不 close / 不 free**。
           * 否则它以后被唤醒时会往已经释放的内存里写（use-after-free）。
           * 一次失败的自检，漏一块缓冲 + 一个 fd 是可以接受的代价。 */
          printf("      （读任务还活着，故意不关 fd / 不释放缓冲，避免它醒来写已释放内存）\n");
        }

      return -1;
    }

  printf("      read 返回 %zd 字节（期望 %d）\n", g_arec_n, nsamples * 2);
  ioctl(fd, AUDIOIOC_STOP, 0);
  close(fd);

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
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  int do_color      = 0;
  int do_gpio       = 0;
  int do_imu        = 0;
  int do_rtc        = 0;
  int do_audio      = 0;
  int standalone;
  int imu_frames    = IMU_DEFAULT_FRAMES;
  int rtc_sec       = RTC_DEFAULT_ALARM_SEC;
  int audio_sec     = AUDIO_DEFAULT_SEC;
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
      else if (strcmp(argv[i], "audio") == 0)
        {
          do_audio  = 1;
          audio_sec = (i + 1 < argc) ? atoi(argv[++i]) : AUDIO_DEFAULT_SEC;
        }
      else
        {
          printf("hw_test: 未知参数 '%s'\n", argv[i]);
          usage();
          return EXIT_FAILURE;
        }
    }

  /* imu / rtc / audio 是各自独立的外设子命令：只跑自己，不跑那套 5 步自检 */

  standalone = do_imu || do_rtc || do_audio;

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
      printf("   子命令模式：只跑 imu/rtc/audio，不做 5 步自检\n");
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

      if (do_audio)
        {
          step_audio(audio_sec);
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
