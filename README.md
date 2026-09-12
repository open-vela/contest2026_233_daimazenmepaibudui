# 智爱守护——基于OpenVeLA的多模态非接触式AI居家看护终端

## 一、作品简介

面向独居及高龄老人，依托SF32LB52-DevKit-LCD运行OpenVeLA系统打造的智能居家安全看护终端。联动大屏和米家生态，解决老人突发意外无反馈的居家痛点。

**核心功能：**
- 多模态AI陪伴：语音交互 + 视觉识别 + 情感计算
- 非接触式健康监测：毫米波雷达跌倒检测、睡眠监测
- 智能看护：异常行为识别、紧急呼叫、用药提醒
- 米家生态联动：智能家居控制、环境监测

## 二、选题方向

**AI 硬件产品创新**

基于 openvela + ai_agent，开发「能主动、会执行」的嵌入式 AI Agent 应用。

## 三、目录结构

```
contest2026_233_daimazenmepaibudui/
├── app/                          # 应用代码目录
│   ├── hello_app/                # AI陪伴系统核心模块
│   │   ├── ai_companion_main.c   # 主程序入口
│   │   ├── ai_llm.c/h           # 大语言模型接口
│   │   ├── ai_audio.c/h         # 音频处理模块
│   │   ├── ai_care.c/h          # 智爱守护核心逻辑
│   │   ├── ai_sound_detect.c/h  # 声音检测模块
│   │   └── ai_state_machine.c/h # 状态机管理
│   ├── robot_ui/                 # 机器人界面模块
│   └── zhi_ai/                   # 智爱应用模块
├── board/                        # 板级适配代码
│   └── contest_board/            # SF32LB52-DevKit-LCD适配
├── quickapp/                     # 快应用代码
│   └── hello_quickapp/           # 快应用示例
├── logs/                         # AI Coding 日志
│   └── gaoxiaoying0207/          # 开发者日志目录
├── nuttx/                        # OpenVeLA内核（通过repo sync获取）
├── vendor/                       # 厂商适配代码（通过repo sync获取）
├── apps/                         # 系统应用（通过repo sync获取）
└── README.md                     # 本文件
```

## 四、运行方式

### 1. 环境准备

```bash
# 安装依赖
sudo apt-get update
sudo apt-get install -y gcc-arm-none-eabi make

# 设置交叉编译工具链路径
export PATH=/path/to/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH
```

### 2. 配置工程

```bash
cd nuttx
./tools/configure.sh -l ../board/contest_board/configs/sf32lb52_ai
```

### 3. 编译固件

```bash
make -j$(nproc)
```

编译完成后生成：
- `nuttx` - ELF可执行文件
- `nuttx.bin` - 二进制固件（约706KB）

### 4. 烧录到开发板

使用SF32LB52专用烧录工具，将 `nuttx.bin` 烧录到 SF32LB52-DevKit-LCD 开发板。

### 5. 运行验证

- 开发板上电后自动启动AI陪伴系统
- 串口控制台可访问 NuttShell (NSH)
- 支持语音交互、触摸操作、LCD显示

### 6. 板子上网（USB RNDIS + Windows 网络共享）

板子通过 USB 线虚拟出一块网卡（RNDIS），再借 Windows 的「Internet 连接共享(ICS)」上外网，
**不依赖 WiFi 模块**。PC 侧有几步必须手动设置，漏一步就不通。

**① 让「以太网 2」自动获取 IP**

1. `Win+R` → 输入 `ncpa.cpl`
2. 右键 **「以太网 2」**（Remote NDIS Compatible Device）→ 属性
3. 双击「Internet 协议版本 4 (TCP/IPv4)」
4. 选「自动获得 IP 地址」+「自动获得 DNS 服务器地址」→ 确定

> ICS **无法接管手工设死静态 IP 的网卡**，所以这一步是必须的。

**② 开启网络共享**

1. 右键**你上网用的那个网卡**（WiFi/以太网，看它是否"已连接"）→ 属性 → 「共享」
2. 勾选「允许其他网络用户通过此计算机的 Internet 连接来连接」
3. 「家庭网络连接」下拉框选 **「以太网 2」** → 确定（弹 UAC 点"是"）
4. 生效后「以太网 2」的 IP 会自动变成 `192.168.137.1`

> ICS **不会自动重试**：若勾共享时「以太网 2」还是静态 IP，之后改成自动获取也不会生效，
> 必须**取消勾选 → 确定 → 再重新勾选**一次。

**③ 板子侧**（已固化在固件里，无需操作）

板子 `eth0` = `192.168.137.2`，网关与 DNS = `192.168.137.1`，由 `.config` 三项决定：

```
CONFIG_NETINIT_IPADDR=0xc0a88902          # 192.168.137.2
CONFIG_NETINIT_DRIPADDR=0xc0a88901        # 192.168.137.1
CONFIG_NETDB_DNSSERVER_IPv4ADDR=0xc0a88901
```

**④ 每次烧录后，必须物理拔插一次原生 USB 线**

烧录会让板子复位、USB 核回到地址 0 重新初始化，但 **Windows 不会因此重新枚举**，
主机仍停在旧枚举状态（旧地址、旧 data toggle）。表现是「网卡显示 Up / 200Mbps 已连接，
但 ping 不通、ARP 完全不回」，很容易误判成固件坏了。用下面命令可确认是否真的重新枚举过：

