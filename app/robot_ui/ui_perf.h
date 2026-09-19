/**
 * ui_perf.h - 刷屏/渲染耗时仪表（"只在慢的时候才说话"）
 *
 * 干什么：回答"断电重启后第一次点『提醒』/『主菜单』整屏黑，黑多久、
 * 黑在哪一段"。两块数据：
 *   ① 每次 flush（把一块脏区推给 LCD 驱动）耗时多少、区域多大；
 *   ② 每轮 lv_timer_handler()（一帧的"渲染 + 刷屏"总时间）最坏多少。
 * 只看这两个数就能分开两类病因：
 *   - flush 慢（几十~200 ms）→ 卡在 LCD 那一侧（QSPI 推屏 / DMA 等待）；
 *   - flush 不慢但 lv_timer_handler 慢 → 卡在画字那一侧（XIP flash 里读字形位图）。
 *
 * 怎么接：main.c 里两处、各 3 行，都在 #if UI_PERF_LOG 里（见那边的注释）。
 *
 * ⚠️ 计时精度只有 10 ms：本工程 CONFIG_USEC_PER_TICK=10000（100 Hz tick），
 * clock_systime_ticks() 的最小刻度就是 10 ms，所以日志里的毫秒数都是 10 的倍数，
 * UI_PERF_SLOW_MS=30 实际含义是"≥30 ms"（29 ms 会被算成 20 或 30，看不出来）。
 * 判断"几十毫秒"级别的问题够用；要看更细的得换高精度定时器，本版本没有。
 */
#ifndef __APP_ROBOT_UI_UI_PERF_H
#define __APP_ROBOT_UI_UI_PERF_H

/* ==================== 总开关（以后只改这一行） ====================
 * 1 = 装仪表；0 = 一字不差地回到没装之前：
 *     - ui_perf.c 整个编译成空翻译单元（里面每一行都在 #if 里）；
 *     - main.c 里那两处调用也被 #if 掉，不留未使用变量、不留日志、不改刷新行为。
 */
#define UI_PERF_LOG 1

/* "慢"的门槛（毫秒）。flush 超过它才打一行，也是主循环汇总的触发线。 */
#define UI_PERF_SLOW_MS 30

#if UI_PERF_LOG

#include <lvgl/lvgl.h>

/* 把仪表挂到显示设备上：内部只加两个 LVGL 显示事件回调
 * （LV_EVENT_FLUSH_START / LV_EVENT_FLUSH_FINISH，LVGL 在调 flush_cb 的前后各发一次），
 * 不改 flush 回调本身，所以不碰 lv_nuttx 那层驱动。lv_nuttx_init() 之后调一次。 */
void ui_perf_attach_display(lv_display_t * disp);

/* 把主循环里的 lv_timer_handler() 夹在中间。 */
void ui_perf_frame_begin(void);
void ui_perf_frame_end(void);

#endif /* UI_PERF_LOG */

#endif /* __APP_ROBOT_UI_UI_PERF_H */
