/****************************************************************************
 * app/hw_test/main.c
 *
 * SF32LB52-DevKit-LCD 硬件自检（显示 / 触摸 / 按键 / GPIO）
 *
 * 用法：
 *   hw_test                 只读自检：不动屏幕、不拉 GPIO 电平
 *   hw_test touch <秒>       指定触摸观察时长（0 = 跳过触摸步骤）
 *   hw_test lcdcolor        额外做一次刷色测试（会改屏，退出前清屏）
 *   hw_test gpio            额外翻转一次板级 GPIO 输出脚 PA26（/dev/gpio1）
 *
 * 设计约定：
 *   - 每一步失败都只打印 FAIL，不中断后面的步骤，也不会卡死
 *     （所有 open/ioctl/read 都判返回值，read 前先用 poll 等超时）
 *   - 默认（不带参数）不碰屏幕、不拉 GPIO 电平，只做只读自检
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

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define LCD_DEV        "/dev/lcd0"
#define INPUT_DEV      "/dev/input0"
#define BTN_DEV        "/dev/buttons"
#define GPIO_IN_DEV    "/dev/gpio0"
#define GPIO_OUT_DEV   "/dev/gpio1"
#define GPIO_INT_DEV   "/dev/gpio2"

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
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  int do_color      = 0;
  int do_gpio       = 0;
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
      else
        {
          printf("hw_test: 未知参数 '%s'\n", argv[i]);
          usage();
          return EXIT_FAILURE;
        }
    }

  /* lcdcolor 只是刷个屏，别让它再干等 10 秒触摸 */

  if (do_color && !touch_set)
    {
      touch_sec = 0;
    }

  printf("\n");
  printf("========================================\n");
  printf("   SF32LB52-DevKit-LCD 硬件自检 (hw_test)\n");
  printf("   默认只做只读检查；lcdcolor/gpio 才会改硬件\n");
  printf("========================================\n\n");

  step_nodes();
  printf("\n");
  step_touch(touch_sec);
  printf("\n");
  step_lcd(do_color);
  printf("\n");
  step_buttons(BTN_TIMEOUT_MS);
  printf("\n");
  step_gpio(do_gpio);

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
