# tools/ —— 板子之外的配套工具（PC / 主机侧）

这些是**板子之外**那部分：屏幕镜像的 PC 端、不需要板子的协议测试、以及这台 PC 的
环境恢复脚本。板端对应实现都在仓库里（`board/contest_board/src/lcd_mirror.c`、
`app/robot_ui/lcd_mirror_glue.c`），补丁见 `patches/README.md`。

```
tools/
├── lcd_mirror/                屏幕镜像（PC 端）
│   ├── lcd_mirror.py              主界面：TCP 或 --serial COMx
│   ├── lcd_mirror_dump.py         无头抓帧（TCP）
│   ├── lcdmirror_serial_dump.py   无头抓帧（串口）+ 可选注入点按
│   ├── mirror_tap.py              在面板坐标注入一次点按（自动化用）
│   ├── mirror_latency.py          量"点一下 → 画面变化"的端到端延迟
│   └── host_test/                 协议测试（**不需要板子**）
└── pc_env/                    这台 PC 的环境/串口工具（Windows）
    ├── fix_net.cmd / netnat_on.cmd / remove_rndis.cmd / usb_nosleep.cmd / ics_on.cmd
    └── cmd_cap.py / raw_cap.py / cmd_seq.py / serial_term.py / diag_query.py
```

## 一、屏幕镜像怎么用

板端两条传输腿（`hw_test lcdmirror uart|tcp|stop|status`）：

| 腿 | 什么时候用 | 说明 |
|---|---|---|
| **TCP（默认）** | 网络正常时 | 走 USB RNDIS，**不碰控制台**；板子开机默认就是这条 |
| **串口** | 网络坏了/换地方时 | 走 CH343 的 `/dev/console`（1 Mbaud）；**会和控制台日志抢同一条线**，所以只当备胎 |

PC 端：

```bash
# 网络那条腿（推荐）
py -3.10 tools/lcd_mirror/lcd_mirror.py --scale 1.5

# 串口那条腿（不依赖网络；起来时脚本自己发 `hw_test lcdmirror uart`，
# 退出时自动 `hw_test lcdmirror stop`，不会把控制台淹死）
py -3.10 tools/lcd_mirror/lcd_mirror.py --serial COM4 --scale 1.5
```

窗口里：**在画布上按/拖 = 板子的触摸**（串口模式下它发的是
`hw_test lcdtap <x> <y> <0|1>` 文本命令）；`r` = 请板子整屏重发一次（画面出现旧
条纹时用）；`Esc`/关窗退出。

实测（1 Mbaud 串口那条腿）：**点一下 → 画面变化 ≈ 9 ms**；整屏切换 ≈1~1.5 秒
（95 KB 压缩数据 ÷ 64 KB/s，是这条线的物理上限）。

### 协议（板端 `lcd_mirror.h` 是权威定义）

```
头 20 字节 LE：'L','M', ver=2, flags(bit0=1 表示载荷是 RLE)
               u16 x, u16 y, u16 w, u16 h, u32 seq, u32 payload_len
载荷：flags bit0=1 → RLE 流（重复 {u8 run(1..255), u16 pixel_le}，run 之和 = w*h）
      flags bit0=0 → 原样 w*h*2 字节 RGB565 小端
一帧最多 30 行（TCP）/ 8 行（串口，避免大帧被 PC 侧串口缓冲截断）
```

串口那条腿上帧字节会和 `nsh`/内核日志交错，PC 端靠扫 `'L','M'` 重同步、坏帧丢掉，
板端每 30 秒兜底发一次整屏；PC 端发现坏带子会发
`hw_test lcdmirror resend <y0> <rows>` 精确补发。想看串口模式下的**控制台文本**，
给 PC 端加 `--raw-log out.bin`（顺带把原始字节流落盘，grep 得到板子的日志）。

## 二、协议测试（不需要板子）

