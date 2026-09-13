/****************************************************************************
 * board/contest_board/src/sf32lb52_backlight.c
 *
 * SF32LB52 屏幕亮度封装：backlight_set() / backlight_get()
 *
 * 只做一件事：把 /dev/lcd0 的 LCDDEVIO_SETPOWER / GETPOWER 包成"0..100 亮度"，
 * 内部拿一把 nxmutex 保护 fd 与当前值，并在锁里完成 open（懒打开、成功后复用）。
 *
 * 本板**只有 0 和 100 两档是真的**（SETPOWER 只切面板 DisplayOn/DisplayOff，
 * SETCONTRAST 是 -ENOSYS），所以 1..99 直接返回 -ENOSYS 且不碰硬件，
 * 不假装成功。原因与"真正亮度在哪"写在 sf32lb52_backlight.h 文件头，
 * 也见 docs/display_touch_gpio_usage.md 第 3.3 节。
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

/* 面板"全亮"时下发的 power 值。宏由 Kconfig 给（本板 defconfig 里
 * CONFIG_LCD_MAXPOWER=100），这里兜个底，免得配置不带 LCD 时编不过。 */

#ifndef CONFIG_LCD_MAXPOWER
#  define CONFIG_LCD_MAXPOWER 100
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 当前打开的 /dev/lcd0 fd，-1 = 还没打开 */

static int g_bl_fd = -1;

/* fd 与"最近一次成功下发"的亮度的保护锁。
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

  /* 最后一步才发布 fd：中途失败时全局状态一直是"没打开" */

  g_bl_fd = fd;
  return fd;
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

  /* 本板的 SETPOWER 只有开/关两档（sf32lb_lcd.c:634 的 sf32lb_lcd_setpower：
   * power > 0 只调面板 DisplayOn、power == 0 只调 DisplayOff，power 的数值
   * 只被存下来给 GETPOWER 回读）。中间值兑现不了，如实返回 -ENOSYS，
   * **不往下发 ioctl**，免得让调用方以为亮度真的变了。 */

  if (percent != BACKLIGHT_MIN_PERCENT && percent != BACKLIGHT_MAX_PERCENT)
    {
      syslog(LOG_WARNING,
             "BACKLIGHT: %d%% unsupported (this board has only 0/100: "
             "LCDDEVIO_SETPOWER is on/off, SETCONTRAST is -ENOSYS)\n",
             percent);
      ret = -ENOSYS;
      goto out;
    }

  /* 0 -> DisplayOff；100 -> DisplayOn（下发 CONFIG_LCD_MAXPOWER 便于回读） */

  if (ioctl(fd, LCDDEVIO_SETPOWER,
            (unsigned long)(percent == 0 ? 0 : CONFIG_LCD_MAXPOWER)) < 0)
    {
      int errcode = errno;

      syslog(LOG_ERR, "BACKLIGHT: SETPOWER(%d) failed: %d\n",
             percent, errcode);
      ret = -errcode;
      goto out;
    }

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
  int power;
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

  power = 0;
  if (ioctl(fd, LCDDEVIO_GETPOWER, (unsigned long)&power) < 0)
    {
      int errcode = errno;

      syslog(LOG_ERR, "BACKLIGHT: GETPOWER failed: %d\n", errcode);
      ret = -errcode;
      goto out;
    }

  /* 本板 power 只可能是 0（关屏）或 CONFIG_LCD_MAXPOWER（开屏），
   * 归一化成 0 / 100 返回。 */

  ret = (power > 0) ? BACKLIGHT_MAX_PERCENT : BACKLIGHT_MIN_PERCENT;

out:
  nxmutex_unlock(&g_bl_lock);
  return ret;
}
