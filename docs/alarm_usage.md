# 设备级报警模块使用说明（`sf32lb52_alarm`）

> 面向队友：**怎么用**这个模块，以及**哪些坑不能踩**。
> 头文件：`board/contest_board/src/sf32lb52_alarm.h`
> 实现：`board/contest_board/src/sf32lb52_alarm.c`
> 状态：**已在 openvela 工作区编译通过（无 error / warning）**；
> 真机"响不响"由调用方上板验证（本文档不代替上板测试）。

## 1. 这个模块解决什么问题

一次"报警"其实分三段，各由不同的人负责：

| 段 | 谁负责 | 交付物 | 关键接口 |
|----|--------|--------|----------|
| ① 感知 / 决策 | 成员二（AI 状态机） | `app/hello_app/ai_state_machine.h` | `SM_STATE_ALARM`（`:42`）、`SM_EVENT_ALARM_DETECTED`（`:56`） |
| ② **设备级动作（响，持续到解除）** | **本模块** | `board/contest_board/src/sf32lb52_alarm.{h,c}` | `alarm_trigger()` / `alarm_clear()` / `alarm_set_callback()` |
| ③ 上报 / 展示 | 成员三（网络 + 界面） | `app/robot_ui/network_comm.h`、`app/robot_ui/robot_ui.h` | `report_alarm()`（`network_comm.h:77`）、`robot_ui_show_alarm()`（`robot_ui.h:62`） |

为什么要有②这一层：**报警现场必须自己有效**。网络可能断、MQTT 可能掉线、界面可能卡住，
但只要进了 `SM_STATE_ALARM`，板子就得自己把喇叭叫起来，并且**一直叫到被解除**
（或 60 秒自动超时）。这段是"设备行为"，放在板级模块里，比塞进应用逻辑更可靠。

**三段怎么串**：

```
状态机(②)──alarm_trigger()──> sf32lb52_alarm ──回调──> 网络/界面(③)
                                   │
                                   └─ 喇叭响(/dev/audio/audio0)（无指示灯）
```

- 上层（状态机/业务）只管调 `alarm_trigger()`；
- 网络上报和界面显示 **通过回调对接**：`alarm_set_callback()` 注册一个函数，
  在回调里调 `report_alarm()` / `robot_ui_show_alarm()`；
- 本模块**不 include 任何 app 头文件**（板级模块不反向依赖应用层）。

## 2. 硬件与资源

| 项 | 值 | 说明 |
|----|----|------|
| 报警音 | `/dev/audio/audio0` | 标准 NuttX audio 接口，16kHz 单声道 16bit；模块自己 open fd |
| 指示灯 | **本模块不驱动任何指示灯** | 报警只有声音这一种输出。板上唯一能由软件控制的用户 GPIO 输出脚是 **PA26**（`/dev/gpio1`），把它留给板级/应用做状态指示，比占用它闪报警灯更有价值；原理图上标着「RGB LED」的 **PA32** 在这块板子（SF32LB52-DevKit-LCD 实物）上**没有实物**，驱动它不会有任何可见效果。所以**不要再把 LED 加回报警模块**：既没有收益，又会把唯一的状态指示脚长期占给报警 |
| 工作线程 | 名字 `alarm`，栈 **4096**，优先级 **115** | 声音和回调全在这个线程里做。NuttX 里**数值越小优先级越高**，115 低于 `robot_ui`(110)、高于 `lpwork`(120)：报警时不能抢在 `lpwork` 前面，那是 USB RNDIS 收发所在，而报警恰恰最需要把上报发出去 |
| 音频缓冲 | 静态 `int16_t g_alarm_tone[800]`（1600 字节） | **静态**，不放栈上（见坑 6） |

各等级的报警音：

| 级别 | 声音 | 重复 |
|------|------|------|
| `ALARM_LEVEL_NOTICE` | 一声短鸣（200ms @1kHz） | 只响一次 |
| `ALARM_LEVEL_WARNING` | 三声短鸣（150ms 响 + 120ms 停 ×3） | 每 **10 秒**一轮 |
| `ALARM_LEVEL_EMERGENCY` | 高低交替（250ms@1.2kHz + 250ms@700Hz ×2） | 每 **5 秒**一轮 |

**三个级别都没有指示灯，只出声。**

## 3. 完整接口

