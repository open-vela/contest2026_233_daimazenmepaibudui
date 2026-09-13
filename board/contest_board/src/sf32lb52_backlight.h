/****************************************************************************
 * board/contest_board/src/sf32lb52_backlight.h
 *
 * SF32LB52 屏幕亮度封装
 *
 * 只暴露两个函数：backlight_set(percent) / backlight_get()。
 * 内部自己 open("/dev/lcd0") + ioctl(LCDDEVIO_SETPOWER / LCDDEVIO_GETPOWER)，
 * 用一把 nxmutex 保护 fd 和当前值，对 percent 做范围校验。
 *
 * ---------------------------------------------------------------------------
 * **本板的真实能力（先看这段再决定怎么用）**
 * ---------------------------------------------------------------------------
 * /dev/lcd0 这套 NuttX LCD ioctl 在本板上**只有"开屏/关屏"两档**，没有中间亮度：
 *
 *   - LCDDEVIO_SETPOWER  -> sf32lb_lcd_setpower()
 *       vendor/sifli/boards/sf32lb52/drivers/lcd/sf32lb_lcd.c:634
 *       实现是 `power > 0 ? DisplayOn : DisplayOff`；power 的**数值本身只被存下来
 *       给 GETPOWER 回读，不影响亮度**。所以传 30 和传 100 出来的亮度一模一样。
 *   - LCDDEVIO_SETCONTRAST -> sf32lb_lcd_setcontrast()
 *       同文件 :697，直接 `return -ENOSYS`，是死的。
 *
 * 因此本封装如实做到：
 *
 *   backlight_set(0)     -> 真的把面板关掉（最暗），返回 OK
 *   backlight_set(100)   -> 面板全亮，返回 OK
 *   backlight_set(1..99) -> **返回 -ENOSYS，且不碰硬件**
 *
 * 中间值故意**不"假装成功"**：那只会让调用方以为自己调到了半亮，
 * 现场排查时反而更难。要真正的百分比亮度，得先补驱动（见下）。
 *
 * ---------------------------------------------------------------------------
 * 真正可用的百分比亮度在哪（两条路，本树都还没走通）
 * ---------------------------------------------------------------------------
 * 1. **面板自己的亮度寄存器**（推荐）：CO5300 的 0x51 `WBRIGHT`，SDK 里已经写好，
 *    见 `vendor/sifli/boards/sf32lb52/drivers/lcd/co5300.c:544` 的
 *    `LCD_SetBrightness()`（入参就是百分比，内部换算成 0..255）。
 *    问题是它只挂在 `LCD_DrvOpsDef.SetBrightness` 上，而 `sf32lb_lcd.c`
 *    **从来不调这个回调**，也没有任何 ioctl 暴露它 —— 所以 /dev/lcd0 够不着。
 * 2. **背光 PWM**：PA01 被 pinmux 成 `GPTIM1_CH4`（`bsp_pinmux.c:227`），
 *    理论上能调亮度；但 `CONFIG_PWM` 没开（`/dev/pwm0` 不存在），
 *    而且现在**没有任何代码去配置 GPTIM1 的占空比**，这条线是空的。
 *
 * 结论：需要真正的百分比亮度时，请先在上面两条路里挑一条补驱动，
 * 再把 backlight_set() 中间值的分支改成走新接口。详见
 * docs/display_touch_gpio_usage.md 第 3.3 节。
 *
 ****************************************************************************/

#ifndef __BOARDS_CONTEST_BOARD_SRC_SF32LB52_BACKLIGHT_H
#define __BOARDS_CONTEST_BOARD_SRC_SF32LB52_BACKLIGHT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/compiler.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 亮度范围（百分比），0 = 最暗，100 = 最亮 */

#define BACKLIGHT_MIN_PERCENT 0
#define BACKLIGHT_MAX_PERCENT 100

/* 设备节点：CO5300 屏的 NuttX LCD dev */

#define BACKLIGHT_DEV         "/dev/lcd0"

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: backlight_set
 *
 * Description:
 *   设置屏幕亮度，0..100（0 = 最暗，100 = 最亮）。
 *
 *   本板**只支持 0 和 100 两个值**（见文件头的说明：SETPOWER 只有开/关两档）：
 *     - 0   -> ioctl(LCDDEVIO_SETPOWER, 0)，面板 DisplayOff，真的变最暗；
 *     - 100 -> ioctl(LCDDEVIO_SETPOWER, CONFIG_LCD_MAXPOWER)，面板全亮；
 *     - 1..99 -> 返回 -ENOSYS，**不动硬件**（不假装成功）。
 *
 *   设备节点打不开时返回负 errno。第一次调用会 open("/dev/lcd0")，
 *   之后复用同一个 fd；fd 和当前值都由模块内的 nxmutex 保护，可多任务调用。
 *
 * Input Parameters:
 *   percent - 目标亮度，0..100
 *
 * Returned Value:
 *   OK on success; 参数越界返回 -EINVAL；本板兑现不了的值返回 -ENOSYS；
 *   open / ioctl 失败返回负 errno。
 *
 ****************************************************************************/

int backlight_set(int percent);

/****************************************************************************
 * Name: backlight_get
 *
 * Description:
 *   读当前亮度（0..100）。内部是 ioctl(LCDDEVIO_GETPOWER) 后归一化：
 *   power > 0 记 100，power == 0 记 0（因为本板只有这两档）。
 *
 *   注意这是**驱动侧的真实状态**，不是"上次 set 了多少"。
 *
 * Returned Value:
 *   0..100；模块打不开设备或不支持时返回负 errno（例如 -ENODEV）。
 *
 ****************************************************************************/

int backlight_get(void);

#endif /* __BOARDS_CONTEST_BOARD_SRC_SF32LB52_BACKLIGHT_H */
