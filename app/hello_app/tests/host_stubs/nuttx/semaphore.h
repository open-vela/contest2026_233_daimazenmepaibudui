/**
 * 给 host 测试用的假 <nuttx/semaphore.h>
 *
 * 谁用：ai_llm.c 第 12 行那句无条件的 `#include <nuttx/semaphore.h>`；真正引用到
 *      的符号只有两个 —— `nxsem_wait()`（等 ai_agent 回包那片等待循环，
 *      ai_llm.c:474）和 `nxsem_post()`（心跳到点那一记叫醒，ai_llm.c:389）。
 *
 * 真身在哪：/home/youdian/openvela/nuttx/include/nuttx/semaphore.h。PC 上编没有
 *      那一份（它拉 nuttx/config.h、nuttx/sched.h、nuttx/atomic.h 一大串内核头），
 *      少了这份假头 ai_llm.c 第一段 include 就编不过。
 *
 * host 上这两个入口其实**不会被调到**：本目录 config.h 没定义
 *      CONFIG_HELLO_APP_LLM_AI_AGENT，ai_llm.c:372 起那一整段
 *      （llm_ai_agent_wait_timeout / llm_ai_agent_callback /
 *      llm_do_ai_agent_request）都没编进来。那也不能乱写：签名和返回值口径都要
 *      跟真身一样，将来谁在 host 上把这个开关打开、或把这段挪出 #ifdef，才不会
 *      踩坑。下面逐项写清对照的是真身哪一处。
 *
 * 逐项对照（真身 = /home/youdian/openvela/nuttx/include/nuttx/semaphore.h）：
 *
 *   nxsem_wait / nxsem_post
 *     · 签名：真身 692 / 758 行都是
 *       `static inline_function int nxsem_wait(FAR sem_t *sem)`
 *       （CONFIG_LIBC_SEM_MUTEX_NOINLINE 没开时的 inline 版；本工程构建目录
 *       里那份 .config 没有这个符号，真机走的就是 inline 版）。
 *       host 这份写成 `static inline int nxsem_xxx(sem_t *sem)`：FAR 是编译器段
 *       属性宏（nuttx/compiler.h），PC 上讲不通也没有意义，参数类型仍是
 *       sem_t *，其余一字不差。
 *     · 返回值口径：**0 = 成功，负值 = -errno**（NuttX 内部错误约定），不是 POSIX
 *       的 -1 + errno。真身文档（125 / 189 行）写的就是这个。ai_llm.c:474
 *       `while (ret == -EINTR && !ctx->request_cancel);` 就是照这个口径写的，
 *       所以这里必须是负的 EINTR，不能是 -1。
 *     · errno：真身文档明确 "It does not modify the errno value"，而 host 这一层
 *       只能落到 glibc 的 sem_wait/sem_post（失败时返回 -1 并把原因写进 errno）
 *       —— 这里进出各存、还原一次 errno，把这个差别抹平，免得被测代码里别处的
 *       errno 判断被这两个调用污染。
 *     · host 上消除不掉的差别（都不影响现在这条路径）：glibc 的 sem_wait 是取消
 *       点，真身不是（"It is not a cancellation point"）；真身是原子快路径 +
 *       nxsem_wait_slow，host 一律走 glibc。host 测试不开线程取消、也不测这处的
 *       并发，碰不到。
 *
 *   MSEC2TICK
 *     · 为什么在这个假头里：真身的 nuttx/semaphore.h 会把 nuttx/clock.h 一起拉
 *       进来，MSEC2TICK 在那边是白拿的；ai_llm.c:469
 *       `wd_start(..., MSEC2TICK(LLM_HTTP_TIMEOUT_MS), ...)` 用它算心跳时长
 *       （LLM_HTTP_TIMEOUT_MS = 30000，见 ai_llm.h:47）。host 上少了它那一行编
 *       不过。
 *     · 口径照抄 nuttx/clock.h：
 *         USEC_PER_TICK  = CONFIG_USEC_PER_TICK（本板生成出来的 config.h 里是
 *                          10000，clock.h 那个 #ifdef 分支走的就是它）
 *         MSEC_PER_TICK  = USEC_PER_TICK / USEC_PER_MSEC   （= 10）
 *         MSEC2TICK(msec) = div_const_roundup(msec, MSEC_PER_TICK)
 *       一 tick 10ms，和 robot_ui/ui_perf.h 里记的一致。
 *     · div_const_roundup 是**向上取整**（nuttx/include/nuttx/lib/math32.h:355
 *       `(((n) + (base) - 1ul) / (base))`），所以这里也必须向上取整：写成截断
 *       除法的话 MSEC2TICK(15) 得 1，真身是 2。当前唯一的调用点 30000ms 正好是
 *       10 的整数倍（两种算法都得 3000），但口径得跟真身一致。
 *
 * ⚠️ 只在 host 测试的 -I 里（见 run_host_tests.sh 的 -I$STUBS）；
 *    真机固件编的是 NuttX 真的那个头文件，本文件对它没有任何影响。
 */

#ifndef __HELLO_APP_HOST_STUB_NUTTX_SEMAPHORE_H
#define __HELLO_APP_HOST_STUB_NUTTX_SEMAPHORE_H

#include <errno.h>
#include <semaphore.h>

/* 真机一 tick 10ms：CONFIG_USEC_PER_TICK = 10000 → MSEC_PER_TICK = 10 */

#ifndef MSEC_PER_TICK
#  define MSEC_PER_TICK       10
#endif

/* 向上取整，对齐 clock.h:158 的 div_const_roundup（理由见文件头）。 */

#ifndef MSEC2TICK
#  define MSEC2TICK(msec)     ((long)(((msec) + MSEC_PER_TICK - 1) / MSEC_PER_TICK))
#endif

/* 0 = 成功、负值 = -errno（真身 692 行）；errno 原样进、原样出。 */

static inline int nxsem_wait(sem_t *sem)
{
  int saved_errno = errno;
  int ret = 0;

  if (sem_wait(sem) != 0)
    {
      ret = -errno;              /* 真身这条只会是 -EINVAL / -EINTR */
    }

  errno = saved_errno;
  return ret;
}

static inline int nxsem_post(sem_t *sem)
{
  int saved_errno = errno;
  int ret = 0;

  if (sem_post(sem) != 0)
    {
      ret = -errno;
    }

  errno = saved_errno;
  return ret;
}

#endif /* __HELLO_APP_HOST_STUB_NUTTX_SEMAPHORE_H */
