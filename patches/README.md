# vendor_sifli 补丁

补丁按顺序应用(`git apply` 相对 `<openvela 工作区>/vendor/sifli` 目录):
先 `vendor_sifli-boot-fixes.patch`,再 `vendor_sifli-audio-driver.patch`、
`vendor_sifli-rtc-alarm-fix.patch`,最后 `vendor_sifli-lcd-brightness.patch`
(四者改的文件互不重叠,顺序只为可复现)。

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

## vendor_sifli-rtc-alarm-fix.patch（2026-09-12 新增，RTC alarm 不触发）

改 `chips/sf32lb52/sf32lb_rtc.c` 三处。**现象**：`hw_test rtc 3` 里
`RTC_SET_RELATIVE` 返回成功，但 3 秒后收不到 SIGUSR1；另外
`date -s "Sep 12 15:30:00 2026"` 之后 `RTC_RD_TIME` 读回来是 **1926**。

### 根因

1. **读路径不还原世纪位**（`:198` `rtctime->tm_year = rtc_date.Year;`）
   `date_2_reg()` 把 2000~2099 写成两位年 + CB=0、1900~1999 写成两位年 + CB=1
   （`bf0_hal_rtc.c:374-377`），而 `HAL_RTC_GetDate()` 只对 19xx 打上
   `RTC_CENTURY_BIT(0x80)`（`bf0_hal_rtc.c:484-490`）。驱动读回来直接赋给
   `tm_year`，于是 2026 变成 1926。修法与厂商 SDK 参考驱动
   `drv_rtc.c:182-185` 一致：CB 置位取低 7 位，否则 +100。
2. **`RTC_SET_RELATIVE` 拿这个年份去喂 libc，算出垃圾 alarm 值**
   本固件 `CONFIG_LIBC_LOCALTIME` 未开（`.config`），所以
   `localtime()` 就是 `gmtime()`、`mktime()` 就是 `timegm()`；而 NuttX 这版
   日历换算只支持 1970 以后（`nuttx/libs/libc/time/lib_gmtimer.c`）。
   `add_timeout()`（`sf32lb_rtc.c:295`）用 `mktime()`/`localtime()` 算绝对
   时间，输入是 1926 这种 <1970 的年份 → 得到负的 `time_t` → 拆出**负的
   时/分/秒和越界日期** → 写进 `ALRMTR/ALRMDR` 的比较值硬件永远匹配不上
   → 永远没有 alarm 中断，也就永远没有 SIGUSR1。ioctl 返回 0 是**假成功**。
3. **星期字段被要求精确匹配、但值不是从硬件来的**（`:301` / `:367`）
   `MSKWD/MSKD/MSKM` 为 0 表示"这几个字段必须精确匹配"，而写进
   `ALRMDR.WD` 的星期来自 `add_timeout()` 里 libc 重算的 `tm_wday`，和硬件
   `DR.WD` 不同源、编码还差一位。厂商 SDK 参考驱动是屏蔽
   `RTC_ALRMDR_MSKD|MSKM|MSKWD` 的（`drv_rtc.c:482-484`）。

### 修改

| 位置 | 改动 |
|------|------|
| `sf32lb_rdtime()` `:198` | 按 CB 还原世纪：CB 置位取 `Year & ~RTC_CENTURY_BIT`，否则 `Year + 100` |
| `sf32lb_rdalarm()` `:405` | 同上（保持 rd/set/rdalarm 一致） |
| `sf32lb_setalarm()` `:301`、`sf32lb_setrelative()` `:367` | `AlarmMask` 加上 `RTC_ALRMDR_MSKWD`（屏蔽星期比较） |

### 验证

- `date -s "Sep 12 15:30:00 2026"` 后 `hw_test rtc 3`：时间应读回 **2026-09-12**，
  并在约 3 秒后打印 `收到 SIGUSR1`，两项都 PASS
- 注意 NSH 的 `date -s` 只认 `MMM DD HH:MM:SS YYYY` 格式（`nsh_timcmds.c`），
  写 `2026-09-12 15:30:00` 会报 `argument invalid`

### 已知遗留

- `HAL_RTC_GetDate()` 在 CB=1 且年份 <70 时会**顺手清掉硬件 CB** 并返回不带
  0x80 的年份（`bf0_hal_rtc.c:485-490`），此时上面的还原会当 20xx 处理。
  这条分支只有写入 1900~1969 才会进，本项目的对时路径不会写这种年份，
  暂不处理。

