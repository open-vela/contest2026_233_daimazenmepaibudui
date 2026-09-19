# 补丁总览（openvela 工作区的三棵上游树）

本目录的补丁 apply 到 **openvela 工作区的三个不同仓库**（`git apply` 的 `cd` 目标
在每节里写明），先总览一遍：

| 补丁 | `cd` 到 | 目标文件 | 现在还需要吗 |
|------|---------|---------|-------------|
| `vendor_sifli-boot-fixes.patch` | `vendor/sifli` | `chips/sf32lb52/{sifli_uart,sifli_irq,sf32lb_flash}.c` | **要**：不修编不过/起不来 |
| `vendor_sifli-rtc-alarm-fix.patch` | `vendor/sifli` | `chips/sf32lb52/sf32lb_rtc.c` | **要**：不修 RTC alarm 永不触发 |
| `vendor_sifli-lcd-brightness.patch` | `vendor/sifli` | `boards/sf32lb52/drivers/lcd/sf32lb_lcd.c` + `.../lcd/co5300.c` | **要**：不修亮度只有 0/100。**2026-09-16 起还带上**：面板重新初始化 `sf32lb_lcd_panel_reinit()`（黑屏救回，`hw_test lcdreinit`）+ 12 步 `[lcdreinit]` 诊断日志 + 关掉"开机第三次复位期间被 SETPOWER 插队"那个竞态 + **面板 QSPI 时钟 48MHz→24MHz**（`co5300.c` 一处，带一键回退），见下 |
| `vendor_sifli-usb-rndis.patch` | `vendor/sifli` | `chips/sf32lb52/sf32lb_usbdev.c` | **要**：不修 USB 枚举失败（Code 10） |
| `nuttx-usbdev-rndis.patch` | `nuttx` | `drivers/usbdev/rndis.c` | **要**：不修 RX 会永久停摆 |
| `nuttx-usbdev-rndis-nomem.patch` | `nuttx` | `drivers/usbdev/rndis.c` | **要**：不修 -ENOMEM 会把 USB 栈打死（需在上一条之后） |
| `apps-ai-agent-velaclaw-sdk.patch` | `packages/ai_agent` | `CMakeLists.txt` | **要**：不补 `velaclaw_client_open` 链不上 |
| `apps-ai-agent-llm-clock-fix.patch` | `packages/ai_agent` | `src/core/agent_loop.c` | **要**：不修 `ask` 永远报超时 |
| `apps-ai-agent-vela-tls-chunked-end.patch` | `packages/ai_agent` | `src/infra/vela_tls.c` | **要**：不修 chunked 响应卡 120 秒 |
| `apps-ai-agent-vela-tls-no-block-close.patch` | `packages/ai_agent` | `src/infra/vela_tls.c` | **要**：不修语音永远停在「处理中」（需在上一条之后） |
| `apps-ai-agent-vela-tls-pool-owner.patch` | `packages/ai_agent` | `src/infra/vela_tls.c` | **要**：不修 TLS 连接池跨 task group 误关 fd（需在前两条之后） |
| `vendor_sifli-uart-rx-dma-reinit.patch` | `vendor/sifli` | `chips/sf32lb52/sifli_uart.c` | **要**：给板级开一个 `sifli_uart_reinit_rx_dma(idx)` 入口。不补这一条，板级音频恢复里那记"复位整块 DMAC1"会把**串口（控制台）RX 的 DMA 通道**一起抹掉 → **控制台永久收不到命令**（板子还活着、日志照打）。详见文末那一节 |

补丁文件本身是给「上游树被重新 sync 之后要重打」用的场景留的：这三棵树的改动
**都已经直接改在工作区里、未提交**，所以补丁都能 `git apply --check --reverse` 通过
（= 和当前工作区逐字对应）。板级（`boards/`）的改动**不再属于这一套**，见下。

## 板级改动不走补丁了（2026-09-15 清理）

原先的 `vendor_sifli-audio-driver.patch` **已删除**。原因是它已经不只是一份
「陈旧的快照」，而是**会骗人**：

- 它的目标路径是 `boards/sf32lb52/sf32lb52_devkit_lcd/src/...`，而工作区里这个
  目录是一个**指向本仓库 `board/contest_board` 的软链接**。这不是可有可无的摆设：
  固件正是从这里取板级源码的 ——
  `cmake_out/contest2026_233_board_sf32lb52_ai/.config` 里
  `CONFIG_ARCH_BOARD_CUSTOM_DIR="../vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd"`。
  于是 `git apply` 拒绝穿过软链接，实际报错是：

  ```
  error: affected file 'boards/sf32lb52/sf32lb52_devkit_lcd/src/sifli_ap.c'
         is beyond a symbolic link
  ```

  也就是说**在现在的工作区里它一个字也打不进去**。
- 更坑的是：在**全新 sync、还没做软链接的上游 vendor 树**里它是能干净 apply 的
  （已用 `git apply --check` 在 HEAD 的干净副本上验证过）。队友照 README 打完，
  得到的是那份 1287 行的**旧驱动**，板级也回到旧版本 —— 当前固件里所有音频改动
  （半双工串行化、`sf32lb52_audio_in.c` 的接入、DMAStop 复核……）全部不在里面，
  而且 `sifli_ap.c` 会被改回 `CONFIG_LVX_USE_DEMO_CONTEST2026_000_HELLO_APP`
  那段**永远不会生效的旧自启**。两边的量级差：
  `sf32lb52_audio.c` 1287 → **2484 行**，`sifli_ap.c` 有 232 行差异，
  `CMakeLists.txt` 现在多编 6 个板级源文件
  （`sf32lb52_alarm.c` / `sf32lb52_rtc_alarm.c` / `sf32lb52_audio_in.c` /
  `sf32lb52_backlight.c` / `sf32lb52_boardbtn.c` / `sf32lb52_status.c`）。
  只有 `sf32lb52_audio.h` 两边**逐字节相同**，其余都不是「打上去就等价」。

所以：**驱动直接在比赛仓库里改，不再靠补丁**。板级文件的唯一权威版本就是
`board/contest_board/src/` 下的那些文件；`vendor/sifli/boards/.../<board>` 只是
指向它的软链接，两边本来就是同一份文件。

> 旧补丁内容仍在 git 历史里（`git show f5ff5ac:patches/vendor_sifli-audio-driver.patch`），
> 需要考古时再取。
>
> 仓库里**已经没有**还在指向这个已删除补丁的地方了：`docs/audio_driver_usage.md`
> 第 6 节「落地方式」2026-09-15 已改成"板级驱动直接写在 `board/contest_board/src/`、
> 不要打板级补丁"（并写清 RNDIS 钩子在 `sifli_ap.c:694`，同样是仓库里的代码）。
>
> 仓库外还剩两处（不在本仓库里，本轮没动）：`/home/youdian/gen_patches.sh`、
> `/home/youdian/commit_audio.sh` 是早期一次性脚本，仍写死旧文件名。它们**不是构建步骤、
> 也不会被自动执行**（全仓没有任何脚本会自动 apply 补丁），但下次手工跑到会报
> file not found；真要用就把那段删了。

## vendor_sifli-boot-fixes.patch

让 openvela 在 SF32LB52-DevKit-LCD 上可编译/可启动的 3 处上游修复。
**本补丁只改 `chips/sf32lb52/`，不含任何 `boards/` 文件。**

### 修复内容

1. `chips/sf32lb52/sifli_uart.c` — `void up_putc()` 中删除非法的 `return ch;`
2. `chips/sf32lb52/sifli_irq.c` — 补充 `arm_lowprintf` 的 extern 声明(定义在 sifli_start.c)
3. `chips/sf32lb52/sf32lb_flash.c` — `HAL_FLASH_CONFIG_FULL_AHB_READ` → `HAL_FLASH_CONFIG_AHB_READ`(头文件中正确的函数名)

> 早先这里还列过两条 `sifli_ap.c` 的修复（补 `sifli_i2cbus_initialize` extern +
> `<nuttx/i2c/i2c_master.h>`；删掉误配到触摸 SCL 的 `HAL_PIN_Set(PAD_PA37, I2C1_SCL, ...)`，
> 触摸 I2C1 是 SCL=PA30/SDA=PA33，由 `bsp_pinmux.c` 配）。
> 那两条现在**直接写在 `board/contest_board/src/sifli_ap.c` 里**，
> 既不在本补丁里，也不在任何其它补丁里 —— 想核对就去看那个文件。

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

