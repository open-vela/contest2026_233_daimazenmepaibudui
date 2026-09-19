/****************************************************************************
 * boards/sf32lb52/sf32lb52_devkit_lcd/src/sf32lb52_audio.c
 *
 * SF32LB52 音频设备驱动（audio_lowerhalf 实现）
 *
 * 架构（参考 SiFli SDK audprc 例程，RT-Thread 侧已验证）：
 *   - AUDCODEC：模拟前端（DAC/ADC 模拟通路，不走 DMA）
 *   - AUDPRC  ：数字音频处理器，数据通路（TX0=播放 DMA / RX0=录音 DMA）
 *
 * 数据流：
 *   播放：内存 → AUDPRC TX0 DMA → AUDPRC → codec DAC 模拟 → AW8155 功放 → 喇叭
 *   录音：麦克风 → codec ADC 模拟 → AUDPRC → AUDPRC RX0 DMA → 内存
 *
 * 注册为 NuttX 音频设备 /dev/audio0。
 *
 * 录音（RX）通路的形状、以及"为什么是这个形状"（与厂商参考实现 drv_audprc.c
 * 的对照），见下面 Pre-processor Definitions 里"录音 RX 通路的实现"那段 ——
 * 那个文件里留下的只有**一条**路径，旧的两套结构交给 git 历史。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <sys/ioctl.h>
#include <unistd.h>              /* getpid()：记会话持有者的任务组 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>
#include <syslog.h>
#include <math.h>

#include <nuttx/kmalloc.h>
#include <nuttx/audio/audio.h>
#include <nuttx/sched.h>        /* nxsched_gettid()/nxsched_get_tcb()：查持有者线程还在不在 */
#include <nuttx/semaphore.h>
#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/arch.h>

#include "bf0_hal_audcodec.h"
#include "bf0_hal_audprc.h"
#include "bf0_hal_pmu.h"
#include "bf0_hal_rcc.h"
#include "bf0_hal_gpio.h"
#include "register.h"
#include "dma_config.h"
#include "sf32lb52_audio.h"

/* 厂商串口驱动为板级开的一个口子（补丁见 patches/vendor_sifli-uart-rx-dma-reinit.patch）：
 * 把某一路 UART 的 RX DMA 重新配置并重新启动接收。
 *
 * 为什么必须要它（2026-09-17 真机踩到）：RX 恢复里那记"复位整块 DMAC1"会把 DMAC1
 * 上**所有**通道的配置抹掉 —— 不只是音频的 AUDPRC RX0，还有**控制台串口 UART1 的
 * RX DMA**（`CONFIG_BSP_UART1_RX_USING_DMA=y`，通道 DMA1_Channel6/7）。而本串口
 * 驱动只在"DMA 传输完成"的中断回调里才会重新武装 RX，被抹之后永远不会完成 →
 * **串口 RX 永久聋**：板子其实活着、日志照打，但 nsh 再也收不到任何命令（现场表现
 * 就是"敲完命令控制台安详了"，只能靠重启救）。
 *
 * idx 用厂商枚举：UART1_INDEX=0（控制台）、UART2_INDEX=1（调试口，本配置也开着）。
 * 没 attach 的那一路返回 -ENODEV，忽略即可。 */
extern int sifli_uart_reinit_rx_dma(int idx);

/* 只为一个原型而来：sf32lb52_audio_rx_stats() 声明在板级录音封装那个头文件里
 * （上层 app 的 include path 里有它，见 board/contest_board/src 的 CMakeLists.txt），
 * 驱动这边包含它只是为了让编译器核对定义与声明一致。它不引任何别的东西。 */
#include "sf32lb52_audio_in.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SF32LB52_AUDIO_CLK_TAB_NUM  9

/* 通道选择：单通道（DAC0 播放 / ADC0 录音；AUDPRC TX0 / RX0）
 * 注意：codec 的 Config_TChanel/RChanel 用通道索引（0/1），
 * 不是 HAL_AUDCODEC_*_CHx 枚举（DAC_CH0=0, ADC_CH0=2） */

#define SF32LB52_AUDIO_DAC_CH       0
#define SF32LB52_AUDIO_ADC_CH       0
#define SF32LB52_AUDIO_PRC_TX_CH    HAL_AUDPRC_TX_CH0
#define SF32LB52_AUDIO_PRC_RX_CH    HAL_AUDPRC_RX_CH0

/* 功放使能 GPIO：PA10 = AU_PA_EN（bsp_pinmux 已配置为 GPIO 功能） */

#define SF32LB52_AUDIO_PA_GPIO      ((GPIO_TypeDef *)hwp_gpio1)
#define SF32LB52_AUDIO_PA_PIN       10

/* AUDPRC 通路选择（MUX/MIX 源），参考 SDK audprc 例程 out_sel=0x5050 */

#define SF32LB52_AUDIO_OUT_SEL      0x5050

/* 默认音量（dB） */

#define SF32LB52_AUDIO_DEFAULT_VOL  (-6)

/* 录音通路数字增益（dB，有效范围 -36 ~ +60）。
 * 原来设 0dB，实测正常说话只有 peak≈2400/32767 ≈ -23dBFS，回放听着很小、
 * 对 VAD/ASR 也不利。+18dB 会削顶（peak 打满 32768），所以取 +12dB（约 4 倍）。 */
#define SF32LB52_AUDIO_ADC_VOL      (12)

/* 标准音量接口把 0~1000 映射到这个 dB 区间（HAL 只接受 -36~+60） */
#define SF32LB52_AUDIO_VOL_MIN      (-36)
#define SF32LB52_AUDIO_VOL_MAX      (6)

/* 开/关功放时先把 DAC 拉到最小音量并等通路静态电平稳定，
 * 避免功放使能/断开那一瞬间把 DAC 输出的跳变放大成"啪"的爆音。
 * HAL 的下限就是 -36（没有真正的静音位），这是它能给的最大衰减。 */
#define SF32LB52_AUDIO_VOL_MUTE      SF32LB52_AUDIO_VOL_MIN
#define SF32LB52_AUDIO_SETTLE_MS     30

/* TE（DMA 传输错误）日志的限频窗口。TE 会让 read **立刻**返回，一个持续性的
 * TE 就是"上层每次重试一条" —— 不限频能把串口刷满、把现场其它线索冲掉，
 * 所以窗口内只累计次数，出窗口时把被吞掉的条数一起报出来。
 * （超时那条日志不需要限频：一次超时要等满 5 秒，天然每秒最多一条。） */
#define SF32LB52_AUDIO_TE_LOG_MS   30000

/* 会话切换的宽限期：START 成功不到这么久的会话**绝不**被判为残留强停。
 *
 * 误伤窗口永远在会话开头：本板存在"起会话的线程是短命线程"的真实用法（见
 * struct 里 holder_tid 的说明），那种线程 START 完就走，而真正读数据的线程
 * 往往还没进第一次 read()（user_tid 还是 -1）。在这条会话刚起来的那一小段
 * 时间里，两条线程证据都是空的，会话却是活的 —— 光看线程就会把它当残局拆了。
 * 宽限期把这一段单独掐掉：宁可按"有人在用"回 -EBUSY（交给上层重试），也不去
 * 拆一条刚开始的会话。 */
#define SF32LB52_AUDIO_SWITCH_GRACE_MS  3000

/* 停 RX DMA 通道时**不动 AUDPRC 的 ADC 数据通路**（CFG.ADC_PATH_EN，CFG 的
 * bit7）—— 只做 HAL_DMA_Abort()，**不**走 HAL_AUDPRC_DMAStop()。
 *
 * 为什么：DMAStop 在做完 abort 之后紧接着 __HAL_AUDPRC_ADCPATH_DISABLE()
 * （bf0_hal_audprc.c:846），也就是关通道的同时把 ADC 数据通路掰断；而下一个
 * HAL_AUDPRC_Receive_DMA() 又把它置回来（:790）。老实现每 20ms 就走一遍这个
 * 关/开，等于反复掰断这条通路。厂商参考驱动整场会话只做一次 ENABLE / 一次
 * DISABLE（都在会话边界），本驱动照它来：这一位由本会话第一次 Receive_DMA()
 * 置起之后一直保持，直到 hw_stop()/hw_shutdown() 里的软复位才清（"5 秒零活动"
 * 那一次分级恢复会连模块一起复位、把它一并清掉，随后由下一次 Receive_DMA 置回，
 * 见 sf32lb52_audio_rx_recover）。
 *
 * 触发频率是"会话边界"级的，不是每帧：sf32lb52_audio_rx_dma_stop_frame 只在
 * 三个时刻被调到 —— read 武装前摆正句柄状态、read 的收尾、hw_start 的会话
 * 初始化。
 *
 * 这条不变量来自上板仪表日志的定案：老实现每 20ms 就 abort 一次通道，一个 arm
 * 周期里 CNDTR 仍是满值、完成与半满中断整段等待里一次都没来 ⇒ DMA 的请求线从未
 * 被拉高；同时句柄一直停在 BUSY、无 TE、软复位只压得住症状 ⇒ 坏在 AUDPRC 的
 * RX/ADC 侧不再向 DMA 发请求。每帧一次的 ADCPATH 关/开正是把这条通路反复掰断
 * 的那个动作。 */

/* 录音（RX）通路的实现 —— **一条路径，没有开关**（旧的两套结构交给 git 历史：
 * 2026-09-16 之前那版是"每帧 arm/abort + 100ms 分片等待 + 三级恢复"，再往前
 * 还有一版"常驻接收环 + 半块通知"。两者都已删除，只剩下面这一条）。
 *
 * 形状（对着厂商参考实现 drv_audprc.c 的 bf0_audio_start/stop 写的）：
 *   read() 一次成型 —— HAL_AUDPRC_Receive_DMA(调用者的 buffer, 这次要读的长度)
 *   → 在 rx_sem 上等一次（5 秒上界）→ 把 RX 通道收干净。
 *   ISR 只做三件事：过会话闸、计数、post 一次信号量（队列模式则交还 buffer）；
 *   **中断里绝不碰通道、绝不做任何恢复**。
 *
 * 为什么是这个形状（上板诊断定案，症状是"read 永久阻塞 + 长跑后 RX 再也不拉请求"）：
 *   1) 老实现是"驱动内阻塞等待 + 一记全驱动唯一的私有看门狗当心跳"：心跳每次
 *      只 post 一次，而 nxsem_reset / wd_cancel 会被别的读者拆掉 → 永久阻塞。
 *      厂商参考实现是**零阻塞**的。
 *   2) 老实现每次 read 超时都去 abort/re-arm DMA 通道（三级恢复）：通道被反复
 *      拆装，RX 侧长跑之后不再拉请求（CNDTR 冻结、只有断电恢复）就是从这里来的。
 *   3) 老实现的 ISR 缺会话闸（厂商有 g_rx_stop）—— 一次 stop 之后飘进来的完成
 *      通知会去叫醒下一次 read，把一块没采满的缓冲当数据交上去。
 *
 * 与厂商的结构差异（必须写明，免得下一个人以为漏抄了）：
 *   厂商的 bf0_audio_start 里那一次 HAL_AUDPRC_Receive_DMA 挂的是驱动自有的
 *   接收 buffer（haudprc->buf[RX_CH0]），之后整场会话靠 HT/TC 两个回调交数据。
 *   本驱动不能那么挂：NuttX audio_lowerhalf 的 read() 是"直接读进调用者给的
 *   buffer"（上层 audio.c 不参与搬运），所以 Receive_DMA 的目的地只能是调用者
 *   的 buffer —— 武装点因此落在 read() 里，而不是 hw_start()。hw_start() 负责的
 *   是一次武装的前置条件：把上一条会话可能留下的句柄状态收干净、并关上会话闸
 *   （rx_arm_gen = 0），位置就在厂商 arm 的那一处（__HAL_AUDPRC_ENABLE 之前）。
 *   "整场会话只 arm 一次"在这个语义下不可实现（每次 read 要把数据读进不同的
 *   buffer），能保住、也必须保住的是厂商另外两条：**DMA 通道平时只在会话边界被
 *   拆装**（read 自己的收尾只是 abort，不关 ADCPATH；唯一的例外是"5 秒零活动"
 *   那一次分级恢复，见 sf32lb52_audio_rx_recover —— 那正是"通道自己没问题、上游
 *   请求线死了"的形态）、**中断里不做任何恢复**。
 *
 * 另两处刻意的偏离，理由各自写在实现处：
 *   - 等待（5 秒上界）用**本驱动私有**的看门狗当心跳，不用内核的
 *     nxsem_clockwait_uninterruptible / nxsem_tickwait（见 struct 里
 *     rx_wait_wdog 那一段与 sf32lb52_audio_rx_wait_slice）：内核那记定时等待的
 *     超时与 ISR 的 post 抢同一份 TCB 字段，本板临界区是 BASEPRI 型、DMA 通道
 *     中断优先级 0 挡不住它 —— 2026-09-19 真机上既抓到过 sem_waitirq.c:137 那条
 *     断言，也抓到过"超时被抢掉、这一次 read 再也没有上界"的永久卡死。
 *   - 收尾用 sf32lb52_audio_rx_dma_stop_frame()（只 abort 通道）而不是
 *     HAL_AUDPRC_DMAStop()：后者顺带 ADCPATH_DISABLE，也就是"每帧掰断 ADC
 *     数据通路"那条老毛病。 */

/* ─── RX 恢复的分级编排（⚠️ **诊断期临时**，定案后必须收敛成生效的那一级）───
 *
 * 上板仪表定案（2026-09-16，串口原文见报告）：坏的不是模块时钟，是
 * "外设 → DMAC 的请求握手"。失败现场这几件事同时成立：
 *   RX_CH0_CFG=0x41  → EN=1、DMA_MSK=0、FIFO_CNT=4       外设侧有数据在积
 *   IRQ=0x80         → RX_IN_FIFO_OF=1（还溢出了）        外设侧认为"该搬了"
 *   CNDTR=160 / ISR=0 / hdma.State=BUSY                   DMAC 一个请求都没收到
 *   ENR2=0x2581147（bit20=1）/ HXT_CR1 bit8=1 / PLL_STAT bit0=0 / CSELR=53
 *                    → 模块时钟、PMU 音频 buffer、音频 PLL、请求映射全都正常
 * 于是原来那套"重开时钟 + RSTR2 + ADCPATH SRESET/FLUSH + restore_adc"（当时
 * 连着试了 79 次）对这一类完全无效；而这一轮几乎开机就不工作（只有 14 个完成
 * 中断、13 次成功读），所以把恢复升级到 DMAC 侧/时序侧，按 recov 计数轮转下面
 * 五级，看哪一级能让请求线重新动起来：
 * **[2026-09-17 真机定案，串口原文在报告里]** 恢复收敛成**一级**：复位整块 DMAC1 +
 * 重建三个通道句柄（就是下面的 SF32LB52_AUDIO_RX_FIX_DMAC1）。
 *
 * 定案依据（同一个会话内、没有跨会话，逐级轮过来的对照实验）：
 *   L1（重排时序）              → 效果检查 irq+0 half+0  ✗
 *   L2（请求重锁存）            → 效果检查 irq+0 half+0  ✗
 *   L3（通道 DeInit+Init 重建） → 效果检查 irq+0 half+0  ✗
 *   L4（会话作废、上层重建会话）→ 新会话第一条 read 照样超时 ✗
 *   L5（复位整块 DMAC1 + 重建） → 下一条 read 50 ms 就出数据 ✓
 *     原文：`RX 恢复 L5 recov=10 armed=2 note=- ...` →
 *           `RX 健康指纹 ... CNDTR=77 ... armed_ms=50` → `[录音] 读已恢复`
 * 结论：卡住的是 **DMAC1 控制器自己的"请求接收"状态**（通道使能、CSELR 映射、
 * 外设 FIFO/标志全都正常，`CNDTR` 却一个请求都不动），只有**模块级复位**能清掉 ——
 * 重建单条通道（旧 L3）不够。顺带否掉两条假设：
 *   ✗ "codec ADC 是 APB 模式、APB FIFO 溢出反压把 ADC 停住"：好/坏两态逐位相同
 *     （ADC_CFG=0xa09、APB_STAT=0x80000、IRQ=0x10000），完好时它也在溢出。
 *   ✗ "屏幕排线/外部连线"：屏幕模组拆掉后故障一模一样，坏的是芯片内部状态。
 *
 * 仍然保留的硬约束：**只在 playback == 0（没有在播放）时才做**。DMAC1 上挂着
 * codec 播放的 DMA（hw_start 给 codec.hdma[DAC_CH0/1] 挂的就是 DMA1_Channel2/3），
 * 整块复位会当场打断正在出声的播报。录音会话的 playback 恒为 0（方向只在 hw_start
 * 里提交，一个会话只有一个方向），所以它在该用的场合总能跑；万一在播放态被轮到，
 * 只记下原因、这一次不生效（报告行里 note=skip-playback），下一次失败还会再来。 */

#define SF32LB52_AUDIO_RX_FIX_DMAC1     1   /* 唯一的恢复级：DMAC1 整块复位（仅 playback==0） */
#define SF32LB52_AUDIO_RX_FIX_LEVELS    1

/* L1/L2/L3/L5 必须"把 DMA 通道真的武装起来"才能观察/重建那条请求握手，而恢复路径
 * 不许挂调用者那块已经交还上层的 buffer（见 sf32lb52_audio_rx_recover 的说明），
 * 于是用一块驱动自有的丢弃缓冲：16 字 = 2ms@16k 单声道（半满 1ms）。
 * 为什么要这么小：判"这一级生效了没有"看的是 irq/half 有没有开始涨，而恢复之后
 * 下一次 read 可能几毫秒就回来了（上层超时后就接着读）—— 缓冲越小，"涨起来"这个
 * 证据出现得越快（循环模式下它会一直转，直到下一次 read 把它收干净）。
 * 诊断期临时件。 */

#define SF32LB52_AUDIO_RX_FIX_BUF_WORDS 16

/* 手动入口（audio_test audfix）每级之间等的时长：让那块丢弃缓冲真的被搬动几轮
 * （2ms 的十几倍余量）之后再回报，才看得出这一级有没有把通路叫活。 */

#define SF32LB52_AUDIO_RX_FIX_SETTLE_MS 30

/* "卡死之前最近发生过什么"（诊断期临时，说明见驱动结构体里那几个字段）：
 * 每种事件各记"最后一次 + 次数"，下标就是事件号；0 号不用。 */

#define SF32LB52_AUDIO_EV_MAX       7

#define SF32LB52_AUDIO_EV_SESS_BEGIN 1  /* 会话开始（hw_start） */
#define SF32LB52_AUDIO_EV_SESS_END   2  /* 会话停止（hw_stop / hw_shutdown） */
#define SF32LB52_AUDIO_EV_PLAY_BEGIN 3  /* 播放开始 */
#define SF32LB52_AUDIO_EV_PLAY_END   4  /* 播放结束 */
#define SF32LB52_AUDIO_EV_RX_ABORT   5  /* RX 通道 abort（真的动手那一次） */
#define SF32LB52_AUDIO_EV_RX_ARM     6  /* RX 武装（read 里 Receive_DMA 成功） */

/* write() 的"等待到点了"由**本驱动自己的看门狗**产生，不用内核的定时信号量等待
 * （nxsem_tickwait_uninterruptible）。
 *
 * 形状：write 一次等满"采样时长 + 500ms"，由 priv->wr_wait_wdog 这记私有心跳
 * 产生"到点了"。心跳回调只做两件事：立一个"这次超时了"的旗（wr_wait_to）、
 * **无条件** post 一次 wr_sem。它**不读也不写任何 TCB 字段**（不碰 task_state、
 * 不碰 waitobj），所以内核里那条会断言崩溃的路径（sem_timeout → sem_waitirq 读
 * waitobj）在这条等待上根本不会被执行。
 *
 * 为什么要这样（上板硬崩的定案）：现场抓到
 *     ASSERT sem_waitirq.c:137 task robot_ui
 *     nxsem_wait_irq ← nxsem_timeout ← wd_timer ← timer_callback ← systick_interrupt
 *   即"定时看门狗到期 → 那条等待的 waitobj 已经是 NULL → DEBUGASSERT(sem != NULL)"。
 *   内核的 nxsem_tickwait_slow() 是这么干的（sched/semaphore/sem_tickwait.c）：
 *       wd_start(&rtcb->waitdog, delay, nxsem_timeout, rtcb);  ← 每次武装一次
 *       nxsem_wait_slow(sem);                                  ← 阻塞在 waitobj 上
 *       wd_cancel(&rtcb->waitdog);                             ← 醒了再取消
 *   而 ISR 里的 nxsem_post()（sched/semaphore/sem_post.c）走的是
 *       wd_cancel(&stcb->waitdog) → stcb->waitobj = NULL → 任务转 ready
 *   两边都声明自己在临界区里，但本板的临界区是 **BASEPRI 型**
 *   （arch/arm/include/arm_m/irq.h:465 up_irq_save → raisebasepri(
 *     NVIC_SYSH_DISABLE_PRIORITY)，非 ARMV6M 分支），它只挡住优先级 **不低于**
 *   阈值的异常；而本驱动的 DMA 句柄是 kmm_zalloc 出来的（`Init.IrqPrio` 全场没有
 *   任何人赋值），厂商 HAL 在通道分配那一步无条件写
 *      NVIC_SetPriority(irq_type, hdma->Init.IrqPrio);
 *   （vendor/sifli/chips/drivers/hal/bf0_hal_dma.c:307）→ 优先级 0（最高），
 *   **BASEPRI 挡不住它**。于是 ISR 里的 post 可以落在 nxsem_timeout 的
 *   "查 task_state == TSTATE_WAIT_SEM"与"读 wtcb->waitobj"之间，
 *   把 waitobj 清成 NULL 而谓词检查已经通过 → 断言。
 *
 *   心跳的代价：一记 wd_start/wd_cancel（每次 write 一次），回调在 systick 中断里
 *   只置旗 + post，工作量与 DMA 中断里那次 post 同级；它和任何 TCB 都无关联，
 *   所以"取消失败 / 与 ISR post 并发"不会有断言，最坏只是多一次 post
 *   （write 开头那句 nxsem_reset 会把多余的计数清掉）。
 *
 * 注：录音 read 那条等待**不走这条路** —— 它一次等满 5 秒，由内核定时等待兜
 * （见上面"录音 RX 通路"那段）。那条等待的暴露面只有每次 read 一次武装
 * 加一次 ISR post。 */

/* AUDIO_TYPE_FEATURE + AUDIO_FU_VOLUME 该落到哪一条通路上：
 *   只有**这条录音会话自己的读取线程**来设音量，才认作"录音方向的音量配置"
 *   （改 ADC 数字增益）；其余一律按播放音量处理 —— 记进 playback_db，
 *   DAC 模拟通路真开着才顺手写寄存器。
 *
 * 为什么要这么判（报一次警就把麦克风按掉 14dB）：
 *   老判据是"没有播放意图 + 录音通路开着 → 当成录音音量"。可"没有播放意图"
 *   根本不是"想改麦克风增益"的证据：NuttX 上层 audio_configure
 *   （nuttx/audio/audio.c:479 那第二个条件）只在 AUDIO_STATE_OPEN 时才把
 *   INPUT/OUTPUT 的 CONFIGURE 放下去，而 AUDIO_TYPE_FEATURE 是**无条件**放行的。
 *   于是报警模块（sf32lb52_alarm.c:283 先 CONFIGURE(OUTPUT)、:295 再
 *   CONFIGURE(FEATURE 音量)）在 hello_app 常开麦录音期间下来时，那记
 *   CONFIGURE(OUTPUT) 被上层静默吞掉 —— 驱动看到的 pending_playback 还是
 *   false、adc_path_on 是 true，一次**播放**音量就被当成录音音量：ADC 数字增益
 *   从 +12dB（SF32LB52_AUDIO_ADC_VOL）改成 −2.4dB（ALARM_PLAYBACK_VOLUME=800 →
 *   −36 + 42×0.8），还写进了 aprc.Init.adc_cfg（之后每次 hw_start 的
 *   sf32lb52_audio_aprc_restore_adc 都复用它），于是报过一次警之后麦克风一直
 *   安静约 14dB，直到下一次 CONFIGURE(INPUT) 走 hw_configure 才恢复。
 *
 * 为什么"必须是读取线程自己来"这个判据成立：
 *   - 麦克风增益是**某一条正在录的通路**的属性，只有正在消费这条通路的那个
 *     线程（read() 每次记下的 priv->user_tid，且与起这条会话的任务组
 *     holder_pid 同组）才有资格代表"录音方向"；
 *   - 播放方向的音量配置必然来自别的线程（robot_ui 的音量滑块、报警模块），
 *     它们**绝不允许**借"没人在标播放意图"溜进 ADC 分支 —— 上层会把它们的
 *     CONFIGURE(OUTPUT) 吞掉，所以它们标不出播放意图，这一条只能由身份来兜；
 *   - 本板唯一真的想改麦克风增益的用法是手动工具（audio_test vol 那种录音时
 *     调增益），它本来就在没有会话或由读数据的线程下发，不受影响。
 *   - 反过来（身份对不上时）一律退到 playback_db：退错的代价只是"这次麦克风
 *     增益没变、播放音量记下来了"，而落错通路的代价是"麦克风被按掉 14dB"，
 *     两边不对称，宁可退。 */

/* 录音 read() 一次等待的预算（毫秒）。它的意义不是"等 5 秒才有数据"
 * （正常一帧 20~30ms 就到），而是两条：
 *   1) 谁都没来时 read 一定会返回的上界 —— app 侧
 *      audio_reap_record_thread() 敢做"确定会返回的 join"，依据就是它；
 *   2) 低于上层的任何容忍窗口（ai_audio.c 判会话死要连续 5 次超时）。
 * 注意这**不是**每帧都等满：hw_stop()/hw_shutdown() 都会无条件 post 一次
 * rx_sem 并作废会话代号，正常的 stop 收尾是立刻被唤醒的。 */
