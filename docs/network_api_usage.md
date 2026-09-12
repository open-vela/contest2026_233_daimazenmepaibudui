# 网络接口使用说明（`network_comm`）

> 面向队友：**怎么用**这些接口，以及**哪些坑不能踩**。
> 实现都在 `app/robot_ui/network_comm.c`，声明在 `network_comm.h`。

## 0. 板子是怎么上网的

板子用一根 USB 线虚拟出一块网卡（RNDIS），再借 Windows 的「Internet 连接共享(ICS)」出外网。
**不需要 WiFi 模块，也没有 WiFi 模块**（`wifi_connect()` 只是把状态置真，用来触发后续流程）。

```
板子 eth0 192.168.137.2  ──USB(RNDIS)──  PC「以太网 2」192.168.137.1  ──ICS NAT──  WiFi  ──> 外网
```

PC 侧的两步手动设置（每台电脑配一次）见项目 README「四、运行方式 → 6. 板子上网」。

## 1. 你基本什么都不用做

`main.c` 里已经按顺序做好了：

```c
network_comm_init();                        // 初始化；内部会创建 network_task
wifi_connect("魔王城", "sjmbahczdszjj");      // 触发网络流程
task_create("net_task", 100, 12288, (main_t)network_task, NULL);   // 后台网络任务
```

`network_task` 会自己：连 MQTT → 订阅命令主题 → 收消息 → 每 30 秒发心跳 → 断了自动重连。
**队友不要重复创建它。**

想加自己的功能，直接用下面第 3 节的接口就行。

## 2. 判断"现在有没有网"

```c
#include "network_comm.h"

if (mqtt_is_connected()) {          // 最常用：MQTT 连上了就说明整条链路通
    // ...
}

wifi_is_connected();                // 占位值，别用来判断真实网络
bool net_ok = mqtt_is_connected();  // 推荐用这个
```

界面上也有可视指示：主界面状态栏的 **`NET OK`（绿）= 连上了，`NET --`（黄）= 没连上**。

## 3. 常用接口

### 3.1 主动上报（最简单，直接用）

| 函数 | 作用 | 主题 |
|------|------|------|
| `report_device_status(&status)` | 上报温度/湿度/电量/状态 | `zhi_ai/<id>/status` |
| `report_heartbeat()` | 心跳（网络任务已自动调） | `zhi_ai/<id>/heartbeat` |
| `report_alarm(type, details)` | 报警，如 `report_alarm("fall", "老人摔倒")` | `zhi_ai/<id>/alarm` |
| `report_abnormal_sound(type, conf)` | 异常声音，`type` 取 `scream`/`fall`/`knock`/`help` | `zhi_ai/<id>/sound_alarm` |
| `report_health_data(hr, spo2)` | 心率 / 血氧 | `zhi_ai/<id>/health` |
| `send_proactive_reminder(t, c)` | 主动关怀提醒 | `zhi_ai/<id>/reminder` |
| `send_device_command(id, cmd)` | 联动米家等设备（`cmd` 是 JSON 串） | `zhi_ai/<id>/device_cmd` |
| `send_command_response(id, ok, msg)` | 回复一条云端命令 | `zhi_ai/<id>/response` |

全部返回 `0` 成功 / `-1` 失败（**没连上时就是 -1，调用方自己判断**）。

```c
device_status_t st = { .temperature = 26.5f, .humidity = 60,
                       .battery_level = 88, .status = "normal" };
report_device_status(&st);

report_alarm("fall", "检测到摔倒，位置：客厅");
```

### 3.2 AI 交互

```c
ai_send_text("今天天气怎么样", my_reply_cb);      // 主题 zhi_ai/<id>/chat
ai_send_voice_data(buf, len, my_reply_cb);        // 主题 zhi_ai/<id>/voice
```

### 3.3 手机推送（走 HTTP，和 MQTT 是两条路）

```c
push_init(PUSH_SERVICE_PUSHPLUS, "<key>");        // main.c 里已初始化
push_send_notification("标题", "内容", "分组");
push_send_alarm("fall", "老人摔倒");
push_send_health_reminder("该吃药了", "记得服用降压药");
push_set_enabled(true/false);
push_is_enabled();
```
> 没配 key 或没开开关时，这些函数会打印 `push not enabled` 并返回 -1。

### 3.4 收云端消息

```c
network_set_mqtt_callback(my_mqtt_cb);            // void cb(const char *topic, const char *payload)
network_set_ai_command_callback(my_cmd_cb);       // void cb(const char *action, const char *param)
```
订阅的主题是 `zhi_ai/<client_id>/command`，`main.c` 里已经注册了一个解析 AI 命令的回调。

### 3.5 想自己发/订阅

```c
mqtt_publish("my/topic", "{\"x\":1}", 0 /*qos*/, false /*retain*/);
mqtt_subscribe("my/topic", 0);
mqtt_unsubscribe("my/topic");
```

## 4. ⚠️ 三条必须遵守的约定