## vendor_sifli-lcd-brightness.patch（2026-09-13 新增，屏幕百分比亮度；2026-09-16 追加"面板重新初始化"与"QSPI 降频"）

改 `boards/sf32lb52/drivers/lcd/` 下的**两个文件**：

- `sf32lb_lcd.c` —— 亮度百分比 + 面板重新初始化（下面"现象/根因/修改"三节的行号
  都是 2026-09-13 那版的，即**改前**的；2026-09-16 的新增在后面几节）。
- `co5300.c` —— 面板 QSPI 时钟 48MHz → 24MHz（**只有这一处改动**，
  在 2026-09-16 第三轮那节）。

补丁文件名**没有改**，但它现在装四件事：亮度百分比（下面前四节）+ 面板重新
初始化（一节）+ 逐步诊断日志（一节）+ QSPI 降频（最后两节）。不拆成多个文件，
是因为四件事改的是同一段显示通路、同一个 `git apply` 的目标，分开打还得保证顺序。

> 两个文件都在 **vendor 公共树**里，且 `nuttx/drivers/platform` 是**指向
> `vendor/sifli/boards/sf32lb52/drivers` 的软链接** —— 也就是说
> `nuttx/drivers/platform/lcd/co5300.c` 和
> `vendor/sifli/boards/sf32lb52/drivers/lcd/co5300.c` 是**同一个文件**，
> 改一处即可（`git diff` 只认 vendor 那条路径）。

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
- 板级封装 `board/contest_board/src/sf32lb52_backlight.c` 和 UI 亮度滑块
  （`app/robot_ui/touch_ui.c` 的 `setting_slider_event_handler`）已经接到这条
  通路上；用法与兼容性（没打补丁的树仍然只有 0/100）见
  `docs/display_touch_gpio_usage.md` 第 3.3 节。
- **只打这个补丁还不够**：板级封装原来那份实现（文件里写着"1..99 直接
  `return -ENOSYS`，不往下发 ioctl"）会自己把中间值挡掉，`hw_test lcd 30`
  报的 `-38` 是它编的，跟驱动无关。改完封装（走 `LCDDEVIO_SETCONTRAST`）
  之后四档才真的 PASS。

### 2026-09-16 追加：面板重新初始化 `sf32lb_lcd_panel_reinit()`

**治的是**：整屏黑，但应用/驱动都正常 —— LVGL 还在响应触摸、还在 20~30 帧/s
往面板推画面（`[ui]` 仪表 `最慢 flush 10 ms`），`hw_test lcd 80` 下发亮度也成功，
可屏幕就是不亮；`reset` 无效，**只有真断电才恢复**。用户观察到的复现规律是
"开机后第一次点「提醒」/「主菜单」容易黑"、"短时间快速点右上角菜单会频繁黑闪、
最后长时间黑屏"，像是**面板自己丢了配置**。救法就是重发一遍初始化序列。

**加了什么**（都在同一份 `sf32lb_lcd.c` 里）：

| 改动 | 说明 |
|------|------|
| `#define SF32LB_LCD_PANEL_REINIT 1` | **一键回退开关**。改 0 之后 `sf32lb_lcd_panel_reinit()` 直接回 `-ENOSYS`、`panel_lock` 的三个宏变空操作，**刷新路径和开机路径一行不多** |
| `sf32lb_lcd_lcdc_setup()` | 把原来只写在 `lcd_hw_setup_thread_entry()` 里的"背景色 / 层复位 / 层像素格式"抽成函数，**开机路径和新入口调的是同一个**（不出现两份初始化） |
| `s_drv_lcd.panel_lock` | 二值信号量当互斥：`putrun` / `putarea` 推像素、重初始化、`setpower`、以及**开机那次面板复位**拿同一把 |
| `sf32lb_lcd_panel_reinit()` | 公开入口，12 个 STEP，每一步一行 syslog（见下）。返回 `OK` / `-ENODEV` / `-EINVAL` / `-ENOSYS` |

`co5300.c` **一个字没改** —— 既有的 `LCD_Drv_Init()` 本来就是一个函数，
开机路径（`LCD_Init()`）和这个新入口调的都是它。

驱动侧只动面板和 LCDC，**不碰 LVGL**；"重初始化之后全屏重绘"由调用方负责：
`app/robot_ui/robot_ui_bridge.c` 的 `robot_ui_bridge_panel_reinit()` 在调完驱动
之后用 `ui_async_call()` 往 LVGL 线程投一次整屏 `lv_obj_invalidate()`。
`hw_test lcdreinit` 调的就是它。

用法、期望输出（正例/反例）、已知边界见
`docs/display_touch_gpio_usage.md` 第 3.4 节。

**这一轮后来上板试过**：`hw_test lcdreinit` 命令本身成功
（`robot_ui_bridge_panel_reinit() -> 0`，耗时 320 ms），**但屏幕还是黑的** ——
所以黑屏不是"面板丢配置"这么简单，于是有了下面第二轮：先让日志把"卡在哪一层"
说出来，再补上开机做过而 reinit 漏掉的步骤。

### 2026-09-16 第二轮：逐步诊断日志 + 补齐"开机做了、reinit 没做"的步骤

上一轮的 `panel_reinit` 救不回黑屏（命令成功、屏幕还是黑的），所以这一轮的目标
从"再试一次"改成**"让日志说出卡在哪一层"**，同时补上几条确实漏掉的步骤。

**① 日志必须先能看见（这一步本身就是个坑）**

`lcdinfo` / `lcdwarn` / `lcderr` 在本板固件里**全被编掉了**：`CONFIG_DEBUG_LCD`
没开，`nuttx/include/debug.h:517-532` 把它们展开成 `_none`。也就是说驱动里原有的
那些打点一行都不会出现在串口里（这正好解释了"为什么日志里只有 `[ui]`/`[Bridge]`"）。
所以新日志一律走 `syslog()`，前缀 `[lcdreinit]`，PASS/WARN/FAIL 三档分别用
LOG_INFO / LOG_WARNING / LOG_ERR（本板 `CONFIG_SYSLOG_DEFAULT_MASK=0xff`，三级都出得来）。

**② 12 个 STEP，其中最有判别力的是读面板 ID**

| STEP | 做什么 | 日志里看什么 |
|------|--------|-------------|
| 1 | 拿 `panel_lock`（带 1s 超时） | 等了多久才拿到 |
| 2 | 状态快照 | `State` / `Lock` / `ErrorCode` / 起帧 vs 完帧计数 / `LCD_CONF` `SPI_IF_CONF` `LCD_IF_CONF` `TE_CONF` `STATUS` / `HCLK` 与 `CLK_DIV` |
| 3 | **动手前读面板 ID** | `ID == 期望` ⇒ QSPI 链路 + 面板都活着，黑屏不在"送不出去"这一层 |
| 4 | 坏状态归零 | `Lock→UNLOCKED`、`State→READY`、`ErrorCode→NONE`，原值不是这样就会打 WARN |
| 5 | `BSP_LCD_PowerUp()` | PA10 供电、PA37 VADD_EN、QSPI 脚 pinmux |
| 6 | RESET 脚时序 | 自己拉 `1→10ms→0→30ms→1→120ms`（开机只拉低 10ms，这里给 3 倍余量） |
| 7 | `p_ops->Init()` | **这一步内部就是 `HAL_LCDC_Init()`**；之后回读 `CLK_DIV` 和 `LCD_RSTB` 确认 |
| 8 | 补开机的中断接线 | `irq_attach` + `up_enable_irq(LCDC1_IRQn)`（幂等） |
| 9 | 层配置 + 像素格式 | `sf32lb_lcd_lcdc_setup()` + `SetColorMode` |
| 10 | 亮度 + DisplayOn | `WBRIGHT` 重下发、`0x29` |
| 11 | 收工后再读一次 ID | 和 STEP3 配对 = "这次救没救回来" |
| 12 | 结论 + 耗时 | 总耗时 / **持锁时长**（= 挡了刷屏多久） |