#define SF32LB52_AUDIO_RX_READ_TIMEOUT_MS   5000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct sf32lb52_audio_s
{
  struct audio_lowerhalf_s dev;         /* NuttX 音频低半部分设备 */
  AUDCODEC_HandleTypeDef  codec;        /* 音频 codec（模拟前端） */
  AUDPRC_HandleTypeDef    aprc;         /* 音频处理器（数据通路 DMA） */
  bool                    running;      /* 设备是否在运行 */
  bool                    playback;     /* 已提交方向：true=播放 false=录音（只在 running 为真时有意义） */
  bool                    pending_playback; /* 最近一次 CONFIGURE 记下的方向意图，START 成功才提交 */
  int                     playback_db; /* 当前播放音量(dB)，标准接口可改 */

  /* 上一次报"音量本该落错通路、被挡住"的时刻（tick），只做限频用。
   * 场景就是上面那段说的那一种：录音会话期间别的线程下播放音量（报警一次一条，
   * 拖音量滑块时一秒一条）。kmm_zalloc 出来是 0 = 还没报过。 */

  clock_t                 vol_block_log;

  /* 会话持有者身份 —— 用来区分"反方向那条通路是真的被别人占着"和"上一个
   * 持有者早就死了、只剩一堆没人清的状态"。
   *
   * holder_tid：hw_start 里记下的、把这次会话起起来的线程。AUDIOIOC_START 走的是
   *             调用者的上下文（nuttx/audio/audio.c 的 audio_start 直接调
   *             lower->ops->start），所以它就是调 start 的那个线程。
   * user_tid  ：最近一次 read()/write() 的线程。为什么要多这一条：起会话的线程
   *             可能是短命的工作线程 —— 本板真的有这种用法（hello_app 的
   *             audio_record_start() 在调用者线程里 audio_in_start()，真正读数据
   *             的是它随后起的录音线程）。只认 holder_tid 会把"会话明明还有人在
   *             读"误判成残留，那就变成替一个活着的会话拆台了。
   * holder_pid：起会话的任务组（getpid()，NuttX 里组长的 pid 就是任务组）。
   *             判"残留"时必须**同时**满足它也查不到 TCB —— 那才等价于"整个
   *             app 没了"。线程死了但组还在时按"有人"处理：本板真有"起会话的
   *             线程是短命线程"的用法（robot_ui 的 DETACHED 开麦线程、
   *             ai_audio 在播放线程里恢复录音后播放线程退出），只认线程会在
   *             "新会话刚 START 成功、读数据的线程还没进第一次 read"的窗口里
   *             把活会话判成残局强停（详见 sf32lb52_audio_holder_alive 的注释）。
   *             **这是有意的取舍**：代价是"app 还活着但那条播放线程真死了"这种
   *             残局会退化成 -EBUSY 等上层重试/由那个 app 自己 close 收尾，
   *             换来的是绝不误停活会话。
   * holder_since：这次会话在 hw_start 里提交成功的时刻，判"会话刚开始"用
   *             （SF32LB52_AUDIO_SWITCH_GRACE_MS，见 sf32lb52_audio_start）。
   *
   * 四个都只在 running 为真时有意义：前三个在 hw_start 里记、hw_stop/hw_shutdown
   * 清 running 时一起清。-1 = 没记过。 */

  pid_t                   holder_tid;
  pid_t                   holder_pid;
  pid_t                   user_tid;
  clock_t                 holder_since;

  /* 真实通路状态位 —— 这是"硬件到底开着哪几条模拟级"的唯一真相，
   * **不是**方向标签。
   *
   * 为什么不能拿 playback 当真相：整机是单一大镜像，priv 全局只有一份，
   * 两个 app（hello_app 常开麦录音 / robot_ui 偶尔播报）都在用它。某一个 app
   * 只做一次 CONFIGURE(OUTPUT)（例如 ai_audio.c 的 audio_hw_set_volume()，
   * 它 open + CONFIGURE + close、**不 START**）就会把正在录音的那个会话的
   * 方向标签翻成"播放"。方向标签一旦是谎话，收尾时就只有两种坏法：
   *   去关一条从没打开过的模拟通路（实测重复关模拟级会整机静默卡死），
   *   或者该关的 DAC/功放永远漏关（录音把标签翻回 false，播放侧收尾就跳过）。
   * 所以下面这三个位在真正开/关那一行如实置位/清位，收尾只看它们。 */

  bool                    dac_path_on;  /* codec DAC 模拟通路确实开着 */
  bool                    adc_path_on;  /* codec ADC 模拟通路确实开着 */
  bool                    pa_on;        /* 功放（AW8155）确实使能着 */
  int                     samplerate;   /* 当前采样率 */
  int                     nchannels;    /* 当前通道数 */
  int                     bpsamp;       /* 当前采样位深 */
  FAR struct ap_buffer_s *tx_apb;       /* 播放中的 buffer（队列模式） */
  FAR struct ap_buffer_s *rx_apb;       /* 录音中的 buffer */
  sem_t                   wr_sem;       /* write() 同步信号量 */
  bool                    wr_busy;      /* write() DMA 进行中 */
  sem_t                   rx_sem;       /* read() 同步信号量 */
  bool                    rx_busy;      /* read() DMA 进行中 */
  bool                    rx_aborted;   /* stop() 打断了正在等待的 read() */

  /* write() 那次等待（采样时长 + 500ms）的"到点了"由这记**本驱动私有**的看门狗
   * 产生（见文件顶部那段说明）。它与任何 TCB 都没有关联，回调里只置旗 + 无条件
   * post 一次信号量，所以不存在"看门狗到期时 waitobj 已被 ISR 的 post 清成 NULL"
   * 这条崩溃路径。
   *
   * wr_wait_to：这一次等待是被上面那记心跳唤醒的（而不是真有人 post 到了东西）。
   * write 靠它把"心跳到点"翻译成 -ETIMEDOUT（本块丢弃）。 */

  struct wdog_s           wr_wait_wdog; /* write() 那一次的等满心跳 */
  bool                    wr_wait_to;   /* 这次等待是心跳到点唤醒的 */

  /* read() 那次等待（5 秒上界）的"到点了"——**和 write 那一记同一套做法**，
   * 也是本驱动私有的看门狗（sf32lb52_audio_rx_wait_slice）。
   *
   * 为什么必须换掉内核那记 `nxsem_clockwait_uninterruptible`（2026-09-19 真机定案）：
   *   上板实测（串口 dump 全文在 _flash/gate_status.txt）抓到的断言就是下面这条：
   *       ASSERT sem_waitirq.c:137 task robot_ui
   *       nxsem_wait_irq ← nxsem_timeout ← wd_timer ← timer_callback ← systick_interrupt
   *   即"内核定时等待的超时回调进来时，那条等待的 waitobj 已经被别人的 post
   *   清成 NULL 了"。根因与 write 那条一模一样、也是文件顶部那段讲的同一个机制：
   *   rtcb->waitdog 与 ISR 里的 nxsem_post 抢同一份 TCB 字段，而本板的临界区是
   *   BASEPRI 型（dump 里 BASEPRI=0x80 = 只挡优先级 ≥ 8 的异常），DMA 通道中断
   *   被厂商 HAL 设成优先级 0（`NVIC_SetPriority(irq_type, hdma->Init.IrqPrio)`，
   *   而本驱动的 DMA 句柄是 kmm_zalloc 出来的、IrqPrio 全场没人赋值）。
   *   更贵的一步是**不崩的那种结局**：超时回调被抢掉（或看门狗被 ISR 的 post
   *   取消）之后，那一次等待就没有上界了 —— 真机上表现为**一次 read 在
   *   sf32lb52_audio_read 里等了 11 万毫秒还不返回**：diag 里 wait 一路涨、
   *   rxr（read 进入次数）一动不动、而 rxt（等待超时次数）恒 0，
   *   录音标志却全健康（rec=1 / ract=1 / died=0）。用户看到的
   *   "录音跑一阵就永久停摆"就是这一条，且因为 rxt=0，下面那套按超时触发的
   *   分级恢复（rx_recover / L1..）一次都没机会跑。
   *
   * 私有看门狗为什么能根治：心跳回调只做两件事（立 rx_wait_to 旗 + **无条件**
   * post 一次 rx_sem），不读也不写任何 TCB 字段 —— 内核那条 sem_waitirq 断言
   * 路径在这条等待上根本不会被走到；而心跳是从 priv 里起的一记普通看门狗，
   * ISR 的 post 取消不了它，所以"5 秒上界"是真的上界（app 侧
   * audio_reap_record_thread 敢做"一定会返回的 join"就靠它）。
   * 多出来的那一次 post 由下一次 read 开头的 nxsem_reset(&rx_sem, 0) 收掉。 */

  struct wdog_s           rx_wait_wdog; /* read() 那一次的等满心跳 */
  bool                    rx_wait_to;   /* 这次等待是心跳到点唤醒的 */

  /* DMA 传输错误（TE）打断了正在等待的 read()。
   *
   * 为什么要单独立这一面旗：TE 发生时 HAL 在中断里就把通道
   * `DMA_FreeChannel()` + `State = READY` 做完了（bf0_hal_dma.c:1228-1235），
   * 所以错误回调进来时句柄已经不是 BUSY 了 —— 光看句柄状态分不出"通道还在跑但
   * 不来数据"和"通道已经被 HAL 拆掉了"，这面旗就是那个分辨器。错误回调在中断
   * 上下文里只做三件轻活：计数、立旗、post 信号量；**不在中断里打日志** ——
   * 串口那一侧会抢控制台锁把整机挂住，本文件在播放完成（TX 中断）回调里就为此
   * 删过一次 printf，那次实测是"整机卡死"。
   * 日志和收尾都在 read() 的失败分支里做。 */

  bool                    rx_dma_err;

  /* 会话代号：每一次让"录音这次会话"作废的事件都会 +1 —— hw_start（新会话开始）、
   * hw_stop（上层 STOP）、hw_shutdown（最后一个 fd 被 close）。
   *
   * 它有两个用处：
   *   1) read() 进等待前记下它、醒来比对一次。等待的出口只有"被 post 唤醒 /
   *      rx_aborted / running 变假"三种，全都依赖**别的上下文确实把那两个标志
   *      写好或真的唤醒过它**；真机上出现过唤醒没生效（见 ai_audio.c 里"连 5 秒
   *      超时都没回来"那条记录）。有了会话代号，只要这次 read 的时代已经过去，
   *      它就自己收摊 —— 不需要任何人记得去唤醒它。
   *   2) ISR 的**会话闸**（rx_arm_gen 与它比对，等价厂商 drv_audprc.c 的
   *      g_rx_stop）。 */

  uint32_t                session_gen;

  /* -110（read 等 DMA 超时）的现场证据。
   *
   * 光看"read 超时"一句话分不清是哪一种坏法，这几个计数器就是分诊用的：
   *   irq 一直不动 + read 次数在涨  → DMA 起了但完成中断压根没来
   *                                  （最可疑：ADC 模拟通路/AUDPRC 没真使能，
   *                                    也就是 hw_start 被 start 的早退跳过了）
   *   busy_fail 在涨                → Receive_DMA 起的不是新传输（HAL 侧 BUSY）
   *   not_armed 在涨                → Receive_DMA 回了 HAL_OK，可 DMA 句柄压根没
   *                                  进 BUSY：Start_IT 没干活（它自己的 HAL_BUSY
   *                                  被厂商丢掉了，见 read 里的第二次校验）
   *   irq 在涨而 timeout 也在涨     → 中断其实来了，是等待/唤醒这一侧的问题
   *   half 在涨                     → 数据真的在流（RxHalfCplt 每 10ms 一次，
   *                                   20ms 一帧的半满）；irq 冻结而 half 还在涨
   *                                   说明"数据是通的、只有 TC 那一路丢了"，
   *                                   和"数据源头压根没动"是两种病
   *   dma_err 在涨 + irq_delta==0   → DMA 传输错误(TE)：HAL 已经 FreeChannel +
   *                                   State=READY，所以 hdma[RX].State=0x1 而
   *                                   irq 不动，**这一组合就是 TE 的指纹**
   *   recov 在涨                    → 那一次 read 失败之后确实把通道收干净了；
   *                                   irq_delta==0 且 half_delta==0 时还会跟着
   *                                   一行"RX 恢复 L?"日志（那一级动了哪一步见
   *                                   文件顶部那段分级编排）
   *
   * 都是单调递增的 32bit 计数，不做回绕保护（真跑到回绕早该查完这件事了）。
   * 失败日志里的 aprc.State[RX] / hdma[RX].State / CNDTR / CCR / ISR 全是**停机
   * 之前**采样的（abort 会把它们刷成刚写进去的值，停机后再采就没意义了），所以
   * 读法是"等待期间通道是不是一直武装着（BUSY）、CNDTR 动过没有"。 */

  uint32_t                rx_irq_count;       /* RxCplt 中断次数（无条件自增） */
  uint32_t                rx_half_irq_count;  /* RxHalfCplt 中断次数（"数据在流"的心跳） */
  uint32_t                rx_read_count;      /* read() 进入次数 */
  uint32_t                rx_timeout_count;   /* read() 一次等待超时的次数 */
  uint32_t                rx_dma_busy_fail;   /* 起 DMA 返回非 HAL_OK 的次数 */
  uint32_t                rx_dma_not_armed;   /* 声称起好了但句柄没进 BUSY 的次数 */
  uint32_t                rx_dma_err_count;   /* DMA 传输错误(TE) 中断次数 */
  uint32_t                rx_recover_count;   /* read 失败之后的恢复次数（收干净 = 下一次
                                               * read 的 Receive_DMA 能落在干净的 READY
                                               * 上；零活动那几次还跑了分级动作，
                                               * 见 sf32lb52_audio_rx_recover） */

  /* 诊断期临时：上一级恢复的"待验证"账（那一次用了哪一级、以及应用它那一刻的
   * irq/half 计数）。下一次 read 一进来就按这三个数打"效果检查" —— 判"这一级
   * 生效了没有"的唯一标准就是 half/irq 有没有重新开始涨。定案后一起删。 */

  uint32_t                rx_fix_level;       /* 待验证的级（0 = 没有待验证的） */
  uint32_t                rx_fix_irq;         /* 应用那一级时的 rx_irq_count */
  uint32_t                rx_fix_half;        /* 应用那一级时的 rx_half_irq_count */
  FAR const char         *rx_fix_note;        /* 这一级要额外说明的（NULL = 无；例如
                                               * L5 被 playback 守卫挡下时写
                                               * "skip-playback"），由 rx_fix_report
                                               * 打进那一行（note=%s） */

  /* 健康指纹（同样是诊断期临时件，目的和上面那几笔账一样：定案"DMAC 为什么收不到
   * 请求"）。失败行只告诉你坏的时候长什么样；没有"好的时候长什么样"就没法逐位
   * 对比，于是每次会话的**第一条成功 read** 打一行字段完全对齐的快照 —— 采样点也
   * 对齐（都在停机之前，见 read 里"现场快照必须在停 DMA 之前取"那一段）。
   *   rx_health_done：本会话已经打过健康指纹了（hw_start 清 0），一次会话只打一行；
   *   rx_arm_tick   ：这一次 read 确认武装成功（句柄进 BUSY）的时刻 —— 健康行里的
   *                   armed_ms 就是它到快照那一刻的毫秒数，回答"武装之后多久
   *                   第一块数据到"。定案后随分级编排一起删。 */

  bool                    rx_health_done;
  clock_t                 rx_arm_tick;

  /* ── "卡死之前最近发生过什么"（诊断期临时）──
   * 现在这条通路已经能自己救回来（复位整块 DMAC1），但还缺最后一块拼图：**是什么
   * 把 DMAC1 的请求接收状态卡住的**。于是每种关键事件各记一笔"最后一次发生的时刻 +
   * 累计次数"，失败行里**全部**打出来（`hist=arm-40msx15000 play_end-6100msx3 ...`）。
   * 为什么不是环形队列：每帧都有一次 arm/abort（25 次/秒），环几格就被这些琐事挤满，
   * play_end / sess_begin 那种稀有事件反而被冲掉 —— 而那恰恰是要看的东西。
   *   1..6 = SF32LB52_AUDIO_EV_*（见定义处）；ev_last == 0 表示从没发生过。
   * 单写多读、故意不加锁：这是诊断量、不是判据（先写 tick、后写计数）。 */

  clock_t                 ev_last[SF32LB52_AUDIO_EV_MAX];
  uint32_t                ev_cnt[SF32LB52_AUDIO_EV_MAX];

  /* TE 那条日志要限频：超时本身一次要等满 5 秒、天然每秒最多一条，而 TE 会让
   * read **立刻**返回，一个持续性的 TE 就是"上层每次重试一条"，不加窗口能把
   * 串口刷满、把别的线索冲掉。窗口内只累计，出窗口时把被吞掉的条数一起报出来。 */

  clock_t                 rx_dma_err_log_next; /* 下一次允许打 TE 日志的时刻 */
  uint32_t                rx_dma_err_silenced; /* 限频窗口内被吞掉的 TE 日志条数 */

  /* 帧长/半满的正确口径（读代码核对过，别再按"半满 = 完成的两倍"去判读）。
   *
   * 一帧：app 的 AUDIO_DEFAULT_FRAME_MS(20ms) @ 16k 单声道 16bit
   *       = 320 样本 = 640 字节；read() **每次整帧**起一次 DMA
   *       （HAL_AUDPRC_Receive_DMA 的 dataSize = Size >> 2 = 160，
   *        bf0_hal_audprc.c:793；DMA 是 WORD 对齐 + DMA_CIRCULAR，
   *        bf0_hal_audprc.c:682-686），所以 CNDTR 的满值就是 160。
   * 半满：CNDTR 走掉一半（80 字 = 10 ms）时硬件置 HTIF —— 阈值是全长的一半，
   *       不是等于全长，也不存在"每次 arm 只搬半帧"（上面那条 dataSize 就是整帧）。
   * 本驱动一次成型的 read 等的是**整块写完的那一次 TC**（HT 只是一记心跳，既不
   *       交数据也不叫醒读者：半满时缓冲里还只有一半数据，叫醒等于把半块没写过
   *       的东西交上去）。所以一个 arm 周期里 half 应当只比 irq 多 0~1 次；
   *       half 一直涨而 irq 不动 = 数据在流、只有 TC 那一路丢了；两个都不涨 =
   *       数据源头真停了。**绝不能按 2 倍关系判读。** */

  /* arm 的两笔账。只在**确认武装成功**（read 里那道 not_armed 校验通过）之后
   * 自增，所以它们数的是"真的起过 DMA 的次数"，不是"read 进来过几次"：
   *   rx_arm_session 回答"这一次失败落在本会话的第几帧"（hw_start 清零），
   *   rx_arm_seq     回答"开机以来第几帧"。 */

  uint32_t                rx_arm_seq;          /* 开机以来确认武装成功的 arm 次数 */
  uint32_t                rx_arm_session;      /* 本会话第几次 arm（hw_start 清零） */

  /* 会话闸（等价厂商 drv_audprc.c 里的 g_rx_stop）：武装那一刻记下本次 read 的
   * 会话代号，ISR 进门前比一次 —— 代号对不上就说明这一代会话已经被 stop/close/
   * 新会话作废了，那次完成中断属于一条已经没人要的传输，**只计数、不 post**。
   *
   * 为什么需要它：read 的等待是有上界的（5 秒），而通道的收尾在 read 自己手里；
   * 一次 stop 之后 ISR 若还往 rx_sem 里 post，就会给**下一次** read 留下一个假
   * 唤醒（它会把一块没采满的缓冲当数据交上去）。nxsem_reset 挡不住这种"下一次
   * 会话刚开始时飘进来的上一次的 post"，会话闸才是那道门。hw_start 里把它清 0
   * （= 闸门关着），read 武装成功的那一刻才写成本次会话的代号（= 闸门开）。 */

  uint32_t                rx_arm_gen;

};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int  sf32lb52_audio_getcaps(FAR struct audio_lowerhalf_s *dev, int type,
                                   FAR struct audio_caps_s *caps);
static int  sf32lb52_audio_configure(FAR struct audio_lowerhalf_s *dev,
                                     FAR const struct audio_caps_s *caps);
static int  sf32lb52_audio_shutdown(FAR struct audio_lowerhalf_s *dev);
static int  sf32lb52_audio_start(FAR struct audio_lowerhalf_s *dev);
#ifndef CONFIG_AUDIO_EXCLUDE_STOP
static int  sf32lb52_audio_stop(FAR struct audio_lowerhalf_s *dev);
#endif
static int  sf32lb52_audio_enqueuebuffer(FAR struct audio_lowerhalf_s *dev,
                                         FAR struct ap_buffer_s *apb);
static int  sf32lb52_audio_cancelbuffer(FAR struct audio_lowerhalf_s *dev,
                                        FAR struct ap_buffer_s *apb);
static int  sf32lb52_audio_ioctl(FAR struct audio_lowerhalf_s *dev, int cmd,
                                 unsigned long arg);
static ssize_t sf32lb52_audio_write(FAR struct audio_lowerhalf_s *dev,
                                    FAR const char *buffer, size_t buflen);
static ssize_t sf32lb52_audio_read(FAR struct audio_lowerhalf_s *dev,
                                   FAR char *buffer, size_t buflen);
static int  sf32lb52_audio_reserve(FAR struct audio_lowerhalf_s *dev);
static int  sf32lb52_audio_release(FAR struct audio_lowerhalf_s *dev);

static int  sf32lb52_audio_hw_init(FAR struct sf32lb52_audio_s *priv);
static int  sf32lb52_audio_hw_configure(FAR struct sf32lb52_audio_s *priv,
                                        int samplerate, int nchannels,
                                        int bpsamp);
static int  sf32lb52_audio_hw_start(FAR struct sf32lb52_audio_s *priv,
                                    bool playback);
static int  sf32lb52_audio_hw_stop(FAR struct sf32lb52_audio_s *priv);
static int  sf32lb52_audio_hw_shutdown(FAR struct sf32lb52_audio_s *priv);
static void sf32lb52_audio_pa_enable(bool enable);
static bool sf32lb52_audio_tid_alive(pid_t tid);
static bool sf32lb52_audio_holder_alive(FAR struct sf32lb52_audio_s *priv);
static void sf32lb52_audio_path_bit(FAR bool *bit, bool on,
                                    FAR const char *name,
                                    FAR const char *why);
static void sf32lb52_audio_tx_freeze(FAR struct sf32lb52_audio_s *priv);
static void sf32lb52_audio_tx_complete(FAR struct sf32lb52_audio_s *priv);
/* 队列模式的 buffer 归还：RX 完成中断里"没人在等"时用它把 buffer 交还上层
 * （直通 read() 那条路上 rx_apb 是 NULL，函数第一句就返回）。 */
static void sf32lb52_audio_rx_complete(FAR struct sf32lb52_audio_s *priv);
static int  sf32lb52_audio_dma1_irq(int irq, FAR void *context,
                                    FAR void *arg);
static bool sf32lb52_audio_aprc_has_config(FAR struct sf32lb52_audio_s *priv);
static void sf32lb52_audio_aprc_soft_reset(FAR struct sf32lb52_audio_s *priv);
static void sf32lb52_audio_aprc_restore_adc(FAR struct sf32lb52_audio_s *priv);
static void sf32lb52_audio_rx_dma_stop_frame(FAR struct sf32lb52_audio_s *priv);

/* RX 通道的两条收尾原语：前者"只在确实还武装着时才 abort"（理由见实现处），
 * 后者 = 前者 + 计数，read 失败时用它（平时只收尾，"这 5 秒零活动"时在同一
 * 条路上接着做分级动作，判据就是传进去的两个观察量）。 */
static void sf32lb52_audio_rx_dma_stop_if_armed(FAR struct sf32lb52_audio_s *priv);
static uint32_t sf32lb52_audio_rx_dma_req(FAR DMA_HandleTypeDef *hdma);
static uint32_t sf32lb52_audio_rx_fix_arm(FAR struct sf32lb52_audio_s *priv);
static uint32_t sf32lb52_audio_rx_fix_apply(FAR struct sf32lb52_audio_s *priv,
                                            uint32_t level);
static void sf32lb52_audio_rx_fix_report(FAR struct sf32lb52_audio_s *priv,
                                         uint32_t level, uint32_t recov,
                                         uint32_t armed, uint32_t settle_ms,
                                         uint32_t irq0, uint32_t half0);
static int  sf32lb52_audio_rx_recover(FAR struct sf32lb52_audio_s *priv,
                                      uint32_t irq_delta, uint32_t half_delta);

/* 诊断期临时的**手动入口**（nsh: `audio_test audfix`：打一次 RX 恢复，也就是复位
 * 整块 DMAC1 + 重建三个通道句柄）。原型在本文件里声明一笔：本轮不改板级头文件
 * sf32lb52_audio_in.h（那边另一个任务在用），所以调它的一方
 * （app/audio_test/main.c）也各自声明一份。定案后删。 */

int sf32lb52_audio_rx_fix(int level);

/* write() / read() 在这个定义之前，先声明一笔。 */

static int  sf32lb52_audio_tx_wait_slice(FAR struct sf32lb52_audio_s *priv,
                                         uint32_t wait_ms);
static int  sf32lb52_audio_rx_wait_slice(FAR struct sf32lb52_audio_s *priv,
                                         uint32_t wait_ms);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 本驱动那**唯一一个** priv（sf32lb52_audio_initialize 里 kmm_zalloc 出来的）。
 *
 * 原来 priv 只活在 initialize 的栈上：驱动内部所有路径都是"从 dev 反推 priv"
 * （offsetof 那一手，见各个 HAL 回调），模块外面根本拿不到它。下面这个指针
 * 只为 sf32lb52_audio_rx_stats() 存在 —— 那个只读观测入口要被**别的任务**
 * （hello_app 的主循环、robot_ui 的 MQTT 收包线程）调用，没有它就只能去
 * open() 一遍设备，而那正是观测最不该做的事。
 *
 * 只写一次、只在初始化成功之后写；读的地方全部只读。它不是一条新的通路，
 * 谁都不许从这里伸手去改状态（理由见 sf32lb52_audio_rx_stats 的说明）。 */

static FAR struct sf32lb52_audio_s *g_sf32lb52_audio_priv;

/* SF32LB52X codec 时钟配置表（采样率 → 内部 PLL/分频参数，参考 SDK） */

static const AUDCODE_DAC_CLK_CONFIG_TYPE codec_dac_clk_config[SF32LB52_AUDIO_CLK_TAB_NUM] =
{
  {48000, 0, 1, 0, 0x14D, 0,  5, 4, 2, 20, 20, 0},
  {32000, 0, 1, 1, 0x14D, 0,  5, 4, 2, 20, 20, 0},
  {24000, 0, 1, 5, 0x14D, 0, 10, 2, 2, 10, 10, 1},
  {16000, 0, 1, 4, 0x14D, 0,  5, 4, 2, 20, 20, 0},
  {12000, 0, 1, 7, 0x14D, 0, 20, 2, 1,  5,  5, 1},
  { 8000, 0, 1, 8, 0x14D, 0, 10, 2, 2, 10, 10, 1},
  {44100, 1, 1, 0, 0x14D, 1,  5, 4, 2, 20, 20, 0},
  {22050, 1, 1, 5, 0x14D, 1, 10, 2, 2, 10, 10, 1},
  {11025, 1, 1, 7, 0x14D, 1, 20, 2, 1,  5,  5, 1},
};

static const AUDCODE_ADC_CLK_CONFIG_TYPE codec_adc_clk_config[SF32LB52_AUDIO_CLK_TAB_NUM] =
{
  {48000, 0,  5, 0, 0, 1, 5, 0},
  {32000, 0,  5, 1, 0, 1, 5, 0},
  {24000, 0, 10, 0, 0, 0, 5, 2},
  {16000, 0, 10, 1, 0, 0, 5, 2},
  {12000, 0, 10, 2, 0, 0, 5, 2},
  { 8000, 0, 10, 3, 0, 0, 5, 2},
  {44100, 1,  5, 0, 1, 1, 5, 1},
  {22050, 1,  5, 2, 1, 1, 5, 1},
  {11025, 1, 10, 2, 1, 0, 5, 3},
};

