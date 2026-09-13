# 板级 RTC 每日定时提醒使用说明（`sf32lb52_rtc_alarm`）

> 面向队友：**怎么用**这个模块，以及**哪些坑不能踩**。
> 头文件：`board/contest_board/src/sf32lb52_rtc_alarm.h`
> 实现：`board/contest_board/src/sf32lb52_rtc_alarm.c`
> 状态：**已在 openvela 工作区编译通过（无 error / warning，见第 8 节）**；
> **真机未验证**——上板由调用方/成员一验证（本文档不代替上板测试）。

---

## 1. 这个模块解决什么问题

给上层一个**"每天 hh:mm 到点回调一次"**的接口，供成员二做
"每天 08:00 提醒吃药""每天 19:00 问候"这类**挂钟时间**的定时。

它和同目录的 `sf32lb52_alarm`（设备级报警）**是两回事，别混**：

| | `sf32lb52_alarm`（报警） | `sf32lb52_rtc_alarm`（本模块） |
|---|---|---|
| 触发条件 | 上层 `alarm_trigger()` 立刻触发 | **挂钟时间到 hh:mm** 触发 |
| 动作 | 自己**响喇叭**，持续到解除/超时 | **只回调上层**，自己不发声、不驱灯 |
| 时间基准 | 系统 tick（开机以来） | 硬件 RTC（掉电会丢，见第 7 节） |
| 用完 | `alarm_clear()` | `rtc_alarm_cancel()` |
| 回调上下文 | 报警工作线程 | 本模块工作线程（栈 4096，优先级 115） |

到点后做什么（播 TTS、刷 UI、发 MQTT）**完全由回调方决定**；本模块不
include 任何 app 头文件（板级模块不反向依赖应用层）。

```
上层业务 ──rtc_alarm_at_daily(8,0,cb,arg)──> sf32lb52_rtc_alarm
                                                  │  工作线程:
                                                  │  读 RTC -> 算下一个 08:00
                                                  │  -> RTC_SET_ALARM(绝对)
                                                  │  -> 等 SIGUSR1
                                                  ▼
                                   到点回调 cb()（工作线程上下文）
                                                  │
                                                  └─ 自动排下一天
```

---

## 2. 硬件与关键语义（源码核实，别按 Linux 直觉写）

| 问题 | 答案 | 出处 |
|------|------|------|
| 设备节点 | `/dev/rtc0`（`CONFIG_RTC/RTC_ALARM/RTC_DRIVER` 都开） | `.config:839-843` |
| `read(/dev/rtc0)` 能等 alarm 吗？ | **不能**。upper half 的 `rtc_read()` 直接 `return 0`（EOF），立即返回 | `nuttx/drivers/timers/rtc.c:318-321` |
| 有 `poll()` 吗？ | **没有**，`g_rtc_fops` 里 poll 是 NULL | `rtc.c:136` |
| alarm 到点怎么通知？ | **信号**：`ioctl` 参数里带 `struct sigevent`，到点 upper half 用 `nxsig_notification()` 通知 pid | `rtc.c:190-200` |
| `pid` 填什么？ | **0 = 通知"设 alarm 的那个任务"自己** | `rtc.c:471-477` |
| 能同时有几个 alarm？ | **1 个**（`CONFIG_RTC_NALARMS=1`，id 只能是 0），设第二个会覆盖第一个 | `.config:842` |
| alarm 精度 | 秒 | `vendor/sifli/chips/sf32lb52/sf32lb_rtc.c` |
| 改时间会顺带改系统时钟吗？ | 会。`RTC_SET_TIME` 成功后内部调 `clock_synchronize(NULL)` | `rtc.c:395-411` |
| 掉电时间会丢吗？ | 会。**板上没有 RTC 备份电池**，开机读到 2000-01-01 附近，必须先对时 | 见第 7 节 |

**"pid=0 通知设 alarm 的那个任务"这一条决定了模块的形态**：设 alarm 的
动作必须发生在将来收信号的那个线程里，否则信号会发给一个已经退出的
任务、没人收。所以本模块自己起了一个**常驻工作线程**，所有 ioctl 都在
它里面做；信号处理函数也装在它里面（`sf32lb52_rtc_alarm.c:762` 的
`rtc_alarm_worker`）。

---

## 3. 完整接口

