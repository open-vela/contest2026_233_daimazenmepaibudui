/**
 * lcd_mirror_glue.h - 屏幕镜像的"LVGL 侧唯一接口"
 *
 * 为什么单独一个文件（照 ui_perf.h/ui_perf.c 的写法）：整个镜像功能里**只有
 * 这一处**需要 LVGL 类型。板级核心（board/contest_board/src/lcd_mirror.c）
 * 不 include LVGL，nsh 命令（app/hw_test）也不需要；两边都只 include
 * 自给自足的 lcd_mirror.h。这样"镜像"这件事在板级是独立可编的，
 * 拿掉 robot_ui 也不会连累别的地方。
 *
 * 接法：app/robot_ui/main.c 里 lv_nuttx_init() 成功之后调一次
 * lcd_mirror_attach_lvgl_display(lv_result.disp);（就在 ui_perf 那一句旁边）。
 *
 * 它做四件事：
 *   ① 挂 LV_EVENT_FLUSH_START：LVGL 每次把一块脏区交给驱动前，把那块像素
 *      拷进影子帧缓冲（`lv_display_get_buf_active()` 拿到的就是同一块 buffer，
 *      见 lv_refr.c 的 call_flush_cb()）；
 *   ② 起镜像任务（开机自动，受 lcd_mirror.c 的 LCD_MIRROR_AUTOSTART 控制）；
 *   ③ 建一个 500 ms 的 lv_timer，在**LVGL 线程里**处理"请整屏重绘一次"的请求。
 *      这一条是必须的：LVGL 只画脏区，而镜像的影子缓冲在界面起来之前是空的，
 *      不主动整屏重绘一次，客户端上就永远缺那一块。
 *   ④ 建**虚拟 input device**（LV_INDEV_TYPE_POINTER + 自定义 read_cb），
 *      把 PC 上的鼠标当触摸用 —— 本机触摸 IC(FT6146) 已经不应答、/dev/input0
 *      都没了，这是现场唯一能操作界面的路。它**不依赖 /dev/input0**，
 *      将来真触摸回来了两者可以同时用（触摸点不动的时候鼠标照样能点）。
 *
 *      注意 touch_ui.c 那边：右滑手势的回调原来是挂在
 *      `lv_indev_get_next(NULL)`（也就是列表里**第一个**设备）上的，而
 *      lv_indev_create() 是插在表头 —— 加了这只设备之后"第一个"就变成它了。
 *      所以那边改成**遍历所有 pointer 设备**各挂一份，两只都能划出菜单。
 */
#ifndef __APP_ROBOT_UI_LCD_MIRROR_GLUE_H
#define __APP_ROBOT_UI_LCD_MIRROR_GLUE_H

#include <lvgl/lvgl.h>

/* lv_nuttx_init() 之后调一次。disp 为 NULL 时只打印一行、什么都不挂。 */
void lcd_mirror_attach_lvgl_display(lv_display_t *disp);

#endif /* __APP_ROBOT_UI_LCD_MIRROR_GLUE_H */
