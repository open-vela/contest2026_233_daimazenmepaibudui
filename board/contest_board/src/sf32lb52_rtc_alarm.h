/****************************************************************************
 * board/contest_board/src/sf32lb52_rtc_alarm.h
 *
 * SF32LB52 板级 RTC 每日定时提醒模块
 *
 * 解决什么问题：
 *   给上层一个"每天 hh:mm 到点回调一次"的接口（例如成员二的
 *   "每天 08:00 提醒吃药"）。到点后模块在**自己的工作线程**里回调上层，
 *   并自动排下一天的 alarm。
 *
 * 和 sf32lb52_alarm 的区别（两回事，别混）：
 *   - sf32lb52_alarm      = 设备级"紧急报警声响"：响喇叭，持续到解除/超时；
 *   - sf32lb52_rtc_alarm  = 本模块：按**挂钟时间**到点回调一次，
 *     自己不发声、不驱灯，到点做什么完全由回调方决定。
 *
 * 硬件资源（语义与坑详见 docs/rtc_alarm_usage.md）：
 *   - /dev/rtc0 + RTC_SET_ALARM（绝对时间），到点用 **SIGUSR1 信号**通知；
 *   - **全板只有 1 个 alarm 槽**（CONFIG_RTC_NALARMS=1），所以本模块
 *     **只支持一个每日提醒**：重复调用 rtc_alarm_at_daily() 就是覆盖上一个。
 *     这也意味着它和别处直接用 RTC_SET_ALARM / RTC_SET_RELATIVE 的代码
 *     （例如 `hw_test rtc`）**不能同时用**——第二个设的人会覆盖第一个。
 *
 ****************************************************************************/

#ifndef __BOARDS_CONTEST_BOARD_SRC_SF32LB52_RTC_ALARM_H
#define __BOARDS_CONTEST_BOARD_SRC_SF32LB52_RTC_ALARM_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/compiler.h>
#include <nuttx/timers/rtc.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* RTC 时间没对过（读到 2000 年之前）时，每多少毫秒重试一次；等别处把
 * RTC 对好（NSH 的 `date -s` / 网络对时）后自动开始排 alarm。
 * 可在包含本头文件前重定义。 */

#ifndef RTC_ALARM_TIME_RETRY_MS
#  define RTC_ALARM_TIME_RETRY_MS   (30 * 1000)
#endif

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* 到点回调。
 *
 * 注意：回调在本模块**工作线程**的上下文里执行（不是调用
 * rtc_alarm_at_daily() 的那个线程，也不是中断）。
 *
 * 回调里**不要阻塞**：不要 sleep / 等信号量 / 做重运算，也**不要直接
 * 播 TTS 或碰 LVGL**（LVGL 不是线程安全的）。推荐只置一个标志，让
 * 主循环去播音 / 刷 UI。回调返回后，模块会自动排下一天的 alarm。
 */

typedef void (*rtc_alarm_cb_t)(FAR void *arg);

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rtc_alarm_at_daily
 *
 * Description:
 *   注册"每天 hour:min 到点回调一次"的提醒。**非阻塞**：只记录状态、
 *   唤醒模块工作线程就返回。调用方**不需要**先 init（懒初始化）。
 *
 *   - hour 0..23、min 0..59，非法返回 -EINVAL；
 *   - 只支持一个每日提醒，重复调用 = 覆盖上一个；
 *   - cb 为 NULL 表示"到点了但不回调"（占位用）；
 *   - RTC 时间还没对过时不会乱排 alarm，而是每 RTC_ALARM_TIME_RETRY_MS
 *     重试一次等对时；
 *   - 到点回调后自动排下一天。
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure (-EINVAL / -ENOMEM ...).
 *
 ****************************************************************************/

int rtc_alarm_at_daily(int hour, int min, rtc_alarm_cb_t cb, FAR void *arg);

/****************************************************************************
 * Name: rtc_alarm_cancel
 *
 * Description:
 *   取消当前每日提醒：工作线程会（最迟约 1 秒内，通常立刻）退出等待、
 *   把硬件的 alarm 槽取消掉。没有在跑时是安全的空操作。
 *
 * Returned Value:
 *   OK on success.
 *
 ****************************************************************************/

int rtc_alarm_cancel(void);

/****************************************************************************
 * Name: rtc_alarm_now
 *
 * Description:
 *   读当前 RTC 时间（方便上层显示）。**不需要先 init**。
 *
 * Input Parameters:
 *   out - 输出缓冲，不能为 NULL
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rtc_alarm_now(FAR struct rtc_time *out);

/****************************************************************************
 * Name: rtc_alarm_time_valid
 *
 * Description:
 *   当前 RTC 时间是否已经"对过"（年份 >= 2000）。**不需要先 init**。
 *   板子没有 RTC 备份电池，掉电后时间会丢（读出来是 2000-01-01），
 *   上层可据此提示"请先对时"。
 *
 * Returned Value:
 *   1 = 已对时，0 = 没对时（或读失败）.
 *
 ****************************************************************************/

int rtc_alarm_time_valid(void);

#endif /* __BOARDS_CONTEST_BOARD_SRC_SF32LB52_RTC_ALARM_H */
