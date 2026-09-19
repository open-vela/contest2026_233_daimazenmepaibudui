/**
 * ui_async.c - ui_async_call() 的实现（背景和残余风险写在 ui_async.h 里）
 *
 * 两个实现上的选择，说明一下为什么：
 *   - 单独一个文件：让"往 LVGL 投递"只有这一处实现，谁想绕过它都得先看见它；
 *   - 锁用 PTHREAD_MUTEX_INITIALIZER 静态初始化，而不是自己写一个 ui_async_init()：
 *     不依赖初始化顺序。main.c 里 robot_ui_bridge_set_ready(true) 一放行，
 *     hello_app 那边随时可能投过来，而那时没有任何注册步骤保证 init 已经跑过。
 *     同样的写法见 ambient_listen.c 的 g_lock。
 *
 * 锁的临界区只有"一次 lv_async_call"那么长：一次 malloc + 一次链表插入，
 * 中间没有阻塞点、没有回调、也不是线程取消点，所以不会拖住 LVGL 线程。
 */

#include "ui_async.h"

#include <pthread.h>

static pthread_mutex_t g_ui_async_lock = PTHREAD_MUTEX_INITIALIZER;

lv_result_t ui_async_call(lv_async_cb_t cb, void *arg)
{
    lv_result_t ret;

    pthread_mutex_lock(&g_ui_async_lock);
    ret = lv_async_call(cb, arg);
    pthread_mutex_unlock(&g_ui_async_lock);

    return ret;
}