```c
#include "sf32lb52_alarm.h"

enum alarm_level_e
{
  ALARM_LEVEL_NONE = 0,
  ALARM_LEVEL_NOTICE,      /* 提示：一声短鸣 */
  ALARM_LEVEL_WARNING,     /* 警告：三声短鸣，然后每 10 秒重复一次 */
  ALARM_LEVEL_EMERGENCY,   /* 紧急：高低交替的警报音，每 5 秒重复一次 */
};

enum alarm_event_e
{
  ALARM_EVENT_TRIGGERED = 1,   /* 报警被触发（含"已在报警中又被触发"） */
  ALARM_EVENT_CLEARED,         /* 被 alarm_clear() 主动解除 */
  ALARM_EVENT_TIMEOUT,         /* 超过自动解除时间，模块自己解除 */
};

struct alarm_status_s
{
  bool              active;         /* 是否正在报警 */
  enum alarm_level_e level;         /* 当前级别 */
  char              reason[32];     /* 短标识："sound" / "sos" / "fall" ... */
  char              text[96];       /* 给人看的描述 */
  uint32_t          triggered_ms;   /* 触发时刻（系统 tick 换算的 ms） */
  uint32_t          repeat_count;   /* 本次已重复播报了几轮 */
};

typedef void (*alarm_cb_t)(enum alarm_event_e event,
                           FAR const struct alarm_status_s *status,
                           FAR void *arg);

int  alarm_init(void);                 /* 幂等；重复调用直接返回 OK */
int  alarm_trigger(enum alarm_level_e level, FAR const char *reason,
                   FAR const char *text);
int  alarm_clear(void);
int  alarm_get_status(FAR struct alarm_status_s *out);
int  alarm_set_callback(alarm_cb_t cb, FAR void *arg);   /* cb=NULL 注销 */
```

### 3.1 行为规则

1. **非阻塞**：`alarm_trigger()` 只记录状态、唤醒工作线程就返回。
   声音由 `alarm` 工作线程做，**不占用调用者的线程**。
2. **懒初始化**：调用方不一定要先 `alarm_init()`；
   `alarm_trigger()` 内部发现没初始化就自己初始化。所以接进来只要一个函数。
3. **持续到解除**：报警会一直重复，直到 `alarm_clear()` 或自动超时。
   自动解除时间 = `ALARM_AUTO_TIMEOUT_MS`（头文件默认 `60 * 1000`，与状态机
   `SM_STATE_ALARM` 的 60 秒对齐），可在包含头文件前重定义。
4. **重复触发规则**（级别 `NONE < NOTICE < WARNING < EMERGENCY`）：
   - 当前未报警 → 按 `level` 开始；
   - 新 `level` **更高** → 按新级别**重来**（重新计时、清 `repeat_count`）；
   - **同级或更低** → 只更新 `reason`/`text`，**不打断**当前播报。
   以上三种都会回调一次 `ALARM_EVENT_TRIGGERED`，让上层知道"又报了一次"。
5. **回调**：触发、主动解除、超时都会调（如果注册了）。
   **回调在 `alarm` 工作线程的上下文里执行**，不是在调用
   `alarm_trigger()`/`alarm_clear()` 的线程。回调里**不要阻塞**（见坑 1、5）。
6. **`alarm_get_status()` 无锁安全读**：随时可调，**不初始化也能安全调**
   （未初始化时返回 `active=false` 的全零状态）。
7. **音频打不开时**：这一轮**没有任何输出**（不等于"静默空转"，也不卡死）。
   `open`/`CONFIGURE`/`START` 任一步失败，工作线程会打一条 `LOG_ERR`
   （`ALARM: audio open failed, this alarm has no output`），然后按 50ms 一块的
   节奏把这一轮"静默走完"（每个块检查一次状态），下一轮重新 `open` 再试。
   等待期间 `alarm_clear()`、再次 `alarm_trigger()` 和 60 秒自动超时都照常生效，
   响应上界 **~50ms**。

### 3.2 可调项（头文件里 `#define`，改前先看）

| 宏 | 默认 | 作用 |
|----|------|------|
| `ALARM_AUTO_TIMEOUT_MS` | `60000` | 自动解除时间（ms） |
| `ALARM_PLAYBACK_VOLUME` | `800` | 报警音量（标准 `AUDIO_FU_VOLUME`，0..1000） |

## 4. 上层怎么接（可直接照抄）

把网络上报和界面显示挂到 `alarm_set_callback()` 上：

