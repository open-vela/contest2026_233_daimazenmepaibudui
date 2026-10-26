/**
 * ui_async.c - ui_async_call() 的实现（背景见 ui_async.h 开头的"为什么"）
 *
 * 2026-10-15 重写：队列化，彻底关掉"插入 vs 遍历"那半边竞态
 * ------------------------------------------------------------
 * 旧实现用互斥锁收口 lv_async_call()，只解决了"多个投递线程互相踩"；
 * 投递线程与 LVGL 线程 lv_timer_handler() 遍历定时器链表的竞态还在
 * （ui_async.h 的"残余风险"一节写过）。2026-10-15 晚上它真的炸了：
 * 对时成功后关怀模块一口气触发 7 个任务 + MQTT voice_state 投递 +
 * ASR 的 ECDH 大数运算拖慢 LVGL 线程 → 链表插坏 → robot_ui 跳飞
 * （Usage Fault: Invalid state，整板 Panic，屏幕永远停在"正在想"）。
 *
 * 现在的做法（就是当年头文件里开的方子）：
 *   - ui_async_call()：不再碰 LVGL 的任何东西，只把 {cb, arg} 塞进
 *     我们自己的互斥锁 FIFO；
 *   - ui_async_pump()：**只许 LVGL 线程**在主循环里调，把队列里攒的
 *     回调逐个取出来执行。
 * LVGL 的定时器链表从此只有 LVGL 线程一个访客，竞态从根上消失。
 *
 * 语义与旧版一致的部分：
 *   - 回调在 LVGL 线程跑，里面可以直接碰控件、不许阻塞；
 *   - 投递顺序 = 执行顺序（FIFO）；
 *   - 不许在中断上下文调（要取锁 + malloc）；
 *   - 投递失败返回 LV_RESULT_INVALID，调用方自己释放 arg。
 * 新增语义：
 *   - 回调里可以再调 ui_async_call()（泵在锁外执行回调，不会自锁；
 *     新投的会被同一趟泵继续取走）；
 *   - 从投递到执行的延迟 = 到主循环下一次泵的间隔（~一帧），
 *     与旧实现（0ms 定时器在下一次 lv_timer_handler 里触发）相同量级。
 */

#include "ui_async.h"

#include <pthread.h>
#include <stdlib.h>

typedef struct ui_async_node_s
{
    lv_async_cb_t          cb;
    void                  *arg;
    struct ui_async_node_s *next;
} ui_async_node_t;

static pthread_mutex_t g_ui_async_lock = PTHREAD_MUTEX_INITIALIZER;
static ui_async_node_t *g_ui_async_head = NULL;
static ui_async_node_t *g_ui_async_tail = NULL;

lv_result_t ui_async_call(lv_async_cb_t cb, void *arg)
{
    ui_async_node_t *node;

    if (cb == NULL)
    {
        return LV_RESULT_INVALID;
    }

    /* malloc 放在锁外：慢事不进临界区（和旧版 lv_async_call 的习惯一致） */
    node = malloc(sizeof(*node));
    if (node == NULL)
    {
        return LV_RESULT_INVALID;
    }

    node->cb   = cb;
    node->arg  = arg;
    node->next = NULL;

    pthread_mutex_lock(&g_ui_async_lock);
    if (g_ui_async_tail == NULL)
    {
        g_ui_async_head = node;
    }
    else
    {
        g_ui_async_tail->next = node;
    }
    g_ui_async_tail = node;
    pthread_mutex_unlock(&g_ui_async_lock);

    return LV_RESULT_OK;
}

void ui_async_pump(void)
{
    /* 每趟最多取 64 个：防止某个回调疯狂再投递把这一帧饿死。
     * 剩下的下一圈主循环接着取，不影响顺序。 */
    int budget = 64;

    while (budget-- > 0)
    {
        ui_async_node_t *node;

        pthread_mutex_lock(&g_ui_async_lock);
        node = g_ui_async_head;
        if (node != NULL)
        {
            g_ui_async_head = node->next;
            if (g_ui_async_head == NULL)
            {
                g_ui_async_tail = NULL;
            }
        }
        pthread_mutex_unlock(&g_ui_async_lock);

        if (node == NULL)
        {
            break;
        }

        /* 锁外执行：回调里可以再投递（不会死锁），也可以碰控件（本线程就是 LVGL 线程） */
        node->cb(node->arg);
        free(node);
    }
}