static const struct audio_ops_s g_sf32lb52_audio_ops =
{
  sf32lb52_audio_getcaps,       /* getcaps        */
  sf32lb52_audio_configure,     /* configure      */
  sf32lb52_audio_shutdown,      /* shutdown       */
  sf32lb52_audio_start,         /* start          */
#ifndef CONFIG_AUDIO_EXCLUDE_STOP
  sf32lb52_audio_stop,          /* stop           */
#endif
  NULL,                         /* pause          */
  NULL,                         /* resume         */
  NULL,                         /* allocbuffer    */
  NULL,                         /* freebuffer     */
  sf32lb52_audio_enqueuebuffer, /* enqueue_buffer */
  sf32lb52_audio_cancelbuffer,  /* cancel_buffer  */
  sf32lb52_audio_ioctl,         /* ioctl          */
  sf32lb52_audio_read,          /* read           */
  sf32lb52_audio_write,         /* write          */
  sf32lb52_audio_reserve,       /* reserve        */
  sf32lb52_audio_release        /* release        */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sf32lb52_audio_pa_enable
 *
 * Description: 控制板上音频功放（AW8155，PA10 = AU_PA_EN）
 *
 ****************************************************************************/

static void sf32lb52_audio_pa_enable(bool enable)
{
  HAL_GPIO_WritePin(SF32LB52_AUDIO_PA_GPIO, SF32LB52_AUDIO_PA_PIN,
                    enable ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/****************************************************************************
 * Name: sf32lb52_audio_path_bit
 *
 * Description:
 *   改一个真实通路状态位，值真的变了才打一行 LOG_INFO（位名 + 为什么）。
 *
 *   为什么不直接 syslog：hw_start/hw_stop 每次会话都会跑到这里，无条件打
 *   就是刷屏；而"哪条模拟通路在什么时候被开了/关了"正是收尾判断的依据，
 *   串口上必须能一条一条对着看（本次改动的验收判据）。
 *   只在值变化时打，正常一次会话最多 3 行（DAC/PA/ADC 各一行）。
 *
 ****************************************************************************/

static void sf32lb52_audio_path_bit(FAR bool *bit, bool on,
                                    FAR const char *name,
                                    FAR const char *why)
{
  if (*bit == on)
    {
      return;
    }

  *bit = on;
  syslog(LOG_INFO, "AUDIO: 通路位 %s -> %d（%s）\n",
         name, (int)on, why);
}

/****************************************************************************
 * Name: sf32lb52_audio_tx_freeze
 *
 * Description:
 *   在 DMA 完成中断里**立刻**掐断 codec 的 DAC 数据通路。
 *
 *   为什么必须在中断里做：HAL_AUDCODEC_Transmit_DMA() 把 TX DMA 设成
 *   DMA_CIRCULAR（见 bf0_hal_audcodec_m.c 的 HAL_AUDCODEC_DMA_Init /
 *   HAL_AUDCODEC_Transmit_DMA），这一块传到末尾的**同时**硬件就从头再传一遍。
 *   而"停下来"原来只有 write() 被信号量唤醒之后才会调 HAL_AUDCODEC_DMAStop()，
 *   中间隔着"中断返回 → 调度器唤醒线程 → 线程真的跑起来"这段软件延迟
 *   （几十 us 到 ms 量级，看有没有更高优先级任务在跑）。这段时间里缓冲区
 *   开头会被重播，块尾跳到块首那一下就是一声"啪" —— 100 ms 一块时一秒十声，
 *   上层听到的就是"每个字都有爆音"。
 *
 *   这里在中断里直接关掉两样东西，重播长度就压到"中断入口延迟"这一个量级
 *   （不到一个 32bit 字，16k 单声道下约 2 个采样），上层再做几毫秒的淡入淡出
 *   （见 app/hello_app/ai_audio.c 的 audio_apply_chunk_fade）就完全盖住了。
 *
 *   只写寄存器，不做 HAL_DMA_Abort 那套重活（禁中断 / 回收通道 / 复位 State）：
 *   那些收尾仍然留给 write() 里的 HAL_AUDCODEC_DMAStop()。
 ****************************************************************************/

static void sf32lb52_audio_tx_freeze(FAR struct sf32lb52_audio_s *priv)
{
  FAR DMA_HandleTypeDef *hdma = priv->codec.hdma[HAL_AUDCODEC_DAC_CH0];

  /* 先关 codec 这一侧的 DMA 使能：数据不再往 DAC 里送，重播当头断掉。
   * 这一位 write() 收尾时也会清（HAL_AUDCODEC_DMAStop），这里是抢在它前面。 */

  priv->codec.Instance->DAC_CH0_CFG &= ~AUDCODEC_DAC_CH0_CFG_DMA_EN;

  /* 再把 DMA 通道停掉。只清 CCR 的 EN 位；Instance 用的是 HAL 动态分配之后
   * 实际占住的那个通道（DMA_AllocChannel 可能改过 Instance），不能写死。 */

  if (hdma != NULL)
    {
      __HAL_DMA_DISABLE(hdma);
    }
}

/****************************************************************************
 * Name: sf32lb52_audio_tx_complete
 *
 * Description:
 *   DMA 完成处理（HAL 在 DMA 中断里调用，见文件底部 HAL_AUDPRC_*
 *   CpltCallback 重写），向 NuttX 上层归还 buffer。
 *
 ****************************************************************************/

static void sf32lb52_audio_tx_complete(FAR struct sf32lb52_audio_s *priv)
{
  FAR struct ap_buffer_s *apb = priv->tx_apb;
  bool final;

  priv->tx_apb = NULL;

  if (apb == NULL)
    {
      return;
    }

  final = (apb->flags & AUDIO_APB_FINAL) != 0;

  priv->dev.upper(priv->dev.priv, AUDIO_CALLBACK_DEQUEUE, apb, OK);
  if (final)
    {
      priv->dev.upper(priv->dev.priv, AUDIO_CALLBACK_COMPLETE, NULL, OK);
    }
}

/****************************************************************************
 * Name: sf32lb52_audio_rx_complete
 *
 * Description:
 *   录音侧的同名收尾，只服务于队列模式（AUDIOIOC_ENQUEUEBUFFER 那条路：
 *   buffer 挂在上层投递的 ap_buffer 上，由 ISR 在"没人在等"时直接归还）。
 *   直通 read() 那条路上 rx_apb 恒为 NULL，本函数第一句就返回。
 *
 ****************************************************************************/

static void sf32lb52_audio_rx_complete(FAR struct sf32lb52_audio_s *priv)
{
  FAR struct ap_buffer_s *apb = priv->rx_apb;
  bool final;

  priv->rx_apb = NULL;

  if (apb == NULL)
    {
      return;
    }

  final = (apb->flags & AUDIO_APB_FINAL) != 0;

  priv->dev.upper(priv->dev.priv, AUDIO_CALLBACK_DEQUEUE, apb, OK);
  if (final)
    {
      priv->dev.upper(priv->dev.priv, AUDIO_CALLBACK_COMPLETE, NULL, OK);
    }
}

/****************************************************************************
 * Name: sf32lb52_audio_dma1_irq
 *
 * Description:
 *   DMA1 通道中断统一入口（把 NuttX IRQ 转给 HAL 的通道池处理，
 *   HAL 会调用对应的 XferCpltCallback → 本驱动的 Tx/RxCpltCallback）
 *
 ****************************************************************************/

static int sf32lb52_audio_dma1_irq(int irq, FAR void *context,
                                   FAR void *arg)
{
  int idx = irq - NVIC_IRQ_FIRST - DMAC1_CH1_IRQn;

  switch (idx)
    {
      case 0:  HAL_DMAC1_CH1_IRQHandler();  break;
      case 1:  HAL_DMAC1_CH2_IRQHandler();  break;
      case 2:  HAL_DMAC1_CH3_IRQHandler();  break;
      case 3:  HAL_DMAC1_CH4_IRQHandler();  break;
      case 4:  HAL_DMAC1_CH5_IRQHandler();  break;
      case 5:  HAL_DMAC1_CH6_IRQHandler();  break;
      case 6:  HAL_DMAC1_CH7_IRQHandler();  break;
      case 7:  HAL_DMAC1_CH8_IRQHandler();  break;
      default: break;
    }

  return OK;
}

/****************************************************************************
 * Name: sf32lb52_audio_hw_init
 *
 * Description:
 *   codec + AUDPRC 硬件初始化：
 *   电源/时钟使能 → HAL Init → AUDPRC TX/RX DMA 句柄配置 → DMA 中断注册
 *
 ****************************************************************************/

static int sf32lb52_audio_hw_init(FAR struct sf32lb52_audio_s *priv)
{
  HAL_StatusTypeDef res;
  int i;

  memset(&priv->codec, 0, sizeof(priv->codec));
  memset(&priv->aprc, 0, sizeof(priv->aprc));

  /* 寄存器基址（HAL 初始化会直接访问 Instance） */

  priv->codec.Instance = (AUDCODEC_TypeDef *)AUDCODEC_BASE;
  priv->aprc.Instance  = (AUDPRC_TypeDef *)AUDPRC_BASE;

  /* 打开音频电源和 codec/AUDPRC 时钟 */

  syslog(LOG_ERR, "AUDIO: PMU enable\n");
  HAL_PMU_EnableAudio(1);
  HAL_RCC_EnableModule(RCC_MOD_AUDCODEC_HP);
  HAL_RCC_EnableModule(RCC_MOD_AUDCODEC_LP);
  HAL_RCC_EnableModule(RCC_MOD_AUDPRC);
  /* DMAC1 时钟：AUDPRC TX0/RX0 DMA 走 DMAC1，必须先开时钟，否则
   * HAL_AUDPRC_Transmit_DMA 返回 OK 但 DMA 永不传输/无完成中断 */
  HAL_RCC_EnableModule(RCC_MOD_DMAC1);
  syslog(LOG_ERR, "AUDIO: clocks ok\n");

  /* codec 基本配置（模拟前端） */

  priv->codec.Init.en_dly_sel      = 0;
  priv->codec.Init.dac_cfg.opmode  = 1;
  priv->codec.Init.adc_cfg.opmode  = 1;
  priv->codec.Init.samplerate_index = 3;   /* 默认 16k，configure 时更新 */

  /* codec 自带 DMA 播放通路的 DMA 句柄延迟到第一次播放时再挂（见 hw_start）——
   * 在板级 bringup 阶段做会让启动挂住（实测黑屏） */

  syslog(LOG_ERR, "AUDIO: codec init...\n");
  res = HAL_AUDCODEC_Init(&priv->codec);
  syslog(LOG_ERR, "AUDIO: codec init res=%d\n", res);
  if (res != HAL_OK)
    {
      return -EIO;
    }

  /* AUDPRC 初始化（数据通路） */

  priv->aprc.Init.clk_div = 1;   /* 音频主时钟分频（非 ASIC 芯片） */
  priv->aprc.Init.adc_div = 1;
  priv->aprc.Init.dac_div = 1;

  /* DMA 句柄必须在 HAL_AUDPRC_Init() 之前分配并填好 Instance/Request：
   * HAL 会用音频正确的参数初始化它们（WORD 对齐 + DMA_CIRCULAR + 高优先级）。
   * 若在 Init 之后才分配（或自己配成 HALFWORD/NORMAL），AUDPRC 的 32bit FIFO
   * 收不到正确数据字，表现为播放只有开关爆音、无实际音频。 */

  priv->aprc.hdma[HAL_AUDPRC_TX_CH0] = kmm_zalloc(sizeof(DMA_HandleTypeDef));
  priv->aprc.hdma[HAL_AUDPRC_RX_CH0] = kmm_zalloc(sizeof(DMA_HandleTypeDef));
  if (priv->aprc.hdma[HAL_AUDPRC_TX_CH0] == NULL ||
      priv->aprc.hdma[HAL_AUDPRC_RX_CH0] == NULL)
    {
      auderr("ERROR: Failed to alloc DMA handle\n");
      return -ENOMEM;
    }

  priv->aprc.hdma[HAL_AUDPRC_TX_CH0]->Instance     = AUDPRC_TX0_DMA_INSTANCE;
  priv->aprc.hdma[HAL_AUDPRC_TX_CH0]->Init.Request = AUDPRC_TX0_DMA_REQUEST;
  priv->aprc.hdma[HAL_AUDPRC_RX_CH0]->Instance     = AUDPRC_RX0_DMA_INSTANCE;
  priv->aprc.hdma[HAL_AUDPRC_RX_CH0]->Init.Request = AUDPRC_RX0_DMA_REQUEST;

  syslog(LOG_ERR, "AUDIO: aprc init...\n");
  res = HAL_AUDPRC_Init(&priv->aprc);
  syslog(LOG_ERR, "AUDIO: aprc init res=%d\n", res);
  if (res != HAL_OK)
    {
      return -EIO;
    }

  /* 功放使能引脚 PA10(AUDIO_PA_CTRL)：必须先 HAL_GPIO_Init 设为输出，
   * 否则 WritePin 无效、AW8155 功放始终关闭（参照板级 BSP_GPIO_Set） */

  {
    GPIO_InitTypeDef pa_init;

    pa_init.Mode = GPIO_MODE_OUTPUT;
    pa_init.Pin  = SF32LB52_AUDIO_PA_PIN;
    pa_init.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(SF32LB52_AUDIO_PA_GPIO, &pa_init);
    HAL_GPIO_WritePin(SF32LB52_AUDIO_PA_GPIO, SF32LB52_AUDIO_PA_PIN,
                      GPIO_PIN_RESET);
  }

  /* 注册 DMA1 通道中断（AUDPRC TX/RX 数据传输完成依赖 DMA 中断） */

  for (i = 0; i < 8; i++)
    {
      int irq = DMAC1_CH1_IRQn + i + NVIC_IRQ_FIRST;
      int r1;

      r1 = irq_attach(irq, sf32lb52_audio_dma1_irq, priv);
      up_enable_irq(irq);
    }

  audinfo("SF32LB52 audio hw initialized\n");
  return OK;
}

/****************************************************************************
 * Name: sf32lb52_audio_hw_configure
 *
 * Description: 按采样率/通道/位深配置 codec 通路（参考 SDK bf0_audio_configure）
 *
 ****************************************************************************/

static int sf32lb52_audio_hw_configure(FAR struct sf32lb52_audio_s *priv,
                                       int samplerate, int nchannels,
                                       int bpsamp)
{
  AUDCODEC_HandleTypeDef *codec = &priv->codec;
  int i;

  for (i = 0; i < SF32LB52_AUDIO_CLK_TAB_NUM; i++)
    {
      if (samplerate == codec_dac_clk_config[i].samplerate)
        {
          codec->Init.samplerate_index = i;
          codec->Init.dac_cfg.dac_clk = (FAR AUDCODE_DAC_CLK_CONFIG_TYPE *)
                                        &codec_dac_clk_config[i];
          codec->Init.adc_cfg.adc_clk = (FAR AUDCODE_ADC_CLK_CONFIG_TYPE *)
                                        &codec_adc_clk_config[i];
          break;
        }
    }

  if (i >= SF32LB52_AUDIO_CLK_TAB_NUM)
    {
      auderr("ERROR: Unsupported samplerate %d\n", samplerate);
      return -EINVAL;
    }

  /* 配置 codec 的 DAC（播放）和 ADC（录音）通道 */

  HAL_AUDCODEC_Config_TChanel(codec, SF32LB52_AUDIO_DAC_CH,
                              &codec->Init.dac_cfg);
  HAL_AUDCODEC_Config_RChanel(codec, SF32LB52_AUDIO_ADC_CH,
                              &codec->Init.adc_cfg);

  /* 配置 AUDPRC 的 TX（播放）和 RX（录音）数据通道
   * （参考 SDK drv_audprc configure：使能通道 + 数据格式）
   *
   * 第二个参数是 HAL 的**通道号 0/1**，不是 DMA 枚举值：
   * HAL_AUDPRC_Config_TChanel/Config_RChanel 只认 0/1，别的一律走 default
   * 分支返回 HAL_ERROR 什么都不写（bf0_hal_audprc.c:293 / :333）。
   * TX0 恰好等于 0 所以一直是对的；RX 原来传的是 DMA 枚举 HAL_AUDPRC_RX_CH0(=4)，
   * 于是这一行 RX 配置**从来没生效过**（返回值也没人看）—— RX_CH0_CFG 只在
   * Receive_DMA 里被置过"通道使能/清 DMA 掩码"，格式/单双声道位一直是 0。
   * 当前 16bit 单声道下两者的寄存器值正好一样（本次改动对现场行为无影响），
   * 但换成 24bit/立体声就会静默用错格式。
   */

  {
    AUDPRC_ChnlCfgTypeDef cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.dma_mask = 0;
    cfg.en       = 1;
    cfg.format   = (bpsamp == 16) ? 0 : 1;
    cfg.mode     = (nchannels == 1) ? 0 : 1;

    HAL_AUDPRC_Config_TChanel(&priv->aprc,
                              SF32LB52_AUDIO_PRC_TX_CH - HAL_AUDPRC_TX_CH0,
                              &cfg);
    HAL_AUDPRC_Config_RChanel(&priv->aprc,
                              SF32LB52_AUDIO_PRC_RX_CH - HAL_AUDPRC_RX_CH0,
                              &cfg);
  }

  /* AUDPRC 采样时钟分频（参考 SDK bf0_audprc_src：按采样率查表，
   * 16k → clk_div 3000，xtal 时钟源）
   */

  priv->aprc.Init.adc_div = 3000;
  priv->aprc.Init.dac_div = 3000;
  priv->aprc.Init.clk_sel = 0;
  __HAL_AUDPRC_CLK_XTAL(&priv->aprc);
  __HAL_AUDPRC_STB_DIV_CLK(&priv->aprc, priv->aprc.Init.adc_div,
                           priv->aprc.Init.dac_div);

  /* AUDPRC 通路初始化（参考 SDK bf0_adc_dac_path_cfg_init：
   * Init.adc_cfg / Init.dac_cfg 字段 → Config_ADCPath/DACPath 写寄存器）
   */

  priv->aprc.Init.adc_cfg.src_hbf3_mode = 0;
  priv->aprc.Init.adc_cfg.src_hbf3_en   = 0;
  priv->aprc.Init.adc_cfg.src_hbf2_mode = 0;
  priv->aprc.Init.adc_cfg.src_hbf2_en   = 0;
  priv->aprc.Init.adc_cfg.src_hbf1_mode = 0;
  priv->aprc.Init.adc_cfg.src_hbf1_en   = 0;
  priv->aprc.Init.adc_cfg.src_ch_en     = 0;
  priv->aprc.Init.adc_cfg.rx2tx_loopback = 0;
  priv->aprc.Init.adc_cfg.data_swap     = 0;
  priv->aprc.Init.adc_cfg.src_sel       = 0;
  priv->aprc.Init.adc_cfg.vol_l         = SF32LB52_AUDIO_ADC_VOL;
  priv->aprc.Init.adc_cfg.vol_r         = SF32LB52_AUDIO_ADC_VOL;

  priv->playback_db                     = SF32LB52_AUDIO_DEFAULT_VOL;
  priv->aprc.Init.adc_cfg.src_sinc_en   = 0;
  priv->aprc.Init.adc_cfg.sinc_ratio    = 0;

  priv->aprc.Init.dac_cfg.dst_sel       = 0;
  priv->aprc.Init.dac_cfg.vol_l         = 0;
  priv->aprc.Init.dac_cfg.vol_r         = 0;
  priv->aprc.Init.dac_cfg.src_hbf3_mode = 0;
  priv->aprc.Init.dac_cfg.src_hbf3_en   = 0;
  priv->aprc.Init.dac_cfg.src_hbf2_mode = 0;
  priv->aprc.Init.dac_cfg.src_hbf2_en   = 0;
  priv->aprc.Init.dac_cfg.src_hbf1_mode = 0;
  priv->aprc.Init.dac_cfg.src_hbf1_en   = 0;
  priv->aprc.Init.dac_cfg.src_ch_en     = 0;
  priv->aprc.Init.dac_cfg.eq_clr        = 0;
  priv->aprc.Init.dac_cfg.eq_stage      = 1;
  priv->aprc.Init.dac_cfg.eq_ch_en      = 0;
  priv->aprc.Init.dac_cfg.src_sinc_en   = 0;
  priv->aprc.Init.dac_cfg.sinc_ratio    = 0;

  /* MUX/MIX 源选择（播放输出选择，参考例程 out_sel=0x5050） */

  priv->aprc.Init.dac_cfg.muxrsrc1 = (SF32LB52_AUDIO_OUT_SEL >> 12) & 0xF;
  priv->aprc.Init.dac_cfg.muxrsrc0 = (SF32LB52_AUDIO_OUT_SEL >> 8) & 0xF;
  priv->aprc.Init.dac_cfg.muxlsrc1 = (SF32LB52_AUDIO_OUT_SEL >> 4) & 0xF;
  priv->aprc.Init.dac_cfg.muxlsrc0 = SF32LB52_AUDIO_OUT_SEL & 0xF;
  priv->aprc.Init.dac_cfg.mixrsrc1 = (SF32LB52_AUDIO_OUT_SEL >> 12) & 0xF;
  priv->aprc.Init.dac_cfg.mixrsrc0 = (SF32LB52_AUDIO_OUT_SEL >> 8) & 0xF;
  priv->aprc.Init.dac_cfg.mixlsrc1 = (SF32LB52_AUDIO_OUT_SEL >> 4) & 0xF;
  priv->aprc.Init.dac_cfg.mixlsrc0 = SF32LB52_AUDIO_OUT_SEL & 0xF;

  HAL_AUDPRC_Config_ADCPath(&priv->aprc, &priv->aprc.Init.adc_cfg);
  HAL_AUDPRC_Config_DACPath(&priv->aprc, &priv->aprc.Init.dac_cfg);

  priv->samplerate = samplerate;
  priv->nchannels  = nchannels;
  priv->bpsamp     = bpsamp;

  audinfo("codec configured: %d Hz %d ch %d bit\n",
          samplerate, nchannels, bpsamp);
  return OK;
}

/****************************************************************************
 * Name: sf32lb52_audio_aprc_has_config
 *
 * Description:
 *   本驱动是否已经往 AUDPRC 数字侧写过一套会话配置（也就是 CONFIGURE 跑过）。
 *
 *   为什么要单独拎出这个判据：软复位（sf32lb52_audio_aprc_soft_reset）是
 *   **破坏性**的 —— 它清掉 CFG.AUDCLK_DIV / STB 里的 adc·dac 分频 /
 *   ADC_PATH_CFG0 / RX_CH0_CFG，而这些值只在 hw_configure() 写过一次、靠
 *   sf32lb52_audio_aprc_restore_adc() 重写回来。破坏和重建必须用**同一个**
 *   判据，否则会留下"复位做了、重建没做"的半截状态：块被清空、配置未知，而
 *   hw_start() 接下来照样无条件 __HAL_AUDPRC_ENABLE() 把它开起来 ——
 *   一个"开着但配置是空的"数字块比"没复位过的旧块"难查得多
 *   （DMA 完成中断照来、数据全是 0，看起来一切正常）。
 *
 *   反过来（判据不成立就不复位）是安全的：samplerate == 0 说明本驱动从没写过
 *   这套配置，没有属于我们的历史状态需要清；HAL_AUDPRC_Init() 写进去的那套
 *   默认值会被下一次 CONFIGURE 整块重写。
 *
 *   现在这条路径还不可达（CONFIGURE 总在 START 之前，而 samplerate 一旦非 0
 *   就再没被清回去过），所以本判据对现场行为零影响 —— 它挡的是将来某条
 *   "没 CONFIGURE 就跑到收尾 / 先 START 后 CONFIGURE"的路径，把那种情况下
 *   的破坏性动作变回无害。
 ****************************************************************************/

static bool sf32lb52_audio_aprc_has_config(FAR struct sf32lb52_audio_s *priv)
{
  return priv->samplerate != 0;
}

/****************************************************************************
 * Name: sf32lb52_audio_aprc_soft_reset
 *
 * Description:
 *   AUDPRC 数字级软复位 —— 厂商 bf0_audio_stop() 收尾那套原样搬过来：
 *   关模块 → 清 RX 通道配置 → CFG.SRESET 拉高再拉低（一个脉冲）。
 *   源码依据：SiFli-SDK rtos/rtthread/bsp/sifli/drivers/drv_audprc.c:1673-1677
 *   （__HAL_AUDPRC_DISABLE → HAL_AUDPRC_Clear_All_Channel →
 *     __HAL_AUDPRC_SRESET_START → __HAL_AUDPRC_SRESET_STOP）。
 *   两个宏在本 tree 的 bf0_hal_audprc.h:322 / :328 都有定义，SRESET 位是
 *   CFG 的 bit1（cmsis/sf32lb52x/audprc.h:102）。
 *
 *   为什么必须有这一步（本文件原来漏了）：老实现的每帧收尾只做 DMAStop(RX)
 *   （它顺带 ADCPATH_DISABLE，见 sf32lb52_audio_rx_dma_stop_frame），AUDPRC 内部
 *   （RX FIFO 读写指针、ADC
 *   通路抽取滤波器的状态机）**没有任何软件复位手段**。厂商在每次 stream stop
 *   都会复位它，我们是每 20ms 拉断一次数据通路、几千次都不复位 —— 现场那次
 *   RX 永久停止（irq 冻结在 2339、之后每次 read 都超时）就是这一类"内部状态
 *   机卡住"的形态。
 *
 *   这里用的是 Clear_Adc_Channel 而不是厂商的 Clear_All_Channel：本板播放走
 *   codec 自带 DMA、不经过 AUDPRC，只清 ADC 侧就能覆盖录音通路，顺带把对
 *   AUDPRC TX 通路（队列模式 enqueuebuffer 才用）的影响降到零。
 *
 *   只写寄存器、不等待、不睡眠 —— 所以它也能用在 hw_shutdown 那种
 *   "持上层锁 + 关中断"的上下文里。
 ****************************************************************************/

static void sf32lb52_audio_aprc_soft_reset(FAR struct sf32lb52_audio_s *priv)
{
  AUDPRC_HandleTypeDef *aprc = &priv->aprc;

  __HAL_AUDPRC_DISABLE(aprc);           /* CFG.ENABLE = 0 */
  HAL_AUDPRC_Clear_Adc_Channel(aprc);   /* RX_CH0_CFG / RX_CH1_CFG = 0 */
  __HAL_AUDPRC_SRESET_START(aprc);      /* CFG.SRESET = 1 */
  __HAL_AUDPRC_SRESET_STOP(aprc);       /* CFG.SRESET = 0（脉冲结束） */
}

/****************************************************************************
 * Name: sf32lb52_audio_aprc_restore_adc
 *
 * Description:
 *   按当前会话参数重建 AUDPRC 数字侧（ADC/RX 通路）的寄存器。
 *
 *   为什么要重跑：软复位会把这个数字块的状态清掉，而下面这些寄存器都是
 *   hw_configure() 那一次写的 —— CFG 里的 AUDCLK_DIV（bit16-19）、STB 的
 *   adc/dac 分频（bit0-15 / bit16-31）、ADC_PATH_CFG0（含录音数字增益）、
 *   RX_CH0_CFG（通道使能 + 格式）。复位之后不重写，就会出现"DMA 起得来、
 *   但数据通路配置是空的"这种更难查的坏法。
 *   写的值和 hw_configure 用的是同一份缓存（priv->aprc.Init.* 与
 *   priv->samplerate/nchannels/bpsamp），所以是幂等的，重复调用无害。
 *
 *   调用时机：hw_start() 每次会话开始（覆盖"stop 复位过"和"上次会话没收干净"
 *   两种情况），以及 read 失败后那次"零活动分级恢复"的底座（模块级 RSTR2 复位
 *   同样会清掉这一批寄存器，见 sf32lb52_audio_rx_fix_apply）。CONFIGURE 从没跑过
 *   （samplerate 还是 0）时不要调 —— 那时的缓存是空的。
 ****************************************************************************/

static void sf32lb52_audio_aprc_restore_adc(FAR struct sf32lb52_audio_s *priv)
{
  AUDPRC_HandleTypeDef *aprc = &priv->aprc;
  AUDPRC_ChnlCfgTypeDef cfg;

  /* ADC 输入源 = codec（复位后 SRC_SEL 位也要重写） */

  __HAL_AUDPRC_ADC_SRC_CODEC(aprc);

  /* 主时钟分频 + xtal 时钟源 + adc/dac 分频（和 HAL_AUDPRC_Init / hw_configure
   * 写的是同一批值） */

  MODIFY_REG(aprc->Instance->CFG, AUDPRC_CFG_AUDCLK_DIV_Msk,
             MAKE_REG_VAL(aprc->Init.clk_div, AUDPRC_CFG_AUDCLK_DIV_Msk,
                          AUDPRC_CFG_AUDCLK_DIV_Pos));
  __HAL_AUDPRC_CLK_XTAL(aprc);
  __HAL_AUDPRC_STB_DIV_CLK(aprc, aprc->Init.adc_div, aprc->Init.dac_div);

  /* ADC 通路（ADC_PATH_CFG0 含左右数字增益，复位后会丢） */

  HAL_AUDPRC_Config_ADCPath(aprc, &aprc->Init.adc_cfg);

  /* RX 通道配置。第二个参数必须是 0/1 通道号，不是 DMA 枚举值
   * （HAL_AUDPRC_RX_CH0 = 4，传进去会返回 HAL_ERROR 什么都不写）。 */

  memset(&cfg, 0, sizeof(cfg));
  cfg.dma_mask = 0;
  cfg.en       = 1;
  cfg.format   = (priv->bpsamp == 16) ? 0 : 1;
  cfg.mode     = (priv->nchannels == 1) ? 0 : 1;

  HAL_AUDPRC_Config_RChanel(aprc,
                            SF32LB52_AUDIO_PRC_RX_CH - HAL_AUDPRC_RX_CH0, &cfg);
}

/****************************************************************************
 * Name: sf32lb52_audio_rx_dma_stop_frame
 *
 * Description:
 *   把 RX 的 DMA 通道收干净（abort + 关中断 + 关通道 + 清标志 + 通道池回收），
 *   **不动 ADC 数据通路**。函数名保留着历史（老实现里它是每帧收尾都要调的），
 *   现在只有三个调用点，全是"会话 / 一次读"的边界，不是每帧：
 *   read 武装前摆正句柄状态、read 的收尾、hw_start 的会话初始化。
 *
 *   它就是 HAL_AUDPRC_DMAStop() 的**第一步**（bf0_hal_audprc.c:832-848）：
 *     HAL_DMA_Abort(hdma) —— 关 TC/HT/TE、关通道、清该通道全部标志、
 *       DMA_FreeChannel()（本构建开了 DMA_SUPPORT_DYN_CHANNEL_ALLOC，
 *       bf0_hal_dma.c:919）；连"hdma != NULL 才动"这个判空都和它一样，
 *       **通道池的簿记（Alloc/Free）完全不变**。
 *   省掉的是它的第二步 __HAL_AUDPRC_ADCPATH_DISABLE()（CFG.ADC_PATH_EN = 0，
 *   见文件顶部那段说明）—— 老实现每帧走一遍"DMAStop 关 ADCPATH → 下一次
 *   Receive_DMA 又打开"，厂商参考驱动整场会话只做一次 ENABLE / 一次 DISABLE。
 *
 *   重新武装时为什么不用绕开 HAL_AUDPRC_Receive_DMA：那个函数对 ADC 通路用的是
 *   `CFG |= ADC_PATH_EN`（bf0_hal_audprc.c:790 / bf0_hal_audprc.h:316），
 *   位已经置着时它就是一次同值 OR —— 无副作用。它另外写的那两下 RX_CH0_CFG
 *   （清 DMA_MSK、置 EN，bf0_hal_audprc.c:821-822）本来也是幂等的，而且
 *   DMAStop 从来不碰 RX_CH0_CFG.EN（只有软复位里的 Clear_Adc_Channel 会清它，
 *   bf0_hal_audprc.c:409-417），所以"通道使能整场不动"这条不变量成立。
 ****************************************************************************/

static void sf32lb52_audio_rx_dma_stop_frame(FAR struct sf32lb52_audio_s *priv)
{
  FAR DMA_HandleTypeDef *hdma = priv->aprc.hdma[SF32LB52_AUDIO_PRC_RX_CH];

  /* 同 HAL_AUDPRC_DMAStop() 第一步：判空后 abort。它内部会把 hdma->State
   * 置 READY（bf0_hal_dma.c:923），下面的调用方依赖这一点。
   * **只做这一步**，不跟 DMAStop 的第二步（ADCPATH_DISABLE，理由见文件顶部）。 */

  if (hdma != NULL)
    {
      HAL_DMA_Abort(hdma);
    }
}

/****************************************************************************
 * Name: sf32lb52_audio_ev
 *
 * Description:
 *   记一笔"关键时刻"（会话起停 / 播放起停 / RX abort / RX 武装），给失败行里的
 *   `hist=` 用 —— 目的是回答"卡死前最近发生过什么"。说明见驱动结构体里那几个字段。
 *
 *   不加锁（诊断量、不是判据）：写入顺序是 tick 在前、code 在后，读侧看到 code
 *   就保证 tick 已经是写好的；ev_next 单写者自增，读侧只用它取模。
 ****************************************************************************/

static const char *const g_ev_name[7] =
{
  "?", "sess_begin", "sess_end", "play_begin", "play_end", "abort", "arm"
};

static void sf32lb52_audio_ev(FAR struct sf32lb52_audio_s *priv, uint8_t code)
{
  if (priv == NULL || code == 0u || code >= SF32LB52_AUDIO_EV_MAX)
    {
      return;
    }

  priv->ev_last[code] = clock_systime_ticks();
  priv->ev_cnt[code]  = priv->ev_cnt[code] + 1u;
}

/****************************************************************************
 * Name: sf32lb52_audio_rx_dma_stop_if_armed
 *
 * Description:
 *   收 RX 通道，但**只在它确实还武装着的时候**才 abort，收完把两个 State 都
 *   摆回 READY。
 *
 *   为什么不能无条件 abort（这一条在本文件踩过）：厂商的 HAL_DMA_Abort()
 *   不看状态，无条件关中断 / 关通道 / 清该通道全部标志，还会 DMA_FreeChannel()；
 *   对一个**已经收好尾**的句柄再来一次，写的是那个物理通道的寄存器 —— 本构建
 *   开了动态通道分配，那几毫秒里它可能已经被别的 DMA 用户重新分配走了
 *   （codec 的播放 DMA 就是下一个），等于把别人的通道静默关掉（不崩，但极难查）。
 *   HAL_DMA_Abort() 自己会把 hdma->State 置成 READY，所以"READY = 已经收过尾"
 *   这个判据是自洽的。
 *
 *   这条路径是**必需**的，不是保险：一次 stop 收尾里会出现"通道已经被别人
 *   （hw_stop / hw_shutdown）收干净、而本次 read 的线程随后才醒"的时序，
 *   那时它必须什么都不做。
 *
 *   两个 State 摆回 READY 是"下一次 read 一定能在干净通道上武装"的保证
 *   （HAL_DMA_Start_IT 只在 READY 时真发传输，而 Receive_DMA 把它的返回值丢了、
 *   自己无条件 return HAL_OK）。
 ****************************************************************************/

static void sf32lb52_audio_rx_dma_stop_if_armed(FAR struct sf32lb52_audio_s *priv)
{
  FAR DMA_HandleTypeDef *hdma = priv->aprc.hdma[SF32LB52_AUDIO_PRC_RX_CH];

  if (hdma != NULL && hdma->State != HAL_DMA_STATE_READY)
    {
      sf32lb52_audio_ev(priv, SF32LB52_AUDIO_EV_RX_ABORT);
      sf32lb52_audio_rx_dma_stop_frame(priv);
      hdma->State = HAL_DMA_STATE_READY;
    }

  priv->aprc.State[SF32LB52_AUDIO_PRC_RX_CH] = HAL_AUDPRC_STATE_READY;
}

/****************************************************************************
 * Name: sf32lb52_audio_rx_dma_req
 *
 * Description:
 *   读 RX 通道在 DMAC 的 CSELR 里那一格请求源（C2S 域）—— 也就是"这条 DMA
 *   请求线当前映射到哪个外设"。HAL_DMA_Init() 写它、HAL_DMA_DeInit() 清它
 *   （bf0_hal_dma.c:81-99 / :672-680），是"通道重建有没有真把请求映射写回去"
 *   最直接的证据。每 8 位一格：通道 1-4 在 CSELR1，5-8 在 CSELR2
 *   （域位置算法与 bf0_hal_dma.c:81 同一份）。
 ****************************************************************************/

static uint32_t sf32lb52_audio_rx_dma_req(FAR DMA_HandleTypeDef *hdma)
{
  uint32_t index;

  if (hdma == NULL || hdma->DmaBaseAddress == NULL)
    {
      return 0u;
    }

  index = (hdma->ChannelIndex >> 2) & 7u;

  if (index <= 3u)
    {
      return (uint32_t)((hdma->DmaBaseAddress->CSELR1 >> (index * 8u)) & 0x3fu);
    }

  return (uint32_t)((hdma->DmaBaseAddress->CSELR2 >> ((index & 3u) * 8u)) &
                    0x3fu);
}

/* ⚠️ 诊断期临时：L1/L2/L3 要把 DMA 通道**真的武装起来**才能观察/重建那条请求
 * 握手，而恢复路径不许挂调用者那块已经交还上层的 buffer（见下面 rx_recover 的
 * 说明）—— 所以用这一块驱动自有的丢弃缓冲。它只在恢复的那一瞬间被搬动，内容
 * 直接丢掉。定案后随分级编排一起删。 */

static uint32_t
  g_sf32lb52_audio_rx_fix_buf[SF32LB52_AUDIO_RX_FIX_BUF_WORDS];

/****************************************************************************
 * Name: sf32lb52_audio_rx_fix_arm
 *
 * Description:
 *   把 RX 的 DMA 通道**真的武装起来**（目的地是上面那块丢弃缓冲），返回武装后
 *   句柄的 State（0 = 句柄不存在、0x2 = BUSY = 起来了）。
 *
 *   为什么读完不管它：16 字（2ms@16k）在循环模式下会自己转圈，而它带来的完成/
 *   半满中断进来时会话闸（rx_arm_gen）是关着的 —— read 在收尾时清过 0，ISR 因此
 *   只计数、不 post、不交 buffer（见 sf32lb52_audio_dma1_irq 的闸门那段）；下一次
 *   read 开头的 sf32lb52_audio_rx_dma_stop_if_armed() 会把它收干净。
 *   也就是说：它留下的痕迹只有"irq/half 计数开始涨"——那正是我们要的证据。
 ****************************************************************************/

static uint32_t sf32lb52_audio_rx_fix_arm(FAR struct sf32lb52_audio_s *priv)
{
  FAR DMA_HandleTypeDef *hdma = priv->aprc.hdma[SF32LB52_AUDIO_PRC_RX_CH];

  if (hdma == NULL)
    {
      return 0u;
    }

  /* Receive_DMA 只在通道状态不是 BUSY_RX 时才动手（bf0_hal_audprc.c:774），先按
   * read() 同一口径把状态摆正 —— 它自己又会置成 BUSY_RX。它内部的动作顺序是
   * "先武装、后开外设"：先 HAL_DMA_Start_IT()，再写 RX_CH0_CFG 的
   * DMA_MSK=0 / EN=1（bf0_hal_audprc.c:818-822）—— 恢复路径要的正是这个顺序。 */

  priv->aprc.State[SF32LB52_AUDIO_PRC_RX_CH] = HAL_AUDPRC_STATE_READY;

  HAL_AUDPRC_Receive_DMA(&priv->aprc,
                         (FAR uint8_t *)g_sf32lb52_audio_rx_fix_buf,
                         sizeof(g_sf32lb52_audio_rx_fix_buf),
                         SF32LB52_AUDIO_PRC_RX_CH);
  __HAL_AUDPRC_ENABLE(&priv->aprc);

  return (uint32_t)hdma->State;
}

/****************************************************************************
 * Name: sf32lb52_audio_rx_fix_apply
 *
 * Description:
 *   按分级编号动一次手（不含"先收干净"那一步 —— 调用方负责），返回这一级做完
 *   之后 RX 通道句柄的 State（0 = 没武装/没有句柄）。
 *
 *   五级共用同一套**底座**：模块时钟/PMU 音频 buffer + AUDPRC 模块级硬复位
 *   （RSTR2）+ ADC 通路 SRESET/FLUSH + restore_adc()。底座单独用已被上板证明
 *   无效（同一个故障连着试 79 次都没救活），留在这里只是为了每级都从同一处干净
 *   起点开始、让"哪一级生效"可归因。
 *
 *   ⚠️ 这里**不调 HAL_AUDPRC_Init()**：它内部会走 HAL_AUDPRC_Config_DACPath()，
 *   那条路上有等 SRC_CH_CLR_DONE 的忙等（见 sf32lb52_audio_aprc_restore_adc）。
 *   ⚠️ L1..L4 **不碰** HAL_RCC_ResetModule(RCC_MOD_DMAC1)：DMAC1 上挂着 codec
 *   播放的 DMA（hw_start 给 codec.hdma[DAC_CH0/1] 挂的就是 DMA1_Channel2/3），
 *   复位整个 DMAC1 会把正在出声的播报一起打断 —— 越界。这一记留给 L5，并且只在
 *   playback == 0 时才做（理由见 L5 那一段）。
 *
 *   priv->rx_fix_note 每次进函数先清空：它是"这一级要额外说明的东西"，默认没有；
 *   报告行（rx_fix_report）会把它打成 note=%s（没有就是 note=-）。
 ****************************************************************************/

static uint32_t sf32lb52_audio_rx_fix_apply(FAR struct sf32lb52_audio_s *priv,
                                            uint32_t level)
{
  AUDPRC_HandleTypeDef  *aprc = &priv->aprc;
  FAR DMA_HandleTypeDef *hdma = priv->aprc.hdma[SF32LB52_AUDIO_PRC_RX_CH];
  uint32_t               armed;

  /* 这一级的"额外说明"每次进来先清空（只有 L5 被 playback 守卫挡下时会写上原因），
   * 免得上一级的说明串到这一级的报告行里。 */

  priv->rx_fix_note = NULL;

  /* 底座 1：模块时钟与 PMU 音频 buffer（与 hw_init 同源的三行）。 */

  HAL_PMU_EnableAudio(1);
  HAL_RCC_EnableModule(RCC_MOD_AUDCODEC_HP);
  HAL_RCC_EnableModule(RCC_MOD_AUDCODEC_LP);
  HAL_RCC_EnableModule(RCC_MOD_AUDPRC);
  HAL_RCC_EnableModule(RCC_MOD_DMAC1);

  /* 底座 2：模块级硬复位脉冲（RSTR2 的 AUDPRC 位）。 */

  HAL_RCC_ResetModule(RCC_MOD_AUDPRC);

  /* 底座 3：ADC 通路软复位 + FLUSH 脉冲（CFG bit5 / bit3）。 */

  aprc->Instance->CFG |= (AUDPRC_CFG_ADC_PATH_SRESET |
                          AUDPRC_CFG_ADC_PATH_FLUSH);
  aprc->Instance->CFG &= ~(AUDPRC_CFG_ADC_PATH_SRESET |
                           AUDPRC_CFG_ADC_PATH_FLUSH);

  /* 底座 4：RSTR2 把 AUDPRC 的配置寄存器整块清了（CFG.AUDCLK_DIV / STB /
   * ADC_PATH_CFG0 / RX_CH0_CFG 全丢），按会话参数重写回 ADC 侧。CONFIGURE 从没
   * 跑过时跳过 —— 那时的缓存是空的。 */

  if (sf32lb52_audio_aprc_has_config(priv))
    {
      sf32lb52_audio_aprc_restore_adc(priv);
    }

  /* 唯一的差异动作（定案见文件顶部那段）：底座已经跑过（模块时钟/PMU + RSTR2 +
   * ADCPATH SRESET/FLUSH + restore_adc），这里只做这一件 —— 复位整块 DMAC1 并
   * 重建三个通道句柄。 */

  switch (level)
    {
      case SF32LB52_AUDIO_RX_FIX_DMAC1:     /* DMAC1 整块复位 + 句柄重建 */
        /* 定案见文件顶部：卡住的是 DMAC1 模块自己的"请求接收"状态 —— 重建单条
         * 通道（老 L3）清不掉它，只有模块级复位能清。动手的顺序必须是"先把三个
         * 句柄收干净 → 再抹模块 → 再重建"，否则 HAL 的账本会和硬件对不上
         * （池里还说某条通道归谁、硬件已经被复位抹空）。
         *
         * ⚠️ **只在 playback == 0（没有在播放）时才做**：
         *   DMAC1 上挂着 codec 播放的 DMA —— hw_start 给 codec.hdma[DAC_CH0/1] 挂的
         *   就是 DMA1_Channel2/3，而这一记是**模块级**复位（RSTR1 的 DMAC1 位，
         *   bf0_hal_rcc.c:516/2032），整块 DMAC1 的通道配置、请求映射、标志位全回
         *   上电默认 —— 正在出声的播报会当场断掉。
         *   录音会话的 playback 恒为 0（方向只在 hw_start 里提交，一个会话只有
         *   一个方向），所以这一级在该用的场合总能跑；万一在播放态被轮到，只记下
         *   原因（报告行里 note=skip-playback），下一轮轮转还会再轮到它。
         *
         * ⚠️ 这一记是**整块控制器**级的：被它一并抹掉的通道配置不限于 codec ——
         *   别的 DMAC1 用户（若此刻正有传输）也会被复位打断、且下一次启动可能因为
         *   HAL 通道池"还是那个持有者"而跳过 DMA_Init、重写不回 CCR/CSELR。本板的
         *   LCD 不占 DMAC1 的搬运通道（面板走 LCDC 自己的 SPI 引擎：co5300.c 用
         *   LCDC_INTF_SPI_DCX_4DATA，不是 _AUX/PTC 那条路），所以这一级的现场代价
         *   主要落在播放通路上；它仍是一记诊断用的重手，别在别的子系统正跑 DMA 时敲。
         *
         * 1) 先停：**只有"确实还武装着"（State != READY）的句柄才 DeInit**。READY
         *    的句柄不做 DeInit —— 那是往一条可能已经被别人重新分配走的物理通道上
         *    写 CCR/IFCR/CSELR，正是本文件在 abort 上守的那条规矩（见
         *    sf32lb52_audio_rx_dma_stop_if_armed）。它也不必 DeInit：HAL_DMA_Init
         *    内部那支静态 DMA_Init 无条件重写 CCR + CSELR 请求映射，并在结尾把
         *    State 摆回 READY（bf0_hal_dma.c:60-99/195、:537-601）。
         *    BUSY 的句柄用 DeInit 停（等价 abort：__HAL_DMA_DISABLE + CCR=0 +
         *    清该通道全部标志 + 还通道回池 + 关该通道 NVIC，bf0_hal_dma.c:614-687），
         *    它顺手清掉回调指针 —— 下一次 Receive/Transmit_DMA 会各自重新挂上。
         *    Init 里那几项（Direction/Mode/Priority/Request）不在 DeInit 的清理
         *    范围之内，所以下面 HAL_DMA_Init 能直接拿它们把通道重写回来。
         *    codec 那两个句柄是首次播放才分配的，可能还是 NULL。
         * 2) HAL_RCC_ResetModule(RCC_MOD_DMAC1)：模块级复位脉冲。复位只动 RSTR、
         *    不关时钟，照 hw_init 的口径把时钟再使能一次。
         * 3) 三个句柄再 Init：重新分配通道 + 重写 CCR + 重写 CSELR 请求映射 + 摆回
         *    该通道 NVIC 的优先级/使能（bf0_hal_dma.c:537-601）。顺序必须是
         *    "复位之后、武装之前"，否则武装出来的是没有请求映射的死通道。
         *    它的返回值没判：HAL_BUSY（通道池被占满）时 CCR/CSELR 不会写回。
         *    这一步成没成的判据就是报告行里的 CSELR= 与 armed= —— CSELR=0、
         *    或 armed 不是 0x2（BUSY），就说明通道没被真正重建。
         * 4) 武装丢弃缓冲：这一级有没有把请求握手叫活，靠 irq/half 涨没涨回答。 */

        if (priv->playback)
          {
            priv->rx_fix_note = "skip-playback";
            armed = 0u;
            break;
          }

        if (hdma != NULL && hdma->State != HAL_DMA_STATE_READY)
          {
            HAL_DMA_DeInit(hdma);
          }

        if (priv->codec.hdma[HAL_AUDCODEC_DAC_CH0] != NULL &&
            priv->codec.hdma[HAL_AUDCODEC_DAC_CH0]->State != HAL_DMA_STATE_READY)
          {
            HAL_DMA_DeInit(priv->codec.hdma[HAL_AUDCODEC_DAC_CH0]);
          }

        if (priv->codec.hdma[HAL_AUDCODEC_DAC_CH1] != NULL &&
            priv->codec.hdma[HAL_AUDCODEC_DAC_CH1]->State != HAL_DMA_STATE_READY)
          {
            HAL_DMA_DeInit(priv->codec.hdma[HAL_AUDCODEC_DAC_CH1]);
          }

        HAL_RCC_ResetModule(RCC_MOD_DMAC1);
        HAL_RCC_EnableModule(RCC_MOD_DMAC1);

        if (hdma != NULL)
          {
            HAL_DMA_Init(hdma);
          }

        if (priv->codec.hdma[HAL_AUDCODEC_DAC_CH0] != NULL)
          {
            HAL_DMA_Init(priv->codec.hdma[HAL_AUDCODEC_DAC_CH0]);
          }

        if (priv->codec.hdma[HAL_AUDCODEC_DAC_CH1] != NULL)
          {
            HAL_DMA_Init(priv->codec.hdma[HAL_AUDCODEC_DAC_CH1]);
          }

        /* ★ 把串口（控制台）的 RX DMA 装回去。
         *
         * 上面那一记是**模块级**复位：DMAC1 上不只挂着音频的 AUDPRC RX0，还挂着
         * **UART1/2 的 RX DMA** 通道。而串口驱动只在"DMA 完成"中断回调里才会重新
         * 武装 RX —— 通道被抹之后永远不会完成，于是**串口 RX 永久聋**：板子还活着、
         * 日志照打，但 nsh 再也收不到任何命令（2026-09-17 真机踩到：`hw_test audio 2`
         * 之后控制台"安详"、只能重启救）。
         *
         * 顺序要在这里：必须在 DMAC1 复位**之后**（先复位再装，否则又被抹）。
         * 两个 index 都试：UART1_INDEX=0（控制台）、UART2_INDEX=1（调试口）。
         * 没 attach 的那一路返回 -ENODEV，忽略。 */

        {
          int ui;

          for (ui = 0; ui < 2; ui++)
            {
              (void)sifli_uart_reinit_rx_dma(ui);
            }
        }

        armed = sf32lb52_audio_rx_fix_arm(priv);
        break;

      default:                              /* 走不到（LEVELS == 1）：不动手 */
        armed = 0u;
        break;
    }

  return armed;
}

/****************************************************************************
 * Name: sf32lb52_audio_rx_fix_report
 *
 * Description:
 *   分级恢复的**唯一一行**日志：用了哪一级、这一级之后通路是什么形状。
 *   recov=0 表示这一行是手动入口（audio_test audfix）打的。
 *   settle_ms > 0 时 irq+/half+ 是 settle 窗口里涨起来的量（手动入口等得及；
 *   自动路径在 read 里不能等，它的效果由下一次 read 的"效果检查"回答）。
 *   note 是这一级的额外说明（priv->rx_fix_note）：正常是 "-"，只有 L5 被
 *   playback 守卫挡下时是 "skip-playback"（那一级没动手，armed/irq+/half+ 因此
 *   必然是 0，这个标记就是把"没做"和"做了没用"分开）。
 ****************************************************************************/

static void sf32lb52_audio_rx_fix_report(FAR struct sf32lb52_audio_s *priv,
                                         uint32_t level, uint32_t recov,
                                         uint32_t armed, uint32_t settle_ms,
                                         uint32_t irq0, uint32_t half0)
{
  AUDPRC_HandleTypeDef  *aprc = &priv->aprc;
  FAR DMA_HandleTypeDef *hdma = priv->aprc.hdma[SF32LB52_AUDIO_PRC_RX_CH];

  syslog(LOG_WARNING,
         "AUDIO: RX 恢复 L%u recov=%u armed=%u note=%s settle=%ums"
         " irq+%u half+%u"
         " RX_CH0_CFG=0x%x IRQ=0x%x CNDTR=%u CFG=0x%x ENR2=0x%x"
         " HXT_CR1=0x%x PLL_STAT=0x%x CSELR=%u hdma.State=0x%x\n",
         (unsigned)level, (unsigned)recov, (unsigned)armed,
         (priv->rx_fix_note != NULL) ? priv->rx_fix_note : "-",
         (unsigned)settle_ms,
         (unsigned)(priv->rx_irq_count - irq0),
         (unsigned)(priv->rx_half_irq_count - half0),
         (unsigned)aprc->Instance->RX_CH0_CFG,
         (unsigned)aprc->Instance->IRQ,
         (unsigned)((hdma != NULL && hdma->Instance != NULL) ?
                    (uint32_t)hdma->Instance->CNDTR : 0u),
         (unsigned)aprc->Instance->CFG,
         (unsigned)hwp_hpsys_rcc->ENR2,
         (unsigned)hwp_pmuc->HXT_CR1,
         (unsigned)priv->codec.Instance->PLL_STAT,
         (unsigned)sf32lb52_audio_rx_dma_req(hdma),
         (unsigned)(hdma != NULL ? (uint32_t)hdma->State : 0u));
}

/****************************************************************************
 * Name: sf32lb52_audio_rx_recover
 *
 * Description:
 *   read 失败之后的收尾与恢复 —— 说得准确些：**这一函数不把数据读进调用者的
 *   buffer**，它把 RX 通道收干净、必要时按分级动一次手，让"真正取数据"这件事由
 *   **下一次 read 自己的 Receive_DMA** 完成。
 *
 *   为什么不能"在这里直接给调用者的 buffer 重新武装一次"：那块缓冲已经交还上层
 *   了（它按超时返回，随时可能被复用/释放），在上面偷偷挂 DMA 等于把数据写进一块
 *   不受本驱动管辖的内存。分级动作里那两次武装因此都挂在驱动自有的丢弃缓冲上
 *   （sf32lb52_audio_rx_fix_arm），只为把请求握手叫活。
 *
 *   判据只用**观察量本身**：
 *     - 一级（任何一次 read 失败）：sf32lb52_audio_rx_dma_stop_if_armed() 把通道
 *       收干净（只在确实还武装着时才 abort，理由见那个函数）。做完的保证：
 *       hdma->State == READY 且 aprc.State[RX] == READY。
 *     - 二级（irq_delta == 0 且 half_delta == 0，即这 5 秒里 RX 一个完成中断、
 *       一个半满中断都没来）＝"外设 → DMAC 这条请求握手死了"的指纹（上板定案：
 *       外设侧 EN=1 且 FIFO 在积甚至溢出，DMAC 侧通道 BUSY 却一个请求都没收到），
 *       于是打**唯一那一记**：复位整块 DMAC1 + 重建三个通道句柄（定案依据见文件
 *       顶部那段）。
 *     动作**无条件**执行，不加任何"判活动静/方向冲突/宽限期"之类的额外判据 ——
 *     零活动这个观察量本身就是判据。
 *     （"只在没在播放时才做"那条守卫是**动作内部**的安全条件，不是这里的判据：
 *     被挡下时报告行里 note=skip-playback，下一次失败还会再来。）
 *
 *   Returned Value:
 *     0   正常（收干净了 + 该做的恢复动作做了）
 *    -EIO  abort 之后通道没回到 READY（下一次 read 开头会再收一次）
 ****************************************************************************/

static int sf32lb52_audio_rx_recover(FAR struct sf32lb52_audio_s *priv,
                                     uint32_t irq_delta, uint32_t half_delta)
{
  FAR DMA_HandleTypeDef *hdma = priv->aprc.hdma[SF32LB52_AUDIO_PRC_RX_CH];
  uint32_t level = 0u;
  uint32_t armed = 0u;
  uint32_t irq0;
  uint32_t half0;

  /* 一级：把通道收干净。分级的动作都从这一步之后开始 —— 重复 abort 一个已经
   * 收好尾的句柄，写的是可能已被别人分配走的物理通道。 */

  sf32lb52_audio_rx_dma_stop_if_armed(priv);

  if (hdma != NULL && hdma->State != HAL_DMA_STATE_READY)
    {
      /* 通道没能回到 READY（abort 没生效）。不下重手，只留证并让这一次 read
       * 照常返回：下一次 read 开头那段"摆正句柄状态"会再收一次。 */

      return -EIO;
    }

  priv->rx_recover_count++;

  /* 二级：这 5 秒零活动 → 请求握手那条路死了，按 recov 计数轮转到某一级。 */

  if (irq_delta == 0u && half_delta == 0u)
    {
      irq0  = priv->rx_irq_count;
      half0 = priv->rx_half_irq_count;
      level = 1u + ((priv->rx_recover_count - 1u) %
                    SF32LB52_AUDIO_RX_FIX_LEVELS);

      armed = sf32lb52_audio_rx_fix_apply(priv, level);

      sf32lb52_audio_rx_fix_report(priv, level, priv->rx_recover_count,
                                   armed, 0u, irq0, half0);

      /* 记一笔"待验证的一级"：下一次 read 一进来就按这三个数打"效果检查"
       * （判这一级生效没有的唯一标准：half/irq 有没有重新开始涨）。 */

      priv->rx_fix_level = level;
      priv->rx_fix_irq   = irq0;
      priv->rx_fix_half  = half0;
    }

  priv->aprc.State[SF32LB52_AUDIO_PRC_RX_CH] = HAL_AUDPRC_STATE_READY;

  return 0;
}

/****************************************************************************
 * Name: sf32lb52_audio_hw_start
 *
 * Description:
 *   启动播放或录音通路（参考 SDK audprc 例程 start_tx/start_rx）：
 *   播放：codec DAC 模拟 → AUDPRC TX 通路 → 功放
 *   录音：codec ADC 模拟 → AUDPRC RX 通路
 *
 ****************************************************************************/

static int sf32lb52_audio_hw_start(FAR struct sf32lb52_audio_s *priv,
                                   bool playback)
{
  AUDCODEC_HandleTypeDef *codec = &priv->codec;
  AUDPRC_HandleTypeDef   *aprc  = &priv->aprc;

  sf32lb52_audio_ev(priv, playback ? SF32LB52_AUDIO_EV_PLAY_BEGIN :
                                     SF32LB52_AUDIO_EV_SESS_BEGIN);

  /* 音频 PLL 时钟配置（播放/录音前必调，参考 SDK bf0_audio_pll_config） */


  /* 参考源/带隙基准（含 BG_CFG0 VREF_SEL、REFGEN 使能）：SDK 的 bf0_enable_pll
   * 内部会调用 HAL_TURN_ON_PLL，openvela 的 weak 版本没有 → 必须显式补齐，
   * 否则 DAC 模拟级无参考源，播放静音（录音通路的 ADCPath 自带这一步） */

  HAL_TURN_ON_PLL();

  /* SDK 的 52X 参考序列（codec_hp_sin1k_test）在此板上用 VREF_SEL=0xc（AVDD 3.3V，
   * AVDD_V18_ENABLE 未定义走 #else 分支）；openvela 的 HAL_TURN_ON_PLL 写的是 4，
   * 带隙基准不正确 → DAC 模拟级无输出（只有通路开关爆音）。这里按 SDK 值纠正。 */

  codec->Instance->BG_CFG0 &= ~AUDCODEC_BG_CFG0_VREF_SEL_Msk;
  codec->Instance->BG_CFG0 |= (0xcUL << AUDCODEC_BG_CFG0_VREF_SEL_Pos);
  up_mdelay(2);


  /* 音频 PLL 是否真的锁定：UNLOCK=1 表示未锁定 → DAC 无模拟时钟 → 静音 */

  /* 播放与录音通路一起使能：播放走 codec 自带 DMA（AUDCODEC DAC_CH0，
   * 见 write()），录音走 AUDPRC RX0；两者可独立使用 */

  /* 注意：播放数据不再经 AUDPRC（改走 codec 自带 DMA，
   * HAL_AUDCODEC_Transmit_DMA），避免 AUDPRC 的 DAC 通路与 codec DMA
   * 争抢 codec 的 DAC 数据源；AUDPRC 现在只负责录音(RX)。 */

  /* AUDPRC：ADC 输入来自 codec */

  __HAL_AUDPRC_ADC_SRC_CODEC(aprc);

  /* 每次会话开始都把 AUDPRC 数字侧的录音配置重写一遍（时钟分频 / 主分频 /
   * ADC 通路 / RX 通道格式），值全部取自 hw_configure 缓存下来的参数。
   *
   * 为什么要在这里多做一遍：hw_stop()/hw_shutdown() 收尾时会做 AUDPRC 软复位
   * （厂商序列，见 sf32lb52_audio_aprc_soft_reset 的注释），复位可能连带清掉
   * CFG.AUDCLK_DIV / STB / ADC_PATH_CFG0 / RX_CH0_CFG —— 那些原来只有 CONFIGURE
   * 那一次写过。而 sf32lb52_audio_start() 在检测到残留 running 时是
   * "stop + start"、中间**没有** CONFIGURE 的（见 sf32lb52_audio_start），
   * 不重写就会用一套空配置录音（DMA 正常起、数据全 0）。
   *
   * CONFIGURE 从没跑过时（samplerate 还是 0）跳过 —— 和 hw_stop()/hw_shutdown()
   * 里那记软复位**共用同一个判据**，两边的开关永远是同向的（见
   * sf32lb52_audio_aprc_has_config 的注释）。
   * 这几行都是普通寄存器写 —— RX_CH0_CFG 那条没有 HAL_AUDPRC_Config_DACPath
   * 里那种"等 SRC_CH_CLR_DONE"的忙等，不会在这里卡住。 */

  if (sf32lb52_audio_aprc_has_config(priv))
    {
      sf32lb52_audio_aprc_restore_adc(priv);
    }

  /* RX 通路的会话账在这里立起来 —— 就是厂商 bf0_audio_start 里那一次
   * HAL_AUDPRC_Receive_DMA 所在的位置（它在 __HAL_AUDPRC_ENABLE 之前 arm、
   * 拿 ENABLE 收尾）。
   *
   * 本驱动**不在这里 arm**，只做 arm 的前置条件，原因只有一个：厂商那次
   * Receive_DMA 挂的是驱动自有的接收 buffer（haudprc->buf[RX_CH0]），而本驱动
   * 的 read() 是"直接读进调用者给的 buffer"——hw_start 这一刻还没有任何调用者
   * buffer 可挂，那一次武装只能发生在 read 里（详见文件顶部"录音 RX 通路"那段）。
   * 于是这一段的职责是让 read 的那一次 Receive_DMA **一定落在干净的 READY 上**：
   *   - 上一条会话若把句柄留在非 READY（abort 没走到 / 上一次收尾被跳过），
   *     这里只 abort 一次把它收干净。判据用 hdma->State，与别处同一条：
   *     已经 READY 就绝不再 abort（重复 abort 写的是可能已被别人分配走的
   *     物理通道）；
   *   - 两个 State 都摆成 READY：HAL_DMA_Start_IT 只在 READY 时才真发传输，
   *     而 HAL_AUDPRC_Receive_DMA 把它的返回值丢掉、自己无条件 return HAL_OK，
   *     状态不对就会变成"以为起来了、其实没起"，一路干等 5 秒；
   *   - 会话闸清 0（关着）：上一条会话残影的完成中断从此进不了 read 的等待，
   *     要等本会话第一次 read 武装成功时才会被写上本次会话代号。
   *
   * 不碰 ADC 通路、不软复位、不动 RX_CH0_CFG：软复位是会话边界（hw_stop /
   * hw_shutdown）的事，这里只是让"武装"有个干净的落点。 */

  {
    sf32lb52_audio_rx_dma_stop_if_armed(priv);
    priv->rx_arm_gen = 0;
  }

  __HAL_AUDPRC_ENABLE(aprc);

  /* 首次播放时挂上 codec 自带 DMA 句柄（SDK 52X 的播放路径，小智固件实测有声），
   * 并让 HAL 重新初始化（它会用音频参数配置句柄并动态分配 DMA 通道）。
   *
   * ★ 2026-09-19：这一段的失败分支原来是**静默继续**的 —— 两个 kmm_zalloc 的
   * 结果没人看、HAL_AUDCODEC_Init 的返回值（cret）也没人看。而 HAL_AUDCODEC_Init
   * 只会"跳过" NULL 句柄、照样返回 HAL_OK：于是现场是"录音正常、播放永远没声"
   * 这种最难查的形状（HAL_AUDCODEC_Transmit_DMA 判到 NULL 就 HAL_ERROR，没人报）。
   * 堆被 cJSON / TTS / MQTT 队列吃紧之后（报警那一路正是这种时刻）这个分支真会走到。
   * 现在如实失败并把错误码交回上层：
   *   - 播放（playback=1）：直接返回错误，这一次播报不响，但设备状态一点没动；
   *   - 录音（playback=0）：录音走的是 AUDPRC RX0，不经过 codec 的 DMA，句柄缺了
   *     不影响本次 START，只记一行错误继续（不让"分配失败"顺手把常听也停掉）。
   * 失败时把句柄放回并清成 NULL：下一次 START 会重新分配重试（不清的话这一段的
   * 判据 `hdma[CH0] == NULL` 永远不成立，坏状态就钉死了）。 */

  if (priv->codec.hdma[HAL_AUDCODEC_DAC_CH0] == NULL)
    {
      int cret;

      priv->codec.hdma[HAL_AUDCODEC_DAC_CH0] = kmm_zalloc(sizeof(DMA_HandleTypeDef));
      priv->codec.hdma[HAL_AUDCODEC_DAC_CH1] = kmm_zalloc(sizeof(DMA_HandleTypeDef));

      if (priv->codec.hdma[HAL_AUDCODEC_DAC_CH0] != NULL)
        {
          /* Instance 必须是某个合法 DMAC 通道：DMA_AllocChannel() 用它定位通道池，
           * 为 NULL 会进 HAL_ASSERT(0)（while(1)）死循环。若该通道已被占用，
           * 分配器会自动改选池里空闲的通道。 */
          priv->codec.hdma[HAL_AUDCODEC_DAC_CH0]->Instance     = DMA1_Channel2;
          priv->codec.hdma[HAL_AUDCODEC_DAC_CH0]->Init.Request = AUDCODEC_DAC0_DMA_REQUEST;
          priv->codec.hdma[HAL_AUDCODEC_DAC_CH0]->Parent       = &priv->codec;
        }

      if (priv->codec.hdma[HAL_AUDCODEC_DAC_CH1] != NULL)
        {
          priv->codec.hdma[HAL_AUDCODEC_DAC_CH1]->Instance     = DMA1_Channel3;
          priv->codec.hdma[HAL_AUDCODEC_DAC_CH1]->Init.Request = AUDCODEC_DAC1_DMA_REQUEST;
          priv->codec.hdma[HAL_AUDCODEC_DAC_CH1]->Parent       = &priv->codec;
        }

      if (priv->codec.hdma[HAL_AUDCODEC_DAC_CH0] == NULL ||
          priv->codec.hdma[HAL_AUDCODEC_DAC_CH1] == NULL)
        {
          syslog(LOG_ERR,
                 "AUDIO: codec DMA 句柄分配失败（ch0=%p ch1=%p）：%s\n",
                 (FAR void *)priv->codec.hdma[HAL_AUDCODEC_DAC_CH0],
                 (FAR void *)priv->codec.hdma[HAL_AUDCODEC_DAC_CH1],
                 playback ? "本次播放不启动（返回 -ENOMEM）"
                          : "录音通路不经过它，本次 START 继续");

          if (priv->codec.hdma[HAL_AUDCODEC_DAC_CH0] != NULL)
            {
              kmm_free(priv->codec.hdma[HAL_AUDCODEC_DAC_CH0]);
            }

          if (priv->codec.hdma[HAL_AUDCODEC_DAC_CH1] != NULL)
            {
              kmm_free(priv->codec.hdma[HAL_AUDCODEC_DAC_CH1]);
            }

          priv->codec.hdma[HAL_AUDCODEC_DAC_CH0] = NULL;
          priv->codec.hdma[HAL_AUDCODEC_DAC_CH1] = NULL;

          if (playback)
            {
              return -ENOMEM;
            }
        }
      else
        {
          cret = HAL_AUDCODEC_Init(&priv->codec);

          if (cret != HAL_OK)
            {
              /* 走到这里说明 codec 句柄没被真正初始化：DAC/ADC 两侧的寄存器配置
               * 都不可信（它内部会走 HAL_DMA_Init → DMA_AllocChannel，那边有几处
               * HAL_ASSERT(0) 的 while(1)，真出问题宁可在这里失败）。放回句柄、
               * 如实报错，交给上层（录音端会按退避重试，播放端这次不响）。 */

              syslog(LOG_ERR,
                     "AUDIO: HAL_AUDCODEC_Init 失败（%d），本次 START 不启动\n",
                     (int)cret);

              if (priv->codec.hdma[HAL_AUDCODEC_DAC_CH0] != NULL)
                {
                  kmm_free(priv->codec.hdma[HAL_AUDCODEC_DAC_CH0]);
                }

              if (priv->codec.hdma[HAL_AUDCODEC_DAC_CH1] != NULL)
                {
                  kmm_free(priv->codec.hdma[HAL_AUDCODEC_DAC_CH1]);
                }

              priv->codec.hdma[HAL_AUDCODEC_DAC_CH0] = NULL;
              priv->codec.hdma[HAL_AUDCODEC_DAC_CH1] = NULL;
              return -EIO;
            }
        }
    }

  /* codec：ADC 模拟通路（录音用） */

  HAL_AUDCODEC_Config_RChanel(codec, SF32LB52_AUDIO_ADC_CH,
                              &codec->Init.adc_cfg);
  HAL_AUDCODEC_Config_Analog_ADCPath(codec->Init.adc_cfg.adc_clk);
  __HAL_AUDCODEC_ADC_ENABLE(codec);

  /* ADC 模拟通路就在这一行真正开了：如实置位。
   * 注意它**不分方向**（原来的行为就是这样）：录音要用它，播放时也跟着使能、
   * 只是数据不经过它。删除它要另外验证，不在本次改动范围里。 */

  sf32lb52_audio_path_bit(&priv->adc_path_on, true, "adc_path_on",
                          "hw_start 开 ADC 模拟通路");

  /* codec：DAC 模拟通路 + 功放 —— 只有播放才碰。
   *
   * 纯录音时打开 DAC/功放是白开：麦克风数据不经过它，但功放使能那一下会在
   * 喇叭上打出一声可听见的爆音（实测 hw_test audio 这种纯录音也能听到）。
   *
   * 播放时的顺序也不能反（SF32LB52X 无独立 HP 块，主 Instance 使能 DAC）：
   *   1. 先把模拟通路以最小音量开起来，等耦合电容充到静态电平；
   *   2. 再开功放 —— 此时 DAC 输出没有跳变；
   *   3. 最后把音量拉到目标值。
   */

  if (playback)
    {
      __HAL_AUDCODEC_DAC_ENABLE(codec);
      HAL_AUDCODEC_Config_DACPath(codec, 1);
      HAL_AUDCODEC_Config_Analog_DACPath(codec->Init.dac_cfg.dac_clk);
      HAL_AUDCODEC_Config_DACPath(codec, 0);

      /* DAC 模拟通路开完就置位：收尾时只按这个位决定要不要关它 */

      sf32lb52_audio_path_bit(&priv->dac_path_on, true, "dac_path_on",
                              "hw_start 开 DAC 模拟通路");

      HAL_AUDCODEC_Config_DACPath_Volume(codec, 0, SF32LB52_AUDIO_VOL_MUTE);
      HAL_AUDCODEC_Config_DACPath_Volume(codec, 1, SF32LB52_AUDIO_VOL_MUTE);
      up_mdelay(SF32LB52_AUDIO_SETTLE_MS);

      sf32lb52_audio_pa_enable(true);

      /* 功放使能这一下也如实记：hw_stop/hw_shutdown 只有 pa_on 为真才去关它 */

      sf32lb52_audio_path_bit(&priv->pa_on, true, "pa_on",
                              "hw_start 开功放");

      up_mdelay(20);   /* 等功放上电稳定 */

      HAL_AUDCODEC_Config_DACPath_Volume(codec, 0, priv->playback_db);
      HAL_AUDCODEC_Config_DACPath_Volume(codec, 1, priv->playback_db);

      audinfo("DAC+PA path started\n");
    }
  else
    {
      audinfo("ADC path started (record only, DAC/PA untouched)\n");
    }

  /* 到这里硬件都真起来了，才把方向"提交"为当前会话的方向。
   * 在 START 成功这一刻提交（而不是 CONFIGURE 那一刻写）是本文件的关键约定：
   * 只 CONFIGURE、没 START 的调用（ai_audio.c 的 audio_hw_set_volume() 就是）
   * 必须**不改变**任何真实通路状态和当前会话方向，否则它会替正在录音的会话
   * 把方向翻成播放。pending_playback 只表示"下一次 START 想要的方向"。 */

  priv->playback = playback;
  priv->running  = true;

  /* 记下这次会话的持有者（谁把它起起来的），并清掉上一条会话留下的
   * "最近读写的线程" —— 新会话这会儿一个字节都还没读/写。
   * 这三个 id 就是 sf32lb52_audio_start() 判"方向冲突是别人真在用还是残留"
   * 的全部依据（见 struct 里那段说明与 sf32lb52_audio_holder_alive）。 */

  priv->holder_tid = nxsched_gettid();
  priv->holder_pid = getpid();
  priv->user_tid   = (pid_t)-1;

  /* 记下这条会话是"什么时候起的"：会话开头那一小段（真正读/写数据的线程还没进
   * 第一次调用）是唯一会"两条线程证据都查不到"的窗口，sf32lb52_audio_start()
   * 靠它判宽限期，绝不在这段时间里把一条刚起来的活会话当残局强停
   * （见 SF32LB52_AUDIO_SWITCH_GRACE_MS）。 */

  priv->holder_since = clock_systime_ticks();

  /* 新会话开始：上一次会话遗留的 read()（如果有）到此作废 —— 它的会话代号
   * 已经对不上，醒来就会自己退出，不会把新会话的数据接走。 */

  priv->session_gen++;

  /* 本会话的 arm 计数从 1 重新数（read 里每次"确认武装成功"自增；日志用它回答
   * "这一次失败落在本会话的第几帧"）。会话闸 rx_arm_gen 已经在上面那段 RX
   * 初始化里清成 0（= 闸门关着，等本会话第一次 read 武装成功才开）。 */

  priv->rx_arm_session = 0;

  /* 健康指纹也是"每会话第一条成功 read"才打一行，所以会话起点把它清掉
   * （见 struct 里 rx_health_done 的说明）。 */

  priv->rx_health_done = false;

  /* 这一行是"hw_start 到底有没有跑"的唯一凭据：
   *   有它 → ADC 模拟通路 / AUDPRC 真使能过，收不到数据要去查中断和数据通路；
   *   没有 → 上层 START 根本没执行到这里（多半被残留 running 的早退挡掉了），
   *          read() 等 DMA 超时（-110）就是从这个洞进来的。 */

  syslog(LOG_INFO, "AUDIO: hw_start 真跑了：playback=%d\n", (int)playback);
  return OK;
}

/****************************************************************************
 * Name: sf32lb52_audio_hw_stop
 *
 * Description: 停止播放/录音，关闭通路和功放
 *
 ****************************************************************************/

static int sf32lb52_audio_hw_stop(FAR struct sf32lb52_audio_s *priv)
{
  /* 幂等：已经停干净就不要再关一遍。
   *
   * 最后一个 fd 被 close() 时，上层 shutdown() 会紧跟在上层的
   * AUDIOIOC_STOP 之后进来，重复关一次已经关掉的模拟通路会卡死
   * （见 sf32lb52_audio_hw_shutdown 的注释）。
   * 判据里除了 running 还要看 rx_busy/rx_aborted/rx_apb/tx_apb：
   * 这些都是"还没收尾"的状态，有一样没清就走完整流程，别漏掉 buffer。
   *
   * 三个真实通路位也必须进判据 —— 原来那五项的判据在新位参与后**不再成立**：
   * hw_shutdown() 是故意不关模拟通路的（关中断上下文里关模拟级会卡死），
   * 它只清 running、把功放关掉，于是会出现"running=false 但 dac_path_on /
   * adc_path_on 仍为真"的合法状态。这种情况下再调 hw_stop()，旧判据会直接
   * 早退，那条还开着的模拟通路就**永远漏关**了（下一位持有者一直带着它跑，
   * 直到下一次 hw_start 又把它当新的开一遍）。加上这三个位之后，
   * 收尾的语义才闭合：只要还有任何一条模拟级开着或功放开着，就走完整流程。
   *   反过来，位都是假时早退是安全的：那些行的作用全是"把 DMA / buffer /
   *   ADC 通路收掉"，没有通路开着就没有对应的东西要收；而无条件 post 出来
   *   的多余计数会被 read() 开头的 nxsem_reset() 清掉（见下面的注释）。
   */

  if (!priv->running && !priv->rx_busy && !priv->rx_aborted &&
      priv->rx_apb == NULL && priv->tx_apb == NULL &&
      !priv->dac_path_on && !priv->adc_path_on && !priv->pa_on)
    {
      /* 早退也要放一次"还卡在 read 里的任务"：设备虽然早停了，但真机上出现过
       * "stop 那次唤醒没生效"，那种情况下录音线程会一直卡在那次等待里（界面
       * 跟着遭殃）。这里补一次和完整流程末尾同样的唤醒 + 会话作废，代价只是
       * 一个信号量计数（read() 开头会 reset 掉），换来的是"任何一次 stop 调用
       * 都不可能留着别人卡住"。 */

      priv->rx_aborted = priv->rx_busy;
      priv->session_gen++;
      nxsem_post(&priv->rx_sem);
      return OK;
    }

  sf32lb52_audio_ev(priv, priv->playback ? SF32LB52_AUDIO_EV_PLAY_END :
                                           SF32LB52_AUDIO_EV_SESS_END);

  /* 收尾的两条 DMAStop 都要**先看句柄状态**：厂商的 `HAL_DMA_Abort()`
   * （DMAStop 内部）不看 State，无条件关中断 / 关通道 / 清该通道全部标志
   * （bf0_hal_dma.c:938-963），而对一个已经收好尾的句柄再 abort 一次，写的是
   * 那个**物理通道**的寄存器 —— 它可能已经被别的 DMA 用户重新分配走了
   * （本构建开了动态通道分配，见 sf32lb52_audio_rx_dma_stop_if_armed 的说明），
   * 等于把别人的通道静默关掉。`HAL_DMA_Abort()` 自己会把 State 置成 READY，
   * 所以"READY 就是已经收过尾"这个判据在几处用的是同一条。
   *
   * TX0 这一条单独说清楚（只加守卫、不删不改）：本固件的播放走 codec 自带 DMA
   * （HAL_AUDCODEC_Transmit_DMA，见 hw_start 的说明），AUDPRC TX0 只在队列模式
   * （enqueuebuffer）里才会真的起来，而本板两个 app 走的都是 read()/write()
   * 直通 —— 所以它平时一直是空转。但 sf32lb52_audio_enqueuebuffer 里那条
   * HAL_AUDPRC_Transmit_DMA 还在，队列模式真被用起来时这条 abort 就是必要的
   * 收尾，因此按"不确定就别动"的规矩**保留**；加了守卫之后它平时也不会再写
   * 别人的通道，两边的目的都达到了。 */

  if (priv->aprc.hdma[HAL_AUDPRC_TX_CH0] == NULL ||
      priv->aprc.hdma[HAL_AUDPRC_TX_CH0]->State != HAL_DMA_STATE_READY)
    {
      HAL_AUDPRC_DMAStop(&priv->aprc, HAL_AUDPRC_TX_CH0);
    }

  if (priv->aprc.hdma[HAL_AUDPRC_RX_CH0] == NULL ||
      priv->aprc.hdma[HAL_AUDPRC_RX_CH0]->State != HAL_DMA_STATE_READY)
    {
      HAL_AUDPRC_DMAStop(&priv->aprc, HAL_AUDPRC_RX_CH0);
    }

  __HAL_AUDPRC_DISABLE(&priv->aprc);

  /* AUDPRC 数字级软复位（厂商 bf0_audio_stop 收尾那套，见
   * sf32lb52_audio_aprc_soft_reset 的注释）。位置和厂商一致：两条 DMAStop
   * 之后、模块 DISABLE 之后。
   *
   * 这是"RX 跑一段时间后永久停止"的**根治**那一步：原来只有 DMAStop +
   * DISABLE，AUDPRC 内部状态机没有任何复位手段，而老实现是每帧（20ms）
   * 都把数据通路拉断一次、从头到尾不复位。现场那次 irq 冻结在 2339、此后每次
   * read 都超时，就是这一类内部卡死的形态。复位可能清掉的
   * 时钟/通路配置，由 hw_start() 里的 sf32lb52_audio_aprc_restore_adc() 每次会话
   * 开头重写，两边配套、缺一不可。
   *
   * 判据和那次重建共用（见 sf32lb52_audio_aprc_has_config）：复位是破坏性的，
   * 只有"这套配置确实写过、将来一定会被重建"时才允许做。 */

  if (sf32lb52_audio_aprc_has_config(priv))
    {
      sf32lb52_audio_aprc_soft_reset(priv);
    }

  /* 播放用的 DAC DMA 也要停，而且**必须停**：
   * HAL 把音频 DMA 初始化成 DMA_CIRCULAR（见 hw_init 的注释：WORD 对齐 +
   * 循环 + 高优先级），也就是传输结束后硬件自己从头再来一遍。write() 正常
   * 返回时它自己会 DMAStop，但"写失败提前 break""上层直接 close""stop 时
   * 正阻塞在 write 里"这几条路上没人停它 —— 最后写进去的那 100 ms 就会
   * 无限重播，现场听感就是"昂昂昂昂"卡住不停。
   * 只动寄存器，和上面两行 AUDPRC DMAStop 一个量级，不碰模拟通路。
   *
   * ★ 2026-09-19 修正：这一句原来是**无条件**调的，和上面两条 AUDPRC 的
   * DMAStop 不一样（那两条都加了"State != READY"守卫）。而
   * HAL_AUDCODEC_DMAStop() 内部就是 HAL_DMA_Abort()，它**不看 State**，
   * 无条件关通道 / 清该通道全部标志 / 把通道还回通道池
   * （bf0_hal_dma.c:898-929）。对一条**已经收好尾**（State 已是 READY）的句柄
   * 再 abort 一次，写的是那个**物理通道**的寄存器 —— 而本构建开了动态通道
   * 分配，那个通道很可能已经被别的 DMAC1 用户重新分配走了：同一个 DMAC1 上还
   * 挂着 UART1/UART2 的 RX DMA（DMA1_Channel6/7，见 sifli_uart.c 那两段说明）。
   * 那一下等于把别人的在途传输静默打断、顺势清掉它的完成标志 —— 后果就是
   * "串口 RX 永久聋"或"某个通道的标志再也没人清"这一类只有上板才看得见的形态。
   * 录音会话（playback=0）每停一次都会走到这里，而这条路上 codec 的 DAC 通道
   * 从来没被武装过 —— 也就是**每次停录音都在赌别人的物理通道**。
   * DAC_CH0_CFG 里那一位 DMA_EN 无论如何都要清（它才是"数据不再往 DAC 里送"
   * 的那一位，见 sf32lb52_audio_tx_freeze 里同一位的用法），所以分两条路写。 */

  if (priv->codec.hdma[HAL_AUDCODEC_DAC_CH0] != NULL &&
      priv->codec.hdma[HAL_AUDCODEC_DAC_CH0]->State != HAL_DMA_STATE_READY)
    {
      HAL_AUDCODEC_DMAStop(&priv->codec, HAL_AUDCODEC_DAC_CH0);
    }
  else
    {
      priv->codec.Instance->DAC_CH0_CFG &= ~AUDCODEC_DAC_CH0_CFG_DMA_EN;
    }

  priv->codec.State[HAL_AUDCODEC_DAC_CH0] = HAL_AUDCODEC_STATE_READY;

  /* 清掉 HAL 的通道状态，否则下一次 Transmit/Receive DMA 会返回 HAL_BUSY */
  priv->aprc.State[HAL_AUDPRC_TX_CH0] = HAL_AUDPRC_STATE_READY;
  priv->aprc.State[HAL_AUDPRC_RX_CH0] = HAL_AUDPRC_STATE_READY;

  /* 唤醒可能正阻塞在 read() 里的任务。
   *
   * read() 等的是 priv->rx_sem，正常由 DMA 完成中断 post。stop 时 DMA 被停掉，
   * 中断不会再来 —— 必须在这里补一次，否则 read 只能干等它自己的超时。
   * **无条件 post**（不再只在 rx_busy 时才 post）：rx_busy 的读写和中断有竞态，
   * 实测出现过"stop 那一下刚好看到 rx_busy==false 就没唤醒"，录音线程卡在
   * read 里出不来，而 join 它的正是 LVGL 线程 —— 界面整块卡死（提交按钮一直
   * 不变色）。配对措施见 read()：起 DMA 前先 nxsem_reset()，多余的计数不会
   * 被下一次 read 当成"采集完成"。
   */

  syslog(LOG_INFO, "AUDIO: stop 唤醒 read（rx_busy=%d running=%d）\n",
         (int)priv->rx_busy, (int)priv->running);

  /* 有 read 在等：置 abort 标记（让它按 EOF 收场，不能把半截缓冲当数据交上去）。
   * 没有 read 在等：把标记清掉，别留下脏状态影响本函数开头的"幂等早退"判断。 */

  priv->rx_aborted = priv->rx_busy;

  /* **无条件** post：rx_busy 与中断之间有竞态，只在 busy 时 post 会漏唤醒
   * （实测那次漏了）。配对措施见 read()：起 DMA 前先 nxsem_reset()，多余计数
   * 不会被下次 read 当成"采集完成"。 */

  priv->session_gen++;      /* 会话作废：read 即使没被这次 post 唤醒，醒来也会自己退出 */

  nxsem_post(&priv->rx_sem);

  /* 把还挂着的 buffer 归还给上层。
   *
   * 上层（NuttX audio 框架）的 read()/write() 是靠下层回调
   * AUDIO_CALLBACK_DEQUEUE 来唤醒的。如果 stop 时 buffer 还挂在 DMA 上
   * 却不归还，正在阻塞的 read() 永远等不到回调 —— 线程就此卡死。
   * 实测：录音 read 阻塞中发 AUDIOIOC_STOP，3 秒都不返回。
   * （DMA 已经停了，这时归还 buffer 是安全的）
   *
   * 只有**确实挂着本会话的 buffer** 时才回调：两个指针都只有队列模式
   * （上层 AUDIOIOC_ENQUEUEBUFFER）才会被赋值，本板两个 app 走的都是直通
   * read()/write()，所以直接通道下这里恒为 NULL、一个回调都不会发出去。
   * 这一点很重要：上层 audio_dequeuebuffer() 会写 upper->status，而
   * upper->status 在"最后一个 fd 被 close"时就被释放并置 NULL 了 —— 真要
   * 在那种时刻回调进去就是空指针。保留"有 buffer 才回调"这个惯判据，
   * 就同时挡住了"打向正在销毁的 task group"和"打向已释放的 upper 状态"。
   */

  if (priv->rx_apb != NULL)
    {
      FAR struct ap_buffer_s *apb = priv->rx_apb;

      priv->rx_apb = NULL;
      priv->dev.upper(priv->dev.priv, AUDIO_CALLBACK_DEQUEUE, apb, OK);
    }

  if (priv->tx_apb != NULL)
    {
      FAR struct ap_buffer_s *apb = priv->tx_apb;

      priv->tx_apb = NULL;
      priv->dev.upper(priv->dev.priv, AUDIO_CALLBACK_DEQUEUE, apb, OK);
    }

  /* 关断顺序必须和开机顺序反过来：先把 DAC 拉到最小音量，再关功放，
   * 最后才断模拟通路。原来的顺序（先 Close_Analog_DACPath 再关功放）是
   * 把还开着的 DAC 输出直接抽掉，那一下直流跳变经功放放大 = 结尾一声爆音。
   *
   * 每一步只看**真实通路位**，不看方向标签：标签是别的 app 一次 CONFIGURE
   * 就能改的谎话（见 struct 里的注释），按标签决定就会去关一条从没开过的
   * 模拟通路（重复关模拟级实测整机静默卡死），或者漏关真正开着的 DAC/功放
   * （录音把标签翻回 false 之后播放收尾就跳过了）。不变量：
   *   关之前一定开着、关完立刻清位，位关闭一次、开一次严格配对。
   */

  if (priv->dac_path_on)
    {
      HAL_AUDCODEC_Config_DACPath_Volume(&priv->codec, 0,
                                         SF32LB52_AUDIO_VOL_MUTE);
      HAL_AUDCODEC_Config_DACPath_Volume(&priv->codec, 1,
                                         SF32LB52_AUDIO_VOL_MUTE);
      up_mdelay(SF32LB52_AUDIO_SETTLE_MS);
    }

  if (priv->pa_on)
    {
      sf32lb52_audio_pa_enable(false);
      sf32lb52_audio_path_bit(&priv->pa_on, false, "pa_on",
                              "hw_stop 关功放");

      /* 留 5ms 让功放放电：紧接着（DAC 还开着时）就要断模拟通路，
       * 顺序反了那一下跳变会被还激活的功放放大成爆音。 */

      up_mdelay(5);
    }

  if (priv->dac_path_on)
    {
      HAL_AUDCODEC_Close_Analog_DACPath();
      sf32lb52_audio_path_bit(&priv->dac_path_on, false, "dac_path_on",
                              "hw_stop 关 DAC 模拟通路");
    }

  /* ADC 模拟通路同理：只有真开着才关。原来这里是无条件关的，加上新判据之后
   * 就危险了 —— 本函数现在会在"running=false、只剩某一两个通路位为真"的状态下
   * 也落进来（比如 hw_shutdown 之后又走到收尾），那种状态下 ADC 通路很可能
   * 早关掉了，无条件关一次就是重复关模拟级（实测整机静默卡死）。 */

  if (priv->adc_path_on)
    {
      HAL_AUDCODEC_Close_Analog_ADCPath();
      sf32lb52_audio_path_bit(&priv->adc_path_on, false, "adc_path_on",
                              "hw_stop 关 ADC 模拟通路");
    }

  priv->running = false;

  /* running 一清，这套持有者 id 就没有意义了。不清的话，下一个会话看到的是
   * 上一条会话的线程 id，判"持有者还在不在"就会拿错人。 */

  priv->holder_tid = (pid_t)-1;
  priv->holder_pid = (pid_t)-1;
  priv->user_tid   = (pid_t)-1;

  audinfo("audio stopped\n");
  return OK;
}

/****************************************************************************
 * Name: sf32lb52_audio_hw_shutdown
 *
 * Description:
 *   给 close() / shutdown() 用的最小化停止。
 *
 *   为什么不能照搬 hw_stop()：
 *   1. 上层 audio_close() 在关最后一个 fd 时是
 *      nxmutex_lock(upper->lock) → spin_lock_irqsave(upper->spinlock)
 *      （非 SMP 下等于关中断）之后，才在持锁关中断的状态里调
 *      lower->ops->shutdown()。所以这里既不能回调上层
 *      （upper 的锁被自己拿着），也不该做耗时的模拟级操作。
 *   2. 同一次会话里 AUDIOIOC_STOP 已经关过一次模拟通路，重复调
 *      HAL_AUDCODEC_Close_Analog_DACPath()/ADCPath() 关已经关掉的模拟级
 *      会整机卡死（实测：STOP 返回 0 后紧接着 close() 静默卡住、不复位、
 *      无 panic）。这正是 hw_test/audio_test 不犯的原因——它们的 fd 不是
 *      最后一个，shutdown() 压根没被调到。
 *
 ****************************************************************************/

static int sf32lb52_audio_hw_shutdown(FAR struct sf32lb52_audio_s *priv)
{
  sf32lb52_audio_ev(priv, priv->playback ? SF32LB52_AUDIO_EV_PLAY_END :
                                           SF32LB52_AUDIO_EV_SESS_END);

  /* DMAStop / DISABLE 只做寄存器操作，风险低于模拟通路关闭，先保留；
   * 这两个本身在关中断上下文里是否安全还没有证据，若仍卡死，
   * 下一步就把这两行也挪出 shutdown。
   *
   * 两条 DMAStop 同样加"先看句柄状态"的守卫，理由和 hw_stop 里那对一模一样
   * （见那段说明）：READY 就说明上一次 abort 已经收过尾，再 abort 一次写的是
   * 可能已被别人分配走的物理通道。TX0 同样是"平时空转、队列模式才用得上"，
   * 所以保留、只加守卫。 */

  if (priv->aprc.hdma[HAL_AUDPRC_TX_CH0] == NULL ||
      priv->aprc.hdma[HAL_AUDPRC_TX_CH0]->State != HAL_DMA_STATE_READY)
    {
      HAL_AUDPRC_DMAStop(&priv->aprc, HAL_AUDPRC_TX_CH0);
    }

  if (priv->aprc.hdma[HAL_AUDPRC_RX_CH0] == NULL ||
      priv->aprc.hdma[HAL_AUDPRC_RX_CH0]->State != HAL_DMA_STATE_READY)
    {
      HAL_AUDPRC_DMAStop(&priv->aprc, HAL_AUDPRC_RX_CH0);
    }

  __HAL_AUDPRC_DISABLE(&priv->aprc);

  /* 这里同样补软复位：close 掉最后一个 fd 是"把 AUDPRC 留在关着且没复位"
   * 的最后一次机会，下一个 app 打开时才能从一个干净的块上开始。
   * 只写 CFG/RX_CH0_CFG 两个寄存器、不等待不睡眠，所以在本函数所处的
   * "持上层锁 + 中断关闭"上下文里是安全的（和上面几条 DMAStop 一个量级）。
   * 复位清掉的配置由下一次 hw_start() 的 sf32lb52_audio_aprc_restore_adc()
   * 重写。判据与那次重建共用（见 sf32lb52_audio_aprc_has_config）：
   * 能重建才允许复位，避免留下"清空过、但没人重建"的块。 */

  if (sf32lb52_audio_aprc_has_config(priv))
    {
      sf32lb52_audio_aprc_soft_reset(priv);
    }

  /* DAC 的 DMA 同样要停：它是 DMA_CIRCULAR，不停就会无限重播最后一段
   * （"昂昂昂昂"卡住不停）。close() 是最后一个 fd 被关时的收尾路径，
   * 上一段播放如果没能自己停掉，这里就是唯一的机会。
   * 顺带把"停的时候还在传"这个异常打出来 —— 正常收尾时 DAC 状态早该是
   * READY，还带 BUSY_TX 就说明上一次传输没走完，下一次 write 会因此起不来。
   *
   * ★ 2026-09-19 修正：这里也必须和别处同一条守卫（理由逐字同 hw_stop 里那段
   * 说明）—— HAL_AUDCODEC_DMAStop() → HAL_DMA_Abort() 不看 State，对一条已经
   * 收好尾的句柄再 abort 一次，动的是那个物理通道的寄存器，而它可能已经归
   * 别的 DMAC1 用户（UART1/2 的 RX DMA）所有。本函数还在"持上层锁 + 关中断"
   * 的上下文里跑，一旦动错通道，连日志都未必出得来。 */

  if ((priv->codec.State[HAL_AUDCODEC_DAC_CH0] & HAL_AUDCODEC_STATE_BUSY_TX) != 0)
    {
      syslog(LOG_WARNING,
             "AUDIO: shutdown 时 DAC 仍是 BUSY_TX(0x%x)，传输没收尾，已停\n",
             priv->codec.State[HAL_AUDCODEC_DAC_CH0]);
    }

  if (priv->codec.hdma[HAL_AUDCODEC_DAC_CH0] != NULL &&
      priv->codec.hdma[HAL_AUDCODEC_DAC_CH0]->State != HAL_DMA_STATE_READY)
    {
      HAL_AUDCODEC_DMAStop(&priv->codec, HAL_AUDCODEC_DAC_CH0);
    }
  else
    {
      priv->codec.Instance->DAC_CH0_CFG &= ~AUDCODEC_DAC_CH0_CFG_DMA_EN;
    }

  priv->codec.State[HAL_AUDCODEC_DAC_CH0] = HAL_AUDCODEC_STATE_READY;

  priv->aprc.State[HAL_AUDPRC_TX_CH0] = HAL_AUDPRC_STATE_READY;
  priv->aprc.State[HAL_AUDPRC_RX_CH0] = HAL_AUDPRC_STATE_READY;

  /* 关功放是普通 GPIO 写，安全；但判据只能用真实位 pa_on，不能用方向标签
   * （标签会被别的 app 的一次 CONFIGURE 改掉：标签说"录音"时功放可能正开着，
   * 那就漏关；标签说"播放"时功放可能从没开过，那就是白关一下）。
   * 注意这条路径在**持上层锁 + 关中断**的上下文里（见本函数上面的注释），
   * 所以这里的 syslog 只在位真的变化时才出现 —— 正常收尾一次都不会打。 */

  if (priv->pa_on)
    {
      sf32lb52_audio_pa_enable(false);
      sf32lb52_audio_path_bit(&priv->pa_on, false, "pa_on",
                              "hw_shutdown 关功放");
    }

  /* 这里**不**关 DAC/ADC 模拟通路，也不清这两个位：关中断上下文里关模拟级
   * 会整机卡死（见上面的注释）。留下的"通路还开着"由下一次 hw_stop() 收尾 ——
   * 它的早退判据已经把这两个位算进去了（见 hw_stop 里的说明）。 */

  /* 最后一件必须做的事：把可能正阻塞在 read() 里的任务放出来。
   *
   * 这条路径只做 DMA/寄存器和 GPIO，原来一句唤醒都没有 —— 于是"最后一个 fd
   * 被 close、但没人发过 STOP"时（fd 被上层收掉、或 STOP 压根没送到驱动），
   * 录音线程只能靠 read() 自己那 5 秒上界出来，收尾慢得离谱（真机上表现
   * 就是 join 等 300ms 直接放弃）。这里的 post 与 hw_stop 里那句同一目的：
   * 给出一个不依赖任何超时的出口。
   *
   * 关于上下文：本函数在"持 upper->lock + 关中断"里被调（见函数头的说明），
   * nxsem_post() 在这种上下文里是合法的（它内部自己关中断，中断处理里也照样
   * 能调），不会去碰模拟级、也不会回调上层 —— 和上面刻意避开的那两类操作不同。
   * 顺序上 DMA 已经在前面停掉了，所以被唤醒的 read() 不会收到"采集完成"的假象。 */

  priv->rx_aborted = priv->rx_busy;
  priv->session_gen++;
  nxsem_post(&priv->rx_sem);

  priv->running = false;

  /* 和 hw_stop() 一样要清持有者 id：close() 之后 running 变假，留着上一条会话的
   * 线程 id 只会把下一次判定带偏（本函数所在的上下文不能加别的活，清几个
   * pid_t 只是普通赋值）。 */

  priv->holder_tid = (pid_t)-1;
  priv->holder_pid = (pid_t)-1;
  priv->user_tid   = (pid_t)-1;
  return OK;
}

/****************************************************************************
 * Name: sf32lb52_audio_getcaps
 *
 * Description: 上报设备能力（输入+输出、PCM、采样率）
 *
 ****************************************************************************/

static int sf32lb52_audio_getcaps(FAR struct audio_lowerhalf_s *dev, int type,
                                  FAR struct audio_caps_s *caps)
{
  DEBUGASSERT(caps && caps->ac_len >= sizeof(struct audio_caps_s));

  caps->ac_format.hw  = 0;
  caps->ac_controls.w = 0;

  switch (caps->ac_type)
    {
      case AUDIO_TYPE_QUERY:
        if (caps->ac_subtype == AUDIO_TYPE_QUERY)
          {
            /* 支持输入（麦克风）和输出（喇叭），PCM 格式 */

            caps->ac_controls.b[0] = AUDIO_TYPE_INPUT |
                                     AUDIO_TYPE_OUTPUT;
            caps->ac_format.hw     = 1 << (AUDIO_FMT_PCM - 1);
          }
        else
          {
            caps->ac_controls.b[0] = AUDIO_SUBFMT_END;
          }
        break;

      case AUDIO_TYPE_INPUT:
      case AUDIO_TYPE_OUTPUT:
        if (caps->ac_subtype == AUDIO_TYPE_QUERY)
          {
            caps->ac_controls.hw[0] = AUDIO_SAMP_RATE_8K |
                                      AUDIO_SAMP_RATE_16K |
                                      AUDIO_SAMP_RATE_44K |
                                      AUDIO_SAMP_RATE_48K;
            caps->ac_channels = 1;
          }
        else
          {
            caps->ac_controls.b[0] = AUDIO_SUBFMT_END;
          }
        break;

      case AUDIO_TYPE_FEATURE:
        if (caps->ac_subtype == AUDIO_FU_UNDEF)
          {
            caps->ac_controls.b[0] = AUDIO_FU_VOLUME;
          }
        break;

      default:
        caps->ac_subtype = 0;
        caps->ac_channels = 0;
        break;
    }

  return caps->ac_len;
}

/****************************************************************************
 * Name: sf32lb52_audio_configure
 *
 * Description: 处理上层 AUDIOIOC_CONFIGURE
 *
 ****************************************************************************/

static int sf32lb52_audio_configure(FAR struct audio_lowerhalf_s *dev,
                                    FAR const struct audio_caps_s *caps)
{
  FAR struct sf32lb52_audio_s *priv =
    (FAR struct sf32lb52_audio_s *)dev;
  int ret = OK;

  /* 方向只在这里记成"下一次 START 的意图"，**不动**真实通路状态、也不动
   * 当前会话方向（真实状态要等 sf32lb52_audio_hw_start() 真的开起硬件才提交）。
   *
   * 原来这两行是直接写 priv->playback 的，而 ai_audio.c 的
   * audio_hw_set_volume() 是 open + CONFIGURE(OUTPUT) + close、**不 START**：
   * 它一进来就把正在录音的那个会话的方向标签翻成"播放"，之后录音侧收尾就
   * 变成"按播放收尾"——去关一条没开的 DAC 通路，同时漏关真正开着的 ADC 通路。
   * 见 struct 里真实通路位那一段注释。 */

  switch (caps->ac_type)
    {
      case AUDIO_TYPE_INPUT:
        priv->pending_playback = false;
        ret = sf32lb52_audio_hw_configure(priv,
                                          caps->ac_controls.hw[0] |
                                          (caps->ac_controls.b[3] << 16),
                                          caps->ac_channels,
                                          caps->ac_controls.b[2]);
        break;

      case AUDIO_TYPE_OUTPUT:
        priv->pending_playback = true;
        ret = sf32lb52_audio_hw_configure(priv,
                                          caps->ac_controls.hw[0] |
                                          (caps->ac_controls.b[3] << 16),
                                          caps->ac_channels,
                                          caps->ac_controls.b[2]);
        break;

      case AUDIO_TYPE_FEATURE:
        if (caps->ac_format.hw == AUDIO_FU_VOLUME)
          {
            /* 标准音量接口：AUDIOIOC_SETPARAMTER + AUDIO_TYPE_FEATURE +
             * AUDIO_FU_VOLUME，值域 0..AUDIO_VOLUME_MAX(1000)。
             *
             * HAL 只接受 -36..+60 dB，**必须做映射** —— 原来把 0..1000
             * 直接当 dB 传进去，大于 60 的一律返回 HAL_ERROR，
             * 等于上层设音量根本没生效。
             *
             * 按方向生效：播放改 DAC 音量，录音改 ADC 数字增益
             * （这套 feature unit 里没有单独的麦克风增益控制，只能这样暴露）。
             *
             * **怎么判"这一次是给哪个方向的"**（判据的由来见文件顶部那段说明）：
             * 只有**正在读这条录音通路的那条线程自己**来设，才算录音方向的配置、
             * 才去改 ADC 数字增益；其余一律按播放音量处理（写 playback_db，
             * DAC 通路真开着才顺手写寄存器）。
             *
             *   原来为什么会落错：老判据 `!pending_playback && adc_path_on`
             *   把"没人在标播放意图"当成了"这次是给录音方向的"。可是上面
             *   audio_configure 只在 AUDIO_STATE_OPEN 时才放行 INPUT/OUTPUT 的
             *   CONFIGURE，FEATURE 是无条件放行的，所以录音会话期间下来的播放
             *   音量，它那记 CONFIGURE(OUTPUT) 被上层静默吞掉、驱动看到的
             *   pending_playback 仍是 false —— 一次**播放**音量就这样被当成录音
             *   音量写进麦克风增益（报警模块那次就是：+12dB → −2.4dB，
             *   报过一次警之后麦克风一直安静约 14dB）。
             *
             * 判据里那几位各自管什么：
             *   - running && !playback && adc_path_on：这条通路此刻真的在录
             *     （playback 是 START 成功才提交的已提交方向，这里只借它"这一刻
             *     硬件真在录"的意思；单拿它当方向标签会翻车，和身份一起用不会）；
             *   - !pending_playback：刚有人标过播放意图，不抢 —— 但上层会把别的
             *     app 那记 CONFIGURE(OUTPUT) 吞掉，所以它只能当"排除项"，
             *     不能当"这次是录音方向"的证据；
             *   - user_tid == 调用者 && getpid() == holder_pid：**唯一能证明
             *     "这次配置是给录音方向的"**的证据 —— 麦克风增益是这条正在录的
             *     通路的属性，只有**这条会话自己**正在把它的数据读走的那条线程
             *     才有资格代表它来调（user_tid 是 read() 每次记下的读取线程；
             *     再要求任务组等于 holder_pid，是挡住"别的 app 往本会话插一次
             *     write() 把 user_tid 翻成它自己"这一种绕法）。
             *
             * 音量的数值先记进 playback_db（下一次播放启动时由 hw_start 应用），
             * 只有 DAC 模拟通路真开着才顺手把音量写进寄存器 —— 通路没开时写它
             * 等于去碰一条没打开的模拟级，没有必要。
             */

            uint16_t volume = caps->ac_controls.hw[0];

            if (volume > AUDIO_VOLUME_MAX)
              {
                ret = -EDOM;
              }
            else
              {
                int db = SF32LB52_AUDIO_VOL_MIN +
                         (SF32LB52_AUDIO_VOL_MAX - SF32LB52_AUDIO_VOL_MIN) *
                         (int)volume / AUDIO_VOLUME_MAX;

                bool rec_dir = priv->running && !priv->playback &&
                               priv->adc_path_on && !priv->pending_playback &&
                               priv->user_tid >= 0 &&
                               priv->user_tid == nxsched_gettid() &&
                               priv->holder_pid >= 0 &&
                               getpid() == priv->holder_pid;

                /* 会去改 ADC、而这次其实不是录音方向的那一种（报警 / UI 在别人
                 * 录音期间下播放音量）：报一行，限频一秒一条
                 * （拖音量滑块时最坏就是每秒一行），板子上靠它确认没落错通路。 */

                if (!rec_dir && priv->adc_path_on && !priv->pending_playback)
                  {
                    clock_t now = clock_systime_ticks();

                    if (priv->vol_block_log == 0 ||
                        TICK2MSEC(now - priv->vol_block_log) >= 1000)
                      {
                        priv->vol_block_log = now;

                        syslog(LOG_WARNING,
                               "AUDIO: 音量 %d/1000 -> %d dB 记成播放音量，"
                               "不动麦克风增益（录音通路读取线程 %d，"
                               "本次线程 %d）\n",
                               volume, db, (int)priv->user_tid,
                               (int)nxsched_gettid());
                      }
                  }

                if (rec_dir)
                  {
                    priv->aprc.Init.adc_cfg.vol_l = db;
                    priv->aprc.Init.adc_cfg.vol_r = db;
                    HAL_AUDPRC_Config_ADCPath(&priv->aprc,
                                              &priv->aprc.Init.adc_cfg);
                  }
                else
                  {
                    priv->playback_db = db;

                    if (priv->dac_path_on)
                      {
                        HAL_AUDCODEC_Config_DACPath_Volume(&priv->codec, 0, db);
                        HAL_AUDCODEC_Config_DACPath_Volume(&priv->codec, 1, db);
                      }
                  }

                audinfo("volume %d/1000 -> %d dB (%s)\n", volume, db,
                        rec_dir ? "adc" : "dac");
              }
          }
        break;

      default:
        audwarn("WARNING: Unsupported configure type %d\n", caps->ac_type);
        break;
    }

  return ret;
}

/****************************************************************************
 * Name: sf32lb52_audio_shutdown
 *
 * Description:
 *   最后一个 fd 被 close() 时，上层在**持 upper->lock + 关中断**的状态下
 *   调到这里。所以只能走最小化停止，不能走完整的 hw_stop()
 *   （回调上层 + 重复关模拟通路都会卡死，详见 hw_shutdown 的注释）。
 *
 ****************************************************************************/

static int sf32lb52_audio_shutdown(FAR struct audio_lowerhalf_s *dev)
{
  FAR struct sf32lb52_audio_s *priv =
    (FAR struct sf32lb52_audio_s *)dev;

  /* 这里**故意不**用 priv->running 早退（原来那句 `if (!priv->running)
   * return OK;` 已删）：close() 是应用退出后的最后一道收尾，而残留
   * running==true 恰恰说明上一个持有者没走干净。无条件跑一遍
   * hw_shutdown()，把 running 和各个 DMA/通道状态清到底，
   * 下一个 app 才有机会拿到一个干净的会话（否则就是"ps 里没有那个 app 了，
   * 但 hw_test 的 audio_in_start 一直 -16 EBUSY"）。 */

  return sf32lb52_audio_hw_shutdown(priv);
}

/****************************************************************************
 * Name: sf32lb52_audio_tid_alive
 *
 * Description:
 *   这个线程 id 对应的任务还在不在（还在返回 true）。
 *
 *   为什么用 nxsched_get_tcb() 而不是 kill(pid, 0) 看 ESRCH：NuttX 里线程退出时
 *   nxsched_release_pid() 会把它的 pid 从 pidhash 里摘掉，三条退出路径都调
 *   （task/exit.c、pthread/pthread_exit.c、task/task_terminate.c），所以"查不到
 *   TCB"就等于"这个线程真的没了"，语义比信号那套干净得多 —— kill() 是"给某个
 *   任务组发信号"，拿线程 id 去问会先过权限/组那一层，ESRCH 和 EPERM 分不干净，
 *   把"没权限"当成"不存在"就是替一个活着的会话拆台。
 *   查到了必须配对释放（引用计数，见 sched/sched/sched_gettcb.c）。
 *
 *   还要把"取值失败"和"没记录（-1）"分开：记进来的值有可能是
 *   nxsched_gettid() 的失败返回值 -ESRCH，那不是"没记录"，见下面的实现。
 *
 ****************************************************************************/

static bool sf32lb52_audio_tid_alive(pid_t tid)
{
  FAR struct tcb_s *tcb;

  /* -ESRCH 不是"没记录"，是取值失败：nxsched_gettid() 的 inline 实现只在
   * 调用者真的处于 RUNNING 态时才回 tid，否则回 -ESRCH
   * （openvela/nuttx/include/nuttx/sched.h 里那段实现），而 hw_start/read/write
   * 是把这个返回值**原样**记进 holder_tid/user_tid 的。
   * 取值失败时我们并没有"这个线程没了"的证据，所以按**活着**处理（保守）：
   * 和 -1 一起归零就等于拿一个查不到的 id 去判"持有者已死"，替活会话拆台。 */

  if (tid == (pid_t)-ESRCH)
    {
      return true;
    }

  if (tid < 0)
    {
      return false;
    }

  tcb = nxsched_get_tcb(tid);
  if (tcb == NULL)
    {
      return false;
    }

  nxsched_put_tcb(tcb);
  return true;
}

/****************************************************************************
 * Name: sf32lb52_audio_holder_alive
 *
 * Description:
 *   当前这次会话（running 为真）还有没有有效持有者。
 *
 *   三条独立证据，任一条成立就算"还活着"：
 *     1) 把这次会话起起来的线程还在；
 *     2) 最近一次 read()/write() 的线程还在（会话真的有人在用）；
 *     3) 起会话的那个任务组还在（app 没退）。
 *   三条都查不到 TCB 才判"没有有效持有者" —— 这时候反方向那条通路已经没人要了，
 *   调用方可以强制收干净再切过去（调用方还有一条宽限期，见 sf32lb52_audio_start）。
 *
 *   第 3 条为什么是"判残留"的**必要条件**（上一轮把它只当日志，代价是一整条误伤
 *   路径）：本板真有"起会话的线程是短命线程"的用法 —— robot_ui 用
 *   PTHREAD_CREATE_DETACHED 起的 voice_open_thread 在 audio_in_start() 之后立刻
 *   return NULL；ai_audio.c 在播放线程里恢复录音、随后播放线程自己退出。
 *   于是在"新会话刚 START 成功 → 起会话的线程已退出 → 真正读数据的线程还没进
 *   第一次 read()（user_tid 还是 -1）"这个窗口里，前两条证据**必然**都是空的，
 *   可会话是**活的**：光看线程就会把这条活会话判成残局强停，后果比原来的 -110
 *   严重得多（现场把正在录音/正在播报的会话拆掉）。
 *   NuttX 里 tg_pid == 组长 pid（holder_pid 取的就是 getpid()，nxsched_get_tcb
 *   按 pidhash 查），所以"holder_pid 也查不到 TCB"**等价于"整个 app 没了"**：
 *   这时候才真的是"app 死了留下残局、没人会再来收"。这条判据一刀砍掉上面那个
 *   误伤窗口（那两个场景里 app 都活着），而真正要治的场景仍然覆盖得到。
 *
 *   代价是**有意的取舍**：app 还活着、但它那条播放线程真的死了时，这种残留不再
 *   被认出来，录音端退化成 -EBUSY —— 交给上层重试，或由那个 app 自己手里的 fd
 *   走 close()/AUDIOIOC_STOP 收尾。相比"误停一条活会话"，这个代价可以接受。
 *
 *   判"活着"时调用方的行为**一个字都不能变**（仍然 -EBUSY）：半双工下录放必须
 *   串行，不能替正在用的人把通路拆了。
 *
 ****************************************************************************/

static bool sf32lb52_audio_holder_alive(FAR struct sf32lb52_audio_s *priv)
{
  /* 一个都没记过（理论上到不了：running 只由 hw_start 置真，那里一定会记），
   * 或者记下的值是 nxsched_gettid() 的失败值 -ESRCH（同样说明不了任何事）：
   * 判不了就按"有人"处理，保持原来的 -EBUSY 行为。 */

  if (priv->holder_tid < 0 && priv->user_tid < 0)
    {
      return true;
    }

  if (sf32lb52_audio_tid_alive(priv->holder_tid) ||
      sf32lb52_audio_tid_alive(priv->user_tid))
    {
      return true;
    }

  /* 两条线程证据都没了，还差最后一条：**整个任务组也没了**才叫残留。
   * 组还在 = app 还活着（上面那两个短命线程的场景都落在这里）→ 按"有人"处理。 */

  return sf32lb52_audio_tid_alive(priv->holder_pid);
}

/****************************************************************************
 * Name: sf32lb52_audio_start
 *
 * Description: 处理上层 AUDIOIOC_START
 *
 ****************************************************************************/

static int sf32lb52_audio_start(FAR struct audio_lowerhalf_s *dev)
{
  FAR struct sf32lb52_audio_s *priv =
    (FAR struct sf32lb52_audio_s *)dev;
  bool want;
  bool live;
  bool dir;
  uint32_t age_ms;

  /* 本次要起的方向：取最近一次 CONFIGURE 记下的意图（START 成功才提交成
   * priv->playback）。不加这一步而直接读 priv->playback 的话，这里的"要起的
   * 方向"其实是上一次会话的方向 —— 而这个函数在方向不同的时候必须拒绝，
   * 判错方向就等于替别人把正在用的通路拆了。 */

  want = priv->pending_playback;

  /* running 已经是 true 时**不能**直接 return OK（原来就是这么写的，把整段
   * hw_start 跳过了）：
   *
   * 这套硬件是半双工，设计上同一时刻只有一个持有者；NuttX 上层只在
   * PREPARED→RUNNING 时才调 start，所以 running==true 只可能是
   *   1) 上一个持有者（app）录音中途挂掉/退出，没走 close → 残留；
   *   2) 同一方向的重复 START（同一个 app 自己重来一次），旧通路还没收尾。
   * 两种情况都需要先把旧通路收干净。直接 OK 走人的后果是：ADC 模拟通路、
   * AUDPRC 使能（都在 hw_start 里）全没做，read() 的 DMA 起得来却永远等不到
   * 完成中断，5 秒后以 -110 收场，应用从此变聋。
   *
   * 但"先完整 stop 再 start"这件事**只在两种情况下**才允许做：
   *   ① 方向一致（上一个持有者和本次要起的是同一条通路，收尾不会动到别人）；
   *   ② 方向不一致，但那条通路的持有者线程已经不存在了（死掉的那个 app/线程
   *      留下的残局，见下面的异方向分岔）。
   * 除这两种以外一律拒绝。两个 app 同时在线、另一个方向的通路是别人正在用的，
   * 拆掉它的后果是录音被中止（或播放只剩第一块），比拒绝这次 START 严重得多。
   * 所以这里按"真实在跑的方向 + 持有者还在不在"分岔。
   */

  if (priv->running)
    {
      /* "真实在跑的方向"= 已提交的方向 + 至少一条通路位确实开着。
       * 三个位全假说明 running 是残留（没有对应的硬件状态），按"没有方向"
       * 处理：那种情况下任何方向都只是残局，可以收尾后重起。
       * 这里**不**看 priv->pending_playback 之外的东西：pending 是本次要起的
       * 方向，把两者比一比就知道是不是在抢别人的通路。 */

      live = priv->dac_path_on || priv->pa_on || priv->adc_path_on;
      dir  = priv->playback;

      if (live && dir != want)
        {
          /* 异方向：先看对面是不是真有活人在用。
           *
           * 持有者还活着 → **拒绝**，不替对方拆台。返回 -EBUSY 是安全的：上层
           * 两个 app 都按 -16 处理 —— audio_in_start() 把它透出来（它自己有一拍
           * 重试），robot_ui 的播报路径报 "audio open failed" 后当次静默走完，
           * 都不会因为这次拒绝崩掉。
           *
           * 持有者线程已经不存在、并且整个任务组也没了（= 那个 app 真死了）→ 这是
           * 死掉的那个 app 留下的残局：没人会再来收，这里不收的话反方向的 START 就
           * 永远卡在 -EBUSY，录音端从此永久聋到重启板子。
           * 判定依据见 sf32lb52_audio_holder_alive（线程 + 任务组三条证据）以及下面
           * 的宽限期（会话刚起来的那一小段绝不强停）。 */

          if (sf32lb52_audio_holder_alive(priv))
            {
              syslog(LOG_WARNING,
                     "AUDIO: 拒绝方向切换：要起 %s，但 %s 通路正被占用"
                     "（dac=%d pa=%d adc=%d running=%d 持有线程=%d 最近读写=%d "
                     "组=%d）→ -EBUSY\n",
                     want ? "播放" : "录音",
                     dir ? "播放" : "录音",
                     (int)priv->dac_path_on, (int)priv->pa_on,
                     (int)priv->adc_path_on, (int)priv->running,
                     (int)priv->holder_tid, (int)priv->user_tid,
                     (int)priv->holder_pid);
              return -EBUSY;
            }

          /* 会话还没过宽限期 → **绝不** force-stop，只照原样返回 -EBUSY。
           *
           * 误伤窗口永远在会话开头：刚 START 成功的会话，起会话的线程可能已经退出
           * （短命线程，见 struct 里 holder_tid 的说明），而真正读数据的线程还没进
           * 第一次 read()（user_tid 还是 -1）—— 这段时间里线程证据看起来是空的，
           * 会话却是活的。宽限期把这一段单独掐掉：宁可按'有人在用'回 -EBUSY，也不去
           * 拆一条刚开始的会话（-EBUSY 对上层是安全值：audio_in_start() 会重试一次，
           * robot_ui 的播报路径报错后当次静默走完）。 */

          age_ms = (uint32_t)TICK2MSEC(clock_systime_ticks() - priv->holder_since);
          if (age_ms < SF32LB52_AUDIO_SWITCH_GRACE_MS)
            {
              syslog(LOG_WARNING,
                     "AUDIO: 方向冲突：%s 通路是 %u ms 前刚起的（还不到宽限期 %d ms），"
                     "按'占用者可能只是还没进第一次读写'处理（持有线程=%d 最近读写=%d "
                     "组=%d 都查不到，但会话还太新）→ -EBUSY\n",
                     dir ? "播放" : "录音", (unsigned)age_ms,
                     (int)SF32LB52_AUDIO_SWITCH_GRACE_MS,
                     (int)priv->holder_tid, (int)priv->user_tid,
                     (int)priv->holder_pid);
              return -EBUSY;
            }

          syslog(LOG_WARNING,
                 "AUDIO: 方向冲突且占用者已无人生还（要起 %s，%s 通路残留："
                 "起会话的线程 %d / 最近读写的线程 %d 都查不到 TCB，"
                 "起会话的任务组 %d 也查不到 TCB（= 整个 app 没了），"
                 "并且这条会话已经起了 %u ms（>= 宽限期 %d ms）），"
                 "判定为残留会话，强制做一次干净 stop 再切到 %s\n",
                 want ? "播放" : "录音",
                 dir ? "播放" : "录音",
                 (int)priv->holder_tid, (int)priv->user_tid,
                 (int)priv->holder_pid,
                 (unsigned)age_ms,
                 (int)SF32LB52_AUDIO_SWITCH_GRACE_MS,
                 want ? "播放" : "录音");
        }
      else
        {
          /* 同方向：残留 / 重复 START，收干净再起 */

          syslog(LOG_WARNING,
                 "AUDIO: 检测到残留运行状态（running=1 dir=%s 与本次要起的 %s 一致，"
                 "dac=%d pa=%d adc=%d），先做一次干净 stop 再启动\n",
                 dir ? "播放" : "录音",
                 want ? "播放" : "录音",
                 (int)priv->dac_path_on, (int)priv->pa_on,
                 (int)priv->adc_path_on);
        }

      /* 两条路都走到这里：把旧会话收干净（异方向残留是刚判定完的，同方向是
       * 重复 START），下面 hw_start 才从零开始把想要的那条通路起起来。 */

      sf32lb52_audio_hw_stop(priv);
    }

  return sf32lb52_audio_hw_start(priv, want);
}

/****************************************************************************
 * Name: sf32lb52_audio_stop
 *
 ****************************************************************************/

#ifndef CONFIG_AUDIO_EXCLUDE_STOP
static int sf32lb52_audio_stop(FAR struct audio_lowerhalf_s *dev)
{
  return sf32lb52_audio_hw_stop((FAR struct sf32lb52_audio_s *)dev);
}
#endif

/****************************************************************************
 * Name: sf32lb52_audio_enqueuebuffer
 *
 * Description:
 *   上层投递一个 buffer：
 *   - 播放：数据经 AUDPRC TX0 DMA 送向 DAC
 *   - 录音：AUDPRC RX0 DMA 把 ADC 数据收进 buffer
 *   完成由 DMA 中断回调（HAL_AUDPRC_Tx/RxCpltCallback）通知。
 *
 ****************************************************************************/

static int sf32lb52_audio_enqueuebuffer(FAR struct audio_lowerhalf_s *dev,
                                        FAR struct ap_buffer_s *apb)
{
  FAR struct sf32lb52_audio_s *priv =
    (FAR struct sf32lb52_audio_s *)dev;
  HAL_StatusTypeDef res;

  DEBUGASSERT(priv && apb);

  /* 队列模式的方向：正在跑就按**已提交的**会话方向（别人一次 CONFIGURE 不能
   * 把在跑的会话的 buffer 改道），没在跑就按刚 CONFIGURE 的意图。
   * 这是原来的等价语义 —— 原来 priv->playback 会被 CONFIGURE 直接改掉，
   * 也就是"没 START 也按 CONFIGURE 的方向"，现在只有 START 过才算数。
   * 本板两个 app（hello_app / robot_ui）走的都是 write/read 直通，没有队列
   * 模式的使用者，这里只是把语义摆正、不留一个会被方向标签污染的口子。 */

  if (priv->running ? priv->playback : priv->pending_playback)
    {
      priv->tx_apb = apb;
      res = HAL_AUDPRC_Transmit_DMA(&priv->aprc, apb->samp, apb->nbytes,
                                    SF32LB52_AUDIO_PRC_TX_CH);
    }
  else
    {
      priv->rx_apb = apb;
      res = HAL_AUDPRC_Receive_DMA(&priv->aprc, apb->samp, apb->nbytes,
                                   SF32LB52_AUDIO_PRC_RX_CH);
    }

  if (res != HAL_OK)
    {
      auderr("ERROR: DMA enqueue failed: %d\n", res);
      priv->tx_apb = NULL;
      priv->rx_apb = NULL;
      return -EIO;
    }

  return OK;
}

/****************************************************************************
 * Name: sf32lb52_audio_cancelbuffer
 *
 ****************************************************************************/

static int sf32lb52_audio_cancelbuffer(FAR struct audio_lowerhalf_s *dev,
                                       FAR struct ap_buffer_s *apb)
{
  FAR struct sf32lb52_audio_s *priv =
    (FAR struct sf32lb52_audio_s *)dev;

  if (priv->tx_apb == apb)
    {
      /* 守卫的规矩和 hw_stop / hw_shutdown 里那两对一样：READY 说明这条通道
       * 已经收过尾，再 abort 一次写的是可能已被别人分配走的物理通道。
       * 这条路只有在**队列模式**（enqueuebuffer 真的起过这条通道）时才走得
       * 进来，正常情况下 tx_apb/rx_apb 都是 NULL、一个 abort 都不会发。 */

      if (priv->aprc.hdma[SF32LB52_AUDIO_PRC_TX_CH] == NULL ||
          priv->aprc.hdma[SF32LB52_AUDIO_PRC_TX_CH]->State != HAL_DMA_STATE_READY)
        {
          HAL_AUDPRC_DMAStop(&priv->aprc, SF32LB52_AUDIO_PRC_TX_CH);
        }

      priv->tx_apb = NULL;
    }

  if (priv->rx_apb == apb)
    {
      if (priv->aprc.hdma[SF32LB52_AUDIO_PRC_RX_CH] == NULL ||
          priv->aprc.hdma[SF32LB52_AUDIO_PRC_RX_CH]->State != HAL_DMA_STATE_READY)
        {
          HAL_AUDPRC_DMAStop(&priv->aprc, SF32LB52_AUDIO_PRC_RX_CH);
        }

      priv->rx_apb = NULL;
    }

  return OK;
}

/****************************************************************************
 * Name: sf32lb52_audio_ioctl
 *
 ****************************************************************************/

static int sf32lb52_audio_ioctl(FAR struct audio_lowerhalf_s *dev, int cmd,
                                unsigned long arg)
{
  int ret = OK;

  switch (cmd)
    {
      case AUDIOIOC_HWRESET:
        audinfo("AUDIOIOC_HWRESET\n");
        break;

      default:
        ret = -ENOTTY;
        break;
    }

  return ret;
}

/****************************************************************************
 * Name: sf32lb52_audio_write
 *
 * Description:
 *   直通写入：把应用数据经 codec 自带的 DAC DMA（HAL_AUDCODEC_Transmit_DMA）
 *   送出并等待播放完成。
 *   （NuttX 标准 buffer 队列流程之外的简单同步播放接口）
 *
 ****************************************************************************/

static ssize_t sf32lb52_audio_write(FAR struct audio_lowerhalf_s *dev,
                                    FAR const char *buffer, size_t buflen)
{
  FAR struct sf32lb52_audio_s *priv =
    (FAR struct sf32lb52_audio_s *)dev;
  HAL_StatusTypeDef res;
  int ret;

  if (buffer == NULL || buflen == 0 || !priv->running)
    {
      return 0;
    }

  /* 记下"这次会话最近一次真的有人在写"（判残留时的一条证据，见
   * sf32lb52_audio_holder_alive）。放在 running 早退之后：设备没在跑就不算
   * 有人用它。 */

  priv->user_tid = nxsched_gettid();

  /* 起这次等待前的两句清理：
   *
   *   nxsem_reset：上一次 write 完成时 ISR 的 post 万一来晚了一步（落在"本次
   *     等待已经返回、wr_busy 已经清零"之后），会留下一个没人要的计数，让本次
   *     写一进等待就立刻返回、把没播完的块当播完了交上去。这里清掉。
   *     （清在 wr_busy = true 之前：post 只可能发生在 wr_busy 为真时，所以这一
   *     行之后不会有任何在途的 post 被误伤。）
   *   wd_cancel：上一次 write 万一没走到取消（线程被删、被强杀）就留下了一记
   *     待触发的私有心跳，它的回调**只看 wr_busy**，所以在下面 wr_busy 置真
   *     之后随时可能补一记 post —— 必须在置真之前把它取消掉。 */

  nxsem_reset(&priv->wr_sem, 0);
  (void)wd_cancel(&priv->wr_wait_wdog);

  priv->wr_busy = true;

  /* 数据体检：样本是否为空/异常（全 0 会让 DAC 只输出直流，只有开关爆音） */
  {
    FAR const int16_t *p = (FAR const int16_t *)buffer;
    int i;
    int mx = 0;

    for (i = 0; i < 256; i++)
      {
        int v = p[i];

        if (v < 0)
          {
            v = -v;
          }

        if (v > mx)
          {
            mx = v;
          }
      }

  }


  clock_t t0 = clock_systime_ticks();

  (void)t0;

  /* ★ 起传输前必须把状态摆正。
   *
   * vendor HAL 里 HAL_AUDCODEC_DMAStop() 那句 "State = READY" 是**注释掉的**，
   * 而 HAL_AUDCODEC_Transmit_DMA() 只要看到 State 还带 HAL_AUDCODEC_STATE_BUSY_TX
   * 就直接 return HAL_BUSY。于是只要上一次传输没走到本函数末尾那两行收尾
   * （上一次会话被 STOP/close 打断、上一次 write 起不来、掉电前的残留……），
   * State 就永远卡在 BUSY_TX：之后**每一次** write 都在这里失败、返回 0，
   * 上层看到的就是"界面显示正在播放，但一个字节都没播出去"。
   *
   * 兜法：残留就先把 DMA 停掉，再把状态强制摆成 READY。这条警告正常播放时
   * 一次都不该出现 —— 出现了就说明上一条路径没收好尾，串口日志能直接看到。 */

  if ((priv->codec.State[HAL_AUDCODEC_DAC_CH0] & HAL_AUDCODEC_STATE_BUSY_TX) != 0)
    {
      syslog(LOG_WARNING,
             "AUDIO: DAC 状态残留 BUSY_TX(0x%x)，先停 DMA 再重起\n",
             priv->codec.State[HAL_AUDCODEC_DAC_CH0]);
      HAL_AUDCODEC_DMAStop(&priv->codec, HAL_AUDCODEC_DAC_CH0);
    }

  priv->codec.State[HAL_AUDCODEC_DAC_CH0] = HAL_AUDCODEC_STATE_READY;

  /* DMA 句柄自己也有状态机：HAL_AUDCODEC_Transmit_DMA 里
   * HAL_DMA_Start_IT() 的返回值是被丢掉的，句柄如果不是 READY 它直接返回
   * HAL_BUSY、DMA 根本没起来，而 codec 那边的 BUSY_TX 已经置上了 —— 现象
   * 就是 write() 干等 600ms 超时、一个字节都没播。这里一并兜住并留证。 */

  if (priv->codec.hdma[HAL_AUDCODEC_DAC_CH0] != NULL &&
      priv->codec.hdma[HAL_AUDCODEC_DAC_CH0]->State != HAL_DMA_STATE_READY)
    {
      syslog(LOG_WARNING,
             "AUDIO: DAC 的 DMA 句柄状态异常(0x%x)，强制复位成 READY\n",
             priv->codec.hdma[HAL_AUDCODEC_DAC_CH0]->State);
      priv->codec.hdma[HAL_AUDCODEC_DAC_CH0]->State = HAL_DMA_STATE_READY;
    }

  res = HAL_AUDCODEC_Transmit_DMA(&priv->codec, (FAR uint8_t *)buffer, buflen,
                                  HAL_AUDCODEC_DAC_CH0);
  if (res != HAL_OK)
    {
      /* 起不来也别把上一次的 DMA 留在循环模式里：DAC 的 DMA 是
       * DMA_CIRCULAR，残留的传输会无限重播最后一段（"昂昂昂昂"）。
       * 同时把 res / Lock / State 打出来 —— 这次现场就是靠它定位的。 */

      syslog(LOG_ERR,
             "AUDIO: Transmit_DMA 失败 res=%d Lock=%d State=0x%x dmastate=0x%x "
             "buflen=%u running=%d\n",
             (int)res, (int)priv->codec.Lock,
             priv->codec.State[HAL_AUDCODEC_DAC_CH0],
             priv->codec.hdma[HAL_AUDCODEC_DAC_CH0] != NULL ?
               (int)priv->codec.hdma[HAL_AUDCODEC_DAC_CH0]->State : -1,
             (unsigned)buflen, (int)priv->running);

      HAL_AUDCODEC_DMAStop(&priv->codec, HAL_AUDCODEC_DAC_CH0);
      priv->codec.State[HAL_AUDCODEC_DAC_CH0] = HAL_AUDCODEC_STATE_READY;
      priv->wr_busy = false;
      return 0;
    }

  {
    /* 按实际采样率算等待上限（ms），再加 500ms 余量。
     *
     * "到点了"由本驱动自己的心跳（wr_wait_wdog）产生，不用内核的
     * rtcb->waitdog + nxsem_timeout —— 后者会和下面两个 TxCplt ISR 里的
     * nxsem_post 抢同一个 waitobj（`if (priv->wr_busy) nxsem_post(&priv->wr_sem)`），
     * 正是上板硬崩的那条断言。 */

    uint32_t wait_ms = (uint32_t)(((uint32_t)(buflen / 2) * 1000U) /
                       (uint32_t)(priv->samplerate ? priv->samplerate : 16000)) + 500U;

    ret = sf32lb52_audio_tx_wait_slice(priv, wait_ms);
  }

  priv->wr_busy = false;

  /* 停止 DMA 传输（单次播放完成）。
   *
   * 注意这里的两下收尾大多已经做过了：DMA 完成中断里的
   * HAL_AUDCODEC_TxCpltCallback 已经把 DAC 的 DMA 使能和 DMA 通道关掉了
   * （sf32lb52_audio_tx_freeze）—— 就是为了不让循环 DMA 在"中断返回 → 本线程
   * 被唤醒"这段窗口里从块首重播。这里再走一遍是给下面几条路兜底：
   * 中断没来（等超时）、Transmit 起不来、上层提前把会话停掉。
   * HAL_AUDCODEC_DMAStop() 内部是 HAL_DMA_Abort + 清 DAC_CHx_CFG 的 DMA_EN，
   * 幂等，重复调没问题。 */

  HAL_AUDCODEC_DMAStop(&priv->codec, HAL_AUDCODEC_DAC_CH0);
  priv->codec.State[HAL_AUDCODEC_DAC_CH0] = HAL_AUDCODEC_STATE_READY;

  if (ret < 0)
    {
      /* 等超时：DMA 没在预期时间内报完成，本块算丢。这条**正常播放时不该
       * 出现**（每块 100ms，等的是 100ms+500ms 余量），出现就说明播放通路
       * 出问题了，串口日志要留着看。 */

      syslog(LOG_ERR, "AUDIO: 等播放完成超时（buflen=%u），本块丢弃\n",
             (unsigned)buflen);
      return 0;
    }

  return buflen;
}

/****************************************************************************
 * Name: sf32lb52_audio_tx_wait_timeout
 *
 * Description:
 *   write() 那一次等待（采样时长 + 500ms）的心跳到点了。
 *
 *   跑在 systick 中断里，只做两件**与 TCB 无关**的事：立 wr_wait_to 旗（告诉
 *   write()"这次是到点了，不是播放完成"），以及**无条件** post 一次 wr_sem 把
 *   人叫醒。
 *   ★ 这里必须无条件 post：心跳就是"到点叫醒去检查超时预算"的那一下，一旦加上
 *   `if (wr_busy) 才 post` 这类条件，只要标志不为真就叫不醒，超时机制被废掉，
 *   write 会永久卡住（rx 那边老实现实测就是这么炸的：diag 里 `wait=311282 ms`）。
 *   多 post 一次只是让那一轮多转一圈，少 post 一次是永久卡死。
 *   **不打日志**（中断里碰串口会抢控制台锁把整机挂住）。
 *
 *   与内核那条 nxsem_timeout 的根本区别：从不读 task_state，也从不读/写
 *   waitobj，只投递一个信号量计数。
 ****************************************************************************/

static void sf32lb52_audio_tx_wait_timeout(wdparm_t arg)
{
  FAR struct sf32lb52_audio_s *priv = (FAR struct sf32lb52_audio_s *)arg;

  priv->wr_wait_to = true;

  nxsem_post(&priv->wr_sem);
}

/****************************************************************************
 * Name: sf32lb52_audio_tx_wait_slice
 *
 * Description:
 *   write() 等完这一次播放的完整动作：武装私有心跳 → 阻塞等 wr_sem → 取消心跳。
 *   （只武装一次，wait_ms 就是这一次播放的预算。）
 *
 *   返回值语义**与内核那句 nxsem_tickwait_uninterruptible 完全一致**：
 *     OK         = 这一次里有人 post 到了东西（播放完成中断 / 其它唤醒）；
 *     -ETIMEDOUT = 谁也没 post，是本驱动自己的心跳叫醒的。
 *   所以 write() 后面的出口（ret < 0 → 打日志 + 本块丢弃返回 0；OK → 返回
 *   buflen）一个字都不用改。
 *
 *   用 nxsem_wait_uninterruptible 而不是 nxsem_wait：语义与那句 tickwait 的
 *   "uninterruptible" 一样，信号打断时不会提前返回，这一次等待仍由我们的
 *   心跳收口。
 ****************************************************************************/

static int sf32lb52_audio_tx_wait_slice(FAR struct sf32lb52_audio_s *priv,
                                        uint32_t wait_ms)
{
  int ret;

  priv->wr_wait_to = false;

  /* 与 rx 那条同形：先腾槽位，再检查 wd_start 有没有真的装上（理由见
   * sf32lb52_audio_rx_wait_slice 里那段 2026-09-19 的定案说明）。 */

  (void)wd_cancel(&priv->wr_wait_wdog);

  ret = wd_start(&priv->wr_wait_wdog, MSEC2TICK(wait_ms),
                 sf32lb52_audio_tx_wait_timeout, (wdparm_t)priv);

  if (ret < 0)
    {
      syslog(LOG_ERR,
             "AUDIO: TX 心跳武装失败(%d)，本次等待退回内核定时等待兜底\n", ret);

      ret = nxsem_tickwait_uninterruptible(&priv->wr_sem, MSEC2TICK(wait_ms));
      if (ret == -ETIMEDOUT)
        {
          priv->wr_wait_to = true;
        }

      return ret;
    }

  ret = nxsem_wait_uninterruptible(&priv->wr_sem);

  /* wd_cancel 对已经到点的看门狗只是返回 -EINVAL；多出来的那一次 post 由 write()
   * 开头那句 nxsem_reset 收掉，不会串到下一次写上。 */

  (void)wd_cancel(&priv->wr_wait_wdog);

  return (ret == OK && priv->wr_wait_to) ? -ETIMEDOUT : ret;
}

/****************************************************************************
 * Name: sf32lb52_audio_rx_wait_timeout
 *
 * Description:
 *   read() 那一次等待（5 秒上界）的心跳到点了。
 *
 *   与 TX 那记心跳逐条对应，跑在 systick 中断里、只做两件**与 TCB 无关**的事：
 *   立 rx_wait_to 旗（告诉 read"这次是到点了"）＋ **无条件** post 一次 rx_sem。
 *   ★ 必须无条件：这记心跳就是"到点叫醒去看看这一帧到底有没有数据"的那一下，
 *   一旦加上 `if (rx_busy) 才 post` 这类条件，只要标志不为真就叫不醒，
 *   超时机制被废掉 —— rx 那边的老实现（APPLICATION 侧的监听守护也吃过一次）
 *   实测就是这么炸的（diag 里 wait 涨到 311282 ms）。
 *   **不打日志**（中断里碰串口会抢控制台锁把整机挂住）。
 *
 *   与内核 nxsem_timeout 的根本区别：从不读 task_state，也从不读/写 waitobj，
 *   只投递一个信号量计数 —— 所以 sem_waitirq.c:137 那条断言在 read 这条等待上
 *   永远不会被触发（理由见 struct 里 rx_wait_wdog 那一段）。
 ****************************************************************************/

static void sf32lb52_audio_rx_wait_timeout(wdparm_t arg)
{
  FAR struct sf32lb52_audio_s *priv = (FAR struct sf32lb52_audio_s *)arg;

  priv->rx_wait_to = true;

  nxsem_post(&priv->rx_sem);
}

/****************************************************************************
 * Name: sf32lb52_audio_rx_wait_slice
 *
 * Description:
 *   read() 等完这一次采集：武装私有心跳 → 阻塞等 rx_sem → 取消心跳。
 *
 *   返回值语义**与原来那句 nxsem_clockwait_uninterruptible 完全一致**：
 *     OK         = 这一次里有人 post 到了东西（DMA 完成 / TE 错误回调 /
 *                  hw_stop / hw_shutdown）；
 *     -ETIMEDOUT = 谁也没 post，是本驱动自己的心跳叫醒的。
 *   所以 read() 后面那条 `if (ret < 0 || rx_err)` 的出口一个字都不用改。
 *
 *   用 nxsem_wait_uninterruptible 而不是 nxsem_wait：语义与老实现那句
 *   clockwait 的 "uninterruptible" 一样（信号打断不提前返回），这一次等待
 *   仍由我们自己的心跳收口。
 ****************************************************************************/

static int sf32lb52_audio_rx_wait_slice(FAR struct sf32lb52_audio_s *priv,
                                        uint32_t wait_ms)
{
  int ret;

  priv->rx_wait_to = false;

  /* 先把槽位腾空：上一拍万一是"心跳到点了、wd_cancel 没来得及跑"，直接
   * wd_start 会返回 -EBUSY 而且**什么都不做**。 */

  (void)wd_cancel(&priv->rx_wait_wdog);

  ret = wd_start(&priv->rx_wait_wdog, MSEC2TICK(wait_ms),
                 sf32lb52_audio_rx_wait_timeout, (wdparm_t)priv);

  if (ret < 0)
    {
      /* 心跳没装上 = 这一次等待会**没有上界**。
       *
       * 2026-09-19 真机定案：现场 read 永久卡在下面的
       * nxsem_wait_uninterruptible 上（dumpstack 落在这里、diag 的 wait 涨到
       * 144 s 而 rxt 一直是 0），录音线程永不退出 → 设备被它占死 → 上层所有
       * 重开都被 -EBUSY 挡回，表现就是"播放一次音频之后麦克风再也起不来"。
       *
       * 退回内核定时等待兜底：它至少保证 wait_ms 会回来。代价是理论上可能撞
       * sem_waitirq.c:137 那条断言（本文件顶部记过这个风险）—— 但"偶尔可能
       * 断言"远好于"必然永久卡死"，而且这条路只在 wd_start 失败时才走。 */

      syslog(LOG_ERR,
             "AUDIO: RX 心跳武装失败(%d)，本次等待退回内核定时等待兜底\n", ret);

      ret = nxsem_tickwait_uninterruptible(&priv->rx_sem, MSEC2TICK(wait_ms));
      if (ret == -ETIMEDOUT)
        {
          priv->rx_wait_to = true;      /* 与下面的返回值语义对齐 */
        }

      return ret;
    }

  ret = nxsem_wait_uninterruptible(&priv->rx_sem);

  /* wd_cancel 对已经到点的看门狗只是返回 -EINVAL；多出来的那一次 post 由
   * 下一次 read 开头的 nxsem_reset(&priv->rx_sem, 0) 收掉，不会串到下一帧。 */

  (void)wd_cancel(&priv->rx_wait_wdog);

  return (ret == OK && priv->rx_wait_to) ? -ETIMEDOUT : ret;
}

/****************************************************************************
 * Name: sf32lb52_audio_read
 *
 * Description:
 *   直通读取，一次成型（形状与理由见文件顶部"录音 RX 通路"那段）：
 *     receive（调用者的 buffer, 本次长度）→ 在 rx_sem 上等一次 → 收干净通道。
 *
 *   返回值的三种语义（上层据此分岔，不能混）：
 *     buflen       = 这一次采满了；
 *     0            = 会话结束（被 STOP 打断 / 设备没在跑 / 会话换代）—— EOF；
 *     -ETIMEDOUT   = 会话还活着，只是这一次没等到数据（上层跳过这一帧接着读）。
 ****************************************************************************/

static ssize_t sf32lb52_audio_read(FAR struct audio_lowerhalf_s *dev,
                                   FAR char *buffer, size_t buflen)
{
  FAR struct sf32lb52_audio_s *priv =
    (FAR struct sf32lb52_audio_s *)dev;

  FAR DMA_HandleTypeDef *hdma_rx;
  HAL_StatusTypeDef res;
  uint32_t gen;
  uint32_t irq_before;
  uint32_t half_before;
  uint32_t cndtr_at_stop;
  uint32_t ccr_at_stop;
  uint32_t isr_at_stop;
  uint32_t dma_err_code;
  uint32_t aprc_state_at_stop;
  uint32_t dma_state_at_stop;
  uint32_t aprc_cfg_at_stop;
  uint32_t aprc_rxcfg_at_stop;
  uint32_t aprc_irq_at_stop;
  uint32_t rcc_enr2_at_stop;
  uint32_t hxt_cr1_at_stop;
  uint32_t pll_stat_at_stop;
  uint32_t dma_csel_at_stop;
  uint32_t codec_adc_cfg_at_stop;
  uint32_t codec_apb_stat_at_stop;
  uint32_t codec_irq_at_stop;
  uint32_t codec_adc_ch0_at_stop;
  uint32_t irq_delta;
  uint32_t half_delta;
  bool     rx_err;
  bool     quiet;
  clock_t  now;
  int      ret;

  priv->rx_read_count++;

  /* ⚠️ 诊断期临时：上一级恢复的**效果检查**。采样点必须在本次 read 动手之前
   * （abort 会把 CNDTR / 句柄状态刷成刚写进去的值、CNDTR 也不再有意义），所以
   * 放在函数最前面 —— "下一次 read 之前"就是这一刻。
   * 内容就是判"那一级生效了没有"的那几个量：irq/half 有没有重新开始涨（下面这两个
   * 增量，涨了就是那条请求握手被叫活了 —— 连丢弃缓冲上的搬运也算，因为计数是无
   * 条件的），RX_CH0_CFG 里的 FIFO_CNT 有没有被搬空、CNDTR 动过没有。
   * 定案后随分级编排一起删。 */

  if (priv->rx_fix_level != 0u)
    {
      FAR DMA_HandleTypeDef *hfix =
        priv->aprc.hdma[SF32LB52_AUDIO_PRC_RX_CH];

      syslog(LOG_WARNING,
             "AUDIO: L%u 效果检查（应用后 irq+%u half+%u）"
             " RX_CH0_CFG=0x%x IRQ=0x%x CNDTR=%u CCR=0x%x hdma.State=0x%x\n",
             (unsigned)priv->rx_fix_level,
             (unsigned)(priv->rx_irq_count - priv->rx_fix_irq),
             (unsigned)(priv->rx_half_irq_count - priv->rx_fix_half),
             (unsigned)priv->aprc.Instance->RX_CH0_CFG,
             (unsigned)priv->aprc.Instance->IRQ,
             (unsigned)((hfix != NULL && hfix->Instance != NULL) ?
                        (uint32_t)hfix->Instance->CNDTR : 0u),
             (unsigned)((hfix != NULL && hfix->Instance != NULL) ?
                        (uint32_t)hfix->Instance->CCR : 0u),
             (unsigned)(hfix != NULL ? (uint32_t)hfix->State : 0u));

      priv->rx_fix_level = 0u;
    }

  if (buffer == NULL || buflen == 0 || !priv->running)
    {
      syslog(LOG_WARNING, "AUDIO: read 时设备没在跑（len=%zu running=%d）\n",
             buflen, (int)priv->running);
      return 0;
    }

  /* 队列模式（enqueuebuffer 真起过 RX DMA）时 RX 通道归那条路，本函数不插手 ——
   * 绝不 abort / 重挂别人正在跑的传输。本板两个 app 走的都是 read/write 直通。 */

  if (priv->rx_apb != NULL)
    {
      syslog(LOG_WARNING, "AUDIO: read 撞上队列模式（rx_apb 非空），本条不取数\n");
      return 0;
    }

  priv->user_tid = nxsched_gettid();

  /* 清掉上一次 stop 可能留下的计数：不清的话本次 read 一进等待就立刻返回，
   * 把一块没采满的缓冲当数据交上去。清在 rx_busy 置真之前 —— ISR 的 post
   * 只可能发生在 rx_busy 为真之后，这一行不会误伤在途的 post。 */

  nxsem_reset(&priv->rx_sem, 0);

  priv->rx_busy    = true;
  priv->rx_aborted = false;
  priv->rx_dma_err = false;

  /* 会话代号在 reset 之后取：接下来任何一个 hw_stop()/hw_shutdown()/新会话的
   * hw_start() 都会让它变号，只要变了就说明这次等待已经没有意义。 */

  gen = priv->session_gen;

  hdma_rx = priv->aprc.hdma[SF32LB52_AUDIO_PRC_RX_CH];

  /* 武装前把 DMA 句柄的状态摆正。HAL_DMA_Start_IT() 只在
   * hdma->State == HAL_DMA_STATE_READY 时才真发传输，否则直接返回 HAL_BUSY
   * 什么都不做（bf0_hal_dma.c:829/883），而 HAL_AUDPRC_Receive_DMA() 把它的
   * 返回值**丢掉**、自己照样 return HAL_OK（bf0_hal_audprc.c:818/828）——
   * 不摆正就会"以为起来了、其实没起"，一路干等 5 秒。
   *
   * abort 只在句柄**确实还武装着**的时候做（判据 hdma->State，见
   * sf32lb52_audio_rx_dma_stop_if_armed）：对一个已经收好尾的句柄再 abort 一次，
   * 写的是那个物理通道的寄存器，而它可能已经被别的 DMA 用户重新分配走了 ——
   * 重复 abort 等于把别人的通道静默关掉。
   * 下面那句总是调用（它内部只在句柄确实还武装着时才 abort，收完摆回 READY）；
   * 这里只在"真的有残留"时补一行告警 —— 正常路径上一次都不会出现：上一次 read
   * 的收尾已经把它置成 READY，会话边界的 hw_stop/hw_shutdown 也各收过一次。 */

  if (hdma_rx != NULL && hdma_rx->State != HAL_DMA_STATE_READY)
    {
      syslog(LOG_WARNING,
             "AUDIO: RX 的 DMA 句柄状态残留(0x%x)，先 abort 再强制复位成 READY\n",
             (unsigned)hdma_rx->State);
    }

  sf32lb52_audio_rx_dma_stop_if_armed(priv);

  /* 武装之前先把中断计数记下来：失败时要靠它回答"这 5 秒里完成中断到底来过
   * 没有"（差值 0 = 这条通路一个字节都没动）。 */

  irq_before  = priv->rx_irq_count;
  half_before = priv->rx_half_irq_count;

  /* ★ 每一次 read 都自己武装到**调用者的 buffer** 上，长度就是上层这次要读的
   *   长度 —— 与厂商 bf0_audio_start 那次 Receive_DMA 同一件事，只是目的地不能
   *   是驱动自有 buffer（见文件顶部"录音 RX 通路"的说明）。整体使能在 DMA 启动
   *   之后，顺序与厂商一致。 */

  res = HAL_AUDPRC_Receive_DMA(&priv->aprc, (FAR uint8_t *)buffer, buflen,
                               SF32LB52_AUDIO_PRC_RX_CH);

  __HAL_AUDPRC_ENABLE(&priv->aprc);

  if (res != HAL_OK)
    {
      priv->rx_busy = false;
      priv->rx_dma_busy_fail++;

      syslog(LOG_ERR,
             "AUDIO: read 起 DMA 失败 res=%d aprc.State[RX]=0x%x "
             "hdma[RX].State=0x%x running=%d\n",
             (int)res,
             (unsigned)priv->aprc.State[SF32LB52_AUDIO_PRC_RX_CH],
             hdma_rx != NULL ? (unsigned)hdma_rx->State : 0u,
             (int)priv->running);
      return 0;
    }

  /* 返回 HAL_OK 也不能就信（上面那段）：再验一次"到底起没起"。没进 BUSY 就
   * 说明 Start_IT 走了 else 分支直接返回 HAL_BUSY、DMA 一个字节都不会传，
   * 这种情况不能接着等 5 秒。顺手把 aprc 的 RX 通道状态掰回 READY ——
   * Receive_DMA 在 Start_IT **之前**就把 State 置成了 BUSY_RX
   * （bf0_hal_audprc.c:785），不回退的话本次会话后面每一次 read 都会被开头
   * 那句 BUSY_RX 判断直接挡回去（:774）。 */

  if (hdma_rx == NULL || hdma_rx->State != HAL_DMA_STATE_BUSY)
    {
      priv->rx_busy = false;
      priv->rx_dma_not_armed++;
      priv->aprc.State[SF32LB52_AUDIO_PRC_RX_CH] = HAL_AUDPRC_STATE_READY;

      syslog(LOG_ERR,
             "AUDIO: read 的 DMA 其实没起来（hdma[RX].State=0x%x "
             "aprc.State[RX]=0x%x running=%d not_armed=%u），不等了直接返回 0\n",
             hdma_rx != NULL ? (unsigned)hdma_rx->State : 0u,
             (unsigned)priv->aprc.State[SF32LB52_AUDIO_PRC_RX_CH],
             (int)priv->running, (unsigned)priv->rx_dma_not_armed);
      return 0;
    }

  /* 到这里这一帧才真的武装上了（句柄已进 BUSY）—— 这才是可以数的"一个 arm"。
   * 会话闸也在这一刻打开：把本次会话代号写进 rx_arm_gen，ISR 只有在
   * rx_arm_gen == session_gen 时才认这次完成中断（等价厂商的 g_rx_stop）。
   * 写 Gen 必须在武装成功之后：写早了，一条刚被作废的传输也能穿过闸门。
   * 顺手记下这一帧的武装时刻（rx_arm_tick）：本会话第一条成功 read 的那一行
   * 健康指纹里 armed_ms 就是"从这一刻起多久数据到了"。 */

  priv->rx_arm_seq++;
  priv->rx_arm_session++;
  priv->rx_arm_gen  = gen;
  priv->rx_arm_tick = clock_systime_ticks();
  sf32lb52_audio_ev(priv, SF32LB52_AUDIO_EV_RX_ARM);

  /* ★ 一次等满。上层的 audio_record_stop() 是"先发 AUDIOIOC_STOP 再 join"，而
   *   hw_stop() 会**无条件** post 一次 rx_sem 并作废会话代号，所以那次等待是被
   *   立刻唤醒的，不靠这 5 秒熬满；5 秒只是"谁都没来"时的上界，也是 ai_audio.c
   *   那条"read 有确定上界"的说法能成立的全部依据。
   *
   *   ⚠️ 这个上界由**本驱动私有的看门狗**产生（sf32lb52_audio_rx_wait_slice），
   *   不再用内核的 nxsem_clockwait_uninterruptible。2026-09-19 真机定案：
   *   内核那记定时等待的超时与 ISR 的 post 抢同一份 TCB 字段（本板临界区是
   *   BASEPRI 型、DMA 通道中断优先级 0，挡不住），真机上抓到的断言正是
   *   `sem_waitirq.c:137 ← nxsem_wait_irq ← nxsem_timeout ← wd_timer ←
   *   timer_callback ← systick_interrupt`；而不崩的那一种结局更贵 ——
   *   超时被抢掉之后这一次等待**再也没有上界**：现场 diag 里 wait 涨到 11 万
   *   毫秒、rxr 一动不动、rxt 恒 0（所以按超时触发的分级恢复一次都没跑），
   *   录音标志却全健康 —— 用户看到的"录音跑一阵就永久停摆"就是它。
   *   私有心跳的写法与理由见 struct 里 rx_wait_wdog 那一段和 write 那条路。 */

  ret = sf32lb52_audio_rx_wait_slice(priv, SF32LB52_AUDIO_RX_READ_TIMEOUT_MS);

  /* 风险 A：**只动属于自己这一代的账**。
   * 一个上一代的残影在这里把 rx_busy 清成 false，会让新一代那条 read 从此收不到
   * ISR 的 post（ISR 只在 rx_busy 为真时才 post），只能等它自己那 5 秒超时 ——
   * 整条录音慢一拍。代号对不上就一律不碰：新一代那条 read 自己的收尾会把这笔账
   * 收干净。 */

  if (gen == priv->session_gen)
    {
      priv->rx_busy = false;
    }

  /* 被 stop() / close / 新会话打断（或设备已经不在跑了）：按 EOF 返回 0。
   * 三个判据合起来就是"这一代会话真的结束了"，与"这一次读没等到数据"
   * （-ETIMEDOUT）必须分开 —— 上层把 0 当会话结束、把 -ETIMEDOUT 当"跳过这一帧"。
   *
   * 收尾顺序与厂商 bf0_audio_stop 一致：先把这一次传输停掉，再让 read 出去。
   * 但**只有这一代还是自己的时候**才动手：代号变了说明通道和那套账已经归新一代
   * （hw_stop/hw_shutdown 收过尾、新一代的 read 可能已经武装上了），这时再去
   * abort / 清闸门就是拆新会话的台 —— 尤其 rx_arm_gen，清成 0 等于把新一代那道
   * 会话闸关上，它那条 read 会一直等不到唤醒。残影只需要安静地退出去。 */

  if (priv->rx_aborted || !priv->running || gen != priv->session_gen)
    {
      if (gen == priv->session_gen)
        {
          priv->rx_aborted = false;
          priv->rx_arm_gen = 0;   /* 会话闸关上：残影的完成中断别再叫醒别人 */
          sf32lb52_audio_rx_dma_stop_if_armed(priv);
        }

      return 0;
    }

  /* 现场快照必须在**停 DMA 之前**取：abort 会把 hdma->State 置 READY、
   * 标清该通道的标志、CNDTR 也失去意义，之后再采样读到的只是自己刚写进去的
   * 值（这一条以前踩过，日志因此毫无价值）。 */

  cndtr_at_stop      = (hdma_rx != NULL && hdma_rx->Instance != NULL) ?
                       (uint32_t)hdma_rx->Instance->CNDTR : 0u;
  ccr_at_stop        = (hdma_rx != NULL && hdma_rx->Instance != NULL) ?
                       (uint32_t)hdma_rx->Instance->CCR : 0u;
  isr_at_stop        = (hdma_rx != NULL && hdma_rx->DmaBaseAddress != NULL) ?
                       (uint32_t)hdma_rx->DmaBaseAddress->ISR : 0u;
  dma_err_code       = (hdma_rx != NULL) ? (uint32_t)hdma_rx->ErrorCode : 0u;
  aprc_state_at_stop = (uint32_t)priv->aprc.State[SF32LB52_AUDIO_PRC_RX_CH];
  dma_state_at_stop  = (hdma_rx != NULL) ? (uint32_t)hdma_rx->State : 0u;
  aprc_cfg_at_stop   = (uint32_t)priv->aprc.Instance->CFG;

  /* AUDPRC 自己的 RX/ADC 侧、模块时钟门、PMU 音频 buffer、音频 PLL、DMA 请求
   * 映射 —— "一个 DMA 请求都没拉"时，这六个值合起来就能回答"是数字块没时钟、
   * 是 RX 通道被掩掉、还是请求线压根没映射到 AUDPRC"。和上面那批一样，
   * 必须在 abort 之前采。 */

  aprc_rxcfg_at_stop = (uint32_t)priv->aprc.Instance->RX_CH0_CFG;
  aprc_irq_at_stop   = (uint32_t)priv->aprc.Instance->IRQ;
  rcc_enr2_at_stop   = (uint32_t)hwp_hpsys_rcc->ENR2;
  hxt_cr1_at_stop    = (uint32_t)hwp_pmuc->HXT_CR1;
  pll_stat_at_stop   = (uint32_t)priv->codec.Instance->PLL_STAT;
  dma_csel_at_stop   = sf32lb52_audio_rx_dma_req(hdma_rx);

  /* codec（AUDCODEC）侧 —— 这一组是"到底谁不喂谁"的唯一判据，必须和上面同一
   * 采样点（abort 之前）：
   *   ADC_CFG   bit4:3 OP_MODE：0=normal（数据走 rx interface 给 AUDPRC），
   *             1=apb mode（数据走 APB 口，AUDPRC 那边就吃不到了）。
   *             SVD 原文见 SF32LB52x.svd 的 AUDCODEC.ADC_CFG.OP_MODE。
   *   APB_STAT  bit19:16 ADC_CH0_FIFO_CNT：APB 口在积多少（opmode=1 时这里会涨）。
   *   IRQ       bit16 ADC_CH0_APB_OF(溢出) / bit17 APB_UF(欠载)：APB 口在溢就说明
   *             ADC 真在产数据、只是没人从 APB 口取。
   *   ADC_CH0_CFG bit7 DMA_EN：codec 自己的 ADC DMA 口（我们不用，走 AUDPRC RX0）。
   * 判读：坏的时候若 APB 口在溢 + ADC_CFG 的 OP_MODE=1，就是"数据被送到没人读的
   * 那个口"；若 APB 口干净且 FIFO_CNT=0，则说明 ADC 侧压根没动，锅不在请求线。 */

  codec_adc_cfg_at_stop    = (uint32_t)priv->codec.Instance->ADC_CFG;
  codec_apb_stat_at_stop   = (uint32_t)priv->codec.Instance->APB_STAT;
  codec_irq_at_stop        = (uint32_t)priv->codec.Instance->IRQ;
  codec_adc_ch0_at_stop    = (uint32_t)priv->codec.Instance->ADC_CH0_CFG;

  rx_err = priv->rx_dma_err;
  priv->rx_dma_err = false;

  /* 收尾：只 abort DMA 通道（不碰 ADC 数据通路，理由见文件顶部那段说明）。
   * 之后两个 State 都是 READY —— 这是"下一次 read 一定能在干净通道上武装"的
   * 全部保证。会话闸同时关上：这一次读已经结束，别再让它的完成中断去叫醒
   * 下一次 read（竞态窗口就是"TC 已经 post、而这次 read 还没返回"那一小段）。 */

  priv->rx_arm_gen = 0;
  sf32lb52_audio_rx_dma_stop_if_armed(priv);

  /* TE 必须和超时走同一条出口：错误回调 post 过一次信号量把等待叫醒，于是
   * ret 是 OK —— 只看 ret 的话会把一块**没被写过的**缓冲当"采满了"交上去。 */

  if (ret < 0 || rx_err)
    {
      irq_delta  = priv->rx_irq_count - irq_before;
      half_delta = priv->rx_half_irq_count - half_before;
      now        = clock_systime_ticks();

      if (rx_err)
        {
          quiet = (int32_t)(now - priv->rx_dma_err_log_next) < 0;

          if (quiet)
            {
              priv->rx_dma_err_silenced++;
            }
          else
            {
              priv->rx_dma_err_log_next =
                now + MSEC2TICK(SF32LB52_AUDIO_TE_LOG_MS);
            }
        }
      else
        {
          quiet = false;
          priv->rx_timeout_count++;
        }

      if (!quiet)
        {
          /* "卡死之前最近发生过什么"：每种关键事件打一次"离现在多久 + 累计次数"
           * （`hist=[arm-40msx15000 play_end-6100msx3 sess_begin-183000msx1]`）。
           * 判读：卡死前最后一次播放结束 / 停会话 / abort 离现在多久、以及它们发生过
           * 几次 —— 这一栏就是"谁把 DMAC1 卡住"的直接线索。拼不下就截断（诊断量，
           * 不影响别的字段）。 */

          char hist[256];
          int  hn = 0;
          uint32_t hi;

          hn = snprintf(hist, sizeof(hist), "[");

          for (hi = 1u; hi < SF32LB52_AUDIO_EV_MAX; hi++)
            {
              if (priv->ev_last[hi] == 0)
                {
                  continue;
                }

              hn += snprintf(hist + hn, sizeof(hist) - (size_t)hn, "%s%s-%umsx%u",
                             hn <= 1 ? "" : " ",
                             g_ev_name[hi],
                             (unsigned)TICK2MSEC(now - priv->ev_last[hi]),
                             (unsigned)priv->ev_cnt[hi]);

              if (hn >= (int)sizeof(hist) - 24)
                {
                  break;
                }
            }

          snprintf(hist + hn, sizeof(hist) - (size_t)hn, "]");

          syslog(LOG_WARNING,
                 "AUDIO: read 等 DMA 失败（ret=%d err_te=%d len=%zu）"
                 " irq=%u half=%u read=%u timeout=%u busy_fail=%u not_armed=%u"
                 " dma_err=%u dma_err_silenced=%u recov=%u"
                 " irq_delta=%u half_delta=%u"
                 " CNDTR=%u CCR=0x%x ISR=0x%x dma_err_code=0x%x CFG=0x%x"
                 " RX_CH0_CFG=0x%x aprc.IRQ=0x%x ENR2=0x%x HXT_CR1=0x%x"
                 " PLL_STAT=0x%x CSELR=%u"
                 " codec.ADC_CFG=0x%x codec.APB_STAT=0x%x codec.IRQ=0x%x"
                 " codec.ADC_CH0_CFG=0x%x"
                 " aprc.State[RX]=0x%x hdma[RX].State=0x%x"
                 " t=%u running=%d playback=%d arm_sess=%u arm_boot=%u"
                 " hist=%s\n",
                 ret, (int)rx_err, buflen,
                 (unsigned)priv->rx_irq_count,
                 (unsigned)priv->rx_half_irq_count,
                 (unsigned)priv->rx_read_count,
                 (unsigned)priv->rx_timeout_count,
                 (unsigned)priv->rx_dma_busy_fail,
                 (unsigned)priv->rx_dma_not_armed,
                 (unsigned)priv->rx_dma_err_count,
                 (unsigned)priv->rx_dma_err_silenced,
                 (unsigned)priv->rx_recover_count,
                 (unsigned)irq_delta,
                 (unsigned)half_delta,
                 (unsigned)cndtr_at_stop,
                 (unsigned)ccr_at_stop,
                 (unsigned)isr_at_stop,
                 (unsigned)dma_err_code,
                 (unsigned)aprc_cfg_at_stop,
                 (unsigned)aprc_rxcfg_at_stop,
                 (unsigned)aprc_irq_at_stop,
                 (unsigned)rcc_enr2_at_stop,
                 (unsigned)hxt_cr1_at_stop,
                 (unsigned)pll_stat_at_stop,
                 (unsigned)dma_csel_at_stop,
                 (unsigned)codec_adc_cfg_at_stop,
                 (unsigned)codec_apb_stat_at_stop,
                 (unsigned)codec_irq_at_stop,
                 (unsigned)codec_adc_ch0_at_stop,
                 (unsigned)aprc_state_at_stop,
                 (unsigned)dma_state_at_stop,
                 (unsigned)TICK2MSEC(now),
                 (int)priv->running, (int)priv->playback,
                 (unsigned)priv->rx_arm_session,
                 (unsigned)priv->rx_arm_seq,
                 hist);

          if (rx_err)
            {
              priv->rx_dma_err_silenced = 0;
            }
        }

      /* 跨代守卫：上面那条 syslog 是几千个 tick，别的线程完全可能在这段里把会话
       * 换代（stop / open）。会话已经不是自己这一代了就**什么都不做**：通道与
       * 那套账（含 rx_arm_gen）都归新一代，这时再 abort / 计数 / 清闸门就是拆
       * 新会话的台。返回 0（EOF）而不是 -ETIMEDOUT：会话换代本来就是 EOF 的三种
       * 死因之一，上层拿到 0 会走正常收尾。 */

      if (gen != priv->session_gen)
        {
          return 0;
        }

      /* 恢复：先只把通道收干净（下一次 read 自己重新武装），零活动时在同一条路上
       * 接着按 recov 计数轮转做一级分级动作。判据就是下面这两个观察量，不看别的
       * 判据。判读：irq_delta==0 且 half_delta==0 且 CNDTR 停在满值 → 这一次等待里
       * DMA 一个字节都没搬，请求线从没被拉高；irq 在涨而这次仍失败 → 中断是来的，
       * 问题在等待/唤醒这一侧；err_te=1 → 这一次是 DMA 传输错误(TE)结束的。
       *
       * 返回值只管一件事：这一次读按 **-ETIMEDOUT** 返回 —— 会话还活着，只是这一帧
       * 没数据，上层跳过这一帧接着读（恢复动作已经在 recover 里做完了；早先那套
       * "会话作废、按 EOF 让上层重建"的做法随 L4 一起删了：实测它救不活，而复位
       * DMAC1 能，见文件顶部）。 */

      (void)sf32lb52_audio_rx_recover(priv, irq_delta, half_delta);
      return -ETIMEDOUT;
    }

  /* ★ 健康指纹：本会话**第一条成功 read** 才打一行，采样点与字段和失败行逐项
   *   对齐（同一批值：都在停 DMA 之前取的，见上面"现场快照必须在停 DMA 之前取"
   *   那一段），这样"好的时候长什么样"和"坏的时候长什么样"能直接逐位对比 ——
   *   定案"DMAC 为什么收不到请求"就差这一份对照。
   *   armed_ms = 从武装成功（rx_arm_tick）到这一刻的毫秒数；失败行没有这个量，
   *   那边 CNDTR 停在满值、一个请求都没收到，所谓"武装之后多久"是无穷大。
   *   诊断期临时件，定案后随分级编排一起删。 */

  if (!priv->rx_health_done)
    {
      priv->rx_health_done = true;
      now = clock_systime_ticks();

      syslog(LOG_INFO,
             "AUDIO: RX 健康指纹（本会话第一条成功 read）"
             " irq=%u half=%u read=%u recov=%u"
             " CNDTR=%u CCR=0x%x ISR=0x%x dma_err_code=0x%x CFG=0x%x"
             " RX_CH0_CFG=0x%x aprc.IRQ=0x%x ENR2=0x%x HXT_CR1=0x%x"
             " PLL_STAT=0x%x CSELR=%u"
             " codec.ADC_CFG=0x%x codec.APB_STAT=0x%x codec.IRQ=0x%x"
             " codec.ADC_CH0_CFG=0x%x"
             " aprc.State[RX]=0x%x hdma[RX].State=0x%x"
             " armed_ms=%u t=%u running=%d playback=%d arm_sess=%u arm_boot=%u"
             " len=%zu\n",
             (unsigned)priv->rx_irq_count,
             (unsigned)priv->rx_half_irq_count,
             (unsigned)priv->rx_read_count,
             (unsigned)priv->rx_recover_count,
             (unsigned)cndtr_at_stop,
             (unsigned)ccr_at_stop,
             (unsigned)isr_at_stop,
             (unsigned)dma_err_code,
             (unsigned)aprc_cfg_at_stop,
             (unsigned)aprc_rxcfg_at_stop,
             (unsigned)aprc_irq_at_stop,
             (unsigned)rcc_enr2_at_stop,
             (unsigned)hxt_cr1_at_stop,
             (unsigned)pll_stat_at_stop,
             (unsigned)dma_csel_at_stop,
             (unsigned)codec_adc_cfg_at_stop,
             (unsigned)codec_apb_stat_at_stop,
             (unsigned)codec_irq_at_stop,
             (unsigned)codec_adc_ch0_at_stop,
             (unsigned)aprc_state_at_stop,
             (unsigned)dma_state_at_stop,
             (unsigned)TICK2MSEC(now - priv->rx_arm_tick),
             (unsigned)TICK2MSEC(now),
             (int)priv->running, (int)priv->playback,
             (unsigned)priv->rx_arm_session,
             (unsigned)priv->rx_arm_seq,
             buflen);
    }

  /* 这一次真的采满了。 */

  return buflen;

}

/****************************************************************************
 * Name: sf32lb52_audio_reserve / sf32lb52_audio_release
 *
 ****************************************************************************/

static int sf32lb52_audio_reserve(FAR struct audio_lowerhalf_s *dev)
{
  return OK;
}

static int sf32lb52_audio_release(FAR struct audio_lowerhalf_s *dev)
{
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sf32lb52_audio_initialize
 *
 ****************************************************************************/

int sf32lb52_audio_initialize(void)
{
  FAR struct sf32lb52_audio_s *priv;
  int ret;

  syslog(LOG_ERR, "AUDIO: initialize start\n");
  priv = kmm_zalloc(sizeof(struct sf32lb52_audio_s));
  if (priv == NULL)
    {
      return -ENOMEM;
    }

  ret = sf32lb52_audio_hw_init(priv);
  syslog(LOG_ERR, "AUDIO: hw_init ret=%d\n", ret);
  if (ret < 0)
    {
      kmm_free(priv);
      return ret;
    }

  nxsem_init(&priv->wr_sem, 0, 0);
  nxsem_init(&priv->rx_sem, 0, 0);

  /* write() 那次等待的心跳。kmm_zalloc 出来的 func 已经是 NULL（= 不活跃），
   * 这里补一次 wd_init 只是把"这记看门狗属于本驱动"这件事写在明面上。 */

  wd_init(&priv->wr_wait_wdog);

  /* 开机打一行"这台固件用的是哪条超时路径"：上板判读就靠它认版本
   * （旧镜像这里一行都没有）。 */

  syslog(LOG_INFO,
         "AUDIO: 录音 read 一次成型（%dms 上界，内核定时等待兜）；"
         "write 的等待超时来自本驱动心跳，内核定时等待不参与\n",
         SF32LB52_AUDIO_RX_READ_TIMEOUT_MS);

  /* 持有者身份显式给 -1：kmm_zalloc 出来是全 0，而 pid 0 是 idle 任务 ——
   * "没记过"和"真的记了一个 pid"必须分得开（见 struct 里那段说明）。 */

  priv->holder_tid = (pid_t)-1;
  priv->holder_pid = (pid_t)-1;
  priv->user_tid   = (pid_t)-1;

  priv->dev.ops = &g_sf32lb52_audio_ops;

  ret = audio_register("audio0", &priv->dev);
  syslog(LOG_ERR, "AUDIO: register ret=%d\n", ret);
  if (ret < 0)
    {
      auderr("ERROR: audio_register failed: %d\n", ret);
      kmm_free(priv);
      return ret;
    }

  /* 到这一步 priv 一定是活的（下面每一条失败路径都已经 kmm_free 掉了），
   * 才把它记进那个模块级的只读入口。放在失败路径之后是必须的：
   * sf32lb52_audio_rx_stats() 只看这个指针是不是 NULL，早一步记下去就等于
   * 教它去读一块已经还给堆的内存。 */

  g_sf32lb52_audio_priv = priv;

  audinfo("/dev/audio0 registered\n");
  return OK;
}

/****************************************************************************
 * Name: sf32lb52_audio_rx_stats
 *
 * Description:
 *   把录音（RX）通路的分诊计数**只读地**拷给调用方（原型和字段口径在
 *   board/contest_board/src/sf32lb52_audio_in.h，那里也有"为什么要它"那段）。
 *
 *   纪律（这个函数存在的全部意义就是守住它们）：
 *     1) **不阻塞**：不加锁、不等信号量、不调任何会拿驱动锁的东西。调用方里
 *        有 robot_ui 的 MQTT 收包线程，它一阻塞，唯一的观测通道就一起没了；
 *     2) **不碰设备**：不 open/close、不 ioctl、不读写硬件寄存器，只读上面那
 *        六个 32 位计数器和两个 bool；
 *     3) **不改变行为**：不 post 信号量（那会把一个正卡在 rx_sem 上的 read
 *        提前叫醒，等于用观测去动等待逻辑）、不清零计数器（口径是"开机以来"）、
 *        不做任何恢复动作。要恢复是录音链路自己的事。
 *
 *   为什么可以不加锁就读：这八个量全是 32 位对齐的 int/bool/uint32，Cortex-M
 *   上的单次读写本身就是原子的，读到的最坏情况是"差一两个中断之前的值" ——
 *   诊断要的正是"现在大概什么形状"，为此加锁反而把调用方挂到驱动的锁上。
 *   也正因为如此，这里**不能**读那些需要配对读的量。
 *
 *   注：armed / lost 这两个字段不是计数，是"此刻"的状态：
 *     armed = RX 通道此刻是否武装着（read 武装成功到收尾之间为真），
 *     lost  恒 0（本驱动没有"环里来不及取、被 DMA 盖掉"这回事）。
 *
 ****************************************************************************/

int sf32lb52_audio_rx_stats(FAR struct sf32lb52_audio_rx_stats_s *stats)
{
  FAR const struct sf32lb52_audio_s *priv = g_sf32lb52_audio_priv;

  if (stats == NULL)
    {
      return -EINVAL;
    }

  if (priv == NULL)
    {
      /* 音频还没初始化（/dev/audio0 还没注册）。**一个字段都不写**：宁可让
       * 调用方看见"取不到"，也不要给它一份全是 0 的假快照 —— 0 在这里是
       * "计数器真的是 0"，和"没读到"必须分得开。 */

      return -ENODEV;
    }

  stats->irq     = priv->rx_irq_count;
  stats->half    = priv->rx_half_irq_count;
  stats->read    = priv->rx_read_count;
  stats->timeout = priv->rx_timeout_count;
  stats->dma_err = priv->rx_dma_err_count;

  /* armed 直接看 RX 通道此刻是不是武装着 —— read 武装成功到收尾之间为真，
   * 正常就是"有人正在读"；lost 在一次成型的 read 上不存在（恒 0，见头文件里
   * 那一列说明）。 */

  stats->lost    = 0;
  stats->armed   = (priv->aprc.hdma[SF32LB52_AUDIO_PRC_RX_CH] != NULL &&
                    priv->aprc.hdma[SF32LB52_AUDIO_PRC_RX_CH]->State ==
                      HAL_DMA_STATE_BUSY) ? 1 : 0;

  stats->busy    = priv->rx_busy ? 1 : 0;

  return OK;
}

/****************************************************************************
 * Name: sf32lb52_audio_rx_fix
 *
 * Description:
 *   ⚠️ **诊断期临时**的手动入口（nsh: `audio_test audfix`）：现场不想等自动恢复时，
 *   立刻打一次**唯一的恢复动作** —— 复位整块 DMAC1 + 重建三个通道句柄（定案与依据
 *   见文件顶部那一段）。参数只为兼容老用法保留：不给参数、给 1、给 "all" 都是打
 *   这一记；给别的值返回 -EINVAL。定案后这个命令连同恢复动作的注释一起删。
 *
 *   ⚠️ 它做的是恢复动作，不是观测：会 abort 掉此刻可能正被某条 read 用着的 RX
 *   通道（那条 read 会因此按超时收尾、丢掉这一帧），也会和自动恢复共用同一笔
 *   "待验证"的账（谁后写谁生效）。诊断用，别在正常使用的会话上随手敲。
 *
 *   返回：1 = 打了那一记；-ENODEV = 音频驱动还没初始化；-EINVAL = 参数不认识。
 ****************************************************************************/

int sf32lb52_audio_rx_fix(int level)
{
  FAR struct sf32lb52_audio_s *priv = g_sf32lb52_audio_priv;
  uint32_t armed;
  uint32_t irq0;
  uint32_t half0;

  if (priv == NULL)
    {
      return -ENODEV;
    }

  if (level > SF32LB52_AUDIO_RX_FIX_DMAC1)
    {
      return -EINVAL;
    }

  sf32lb52_audio_rx_dma_stop_if_armed(priv);

  irq0  = priv->rx_irq_count;
  half0 = priv->rx_half_irq_count;
  armed = sf32lb52_audio_rx_fix_apply(priv, SF32LB52_AUDIO_RX_FIX_DMAC1);

  up_mdelay(SF32LB52_AUDIO_RX_FIX_SETTLE_MS);

  sf32lb52_audio_rx_fix_report(priv, SF32LB52_AUDIO_RX_FIX_DMAC1, 0u, armed,
                               SF32LB52_AUDIO_RX_FIX_SETTLE_MS, irq0, half0);

  priv->rx_fix_level = SF32LB52_AUDIO_RX_FIX_DMAC1;
  priv->rx_fix_irq   = priv->rx_irq_count;
  priv->rx_fix_half  = priv->rx_half_irq_count;

  return (int)SF32LB52_AUDIO_RX_FIX_DMAC1;
}

/****************************************************************************
 * HAL 弱符号回调重写（DMA 中断 → 本驱动）
 *
 ****************************************************************************/

void HAL_AUDPRC_TxCpltCallback(AUDPRC_HandleTypeDef *haprc, int cid)
{
  FAR struct sf32lb52_audio_s *priv;

  if (haprc == NULL)
    {
      return;
    }

  priv = (FAR struct sf32lb52_audio_s *)
         ((FAR char *)haprc - offsetof(struct sf32lb52_audio_s, aprc));

  if (priv->wr_busy)
    {
      priv->wr_busy = false;
      nxsem_post(&priv->wr_sem);
    }
  else
    {
      sf32lb52_audio_tx_complete(priv);
    }
}

void HAL_AUDCODEC_TxCpltCallback(AUDCODEC_HandleTypeDef *hacodec, int cid)
{
  FAR struct sf32lb52_audio_s *priv;

  if (hacodec == NULL)
    {
      return;
    }

  priv = (FAR struct sf32lb52_audio_s *)
         ((FAR char *)hacodec - offsetof(struct sf32lb52_audio_s, codec));

  if (priv->wr_busy)
    {
      priv->wr_busy = false;

      /* 传输完成：**先冻结再唤醒 write()**。
       * 顺序反了（或者干脆不冻结）就会在"唤醒 → 线程真的跑到 DMAStop()"
       * 这段窗口里从块首重播，每块一声"啪" —— 详见 tx_freeze 的注释。
       * write() 只用 DAC_CH0，别的通道进来不动。 */

      if (cid == HAL_AUDCODEC_DAC_CH0)
        {
          sf32lb52_audio_tx_freeze(priv);
        }

      nxsem_post(&priv->wr_sem);
    }
}

void HAL_AUDPRC_RxCpltCallback(AUDPRC_HandleTypeDef *haprc, int cid)
{
  FAR struct sf32lb52_audio_s *priv;

  if (haprc == NULL)
    {
      return;
    }

  priv = (FAR struct sf32lb52_audio_s *)
         ((FAR char *)haprc - offsetof(struct sf32lb52_audio_s, aprc));

  /* 无条件计数，**放在 rx_busy 判断之前**：这条只回答一个事实 ——
   * "RX 完成中断到底来过没有"。如果它在 read 超时时还是 0，就说明
   * DMA 根本没收到数据/中断没使能，而不是等待逻辑的问题。 */

  priv->rx_irq_count++;

  /* 一个 TC = 这一次 read 的整块写完了 = 这一帧到手。ISR 只做三件事 ——
   * 过会话闸、计数、叫醒读者（队列模式则交还 buffer）；**绝不碰通道、
   * 绝不做任何恢复**（恢复和收尾全在 read 的 task 上下文里做，中断里只记账）。
   *
   * 会话闸（等价厂商 drv_audprc.c 的 g_rx_stop）：rx_arm_gen 只在 read 武装
   * 成功那一刻被写成本次会话代号，而 stop/close/新会话都会让 session_gen 变号
   * —— 对不上就说明这条完成中断属于一条已经被作废的传输，只留上面那个计数，
   * **不 post**。少了这道门，一次 stop 之后飘进来的 post 会去叫醒下一次 read，
   * 把一块没采满的缓冲当数据交上去。 */

  if (priv->rx_arm_gen != priv->session_gen)
    {
      return;
    }

  if (priv->rx_busy)
    {
      priv->rx_busy = false;
      nxsem_post(&priv->rx_sem);
    }
  else
    {
      /* 没人在等：队列模式（rx_apb 挂着）时把 buffer 交还上层；直通模式下
       * rx_apb 是 NULL，sf32lb52_audio_rx_complete 里第一句就返回。 */
      sf32lb52_audio_rx_complete(priv);
    }
}

/****************************************************************************
 * Name: HAL_AUDPRC_RxHalfCpltCallback
 *
 * Description:
 *   RX 半满中断 —— "数据真的在流"的心跳。
 *
 *   Receive_DMA 本来就把 RX 装成了 DMACIRCULAR 并把半满回调挂在句柄上
 *   （bf0_hal_audprc.c 里 AUDPRC_DMAHalfRxCplt → 这个 __weak 空实现），
 *   只是一直没人接。read 等的是**整块写完的那一次 TC**，一旦 TC 那一路丢了，
 *   日志上就只剩"irq=0"，分不清"数据源头压根没动"和"数据在流、只是完成通知
 *   丢了"。20ms 一帧 → 半满每 10ms 一次，这个计数就是那个分辨器。
 *
 *   这里**只累加一个计数器**（HT 不交数据、也不叫醒读者：半满时缓冲里还只有
 *   一半数据，那时叫醒读者等于把半块没写过的东西交上去）：中断上下文里不能打
 *   串口日志（会抢控制台锁、把整机挂住，本文件踩过一次），也不能做任何会改变
 *   通路状态的事 —— 它每 10ms 就来一次，做重活等于把录音通路拖垮。
 ****************************************************************************/

void HAL_AUDPRC_RxHalfCpltCallback(AUDPRC_HandleTypeDef *haprc, int cid)
{
  FAR struct sf32lb52_audio_s *priv;

  if (haprc == NULL)
    {
      return;
    }

  priv = (FAR struct sf32lb52_audio_s *)
         ((FAR char *)haprc - offsetof(struct sf32lb52_audio_s, aprc));

  /* 无条件自增（和 rx_irq_count 同一个规矩：这条只回答"来过没有"）。 */

  priv->rx_half_irq_count++;

  /* HT 只是一个"数据在流"的心跳，**不交数据、不叫醒任何人**。保留这个计数的
   * 用途：irq 冻结而 half 还在涨 = 数据是通的、只有 TC 那一路丢了；两个都不涨
   * = 数据源头真停了。 */
}

/****************************************************************************
 * Name: HAL_AUDPRC_ErrorCallback
 *
 * Description:
 *   DMA 传输错误（TE）—— 这一类原来**永远不会自愈**，这里补上。
 *
 *   故障形态：TE 中断进来后 HAL 自己就做完了收尾
 *   （bf0_hal_dma.c:1228-1235：关 TC/HT/TE 中断 → DMA_FreeChannel →
 *   State=READY），随后 AUDPRC 侧的 AUDPRC_DMAError（bf0_hal_audprc.c:1062）
 *   把 aprc->State[cid] 也置 READY 再回调到这里。于是现场表现是：
 *       irq_delta == 0（完成中断确实没来）
 *       hdma[RX].State == 0x1（READY，而不是 BUSY）
 *   所以 read 认的是本回调立的那面旗（rx_dma_err），不是句柄状态 —— TE 一次就
 *   收尾并按 -ETIMEDOUT 返回，不会空转满 5 秒。
 *
 *   本回调只做三件**中断安全**的轻活（**不打日志**：串口那一侧会抢控制台锁、
 *   把整机挂住，本文件在播放完成回调里就为此删过一次 printf）：
 *     1) rx_dma_err_count++：TE 次数的唯一凭据；
 *     2) 立 rx_dma_err 旗：read() 一从等待里醒来就看它（那次等满的等待靠这一次
 *        post 提前结束，不等 5 秒熬满）；
 *     3) 唤醒正在等帧的 read（post rx_sem）。
 *   日志与收尾都在 read() 的失败分支里做（那里能拿到 CNDTR/CCR/ISR 的
 *   停机前快照，信息比中断里全），**中断里不做任何恢复、不碰通道**。
 *
 *   只对 RX 通道立旗：TX0 只在队列模式（enqueuebuffer）里才会起来，那条路
 *   没有阻塞在信号量上的读者，立旗没人消费反而会污染下一次 read。
 ****************************************************************************/

void HAL_AUDPRC_ErrorCallback(AUDPRC_HandleTypeDef *haprc, int cid)
{
  FAR struct sf32lb52_audio_s *priv;

  if (haprc == NULL)
    {
      return;
    }

  priv = (FAR struct sf32lb52_audio_s *)
         ((FAR char *)haprc - offsetof(struct sf32lb52_audio_s, aprc));

  /* 计数无条件做：日志是限频的，计数器不是 —— "到底出过几次"只能靠它。
   * 错误码不用在这里记：HAL 在调本回调**之前**已经把它写进
   * hdma[cid]->ErrorCode（bf0_hal_dma.c:1228，值 HAL_DMA_ERROR_TE = 0x1），
   * read() 的收尾日志直接读那个字段；直到下一次武装那一刻才被 HAL 清掉
   * （HAL_DMA_Start_IT 开头置 NONE），所以这一帧读到的就是本帧的值。 */

  priv->rx_dma_err_count++;

  /* 会话闸：等价 TC 回调里那道（见 HAL_AUDPRC_RxCpltCallback）。一条已经被
   * stop/close/换会话作废的传输报出来的 TE 属于上一代，不立旗、不 post，
   * 否则下一代会把它当成"自己这一次出错了"。 */

  if (priv->rx_arm_gen != priv->session_gen)
    {
      return;
    }

  if (cid != SF32LB52_AUDIO_PRC_RX_CH || !priv->rx_busy)
    {
      /* 非 RX 通道，或者当前这一次等待已经收过尾（rx_busy 已清）：只计数。
       * 不立旗是必须的 —— 旗子只有一个消费者（read()），在没人等的时候立下去，
       * 下一次 read 会把它当成"自己这一帧出错了"。 */

      return;
    }

  priv->rx_dma_err = true;
  nxsem_post(&priv->rx_sem);
}