```c
#include "sf32lb52_alarm.h"     /* 板级报警模块 */
#include "network_comm.h"       /* report_alarm()        : app/robot_ui/network_comm.h:77 */
#include "robot_ui.h"           /* robot_ui_show_alarm() : app/robot_ui/robot_ui.h:62 */

static void on_alarm(enum alarm_event_e event,
                     FAR const struct alarm_status_s *st, FAR void *arg)
{
  (void)arg;

  switch (event)
    {
      case ALARM_EVENT_TRIGGERED:
        report_alarm(st->reason, st->text);   /* MQTT zhi_ai/<id>/alarm + 手机推送 */
        robot_ui_show_alarm(st->text);        /* 界面（线程约束见坑 1 的提醒） */
        break;

      case ALARM_EVENT_CLEARED:
      case ALARM_EVENT_TIMEOUT:
        robot_ui_close_alarm();               /* robot_ui.h:65 */
        break;

      default:
        break;
    }
}

/* 开机时注册一次即可 */
void alarm_wire_up(void)
{
  alarm_set_callback(on_alarm, NULL);
}

/* 状态机收到 SM_EVENT_ALARM_DETECTED 时： */
alarm_trigger(ALARM_LEVEL_EMERGENCY, "fall", "检测到摔倒，位置：客厅");

/* 用户确认/异常解除时： */
alarm_clear();
```

> `report_alarm()` 没连上网络时返回 -1，但**不影响本地报警**——
> 声音是本地行为，网络是"锦上添花"。
>
> **界面提醒**：`robot_ui_show_alarm()` 内部若直接操作 LVGL，而回调又在
> `alarm` 工作线程里，就有线程安全问题（LVGL 不是线程安全的）。
> 对接时要么确认 `robot_ui_show_alarm()` 自己做了转发（只置标志、由 main
> 主循环刷新），要么在回调里只置一个全局标志，让主循环去刷界面。
> 详见 `docs/network_api_usage.md` 第 4 节。

## 5. `hw_test alarm` 用法与期望输出

```
nsh> hw_test alarm [级别] [秒]
```

- 级别：`1` = NOTICE，`2` = WARNING，`3` = EMERGENCY（默认 3）
- 秒数：保持报警的秒数（默认 3）

例：`hw_test alarm 3 3` —— 紧急报警 3 秒后自动解除。期间应该能听到高低交替的
警报音（本模块没有指示灯，只能听声音）。

期望输出（节选，回调打印会与主流程交错）：

```
[ALARM] 级别=3(EMERGENCY) 保持 3 秒
      [ALARM] event=TRIGGERED level=EMERGENCY reason=hw_test
      [PASS] 触发报警  (EMERGENCY)
      ...（期间：高低交替的警报音）...
      状态: active=1 level=EMERGENCY repeat=1 reason=hw_test text=hw_test alarm 子命令
      [PASS] 报警持续  (仍在报警中)
      [ALARM] event=CLEARED level=EMERGENCY reason=hw_test
      解除后: active=0 level=EMERGENCY 回调事件=2 次
      [PASS] 解除报警  (已解除)
      [PASS] 报警回调  (收到回调)
```

自检项：`触发报警` / `报警持续` / `解除报警` / `报警回调` 四项都应为 `[PASS]`。
`alarm 1 2`（NOTICE）只响一声短鸣，2 秒后自动解除。

## 6. 坑（按重要性排序）

