/**
 * reminder_sched.h - 提醒的"软调度"：把提醒列表接到板子的 RTC 日循环闹钟上
 *
 * 为什么单开一个模块：
 *   硬件只有**一个**日循环 alarm 槽（CONFIG_RTC_NALARMS=1，见
 *   docs/rtc_alarm_usage.md），多个提醒没法各挂一个。这里做软件层排队：
 *   每次只把**最近的一条**挂到 rtc_alarm_at_daily() 上，到点回调里弹提醒、
 *   再接着挂下一条 —— 于是"多条提醒"就跑起来了。
 *
 * 数据放哪：
 *   提醒列表（时:分 + 标题 + 是否启用）就在本模块，是提醒的**唯一真相**。
 *   界面（touch_ui.c）通过下面的接口增删查，不要再自己存一份。
 *   **掉电就没了**：本板没有可写的持久存储（/etc 是只读 ROMFS，/data 是
 *   tmpfs，见 docs/rom_assets_usage.md），所以提醒只在本开机周期内有效；
 *   开机时 main.c 会重新放三条默认提醒。
 *
 * 线程约定（重要）：
 *   - add/remove/clear/snapshot/reload/tick 任何线程都能调，内部一把互斥锁
 *     保护；真正碰 /dev/rtc0 的是板级 RTC 模块自己的工作线程。
 *   - 到点回调 reminder_fire_cb_t 在**板级 RTC 模块的工作线程**里执行
 *     （不是 LVGL 线程，也不是中断）：回调里**不要阻塞、不要碰 LVGL**，
 *     只置标志，或者用 lv_async_call() 把活儿投给 UI 线程。
 *
 * 时区（重要）：
 *   列表里的 hour/min 和回调给出的 hour/min 都是**用户看到的本地时间**
 *   （状态栏上的北京时间，见 robot_ui.c 的 UI_TZ_OFFSET_SEC）。系统时钟 /
 *   硬件 RTC 走的是 UTC，换算只在模块内部跟 RTC 打交道处做 —— 调用方不用管。
 */

#ifndef REMINDER_SCHED_H
#define REMINDER_SCHED_H

#include <stdbool.h>

/* 最多几条提醒、标题最长多少字节（含结尾 '\0'） */
#define REMINDER_MAX_ITEMS   10
#define REMINDER_TITLE_MAX   64

/* 一条提醒。时间用 hour/min 而不是字符串：RTC 闹钟要的就是两个整数，
 * 字符串只用来显示（格式统一在格式化处拼 "HH:MM"）。hour/min 是**本地
 * 时间**（用户照着屏幕设的那个钟点），见文件头"时区"。 */
typedef struct {
    char    title[REMINDER_TITLE_MAX];
    int     hour;               /* 0..23（本地时间） */
    int     min;                /* 0..59（本地时间） */
    bool    enabled;            /* 关掉的提醒不参与调度（界面上暂时没有开关） */
} reminder_item_t;

/* 到点回调（在 RTC 模块的工作线程里执行）。
 *
 * hour/min  —— 响的是哪一条（同一时刻的多条会合成一次回调）；
 * titles    —— 该时刻所有提醒的标题，用"、"连起来，例如 "吃药、喝水"。
 *              这块内存在本模块的调用栈上，**只在回调期间有效**：
 *              要留存请自己拷一份（main.c 就是这么做的）。
 * arg       —— reminder_sched_set_fire_cb() 传进来的用户数据。
 */
typedef void (*reminder_fire_cb_t)(int hour, int min, const char *titles,
                                   void *arg);

/* 复位：清空列表和调度状态（touch_ui_init() 里调一次；builtin 应用重跑时
 * .bss 不清零，所以必须显式复位）。不碰已注册的到点回调。 */
int reminder_sched_init(void);

/* 加一条提醒。返回新条目的下标（>=0），列表满了返回 -ENOSPC。
 * 只动列表，**不重挂闹钟**：调用方改完调 reminder_sched_reload()。 */
int reminder_sched_add(const char *title, int hour, int min);

/* 按下标删一条（界面上的位置就是这个下标）。越界返回 -EINVAL。
 * 同样的：改完要自己调 reminder_sched_reload()。 */
int reminder_sched_remove(int index);

/* 清空所有提醒（改完自己调 reload()）。 */
void reminder_sched_clear(void);

/* 当前有几条。 */
int reminder_sched_count(void);

/* 取一份列表快照（拷贝到 out，最多 max 条），返回实际拷了几条。
 * 界面渲染 / 按下标查详情都用它 —— 拿到的是拷贝，不会跟着别的线程改。 */
int reminder_sched_snapshot(reminder_item_t *out, int max);

/* 注册"到点干什么"。回调在工作线程上下文，别阻塞（见文件头）。 */
void reminder_sched_set_fire_cb(reminder_fire_cb_t cb, void *arg);

/* 重算"下一条该响的提醒"并（重新）挂到 RTC 闹钟上。
 * 新增 / 删除 / 清空提醒之后必须调一次；列表空了会 rtc_alarm_cancel()。
 * 系统时间还没对过时不会挂，而是打印提示并等 reminder_sched_tick()。 */
void reminder_sched_reload(void);

/* 主循环里隔一阵子（建议 30 秒）调一次。两个场合要补挂：
 *   ① "RTC 时间还没对过"时没挂上的（列表非空）；
 *   ② 两次 tick 之间系统时间被对时 / date -s 大步改过 —— 之前按老时间
 *      算出来的"下一条"已经不算数了。
 * 其余情况是空操作，可以随便调。 */
void reminder_sched_tick(void);

/* 新建提醒时用的默认时刻：当前**本地**时间往后取整到 5 分钟刻度（大约
 * "5 分钟内"），时间没对过时给 08:00。 */
void reminder_sched_default_time(int *hour, int *min);

#endif /* REMINDER_SCHED_H */
