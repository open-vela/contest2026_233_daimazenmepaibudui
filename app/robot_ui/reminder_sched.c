/**
 * reminder_sched.c - 提醒软调度（接口与线程约定见 reminder_sched.h）
 *
 * 核心规则（一句话）：
 *   硬件只有一个日循环 alarm 槽，所以"下一条"= 比**上一次响过的时刻**晚的、
 *   最近的启用提醒；一个日循环走完了就回到最早的那条（RTC 模块自己会把它
 *   排到明天）。挂上去之后，到点回调里再把"下一条"挂上，如此往复。
 *
 * 为什么用"上次响过的时刻"当下标（g_cursor_min）而不是"当前时间"：
 *   同一分钟里有两条提醒时（用户就是会这么设），按当前时间算会算出"明天"
 *   从而漏掉第二条；按上一次响过的时刻算，回调里一次把同刻的几条一起报掉，
 *   下一条自然落在后面。
 */

#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <pthread.h>

#include "sf32lb52_rtc_alarm.h"   /* 板级日循环闹钟（board/contest_board/src） */
#include "reminder_sched.h"

#define MINUTES_PER_DAY (24 * 60)

/* ==================== 内部状态（全部由 g_lock 保护） ==================== */
static reminder_item_t g_items[REMINDER_MAX_ITEMS];
static int  g_count = 0;

/* 本日循环里**已经响过的最后一条**（分钟数），-1 = 本轮还没响过。
 * 算"下一条"时用它当下界，见文件头。 */
static int  g_cursor_min = -1;

/* 当前挂在 RTC 上的时刻（分钟数），-1 = 没挂（列表空 / 时间没对过）。 */
static int  g_armed_min = -1;

/* 因为"RTC 时间没对过"而暂时没挂，等 reminder_sched_tick() 补挂 */
static bool g_waiting_time = false;
static bool g_warned_no_time = false;

static reminder_fire_cb_t g_fire_cb = NULL;
static void          *g_fire_arg = NULL;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* ==================== 内部工具 ==================== */

static int item_minutes(const reminder_item_t *it)
{
    return it->hour * 60 + it->min;
}

/* 调用者必须已持有 g_lock：数一下有几条启用中的提醒 */
static int enabled_count_locked(void)
{
    int n = 0;

    for (int i = 0; i < g_count; i++) {
        if (g_items[i].enabled) {
            n++;
        }
    }
    return n;
}

/* 调用者必须已持有 g_lock：找"严格晚于 after 分钟"的最近一条启用提醒，
 * 返回它的分钟数；没有比它更晚的返回 -1（after 传 -1 就是取最早的那条）。 */
static int find_next_locked(int after)
{
    int best = -1;

    for (int i = 0; i < g_count; i++) {
        int m;

        if (!g_items[i].enabled) {
            continue;
        }

        m = item_minutes(&g_items[i]);
        if (m > after && (best < 0 || m < best)) {
            best = m;
        }
    }
    return best;
}

/* 把标题接到 dst 后面，中间用"、"隔开；放不下就截断（宁少不溢出） */
static void append_title(char *dst, size_t cap, const char *title)
{
    /* "、" 的 UTF-8 字节（U+3001，字库里有）。
     * 别写成字符常量 '、'：那是 multi-character constant，编译器会告警。 */
    static const char sep[3] = { (char)0xE3, (char)0x80, (char)0x81 };
    size_t len  = strlen(dst);
    size_t tlen = strlen(title);

    if (len > 0 && len + sizeof(sep) + 1 <= cap) {
        memcpy(dst + len, sep, sizeof(sep));
        len += sizeof(sep);
        dst[len] = '\0';
    }

    if (len + tlen + 1 <= cap) {
        memcpy(dst + len, title, tlen + 1);
    }
}

/* ==================== 到点回调（RTC 模块工作线程上下文） ==================== */

/**
 * 到点了：① 把这一分钟里所有提醒的标题拼起来交给上层（弹窗 / 出声），
 * ② 算下一条并重新挂上。
 *
 * 注意这里**不能阻塞**：拼字符串 + 一次回调（main.c 里只做 lv_async_call）+ 
 * 一次 rtc_alarm_at_daily()（非阻塞，只记状态唤醒工作线程）。
 */
