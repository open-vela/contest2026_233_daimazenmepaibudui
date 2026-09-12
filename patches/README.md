# vendor_sifli 补丁

两个补丁按顺序应用(`git apply` 相对 `<openvela 工作区>/vendor/sifli` 目录):
先 `vendor_sifli-boot-fixes.patch`,再 `vendor_sifli-audio-driver.patch`。

## vendor_sifli-boot-fixes.patch

让 openvela 在 SF32LB52-DevKit-LCD 上可编译/可启动的 5 处上游修复。

### 修复内容

1. `chips/sf32lb52/sifli_uart.c` — `void up_putc()` 中删除非法的 `return ch;`
2. `chips/sf32lb52/sifli_irq.c` — 补充 `arm_lowprintf` 的 extern 声明(定义在 sifli_start.c)
3. `chips/sf32lb52/sf32lb_flash.c` — `HAL_FLASH_CONFIG_FULL_AHB_READ` → `HAL_FLASH_CONFIG_AHB_READ`(头文件中正确的函数名)
4. `boards/.../sf32lb52_devkit_lcd/src/sifli_ap.c` — 补 `sifli_i2cbus_initialize` extern 声明 + `<nuttx/i2c/i2c_master.h>`
5. `boards/.../sf32lb52_devkit_lcd/src/sifli_ap.c` — 删除错误的 `HAL_PIN_Set(PAD_PA37, I2C1_SCL, ...)`(DevKit-LCD 触摸 I2C1 SCL 应为 PA30,PA37 是 LCD 数据线;正确引脚已在 bsp_pinmux.c 配置)

## vendor_sifli-audio-driver.patch

SF32LB52-DevKit-LCD 音频驱动,注册 `/dev/audio0`(NuttX audio_lowerhalf),支持播放与录音。

### 内容

- 新增 `boards/.../sf32lb52_devkit_lcd/src/sf32lb52_audio.c` / `.h` — audio_lowerhalf 实现
  (codec 模拟通路 AUDCODEC + 数字通路 AUDPRC TX0/RX0 DMA + AW8155 功放 GPIO)
- `boards/.../src/CMakeLists.txt` — 加入音频源文件
- `boards/.../src/sifli_ap.c` — bringup 中调用 `sf32lb52_audio_initialize()`(`CONFIG_AUDIO`)

### 验证

- 板级配置: `CONFIG_AUDIO=y` 等(见 `board/contest_board/configs/sf32lb52_ai/defconfig`)
- 播放: `audio_test 3000 1000`(1 kHz 3 秒);录音: `audio_test record 3000`
- 此补丁基于 boot 补丁已应用的状态生成,顺序不可颠倒

## 应用方式

```bash
cd <openvela 工作区>/vendor/sifli
git apply patches/vendor_sifli-boot-fixes.patch
git apply patches/vendor_sifli-audio-driver.patch
```

## 说明

