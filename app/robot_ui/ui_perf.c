/**
 * ui_perf.c - 刷屏/渲染耗时仪表的实现（说明见 ui_perf.h 开头）
 *
 * 为什么用 LVGL 的显示事件（FLUSH_START/FLUSH_FINISH）而不是去包 lv_nuttx 的
 * flush_cb：那个回调在 openvela 的 lvgl 包里（apps/graphics/lvgl/...），改它等于
 * 改公共组件；而 LVGL 9.1 在调 flush_cb 的前后正好各发一个事件
 * （lv_refr.c 的 call_flush_cb），从应用层挂事件拿到的就是同一段时间，还更抗上游改动。
 *
 * 为什么不会把控制台冲爆：
 *   - flush 那条只在"超过 UI_PERF_SLOW_MS"时打（正常刷新 30 Hz，一秒几十次，一律不吭声）；
 *   - 汇总那条一秒最多一行，而且只有"这一秒出现过慢"才打，安静时一个字都不打。
 */

#include "ui_perf.h"

#if UI_PERF_LOG

#include <nuttx/config.h>
#include <nuttx/arch.h>     /* up_interrupt_context() */
#include <nuttx/clock.h>    /* clock_systime_ticks() / TICK2MSEC() / MSEC_PER_TICK */
#include <stdio.h>
#include <stdint.h>

/* ---- 单次 flush（FLUSH_START -> FLUSH_FINISH 之间的时间） ---- */
static uint32_t  g_flush_tick;     /* FLUSH_START 时刻的 tick */
static lv_area_t g_flush_area;     /* FLUSH_START 报的脏区（= 驱动真正推的那块） */
static uint32_t  g_slow_total;     /* 上电以来慢 flush 的次数（日志里那个"累计"） */

/* ---- 本次 lv_timer_handler（一帧的渲染 + 刷屏） ---- */
static uint32_t  g_frame_tick;

/* ---- 本统计秒 ---- */
static uint32_t  g_sec_tick;         /* 本秒起点 */
static uint32_t  g_sec_flush_cnt;    /* 本秒 flush 次数 */
static uint32_t  g_sec_flush_max;    /* 本秒最慢的一次 flush */
static uint32_t  g_sec_flush_slow;   /* 本秒超阈值的 flush 次数 */
static uint32_t  g_sec_handler_max;  /* 本秒 lv_timer_handler 单次最坏 */

/* 单调时钟 + 转换成毫秒。刻度就是 tick（本工程 10 ms），差值不会超过 tick 回绕范围。 */
static uint32_t ui_perf_now(void)
{
    return (uint32_t)clock_systime_ticks();
}

static uint32_t ui_perf_ms_since(uint32_t t0)
{
    return (uint32_t)TICK2MSEC(ui_perf_now() - t0);
}

/* LV_EVENT_FLUSH_START / LV_EVENT_FLUSH_FINISH 共用一个回调：
 * 事件参数是 lv_area_t*（LVGL 已经加上显示偏移，就是交给 flush_cb 的那块区域）。 */
static void ui_perf_flush_evt(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_FLUSH_START)
    {
        const lv_area_t * area = (const lv_area_t *)lv_event_get_param(e);

        g_flush_tick = ui_perf_now();
        if (area != NULL)
        {
            g_flush_area = *area;
        }
        else
        {
            g_flush_area.x1 = 0;
            g_flush_area.y1 = 0;
            g_flush_area.x2 = -1;
            g_flush_area.y2 = -1;
        }
        return;
    }

    {
        uint32_t ms = ui_perf_ms_since(g_flush_tick);
        int32_t  w  = lv_area_get_width(&g_flush_area);
        int32_t  h  = lv_area_get_height(&g_flush_area);

        g_sec_flush_cnt++;
        if (ms > g_sec_flush_max)
        {
            g_sec_flush_max = ms;
        }

        if (ms >= UI_PERF_SLOW_MS)
        {
            g_sec_flush_slow++;
            g_slow_total++;

            /* flush 是在 LVGL 线程（任务上下文）里跑的，正常可以直接 printf；
             * 保险起见中断上下文里只计数、不打日志，交给主循环的汇总去打。 */
            if (!up_interrupt_context())
            {
                printf("[ui] flush 慢: %u ms, 区域 %dx%d, 累计慢次数 %u\n",
                       (unsigned)ms, (int)w, (int)h, (unsigned)g_slow_total);
            }
        }
    }
}

void ui_perf_attach_display(lv_display_t * disp)
{
    if (disp == NULL)
    {
        printf("[ui] 仪表没挂上：disp 是 NULL\n");
        return;
    }

    lv_display_add_event_cb(disp, ui_perf_flush_evt, LV_EVENT_FLUSH_START, NULL);
    lv_display_add_event_cb(disp, ui_perf_flush_evt, LV_EVENT_FLUSH_FINISH, NULL);

    g_sec_tick = ui_perf_now();
    /* 开机只打这一行：它在串口里出现，就说明仪表确实装上了。
     * 没看到这行 = UI_PERF_LOG 关了或者 disp 没创建成功，别去猜"为什么一直没日志"。 */
    printf("[ui] 慢速仪表已挂上: 阈值 %u ms, tick 精度 %u ms\n",
           (unsigned)UI_PERF_SLOW_MS, (unsigned)MSEC_PER_TICK);
}

void ui_perf_frame_begin(void)
{
    g_frame_tick = ui_perf_now();
}

void ui_perf_frame_end(void)
{
    uint32_t ms = ui_perf_ms_since(g_frame_tick);

    if (ms > g_sec_handler_max)
    {
        g_sec_handler_max = ms;
    }

    if (ui_perf_ms_since(g_sec_tick) < 1000)
    {
        return;
    }

    /* 这一秒里出现过"慢"才打一行；一切正常（不慢、也没在刷屏）时保持安静。
     *
     * ⚠️ 末尾那个 up= 是**系统开机以来的秒数**（`clock_systime_ticks()`，不是本
     * 模块自己的计时），2026-09-20 加的：真机上出现过"跑着跑着整台机器重新初始
     * 化一遍、而串口里一行原因都没有"，当时只能靠"两次冷启动的堆水位差 8 字节"
     * 去推断到底有没有复位。有了 up= —— **它归零的那一秒就是复位发生的那一秒**，
     * 而且这条每秒都在打，不用等抓取刚好盖住复位那一刻。
     * 判读：up 一直涨 = 没复位；up 突然跳回个位数 = 整机重启过一次。 */
    if (g_sec_flush_slow > 0 || g_sec_handler_max >= UI_PERF_SLOW_MS)
    {
        printf("[ui] 慢统计 1s: flush %u 次, 最慢 flush %u ms, lv_timer_handler 最慢 %u ms, 慢 flush %u 次, up=%u s\n",
               (unsigned)g_sec_flush_cnt, (unsigned)g_sec_flush_max,
               (unsigned)g_sec_handler_max, (unsigned)g_sec_flush_slow,
               (unsigned)(TICK2MSEC(clock_systime_ticks()) / 1000u));
    }

    g_sec_flush_cnt   = 0;
    g_sec_flush_max   = 0;
    g_sec_flush_slow  = 0;
    g_sec_handler_max = 0;
    g_sec_tick        = ui_perf_now();
}

#endif /* UI_PERF_LOG */
