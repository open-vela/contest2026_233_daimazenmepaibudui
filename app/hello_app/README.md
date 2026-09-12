# hello_app - AI陪伴系统核心模块

## 模块简介

AI陪伴系统的核心应用模块，实现多模态交互和智能看护功能。

## 目录结构

```
hello_app/
├── ai_companion_main.c    # 主程序入口，系统初始化
├── ai_llm.c/h            # 大语言模型接口，对接Xiaomi MiMo
├── ai_audio.c/h          # 音频处理，语音识别与合成
├── ai_care.c/h           # 智爱守护核心逻辑
├── ai_sound_detect.c/h   # 声音检测，异常声音识别
├── ai_state_machine.c/h  # 状态机管理，行为逻辑控制
├── ai_network.c/h        # 网络通信集成模块
├── Makefile              # NuttX构建配置
├── Kconfig               # 内核配置选项
└── CMakeLists.txt        # CMake构建配置
```

## 核心功能

### 1. AI语音交互
- 语音唤醒词检测
- 语音识别（ASR）
- 语音合成（TTS）
- 大语言模型对话

### 2. 智能看护
- 跌倒检测与报警
- 异常行为识别
- 睡眠质量监测
- 用药提醒

### 3. 情感计算
- 语音情感分析
- 情绪状态识别
- 个性化陪伴策略

### 4. 网络通信
- WiFi 连接管理
- MQTT 云端通信
- 设备状态上报
- 异常声音报警
- 主动关怀提醒
- 健康数据上报
- 手机推送通知

## 编译配置

在NuttX配置中启用：
```
CONFIG_HELLO_APP=y
CONFIG_AI_COMPANION=y
CONFIG_AI_AUDIO=y
CONFIG_AI_LLM=y
CONFIG_AI_NETWORK=y
```

## 依赖模块

- `nuttx/drivers/audio/` - 音频驱动
- `nuttx/drivers/sensors/` - 传感器驱动
- `vendor/sifli/` - SF32LB52 HAL库
- `contest2026_233_daimazenmepaibudui/app/robot_ui/` - 网络通信模块

## 网络配置

在 `ai_companion_main.c` 中修改以下配置：

```c
/* WiFi 配置 */
strncpy(net_cfg.wifi_ssid, "YourWiFiSSID", sizeof(net_cfg.wifi_ssid) - 1);
strncpy(net_cfg.wifi_password, "YourWiFiPassword", sizeof(net_cfg.wifi_password) - 1);

/* MQTT 配置 */
strncpy(net_cfg.mqtt_broker, "broker.emqx.io", sizeof(net_cfg.mqtt_broker) - 1);
net_cfg.mqtt_port = 1883;
strncpy(net_cfg.mqtt_client_id, "zhi_ai_001", sizeof(net_cfg.mqtt_client_id) - 1);
```

## MQTT 主题

| 主题 | 说明 |
|------|------|
| `zhi_ai/{client_id}/command` | 接收云端命令 |
| `zhi_ai/{client_id}/response` | 发送命令响应 |
| `zhi_ai/{client_id}/status` | 上报设备状态 |
| `zhi_ai/{client_id}/voice` | 发送语音数据 |
| `zhi_ai/{client_id}/chat` | 发送聊天消息 |
| `zhi_ai/{client_id}/alarm` | 上报报警信息 |
| `zhi_ai/{client_id}/health` | 上报健康数据 |
| `zhi_ai/{client_id}/reminder` | 发送关怀提醒 |
| `zhi_ai/{client_id}/sound_alarm` | 上报异常声音 |

## SF32LB52-DevKit-LCD 板级配置

本应用支持 SF32LB52-DevKit-LCD 开发板，配置文件位于：
`vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/configs/hello_app/defconfig`

### GPIO 引脚分配

| 功能 | 引脚 | 说明 |
|------|------|------|
| 麦克风 | PA37 | 音频输入 |
| 音频 PA 使能 | PA10 | 扬声器功放控制 |
| 触摸屏 SCL | PA30 | I2C1 时钟 |
| 触摸屏 SDA | PA33 | I2C1 数据 |
| 触摸屏中断 | PA31 | 触摸事件中断 |
| 触摸屏复位 | PA09 | 触摸屏复位 |
| LCD CS | PA03 | QSPI 片选 |
| LCD CLK | PA04 | QSPI 时钟 |
| LCD D0-D3 | PA05-PA08 | QSPI 数据 |
| LCD 复位 | PA00 | 显示屏复位 |
| LCD 背光 | PA01 | PWM 背光控制 |
| RGB LED | PA32 | 状态指示灯 |
| Key2 | PA11 | 用户按键 |

### 编译配置

```bash
# 选择 hello_app 配置
./tools/build.sh vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd:hello_app

# 或使用 menuconfig
make menuconfig  # 启用 CONFIG_LVX_USE_DEMO_CONTEST2026_000_HELLO_APP
```

### 网络配置

编辑 `ai_network_config.h` 文件，配置 WiFi 和 MQTT 服务器信息：

```c
#define AI_DEFAULT_WIFI_SSID      "YourWiFiSSID"
#define AI_DEFAULT_WIFI_PASSWORD  "YourWiFiPassword"
#define AI_DEFAULT_MQTT_BROKER    "broker.emqx.io"
#define AI_DEFAULT_MQTT_PORT      1883
```

### 板级配置文件

- `board_ai_config.h` - 硬件引脚和参数配置
- `ai_network_config.h` - 网络配置模板
- `configs/hello_app/defconfig` - NuttX 内核配置