**③ 为什么没有单独加 `HAL_LCDC_Reset()`（这是任务里点名要评估的）**

读实现（`bf0_hal_lcdc.c:2373`）可以确认 `HAL_LCDC_Reset()` = `LCDC_HW_Init()` +
`State = READY`；`HAL_LCDC_Init()` = `LCDC_HW_Init()` + 置 `Layer[].disable`。
而 **`LCDC_HW_Init()` 每次都会 `HAL_RCC_EnableModule + HAL_RCC_ResetModule(RCC_MOD_LCDC1)`**
（`bf0_hal_lcdc.c:2267-2272`），那一步就是 LCDC 整个模块的硬复位 ——
"显示通路重来一遍"要的正是它，而 `p_ops->Init()` 里已经调了 `HAL_LCDC_Init()`。
可重入性也不是靠猜：**开机路径已经把 `p_ops->Init()` 整整调了三次**
（`find_right_driver`、`lcd_init_thread_entry`、`lcd_hw_setup_thread_entry`）。
所以再插一次 `HAL_LCDC_Reset()` 只是多一次全模块复位 + 多一个 ROM 函数的重入面，
什么也不多给 —— 不加，改成"回读寄存器确认它确实做了"。

**④ 显示时钟 / PMU 显示电源域：这条路上没有**

LCDC1 的模块时钟来自 HCLK（本板 240MHz，`bsp_init.c:155-159` 用 DLL1 提供），
分频写进 `SPI_IF_CONF.CLK_DIV`，`SetFreq()` 从 `HAL_RCC_GetHCLKFreq()` 现算。
**没有独立的显示 PLL，也没有 PMU 里的显示电源域**需要重新使能；面板的 VADD_EN 是
板级 GPIO PA37（`bsp_lcd_tp.c:35`，由 `BSP_LCD_PowerUp()` 拉）。LCDC 那个"QSPI"
就是面板总线本身（LCDC 的 SPI 接口），**不是 flash 的 SFC 控制器**，别去动 SFC。
所以 STEP2/STEP7 把 HCLK 和 CLK_DIV 都打出来，直接看数值判断。

**⑤ 顺手关掉一个开机竞态（"重插有概率黑屏起不来"）**

开机那条路把面板**复位了三次**：

```
board_lcd_initialize()            sf32lb_lcd.c:1128  起 lcd_init 任务
 └ lcd_init_thread_entry()        sf32lb_lcd.c:994   BSP_LCD_PowerUp → find_right_driver
    ├ find_right_driver()         :232   p_ops->Init()   ← 第 1 次（:280）
    ├ p_ops->Init()               :1055  ← 第 2 次（然后 lcddev_register :1077 → /dev/lcd0）
    └ task_create("lcd_hw")       :1101
       └ lcd_hw_setup_thread_entry()      sf32lb_lcd.c:931
          ├ fb_register(0,0)      :942    ← /dev/fb0 从这一刻就存在
          ├ p_ops->Init()         :977    ← 第 3 次（~0.4s）
          └ s_lcd_hw_ready = true :987
```

`putrun` / `putarea` 有 `s_lcd_hw_ready` 挡着（这段里丢帧无害），**但
`sf32lb_lcd_setpower()`（`LCDDEVIO_SETPOWER`）过去是不挡的**：上层只要在
第 3 次复位期间调一次 SETPOWER，就会往"RESET 脚正被拉低、寄存器序列写到一半"
的面板上插一个 `0x29`/`0x28`，面板停在一个半配置状态 —— 之后一直黑，直到真断电。

`board_late_initialize()` 里那句 `usleep(2*1000*1000)` 本来是防这个的，但不够：
`lcd_init` 的优先级是 `SCHED_PRIORITY_DEFAULT - 5`（=95）、`lcd_hw` 是 100，
而 **robot_ui 是 110**。本树的优先级是**数值越大越优先**
（`nuttx/sched/sched/sched.h:417` 明写 ready-to-run 链表按 `sched_priority`
**降序**维护，`sifli_ap.c` 里那句"提到 110 让 UI 永远优先、但仍低于 hpwork(224)"
也是这个意思）—— 所以 robot_ui 一旦就绪就会**抢**正在做第 3 次复位的 `lcd_hw`
（后者在 `up_mdelay` 里忙等、不让出 CPU）。`/dev/lcd0` 那时已经存在、
`/dev/fb0` 也已注册，于是 SETPOWER 就插进复位序列里了 —— "有概率"。

修法：让第 3 次复位和 `setpower` 拿同一把 `panel_lock`（`setpower` 用带 1s 超时的
等，超时只打 WARN 后照原样下发）。`s_lcd_hw_ready = true` 也挪到锁里，保证第一个推
像素的人一定看到复位后的面板。**开关关掉（`SF32LB_LCD_PANEL_REINIT 0`）时这些
全部退回原样**，开机语义一个字没变。

**⑥ 关于"重初始化持锁太久会挡住刷屏"（`hw_test lcdreinit` 实测过 290ms 被挡）**

新一轮把 RESET 低电平从 10ms 提到 30ms、多两次 ReadID、多几行日志，持锁时长会从
~0.32s 涨到 **~0.40s** 左右 —— STEP12 那行会把实测值打出来（`总耗时 X ms，其中持锁
Y ms`）。这个量级下"先停刷屏再重初始化"（LVGL 的刷新由它自己的定时器驱动，本项目
没有现成的暂停接口）不值得为它引入新耦合；真要救黑屏，用户敲一次命令等 0.4 秒完全
可以接受。哪天真需要缩短，第一步是把 `BSP_LCD_PowerUp()` 的 pinmux 和 STEP6 的额外
复位脉冲砍掉（各 ~0.16s / 0.14s）。

**⑦ 验证（本轮没上板）**

- `syntax_check.sh vendor/sifli/.../lcd/sf32lb_lcd.c` → `[OK] 0 error`；
  把 `#define SF32LB_LCD_PANEL_REINIT` 改成 `0` 再查一次 → 同样 `[OK] 0 error`
  （两个编译期位置都编得过，且 0 那一档没有 `-Wunused-function` 之类的告警）。
- `git apply --check --reverse` 通过（补丁与工作区逐字对应）。
- 补丁自检：`git show HEAD:boards/sf32lb52/drivers/lcd/sf32lb_lcd.c` 打上本补丁后，
  与当前工作区文件 `cmp` **逐字节相同**。
- **没上板**：只做了语法检查、补丁一致性检查和上面这份阅读结论。黑屏现场要敲的
  命令与逐行判读见 `docs/display_touch_gpio_usage.md` 第 3.4 节。


### 2026-09-16 第三轮：面板 QSPI 时钟 48MHz → 24MHz（`co5300.c`）

**治的假设**：这块屏是**间歇性**的 —— 用户原话"偶尔闪出正常的界面，然后只能看到字、
很浅、整体是黑，现在全黑（刚刚闪了一下）"。而黑屏时**应用侧完全正常**：LVGL 在刷
（20~30 帧/s、每次 flush 0~10ms）、`hw_test lcd 80/100` 能成功下发亮度、面板 ID 读得回
`0x00331100`、LCDC `起帧=完帧`。也就是**面板会显示、像素能写进去，只是大部分时候不对**。
形状（大块背景填充丢、只剩细笔画）最像**写像素时丢数据**，而不是"面板丢了配置"
（那正是第二轮 `panel_reinit` 救不回来的原因）。

**为什么怀疑时钟**：厂商提交 `0a3cd0a` 的描述里写着 *"some panels do not respond
reliably to ID queries **on USB-only power where read timing is marginal**"*，
而本板就是 USB 直供（`board/contest_board/README_zh-cn.md`：不带电池）。那个提交的
两处修复（`sf32lb_lcd.c:1053` 的显式 `Init()`、`co5300.c:214-230` 的 SWRESET+ReadID
降级）**都已经在树里**，所以不是"它没修"，而是**边缘时序这件事本身还在**。

⚠ 注意厂商那句话说的是**读**时序（ID 查询），不是写；我们这条现象是写像素。
所以降频只是**最便宜、最好回退的那个实验**，不等于已经证明是时钟问题（判据见下）。

