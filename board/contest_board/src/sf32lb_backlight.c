/****************************************************************************
 * board/contest_board/src/sf32lb_backlight.c
 *
 * SF32LB52 屏幕亮度封装：backlight_set() / backlight_get()
 *
 * 把 /dev/lcd0 包成“0..100 亮度”：
 *
 *   - 0     -> ioctl(LCDDEVIO_SETPOWER, 0)，真的把面板关掉（最暗）；
 *   - 1..99 -> ioctl(LCDDEVIO_SETCONTRAST, percent)，走面板自己的亮度寄存器
 *              （CO5300 的 0x51 WBRIGHT，见 co5300.c 的 LCD_SetBrightness()）；
 *   - 100   -> 同上，再确保屏幕是亮的（SETPOWER 全功率）。
 *
 * 中间值这条通路依赖 vendor/sifli 的补丁
 * patches/vendor_sifli-lcd-brightness.patch：它把 sf32lb_lcd.c 里死掉的
 * LCDDEVIO_SETCONTRAST/GETCONTRAST 接到面板驱动的 SetBrightness 回调上。
 * **没打补丁的树仍然只有 0/100 两档**：SETCONTRAST 回 -ENOSYS，本封装照实
 * 把 -ENOSYS 交出去，1..99 不假装成功（0 和 100 依旧可靠，见下面各档的处理）。
 * 细节见 docs/display_touch_gpio_usage.md 第 3.3 节。
 *
 * 内部拿一把 nxmutex 保护 fd 与状态，并在锁里完成 open（懒打开、成功后复用）。
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
#include <errno.h>
#include <syslog.h>

#include <nuttx/lcd/lcd_dev.h>
#include <nuttx/mutex.h>

#include "sf32lb52_backlight.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 面板“全亮”时下发给 SETPOWER 的 power 值。宏由 Kconfig 给（本板 defconfig 里
 * CONFIG_LCD_MAXPOWER=100），这里兜个底，免得配置不带 LCD 时编不过。 */

#ifndef CONFIG_LCD_MAXPOWER
#  define CONFIG_LCD_MAXPOWER 100
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 当前打开的 /dev/lcd0 fd，-1 = 还没打开 */

static int g_bl_fd = -1;

/* 模块内的屏幕开关视图：-1 = 还不知道（开机后还没设过），0 = 关，1 = 开。
 * 只用来避免拖滑块时每次都重复下发 SETPOWER(DisplayOn)。 */

static int g_bl_on = -1;

/* “最近一次成功下发”的亮度，-1 = 还没成功设过。
 * **这是模块内的缓存值，不是驱动回读值**：只在 GETCONTRAST 用不了
 * （未打 vendor 补丁的树，GETCONTRAST/SETCONTRAST 都是 -ENOSYS）时兜底。
 * 打了补丁的树一律以驱动回读为准，不走这里。 */

static int g_bl_last = -1;

/* fd 与上面两个状态量的保护锁。
 * 只覆盖很短的一段（open + ioctl），不会阻塞别的任务。 */

static mutex_t g_bl_lock = NXMUTEX_INITIALIZER;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: backlight_ensure_fd
 *
 * Description:
 *   懒打开 /dev/lcd0，成功后复用同一个 fd。
 *   **调用者必须已持有 g_bl_lock**。
 *
 * Returned Value:
 *   >= 0 为 fd；负值为负 errno。
 *
 ****************************************************************************/

static int backlight_ensure_fd(void)
{
  int fd;

  if (g_bl_fd >= 0)
    {
      return g_bl_fd;
    }

  fd = open(BACKLIGHT_DEV, O_RDWR);
  if (fd < 0)
    {
      int errcode = errno;

      syslog(LOG_ERR, "BACKLIGHT: open %s failed: %d\n",
             BACKLIGHT_DEV, errcode);
      return -errcode;
    }

  /* 最后一步才发布 fd：中途失败时全局状态一直是“没打开” */

  g_bl_fd = fd;
  return fd;
}

/****************************************************************************
 * Name: backlight_setpower
 *
 * Description:
 *   开 / 关屏：power == 0 关，power > 0 开。这是 SETPOWER 的本来语义。
 *
 ****************************************************************************/

static int backlight_setpower(int fd, int power)
{
  if (ioctl(fd, LCDDEVIO_SETPOWER, (unsigned long)power) < 0)
    {
      int errcode = errno;

      syslog(LOG_ERR, "BACKLIGHT: SETPOWER(%d) failed: %d\n",
             power, errcode);
      return -errcode;
    }

  return OK;
}

/****************************************************************************
 * Name: backlight_setcontrast
 *
 * Description:
 *   下发亮度百分比（打上 vendor 补丁后，驱动的 SETCONTRAST 就是亮度）。
 *
 ****************************************************************************/

