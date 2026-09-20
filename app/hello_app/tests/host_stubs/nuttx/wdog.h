/**
 * 给 host 测试用的假 <nuttx/wdog.h>
 *
 * 为什么需要：ai_llm.h 里有一个 `struct wdog_s backend_wdog` 成员、ai_llm.c 用
 * `wd_start()` / `wd_cancel()`（等 ai_agent 回包那片等待的私有心跳，见 ai_llm.c
 * 那段注释）。真机那份来自 NuttX 的 include/nuttx/wdog.h；PC 上编没有它，
 * 少了这一份 host 测试从 ai_llm.h 那一行就编不过。
 *
 * 真身（这份是照着它逐项核的）：
 *   nuttx/include/nuttx/wdog.h          —— WDOG_ISACTIVE / 类型 / wdog_s / 原型
 *   nuttx/sched/wdog/wd_start.c         —— wd_start 的实现
 *   nuttx/sched/wdog/wd_cancel.c        —— wd_cancel 的实现
 *   nuttx/include/nuttx/list_type.h:43  —— struct list_node
 *   nuttx/include/sys/types.h:255-258   —— clock_t
 * （都在 /home/youdian/openvela/ 下。）
 *
 * host 这条路上真正引用到本头文件的只有 ai_llm.h:175 那个成员声明、
 * ai_llm.c:469 的 wd_start、ai_llm.c:480 的 wd_cancel —— 三个都照着真身核过。
 * test_member2.c 只走 llm_init / llm_build_request_json / llm_deinit，
 * **不会真的跑**到那条 ai_agent 等待（见下面"简化掉的"）。
 *
 * 与真身保持一致、别图省事改掉的：
 *   - wdparm_t / wdentry_t：真身在 64 位下是 uintptr_t（wdog.h:72），回调签名一致。
 *   - 返回值口径：wd_start / wd_cancel 都是 int，成功 0(OK)、失败**负 errno**
 *     （真身注释原话："a negated errno value is return to indicate the nature
 *     of any failure"），不是 POSIX 的 -1 + errno —— ai_llm.c:480 那句
 *     `if (wd_cancel(...) != OK)` 就是靠这个口径分岔的。
 *   - struct wdog_s 的成员名与顺序：node / arg / func / expired（CONFIG_PIC 那个
 *     picbase 本板没开）。node 是 list_node，所以这里把 list_node 一并照抄 ——
 *     不自造 node_prev/node_next：成员名对不上时 host 上没人会发现，等以后有人
 *     读了就晚，而照抄的成本只有几行。
 *   - wd_cancel 的判定与真身 wd_cancel.c 一样：wdog 为 NULL 或
 *     !WDOG_ISACTIVE(wdog) → -EINVAL，否则清掉 func、返回 OK。
 *   - include guard 风格与 host_stubs 里另外两份一致（带 host stub 字样的名字，
 *     真身是 __INCLUDE_NUTTX_WDOG_H）：故意不跟真身重名，这样万一真身和这份同时
 *     进了包含路径，报出来的是重定义，而不是"先到先得"把另一份悄悄顶掉。
 *
 * host 上简化掉的（都不影响 test_member2 覆盖到的行为）：
 *   - FAR / CODE：平坦构建下它们是空宏（nuttx/compiler.h:984-987），这里省掉。
 *   - clock_t：真机是本板 .config 里 CONFIG_SYSTEM_TIME64 未开时的 uint32_t
 *     （sys/types.h:258）。host 上不能自己 typedef 这个名字 —— ai_llm.c:20 也
 *     include <time.h>，重定义会撞 glibc 的 typedef（error: conflicting types
 *     for 'clock_t'），所以这里直接借 glibc 那份（x86-64 上是 long）。delay 在
 *     下面那个简化实现里只被记下/丢弃，这点宽度差异碰不到被测行为。
 *   - wd_start：真机把 wdog 按绝对到点 tick 排进队列、到点由 systick 中断里调
 *     wdentry(arg)（wd_start.c 的 wd_expiration）。host 上没有 tick 源，这里只把
 *     func/arg 记进 wdog —— 等价于真机"刚武装、还没到点"那一刻，于是
 *     WDOG_ISACTIVE() 为真、wd_cancel() 返回 OK。**注意**：真机那种"到点"host 上
 *     不会发生，所以 ai_llm.c 里 -ETIMEDOUT 那条路在 host 上跑不出来；host 没实现
 *     真机 wd_start 里 delay > WDOG_MAX_DELAY → -EINVAL 那道闸，也是同一个理由
 *     （引用者没有用它）。
 *     ⚠️ **后果要记住**：假 wd_start 只会记下 func/arg、永远不会到点（假
 *     semaphore.h 那边 nxsem_wait 也没有超时），所以**别在 host 测试里去调那条等待
 *     路径**（`llm_send_text()` 之类，见 ai_llm.c 里 `#ifdef
 *     CONFIG_HELLO_APP_LLM_AI_AGENT` 那一段）—— 它会**永久挂住**，而不是报个错。
 *     现在跑不到：host 这份 config.h 没定义那个开关，那段根本没编进来。
 *
 * 真机固件用的是 NuttX 真的那个头文件，本文件对它没有任何影响。
 *
 * ⚠️ 只在 host 测试的 -I 里（见 run_host_tests.sh 的 -I$STUBS）。
 */