- 补丁修复/功能均为上游 vendor_sifli 缺口(开启 I2C/DEBUG 等配置后编译必现),建议以团队名义向 [open-vela/vendor_sifli](https://github.com/open-vela/vendor_sifli) 提交 PR
- 工作区中已直接应用了这些修复(未提交),补丁用于留存/提交/队友复现

---

# USB RNDIS 上网功能（2026-09-12 新增补丁）

板子通过 USB 线虚拟出一块网卡（RNDIS），再借 PC 的「Internet 连接共享(ICS)」上外网，
**不依赖 WiFi 模块**。这两个补丁是让它真正跑起来所必需的驱动侧改动，
按顺序应用（都已直接应用在工作区里，未提交）：

```bash
# 1) nuttx 侧：RNDIS 网卡驱动修复
cd <openvela 工作区>/nuttx
git apply <本仓库>/patches/nuttx-usbdev-rndis.patch

# 2) vendor_sifli 侧：USB device 控制器驱动修复 + 清理
cd <openvela 工作区>/vendor/sifli
git apply <本仓库>/patches/vendor_sifli-usb-rndis.patch
```

## nuttx-usbdev-rndis.patch（`nuttx/drivers/usbdev/rndis.c`）

修的是会让 **RX 永久停摆**的真 bug（现象：网卡 link up、主机能发包，但板子一个包都不回、
ping 不通）：

1. **`rndis_submit_rdreq()`** — 原来"`EP_SUBMIT()` 成功返回**之后**才置
   `rdreq_submitted = true`"。但 UDC 可能在 `EP_SUBMIT()` 内部就同步把请求完成掉并跑完
   完成回调（回调第一件事就是把该标志清掉），外层再置回 `true` 后，
   **标志说"读请求已挂"、实际队列是空的** → 之后每次提交都被跳过 → 再也没有读完成。
   改成**先置位、失败回滚**。
2. **`rndis_rxdispatch()`** — 结尾**无条件重挂读请求**。原来只有在"要回包"的路径
   （`rndis_freenetreq()` → `rndis_freewrreq()`）上才重挂，于是收到一个**不需要回复的包**
   （别人的 ARP、重复包、被协议栈丢掉的包）就再也不武装读请求 → RX 永久停摆。
3. **给 netdev 设 `d_mac`** — 原来 `d_mac` 全零，板子发出的帧源 MAC 是
   `00:00:00:00:00:00`（非法单播地址）。RNDIS 是点对点链路所以能凑合用，
   但帧本身是不合规的。
4. **`usbclass_setconfig()` 的"已配置"早退分支重新武装读请求** —
   总线复位会取消所有请求但 `priv->config` 还在，重新枚举时主机的
   SET_CONFIGURATION 走到这个早退分支，原来直接 return，导致 bulk OUT 管道
   整场没有读请求（换 USB 口后必现）。

## vendor_sifli-usb-rndis.patch（`vendor/sifli/chips/sf32lb52/sf32lb_usbdev.c`）

USB device 控制器驱动的修复与清理：

- **EP0 DATAEND 只在控制 IN 的末包置位** —— 否则 75 字节配置描述符只回 64 字节，
  Windows 报 "Configuration Descriptor Request Failed"，设备管理器里是 **Code 10**
- class OUT 数据阶段补 DATAEND；`SETUPEND` 不再吞掉 SETUP 包
- `sf32lb52_rdrequest()` 读前检查 `RXPKTRDY` —— 否则同一包会被读两次，
  上层 RNDIS 的重组状态（`current_rx_received/msglen`）被破坏
- `sf32lb52_epsubmit()` 的 OUT 分支**仅对非 EP0** 做硬件状态判断
  （对 EP0 也做会导致枚举直接失败）
- 删除全部 `up_putc` / `[usbdbg]` 调试打点，以及 EP1..7 的 txmaxp 探测循环
  （那个循环还会把 EP5 的 txmaxp 覆盖成 64）

> 注：`boards/.../src/sifli_ap.c` 里的 RNDIS bring-up 已经包含在
> `vendor_sifli-audio-driver.patch` 中（该补丁生成时就带上了 RNDIS）。
> 之后在这个文件上再改的只是删掉几行 `[usbdbg]` 调试打印（纯日志，不影响功能），
> 以及加了一个 `usleep` 给 app 自启让路。

## 板级配置（重要）

`board/contest_board/configs/sf32lb52_ai/defconfig` 目前只写了 `CONFIG_RNDIS=y`。
工作区里实际生效的 `.config` 还包含下面这些（在 cmake_out 的 `.config` 里手动配的，
改完必须 `touch nuttx/CMakeLists.txt` 再 `cmake --build`，否则 `config.h` 不会重新生成）：

```
CONFIG_NET=y
CONFIG_NET_ICMP_SOCKET=y                  # 让 net_test 的第 4 步能用
CONFIG_NET_ICMP_PREALLOC_CONNS=4
CONFIG_NET_ICMP_ALLOC_CONNS=0
CONFIG_NET_ICMP_NPOLLWAITERS=1
CONFIG_NETUTILS_PING=y                    # 注意：这个 OpenVela 树里拿不到 ping 命令,
                                          #       别浪费时间, 用 nslookup 验证联网即可
CONFIG_NET_ETH_PKTSIZE=1514               # 默认 590 → MTU 只有 576, 这里要 1500
CONFIG_NETDB_DNSSERVER_IPv4ADDR=0xc0a88901  # 192.168.137.1 (ICS 的 DNS 代理)
CONFIG_NETINIT_IPADDR=0xc0a88902            # 192.168.137.2
CONFIG_NETINIT_DRIPADDR=0xc0a88901          # 192.168.137.1 (ICS 固定网段)
```

网络地址是配合 Windows ICS 的固定网段（`192.168.137.x`）定的：
板子 `192.168.137.2`，PC 侧「以太网 2」由 ICS 自动配成 `192.168.137.1`。

> 复现这一步时如果换了 PC / 换了网络，只要 ICS 还在用，这套地址就不用改。

---

# 板级配置补齐 + hello_app 能编译进固件了（2026-09-12 晚）

## 背景：defconfig 之前是不完整的

工作区真正生效的 `.config` 是**手工维护**的，板上网要用的一堆配置 **defconfig 里一条都没有**。
后果：一旦重新 configure（`.config` 被从 defconfig 重建），**USB 网络功能会全部丢失**。

现在已补齐。用 NuttX 自带的 `savedefconfig` 从当前 `.config` 反推出最小配置，
只补了 **12 行**（其余都是默认值，不用写）：

```
# ==== USB 网络（RNDIS + Windows ICS 共享上网）====
CONFIG_DEBUG_USB=y
CONFIG_DEBUG_USB_ERROR=y
CONFIG_DEBUG_USB_WARN=y
CONFIG_DEBUG_USB_INFO=y
CONFIG_RNDIS_EPINTIN=5
CONFIG_NET_ETH_PKTSIZE=1514
CONFIG_NET_ICMP_SOCKET=y
CONFIG_NETDB_DNSSERVER_IPv4ADDR=0xc0a88901
CONFIG_NETINIT_IPADDR=0xc0a88902
CONFIG_NETINIT_DRIPADDR=0xc0a88901
CONFIG_NETUTILS_PING=y
CONFIG_AI_LLM_MODEL="gpt-3.5-turbo"
```

> ⚠️ 注意：`vendor/openvela/boards/contest2026_233_board` 是**软链接**指向
> `board/contest_board`，所以它们其实是同一个文件，改一处即可。

## 重新 configure 必须带 PYTHONPATH（否则报 `No module named 'olddefconfig'`）

`build_audio.sh` 只设了 `PATH`，**少了 PYTHONPATH**。一旦触发重新 configure 就会失败：

```bash
PB=/home/youdian/openvela/prebuilts/tools/python
export PATH="$PB/bin:/home/youdian/openvela/prebuilts/tools/linux/x86_64:$PATH"
export PYTHONPATH="$PB/dist-packages/kconfiglib"     # ← 注意是 kconfiglib 子目录
cmake --build cmake_out/contest2026_233_board_sf32lb52_ai
```

（已放在 `/home/youdian/build_full.sh`。）

## hello_app（ai_companion）：两个上游遗漏

队友把 app 的配置符号从 `LVX_USE_DEMO_CONTEST2026_000_HELLO_APP` 改名成
`LVX_USE_CONTEST2026_233_HELLO_APP`，但：

1. **defconfig 里没跟着改** → 符号不存在 → app 永远不编译。
   （另外注意 `CMakeLists.txt` 里的 `MODULE ${CONFIG_...}` 展开是 `MODULE y`，
   而**只有 `MODULE m` 才是可加载模块**，所以它其实就是普通内建 app，不用动。）
2. 它的代码调用 `velaclaw_client_open / velaclaw_ask / velaclaw_client_close`
   （来自 ai_agent），但 **ai_agent 的 `CMakeLists.txt` 从来没把
   `src/sdk/velaclaw_client_local.c` 编进去** —— 光加会链不上。
   → 已补，见 `patches/apps-ai-agent-velaclaw-sdk.patch`：

```bash
cd <openvela 工作区>/packages/ai_agent
git apply <本仓库>/patches/apps-ai-agent-velaclaw-sdk.patch
```

补齐后 hello_app 正常编入固件，内建命令名是 **`ai_companion`**
（由 `CONFIG_HELLO_APP_PROGNAME` 决定），不是 `hello_app`。

## 验证结果（全部通过）

```
config.h: CONFIG_NET_ETH_PKTSIZE 1514 / CONFIG_NETINIT_IPADDR 0xc0a88902
          CONFIG_NETINIT_DRIPADDR 0xc0a88901 / CONFIG_NET_ICMP_SOCKET 1
          CONFIG_NETUTILS_PING 1 / CONFIG_NETDB_DNSSERVER_IPv4ADDR 0xc0a88901
nm nuttx | grep companion_main   → 1   (已链入内核)
固件内建命令: ai_agent  ai_companion  net_test  robot_ui  zhi_ai
```
