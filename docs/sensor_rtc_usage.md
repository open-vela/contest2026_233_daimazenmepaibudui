# SF32LB52 传感器现状 / RTC 定时 / 持久存储 使用说明

> 智爱陪伴 —— openvela 板级硬件接口（成员一交付物）
> 硬件：SF32LB52-DevKit-LCD（**没有 IMU** + Sifli RTC + SPI1 TF 卡座）
> 状态：**本文所有结论都从源码核实过**（file:line 都在文里，可以自己点开对）；
> 第 1 节的 IMU 结论是真机 + 官方 wiki 核过的；RTC alarm 的现状见第 2 节。
> 自检命令：`hw_test imu` / `hw_test rtc`（实现见 `app/hw_test/main.c`）

---

## 0. 一句话总览（先看这个，别踩空）

| 想做的事 | 标准接口 | 节点 | 现在能不能用 |
|---------|---------|------|-------------|
| 读加速度/陀螺（跌倒/撞击检测） | —— | —— | ❌ **本板没有 IMU 硬件**（模组 BOM 里就没有），节点不会出现；见第 1 节 |
| 读/设当前时间 | `ioctl(RTC_RD_TIME / RTC_SET_TIME)` | `/dev/rtc0` | ✅ 节点会建出来（配置齐了），时间没对过而已 |
| 定时触发（"N 秒后"/"每天 08:00"） | `ioctl(RTC_SET_ALARM / RTC_SET_RELATIVE)` + **信号** `sigevent` | `/dev/rtc0` | ✅ 接口齐；注意**不是** `read()` 等 alarm |
| 掉电保存数据 | vfat 挂载 | `/data/tf` | ⚠️ 插了 TF 卡才有；`/data` 是 tmpfs，掉电就没 |

> 两个最容易踩的坑先记住：
> 1. **本板没有 IMU**（模组 BOM 里没有加速度计）。别开
>    `CONFIG_SENSORS_LSM6DSL`，也别照 uORB 传感器驱动的例子写代码。
> 2. **`/dev/rtc0` 的 `read()` 返回 0（EOF），不会阻塞等 alarm**；
>    alarm 到点是用**信号**通知的（`struct sigevent`），等法见第 2.4 节。

---

## 1. 传感器（IMU）：**本板没有**

### 1.1 结论（先看这个）

**SF32LB52-DevKit-LCD 上没有任何加速度计 / 陀螺。** 所以：

- 不会出现 `/dev/lsm6dsl0`，也**不要**去开 `CONFIG_SENSORS_LSM6DSL`；
- 成员二的"跌倒检测"在这块板子上**没有硬件可做**，备选方案见 1.4；
- `hw_test imu` 永远 FAIL，而且是**故意**的 —— 它报的是
  "本板无 IMU 硬件"，不是"驱动没编"。硬件不存在这件事要用 FAIL 说清楚，
  不能让它假装通过。

### 1.2 证据（为什么可以确定没有）

1. **模组 BOM 里就没有这个器件。** 板载模组是 SF32LB52-MOD-1（N16R8），
   器件只有：MCU（SF32LB525UC6）+ 128Mb NOR Flash（PY25Q128HA）+
   32.768kHz / 48MHz 晶振 + u.FL 天线座 + 阻容感 + 屏蔽罩。
   没有任何 I2C 从器件，更没有加速度计。