> ⚠️ **第 0 条，最严重：别以为 `close()` 是安全的收尾 —— 它一度会让整机卡死。**
> 上层 `audio_close()` 在**最后一个 fd** 被关闭时（`nuttx/audio/audio.c:235-272`），
> 会先拿 `upper->lock`、再 `spin_lock_irqsave()`（本构建非 SMP，等价于**关中断**），
> 然后在这个上下文里调下层 `ops->shutdown()`。而我们的 `shutdown()` 原来又走了一遍
> **完整的 `hw_stop()`** —— 可 `AUDIOIOC_STOP` 时已经做过一次了。在关中断的上下文里
> **重复关闭已经关掉的音频模拟通路**，就整机静默卡死：**无 panic、无 backtrace、
> 不复位、串口探活毫无响应**（只能重新烧录复位）。
>
> 已修（`board/contest_board/src/sf32lb52_audio.c`）：
> - `hw_stop()` 改成**幂等**（已停过就直接返回）；
> - 给 `shutdown()` 单独写了一个**最小化停止函数**：只停 DMA / 禁用 AUDPRC /
>   复位通道状态 / 关功放，**不回调上层、不碰模拟通路**；
> - `AUDIOIOC_STOP` 那条路（中断是开的）保持原样调完整 `hw_stop()`，所以
>   "STOP 唤醒阻塞 `read()`"的行为不变。
>
> **为什么这个 bug 藏了这么久**：只有 fd 是最后一个时上层才调 `shutdown()`。
> 上层应用（`hello_app`）通常一直占着同一个设备，所以 `audio_test` / `hw_test audio`
> 的 `close()` 从来没走到过这条路 —— 报警模块自己是"开一次、关一次"的用法，
> 才把它踩出来。
>
> 排查手法（留个记录）：现象是"整机静默无输出"，先怀疑自旋/死锁而不是崩溃；
> 在可疑函数**逐步打 `syslog(LOG_ERR, ...)`**，最后一条打印就是卡死点。
> 另外本项目 `HAL_ASSERT()` 展开成 `while(1){}`（`USE_FULL_ASSERT` 是注释掉的），
> 任何 HAL 断言失败也是静默死循环——所以**没有打印 ≠ 没出错**。

1. **回调在工作线程上下文**：`alarm_cb_t` 是在 `alarm` 工作线程里被调用的
   （`sf32lb52_alarm.c` 的 `alarm_emit()`）。别在回调里 sleep、等信号量、
   做重运算；也别直接碰 LVGL。耗时动作丢给别的任务或只置标志。
2. **音频设备是独占的**：报警音和成员二的 TTS/提示音会抢同一个
   `/dev/audio/audio0`。现象：报警正在响时 TTS `write()` 会阻塞或失败；
   反过来 TTS 正占用设备时报警 `open()`/`write()` 失败，**这一轮就没有任何
   输出**——模块只打一条 `LOG_ERR`：`ALARM: audio open failed, this alarm has
   no output`，本轮剩下的时间静默走完，下一轮重新 `open` 再试；期间
   `alarm_clear()` 和自动超时仍然有效（见 3.1 第 7 条）。
   规避：业务上尽量别让"警报音"和"长 TTS"同时起；报警的两轮之间会主动
   `AUDIOIOC_STOP` + `close`（间隔约 4~9 秒），TTS 可以趁这个空隙播。
3. **没有视觉反馈，纯靠声音**：本模块不驱动任何指示灯（原因见第 2 节），
   报警在现场唯一的表现就是喇叭声。演示/验收前务必确认**音量**
   （`ALARM_PLAYBACK_VOLUME`）和**现场噪声**；设备静音或环境嘈杂时，
   报警会"既看不见也听不清"。不要指望靠灯判断报警有没有起来，
   要么听声音，要么读 `alarm_get_status()`。
4. **自动超时是 60 秒**：`ALARM_AUTO_TIMEOUT_MS` 默认 60000，对齐状态机
   `SM_STATE_ALARM`。超时会回调 `ALARM_EVENT_TIMEOUT`，**不是** CLEARED，
   上层别把两者混为一谈。
5. **不要在回调里阻塞**（重复强调）：报警线程阻塞 = 声音卡住、解除变慢、
   后续触发没人处理。这是最常见的误用。
6. **音频缓冲必须是静态的**：本模块的工作线程栈只有 4096 字节。工程踩过
   "32KB 局部数组被 GCC 提到函数序言导致 hardfault"的坑，所以报警音缓冲
   `g_alarm_tone` 是文件级 `static`，且只按 50ms（800 样本 = 1600 字节）一块生成。
   改这块代码时**别把它挪到栈上**。
7. **板级源码的 include 顺序**：同一份板级源文件里同时用 Sifli HAL 时，
   `bf0_hal.h` 必须在 `bf0_hal_gpio.h` 之前。若先包含 `bf0_hal_gpio.h`，它内部
   `bf0_hal_def.h -> register.h -> bf0_hal.h` 会先占住 include guard，导致
   `conf_hcpu.h` 里的 `bf0_hal_gpio.h` 被跳过、`GPIO_TypeDef` 没定义，
   `bf0_hal_aon.h` 随即报 `unknown type name 'GPIO_TypeDef'`。
   （报警模块本身只用 NuttX 接口，不 include 任何 Sifli HAL 头。）
