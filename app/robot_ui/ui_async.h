/**
 * ui_async.h - 跨线程"改界面"的唯一投递口（把 lv_async_call 收口加锁）
 *
 * 为什么要这一层
 * --------------
 * LVGL 9.1 的 lv_async_call() 干的事就是 lv_timer_create()：往一张全局定时器
 * 链表上插一个 0ms、只跑一次的节点。而 lv_timer.c 里那次插入是"先把节点挂进
 * 链表、再回头填 period / timer_cb / last_run"，**全程一把锁都没有**
 * （v9.1.0 的 lv_timer.c 里连 lv_lock() 都没调用过；CONFIG_LV_USE_OS 也救不了
 * 这个，那个开关只被 VGLite 之类的外设渲染线程用到，跟 timer 链表无关）。
 *
 * 本工程往这张链表上插节点的却有 5 类线程：robot_ui 的 ui_post_* 系列、
 * touch_ui_* 系列、RTC 提醒工作线程、播报线程、hello_app 的录音/语音线程、
 * network_task。两个线程同时插就会把链表插坏；LVGL 线程正巧遍历到那个
 * "挂进去了、但 timer_cb 还是垃圾"的节点，就会跳到非法地址 —— 现场表现就是
 * 整机硬故障 + 串口彻底静默（2026-09-14 的两次卡死都发生在跨线程投递最密集
 * 的时刻）。
 *
 * 这一层做什么（2026-10-15 重写：队列化，竞态已根治）
 * ----------------------------------------------------
 * ui_async_call() 完全不碰 LVGL：把 {cb, arg} 塞进我们自己的互斥锁 FIFO；
 * UI 主循环每圈调一次 ui_async_pump()，在 LVGL 线程里把队列取干。
 * LVGL 的定时器链表从此只有 LVGL 线程一个访客 —— 旧版"锁只护投递、
 * 插入 vs 遍历仍在"的残余风险（2026-10-15 晚上真炸过一次：对时后关怀
 * 风暴 + MQTT 投递 + ASR ECDH 拖慢消费 → 链表插坏 → robot_ui INVSTATE
 * Panic，屏幕永远停在"正在想"）从根上消失。
 *
 * 用法约定
 * -------
 * 1) 任何**非 LVGL 线程**要改界面，都走 ui_post_* / touch_ui_*，它们的最后一跳
 *    统一是这里；不要在新代码里直接调 lv_async_call()。
 * 2) 回调 cb 一定在 LVGL 线程里跑（ui_async_pump 里执行），里面可以直接碰
 *    控件，但不许阻塞。回调里可以再调 ui_async_call()（不会死锁，新投的
 *    同趟就会被取走）。
 * 3) **不许在中断上下文调用**：内部要取互斥锁 + malloc（robot_ui_bridge.c
 *    里那道 up_interrupt_context() 检查就是拦这个的）。
 * 4) 同一个线程连续投递时，投递顺序就是执行顺序，所以"先弹面板、再写文字"
 *    这类依赖顺序的写法照样成立。
 * 5) ui_async_pump() **只许在 LVGL 线程**（robot_ui 主循环）调，其他线程
 *    调了等于在错误的线程执行回调，会踩回老毛病。
 */

#ifndef __APP_ROBOT_UI_UI_ASYNC_H
#define __APP_ROBOT_UI_UI_ASYNC_H

#include <lvgl.h>

/* 投递：把 cb 连同 arg 排进队列，等 LVGL 线程的 ui_async_pump() 取走执行。
 * 返回 LV_RESULT_OK 表示已入队，LV_RESULT_INVALID 表示投递失败（cb 为空或
 * 内存不够）—— 这时调用方自己负责把 arg 释放掉（原来的调用点就是这么做的）。 */
lv_result_t ui_async_call(lv_async_cb_t cb, void *arg);

/* 消费：把队列里攒的回调逐个取出来执行。**只许在 LVGL 线程调**
 * （robot_ui 主循环每圈一次）；每趟最多取 64 个，剩下的下一圈接着取。 */
void ui_async_pump(void);

#endif /* __APP_ROBOT_UI_UI_ASYNC_H */
