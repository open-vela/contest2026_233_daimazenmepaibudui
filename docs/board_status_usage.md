# 统一外设状态查询：`board_status_get()` / `board_status_dump()`

对应代码：`board/contest_board/src/sf32lb52_status.h` / `.c`
（板级模块，头文件路径对 app 可见：`app/*/CMakeLists.txt` 里
`INCLUDE_DIRECTORIES ${NUTTX_BOARD_ABS_DIR}/src` 就有了，直接
`#include "sf32lb52_status.h"` 即可。）

## 1. 它是干什么的 / 为什么要有

以前每个模块各查各的：UI 自己 `socket()` + `ioctl(SIOCGIF*)` 查网卡、
别处又 `open("/dev/lcd0")` 试一下、再有人 `stat("/etc/assets")`……
同一件事好几份实现，查出来的口径还不一样。

现在统一成一个**只读快照**：

```c
struct board_status_s st;

board_status_get(&st);        /* 不需要先 init，随调随用 */

if (st.net_link_up)
  {
    printf("IP = %s\n", st.net_ip);
  }
```

也可以直接打一份人看的清单（`hw_test status` 用的就是它）：

```c
board_status_dump();
```

```
外设状态（board_status_get 快照）
  网络        : 已获取 IPv4 192.168.137.2
  MQTT        : 未连接（由使用方 board_status_set_mqtt() 喂）
  ROM 素材    : /etc/assets 可读
  /data       : 可写
  音频播放    : /dev/audio/audio0 可打开
  音频录音    : /dev/audio/audio0 可打开
  显示        : /dev/lcd0 可打开
  触摸        : /dev/input0 可打开
  按键 PA11   : 松开
  RTC         : 有效 2026-09-13 12:34:56
  运行时间    : 123 s
```

`board_status_dump()` 是往**调用者的 stdout** 打的（`printf`），
所以它在 NSH 里跑就打在串口上，在别的任务里跑就打在别的任务的标准输出上。

## 2. 接口

### 2.1 字段

| 字段 | 类型 | 含义 | 怎么探的 |
|------|------|------|----------|
| `net_link_up` | bool | 有没有**非回环** IPv4 地址 | `socket()` + `ioctl(SIOCGIFCONF)` 枚举 netdev，跳过 `lo` |
| `net_ip` | char[16] | 例如 `"192.168.137.2"`；没地址时空串 | 同上，取第一个非回环接口的地址 |
| `mqtt_connected` | bool | MQTT 连上没 | **由使用方喂**，见 2.3 |
| `rom_assets_ready` | bool | `/etc/assets` 能不能读到（ROMFS 挂上没） | `stat()` + `S_ISDIR` |
| `data_ready` | bool | `/data` 能不能写（tmpfs 挂上没） | 真写一个 0 字节探针文件，立刻删掉 |
| `audio_play_ready` | bool | `/dev/audio/audio0` 能不能按播放方向打开 | `open(O_WRONLY)` + 立刻 `close()` |
| `audio_rec_ready` | bool | 同上，录音方向 | `open(O_RDONLY)` + 立刻 `close()` |
| `lcd_ready` | bool | `/dev/lcd0` 能打开 | `open(O_RDONLY)` + 立刻 `close()` |
| `touch_ready` | bool | `/dev/input0` 能打开 | `open(O_RDONLY\|O_NONBLOCK)` + `close()` |
| `btn_key_pressed` | bool | PA11(KEY) 此刻按着没 | 板级按键接口 `board_btn_is_pressed()`（不用先 init） |
| `rtc_valid` | bool | RTC 年份 >= 2000 | `ioctl(RTC_RD_TIME)`；板子没有备份电池，掉电后读出来是 1970/1990 |
| `uptime_s` | uint32 | 开机到现在几秒 | `clock_systime_ticks()` 换算 |

> `audio_play_ready` / `audio_rec_ready` 指的是"**以播放/录音方向能不能打开**同一个
> 节点"—— 本板播放和录音共用 `/dev/audio/audio0`，驱动在 `open()` 时不校验方向
> （`sf32lb52_audio_reserve()` 是空实现），所以两个字段通常同时为真。
> 它只说明"音频子系统在"，不代表此刻没被别的模块占着。

### 2.2 函数

| 函数 | 说明 |
|------|------|
| `int board_status_get(FAR struct board_status_s *out)` | 探一遍所有外设填进 `out`（先 `memset` 清零）。**返回 OK 只表示"快照填好了"**，字段全 false 也是 OK；只有 `out == NULL` 才返回 `-EINVAL` |
| `int board_status_dump(void)` | 内部 `get` 一遍再逐行 `printf` |
| `void board_status_set_mqtt(bool connected)` | 使用方喂 MQTT 连接状态（下一次 `get` 就能读到） |

**不需要先 init，也没有 init 函数**：`board_status_get()` 是"随调随用"的只读探测。

## 3. 照抄示例：UI 每 200ms 拉一次状态刷状态栏

要点：**状态采集和刷界面都放在 LVGL 自己的定时器里**（也就是跑在创建界面的那个
线程里）。这样既不用跨线程调 `lv_*`（LVGL 不是线程安全的），也不用担心别的任务
阻塞 LVGL。现成的例子是 `robot_ui/main.c` 里那个 `net_tick` 定时器。

