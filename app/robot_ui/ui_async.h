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
 * 这一层做什么
 * ------------
 * 把全工程所有跨线程投递收口到 ui_async_call()，内部用一把进程内互斥锁把
 * lv_async_call() 包起来。效果是"任意时刻最多只有一个线程在动那张链表"，
 * 把原来的 N 路并发插降成 1 路。
 *
 * 残余风险（必须知道，别以为加了锁就干净了）
 * ------------------------------------------
 * 锁只保护**投递**。LVGL 线程里的 lv_timer_handler() 走的是它自己的遍历，
 * 我们不（也不能）在那里加锁：handler 外面一旦套锁，handler 里跑的用户回调
 * 只要再调一次 ui_async_call 就会自己锁死自己（本工程 ui_apply_msg 这类回调
 * 确实有再投递的可能），而 LVGL 线程一卡就是整机没界面。
 * 所以"插入 vs 遍历"剩下那半边竞争仍然存在，只是窗口从"N 个线程随时可能在插"
 * 缩到"1 个线程在插"。
 * 要彻底关掉得改 LVGL 侧的做法（比如投递只写我们自己的队列，由 LVGL 线程的
 * 定时器去取），那是比这次更大的改动，先按现状收口。
 *
 * 用法约定
 * -------
 * 1) 任何**非 LVGL 线程**要改界面，都走 ui_post_* / touch_ui_*，它们的最后一跳
 *    统一是这里；不要在新代码里直接调 lv_async_call()。
 * 2) 回调 cb 一定在 LVGL 线程里跑，里面可以直接碰控件，但不许阻塞。
 * 3) **不许在中断上下文调用**：内部要取互斥锁，而 lv_async_call() 还要 malloc，
 *    这两件事在中断里都不能做（robot_ui_bridge.c 里那道
 *    up_interrupt_context() 检查就是拦这个的）。
 * 4) 同一个线程连续投递时，投递顺序就是执行顺序，所以"先弹面板、再写文字"
 *    这类依赖顺序的写法照样成立。
 */

#ifndef __APP_ROBOT_UI_UI_ASYNC_H
#define __APP_ROBOT_UI_UI_ASYNC_H

#include <lvgl.h>

/* 加锁版的 lv_async_call()：把 cb 连同 arg 投到 LVGL 线程去执行。
 * 返回值与 lv_async_call() 一致：LV_RESULT_OK 表示已经挂进定时器链表，
 * LV_RESULT_INVALID 表示投递失败（通常是内存不够）—— 这时调用方自己
 * 负责把 arg 释放掉（原来的调用点就是这么做的）。 */
lv_result_t ui_async_call(lv_async_cb_t cb, void *arg);

#endif /* __APP_ROBOT_UI_UI_ASYNC_H */