static int backlight_setcontrast(int fd, int percent)
{
  if (ioctl(fd, LCDDEVIO_SETCONTRAST, (unsigned long)percent) < 0)
    {
      int errcode = errno;

      syslog(LOG_ERR, "BACKLIGHT: SETCONTRAST(%d) failed: %d\n",
             percent, errcode);
      return -errcode;
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: backlight_set
 ****************************************************************************/

int backlight_set(int percent)
{
  int fd;
  int ret;

  if (percent < BACKLIGHT_MIN_PERCENT || percent > BACKLIGHT_MAX_PERCENT)
    {
      syslog(LOG_ERR, "BACKLIGHT: percent %d out of range 0..100\n", percent);
      return -EINVAL;
    }

  if (nxmutex_lock(&g_bl_lock) < 0)
    {
      return -EINVAL;                 /* 锁坏掉了，属于不可能的状态 */
    }

  fd = backlight_ensure_fd();
  if (fd < 0)
    {
      ret = fd;
      goto out;
    }

  /* 0 = 关屏。SETPOWER(0) 是唯一能真的把面板关掉的手段（DisplayOff）；
   * 顺手把亮度也压到 0，让关屏期间的回读一致。老驱动 SETCONTRAST 是
   * -ENOSYS，那次失败忽略 —— 关屏本身已经成功了，别把它当失败报出去。 */

  if (percent == BACKLIGHT_MIN_PERCENT)
    {
      ret = backlight_setpower(fd, 0);
      if (ret >= 0)
        {
          (void)backlight_setcontrast(fd, 0);
          g_bl_on   = 0;
          g_bl_last = 0;
        }

      goto out;
    }

  /* 1..100：先下亮度，走面板的亮度寄存器 */

  ret = backlight_setcontrast(fd, percent);
  if (ret < 0)
    {
      /* 未打补丁的树上 SETCONTRAST 回 -ENOSYS，1..99 兑现不了，如实上报。
       * 但 100% 不算失败：SETPOWER(>0) 本来就是“全亮”，下面的开屏一步
       * 就能兑现，所以保留 100 的可靠行为。 */

      if (!(percent == BACKLIGHT_MAX_PERCENT && ret == -ENOSYS))
        {
          goto out;
        }
    }

  /* 屏幕关着（或状态还不知道）就确保点亮；已经确认亮着就不重复下发 */

  if (g_bl_on != 1)
    {
      ret = backlight_setpower(fd, CONFIG_LCD_MAXPOWER);
      if (ret < 0)
        {
          goto out;
        }

      g_bl_on = 1;
    }

  g_bl_last = percent;
  ret = OK;

out:
  nxmutex_unlock(&g_bl_lock);
  return ret;
}

/****************************************************************************
 * Name: backlight_get
 ****************************************************************************/

int backlight_get(void)
{
  int fd;
  int contrast;
  int ret;

  if (nxmutex_lock(&g_bl_lock) < 0)
    {
      return -EINVAL;
    }

  fd = backlight_ensure_fd();
  if (fd < 0)
    {
      ret = fd;
      goto out;
    }

  /* 先读驱动：打上 vendor 补丁后 GETCONTRAST 就是亮度百分比（驱动在关屏时
   * 回 0），这是驱动侧的真实状态。没打补丁的树它会回 -ENOSYS(-9)，落到下面
   * 两级兜底。 */

  contrast = -1;
  if (ioctl(fd, LCDDEVIO_GETCONTRAST, (unsigned long)&contrast) == 0 &&
      contrast >= BACKLIGHT_MIN_PERCENT && contrast <= BACKLIGHT_MAX_PERCENT)
    {
      ret = contrast;
      goto out;
    }

  /* 兜底 1：模块内的“最近一次成功设定值”。
   * **注意是缓存值**，不代表驱动侧真实状态（老驱动没有亮度回读通路）。 */

  if (g_bl_last >= 0)
    {
      ret = g_bl_last;
      goto out;
    }

  /* 兜底 2：一次都还没设过，退回 GETPOWER 归一化成 0 / 100 —— 老树只有
   * 这两档，至少把“屏幕现在亮不亮”如实报出来。 */

  contrast = 0;
  if (ioctl(fd, LCDDEVIO_GETPOWER, (unsigned long)&contrast) < 0)
    {
      int errcode = errno;

      syslog(LOG_ERR, "BACKLIGHT: GETPOWER failed: %d\n", errcode);
      ret = -errcode;
      goto out;
    }

  ret = (contrast > 0) ? BACKLIGHT_MAX_PERCENT : BACKLIGHT_MIN_PERCENT;

out:
  nxmutex_unlock(&g_bl_lock);
  return ret;
}