```c
#include "sf32lb52_rtc_alarm.h"     /* 板级头文件，include 路径已配好 */

typedef void (*rtc_alarm_cb_t)(FAR void *arg);

/* 每天 hour:min 到点回调一次，然后自动排下一天。
 * hour 0..23、min 0..59，非法返回 -EINVAL；cb 可为 NULL（到点不回调）。 */
int rtc_alarm_at_daily(int hour, int min, rtc_alarm_cb_t cb, FAR void *arg);

/* 取消当前每日提醒；没有在跑时是安全的空操作。 */
int rtc_alarm_cancel(void);

/* 读当前 RTC 时间，方便上层显示；不需要先 init。 */
int rtc_alarm_now(FAR struct rtc_time *out);

/* 时间是否已经对过（>= 2000 年）。1 = 已对时，0 = 没对时。 */
int rtc_alarm_time_valid(void);
```

`struct rtc_time` 就是 `nuttx/include/nuttx/timers/rtc.h` 里的那个，
字段语义和 `struct tm` 完全一致（`tm_year` 从 1900 起、`tm_mon` 0..11，
**显示时要 +1900 / +1**）。

### 3.1 行为规则（重点）

1. **懒初始化**：调用方**不用**先 init。第一次 `rtc_alarm_at_daily()`
   内部会创建常驻工作线程（名字 `rtcalarm`，栈 4096，优先级 115），
   之后重复调用复用同一个线程。
2. **时间无效时不排 alarm**：`RTC_RD_TIME` 读出来 `tm_year < 100`
   （早于 2000 年）时，**不会**去算"下一个 08:00"（算出来没意义），
   而是**每 30 秒重试一次**，等别处把 RTC 对好（`date -s` 或网络对时）
   后自动开始正常工作。这个"等对时"的循环能被 `rtc_alarm_cancel()`
   立刻打断（`sf32lb52_rtc_alarm.c:476` 的 `rtc_alarm_wait_valid`）。
3. **算"下一个 hh:mm"**：读当前时间 → 今天这个时刻还没到就用今天，
   否则明天（日期正确进位，跨月/跨年都对）；**星期字段也按最终日期算对**
   （`rtc_alarm_weekday`，Sakamoto 算法）。**不用 `mktime()`/`localtime()`**
   ——本固件 `CONFIG_LIBC_LOCALTIME` 未开（`localtime()` 就是 `gmtime()`），
   日历换算只支持 1970 以后，而且 `time_t` 是 32 位、2099 年会溢出。
   为什么这么选见 `sf32lb52_rtc_alarm.c` 文件头注释。
4. **到点回调后自动排下一天**：这是"每天"的关键；即使本次 alarm 因为
   别的原因（被别的代码覆盖了唯一的槽、时间被改）**漏触发**，工作线程
   也会在目标时刻过后检测到并报一条 `LOG_WARNING` 后重排，不会只响一次
   就永远哑掉。
5. **可以被打断**：`rtc_alarm_cancel()` 用"标志 + 信号量"唤醒工作线程
   （`sf32lb52_rtc_alarm.c:874`），不需要等它慢慢醒；工作线程退出前会
   `RTC_CANCEL_ALARM` 把唯一的硬件槽还回去。
6. **只支持一个每日提醒**：硬件只有一个 alarm 槽，重复调用
   `rtc_alarm_at_daily()` = **覆盖**上一个（回调、时刻都换掉）。
7. **回调在工作线程上下文**：`rtc_alarm_cb_t` 是在 `rtcalarm` 工作线程
   里被调用的（不是调用 `rtc_alarm_at_daily()` 的那个线程）。**回调里
   不要阻塞**：不要 sleep、等信号量、做重运算，**也不要直接播 TTS 或碰
   LVGL**（LVGL 不是线程安全的）。推荐只置一个标志，让主循环去做。

---

## 4. 成员二怎么接"定时问候 / 吃药提醒"（可直接照抄）

**核心原则：回调里只置标志，播音/刷 UI 交给主循环。** 不要在回调里
直接 `audio_play()` / 直接调 LVGL，否则会拖住工作线程，且 LVGL 有线程
安全问题。

