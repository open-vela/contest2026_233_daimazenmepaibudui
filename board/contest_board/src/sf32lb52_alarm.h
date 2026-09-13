/****************************************************************************
 * board/contest_board/src/sf32lb52_alarm.h
 *
 * SF32LB52 设备级报警模块
 *
 * 负责报警的「设备级动作」：喇叭响，并持续到主动解除或自动超时。
 * 网络上报、界面显示、状态机由上层通过 alarm_set_callback() 挂回调对接
 * （本模块不 include 任何 app 头文件）。
 *
 * 本模块**不驱动任何指示灯**：板上唯一能由软件控制的用户输出脚 PA26
 * 留给板级/应用做状态指示更值；原理图上标着 RGB LED 的 PA32 在这块板子
 * 实物上并不存在。报警只有声音这一种输出。
 *
 * 节点/引脚：
 *   - 报警音：/dev/audio/audio0（标准 NuttX audio 接口，自己 open fd）
 *
 ****************************************************************************/

#ifndef __BOARDS_CONTEST_BOARD_SRC_SF32LB52_ALARM_H
#define __BOARDS_CONTEST_BOARD_SRC_SF32LB52_ALARM_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/compiler.h>

#include <stdint.h>
#include <stdbool.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 自动解除时间（ms）。与状态机 SM_STATE_ALARM 的 60 秒对齐，
 * 超时后模块自己解除并回调 ALARM_EVENT_TIMEOUT。
 * 上层可在包含本头文件前重定义（#define ALARM_AUTO_TIMEOUT_MS ...）。 */

#ifndef ALARM_AUTO_TIMEOUT_MS
#  define ALARM_AUTO_TIMEOUT_MS   (60 * 1000)
#endif

/* 报警音默认音量（标准 AUDIO_FU_VOLUME 接口，0..1000）。
 * 800 约合 -2.4dB，够响但不至于爆音。 */

#ifndef ALARM_PLAYBACK_VOLUME
#  define ALARM_PLAYBACK_VOLUME   800
#endif

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* 报警级别（数值越大越紧急） */

enum alarm_level_e
{
  ALARM_LEVEL_NONE = 0,
  ALARM_LEVEL_NOTICE,      /* 提示：一声短鸣 */
  ALARM_LEVEL_WARNING,     /* 警告：三声短鸣，然后每 10 秒重复一次 */
  ALARM_LEVEL_EMERGENCY,   /* 紧急：高低交替的警报音，每 5 秒重复一次 */
};

/* 报警事件 */

enum alarm_event_e
{
  ALARM_EVENT_TRIGGERED = 1,   /* 报警被触发（含"已在报警中又被触发"的情况） */
  ALARM_EVENT_CLEARED,         /* 被 alarm_clear() 主动解除 */
  ALARM_EVENT_TIMEOUT,         /* 超过自动解除时间，模块自己解除 */
};

/* 报警状态（alarm_get_status() 的输出，也是回调里 status 的内容） */

struct alarm_status_s
{
  bool              active;         /* 当前是否处于报警中 */
  enum alarm_level_e level;         /* 当前报警级别 */
  char              reason[32];     /* 短标识，如 "sound" / "sos" / "fall" */
  char              text[96];       /* 给人看的描述 */
  uint32_t          triggered_ms;   /* 触发时刻，系统 tick 换算的 ms（自开机起） */
  uint32_t          repeat_count;   /* 本次报警已经重复播报了几轮 */
};

/* 事件回调。
 *
 * 注意：回调是在本模块**工作线程**的上下文里调用的（不是调用
 * alarm_trigger()/alarm_clear() 的那个线程，也不是中断）。
 * 回调里**不要阻塞**（sleep、等信号量、长时间运算、写文件…），
 * 否则会拖住报警播报和解除。耗时动作请丢给别的任务做。
 *
 * status 指向的内容只在本次回调期间有效（内部快照），需要留存请自行拷贝。
 */

typedef void (*alarm_cb_t)(enum alarm_event_e event,
                           FAR const struct alarm_status_s *status,
                           FAR void *arg);

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: alarm_init
 *
 * Description:
 *   初始化报警模块：创建报警工作线程。
 *   **幂等**：重复调用直接返回 OK。
 *
 *   调用方不一定要先调它 —— alarm_trigger() 内部会在需要时自己初始化
 *   （懒初始化），这样别人接进来只需要调 alarm_trigger() 一个函数。
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int alarm_init(void);

/****************************************************************************
 * Name: alarm_trigger
 *
 * Description:
 *   触发（或再次触发）报警。**非阻塞**：只更新状态、唤醒工作线程就返回，
 *   声音由工作线程完成，不占用调用者的线程。
 *
 *   重复触发规则（NONE < NOTICE < WARNING < EMERGENCY）：
 *     - 当前未报警        ：按 level 开始报警；
 *     - 新 level 更高     ：按新级别**重来**（重新计时、清重复计数）；
 *     - 同级或更低        ：只更新 reason/text，不打断当前播报。
 *   以上三种情况都会回调 ALARM_EVENT_TRIGGERED，让上层知道"又报了一次"。
 *
 *   reason/text 允许为 NULL，内部按空串处理。
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure
 *   （level 非法返回 -EINVAL，初始化失败返回对应负值）.
 *
 ****************************************************************************/

int alarm_trigger(enum alarm_level_e level, FAR const char *reason,
                  FAR const char *text);

/****************************************************************************
 * Name: alarm_clear
 *
 * Description:
 *   主动解除当前报警：停音频、清 active，并回调
 *   ALARM_EVENT_CLEARED（在工作线程上下文里回调）。
 *   没有处于报警中时是安全的空操作。
 *
 * Returned Value:
 *   OK on success.
 *
 ****************************************************************************/

int alarm_clear(void);

/****************************************************************************
 * Name: alarm_get_status
 *
 * Description:
 *   无锁安全读：拿到当前报警状态的快照。**不需要先初始化模块**也能安全调用
 *   （未初始化时返回 active=false 的全零状态）。
 *
 * Input Parameters:
 *   out - 输出缓冲，不能为 NULL
 *
 * Returned Value:
 *   OK on success; -EINVAL if out is NULL.
 *
 ****************************************************************************/

int alarm_get_status(FAR struct alarm_status_s *out);

/****************************************************************************
 * Name: alarm_set_callback
 *
 * Description:
 *   注册/注销事件回调。cb 传 NULL 表示注销。
 *   回调在工作线程上下文里执行，规则见 alarm_cb_t 的注释。
 *
 * Returned Value:
 *   OK on success.
 *
 ****************************************************************************/

int alarm_set_callback(alarm_cb_t cb, FAR void *arg);

#endif /* __BOARDS_CONTEST_BOARD_SRC_SF32LB52_ALARM_H */