1. **回调是在 `network_task` 里跑的**（不是新任务）。所以回调里**别阻塞、别做重活**
   （比如 sleep、等信号量、长时间运算）——会直接卡住收包和心跳。要走耗时逻辑就丢给别的任务。
2. **LVGL 不是线程安全的**。任何任务都**不要直接调 `lv_*` 控件函数**。
   需要根据网络状态刷新界面时，只更新一个全局标志，由 `main` 的主循环去刷
   （现成的做法：`NET OK / NET --` 就是这么实现的，见 `main.c` 主循环里那段 `net_tick`）。
3. **`network_task` 已由 `main` 创建**，不要再 `task_create` 一次。

## 5. 主题一览（`<id>` = `client_id`，当前是 `zhi_ai_001`）

| 方向 | Topic | 内容 |
|------|-------|------|
| 订阅 | `zhi_ai/<id>/command` | 云端下发命令（`action` + `param`） |
| 发布 | `zhi_ai/<id>/status` | 设备状态；同时作为遗嘱（LWT） |
| 发布 | `zhi_ai/<id>/heartbeat` | 心跳，30 秒一条 |
| 发布 | `zhi_ai/<id>/alarm` | 报警 |
| 发布 | `zhi_ai/<id>/sound_alarm` | 异常声音 |
| 发布 | `zhi_ai/<id>/health` | 心率/血氧 |
| 发布 | `zhi_ai/<id>/reminder` | 主动关怀 |
| 发布 | `zhi_ai/<id>/response` | 命令响应 |
| 发布 | `zhi_ai/<id>/chat` / `voice` | AI 对话 |
| 发布 | `zhi_ai/<id>/device_cmd` | 设备联动 |

## 6. 自检命令：`net_test`

网络不通时，**先跑这个**，它会告诉你断在哪一层。串口 NSH 里敲：

```
nsh> net_test
```

```
[1/4] 网卡 eth0 配置          IP / 网关
[2/4] DNS 解析 broker.emqx.io
[3/4] TCP 连接 :1883          非阻塞 connect + 5s 超时，不通也不会卡死
[4/4] MQTT CONNECT -> CONNACK 依次试 emqx / mosquitto / hivemq，任一通过即算过
```

## 7. 关于 broker（重要）

默认用 `broker.emqx.io`，连不上会**自动降级**到 `test.mosquitto.org` → `broker.hivemq.com`，
串口会打印 `MQTT broker 切换到 xxx`。列表在 `network_comm.c` 的 `g_mqtt_broker_list`。

**公共 broker 会限流**：实测 `broker.emqx.io` 在用**格式正确**的最小 CONNECT 单独测试时
也不回 CONNACK（「TCP 能连上、但立刻被对端关闭」），而 mosquitto / hivemq 正常。
这是**服务器侧**的问题，与板子无关 —— 同一时刻在 PC 上测（板子经 ICS 共享同一公网 IP）
结果一样。所以不要写"每 1~5 秒重连一次"的循环（`network_task` 已做退避）。

> 另外注意：`mqtt_send_connect()` 发完 CONNECT **不等 CONNACK**，只看 `send()` 返回值，
> 所以它打印 "MQTT connected" 是"乐观成功"。要确认真连上了，**在 PC 上订阅主题看有没有消息**
> （`_flash/mqtt_watch.py`），那才是硬证据。

## 8. 验证"板子真的发到云端了"（最硬的证据）

在 PC 上跑（板子经 ICS 共享同一公网出口）：

```
D:\apply\claw\_flash\watch_mqtt.bat test.mosquitto.org 120
```

它会订阅 `zhi_ai/#` 并实时打印板子发的每一条消息：

```
CONNACK: 20020000
SUBACK : 9003000100
listening on zhi_ai/# for 120s ...
[PUBLISH] zhi_ai/zhi_ai_001/heartbeat {"type":"heartbeat","device_id":"zhi_ai_001",...}   ← 每 30 秒一条
```

> 别直接敲 `python` —— 这台机器的 PATH 里 `python` 指向 `D:\iverilog\gtkwave\bin\python.exe`
> （一个坏的 MSYS 版本，会报 `ModuleNotFoundError: No module named 'encodings'`）。
> 用 `watch_mqtt.bat`（内部走 `py -3.10`），或者写全路径 `D:\应用\py\python.exe`。

## 9. 排错速查

| 现象 | 先查什么 |
|------|---------|
| 状态栏一直 `NET --` | 串口有没有 `MQTT connecting` / `MQTT CONNECT failed`；跑 `net_test` |
| `ping 192.168.137.2` 不通 | **烧录后有没有拔插一次原生 USB**（见 README） |
| 板子完全没有 IP | PC 侧「以太网 2」是不是"自动获得 IP"、ICS 共享有没有勾 |
| 收到消息但没反应 | 回调里是不是阻塞了（拖住了 `network_task`） |
| `report_*` 一直返回 -1 | `mqtt_is_connected()` 是 false，先解决连接 |
| `MQTT connected` 打印了但云端收不到 | 它**不等 CONNACK**，打印是"乐观成功"；用 `_flash/mqtt_watch.py` 在 PC 上订阅验证 |