## vendor_sifli-lcd-brightness.patch（2026-09-13 新增，屏幕百分比亮度）

改 `boards/sf32lb52/drivers/lcd/sf32lb_lcd.c`（行号都是**改前**的）。

### 现象

屏幕只能开/关，中间亮度够不着：`hw_test lcd 50` 判 FAIL，
`backlight_set(50) -> -38`（`-ENOSYS`），屏幕亮度和 100 时一模一样。

### 根因

1. **`LCDDEVIO_SETPOWER` 根本不是亮度**（`:634` `sf32lb_lcd_setpower()`）：
   实现是 `power > 0 ? DisplayOn : DisplayOff`，power 的数值只被存下来给
   `GETPOWER` 回读。传 30 和传 100 出来一样亮 —— 典型的"假亮度"。
2. **真正该干这事的 `LCDDEVIO_SETCONTRAST` 是死的**：`:697`
   `sf32lb_lcd_setcontrast()` 直接 `return -ENOSYS`，`:683` 的
   `GETCONTRAST` 同理。
3. **面板其实有这个能力**：CO5300 的 `0x51 WBRIGHT`，SDK 里已经写好
   `co5300.c:544` 的 `LCD_SetBrightness(hlcdc, br)`（**入参就是百分比 0..100**，
   内部换算成 0..255），挂在 `LCD_DrvOpsDef.SetBrightness` 回调上
   （`sf32lb_lcd.h:36`）—— 但 `sf32lb_lcd.c` 从来不调它。
   能力在，入口没接。

### 修改

| 位置 | 改动 |
|------|------|
| `struct sf32lb_lcd_dev_s` `:87` | 新增 `int contrast;`（亮度百分比 0..100） |
| `board_lcd_initialize()` `:923` | `memset` 之后置 `s_drv_lcd.contrast = SF32LB_LCD_BRIGHTNESS_MAX`（面板上电默认就是满值） |
| `sf32lb_lcd_setcontrast()` `:697` | 真实现：校验 `0..100` → `priv->p_drv_ops->p_ops->SetBrightness(&priv->hlcdc, percent)` → 记下值 |
| `sf32lb_lcd_getcontrast()` `:683` | 回 `priv->power > 0 ? priv->contrast : 0`（关屏按 0 报） |

三个注意点：

- `SetBrightness` 是**可选回调**，别的面板可能没实现：判空后返回 `-ENOSYS`，
  **不空指针调用**。本补丁不新增任何 Kconfig 符号。
- 参数按**亮度百分比 0..100** 解释，**不跟 `CONFIG_LCD_MAXCONTRAST`**
  （本板那个宏是 **63**，照它走会把 100% 挡在外面），所以用自己定义的
  `SF32LB_LCD_BRIGHTNESS_MAX` 做上界。面板只有 `WBRIGHT` 写口、没有回读通路
  （CO5300 的 `0x52 RBRIGHT` 在本驱动里没有读函数），`GETCONTRAST` 回的是
  “最近一次下发的值”。
- **`SETPOWER` 的语义一个字没改**：它仍然是开/关屏，别的地方
  （`sf32lb_lcd_ensure_display_on`、LVGL 那条路）还在依赖它。

### 验证

- 逐档下发：`hw_test lcd 0` / `hw_test lcd 30` / `hw_test lcd 60` /
  `hw_test lcd 100`，四档都应当 PASS，并且**屏幕亮度肉眼可见地变化**
  （0 = 关屏，100 = 全亮），`回读亮度` 等于设定值。
- 板级封装 `board/contest_board/src/sf32lb_backlight.c` 和 UI 亮度滑块
  （`app/robot_ui/touch_ui.c` 的 `setting_slider_event_handler`）已经接到这条
  通路上；用法与兼容性（没打补丁的树仍然只有 0/100）见
  `docs/display_touch_gpio_usage.md` 第 3.3 节。

## 应用方式