**时钟是怎么定的（全驱动唯一一条路径）**：

```
co5300.c  lcdc_int_cfg_qadspi.freq            ← 唯一来源，就改这里
  -> LCD_Init()  memcpy 进 hlcdc->Init
  -> LCD_Drv_Init()  HAL_LCDC_Init(hlcdc)
  -> LCDC_HW_Init()  SetFreq(lcdc, init->freq)     bf0_hal_lcdc.c:2284
       clk_div = ceil(HCLK / freq)    ← HAL 里那个除法是**向上取整**，硬件最小分频 2
       SPI_IF_CONF.CLK_DIV = clk_div  (位域 [13:6]，8 位，最大 255)
```

HCLK = 240MHz（`board/contest_board/src/bsp_init.c:158`
`HAL_RCC_HCPU_EnableDLL1(240000000)`），所以：

| `.freq` | `CLK_DIV` | 实测面板时钟 |
|---------|-----------|-------------|
| `50000000`（改前 / `SF32LB_LCD_QSPI_CLK_SLOW 0`） | 5 | 48MHz |
| `24000000`（现在默认） | 10 | 24MHz |

**只改一处就够**：驱动侧没有任何代码写 `SPI_IF_CONF`，`sf32lb_lcd.c` 里连 `freq`
这个字段都不碰；读寄存器时的降速/恢复（`co5300.c` 的 `LCD_ReadMode()`：读时
`HAL_LCDC_SetFreq(hlcdc, 2000000)`，出来恢复成 `lcdc_int_cfg.freq`）用的也是同一个
字段。所以改成宏之后，`HAL_LCDC_Init` 那条路和读写切换那条路一起变了。

**改动**（`co5300.c` 顶部 + 结构体初始化，都在 124~164 行这一小段里）：

| 位置 | 改动 |
|------|------|
| `co5300.c:126-153` 顶部 | 新增 `#define SF32LB_LCD_QSPI_CLK_SLOW 1`（**一键回退开关**）与 `SF32LB_LCD_QSPI_CLK_HZ 24000000`，注释里写清公式、两个频率的分频、为什么降、代价 |
| `co5300.c:158-164` | `.freq` 改成三分支：`LCD_MAX_CLK_FREQ`（原来那个 Kconfig 钩子，全树没有任何地方定义它，保留优先级）→ `SF32LB_LCD_QSPI_CLK_HZ` → `50000000`（回退档，**字面值逐字等于改前**） |

**一键回退**：`#define SF32LB_LCD_QSPI_CLK_SLOW 0`
→ 预处理器走 `#else`，`.freq = 50000000`，逐字回到 48MHz（已用预处理器输出验证，见下）。

**上板怎么确认生效**：敲 `hw_test lcdreinit`，看两行（这两行读的是**实时硬件寄存器**，
`hlcdc->Instance` 是指针，不是快照）：

```
[lcdreinit] STEP2 显示时钟: HCLK=240MHz CLK_DIV=10 ⇒ 面板时钟≈24MHz
[lcdreinit] PASS STEP7 SPI_IF_CONF.CLK_DIV=10 ⇒ 面板时钟≈24MHz
```

`SPI_IF_CONF` 也会从 `0x00000140` 变成 `0x00000280`（10 << 6）。
两行还是 `CLK_DIV=5` ⇒ 降频没编进去（开关被置 0 / 补丁没打 / 树被重新 sync）。

**正例 / 反例**（怎么判断这个假设成不成立）：

- **正例**：长时间不再"只剩字/很浅/全黑"，反复点菜单、亮暗切换、长时间跑都稳定
  —— 那 48MHz 的边缘时序就是根因，此后可以试 30MHz / 40MHz 找"稳且快"的那个点。
- **反例**：黑屏与丢像素的**频率、形状都没有变化** ⇒ 时钟不是根因，
  把开关置回 0，回头查背光/偏置（PA37 VADD_EN）/排线/面板本身。

**代价**：推送带宽减半（4 数据线 24MHz ≈ 12MB/s，全屏 390×450×2B ≈ 29ms/帧，
48MHz 时 ≈ 15ms）。面板自刷新不受影响（CO5300 带 GRAM，自刷新由它内部振荡器驱动），
只有"SoC 往面板推画面"变慢，所以 LVGL 帧率上界从 ~60 降到 ~34 帧/s。

**验证（本轮没上板）**：

- `syntax_check.sh boards/sf32lb52/drivers/lcd/co5300.c` → `[OK] 0 error`
  （唯一那条 warning 在 `:518` `DEBUG_PRINTF("result: [%x]\n", read_value)`，
  是**改动前就有的**，跟本次改动无关）。
- 把 `SF32LB_LCD_QSPI_CLK_SLOW` 改成 `0` 的副本再查一次 → 同样 `[OK] 0 error`。
- **预处理器实测**（借 `compile_commands.json` 里 co5300.c 的真实参数跑 `-E`）：
  默认档 `.freq = 24000000,`；`SLOW=0` 档 `.freq = 50000000,`。
  这同时证明 `LCD_MAX_CLK_FREQ` 那个分支**没有被任何头文件打开**
  （全树 grep 过：只有 co5300.c 自己提到这个名字）。
- `git apply --check --reverse` 通过；补丁自检：`git show HEAD:<两个文件>`
  打上本补丁后与工作区 `cmp` **逐字节相同**。
- **没上板**：只做语法检查 + 预处理取值 + 补丁一致性检查。上板命令与逐行判读见
  `docs/display_touch_gpio_usage.md` 第 3.4.6 节。


## 应用方式

三条 `vendor/sifli` 补丁（顺序只为可复现，改的文件互不重叠；
其中 lcd 那个补丁改的是**两个**文件 —— `boards/sf32lb52/drivers/lcd/sf32lb_lcd.c`
与 `boards/sf32lb52/drivers/lcd/co5300.c`，同属 LCD 驱动）：

```bash
cd <openvela 工作区>/vendor/sifli
git apply <本仓库>/patches/vendor_sifli-boot-fixes.patch
git apply <本仓库>/patches/vendor_sifli-rtc-alarm-fix.patch
git apply <本仓库>/patches/vendor_sifli-lcd-brightness.patch
```

打之前先 `git apply --check <补丁>`（或 `--check --reverse` 看是不是已经打过了）。

## 说明