#ifndef __HELLO_APP_HOST_STUB_NUTTX_WDOG_H
#define __HELLO_APP_HOST_STUB_NUTTX_WDOG_H

#include <nuttx/config.h>     /* OK —— 真身也 include 它 */
#include <errno.h>            /* -EINVAL */
#include <stdint.h>           /* uintptr_t */
#include <time.h>             /* clock_t（理由见文件头） */

/* 真身 wdog.h:43，同一个写法（ai_llm.c 没有直接用，但下面 wd_cancel 要用）。 */

#define WDOG_ISACTIVE(w)   ((w)->func != NULL)

/* 真身 list_type.h:43；wdog.h 只是为了 wdog_s 的第一个成员才 include 它。 */

struct list_node
{
  struct list_node *next;
  struct list_node *prev;
};

/* 真身 wdog.h:72（64 位下走 uintptr_t 这一支）。 */

typedef uintptr_t wdparm_t;

/* 真身 wdog.h:81（CODE 见文件头）。 */

typedef void (*wdentry_t)(wdparm_t arg);

/* 真身 wdog.h:85，成员名/顺序一致。 */

struct wdog_s
{
  struct list_node node;    /* Supports a doubly linked list */
  wdparm_t         arg;     /* Callback argument */
  wdentry_t        func;    /* Function to execute when delay expires */
  clock_t          expired; /* Timer associated with the absolute time */
};

/* 真身 wdog.h:166，签名逐字一致（FAR 见文件头）。 */

static inline int wd_start(struct wdog_s *wdog, clock_t delay,
                           wdentry_t wdentry, wdparm_t arg)
{
  wdog->arg   = arg;
  wdog->func  = wdentry;
  (void)delay;                /* 见文件头：host 上没有 tick 源，不会到点 */

  return OK;
}

/* 真身 wdog.h:363 的原型 + wd_cancel.c 里那段判定。 */

static inline int wd_cancel(struct wdog_s *wdog)
{
  /* 真机那句 `wdog != NULL && WDOG_ISACTIVE(wdog)` 反过来写：不满足就是
   * ret = -EINVAL 那条（没武装过、或者已经到点被清成 NULL 了）。 */

  if (wdog == NULL || !WDOG_ISACTIVE(wdog))
    {
      return -EINVAL;
    }

  wdog->func = NULL;          /* 真机在临界区里做的也是这一句 */
  return OK;
}

#endif /* __HELLO_APP_HOST_STUB_NUTTX_WDOG_H */