```c
#include "sf32lb52_rtc_alarm.h"

/* 回调与主循环共享的标志。用 volatile：回调在工作线程写，主循环读。 */
static volatile bool g_medicine_due;      /* 每天 08:00 吃药 */
static volatile bool g_greeting_due;      /* 每天 19:00 问候 */

static void on_medicine(FAR void *arg)
{
  (void)arg;
  g_medicine_due = true;                  /* 只置标志，别在这里播音/碰 UI */
}

static void on_greeting(FAR void *arg)
{
  (void)arg;
  g_greeting_due = true;
}

/* 开机时注册。注意：只有一个槽，所以"多个提醒"要在业务层自己排队
 * （见第 7 节第 1 条），不能同时注册两个。 */
void rtc_reminder_wire_up(void)
{
  if (rtc_alarm_time_valid())
    {
      rtc_alarm_at_daily(8, 0, on_medicine, NULL);
      /* 想再加 19:00：不能同时排两个，只能先排 8:00，到点在回调里
       * 置标志后，再由业务层调 rtc_alarm_at_daily(19,0,...) 换过去，
       * 或自己在业务层算"下一个提醒"（见坑 1）。 */
    }
  else
    {
      /* 没对时：模块会每 30 秒重试等对时，这里可以提示先对时 */
      syslog(LOG_WARNING, "RTC 未对时，先 date -s 或用网络时间对时\n");
    }
}

/* 主循环（和 LVGL 同一个线程）里轮询标志： */
void app_main_loop(void)
{
  for (;;)
    {
      if (g_medicine_due)
        {
          g_medicine_due = false;
          audio_play_tts("该吃药了");       /* 成员二自己的播音接口 */
          robot_ui_show_reminder("吃药", "08:00");
        }

      if (g_greeting_due)
        {
          g_greeting_due = false;
          audio_play_tts("该起床啦");
        }

      /* ... 其余主循环逻辑 ... */
    }
}
```

> 如果不确定 RTC 有没有对时，先 `rtc_alarm_now()` 打印一下当前时间，
> 或者用 `rtc_alarm_time_valid()` 判断。**没对时的时候模块不会乱排
> alarm**，但也意味着"每天 08:00"真正开始生效是在对时之后。

---

## 5. `hw_test rtcday` 用法与期望输出

```
nsh> hw_test rtcday [时] [分]
```

- 不传参：用**当前时间 + 1 分钟**（这样不用等到明天就能验）。
- 传 `时`（可选再传 `分`）：注册 `每天 时:分`。
- 行为：打印当前 RTC 时间、`rtc_alarm_time_valid()` 结果、下一个提醒
  时刻；注册一个只打印的回调；然后**最多等 90 秒**等回调。等到 = PASS，
  超时 = FAIL 并 `rtc_alarm_cancel()` 收尾，**绝不永久卡住**。
- 计时用 `clock_systime_ticks()` + `TICK2MSEC()`（工程刚修过"累加 sleep"
  的计时 bug；实现见 `app/hw_test/main.c:1277` 的 `step_rtcday`）。

**已对时的期望输出**（`hw_test rtcday`，假设现在是 15:30:20）：

```
[RTCDAY] 每日 RTC 提醒 /dev/rtc0
      当前时间 : 2026-09-13 15:30:20
      [PASS] 读 RTC 时间
      时间已对时 : 是（rtc_alarm_time_valid()=1）
      未指定时刻：用当前时间 + 1 分钟 = 15:31
      下一个提醒 : 今天 15:31
      [PASS] 注册每日提醒  (已交给模块工作线程排 alarm)
      等回调（最多 90 秒）...
RTCALARM: worker started (pid=20)
RTCALARM: daily 15:31 armed for 2026-09-13 (wday=0, now 2026-09-13 15:30:20)
      [RTCDAY] 每日提醒回调触发（模块工作线程上下文）
      等了 40012 ms 收到回调（共 1 次）
      [PASS] 每日提醒  (回调已触发)
   结果: 3/3 PASS
```

（`RTCALARM:` 开头的行是模块 `syslog` 打的，和 `hw_test` 的 printf 会交错，
属正常现象。）

**没对时的期望输出**（模块会进入"等对时"状态，本命令会等到 90 秒超时
FAIL——这是**正确**行为，说明现场得先 `date -s`）：

```
      时间已对时 : 否，RTC 还没对时；模块会每 30 秒重试等对时（rtc_alarm_time_valid()=0）
      下一个提醒 : 待 RTC 对时后才能确定（对时前模块不排 alarm）
RTCALARM: RTC time not set (year 1996 < 2000); waiting for time sync, retry every 30 s
      超时：等了 90000 ms 没收到回调
      [FAIL] 每日提醒  (超时未回调（已 rtc_alarm_cancel 收尾）)
```

> **判据的边界**：本模块（和 `hw_test rtc` 一样）用 `tm_year >= 100`
> 即"年份 >= 2000"作为"已对时"。如果这块板子掉电后恰好读回
> **2000-01-01**，按这个判据会被当成"已对时"并去排 2000 年的 alarm。
> 现场如果确认读回的就是 2000-01-01，先 `date -s` 对到真实时间再注册
> 提醒即可。

---

## 6. 和 `hw_test rtc` 一起用的注意