2. **官方 wiki 的 GPIO 分配表里没有 IMU。**
   [SF32LB52-DevKit-LCD 使用指南](https://wiki.sifli.com/board/sf32lb52x/SF32LB52-DevKit-LCD.html)
   表 1 逐脚列了 68 个管脚：PA30 = 触摸屏 I2C_SCL（脚 24）、
   PA31 = 触摸屏 INT（脚 25）、PA33 = 触摸 I2C_SDA（脚 15）、
   PA09 = 触摸 RST（脚 38）、PA10 = AU_PA_EN（脚 32）……
   **没有任何一脚写着"传感器 LDO"或"IMU INT"**。
3. **我们板级代码里那段 IMU 代码是从别的板抄错的**（已删除，见 1.3）。

### 1.3 那段错位的 IMU 代码（已从板级删除）

`board/contest_board/src/sifli_ap.c` 里原本有一段
`sf32lb52_lsm6ds3_initialize()`，它会：

```c
HAL_PIN_Set(PAD_PA30, GPIO_A30, PIN_NOPULL, 1);   /* ← 把触摸的 I2C 时钟脚抢成 GPIO */
sifli_gpio_config(SF32LB52_LSM6DS3_LDO_PIN, GPIO_OUTPUT);
sifli_gpio_write(SF32LB52_LSM6DS3_LDO_PIN, true);
HAL_PIN_Set(PAD_PA31, GPIO_A31, PIN_PULLUP, 1);
```

它声称"PA30 = 传感器 LDO 使能、PA31 = IMU INT"。而**本板
PA30 = 触摸屏 I2C1_SCL、PA31 = 触摸 INT**，这是引脚错位。

错位的来源可以坐实：那个 bringup 函数的名字就叫
`sf32lb52_lchspi_ulp_bringup()`（`sifli_ap.c` 里定义），对应的是
**立创·黄山派 / SF32LB52-ULP** 板 —— 那块板才是
PA30 = Sensor Power、触摸 SCL = PA37。本仓库自己的
`board/contest_board/README_zh-cn.md:112` 也写着
"触摸 I2C SCL：**PA30**（-ULP 是 PA37）"。

**已删除的内容**（`board/contest_board/src/sifli_ap.c`）：

| 删掉的东西 | 原内容 |
|-----------|--------|
| 头文件 | `#include <nuttx/sensors/lsm6dsl.h>`（原先被 `CONFIG_SENSORS_LSM6DSL` 包着） |
| 宏 | `SF32LB52_LSM6DS3_I2C_BUS` / `_DEVPATH` / `_LDO_PIN` / `_INT_PIN` |
| 函数 | `sf32lb52_lsm6ds3_initialize()` 整个 |
| I2C2 pinmux | `PAD_PA40 → I2C2_SCL`、`PAD_PA39 → I2C2_SDA` |
| 调用 | `sf32lb52_lsm6ds3_initialize(i2c1)` |

注意：删掉之后 **I2C2（`/dev/i2c1`）照常初始化** —— 那条总线原本是给
板载充电芯片 AW32001 用的，和 IMU 无关，不受影响。

**为什么必须删、而不是留着（反正编译不进去）**：它是一个只在特定配置下
才引爆的地雷，留着有两个雷：

1. 一旦有人为了 IMU 打开 `CONFIG_SENSORS_LSM6DSL`，PA30 就被
   `HAL_PIN_Set` 抢成 GPIO → **触摸屏 I2C 没时钟，触摸直接坏**；
   更隐蔽的是 **`CONFIG_LCD=n`** 时触摸走的是 bringup 里的内联初始化
   （`sifli_ap.c` 里 `#elif defined(CONFIG_INPUT_FT6146)` 那个分支），
   它排在 IMU 段**前面**，于是 PA30 最后会被 IMU 抢走。
2. `PAD_PA39 / PA40` 被 mux 成 I2C2 之后，如果以后换 **8080(MCU) 屏**，
   这两脚正好是 LCD 数据线 DB3 / DB4（wiki 表 1 的脚 9 / 脚 8）→ 屏幕花。

### 1.4 成员二的"跌倒检测"怎么办

本板没有加速度计，"靠 IMU 判跌倒"这条路在这块板子上走不通。三条备选，
按可落地程度排序：

1. **外接毫米波雷达** —— 和原方案"非接触式"的定位一致。走 UART，
   或者走 I2C2（现在 I2C2 已经腾干净了，`/dev/i2c1` 可用）。
2. **外接 IMU 模块到 I2C2** —— 当前 QSPI 屏配置下 PA39 / PA40 是空的，
   可以直接挂一个 LSM6DS3 / MPU6050 模块。接上之后再写 bringup，
   **绝对不要碰 PA30 / PA33**（那是触摸的 I2C1）。
3. **改用声音 / 交互事件** —— `/dev/audio/audio0` 的录音通路已经实测通了
   （见 `docs/audio_driver_usage.md`），做"长时间无声音 / 异常声响"
   这类弱一些的判据是现成可用的。

### 1.5 如果以后真的换了带 IMU 的板

那时要做的事（现在**都不要**做）：

- 打开 `CONFIG_SENSORS_LSM6DSL=y`（依赖已经满足：`CONFIG_ALLOW_BSD_COMPONENTS=y`）；
- 节点是**老式字符设备** `/dev/lsm6dsl0`，**不是** uORB 的
  `/dev/uorb/sensor_accel0`；接口是
  `ioctl(fd, SNIOC_START, 0)` → `ioctl(fd, SNIOC_LSM6DSLSENSORREAD, &s)`
  或 `read(fd, raw, 6*N)` → `ioctl(fd, SNIOC_STOP, 0)`；
- 驱动**没有 `poll()`**，只能自己按周期轮询；不 `SNIOC_START` 读到的是全 0；
- `SNIOC_START` 后量程固定 ±16g（0.488 mg/LSB）、陀螺 ±2000dps（70 mdps/LSB）；
- `struct lsm6dsl_sensor_data_s`：`x/y/z_data` 单位是 **mg**、
  `g_x/y/z_data` 是 **mdps**、`temperature` 是摄氏度、
  `timestamp` **不是毫秒**（是传感器内部计数器被截成 16bit）；
- uORB 那条路在本构建里**连库都没编**（没有 `CONFIG_UORB`，
  `CONFIG_USENSOR` 也没开），别照 uORB 的例子写。

## 2. RTC + 定时 / 闹钟（给成员二的"定时问候 / 健康提醒"）

### 2.1 现状

- `/dev/rtc0` **会被建出来**。配置是齐的：
  `CONFIG_RTC=y`、`CONFIG_RTC_ALARM=y`、`CONFIG_RTC_DRIVER=y`
  （`defconfig:160-164`，`.config:839-846`），板级在启动时调
  `rtc_initialize(0, sf32lb_rtc_lowerhalf())`（`sifli_ap.c:381-397`）。
- **板子的 RTC 时间没被设置过**（`key-paths.md` 里也记了"心跳时间戳很怪"），
  所以读出来是 2000-01-01 之类的值。要做"每天 08:00"必须**先对时**：
  NSH 的 `date -s` 只认 `MMM DD HH:MM:SS YYYY` 格式
  （`apps/nshlib/nsh_timcmds.c`），写 `2026-09-12 15:30:00` 会报
  `argument invalid`。板上没装 RTC 备份电池，**每次掉电时间都会丢**。
- **下层驱动原先有两个真 bug，已修**（见 2.8 节；补丁在
  `patches/vendor_sifli-rtc-alarm-fix.patch`）：读时间不还原世纪位
  （写 2026 读回 **1926**）、以及 alarm 精确要求匹配"星期"字段。
  不修的话 `RTC_SET_RELATIVE` 会**返回成功但永远不触发** —— 这正是
  `hw_test rtc` 一开始 alarm 那条一直 FAIL 的原因。
- app 里现在的"提醒"只是存了个字符串 `touch_ui_add_reminder("Medicine","08:00")`，
  **没有任何定时触发**。

### 2.2 关键语义（源码核实，别按 Linux 直觉写）

| 问题 | 答案 | 出处 |
|------|------|------|
| `read(/dev/rtc0)` 能等 alarm 吗？ | **不能**。upper half 的 `rtc_read()` 直接 `return 0;`（EOF），立即返回 | `nuttx/drivers/timers/rtc.c:318-321` |
| 有 `poll()` 吗？ | **没有**，`g_rtc_fops` 里 poll 是 NULL | `rtc.c:136` |
| alarm 到点怎么通知？ | 用 **信号**：`ioctl` 参数里带 `struct sigevent`，upper half 在回调里 `nxsig_notification(pid, event, SI_QUEUE, ...)` | `rtc.c:190-200` |
| `pid` 填什么？ | **0 = 通知调用者自己** | `rtc.c:471-477` |
| 能同时有几个 alarm？ | **1 个**，`CONFIG_RTC_NALARMS=1`（`.config:842`），id 只能是 0 | `.config:842` |
| alarm 精度 | 秒（亚秒只比较高几位；同时屏蔽"星期"比较：`AlarmMask = RTC_ALRMDR_MSKWD \| (10 << RTC_ALRMDR_MSKSS_Pos)`） | `vendor/sifli/chips/sf32lb52/sf32lb_rtc.c:301` |
| 改时间会顺带改系统时钟吗？ | 会。`RTC_SET_TIME` 成功后内部调 `clock_synchronize(NULL)` | `rtc.c:395-411` |
| 时间字段格式 | 和 `struct tm` 完全一致（`tm_year` 从 1900 起，`tm_mon` 0..11） | `rtc.h:209-225` |

可用的 ioctl（都在 `nuttx/include/nuttx/timers/rtc.h`）：

| ioctl | 参数 | 作用 |
|-------|------|------|
| `RTC_RD_TIME`（`:123`） | `struct rtc_time *` | 读当前时间 |
| `RTC_SET_TIME`（`:131`） | `const struct rtc_time *` | 设时间（并同步系统时钟） |
| `RTC_HAVE_SET_TIME`（`:139`） | `bool *` | 时间是否设过 |
| `RTC_SET_ALARM`（`:147`） | `const struct rtc_setalarm_s *` | 设**绝对**时间 alarm |
| `RTC_SET_RELATIVE`（`:155`） | `const struct rtc_setrelative_s *` | 设**相对** N 秒后的 alarm |
| `RTC_RD_ALARM`（`:169`） | `struct rtc_rdalarm_s *` | 查询当前 alarm |
| `RTC_CANCEL_ALARM`（`:162`） | alarm id | 取消 alarm |

下层的具体实现（Sifli HAL 封装）在
`vendor/sifli/chips/sf32lb52/sf32lb_rtc.c`：
`rdtime` `:175`、`settime` `:207`、`setalarm` `:260`、
`setrelative` `:320`（内部就是"读当前时间 + N 秒"）、`cancelalarm` `:357`。

### 2.3 读 / 写当前时间（可直接抄）

```c
#include <nuttx/timers/rtc.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdio.h>

int fd = open("/dev/rtc0", O_RDONLY);
struct rtc_time rt;

/* ---- 读 ---- */
memset(&rt, 0, sizeof(rt));
if (ioctl(fd, RTC_RD_TIME, (unsigned long)&rt) == 0)
  {
    /* tm_year 是从 1900 起的年数；tm_mon 是 0..11，显示时都要 +1 */
    printf("%04d-%02d-%02d %02d:%02d:%02d\n",
           rt.tm_year + 1900, rt.tm_mon + 1, rt.tm_mday,
           rt.tm_hour, rt.tm_min, rt.tm_sec);
  }

/* ---- 写（对时）---- */
struct rtc_time set;
memset(&set, 0, sizeof(set));
set.tm_year = 2026 - 1900;
set.tm_mon  = 9 - 1;      /* 9 月 */
set.tm_mday = 12;
set.tm_hour = 15;
set.tm_min  = 30;
set.tm_sec  = 0;
set.tm_wday = 6;          /* 星期几，0=周日；驱动会写进 HAL，最好填对 */
ioctl(fd, RTC_SET_TIME, (unsigned long)&set);   /* 成功后系统时钟一起变 */

close(fd);
```

> NSH 里如果有 `date` 命令，`date -s "2026-09-12 15:30:00"` 更省事
> （走的是同一套 RTC_SET_TIME / `up_rtc_settime`）。
> 没有 RTC 备份电池的话，**每次掉电时间都会丢**，开机要重新对时
> （可以从网络拿时间：本项目已经有 MQTT/HTTP 了，拿个 NTP 或
> server 时间戳再 `RTC_SET_TIME` 即可）。

### 2.4 设一个 N 秒后的 alarm 并等它（可直接抄）

这是 `hw_test rtc` 用的完整流程，**带超时，不会永久卡住**：

```c
#include <nuttx/timers/rtc.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

static volatile sig_atomic_t g_alarm_fired;

static void alarm_handler(int signo)
{
  (void)signo;
  g_alarm_fired = 1;
}

static int rtc_alarm_demo(int seconds)
{
  struct rtc_setrelative_s rel;
  struct sigaction sa;
  int fd, waited_ms;

  fd = open("/dev/rtc0", O_RDONLY);
  if (fd < 0)
    {
      return -1;
    }

  /* 1) 装信号处理：alarm 到点会收到 SIGUSR1 */
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = alarm_handler;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGUSR1, &sa, NULL) < 0)
    {
      close(fd);
      return -1;
    }

  /* 2) 设 alarm：相对当前 RTC 时间 N 秒（更简单，不用自己算绝对时间） */
  g_alarm_fired = 0;
  memset(&rel, 0, sizeof(rel));
  rel.id                 = 0;              /* 只有一个 alarm，id 必须 0 */
  rel.pid                = 0;              /* 0 = 通知调用者自己 */
  rel.event.sigev_notify = SIGEV_SIGNAL;
  rel.event.sigev_signo  = SIGUSR1;
  rel.reltime            = seconds;

  if (ioctl(fd, RTC_SET_RELATIVE, (unsigned long)&rel) < 0)
    {
      close(fd);
      return -1;
    }

  /* 3) 带超时地等。**不要 read(fd, ...)** —— 那个是 EOF，立刻返回 */
  for (waited_ms = 0;
       !g_alarm_fired && waited_ms < (seconds + 2) * 1000;
       waited_ms += 50)
    {
      usleep(50 * 1000);
    }

  if (!g_alarm_fired)
    {
      ioctl(fd, RTC_CANCEL_ALARM, 0);
      close(fd);
      return -1;                             /* 超时：FAIL 退出，不卡死 */
    }

  close(fd);
  return 0;                                  /* alarm 到点 */
}
```

要设**绝对时间**（比如"今天 08:00"）就把第 2 步换成：

```c
struct rtc_setalarm_s abs;
struct rtc_time now;

memset(&now, 0, sizeof(now));
ioctl(fd, RTC_RD_TIME, (unsigned long)&now);   /* 先读当前时间 */

memset(&abs, 0, sizeof(abs));
abs.id                 = 0;
abs.pid                = 0;
abs.event.sigev_notify = SIGEV_SIGNAL;
abs.event.sigev_signo  = SIGUSR1;
abs.time               = now;                  /* 复制一份，只改时分秒 */
abs.time.tm_hour       = 8;
abs.time.tm_min        = 0;
abs.time.tm_sec        = 0;
abs.time.tm_wday       = 0;                    /* 驱动会写进 HAL，最好填对 */

ioctl(fd, RTC_SET_ALARM, (unsigned long)&abs);
```

### 2.5 「每天 08:00 提醒」怎么做

**关键限制：`CONFIG_RTC_NALARMS=1`（`.config:842`），只有一个 alarm 槽。**
所以不能"一次注册 7 个闹钟"，必须：

```
        启动 / 每次 alarm 触发
                 │
                 ▼
        读 RTC 当前时间 (RTC_RD_TIME)
                 │
                 ▼
        算出"下一个 08:00"的绝对时间
        （今天 08:00 已过就 +1 天；用 mktime/localtime 做日期进位）
                 │
                 ▼
        ioctl(RTC_SET_ALARM) + 装好的信号处理
                 │
        信号到点 / 或者距上次设置超过 N 秒的兜底重排
                 │
                 ▼
        回到第一步，重排"下一个 08:00"
```

建议做成一个**独立任务**（别和 LVGL 主循环混在一起）：

```c
/* 伪代码骨架，业务自己补 */
for (;;)
  {
    time_t now, next8;
    struct tm tm_now;
    struct rtc_time rt;

    ioctl(fd, RTC_RD_TIME, (unsigned long)&rt);
    if (rt.tm_year < 100)          /* RTC 没对时，先别排，等网络对时 */
      {
        sleep(60);
        continue;
      }

    now = mktime((struct tm *)&rt);          /* rtc_time 与 tm 布局兼容 */
    localtime_r(&now, &tm_now);
    tm_now.tm_hour = 8;
    tm_now.tm_min  = 0;
    tm_now.tm_sec  = 0;
    next8 = mktime(&tm_now);
    if (next8 <= now)                        /* 今天的 8 点已经过了 */
      {
        next8 += 24 * 3600;
      }

    /* 下一次 08:00 = next8；这里设 alarm（绝对时间），然后等信号 */
    /* ... RTC_SET_ALARM + 标志位等待（见 2.4）... */

    /* alarm 到点 -> 弹提醒/播报 -> 循环回来重排第二天的 */
  }
```

两个**必须处理**的点：
1. **RTC 没对时**时不要排 alarm：`tm_year` 是垃圾值，算出来的"08:00"没意义。
   先对时（2.3 节），或者干脆先不用 RTC 路线。
2. **重排必须可靠**：alarm 触发后要立刻排第二天的；如果中间因为别的原因
   （例如 `AUDIOIOC_STOP` 之类打断了你的等待），要用"距上次设置超过 24h+余量"
   兜底重排一次，否则提醒只触发一次就永远哑了。

### 2.6 不想用 RTC：POSIX 定时器也能做相对延时

如果只是"我说话后 5 秒提醒一下"这种**进程内的相对延时**，
完全不需要 RTC：

```c
#include <signal.h>
#include <time.h>

timer_t tid;
struct sigevent ev;
struct itimerspec its;

ev.sigev_notify = SIGEV_SIGNAL;
ev.sigev_signo  = SIGUSR1;
timer_create(CLOCK_MONOTONIC, &ev, &tid);

its.it_value.tv_sec     = 5;      /* 5 秒后触发 */
its.it_value.tv_nsec    = 0;
its.it_interval.tv_sec  = 0;      /* 0 = 只触发一次 */
its.it_interval.tv_nsec = 0;
timer_settime(tid, 0, &its, NULL);
```

**和 RTC alarm 的区别（选型用）**：

| | RTC alarm | POSIX `timer_create` |
|---|---|---|
| 时间基准 | 硬件 RTC（挂钟时间） | `CLOCK_MONOTONIC`（开机以来的时间） |
| 掉电/重启 | alarm 寄存器如果保持供电就还在 | **没了**，进程重启要重设 |
| 依赖系统时间被设置 | 需要（"每天 08:00"必须知道现在几点） | 不需要（相对延时） |
| 能不能当"绝对时刻"用 | 能（08:00） | 不能（只有相对） |
| 能不能唤醒系统 | 硬件 RTC 可以（Sifli RTC 有 alarm 中断） | 通常不能 |
| 多个并存 | 本板只有 1 个 | 可以很多个 |
| 复杂度 | 要处理信号 + 重排 | 简单 |

结论：**"每天 08:00 健康提醒"用 RTC**；**"3 秒后问候一下"用 POSIX 定时器
或直接 `usleep`**（后者最简单，但会占住线程）。

### 2.7 RTC 的坑

1. **`read(/dev/rtc0)` 不会等 alarm**，返回 0（EOF）。等 alarm 只能等信号
   （`rtc.c:318-321`、`rtc.c:136`）。`hw_test rtc` 就是踩过这个才改成信号的。
2. **本板只有 1 个 alarm**（`RTC_NALARMS=1`），设第二个会覆盖第一个。
3. **信号是"通知 pid"**，`pid=0` 是"调用者自己"。如果 alarm 是在 A 任务设的、
   但 A 任务已经退出，信号就没人收了。
4. **RTC 时间没设过**时读出来是垃圾值（`sf32lb_havesettime()` 返回的是
   lower half 的 `initialized`，`sf32lb_rtc.c:244-248`）。做时间逻辑前先
   `RTC_HAVE_SET_TIME` 或直接判 `tm_year`。
5. **`RTC_SET_TIME` 会顺带改系统时钟**（`clock_synchronize`，`rtc.c:409`），
   所以 `time(NULL)` / `CLOCK_REALTIME` 会跟着跳，别在动画/超时逻辑里依赖
   `CLOCK_REALTIME` 单调递增 —— 用 `CLOCK_MONOTONIC`。
6. 绝对 alarm 的**日期字段也参与匹配**（`sf32lb_rtc.c:279-282` 写了
   Date/Month/Year），跨天/跨月要算对，否则 alarm 可能不触发或立刻触发。
7. LVGL 不是线程安全的：**alarm 的提醒业务不要直接碰 LVGL 控件**，
   只置全局标志，让 UI 主循环去刷（同 `docs/network_api_usage.md` 第 4 节）。

### 2.8 下层驱动的两个坑（已修，值得记一笔）

**坑 1：读时间不还原世纪位 → 写 2026 读回 1926。**
`date_2_reg()`（`bf0_hal_rtc.c:374-377`）把 2000~2099 写成两位年 + `CB=0`、
1900~1999 写成两位年 + `CB=1`；`HAL_RTC_GetDate()`（`:484-490`）只对 19xx
打上 `RTC_CENTURY_BIT(0x80)`。而 `sf32lb_rdtime()` 原先把 `Year` **直接**
赋给 `tm_year`，于是 2026 变成 1926。修法与厂商 SDK 参考驱动
`drv_rtc.c:182-185` 一致：CB 置位取 `Year & ~RTC_CENTURY_BIT`，否则 `+100`。

**坑 2：`RTC_SET_RELATIVE` 是"假成功"。**
它内部用 `mktime()`/`localtime()`（`sf32lb_rtc.c:295` `add_timeout()`）把
"当前时间 + N 秒"算成绝对时间。本固件 `CONFIG_LIBC_LOCALTIME` **未开**，
所以 `localtime()` 就是 `gmtime()`、`mktime()` 就是 `timegm()`
（`nuttx/libs/libc/time/lib_gmtime.c:55`、`lib_timegm.c:234`），而 NuttX 这版
日历换算只支持 **1970 以后**（`lib_gmtimer.c`）。输入的年份一旦 <1970
（坑 1 造成的 1926，或者开机没对时的 2000 之前），就会算出**负的时/分/秒
和越界日期**，写进 `ALRMTR/ALRMDR` 的比较值硬件永远匹配不上
→ alarm 中断永远不来 → 永远收不到 SIGUSR1。ioctl 却返回 0。

**坑 3（连带）：alarm 要求精确匹配"星期"。**
`AlarmMask` 里 `MSKWD/MSKD/MSKM` 都为 0，表示这几个字段必须精确匹配，
而写进 `ALRMDR.WD` 的星期来自 libc 重算的 `tm_wday`，和硬件 `DR.WD`
不同源、编码还差一位。厂商 SDK 参考驱动是屏蔽 `MSKD|MSKM|MSKWD` 的。

三条都已修，补丁见 `patches/vendor_sifli-rtc-alarm-fix.patch`
（只改 `vendor/sifli/chips/sf32lb52/sf32lb_rtc.c`，不动 NuttX 公共代码）。

> 顺带纠正一个容易误读的地方：`AlarmMask` 里那个 `10 << MSKSS_Pos`
> 不是"把亚秒屏蔽掉"，原意是**提高亚秒匹配精度、避免一次 alarm 反复触发**
> （厂商 SDK 同款值）。另外 `bf0_hal_rtc.h` 里那组
> `RTC_ALARMSUBSECONDMASK_*` 是从 STM32 抄来的、位位置在 bit[27:24]，
> 而 SF32 的 `MSKSS` 在 bit[23:20]（`cmsis/sf32lb52x/rtc.h:252`），
> **那组宏在 SF32 上不能用**，只能自己 `x << RTC_ALRMDR_MSKSS_Pos`。

---

## 3. TF 卡 `/data/tf`：持久存储

- 板级在 SPI1 上初始化 TF 卡，**挂载点是 `/data/tf`，文件系统 vfat**：
  `sifli_ap.c:174-239`（`mkdir` 在 `:186`，`nx_mount(..., "vfat", ...)` 在 `:195`），
  挂载点宏 `SF32LB52_TFCARD_MOUNTPOINT` 在 `sifli_ap.c:111`。
- 相关配置：`CONFIG_MMCSD=y`（`defconfig:106`）、`CONFIG_SPI_DRIVER=y`、
  `CONFIG_BSP_USING_SPI1=y`（`defconfig:48,167,168`）。
- 引脚：TF 卡 = SPI1，`PA24/PA25/PA28/PA29`（`bsp_pinmux.c:183-186`）。

注意：

1. **`/data` 本身是 tmpfs**（`sifli_ap.c:366-379`），**重启就没了**。
   要掉电保存必须写 `/data/tf`。
2. **没插卡时 `/data/tf` 目录仍然存在**（`mkdir` 先执行了），只是 mount 失败。
   所以"能不能用"不要用 `stat("/data/tf")` 判断，要**真的写一个文件试试**：
   ```c
   FILE *fp = fopen("/data/tf/probe.txt", "w");
   if (fp == NULL) { /* 没插卡 / 卡不识别 / vfat 挂载失败 */ }
   else { fputs("ok\n", fp); fclose(fp); }
   ```
   启动日志里对应的是 `WARN: TF mount failed: ...`（`sifli_ap.c:235`）。
3. vfat **不保留 Unix 权限**，也别指望符号链接。
4. 录音文件用裸 PCM（`audio_test record <ms> <file>` 就是存裸 PCM，
   16k mono s16le）最省事；要 wav 自己在前面加 44 字节头。

---

## 4. 自检命令：`hw_test imu` / `hw_test rtc` / `hw_test audio`

这三个是**独立子命令**（不跑那套 5 步显示/触摸自检，直接跑自己）：

| 命令 | 做什么 | 会不会改硬件 |
|------|--------|-------------|
| `hw_test imu [帧数]` | 永远 FAIL：明确报"本板无 IMU 硬件"（模组 BOM 里没有加速度计） | 不碰硬件 |
| `hw_test rtc [秒]` | 读当前时间，设 N 秒后 alarm（默认 3），带超时等它触发 | 会设/取消 alarm（改 RTC 寄存器） |
| `hw_test audio [秒]` | 录 N 秒到内存（默认 2），打印 peak/avg 和是否检测到声音，不写文件 | 会开麦克风通路 |

**期望输出**（按代码逻辑推的；IMU 那步在当前固件上会是 FAIL，因为没有驱动）：

```
nsh> hw_test imu
[IMU] LSM6DS3 /dev/lsm6dsl0
      本板**没有加速度计/陀螺**：模组 SF32LB52-MOD-1 的 BOM 里
      只有 MCU + 128Mb NOR Flash + 晶振 + 天线，没有任何 IMU 器件。
      ...
      [FAIL] IMU 读数  (本板无 IMU 硬件)

nsh> hw_test rtc
[RTC] /dev/rtc0
      当前时间 : 2026-09-12 15:30:00
      [PASS] 读 RTC 时间
      已设 3 秒后的 alarm，最多等 5 秒...
      收到 SIGUSR1：alarm 触发了（约 3010 ms）
      [PASS] RTC alarm  (alarm 已触发)

nsh> hw_test audio
[AUDIO] /dev/audio/audio0 录音 2 秒
      read 返回 64000 字节（期望 64000）
      peak=1636 avg=43 (16k mono 16bit)
      声音检测 : 有声音
      [PASS] 录音  (64000 字节, peak=1636 有声音)
```

失败路径全部**只打 FAIL、正常退出**（超时也退出，不永久卡住）：
`hw_test rtc` 收不到信号会 `RTC_CANCEL_ALARM` 后 FAIL；
`hw_test audio` 的 `read` 不返回会发 `AUDIOIOC_STOP` 救场再 FAIL。

---

## 5. defconfig 注意事项

```conf
# ❌ 不要加这一条：本板没有 IMU 器件，而且那段板级 bringup 代码已经删除
# CONFIG_SENSORS_LSM6DSL=y
```

RTC 和音频**不需要改任何 defconfig**（`CONFIG_RTC*` / `CONFIG_AUDIO`
都已具备）。

顺带记两笔（不属于本次交付，但 `hw_test` 默认自检会报）：

- `CONFIG_PWM` 没开（`.config:833`），虽然 `CONFIG_BSP_USING_PWM=y`，
  但 `sifli_ap.c` 里 `#ifdef CONFIG_PWM` 不成立 → **`/dev/pwm0` 不会出现**，
  `hw_test` 节点枚举里 pwm0 那行永远是 `--`。要修就加 `CONFIG_PWM=y`。
- uORB 库没编（没有 `CONFIG_UORB`，`CONFIG_USENSOR` 也没开）。
  本板没有传感器，用不到它。
