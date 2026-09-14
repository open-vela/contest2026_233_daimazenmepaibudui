/****************************************************************************
 * board/contest_board/src/sf32lb52_backlight.h
 *
 * SF32LB52 屏幕亮度封装
 *
 * 只暴露两个函数：backlight_set(percent) / backlight_get()。
 * 内部自己 open("/dev/lcd0") + ioctl + close，**不缓存 fd、不保存任何跨任务
 * 状态**（原因见 .c 文件头"为什么不缓存 fd"），对 percent 做范围校验。
 *
 * ---------------------------------------------------------------------------
 * 怎么调亮度（先看这段再决定怎么用）
 * ---------------------------------------------------------------------------
 *   backlight_set(0)     -> LCDDEVIO_SETPOWER(0)，面板 DisplayOff，真的最暗，返回 OK
 *   backlight_set(100)   -> LCDDEVIO_SETCONTRAST(100) + 确保屏幕点亮，返回 OK
 *   backlight_set(1..99) -> LCDDEVIO_SETCONTRAST(percent)，走面板亮度寄存器
 *   backlight_get()      -> 驱动侧回读（关闭屏时驱动报 0），0..100；负值是负 errno
 *
 * 百分比亮度走的是面板自己的 0x51 WBRIGHT 寄存器（CO5300 的
 * `LCD_SetBrightness()`，`vendor/sifli/boards/sf32lb52/drivers/lcd/co5300.c:544`，
 * 入参就是百分比），链路是：
 *
 *   应用 -> backlight_set() -> ioctl(LCDDEVIO_SETCONTRAST)
 *        -> sf32lb_lcd_setcontrast() -> p_ops->SetBrightness() -> 0x51 WBRIGHT
 *
 * 其中 `sf32lb_lcd.c` 那两跳（把 SETCONTRAST 接到 `SetBrightness`）是本仓库的
 * 补丁 `patches/vendor_sifli-lcd-brightness.patch`。**所以这套封装依赖那个补丁**：
 *
 *   - 打了补丁的树：0..100 全档可用；
 *   - 没打补丁的树：SETCONTRAST 是 `-ENOSYS`，只有 0 / 100 是真的，
 *     `backlight_set(1..99)` 如实返回 `-ENOSYS` 且不碰硬件（不假装成功）。
 *
 * 为什么必须这样：`LCDDEVIO_SETPOWER` 在本板只有开/关两档
 * （`sf32lb_lcd.c` 的 `sf32lb_lcd_setpower()`：`power > 0 ? DisplayOn : DisplayOff`，
 * power 的数值只被存下来给 GETPOWER 回读，不影响亮度）。所以传 30 和传 100
 * 出来一样亮 —— 那是“假亮度”，本封装不拿它冒充百分比。
 *
 * 背光 PWM（PA01 = GPTIM1_CH4）这条线仍然是空的：`CONFIG_PWM` 没开
 * （`/dev/pwm0` 不存在），也没有代码配 GPTIM1 占空比；不需要动。
 *
 * 细节和排错见 docs/display_touch_gpio_usage.md 第 3.3 节。
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
 *     - 0   -> ioctl(LCDDEVIO_SETPOWER, 0)，面板 DisplayOff，真的变最暗；
 *     - 1..99 -> ioctl(LCDDEVIO_SETCONTRAST, percent)，面板 0x51 WBRIGHT；
 *     - 100 -> 同 1..99，再确保屏幕是亮的（SETPOWER 全功率）。
 *
 *   中间值依赖 vendor 补丁（见文件头）：没打补丁的树上 SETCONTRAST 回
 *   `-ENOSYS`，这时 1..99 如实返回 `-ENOSYS` 且不碰硬件；0 和 100 不受影响
 *   （0 走 SETPOWER 关屏，100 靠 SETPOWER 全功率兜住）。
 *
 *   设备节点打不开时返回负 errno。每次调用自己 open("/dev/lcd0")、用完
 *   close，**不缓存 fd**（见 .c 文件头），所以任何任务里都能调，包括 NSH
 *   命令、robot_ui(LVGL) 任务同时用。
 *
 * Input Parameters:
 *   percent - 目标亮度，0..100
 *
 * Returned Value:
 *   OK on success; 参数越界返回 -EINVAL；本树兑现不了的值返回 -ENOSYS；
 *   open / ioctl 失败返回负 errno。
 *
 ****************************************************************************/

int backlight_set(int percent);

/****************************************************************************
 * Name: backlight_get
 *
 * Description:
 *   读当前亮度（0..100），只信驱动：
 *
 *   1. `ioctl(LCDDEVIO_GETCONTRAST)` —— 打了 vendor 补丁后这就是驱动侧的
 *      真实亮度（驱动在关屏时回 0）；
 *   2. 驱动没有这条通路时退回 `ioctl(LCDDEVIO_GETPOWER)` 归一化成 0 / 100 ——
 *      没打补丁的树只有这两档，至少如实报出“屏幕现在亮不亮”。
 *
 *   **没有"模块内缓存上次设定值"这一层**：缓存跨任务会过期（见 .c 文件头），
 *   报一个自己编的数字比报错更坑。
 *
 * Returned Value:
 *   0..100；两级都拿不到时返回负 errno（例如 -ENODEV）。
 *
 ****************************************************************************/

int backlight_get(void);

#endif /* __BOARDS_CONTEST_BOARD_SRC_SF32LB52_BACKLIGHT_H */