`hw_test rtc [秒]` 用的是同一个 `/dev/rtc0` 的 alarm 槽（`RTC_SET_RELATIVE`）。
**全板只有一个槽**，所以：

- 别在 `rtcday` 排着的期间跑 `hw_test rtc`（会把每日提醒覆盖掉；模块
  到点后检测到漏触发会重排，但这期间那一次就没了）。
- `rtcday` 本身结束前一定会 `rtc_alarm_cancel()`，正常情况下不占着槽。

---

## 7. 坑（按重要性排序）

1. **只有一个 alarm 槽（`CONFIG_RTC_NALARMS=1`）**。本模块是"单每日提醒"
   语义：重复注册 = 覆盖。想做"8:00 吃药 + 19:00 问候"多个提醒，要在
   **业务层自己排队**（例如：注册 8:00，到点回调后由主循环再注册 19:00；
   或者模块只维护"下一个提醒"，业务层每次回调后决定下一次的时刻）。
   也可以以后在模块里加"提醒列表 + 每次只排最近的一个"，但**不要在硬件
   层放多个槽**——没有。
2. **等 alarm 必须靠信号，不能靠 `read()`**。`read(/dev/rtc0)` 永远返回 0
   （EOF），而且没有 `poll()`（`rtc.c:318-321`、`rtc.c:136`）。本模块已经
   把信号处理封装好了，上层不用管。
3. **掉电时间就丢**。板上没有 RTC 备份电池，每次掉电开机读到的是
   2000-01-01 附近。所以"每天 08:00"真正生效前必须先对时。NSH 的
   `date -s` **只认 `MMM DD HH:MM:SS YYYY` 格式**，例如
   `date -s "Sep 13 15:30:00 2026"`；写 `2026-09-13 15:30:00` 会报
   `argument invalid`（`apps/nshlib/nsh_timcmds.c`）。
4. **回调在工作线程上下文**：别在回调里 sleep / 等锁 / 做重运算，更别
   直接播音或碰 LVGL。只置标志，主循环去处理（第 4 节示例）。
5. **`RTC_SET_TIME` 会顺带同步系统时钟**（`clock_synchronize`，
   `rtc.c:409`），所以 `time(NULL)` / `CLOCK_REALTIME` 会跟着跳。别在
   动画/超时逻辑里依赖 `CLOCK_REALTIME` 单调递增——用 `CLOCK_MONOTONIC`
   或 `clock_systime_ticks()`。
6. **回调后要"自动排下一天"才叫每日提醒**。本模块内部已做；如果你绕过
   本模块自己写 RTC alarm，务必在每次触发后重排下一天，否则只响一次就
   永远哑了。模块还有个兜底：目标时刻过了还没收到信号会报 WARNING 重排
   （`sf32lb52_rtc_alarm.c:556` 的 `rtc_alarm_wait_fire`）。
7. **绝对 alarm 的日期字段也参与匹配**（`sf32lb_rtc.c` 的 `sf32lb_setalarm`
   写了 Date/Month/Year），跨天/跨月要算对，否则 alarm 可能不触发或立刻
   触发。本模块的日期进位是手算的（月长表 + 闰年），已覆盖跨月/跨年。
   另外 **RTC 硬件只表示 2000–2099 年**（`date_2_reg` 的世纪位），
   别把时间设到范围外。
8. **不要自己给年份 +100 修正**。下层"写 2026 读回 1926"的 bug 已修
   （补丁 `patches/vendor_sifli-rtc-alarm-fix.patch`，工作区已应用），
   读回来的 `tm_year` 是对的，`tm_year >= 100` 就表示 >= 2000 年。
9. **LVGL 不是线程安全的**：alarm 的提醒业务不要直接碰 LVGL 控件，只置
   全局标志，让 UI 主循环去刷（同 `docs/network_api_usage.md`）。

---

## 8. 验证情况

- ✅ **编译**：`bash /home/youdian/build_full.sh` 通过，无 error / warning；
  `nm nuttx` 可见 `rtc_alarm_at_daily / rtc_alarm_cancel / rtc_alarm_now /
  rtc_alarm_time_valid` 已链入固件，`hw_test` 里也有 `rtcday` 子命令。
  另外用 `compile_commands.json` 里的**原始命令**把新 `.c`（去掉 `-w`
  也试过）单独重编，stderr 为空。
- ⏳ **真机未验证**：本文没有任何"已上板跑通"的结论；`hw_test rtcday`
  的实际输出（尤其"到点回调"）需要上板确认。
- ⏳ 没做：多个提醒的业务层排队（第 7 节第 1 条）、和网络对时（NTP /
  server 时间戳 + `RTC_SET_TIME`）的联调。