static void reminder_rtc_fired(FAR void *arg)
{
    char titles[REMINDER_TITLE_MAX * 4];
    int  fired_min;
    int  next;
    int  hour = -1;
    int  min = 0;
    reminder_fire_cb_t cb;
    void *cb_arg;

    (void)arg;

    pthread_mutex_lock(&g_lock);

    fired_min = g_armed_min;
    g_armed_min = -1;

    titles[0] = '\0';

    if (fired_min >= 0) {
        /* 这一分钟里的每条都报一遍（同刻多条 = 一次回调里全带上） */
        for (int i = 0; i < g_count; i++) {
            if (g_items[i].enabled && item_minutes(&g_items[i]) == fired_min) {
                append_title(titles, sizeof(titles), g_items[i].title);
            }
        }

        hour = fired_min / 60;
        min  = fired_min % 60;

        /* 游标前移到刚响过的时刻，下一条一定比它晚 */
        g_cursor_min = fired_min;
    }

    next = find_next_locked(g_cursor_min);
    if (next < 0) {
        /* 本轮排完了：回到最早的那条，RTC 模块会把它算到明天 */
        next = find_next_locked(-1);
    }

    if (next >= 0) {
        g_armed_min = next;
    }

    cb     = g_fire_cb;
    cb_arg = g_fire_arg;

    pthread_mutex_unlock(&g_lock);

    /* titles 空 = 这一分钟里已经没有启用的提醒了（刚被删掉 / 陈旧信号）：
     * 那就什么也不弹，只把下一条挂上。 */
    if (hour >= 0 && titles[0] != '\0' && cb != NULL) {
        cb(hour, min, titles, cb_arg);
    }

    if (next >= 0) {
        /* 重挂下一条。这里必须**换一代**（rtc_alarm_at_daily 内部 seq++），
         * 板级模块才会放弃"把这一条再排到明天"的循环、改用新的时刻 —— 
         * 详见 docs/rtc_alarm_usage.md 的"只有一个槽"和 rtc_alarm_run()。 */
        if (rtc_alarm_at_daily(next / 60, next % 60, reminder_rtc_fired, NULL) < 0) {
            printf("[Reminder] 重挂 %02d:%02d 失败，下一条要等下次 reload\n",
                   next / 60, next % 60);
        }
    } else {
        printf("[Reminder] 没有启用的提醒了，不再挂 RTC 闹钟\n");
    }
}

/* ==================== 公共接口 ==================== */

int reminder_sched_init(void)
{
    pthread_mutex_lock(&g_lock);

    memset(g_items, 0, sizeof(g_items));
    g_count = 0;
    g_cursor_min = -1;
    g_armed_min = -1;
    g_waiting_time = false;
    g_warned_no_time = false;

    pthread_mutex_unlock(&g_lock);

    return 0;
}

int reminder_sched_add(const char *title, int hour, int min)
{
    int index = -1;

    if (title == NULL || title[0] == '\0' ||
        hour < 0 || hour > 23 || min < 0 || min > 59) {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_lock);

    if (g_count >= REMINDER_MAX_ITEMS) {
        pthread_mutex_unlock(&g_lock);
        return -ENOSPC;
    }

    index = g_count++;
    memset(&g_items[index], 0, sizeof(g_items[index]));
    strncpy(g_items[index].title, title, sizeof(g_items[index].title) - 1);
    g_items[index].hour = hour;
    g_items[index].min = min;
    g_items[index].enabled = true;

    pthread_mutex_unlock(&g_lock);

    return index;
}

int reminder_sched_remove(int index)
{
    if (index < 0) {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_lock);

    if (index >= g_count) {
        pthread_mutex_unlock(&g_lock);
        return -EINVAL;
    }

    /* 后面的整体前移：界面上的下标就是列表下标，别打乱顺序 */
    for (int i = index; i < g_count - 1; i++) {
        g_items[i] = g_items[i + 1];
    }

    g_count--;
    memset(&g_items[g_count], 0, sizeof(g_items[g_count]));

    pthread_mutex_unlock(&g_lock);

    return 0;
}

void reminder_sched_clear(void)
{
    pthread_mutex_lock(&g_lock);

    memset(g_items, 0, sizeof(g_items));
    g_count = 0;
    g_cursor_min = -1;

    pthread_mutex_unlock(&g_lock);
}

int reminder_sched_count(void)
{
    int n;

    pthread_mutex_lock(&g_lock);
    n = g_count;
    pthread_mutex_unlock(&g_lock);

    return n;
}

int reminder_sched_snapshot(reminder_item_t *out, int max)
{
    int n;

    if (out == NULL || max <= 0) {
        return 0;
    }

    pthread_mutex_lock(&g_lock);

    n = (g_count < max) ? g_count : max;
    memcpy(out, g_items, sizeof(g_items[0]) * (size_t)n);

    pthread_mutex_unlock(&g_lock);

    return n;
}