8. **音量接口**：标准接口是 `AUDIOIOC_CONFIGURE` + `AUDIO_TYPE_FEATURE` +
   `AUDIO_FU_VOLUME`（`0..1000`），不是 `AUDIOIOC_SETPARAMTER`（那是源码注释里的
   旧拼写）。参考 `app/audio_test/main.c:430` 的 `audio_test vol`。

## 7. 验证情况

- ✅ **编译**：`bash /home/youdian/build_full.sh` 通过，无 error / warning；
  `nm nuttx` 可见 `alarm_init/alarm_trigger/alarm_clear/alarm_get_status/
  alarm_set_callback` 与 `hw_test_main` 都已链入固件。
- ✅ **真机（修复后）**：三个级别连跑全部 4/4 PASS，报警音能听到，不再卡死。
  详见下面"真机实测结果"。
- 未做：网络/界面回调的实际联调（依赖成员三的模块与网络环境）。

### 真机实测结果（2026-09-12，SF32LB52-DevKit-LCD）

> ⚠️ **第一版是坏的，会卡死整机 —— 过程和修复见第 6 节第 0 条。**
> 下面是**修复后**的结果。

连跑三个级别（每级各一次，中间不重启）：

```
hw_test alarm 1 2   -> 4/4 PASS   (NOTICE,    只响一声)
hw_test alarm 3 4   -> 4/4 PASS   (EMERGENCY, 高低交替警报音)
hw_test alarm 2 2   -> 4/4 PASS   (WARNING,   三声短鸣)
```

单次完整输出（`hw_test alarm 3 4`）：

```
[ALARM] 级别=3(EMERGENCY) 保持 4 秒
ALARM: worker started (pid=19)
      [ALARM] event=TRIGGERED level=EMERGENCY reason=hw_test
      [PASS] 触发报警  (EMERGENCY)
      状态: active=1 level=EMERGENCY repeat=1 reason=hw_test text=hw_test alarm 子命令
      [PASS] 报警持续  (仍在报警中)
      [ALARM] event=CLEARED level=EMERGENCY reason=hw_test
      解除后: active=0 level=EMERGENCY 回调事件=2 次
      [PASS] 解除报警  (已解除)
      [PASS] 报警回调  (收到回调)
   结果: 4/4 PASS
```

- ✅ **出声**：现场确认能听到高频/低频交替的警报音（EMERGENCY 的
  `1200/700Hz`、每 5 秒一轮）。`repeat=1` 与"保持 4 秒 < 5 秒轮间隔"一致。
- ✅ 事件回调、状态查询、主动解除都按预期工作（4/4 PASS）。
- ✅ **修 `shutdown()` 之后的回归检查**（同一次烧录内）：
  `hw_test audio 2` → `read 返回 64000/64000`、1/1 PASS；
  `audio_test 2000 1000` → `WRITE done / STOP done / done`；
  `hw_test rtc 3` → 2/2 PASS。说明改 `shutdown()` 没有弄坏录音/播放/RTC。
- ⏳ 音量（`ALARM_PLAYBACK_VOLUME=800`）只按"够响"取的值，没按现场调过。
- ⏳ 报警时没有指示灯，现场只能靠听；演示前请确认音量与环境噪声。
- ⏳ 报警音和成员三的网络上报 / 界面**还没联调过**（回调怎么接见第 4 节）。

```
[ALARM] 级别=3(EMERGENCY) 保持 4 秒
ALARM: worker started (pid=19)
      [ALARM] event=TRIGGERED level=EMERGENCY reason=hw_test
      [PASS] 触发报警  (EMERGENCY)
      状态: active=1 level=EMERGENCY repeat=1 reason=hw_test text=hw_test alarm 子命令
      [PASS] 报警持续  (仍在报警中)
      [ALARM] event=CLEARED level=EMERGENCY reason=hw_test
      解除后: active=0 level=EMERGENCY 回调事件=2 次
      [PASS] 解除报警  (已解除)
      [PASS] 报警回调  (收到回调)
   结果: 4/4 PASS
```

- ✅ **出声**：现场确认能听到高频/低频交替的警报音（EMERGENCY 的
  `1200/700Hz`、每 5 秒一轮）。`repeat=1` 与"保持 4 秒 < 5 秒轮间隔"一致。
- ✅ 事件回调、状态查询、主动解除都按预期工作（4/4 PASS）。
- ⏳ 音量（`ALARM_PLAYBACK_VOLUME=800`）只按"够响"取的值，没按现场调过。
- ⏳ 报警时没有指示灯，现场只能靠听；演示前请确认音量与环境噪声。
