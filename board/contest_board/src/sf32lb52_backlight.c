/****************************************************************************
 * board/contest_board/src/sf32lb52_backlight.c
 *
 * SF32LB52 屏幕亮度封装：backlight_set() / backlight_get()
 *
 * 把 /dev/lcd0 包成"0..100 亮度"：
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
 * ---------------------------------------------------------------------------
 * 为什么不缓存 fd（2026-09-13 踩的坑，别再改回去）
 * ---------------------------------------------------------------------------
 * 这里原来用一个模块级 `static int` 缓存 open 出来的 fd，靠一把 nxmutex 保护，
 * 想在"第一次调用"之后复用。**在 NuttX 上这是错的**：fd 属于 task group，
 * 而用它的代码分属不同任务 —— NSH 里每条命令是一个独立任务，robot_ui 又是
 * 另一个任务。A 任务 open 出来的 fd 数字缓存进全局变量，B 任务拿同一个数字去
 * ioctl，拿到的是 EBADF（errno 9）；更糟的情况是那个数字在 B 的 fd 表里恰好
 * 是别的文件，于是对错误的设备下手。
 *
 * 现场现象：`hw_test lcd 30` 单独跑 PASS（同一次调用里 open + ioctl），紧接着
 * 再跑 `hw_test lcd 60` 就报 `BACKLIGHT: SETCONTRAST(60) failed: 9`；而且
 * `设置前` 打印的是 30 —— 那已经不是驱动回读，是模块内缓存的旧值。
 *
 * 所以现在**每次调用自己 open、发完 ioctl 立刻 close，不保存任何 fd**。
 * 代价是每次多一对 open/close（`lcddev_open` 只在 crefs 0->1 时动作，本驱动
 * 也没实现 `dev.open`，等于什么都不做），换来的是任何任务里都能用。
 * 同理，模块里**不再有** `g_bl_fd` / `g_bl_on` / `g_bl_last` 这类跨任务的
 * 状态缓存：跨任务缓存本身是上面那个 bug 的同源问题。
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
#include <errno.h>
#include <syslog.h>

#include <nuttx/lcd/lcd_dev.h>

#include "sf32lb52_backlight.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 面板"全亮"时下发给 SETPOWER 的 power 值。宏由 Kconfig 给（本板 defconfig 里
 * CONFIG_LCD_MAXPOWER=100），这里兜个底，免得配置不带 LCD 时编不过。 */

#ifndef CONFIG_LCD_MAXPOWER
#  define CONFIG_LCD_MAXPOWER 100
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: backlight_setpower
 *
 * Description:
 *   开 / 关屏：power == 0 关，power > 0 开。这是 SETPOWER 的本来语义
 *   （`sf32lb_lcd_setpower()` 就是 `power > 0 ? DisplayOn : DisplayOff`）。
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

  fd = open(BACKLIGHT_DEV, O_RDWR);
  if (fd < 0)
    {
      int errcode = errno;

      syslog(LOG_ERR, "BACKLIGHT: open %s failed: %d\n",
             BACKLIGHT_DEV, errcode);
      return -errcode;
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
        }

      goto out;
    }

  /* 1..100：先下亮度，走面板的亮度寄存器 */

  ret = backlight_setcontrast(fd, percent);
  if (ret < 0)
    {
      /* 未打补丁的树上 SETCONTRAST 回 -ENOSYS，1..99 兑现不了，如实上报。
       * 但 100% 不算失败：SETPOWER(>0) 本来就是"全亮"，下面那一步就能兑现，
       * 所以保留 100 在没有补丁的树上也可靠。 */

      if (!(percent == BACKLIGHT_MAX_PERCENT && ret == -ENOSYS))
        {
          goto out;
        }
    }

  /* 关屏时（不管是自己刚设的还是别人设的）确保点亮。
   * 不缓存"屏幕已经亮着"的判断 —— 那是跨任务的过期状态，宁可多发一次
   * SETPOWER(CONFIG_LCD_MAXPOWER)。 */

  ret = backlight_setpower(fd, CONFIG_LCD_MAXPOWER);

out:
  close(fd);
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

  fd = open(BACKLIGHT_DEV, O_RDWR);
  if (fd < 0)
    {
      int errcode = errno;

      syslog(LOG_ERR, "BACKLIGHT: open %s failed: %d\n",
             BACKLIGHT_DEV, errcode);
      return -errcode;
    }

  /* 先读驱动：打上 vendor 补丁后 GETCONTRAST 就是亮度百分比（驱动在关屏时
   * 回 0），这是驱动侧的真实状态。没打补丁的树它会回 -ENOSYS，落到下面的
   * GETPOWER 兜底。 */

  contrast = -1;
  if (ioctl(fd, LCDDEVIO_GETCONTRAST, (unsigned long)&contrast) == 0 &&
      contrast >= BACKLIGHT_MIN_PERCENT && contrast <= BACKLIGHT_MAX_PERCENT)
    {
      ret = contrast;
      goto out;
    }

  /* 兜底：老驱动没有亮度回读通路，退回 GETPOWER 归一化成 0 / 100 ——
   * 那两档是真的，至少把"屏幕现在亮不亮"如实报出来。 */

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
  close(fd);
  return ret;
}