```powershell
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like '*VID_584E*' } |
  ForEach-Object { (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_LastArrivalDate).Data }
```

**⑤ 验证**

```powershell
ping 192.168.137.2
```

串口（1000000 8N1）里：

```
nsh> nslookup broker.emqx.io
Host: broker.emqx.io Addr: 34.243.217.54
```

> **怎么开串口交互终端**：跑 `_flash\nsh.bat`（默认 COM4）。
> 它带噪声过滤 —— Windows 会不停用 RNDIS OID 查询轮询网卡，每秒往串口刷好几行
> `usbclass_setup:` / `rndis_send_encapsulated_response:`，不过滤看不清命令输出。
> 里面敲 `net_test` / `ifconfig` / `ps`，`:raw` 看全部原始输出，`:q` 退出。
> （用 PuTTY 也行：COM4、**1000000** 8N1；注意串口的 RTS 控制板子供电负载开关。）

开机后 app 会自动 MQTT 连 `broker.emqx.io:1883`，串口可见：

```
network_task started
MQTT connecting: broker.emqx.io:1883
MQTT connected
Publish to zhi_ai/zhi_ai_001/heartbeat: {"type":"heartbeat",...}
```

**常见问题**

| 现象 | 原因 |
|------|------|
| 「以太网 2」一直是 `169.254.x.x` | 共享没勾；或勾的时候网卡是静态 IP，需取消再重勾 |
| 网卡 Up 但 ping 不通 | 烧录后没拔插 USB，主机枚举状态陈旧 |
| 串口完全没输出 / 板子像被按住 | 串口终端的 RTS 控制板子供电负载开关，见 `flash/README.md` |
| 命令不被执行 | 串口命令行必须以 `\r\n` 结尾，只发 `\r` 会被吞掉、多条命令被拼接 |

### 7. 网络自检命令 `net_test`

在串口 NSH 里直接敲：

```
nsh> net_test
```

它从底层到应用层逐级验证，任何一层断了都能看出断在哪：

```
[1/4] 网卡 eth0 配置          IP / 网关
[2/4] DNS 解析 broker.emqx.io
[3/4] TCP 连接 :1883          (非阻塞 connect + 5s 超时，不通也不会卡死)
[4/4] MQTT CONNECT -> CONNACK (依次试 emqx / mosquitto / hivemq，任一通过即算过)
```

**注意公共 MQTT broker 会限流**：实测 `broker.emqx.io` 在被高频重连后会
「TCP 能连上、但立刻被对端关闭、不给 CONNACK」。这是服务器侧限流，**与板子无关** ——
同一时间在 PC 上测（板子经 ICS 共享同一公网 IP）也是同样结果，而
`test.mosquitto.org` / `broker.hivemq.com` 都正常返回 CONNACK。

因此：

- `net_test` 第 4 步会自动轮询多个公共 broker；
- app 的 `network_task` 也带**退避 + broker 自动降级**：前 5 次 5 秒一次，
  失败多了退到 30 秒一次，连 5 次连不上就自动换下一台 broker，
  串口会打印 `MQTT broker 切换到 xxx`。

主界面状态栏还有一个网络指示：`NET OK`（绿）表示 MQTT 已连上，`NET --`（黄）表示未连上。

### 8. 队友怎么在自己的代码里用网络

接口怎么用、有哪些坑（回调在哪个任务里跑、LVGL 不能跨任务调），
见 **`docs/network_api_usage.md`**。

## 五、AI Coding 使用说明

### 1. 开发工具

本项目使用 **Claude Code** 进行AI辅助开发，全程记录对话日志。

### 2. AI协助环节

| 环节 | AI协助内容 | 效率提升 |
|------|-----------|---------|
| **需求分析** | 功能模块拆解、技术方案设计 | 节省30%设计时间 |
| **代码实现** | HAL驱动适配、NuttX系统集成 | 节省50%编码时间 |
| **调试优化** | 编译错误修复、性能优化 | 节省40%调试时间 |
| **文档编写** | 代码注释、README生成 | 节省60%文档时间 |

### 3. 关键技术突破

通过AI协作解决的核心问题：
- **HAL库集成**：修复SF32LB52芯片Make.defs，正确引入HAL源文件
- **SysTick驱动**：配置ARMv8M_SYSTICK，解决系统时钟初始化
- **LCD/触摸驱动**：适配bsp_lcd_tp.c，实现屏幕显示和触摸交互
- **内置应用系统**：恢复builtin注册机制，支持NSH命令行

### 4. 日志管理

AI对话日志自动归集到 `logs/` 目录，格式：
```
logs/<github_login>/<date>/<tool>__<session_id>.jsonl
```

提交时执行：
```bash
git add logs/
git commit -s -m "logs: sync AI sessions"
git push
```

---

**项目地址**: https://github.com/gaoxiaoying0207/contest2026_233_daimazenmepaibudui  
**开发者**: gaoxiaoying0207  
**开发板**: SF32LB52-DevKit-LCD  
**系统**: OpenVeLA (NuttX RTOS)