```bash
cd tools/lcd_mirror/host_test
bash run.sh          # 编译"真板级源码 + 主机桩"，跑 TCP 那条腿，产出 frames*.bin
bash run_uart.sh     # 串口那条腿（同一个 lcd_mirror.c 的串口分支）
# 再在 Windows 侧校验（用 lcd_mirror.py 里同一套解码器逐像素比）
py -3.10 check_frames.py frames.bin frames_after_reconnect.bin touch_sent.txt c_test_output.txt
py -3.10 check_mapping.py
py -3.10 check_serial_resync.py        # 串口重同步/坏帧隔离（纯 Python，不用编 C）
```

判据：`check_frames.py` 出 `PASS: 全屏逐像素一致 + v2 头部/RLE 载荷（两条路都验过）
+ 断线自动重连 + 重连后整屏重发 + 反向通道触摸`。

## 三、这台 PC 的环境脚本（Windows，多数要**管理员**）

背景：板子上网 = `板子 eth0 192.168.137.2 ─USB RNDIS─ PC 以太网 2 ─NAT─ PC WLAN`。
这条链上任何一环变了都要按下面的表处理。**规范做法：用 Windows 自带 NetNat（纯 IP
转发），不用 ICS**（ICS 的 DNS 代理反复失效）；板子固件里 DNS 直接写 **223.5.5.5**。

| 症状 | 原因 | 跑哪个 |
|---|---|---|
| `以太网 2` 在设备管理器里 Code 10 / `CM_PROB_FAILED_START` | RNDIS 实例卡在失败态 | `remove_rndis.cmd`（管理员）→ 拔 USB → **板子断电重上电** → 等固件起来 → 最后插 USB |
| 板子有 IP、DNS 超时 / ping 不通 / 板子 `MQTT DNS 解析失败` | NAT 绑定丢了（换 WiFi、拔插、重建网卡都会） | `netnat_on.cmd`（管理员：关 ICS、静态 `192.168.137.1/24`、建 `ZhiAi` NetNat） |
| 板子→PC 被挡（镜像 `连上过 0 次`） | 网卡掉回 Public 防火墙配置 | `fix_net.cmd`（管理员：设 Private + 按网段放行 + **关该网卡 IPv6**） |
| USB 网卡睡死 / Code 10 反复 | Windows 省电把设备关了 | `usb_nosleep.cmd`（管理员，写 `PnPCapabilities=0x18`） |
| 只想把 ICS 共享重新绑上（旧方案，留作备选） | — | `ics_on.cmd`（管理员） |

判据永远这三条：`Get-PnpDevice` 里两个 `VID_584E` 都 OK + `192.168.137.1` 在 +
`ping 192.168.137.2` 通。完整排查顺序与今晚踩过的坑见
`docs/`（板子侧）与仓库外那份 `key-paths.md`。

### 串口工具（1 Mbaud，Windows 侧）

| 脚本 | 干嘛 |
|---|---|
| `cmd_cap.py <秒> "<命令>" [文件]` | 发一条命令并抓输出。**发前读到安静才发、发完读到安静才关端口**（关端口会让板子 `nsh` 的写阻塞、命令积压）；中间会静默几十秒的命令（如 `net_test`）加 `CAP_QUIET_AFTER=8` |
| `raw_cap.py <秒>` | 纯被动抓（固定时长） |
| `cmd_seq.py <间隔> <文件> "命令"…` | 依次发多条 |
| `serial_term.py [COM] [波特率]` | **带窗口的串口终端**（敲命令、Ctrl-C、清屏） |
| `diag_query.py [broker]` | **不占串口**查板子状态（走 MQTT 的 `{"action":"diag"}`） |

## 四、相关文档

- 板级用法/踩坑：`docs/display_touch_gpio_usage.md`、`docs/audio_driver_usage.md`
- 上游树补丁（含串口 RX 被 DMAC1 复位抹掉那条）：`patches/README.md`
- 屏幕镜像板端设计：`board/contest_board/src/lcd_mirror.h` 的文件头