- 补丁修复/功能均为上游 vendor_sifli 缺口(开启 I2C/DEBUG 等配置后编译必现),建议以团队名义向 [open-vela/vendor_sifli](https://github.com/open-vela/vendor_sifli) 提交 PR
- 工作区中已直接应用了这些修复(未提交),补丁用于留存/提交/队友复现
- **`boards/` 下的板级改动不在这里**：`board/contest_board` 是仓库内的文件，
  `vendor/sifli/boards/.../<board>` 只是指向它的软链接。板级要改就改仓库，
  不要为它生成补丁（原因见上面「板级改动不走补丁了」）。

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

> 注：`sifli_ap.c` 里的 RNDIS bring-up（`board_late_initialize()` 里
> `#ifdef CONFIG_RNDIS` 那段 `usbdev_rndis_initialize()`，MAC `00:e0:4c:53:42:31`）
> 现在是**比赛仓库里的代码**：`board/contest_board/src/sifli_ap.c:694`。
> 它以前被裹在 `vendor_sifli-audio-driver.patch` 里，那个补丁已删除
> （见上面「板级改动不走补丁了」）—— 要核对这段就去读仓库里那个文件。

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

---

# USB RNDIS 收到 -ENOMEM 就断言 → USB 设备栈死掉（2026-09-14 新增补丁）

## 现象

用户第二次点「语音聊天」，Windows 报「无法识别的 USB 设备」：

```
Assertion failed at /drivers/usbdev/rndis.c:1820   task: robot_ui
  rndis_rdcomplete → sf32lb52_reqcomplete → sf32lb52_rdrequest → exception_direct
```

## 根因

`rndis.c:1820` 原来是 `DEBUGASSERT(ret != -ENOMEM)`。`rndis_recvpacket()` 只在**一处**
返回 `-ENOMEM`：`rndis_allocrxreq()` 拿不到 RX 缓冲（`iob_tryalloc()` 返回 NULL，
或 `rndis_allocwrreq()` 的写请求空闲链空了）。IOB 池是启动时固定大小的，语音链路
一轮里同时活着的东西不少（录音 PCM 最多 320 KB、ASR 请求体约 430 KB、TTS 响应
1.5 MB、播放缓冲 256 KB），把网络收发缓冲挤空一次，就在 **USB 中断上下文**里踩中
了这条断言 —— 丢的本来只是一帧以太网帧（TCP 会重传、ARP/ICMP 会重试），
代价却是一整块 USB 网卡。

## 修改

文件：`nuttx/drivers/usbdev/rndis.c`。

| 位置 | 改动 |
|------|------|
| `struct rndis_dev_s` | 新增 `uint32_t rx_dropped_nomem`（丢帧计数，给限流日志用） |
| `rndis_rdcomplete()` `:1820` | 删掉 `DEBUGASSERT(ret != -ENOMEM)`，改成**丢帧 + 重挂读请求 + 限流日志**：`ret` 归位 `OK`，于是走下面原有的 `rndis_submit_rdreq(priv)` 重新武装 bulk OUT 管道；日志前 3 次全打、之后每 64 次打一条 |

两条依据：

- `rndis_recvpacket()` 的 `-ENOMEM` **只可能出现在一个数据报的开头**：续包进来时
  `priv->rx_req` 已经不是 NULL，`rndis_allocrxreq()` 直接返回 true。所以丢这一帧
  不会留下半截重组状态。
- 丢帧之后**必须重挂读请求**，否则 `ret != OK` 会让 `if (ret == OK) rndis_submit_rdreq(priv);`
  跳过，RX 永久停摆（和 `nuttx-usbdev-rndis.patch` 第 2 条同一个坑）。

## 应用方式

**在 `nuttx-usbdev-rndis.patch` 之后应用**（两者改同一个文件，改动不重叠但顺序固定）：

```bash
cd <openvela 工作区>/nuttx
git apply <本仓库>/patches/nuttx-usbdev-rndis.patch
git apply <本仓库>/patches/nuttx-usbdev-rndis-nomem.patch
```

## 验证

- 编译通过；`syntax_check.sh nuttx/drivers/usbdev/rndis.c` 0 error。
- 上板：正常跑一轮「语音聊天」不应再出现 `Assertion failed at .../rndis.c:1820`；
  内存真被挤空时串口会出现 `ERROR: RX buffer unavailable, frame dropped (N total)`，
  网卡仍然可用（丢一帧而已）。
- 想主动复现 `-ENOMEM`：临时把 `CONFIG_IOB_NBUFFERS` 调小，或者在压满堆的同时
  连续收报文。

---

# vela_tls 读到 chunked 响应的结尾还在等（2026-09-14 新增补丁）

## 现象

`[mimo_voice] TTS: 请求 406 字节文本` 之后：

```
[vela_tls] Handshake OK: TLSv1.2 ...
        ← 之后再没有任何 TTS 日志，卡了两分多钟，直到用户手动关掉弹窗
```

「两分多钟」正好是 `AGENT_LLM_SOCKET_TIMEOUT_SEC` 给 socket 设的 `SO_RCVTIMEO`。

## 根因

`src/infra/vela_tls.c` 的 `tls_read_response()`（以及明文那条 `vela_http_post_json()`）
读 body 的循环只有两个退出条件：**有 `Content-Length` 时按长度收尾**、连接关闭。
而请求头里写死了 `Connection: keep-alive`（`:427`），服务器用
`Transfer-Encoding: chunked` 时既没有 `Content-Length`、也不会主动关连接 ——
循环就一直卡在 `mbedtls_ssl_read()` 上等永远不会再来的数据，直到 120 秒超时。

## 修改

| 位置 | 改动 |
|------|------|
| `chunked_body_complete()`（新增，在 `tls_read_response()` 上方） | 在已经读到的 body 末尾找 chunked 的终止块 `0\r\n\r\n` |
| `tls_read_response()` body 读循环 `:651` | 每读到一段就判一次，终止块到了立刻 `break` |
| `vela_http_post_json()` body 读循环 `:1022` | 同一处 guard（明文那条路有一样的毛病） |

只认「没有 trailer」的终止形式（在末尾 8 字节里找 `0\r\n\r\n`）：带 trailer 的响应
会退化成原来的行为（等超时），不会误判。

## 应用方式

```bash
cd <openvela 工作区>/packages/ai_agent
git apply <本仓库>/patches/apps-ai-agent-llm-clock-fix.patch        # 若有
git apply <本仓库>/patches/apps-ai-agent-vela-tls-chunked-end.patch
```

## 验证

- 编译通过；`syntax_check.sh packages/ai_agent/src/infra/vela_tls.c` 0 error。
- 上板：`[mimo_voice] TTS: 请求 N 字节文本` 之后应在几秒内出现
  `[mimo_voice] TTS: 收到 base64 音频 N 字节`，不再卡 120 秒。

---

# vela_tls 关闭旧连接时发 close_notify → TLS 的 send 没有超时 → 永久卡死（2026-09-14 新增补丁）

## 现象

语音聊天里点「提交」之后，界面**永远停在「处理中」**（不是崩溃：弹窗还能点、主菜单还能用，
只是这一轮永远不结束）。串口日志停在：

```
[VoiceChat] 开始识别 (124160 字节)
[mimo_voice] ASR: PCM 124160 字节 -> WAV -> base64 165608 字节
[mimo_voice] alloc  asr.resp       16384 字节
        ← 之后连 "[vela_tls] Handshake start" 都没有，一直不出来
```

## 根因（栈抓出来的）

```
voice_worker → mimo_asr_recognize → vela_tls_pool_cleanup
  → tls_ctx_free → mbedtls_ssl_close_notify → mbedtls_ssl_flush_output
  → mbedtls_net_send → write → psock_tcp_send → net_sem_timedwait2   ← 卡在这
```

两个叠加的原因：

1. `tls_ctx_free()`（`src/infra/vela_tls.c`）**会发 close_notify** —— 那是一次**阻塞的 TCP 发送**。
2. 本文件的 socket **只在 `CONFIG_AI_AGENT_NET_RPMSG` 下才设 `SO_SNDTIMEO`**（我们这版没开），
   所以这次 send **没有超时**：对端已经失联时，它就死等 TCP 重传（分钟级）。

`mimo_post()` 每次请求前会调 `vela_tls_pool_cleanup()`（为了强制新建连接、避开"复用死连接"），
于是**每次识别/对话都要先陪这一次卡死**。而且 `tls_ctx_free()` 是在 `s_pool_lock` 里调的，
别的线程跟着一起堵。

## 修改

| 位置 | 改动 |
|------|------|
| `tls_ctx_free()` | **不再发 `mbedtls_ssl_close_notify()`**：这些连接都是要丢掉的旧连接，本地拆掉就行，服务端收到 FIN/RST 自己回收 |
| `tls_ctx_connect()` 里设 socket 超时那段 | **无条件设 `SO_SNDTIMEO = 30s`**（原来只在 RPMSG 下设）——请求体最大 1.5 MB，USB RNDIS 上一两秒就传完，30s 足够，超时就报错走失败分支而不是卡死 |

## 应用方式

```bash
cd <openvela 工作区>/packages/ai_agent
git apply <本仓库>/patches/apps-ai-agent-vela-tls-chunked-end.patch
git apply <本仓库>/patches/apps-ai-agent-vela-tls-no-block-close.patch
```

## 验证

- `git apply --check --reverse` 通过（补丁与已改好的工作区逐字对应）。
- 上板：`ASR: PCM ... base64 ...` 之后应在几百毫秒内出现 `[vela_tls] Handshake start`
  → `Handshake OK` → `ASR: 识别结果`，不再停在"处理中"。

---

# 2026-09-14 上板实测暴露的三个问题（本轮改动总览）

| 文件 | 改了什么 |
|------|----------|
| `app/robot_ui/lv_font_ui_{16,20,24}.c` | 字库字符集从「源码里出现过的 971 字」扩到常用汉字全集 |
| `board/contest_board/configs/sf32lb52_ai/defconfig` | 加 `CONFIG_LV_FONT_FMT_TXT_LARGE=y`（字变大之后必须开，见下） |
| `app/robot_ui/main.c` | 新增显示用清洗 `sanitize_for_display()`；`voice_speak_reply()` 的状态行改成如实反映 |
| `app/hello_app/mimo_voice.c` | TTS 合成文本截断；响应 cap 1 MB→1.5 MB；base64 就地解码；重采样直接写进调用方缓冲；每步分配/释放打日志 |
| `app/robot_ui/CMakeLists.txt`、`robot_ui.c`、`touch_ui.c` | 更新「971 个字符」那段过期注释 |
| `patches/nuttx-usbdev-rndis-nomem.patch` | RNDIS `-ENOMEM` 不再断言 |
| `patches/apps-ai-agent-vela-tls-chunked-end.patch` | chunked 响应提前收尾 |
| `patches/apps-ai-agent-vela-tls-no-block-close.patch` | 关旧连接不再发 close_notify、TLS send 加 30s 超时（修「永远停在处理中」） |

## 中文字库覆盖常用汉字全集

### 为什么

原来的三档字库是用 `lv_font_conv` + `simhei.ttf` 从 `app/` 源码里抽字符生成的
（971 个），所以 AI 回复里任何「源码里没出现过的汉字」都会显示成方块，
emoji（😊📱🔍🗣️）更是必然没有。

### 字符集

`_ui_chars_full.txt`，共 **7178 个码点**，字库里实际 **7010 个字形**
（simhei 没有的码点会被跳过）：

- GB2312 一级 + 二级汉字 **6763** 个（遍历 0xB0A1..0xF7FE 全部双字节组合解码得到）
- ASCII 0x20-0x7E
- CJK 符号/标点 U+3000-U+303F
- 全角字符 U+FF00-U+FFEF
- 常用符号 °±×÷—…·“”‘’←↑→↓≈「」『』【】

### 怎么生成的

simhei.ttf 在 Windows 侧，所以用 Windows 的 node 跑 lv_font_conv：

```bash
node C:/Users/<user>/AppData/Roaming/npm/node_modules/lv_font_conv/lv_font_conv.js \
  --font C:/Windows/Fonts/simhei.ttf \
  --symbols "$(cat _ui_chars_full.txt)" \
  --size 16 --bpp 4 --format lvgl --no-compress \
  --lv-font-name lv_font_ui_16 --lv-include lvgl.h \
  -o _lv_font_ui_16.c
# 20 / 24 同理（--size 20 / 24，--lv-font-name lv_font_ui_20 / lv_font_ui_24）
```

生成后直接 `cp` 进 `app/robot_ui/`（变量名由 `--lv-font-name` 直接给定，
原来 `ui_font_16 → lv_font_ui_16` 的那步 `sed` 已经不需要了）。
`line_height` / `base_line` 和原来一模一样（16/19/3、20/22/4、24/25/4），
**字号和行距没动，只扩了字符集**。

> **别改成 `--range`**：GB2312 的 6763 个码点在 Unicode 上是散的，展开成连续区间
> 要 3555 段、命令行 44 KB，超过 Windows CreateProcess 的 32 KB 上限；
> `--symbols` 只要 7 KB（7178 个 UTF-16 码元）。

### 三档字库各多大（`.rodata` 实测，`arm-none-eabi-size -A`）

| 档 | C 源文件 | 旧 `.rodata`（971 字） | 新 `.rodata`（7010 字） | 增量 |
|----|----------|----------------------|----------------------|------|
| `lv_font_ui_16` | 5,895,482 B | ~111 KB | **945,537 B**（bitmap 820,127 + dsc 112,160 + cmap 13,250） | +835 KB |
| `lv_font_ui_20` | 8,647,457 B | ~167 KB | **1,403,058 B**（bitmap 1,277,648 + dsc 112,160 + cmap 13,250） | +1.24 MB |
| `lv_font_ui_24` | 11,585,160 B | ~229 KB | **1,895,118 B**（bitmap 1,769,708 + dsc 112,160 + cmap 13,250） | +1.67 MB |
| 合计 | 26,128,099 B | ~507 KB | **4,243,713 B（4.05 MB）** | **+3.74 MB** |

整体 flash：**2,184,592 B (13.02%) → 5,930,240 B (35.35%)**（+3,745,648 B ≈ 3.57 MB），
16 MB 里还剩 10 MB 出头。sram 基本没动（228,376 → 228,440 B，+64 B）；psram 仍为 0。

### 必须同时开 `CONFIG_LV_FONT_FMT_TXT_LARGE=y`

20 px / 24 px 的 4bpp 位图分别是 1.28 MB / 1.77 MB，超过 LVGL 默认
`lv_font_fmt_txt_glyph_dsc_t.bitmap_index` 的 **20 位（1 MB）** 上限，不开就是：

```
error: "Too large font or glyphs in LV_FONT_UI_20.
        Enable LV_FONT_FMT_TXT_LARGE in lv_conf.h"
```

所以 `defconfig` 末尾加了这一行（附带 ASCII-only 注释，理由见上面
`CONFIG_MIMO_API_KEY` 那段关于 CMake `file(STRINGS)` 丢非 ASCII 的警告）。
代价是 `lv_font_fmt_txt_glyph_dsc_t` 从 8 字节变成 16 字节，
每档多 `7010 × 8 ≈ 112 KB`（已含在上表里）。

## 显示前过滤渲染不了的东西（`app/robot_ui/main.c`）

新增 `sanitize_for_display()`：送 `lv_label` 之前先把字库渲染不了的码点丢掉、
把 Markdown 降级成纯文本。

- 丢：emoji（U+1F000 以上）、杂项符号/装饰符（U+2600-U+27BF）、
  几何图形与制表符（U+2500-U+25FF）、带圈数字与技术符号（U+2300-U+24FF）、
  箭头只留字库里有的 ←↑→↓、变体选择符/零宽字符（U+FE00-U+FE4F、
  U+2000-U+206F 里除常用标点之外的全部）、Latin-1 里那堆重音字母。
- Markdown：`*` 和反引号去掉、`__` 去掉（单个 `_` 留着，别拆 `ai_audio`）、
  行首 `#`/`>` 连它后面的空白一起去掉、行首 `- `/`+ `/`* ` 列表符号换成 `· `。
- 换行保留（连续 3 个以上压成 2 个），制表符换成空格，同一行里的连续空格压成一个
  （丢掉 emoji 之后很容易留下双空格，比如 `- 📱 天气` → `· 天气`）。

### 验证（不用上板）

清洗是个纯函数，抽出来在 PC 上跑了一组用例（25 条，含真机现场那条回复）：

```bash
cd /home/youdian/contest2026_233_daimazenmepaibudui   # 脚本从 main.c 里抽函数
python3 <scratch>/_check_sanitizer.py                 # 生成 /tmp/san_test.c
gcc -std=c99 -Wall -Wextra -o /tmp/san_test /tmp/san_test.c && /tmp/san_test
```

覆盖：emoji / ZWJ 家庭序列 / 国旗、变体选择符、带圈数字、三种列表符号、
标题号/引用号、反引号与 `__`、单个 `_` 保留、连续换行、CRLF、制表符、
字库里有的箭头与标点保留、控制字符、空串、`out_cap` 截断不越界。
**这个测试还抓到了两个真 bug**（只改 `cp` 不改拷贝源，导致制表符照样输出成制表符；
`·` 被自己的过滤规则判成"没有字形"），已在实现里修掉。

**只洗显示用的那一份**：`voice_speak_reply()` 里 `shown` 给界面、
`response` 原样给 `voice_tts_speak()`；MQTT 的 `ai_reply` 动作也是先洗再
`robot_ui_set_ai_reply()`。

已知局限：GB2312 之外的生僻汉字（GBK 独有的字）仍会显示成方块 ——
在设备上精确判断「这个字在字库里吗」要带一张表，这里只处理可枚举的几个符号区。

## TTS 卡住 + 界面状态不实（`app/hello_app/mimo_voice.c` + `main.c`）

| 改动 | 说明 |
|------|------|
| `MIMO_TTS_TEXT_MAX = 120` 字 + `tts_truncate()` | 按 UTF-8 码点截断，末尾补 `…`，日志打 `TTS: 合成文本过长（N 字节），只念前 120 个字` |
| `MIMO_TTS_RESP_CAP` 1 MB → **1.5 MB** | 实测约 11 KB/字，120 字 ≈ 1.32 MB（占 cap 86%）。原来的 1 MB 装不下 130 字左右的回复 |
| `b64_decode_inplace()`（替代 `b64_decode_alloc()`） | base64 就地解码，省掉一份 3/4 大小的解码缓冲（mbedtls 的写指针永远落后于读指针，原地安全） |
| `wav_extract_16k_into()` | 重采样直接写进调用方的 PCM 缓冲，省掉 pcm16 临时缓冲和第二遍 memcpy；装不下就填满前一段（语义同原「装得下多少给多少」） |
| `mimo_alloc_logged()` / `mimo_calloc_logged()` / `mimo_free_logged()` | ASR/chat/TTS 每步的分配和释放都打字节数，失败打 `LOG_ERR` |
| 请求体不再先 malloc 一份 `esc` | TTS/chat 的 JSON 转义直接写进 body，各少一次 malloc+free |
| 响应找不到 `audio.data` | 明确 `-EPROTO` 返回并打日志（响应被 cap 截断时就是这样），不再让调用方以为还在合成 |
| `main.c` `voice_speak_reply()` | 合成期间显示「正在合成语音…」；`audio_play_start()` 返回 0 之后才显示「正在播放…」；失败显示 `语音合成失败 (errno)` / `播放失败 (errno)` |

峰值对比（TTS 这一路，一轮里依次发生、不是同时）：

| | 旧 | 新 |
|---|---|---|
| resp | 1 MB | 1.5 MB |
| base64 解码缓冲 | 0.75 MB（独立 malloc） | 0（就地） |
| 16k PCM 临时 | 0.5 MB（独立 malloc）+ 一次 memcpy | 0（直接写进调用方缓冲） |
| **合计** | **≈ 2.25 MB** | **≈ 1.5 MB + 调用方 256 KB** |

### 为什么没做「大块缓冲池复用」

任务里提到「别在 calloc/free 之间反复要同样大小的大块（碎片），能复用就复用」。
本轮没有引入常驻的响应缓冲池：那样会把峰值换成**永久占用**（1.5 MB 一直拿在手里），
和同一条里「用完立刻 free」相冲突，而且 `mimo_voice.c` 的三个入口都在
`voice_tts`/`voice_asr` 的分发层后面，不能假设调用方一定串行。改为**就地复用**
（解码原地、重采样直接写调用方缓冲）+ 缩小块大小，同样把分配次数和峰值都降下来了。
如果上板后仍然观察到 PSRAM 碎片问题，再考虑加带锁的 scratch 池。

---

# vela_tls 连接池的槽位被跨 task group 复用 → 误关别人的 fd → TLS 读到别人的数据流（2026-09-14 新增补丁）

## 现象

20:10 那次**提醒人声没出声**（到点该播的播报丢了）。日志里这一轮的最后一条
TLS 记录是握手失败：

```
[vela_tls] Handshake start: Host=..., UNIX=...
[vela_tls] ssl_handshake ret=-0x7200: <错误串>
```

而且是**发出去约 40 秒之后**才失败（不是立刻），界面这一侧的播报已经过去了。

## 根因

`src/infra/vela_tls.c` 里的连接池 `static conn_slot_t s_pool[CONN_POOL_SIZE]`
（池大小 2，`CONFIG_AI_AGENT_TLS_CONN_POOL_SIZE=2`）是**全机共享**的静态对象，
槽位里存的是一个**裸 fd 号**（藏在 `ctx.net.fd`）。而：

- NuttX 的 **fd 号只在 task group 内有意义**：每个 group 的 fd 表各自从 3 开始编号；
- 整机是**单一大镜像**：`ai_agent`、`hello_app`、`robot_ui` 是三个**独立 task group**
  （defconfig 里 `CONFIG_EXAMPLES_AI_AGENT_VELA=y` / `CONFIG_LVX_USE_CONTEST2026_233_HELLO_APP=y` /
  `CONFIG_LVX_USE_CONTEST2026_233_ZHI_AI=y` 各是一个 app），但它们共享同一份 static 变量、
  同一份函数代码 —— 于是 A 组的代码能拿到 B 组放进去的槽位。

`vela_tls_pool_cleanup()` 被 `app/hello_app/mimo_voice.c:784`（`mimo_post()`，
**每次 ASR / TTS / chat 请求前**都调）、`mimo_voice.c:2473`（天气 GET 前）、
`mimo_location.c:522` 调用 —— 它跑在**调用者自己的组**里，里面 `tls_ctx_free()`
→ `close(fd)`，也就是拿 B 组的 fd 号在 A 组里关。两种坏结果：

1. 该号在 A 组不存在 → `close()` 静默 `EBADF`，对端的 keep-alive 永远不回收；
2. 该号在 A 组**恰好被另一个活着的文件占着**（MQTT socket、push socket、音频设备、
   lcd…全都从 3 开始编）→ **误关别人的 fd**；随后 A 组下一次 `socket()` 极可能拿回
   同一个号，别的持有者（`network_task` 每 100ms 的 `recv`、每 30s 的心跳 `send`）
   继续用这个号收发，就和这条 TLS 连接**共享同一个 socket** → mbedtls 读到的是别人
   的协议字节流 → `ssl_handshake ret=-0x7200`（`INVALID_RECORD`）。那条连接上
   服务端 keep-alive 的数据被对端任务读走/自己的读被别人的流污染，所以表现为
   "发出去之后卡一会儿才失败"，正是提醒播报那一轮。

同类教训工程里已经记过一次：`app/robot_ui/network_comm.c:477-482` 写着
"跨任务 close() 会和 network_task 里的 recv() 抢同一个 fd（关掉后 fd 号立刻被复用），
**实测会把整机打复位**"。

## 修改

| 位置 | 改动 |
|------|------|
| `conn_slot_t` `:164` | 新增 `pid_t owner` —— 槽位归属的 task group |
| `pool_owner()`（新增 `:178`） | 用 `getpid()`。NuttX 下它就是 **group 的 pid**（`group_create()` 里 `tg_info->ta_pid = tg_pid`，`libs/libc/sched/task_getpid.c` 读它；`pthread_create()` 走 `group_bind()` 继承创建者的 group、不新建 group），所以同组各线程一样、跨组不一样，正好是"fd 属于哪个组"的判据 |
| `pool_acquire()` `:184` | ① 只复用 `owner == 本组` 的槽位（别人的 fd 绝不借来用）；② `valid==false` 的槽位（ctx 早就 free 过、fd 已关）可以被本组**认领**，换个 owner 是安全的；③ **只 evict 本组自己的空闲槽位**，本组没有可用槽位就返回 NULL 走临时连接 —— **绝不 evict 别人的槽位**，那等于在别人的组里 close 别人的 fd |
| `vela_tls_pool_cleanup()` `:274` | 只释放 `owner == 本组` 的槽位（`in_use` 的照旧跳过），别人的留给它们自己的主人 |
| `vela_https_request()` 的 `pool_reconnect:` `:901` | 原来**无锁**地改 `slot->valid/ctx`，收进 `s_pool_lock`（理由写在注释里：槽位字段的改动路径必须一致，不然是"用没保护的状态指挥别人"） |
| 新增 `s_req_lock` + `req_lock_try_acquire()` / `req_lock_release()` `:809` | 给**一次 HTTPS 请求**套一层串行，覆盖 `pool_acquire → connect → write → read → release`；**带超时的 trylock，拿不到就不串行继续跑** |

### 为什么用「带超时的 trylock」而不是老实 `pthread_mutex_lock`

- **嵌套会自死锁**：本构建 `CONFIG_PTHREAD_MUTEX_TYPES=y`、默认类型是
  `PTHREAD_MUTEX_NORMAL`（非递归），NuttX 对"同线程再次 lock"是**故意死锁**的
  （`pthread_mutex_timedlock.c` 注释：`is required to deadlock for the case of the
  non-robust NORMAL mutex`）。今天这条路**没有嵌套**（顺调用链查过：内部只走
  `pool_acquire` / `tls_ctx_connect` / `tls_write_request` / `tls_read_response` /
  `pool_release` + `proxy_open_tunnel()` + `network_acquire_resource()`，这些都不回调
  `vela_https_*`；上层 `llm_chat` / `llm_chat_tools` / 各 tool / `mimo_post` 都是
  "这一次发完再发下一次"），但以后很可能变成"工具调用里再发一次 HTTPS"，那时同线程
  重入就是永久卡死一块板子。用 trylock 就永远不会死等。
- **等锁必须有上界**：这把锁覆盖的区间里有 120s 读超时 / 30s 写超时（LLM 流式响应
  真会占满），而提醒人声、语音播报这种"说到就到"的路径不能被它拖住。所以只等
  `REQ_LOCK_WAIT_MS`（500ms）就放弃、退化成并行发 —— 并行是安全的（池按 owner 隔离、
  raw buffer 自带 trylock），只是少一层保险。

### 为什么**不**把 `vela_tls_pool_cleanup()` 也塞进这把锁

- **正确性不需要**：请求正在用的槽位 `in_use==true`（`pool_acquire` 里在
  `s_pool_lock` 下置位、`pool_release` 清），cleanup 遇到 `in_use` 本来就跳过，
  所以并发 cleanup 不可能 free 掉一个正在使用的槽位；两边改槽位字段又都在
  `s_pool_lock` 下，不会撕裂。
- **代价很实在**：`mimo_post()` 是"先 cleanup 再 request"。如果 cleanup 也抢这把锁，
  语音链路上一次"立刻要出声"的请求就得先等一个正在跑的 LLM 请求（可能上百秒）放下
  锁，白白把瞬间返回的调用变成长期阻塞。
- **owner 隔离之后剩下的唯一交错**是"同组另一个线程刚要 acquire 的槽位被清了"，
  最坏结果只是多一次握手（`pool_acquire` 看到 `valid==false` 会重连），无害。

## 应用方式

```bash
cd <openvela 工作区>/packages/ai_agent
git apply <本仓库>/patches/apps-ai-agent-vela-tls-chunked-end.patch
git apply <本仓库>/patches/apps-ai-agent-vela-tls-no-block-close.patch
git apply <本仓库>/patches/apps-ai-agent-vela-tls-pool-owner.patch
```

三个补丁都改 `src/infra/vela_tls.c` 的**不同位置**，按上面顺序应用。
（本补丁的基线是"前两个补丁已应用"的工作区，单独应用 **不能**跳过前两个。）

## 验证

- `syntax_check.sh packages/ai_agent/src/infra/vela_tls.c` → `[OK] 0 error`
  （该包在 `compile_commands.json` 里有真实编译命令，`-I` / `-D` / `-march` 与固件一致）。
- `git apply --check --reverse` 通过（补丁与已改好的工作区逐字对应）。
- 补丁链自检：`git show HEAD:src/infra/vela_tls.c` 依次打上 chunked-end、
  no-block-close、pool-owner 三个补丁后，与当前工作区文件 `cmp` **逐字节相同**。
- **没上板**：板子当时卡死、串口静默（主 agent 在全程录串口），本轮只做语法预检
  和补丁一致性检查，没有烧录、没跑完整构建。上板要看的是：提醒到点时人声能出声，
  且日志里不再出现 `-0x7200`。

## vendor_sifli-uart-rx-dma-reinit.patch（2026-09-17 新增：串口 RX 被 DMAC1 复位抹掉的补救）

**`cd` 到 `vendor/sifli`，`git apply` 之。目标文件：`chips/sf32lb52/sifli_uart.c`。**

### 现象（真机实测）

板级音频恢复里那一记为"麦克风跑一阵就永久聋"打的是**复位整块 DMAC1**
（`HAL_RCC_ResetModule(RCC_MOD_DMAC1)`，见 `board/contest_board/src/sf32lb52_audio.c`
的 L1/DMAC1 那一级）。打完这一记**麦克风确实活了**（实测 `irq+3 half+4`），
但**串口控制台从此收不到任何命令**：板子还活着（`[ui] 慢统计`、MQTT 日志照打），
可 `nsh` 不回显、不执行 —— 现场看起来像"整机卡死"，其实是**输入通路死了**。

### 根因（仪表定案，三层）

1. **DMAC1 上不只挂音频**：`UART1_DMA_TX_INSTANCE = DMA1_Channel6`、
   `CONFIG_BSP_UART1_RX_USING_DMA=y` —— 控制台串口的 **RX DMA 也在 DMAC1 上**，
   模块级复位把它一并抹掉。
2. **本驱动的 RX 只在 `sifli_attach()` 武装一次**（`sifli_dma_receive()` 全树唯一
   真实调用点就是 attach 那一处，循环模式之后靠 DMA 自己跑）→ 被抹之后就**再也
   没有任何代码路径去重装**。
3. 即使板级主动去调 `sifli_dma_receive()` 重装，**也会被静默跳过**：
   `HAL_UART_Receive_DMA()` 的守门条件是
   `if (huart->RxState == HAL_UART_STATE_READY)`，而 attach 之后它一直是
   `HAL_UART_STATE_BUSY_RX`（UART 外设没被复位，这个**软件状态**自然也不回 READY）
   → 函数什么都不做却返回 `HAL_OK`。仪表实测：`CCR.EN=0 / CNDTR=0 / CPAR=0 / CM0AR=0`
   （= 传输根本没启动），返回值却是 0。

### 修改

`chips/sf32lb52/sifli_uart.c` 里在 `sifli_dma_receive()` 之后新增一个非 static 入口
（板级用，不改任何现有行为）：

```c
int sifli_uart_reinit_rx_dma(int idx)
{
    ...
    uart->handle.RxState = HAL_UART_STATE_READY;   /* ★ 不摆正，Receive_DMA 进不了门 */
    memset(uart->serial.recv.buffer, 0, uart->serial.recv.size);  /* 清残影，防重放旧命令 */
    uart->serial.recv.head = uart->serial.recv.tail = 0;          /* 环形指针归零 */
    if (uart->handle.hdmarx && uart->handle.hdmarx->State != HAL_DMA_STATE_READY)
      {
        HAL_DMA_DeInit(uart->handle.hdmarx);      /* 先还通道回池，否则 Init 会 HAL_BUSY */
      }
    return sifli_dma_receive(&uart->serial);      /* 与 attach 同一条路 */
}
```

板级在"复位整块 DMAC1"**之后**调 `sifli_uart_reinit_rx_dma(0)`（UART1 控制台）与
`(1)`（UART2 调试口），见 `board/contest_board/src/sf32lb52_audio.c` 的 DMAC1 那一级。

**三步都不能少**：`RxState` 摆正（否则静默跳过）；清缓冲 + 归零 `head/tail`
（否则驱动把复位前缓冲里的旧字节当成新输入上报，`nsh` 会**反复重放一条几分钟前的
命令** —— 实测刷了 125 KB 同一条命令）；`DeInit` 还通道（否则 `HAL_DMA_Init` 撞上
动态通道池的状态、返回 HAL_BUSY 却没人看）。

### 验证

同一次 `audio_test audfix`（它打的就是那一记）前后：

```
修法前： CCR=0x20a0 EN=0  CNDTR=0    CPAR=0x0        CM0AR=0x0        State=READY(0x1)
修法后： CCR=0x20af EN=1  CNDTR=1024 CPAR=0x50084024 CM0AR=0x2002d880 State=BUSY(0x2)
之后：   nsh> hw_test status   ← 有回显、有输出 ✓
```

### ⚠️ 遗留（下轮要专门验）

真机出现过一次"开机之后控制台变聋"，怀疑与 `head/tail = 0` 那两行有关（NX 环形的
记账被我们直接归零）。下轮的验收步骤：干净启动 → 敲命令（应正常）→
`audio_test audfix`（打一记）→ 再敲命令（应正常）→ 跑十几分钟看是否中途变聋。
若变聋，改成只清 `last_index`、不碰 NX 记账。