```c
#include "sf32lb52_status.h"
#include <lvgl/lvgl.h>

#define STATUS_POLL_MS 200      /* 状态栏刷新周期 */

static lv_obj_t *g_lbl_net;     /* 状态栏上的网络标签 */
static lv_obj_t *g_lbl_dev;     /* 状态栏上的设备标签 */

static void status_tick(lv_timer_t *t)
{
  struct board_status_s st;
  char buf[64];

  (void)t;

  if (board_status_get(&st) < 0)
    {
      return;                                        /* 探不到就保持上一次显示 */
    }

  /* 网络：MQTT 连上算"通"，只有 IP 没有 MQTT 也要标出来 */

  if (st.mqtt_connected)
    {
      lv_label_set_text(g_lbl_net, "NET OK");
    }
  else
    {
      lv_label_set_text(g_lbl_net, st.net_link_up ? "NET IP" : "NET --");
    }

  /* 设备：一句话把要紧的几个拼起来 */

  snprintf(buf, sizeof(buf), "LCD%s TOU%s AUD%s RTC%s",
           st.lcd_ready        ? "✓" : "×",
           st.touch_ready      ? "✓" : "×",
           st.audio_play_ready ? "✓" : "×",
           st.rtc_valid        ? "✓" : "×");
  lv_label_set_text(g_lbl_dev, buf);
}

void my_ui_init(void)
{
  /* ... 建好 g_lbl_net / g_lbl_dev ... */

  /* lv_timer 的回调在 lv_timer_handler() 里跑，也就是建 UI 的那个线程 */
  lv_timer_create(status_tick, STATUS_POLL_MS, NULL);
}
```

不想用 LVGL、只是自己一个任务周期性地看一眼，也一样：

```c
for (;;)
  {
    struct board_status_s st;

    if (board_status_get(&st) == OK && !st.net_link_up)
      {
        syslog(LOG_WARNING, "STATUS: no IPv4 address yet\n");
      }

    sleep(2);
  }
```

## 4. MQTT 状态怎么喂进来

板级模块**不认识** `app/robot_ui` 的 MQTT 客户端（不能反向 include 应用头文件），
所以 `mqtt_connected` 只能由使用方"喂"：

```c
#include "sf32lb52_status.h"

/* 在 MQTT 连上/断开的那两个分支里各调一次（例如 network_comm.c 里
 * socket 状态变化的地方） */
board_status_set_mqtt(true);    /* CONNACK 成功 / 重连成功 */
board_status_set_mqtt(false);   /* 断开 / 心跳超时 */
```

没喂过时这个字段是 `false`（不会猜）。喂进去的值一直保留到下次被改写。

## 5. 自检：`hw_test status`

```
nsh> hw_test status
```

判定规则（和默认自检里 `optional` 节点的思路一致）：

- **必需**只有一条：**网络拿到非回环 IPv4 地址**，没拿到就是
  `[FAIL] 网络已获取 IPv4 地址`。板子的 MQTT / AI 全链路都靠它，这是真故障。
- 其它（`/etc/assets`、`/data`、音频、LCD、触摸、RTC、MQTT、按键）**只提示**：
  打印成 `[提示] ...（不计入判定）`，**不算 FAIL**。理由和 `eth0` 未枚举、
  `CONFIG_PWM=n` 一样 —— 可能是配置差异，也可能正被别的模块占着，拿来判死刑会误报。

## 6. 坑

1. **它只做轻量探测，什么也不初始化**：`open()` 一下立刻 `close()`、`stat()`、
   读一次 RTC。**绝对不要**在这里加 `AUDIOIOC_CONFIGURE` / 挂载 / 复位之类的动作 ——
   这些设备（`/dev/lcd0` 被 LVGL 开着、音频被录音/报警开着）平时就有主，
   你去 configure 一下会把别人正在用的东西搅乱。
2. **不在中断里调**：里面有 `open` / `ioctl` / `socket`。任务里调没问题。
3. **`/data` 的探测会真的写文件**（`/.status_probe`，写完立刻 `unlink`）：
   因为这一版 NuttX 的 `access()` 对目录永远返回 0（"NuttX 里没有用户，
   永远成功"），只有真写一下才知道能不能写。`/data` 是 tmpfs，不写 flash。
4. **别高频狂调**：一次 `get` 有 4 次 `open/close` + 1 次 `socket` + 1 次写探针 +
   1 次 RTC ioctl。UI 状态栏 **1~2 秒一次**足够；真要 200ms 刷（上面的示例），
   如果发现它拖慢了 UI，就把周期放宽，或者只取便宜的几个字段
   （`btn_key_pressed` / `uptime_s` / `rtc_valid` 基本不要钱，
   `net_link_up` 和 `/data` 那两条最贵）。
5. **`board_status_set_mqtt()` 要在断开的地方也调**：只在连上时置 true、
   断开时不置回 false，状态栏会一直显示"已连接"。
6. **它不判断"设备此刻归谁用"**：`lcd_ready = true` 只说明能打开，
   不代表你可以直接往上画（`/dev/lcd0` 的"当前画面"是 LVGL 的，见
   `docs/display_touch_gpio_usage.md` 第 9 节第 6 条）。