void reminder_sched_set_fire_cb(reminder_fire_cb_t cb, void *arg)
{
    pthread_mutex_lock(&g_lock);
    g_fire_cb = cb;
    g_fire_arg = arg;
    pthread_mutex_unlock(&g_lock);
}

void reminder_sched_reload(void)
{
    int  next;
    int  prev_armed;
    bool no_time;

    pthread_mutex_lock(&g_lock);

    if (enabled_count_locked() == 0) {
        g_cursor_min = -1;
        g_armed_min = -1;
        g_waiting_time = false;
        pthread_mutex_unlock(&g_lock);

        rtc_alarm_cancel();
        printf("[Reminder] 没有启用的提醒，已取消 RTC 闹钟\n");
        return;
    }

    /* 时间没对过就不挂：挂在 2000-01-01 上没有任何意义。等别处
     * （NSH 的 date -s / 网络对时）把时间对好，reminder_sched_tick() 会补挂。 */
    no_time = (rtc_alarm_time_valid() == 0);
    if (no_time) {
        g_armed_min = -1;
        g_waiting_time = true;
        if (!g_warned_no_time) {
            g_warned_no_time = true;
            pthread_mutex_unlock(&g_lock);
            printf("[Reminder] 系统时间还没对过（RTC 在 2000 年附近），提醒暂不生效；\n"
                   "           请先在 NSH 里 date -s \"Sep 13 15:30:00 2026\" "
                   "或让网络对时\n");
            return;
        }
        pthread_mutex_unlock(&g_lock);
        return;
    }

    g_waiting_time = false;
    g_warned_no_time = false;

    next = find_next_locked(g_cursor_min);
    if (next < 0) {
        /* 本轮走完了：回到最早的那条（RTC 模块会算到明天）。
         * 注意**不改 g_cursor_min** —— 改了的话，之后新加的"今天还没到"的
         * 提醒会被当成明天而漏掉一次。 */
        next = find_next_locked(-1);
    }

    prev_armed = g_armed_min;
    g_armed_min = next;

    pthread_mutex_unlock(&g_lock);

    if (next < 0) {
        /* enabled_count 已经过滤过一遍，理论上到不了这里 */
        printf("[Reminder] 算不出下一条提醒\n");
        return;
    }

    if (rtc_alarm_at_daily(next / 60, next % 60, reminder_rtc_fired, NULL) < 0) {
        printf("[Reminder] 挂 %02d:%02d 失败\n", next / 60, next % 60);
        return;
    }

    /* 时刻没变就不重复刷屏（开机连加三条默认提醒会各调一次 reload） */
    if (next != prev_armed) {
        struct rtc_time now;

        if (rtc_alarm_now(&now) == 0) {
            printf("[Reminder] 下一条提醒 %02d:%02d（现在 %02d:%02d）\n",
                   next / 60, next % 60, now.tm_hour, now.tm_min);
        } else {
            printf("[Reminder] 下一条提醒 %02d:%02d\n", next / 60, next % 60);
        }
    }
}

void reminder_sched_tick(void)
{
    bool need_reload = false;

    pthread_mutex_lock(&g_lock);
    need_reload = g_waiting_time && (enabled_count_locked() > 0);
    pthread_mutex_unlock(&g_lock);

    if (!need_reload) {
        return;
    }

    /* 只是"可能对好时了"：reload() 里还会再判一次，
     * 没对好就还是不动（并保持 g_waiting_time）。 */
    reminder_sched_reload();
}

void reminder_sched_default_time(int *hour, int *min)
{
    struct rtc_time now;

    if (hour == NULL || min == NULL) {
        return;
    }

    /* 时间没对过：给个常见的早上八点，别给 2000-01-01 那种算出来的怪时刻 */
    if (rtc_alarm_time_valid() == 0 || rtc_alarm_now(&now) < 0) {
        *hour = 8;
        *min = 0;
        return;
    }

    /* 当前时间 +5 分钟，再取整到 5 分钟刻度（步进按钮的粒度就是 5 分钟，
     * 这样默认值一定落在刻度上，而且总在当前时刻之后 0~5 分钟内）。 */
    int total = now.tm_hour * 60 + now.tm_min + 5;

    total = (total / 5) * 5;
    total %= MINUTES_PER_DAY;

    *hour = total / 60;
    *min  = total % 60;
}