```bash
cd <openvela 工作区>/vendor/sifli
git apply patches/vendor_sifli-boot-fixes.patch
git apply patches/vendor_sifli-audio-driver.patch
git apply patches/vendor_sifli-rtc-alarm-fix.patch
git apply patches/vendor_sifli-lcd-brightness.patch
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

---

# ai_agent 的 LLM 计时/超时修复（2026-09-13 新增补丁）

## 应用方式

```bash
cd <openvela 工作区>/packages/ai_agent
git apply <本仓库>/patches/apps-ai-agent-llm-clock-fix.patch
```

> 注意目录是 **`packages/ai_agent`**（不是 `apps/...`；`apps/packages` 只是指向 `packages` 的软链接）。
> 补丁里的路径是 `src/core/agent_loop.c`，所以必须在 `packages/ai_agent/` 下 apply。

## apps-ai-agent-llm-clock-fix.patch（`packages/ai_agent/src/core/agent_loop.c`）

### 现象

`set_llm` 配好后端后执行 `ask`：**请求真的发出去、模型也真的回了**，但界面永远显示"请求超时"：

```
[llm] Response: 12 bytes text, 0 tool calls, finish=end_turn      ← 收到真实回复（12 字节 = "连接成功"）
[agent] LLM watchdog: call took 955862003 ms (limit 60s), treating as timeout   ← 耗时是脏值
[trace:...] END status=timeout ... llm_ms=955862003 elapsed=825589583s
```

### 根因（算术闭环，可复核）

1. `src/infra/vela_tls.c:255-259` 在**每次 TLS 握手**时检查墙钟，若 `< 2024-01-01` 就把它
   **硬跳到硬编码常量 `1772275200`**：
   ```c
   if (now < 1704067200) {
       syslog(LOG_WARNING, "[%s] Clock too old, forcing to 2026\n", TAG);
       struct timespec ts = { .tv_sec = 1772275200, .tv_nsec = 0 };
       clock_settime(CLOCK_REALTIME, &ts);
   }
   ```
2. `agent_loop.c` 用**墙钟**（`gettimeofday`）算 LLM 耗时，而这次跳变正好发生在请求进行中。
   `calc_elapsed_ms()` 只防了"时钟倒退"，没防"往前跳"：
   ```
   sec_diff = 1772275203 - 946685621 = 825589582 秒
   825589582 * 1000 = 825,589,582,000
   对 2^32 取模（192 × 4294967296 = 824,633,720,832）
   = 955,861,168 + 微秒部分 835 = 955,862,003 ms       ← 与日志逐位一致
   ```
3. watchdog（阈值 `AGENT_LLM_TIMEOUT_SEC = 60`）据此判超时，并在 `:1126` **无条件
   `llm_response_free(&resp)`**，把已经到手的 `resp.text`（那 12 字节）free 掉、换成超时文案。

**独立佐证**：串口日志里心跳的 `timestamp` 在 `ask` 那一次从 `946685544`
**精确跳到 `1772275200`** —— 正是上面那个硬编码常量。（见
`docs/test_logs/2026-09-13-ai_agent-llm-verify.log`。）

### 修改

| 改动 | 内容 |
|------|------|
| `calc_elapsed_ms()` 上方 | 新增 `mono_gettimeofday()`：用 `CLOCK_MONOTONIC`，不受改墙钟影响 |
| 5 对计时打点（`:823/825`、`:993/995`、`:1068/1070`、`:1086/1089`、`:1186/1189`） | `gettimeofday` → `mono_gettimeofday`（共 10 处） |
| `calc_elapsed_ms()` | 加"往前跳"钳位：`sec_diff > 4294967`（约 49 天）时打警告并返回上限，杜绝 32 位回绕 |
| watchdog 分支 | **响应其实已到时优先交付文本**：`resp.text_len > 0` 就 `strdup(resp.text)` 并打警告，只有真的没有文本才用超时文案 |

### 为什么保留 `vela_tls.c` 的对时不动

它确实会在请求中途跳墙钟（会污染 `message_bus` / `cron` / `network_manager` 那些基于
`CLOCK_REALTIME` 的超时），但把它删掉会让板子时钟回到 2000-01-01（界面上时间会很难看）。
**改成用单调时钟之后，这次跳变对 LLM 计时已经无害**，所以本轮不动它；如果以后要根治，
应该是"启动时对一次时"而不是"每次握手都跳"。

### 验证

- 修复后 `ask` 应看到 `Response: N bytes text` **且不出现** `watchdog ... treating as timeout`；
  即使再遇到时钟异常，也只会打印 `watchdog: delivering received reply (N bytes) despite latency=...`
  并把回复交付出来。
- 复现手法：故意把时钟设早（`date -s` 到 2024 之前）再 `ask`，看是否还能拿到回复。
