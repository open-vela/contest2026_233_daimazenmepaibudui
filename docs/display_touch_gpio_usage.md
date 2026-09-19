# SF32LB52 显示 / 触摸 / 按键 / GPIO / 定时器 / PWM / I2C / ADC 使用说明

> 智爱陪伴 —— openvela 板级硬件接口（成员一交付物）
> 硬件：SF32LB52-DevKit-LCD（1.85" AMOLED：**CO5300 屏 + FT6146 触摸**）
> 状态：显示与触摸已在真机验证（`robot_ui` 触摸 UI）；本文档的节点名、引脚号全部
> 从板级源码（`board/contest_board/`）核实，不是抄来的
> 自检命令：`hw_test`（见第 10 节）

## 1. 概述

这一层**全部是标准 openvela/NuttX 设备节点，没有任何私有 ioctl**。
应用只需要按 NuttX 标准接口 `open` / `read` / `write` / `ioctl` 用就行。

两条推荐路线：

| 需求 | 推荐做法 |
|------|---------|
| 做界面 | **用 LVGL 内建 NuttX 后端**（`lv_nuttx_init()`），它自己接管 `/dev/lcd0` + `/dev/input0`，**应用不要直接碰驱动** |
| 做逻辑 | 直接读 `/dev/input0`（触摸）、`/dev/gpioN`，或走 LVGL 回调；**按键走板级 `sf32lb52_boardbtn`（GPIO 轮询），不要读 `/dev/buttons`**，见第 5 节 |

屏幕是 **390x450、RGB565**，走 `LV_USE_NUTTX_LCD`（`/dev/lcd0`）+ 分块缓冲。

## 2. 设备节点总表

| 节点 | 用途 | 接口类型 | 头文件 |
|------|------|---------|--------|
| `/dev/lcd0` | AMOLED 显示（主接口） | NuttX LCD dev：`open` + `ioctl(LCDDEVIO_GETVIDEOINFO / GETPLANEINFO / GETAREAALIGN / PUTAREA)` | `<nuttx/lcd/lcd_dev.h>` |
| `/dev/fb0` | 同一块屏的 framebuffer 前端 | NuttX fb：`open` + `ioctl(FBIOGET_*)` | `<nuttx/video/fb.h>` |
| `/dev/input0` | FT6146 触摸（`touch_register(...,16)`） | touchscreen upper half：`open` + `read()` 得到 `struct touch_sample_s` | `<nuttx/input/touchscreen.h>` |
| `/dev/buttons` | 按键（Key2 = PA11）。**节点在，但别用**：`poll()`/`read()` 在真机上会让整机静默卡死（见第 5 节）；按键改用板级 `sf32lb52_boardbtn`（PA11/PA34 当 GPIO 轮询） | buttons upper half：`open` + `read()` 得到 `btn_buttonset_t` | `<nuttx/input/buttons.h>` |
| `/dev/gpio0` | GPIO **输入**（PA34 / Key1 电源键脚） | 通用 GPIO：`read` / `write` + `ioctl(GPIOIOC_GETPINTYPE / SETPINTYPE)` | `<nuttx/ioexpander/gpio.h>` |
| `/dev/gpio1` | GPIO **输出**（PA26，板级唯一的用户输出脚） | 同上 | 同上 |
| `/dev/gpio2` | GPIO **中断输入**（PA34） | 同上 | 同上 |
| `/dev/timer0` | 定时器 | `ioctl(TIMERIOC_SETTIMEOUT / START / STOP / GETSTATUS)` | `<nuttx/timers/timer.h>` |
| `/dev/pwm0` | PWM（背光 PA01 = GPTIM1_CH4） | `ioctl(PWMIOC_SETCHARACTERISTICS / START / STOP)` | `<nuttx/timers/pwm.h>` |
| `/dev/i2c0` | I2C master：触摸 FT6146（SCL=PA30 / SDA=PA33，400kHz） | `ioctl(I2C_TRANSFER)` | `<nuttx/i2c/i2c_master.h>` |
| `/dev/i2c1` | I2C master（I2C2：PA39/PA40，挂 LSM6DS3 时用） | 同上 | 同上 |
| `/dev/adc0` | ADC | `read()` 得 `struct adc_msg_s` | `<nuttx/analog/adc.h>` |
| `/dev/audio/audio0` | 音频播放 / 录音 | NuttX audio：`ioctl(AUDIOIOC_CONFIGURE / START / STOP / GETCAPS)` + `read/write` | `<nuttx/audio/audio.h>` |
| `/dev/eth0` | USB RNDIS 网卡 | POSIX socket | 见 `docs/network_api_usage.md` |

> ⚠️ 音频节点**带子目录**：是 `/dev/audio/audio0`，不是 `/dev/audio0`。
> 驱动里写的是 `audio_register("audio0", ...)`（`board/contest_board/src/sf32lb52_audio.c:1205`），
> NuttX 的 audio upper half 会自动拼成 `/dev/audio/<name>`。

板级注册这些节点的位置（想改驱动先看这里）：

| 节点 | 注册代码 |
|------|---------|
| `/dev/adc0` | `sifli_ap.c:400` |
| `/dev/timer0` | `sifli_ap.c:440` |
| `/dev/pwm0` | `sifli_ap.c:464` |
| `/dev/buttons` | `sifli_ap.c:475` |
| `/dev/i2c0` / `/dev/i2c1` | `sifli_ap.c:497` / `sifli_ap.c:532` |
| `/dev/gpio0..2` | `sifli_gpio.c:365` `sifli_gpio_initialize()`（按 in → out → int 顺序注册，所以 0=in / 1=out / 2=int） |
| `/dev/audio/audio0` | `sf32lb52_audio.c:1205` |
| `/dev/lcd0` + `/dev/input0` | LCD 异步初始化线程 `sifli_ap.c:303` `lcd_async_init_thread()` → `board_lcd_initialize()` + `ft6146_touch_initialize()` |

## 3. 显示：`/dev/lcd0`

### 3.1 推荐路线：交给 LVGL（应用不要直接碰 `/dev/lcd0`）

`CONFIG_LV_USE_NUTTX=y` + `CONFIG_LV_USE_NUTTX_LCD=y` 时，LVGL 有内建的 NuttX LCD 后端，
`lv_nuttx_init()` 会自己打开 `/dev/lcd0`（默认路径就在 `lv_nuttx_dsc_init()` 里），
应用只写 `lv_*` 控件：

```c
#include <lvgl/lvgl.h>

lv_init();

lv_nuttx_dsc_t   dsc    = {0};
lv_nuttx_result_t result = {0};

lv_nuttx_dsc_init(&dsc);      /* 默认 lcd_path="/dev/lcd0", input_path="/dev/input0" */
lv_nuttx_init(&dsc, &result);
if (result.disp == NULL)
  {
    printf("LVGL display init failed\n");
    return;
  }

/* 触摸读取周期默认偏慢，手感会"点不动"；robot_ui 就是这么改成 10ms 的 */
if (result.indev != NULL)
  {
    lv_timer_set_period(lv_indev_get_read_timer(result.indev), 10);
  }

/* 之后必须**在同一个线程**里跑主循环（见第 9 节：LVGL 非线程安全） */
while (1)
  {
    lv_timer_handler();
    usleep(5 * 1000);
  }
```

参考实现：`app/robot_ui/main.c:144-160`。

LVGL 相关开关（`board/contest_board/configs/sf32lb52_ai/defconfig`）：

```
CONFIG_LV_USE_NUTTX=y
CONFIG_LV_USE_NUTTX_LCD=y
CONFIG_LV_USE_NUTTX_TOUCHSCREEN=y
CONFIG_LV_NUTTX_LCD_CUSTOM_BUFFER=y     # 分块缓冲 + PARTIAL 渲染，只刷脏区
CONFIG_LV_NUTTX_LCD_BUFFER_SIZE=60      # 单位是行数
```

### 3.2 不用 LVGL：直接操作 `/dev/lcd0`

只读拿信息（安全，任何时候都能跑）：

```c
#include <nuttx/lcd/lcd_dev.h>
#include <nuttx/video/fb.h>
#include <sys/ioctl.h>
#include <fcntl.h>

struct fb_videoinfo_s     vinfo;
struct fb_planeinfo_s     pinfo;
struct lcddev_area_align_s align;
int fd = open("/dev/lcd0", O_RDWR);
if (fd < 0) { /* 打不开就别继续 */ }

ioctl(fd, LCDDEVIO_GETVIDEOINFO, (unsigned long)&vinfo);   /* 分辨率 / fmt / planes */
ioctl(fd, LCDDEVIO_GETPLANEINFO, (unsigned long)&pinfo);   /* fbmem / stride / bpp */
ioctl(fd, LCDDEVIO_GETAREAALIGN, (unsigned long)&align);   /* PUTAREA 的对齐要求 */

printf("%dx%d fmt=%d bpp=%d stride=%u\n",
       vinfo.xres, vinfo.yres, vinfo.fmt, pinfo.bpp, (unsigned)pinfo.stride);
```

写一块区域（把 `[row0,row1]` 行刷成一个颜色，RGB565 小端）：

```c
/* 一次只提交几行，别一次 malloc 整屏（390*450*2 = 351KB） */
uint8_t *buf = malloc(vinfo.xres * 60 * 2);
uint16_t color = 0xf800;                    /* 红 */

for (int i = 0; i < vinfo.xres * 60; i++)
  {
    buf[i * 2]     = (uint8_t)(color & 0xff);
    buf[i * 2 + 1] = (uint8_t)(color >> 8);
  }

struct lcddev_area_s area;
memset(&area, 0, sizeof(area));
area.row_start = 0;
area.row_end   = 59;
area.col_start = 0;
area.col_end   = vinfo.xres - 1;
area.stride    = (uint32_t)vinfo.xres * 2;  /* 一行多少字节 */
area.fmt       = vinfo.fmt;                 /* 用 GETVIDEOINFO 拿到的 fmt 回填 */
area.data      = buf;

ioctl(fd, LCDDEVIO_PUTAREA, (unsigned long)&area);
free(buf);
close(fd);
```

> `row_end` / `col_end` 按"窗口含端点"理解（`hw_test lcdcolor` 就是这么用的）。
> 如果刷不满，先看 `GETAREAALIGN` 给的 `row_start_align / height_align / width_align`，
> 把起止行列对齐到它要求的值。

### 3.3 屏幕亮度：板级封装 `backlight_set()` / `backlight_get()`

调亮度**不要**自己 open `/dev/lcd0` 拼 ioctl，用板级封装
`board/contest_board/src/sf32lb52_backlight.{h,c}`：

```c
#include "sf32lb52_backlight.h"   /* 头文件在 board/src，app 的 CMakeLists 里加 INCLUDE_DIRECTORIES */

int ret = backlight_set(100);     /* 0 = 最暗（关屏），100 = 最亮（全亮） */
if (ret != OK)
  {
    printf("backlight_set failed: %d\n", ret);
  }

int now = backlight_get();        /* 回读驱动侧真实状态，0..100；负值是负 errno */
printf("brightness = %d%%\n", now);
```

现在 **0..100 全档都能调**（百分比亮度走面板自己的亮度寄存器）：

| 调用 | 结果 |
|------|------|
| `backlight_set(0)` | 真的把面板关掉（`LCDDEVIO_SETPOWER` → `DisplayOff`），返回 `OK` |
| `backlight_set(100)` | 面板全亮（`LCDDEVIO_SETCONTRAST(100)` + 确保 `DisplayOn`），返回 `OK` |
| `backlight_set(1..99)` | 下发百分比亮度，返回 `OK` |
| `backlight_set(其他)` | `-EINVAL` |
| `backlight_get()` | 优先 `ioctl(LCDDEVIO_GETCONTRAST)` 回驱动侧真实亮度（关屏时报 0） |

链路（**依赖下面那个 vendor 补丁**）：

```
应用 -> backlight_set() -> ioctl(LCDDEVIO_SETCONTRAST, percent)
     -> sf32lb_lcd_setcontrast() -> p_ops->SetBrightness()
     -> co5300.c 的 LCD_SetBrightness() -> 写 0x51 WBRIGHT
```

### 3.3.1 前提：必须打上 `patches/vendor_sifli-lcd-brightness.patch`

`sf32lb_lcd.c` 在 openvela 的 **vendor 公共树**里，不在本仓库。上游那两跳是断的：

- `LCDDEVIO_SETCONTRAST` → `sf32lb_lcd_setcontrast()` 直接 `return -ENOSYS`（`GETCONTRAST` 同理）；
- CO5300 面板**确实有**真亮度寄存器（0x51 `WBRIGHT`，`co5300.c:544` 的
  `LCD_SetBrightness()` 已经写好，入参就是百分比），但它只挂在
  `LCD_DrvOpsDef.SetBrightness` 回调上，`sf32lb_lcd.c` **从来不调它**，
  也没暴露成任何 ioctl —— 从 `/dev/lcd0` 够不着。

补丁把 `SETCONTRAST` / `GETCONTRAST` 接到 `p_ops->SetBrightness()`：

```bash
cd <openvela 工作区>/vendor/sifli
git apply <本仓库>/patches/vendor_sifli-lcd-brightness.patch
```

**没打补丁的树仍然只有 0 / 100 是真的**（`SETCONTRAST` 回 `-ENOSYS`）：
`backlight_set(1..99)` 会**如实返回 `-ENOSYS` 且不碰硬件**，
`backlight_get()` 退化成用 `GETPOWER` 归一化成 0 / 100。不是假成功，别怀疑。

为什么不能拿 `LCDDEVIO_SETPOWER` 冒充百分比：本板的
`sf32lb_lcd_setpower()`（`sf32lb_lcd.c:634`）实现是
`power > 0 ? DisplayOn : DisplayOff`；**power 的数值只被存下来给 `GETPOWER`
回读，不影响亮度**。所以传 30 和传 100 一样亮 —— 那正是"假亮度"的坑。

> 背光脚 PA01 虽然被 pinmux 成 `GPTIM1_CH4`（`bsp_pinmux.c:227`），但
> `CONFIG_PWM` 没开（`/dev/pwm0` 不存在），也没有代码配 GPTIM1 占空比；
> 本方案**没有**走它，不需要开 PWM。
>
> 自测：`hw_test lcd 0 / 30 / 60 / 100` 逐档看屏幕变化（默认 100，见第 10 节）。

**踩过的坑**：

- **板级封装自己把中间值挡掉了**（2026-09-13 修）：`sf32lb52_backlight.c` 里曾经
  有一句"本板只有 0/100 是真的，中间值直接 `return -ENOSYS`，**不往下发 ioctl**"
  的短路，所以 `hw_test lcd 30` 看到的 `-38` 是**封装自己编的**，跟驱动补丁没关系
  ——补丁早就打上、编进去了（`objdump` 能在 `final_nuttx` 里看到
  `sf32lb_lcd_setcontrast` 里对 `p_ops->SetBrightness` 的判空与调用）。排错时
  **别只看 `-ENOSYS` 就等于"驱动没实现"**：先确认那一跳有没有真的发出去
  （日志里有没有 `BACKLIGHT: SETCONTRAST(n) failed`）。
- **跨任务缓存 fd**（同一天紧接着踩的）：修好上面那条之后，
  `hw_test lcd 30` 单跑 PASS，紧接着 `hw_test lcd 60` 却报
  `BACKLIGHT: SETCONTRAST(60) failed: 9`（`EBADF`），而且 `设置前` 打印的
  30 是模块内缓存的旧值、不是驱动回读。根因是封装用一个**模块级 `static int`
  缓存 open 出来的 fd**，而 NuttX 的 fd 属于 task group：NSH 每条命令是一个
  任务、`robot_ui` 又是另一个任务，A 任务 open 的 fd 数字在 B 任务里无效
  （更糟时会指向 B 的另一个文件）。现在封装改成**每次调用 open/close、
  不保存任何 fd 和跨任务状态**。用 `backlight_set()` 时不必再考虑调用方是哪个
  任务。
- `LCDDEVIO_SETCONTRAST` 的参数在本驱动里按**亮度百分比 0..100** 解释，
  **不是** NuttX 惯例的 `0..CONFIG_LCD_MAXCONTRAST`（本板该宏是 **63**，
  照它走会把 100% 挡掉）。所以补丁里用自己定义的 100 做上界校验。
- 面板只有 `0x51 WBRIGHT` 写口、**没有回读通路**（CO5300 的 `0x52 RBRIGHT`
  在本驱动里没有读函数），所以 `GETCONTRAST` 回的是驱动里"最近一次下发的值"，
  关屏时按 0 报；`backlight_get()` 没有"上次设了多少"的缓存层，拿不到
  `GETCONTRAST` 就退回 `GETPOWER` 归一化成 0/100。

**坑**：

1. `backlight_set()` 每次调用都会 `open("/dev/lcd0")` 再 `close`（不缓存 fd）。
   驱动侧 `lcddev_open` 只在 crefs 0->1 时动作、本驱动也没实现 `dev.open`，
   所以这一对 open/close 很便宜。`robot_ui`(LVGL) 正接管屏幕时仍然别乱调：
   `backlight_set(0)` 会把 LVGL 的画面一起关掉（那是同一块面板）。
2. 别自己写 `ioctl(fd, LCDDEVIO_SETPOWER, 50)` 想"调暗一点"——那是自欺：
   `GETPOWER` 会回你 50，但屏幕亮度一点没变。
3. 别用 `/dev/pwm0` 调背光：本配置 `CONFIG_PWM=n`，节点根本不存在
   （第 7.2 节那段示例只在 `CONFIG_PWM=y` 时可用）。

### 3.4 黑屏救回：面板重新初始化 `hw_test lcdreinit`

#### 3.4.1 它治的是哪一种黑屏

**整屏黑，但应用一切正常**：LVGL 还在响应触摸、还在以 20~30 帧/s 往面板推画面
（`[ui]` 计时仪表能看到 `最慢 flush 10 ms`），`hw_test lcd 80` 下发亮度也成功、
能回读，可屏幕就是不亮。`reset` 无效，**只有真断电才恢复**（有时过一会儿自己好）。

复现规律：开机后第一次点「提醒」/「主菜单」容易黑；短时间快速点右上角菜单会频繁
黑闪、最后长时间黑屏 —— 像是**面板自己丢了配置**（一次是偶发，反复操作会累积），
不是 CPU / LCDC / 应用挂了。所以救法是**重发一遍面板初始化序列**，不是重启系统。

#### 3.4.2 敲什么

在 NSH 里（串口）：

```
hw_test lcdreinit
```

单独运行（不跑那套 5 步自检）。耗时约 0.5 秒，期间画面会闪一下、触摸停约 0.5 秒，
都是正常的。

#### 3.4.3 期望看到什么：`[lcdreinit]` 逐步日志

> ⚠ **驱动里原有的 `lcdinfo` / `lcdwarn` / `lcderr` 在本固件里全被编掉了**
> （`CONFIG_DEBUG_LCD` 没开，`nuttx/include/debug.h:517-532` 把它们展开成 `_none`），
> 所以那几行看不到。2026-09-16 起 `sf32lb_lcd_panel_reinit()` 的每一步都改成走
> **`syslog()`**，前缀统一 `[lcdreinit]`，串口日志里直接 grep 它。

```
[lcdreinit] ===== 开始 (面板 co5300 bpp 16 power 100 亮度 80%) =====
[lcdreinit] STEP1 拿到面板锁（等了 3 ms），之后刷屏会被挡住
[lcdreinit] STEP2 LCDC: State=1 Lock=0 ErrorCode=0x00000000 起帧=18231 完帧=18231
[lcdreinit] STEP2 寄存器: LCD_CONF=0x00001402 SPI_IF_CONF=0x00000280 LCD_IF_CONF=0x00800000
[lcdreinit] STEP2 寄存器: TE_CONF=0x00000000 STATUS=0x00000000 LCD_SINGLE=0x00000000
[lcdreinit] STEP2 显示时钟: HCLK=240MHz CLK_DIV=10 ⇒ 面板时钟≈24MHz
[lcdreinit] PASS STEP3 读回面板 ID=0x00331100 (期望 0x00331100) ⇒ QSPI 链路和面板都活着
[lcdreinit] PASS STEP4 Lock 干净 (HAL_UNLOCKED)
[lcdreinit] PASS STEP4 State=READY
[lcdreinit] PASS STEP5 BSP_LCD_PowerUp 完成（PA10 供电、PA37 VADD_EN=1、QSPI 脚 pinmux）
[lcdreinit] PASS STEP6 RESET 脚: 1→10ms→0→30ms→1→120ms（开机只拉低 10ms）
[lcdreinit] STEP7 面板 Init 完成（含 HAL_LCDC_Init: RCC_MOD_LCDC1 复位→接口/分频/格式/TE）
[lcdreinit] PASS STEP7 SPI_IF_CONF.CLK_DIV=10 ⇒ 面板时钟≈24MHz
[lcdreinit] PASS STEP8 irq_attach + up_enable_irq(LCDC1_IRQn=79)
[lcdreinit] PASS STEP9 层复位/背景色清零 + SetColorMode(RGB565)
[lcdreinit] PASS STEP10 亮度重下发 80%
[lcdreinit] PASS STEP10 DisplayOn 已下发 (0x29)，power=100
[lcdreinit] PASS STEP11 重发配置后读回面板 ID=0x00331100
[lcdreinit] STEP12 结论: 动手前/后 ID 都读得回 ⇒ …
[lcdreinit] ===== 结束: 总耗时 402 ms，其中持锁 399 ms（这段时间刷屏被挡） =====
[Bridge] 面板已重初始化，已投一次全屏重绘
      robot_ui_bridge_panel_reinit() -> 0，耗时 420 ms
      [PASS] 面板重新初始化  (初始化序列已下发（亮不亮要看屏幕）)
      看屏幕：
        救回来了 -> 1~2 秒内整屏闪一下然后恢复画面，触摸也恢复响应；
                   这时不用再敲别的，界面自己会继续刷。
        没救回来 -> 屏幕仍然全黑 —— 接着看下面那张判读表。
```

#### 3.4.3.1 怎么从日志判断"卡在哪一层"（**这次的核心用途**）

最有判别力的是 **STEP3 / STEP11 两次读面板 ID**（`0x04` 寄存器，本板期望
`0x331100`）：ID 读得回来 = QSPI 链路 + LCDC 出位 + 面板**同时**活着。

| 日志形状 | 结论 | 下一步查哪 |
|---------|------|-----------|
| STEP3 PASS + STEP11 PASS，屏幕仍黑 | 面板通路本来就是通的，**黑屏不在这一层** | 背光 PWM / 亮度（`backlight_set`、`0x51 WBRIGHT`）、LCDC 层格式、面板显示开关（`0x29`/`0x28`）、`SF32LB_LCD_CO5300_VSYNC` 那条死配置 |
| STEP3 WARN + STEP11 PASS | 原本是 **QSPI / 面板那侧**的问题，这次重初始化把它救回来了 | 继续查"什么操作把面板/QSPI 弄坏了"（快速连点菜单时那条路径） |
| STEP3 与 STEP11 都 WARN（ID=0 或 `0xffffffff`） | 面板通路**根本没起来**，不是"丢配置" | 看同一份日志里的 `State` / `Lock` / `CLK_DIV`：`State!=READY` ⇒ 见下面那条；`CLK_DIV=0` ⇒ 时钟没配上；两个都正常 ⇒ 面板供电（PA37 VADD_EN）/ RESET 脚（PA00）/ 排线 |
| `STEP3 注意 State=… Lock=…` 紧跟一条 WARN | 读失败**未必**是 QSPI 坏：`HAL_LCDC_ReadDatas()` 开头就判 `State != READY` 返回 `HAL_BUSY` | 看 STEP4 有没有把它强置回 READY、STEP11 是否就好了 |
| `STEP2 起帧` 远大于 `完帧` | LCDC 有传输开始却从不完成 | LCDC1 中断（STEP8 会重接）/ `draw_sem` 超时（`lcd xfer wait timeout`） |
| `STEP2 LCDC: State=2`（BUSY） | State 卡在 BUSY，之后**每一次寄存器写都静默失败** | STEP4 会强置 READY；要查是谁把 `XferCpltCallback` 吃掉/中断没来 |

另外 STEP12 会把**持锁时长**直接打出来（`总耗时 X ms，其中持锁 Y ms`）——
那 Y 就是"这一下挡了刷屏多久"，和 `hw_test lcdreinit` 里那个 `耗时 420 ms` 对得上。

**这两行是面板 QSPI 时钟唯一的窗口**：`STEP2 显示时钟` 是动手前的实时读值、
`STEP7 SPI_IF_CONF.CLK_DIV` 是 `HAL_LCDC_Init()` 重写之后回读的值
（`hlcdc->Instance` 是寄存器指针，读的是硬件、不是快照）。默认应当两行都是
`CLK_DIV=10 ⇒ 24MHz`；读到 `CLK_DIV=5 ⇒ 48MHz` 说明降频那档没编进去（见 3.4.6）；
`CLK_DIV=0` 才是"时钟没配上"。

#### 3.4.4 它到底做了什么（以及为什么）

| STEP | 在哪 | 说明 |
|------|------|------|
| 1 | 驱动内 | 拿 `panel_lock`（**带 1s 超时**）；超时只打 WARN 后继续，不会把调用方永久挡住 |
| 2 | 驱动内 | 状态快照：`State` / `Lock` / `ErrorCode` / 起帧-完帧计数 / 四个关键寄存器 / `HCLK` 与 `CLK_DIV` |
| 3 | `co5300.c:330` `LCD_ReadID()` | **动手前读一次面板 ID** —— 全流程最有判别力的一条 |
| 4 | 驱动内 | 坏状态归零：`Lock→UNLOCKED`（防 `__HAL_LCDC_LOCK` 里的 `HAL_LCDC_ASSERT(0)` 死等）、`State→READY`、`ErrorCode→NONE` |
| 5 | `board/contest_board/src/bsp_lcd_tp.c:29` | `BSP_LCD_PowerUp()`：VADD_EN(PA37) 拉高 + LCD 那几根脚重新 pinmux（幂等） |
| 6 | 驱动内 + PA00 | 自己拉 RESET 脚：`1→10ms→0→30ms→1→120ms`。开机只拉低 10ms（`co5300.c:214`），这里给 3 倍余量（复位不彻底是"丢配置"最常见的形状） |
| 7 | `co5300.c:200` `LCD_Drv_Init()` | **内部第一步就是 `HAL_LCDC_Init()`**：`LCDC_HW_Init()` 里 `HAL_RCC_EnableModule + HAL_RCC_ResetModule(RCC_MOD_LCDC1)` = LCDC 整模块硬复位，然后选接口 / 写 `SPI_IF_CONF.CLK_DIV` / 输出格式 / TE / 释放 RSTB。之后才是 RESET 脚 + 整串面板寄存器（0xFE 页切换、密码锁、0xC4 SPI 模式、0x3A 像素格式、0x2A/0x2B 窗口、0x11 出睡眠、**0x29 DisplayOn**） |
| 8 | 驱动内 | 补开机那一步中断接线：`irq_attach` + `up_enable_irq(LCDC1_IRQn)`（幂等）。中断没使能时 `SendLayerData2Reg_IT` 永远等不到完成、`State` 会卡在 BUSY |
| 9 | `sf32lb_lcd.c` `sf32lb_lcd_lcdc_setup()` | LCDC 侧背景色 / 层复位 / 层像素格式（**开机路径和重初始化调的是同一个函数**），再按当前 bpp 走一次 `SetColorMode` |
| 10 | `SetBrightness()` + 驱动内 | 面板复位后 `0x51 WBRIGHT` 会回到上电默认值，按驱动里记着的百分比重下发；再重发 DisplayOn 并把 `priv->power` 对齐 |
| 11 | `co5300.c:330` | 收工后再读一次 ID（和 STEP3 配对 = "这次救没救回来"） |
| 12 | 驱动内 | 结论 + 耗时 / 持锁时长 |
| ⑥(应用侧) | `app/robot_ui/robot_ui_bridge.c` | 请 LVGL 全屏重绘：面板被复位过、GRAM 里是随机内容，**光修面板它不会自己重画**。`lv_obj_invalidate(lv_screen_active())` 必须投到 LVGL 线程（`ui_async_call`），在 NSH 线程里直接碰控件会撞 `Invalidate area is not allowed during rendering` 断言 |

调用链就一条：

```
hw_test lcdreinit
  -> robot_ui_bridge_panel_reinit()          (app/robot_ui/robot_ui_bridge.c)
       -> sf32lb_lcd_panel_reinit()          (vendor/sifli/.../drivers/lcd/sf32lb_lcd.c，STEP1~12)
       -> ui_async_call(全屏 invalidate)      (⑥，跑在 LVGL 线程)
```

**为什么没有单独调 `HAL_LCDC_Reset()`**：读实现（`bf0_hal_lcdc.c:2373`）可知它就是
`LCDC_HW_Init()` + `State = READY`，而 `HAL_LCDC_Init()` = 同样的 `LCDC_HW_Init()` +
置 `Layer[].disable` —— **`p_ops->Init()` 里已经调过 `HAL_LCDC_Init()` 了**，再插一次
只是多一次全模块复位。可重入性也不是靠猜：开机路径已经把 `p_ops->Init()` 调了三次
（见 3.4.5）。所以改成"回读 `CLK_DIV` / `LCD_RSTB` 确认它确实做了"。

**显示时钟 / PMU 显示电源域**：LCDC1 的模块时钟来自 HCLK（本板 240MHz，`bsp_init.c:155-159`
用 DLL1 提供），分频写在 `SPI_IF_CONF.CLK_DIV`，`SetFreq()` 从 `HAL_RCC_GetHCLKFreq()`
现算 —— **没有独立的显示 PLL，也没有 PMU 里的显示电源域**要重新使能。面板的 VADD_EN
是板级 GPIO PA37，由 `BSP_LCD_PowerUp()` 拉（STEP5）。LCDC 那个"QSPI"就是面板总线
本身，**不是 flash 的 SFC 控制器**，别去动 SFC。

那条时钟的**唯一来源**是面板驱动里的请求频率
（`co5300.c` 的 `lcdc_int_cfg_qadspi.freq` → `hlcdc->Init.freq` → `SetFreq()`），
2026-09-16 起默认从 50MHz（实测 48MHz）降到 24MHz，见 3.4.6。

**线程安全**：LVGL 线程一直在刷（20~30 帧/s），而 `lcdreinit` 是从 NSH 任务里调的。
驱动里加了一把 `panel_lock`，`putrun` / `putarea` 推像素、`setpower` 和重初始化拿的是
同一把，所以"重发配置"和"推一帧像素"不会同时在 QSPI 上跑。重初始化这 ~0.4 秒里刷新
会卡住 —— 面板本来就在复位，卡住是应该的（STEP12 会打出实测值）。

`hw_test lcdreinit` 的另一半用途是**验根因**：如果敲了它屏幕就回来，就证明原来那次
黑屏是"面板 / QSPI 那侧坏了"，而不是 CPU / LCDC / 应用挂了；如果敲十次八次都能救回来，
那下一步该查的是"什么操作把面板弄坏了"（大概率是快速连点时某条 SPI 时序出了问题），
而不是继续在 LVGL / 应用这一侧找。**要是 STEP3/STEP11 两次 ID 都读得回来、屏幕还是黑**，
那就别再怀疑面板通路了，去查背光 / 亮度 / 层格式那一侧。

#### 3.4.5 回退开关 / 已知边界

- **一键回退**：`vendor/sifli/boards/sf32lb52/drivers/lcd/sf32lb_lcd.c` 顶部
  `#define SF32LB_LCD_PANEL_REINIT 1` 改成 `0` —— `sf32lb_lcd_panel_reinit()`
  直接回 `-ENOSYS`，`panel_lock` 的三个宏全变成空操作，**刷新路径和开机路径一行不多**
  （那两个小工具函数也一起被 `#if` 掉，不会有 unused 告警），等于完全没加过这个功能
  （`hw_test lcdreinit` 还在，只是会报 `-38` / `本固件把 SF32LB_LCD_PANEL_REINIT 编成了 0`）。
- **只治"面板 / QSPI 那侧坏了"这一种黑屏**。LVGL 没起来（连 `/dev/lcd0` 都开不了）、
  LCDC 没出帧（`[ui]` 仪表根本没有 flush 日志）、或者背光关着，这个命令都不解决 ——
  它不会替你判断，只会如实报每一步的结果（所以才有了 3.4.3.1 那张判读表）。
- **重初始化**期间**不要**并发敲 `hw_test lcdcolor` / `hw_test lcd`：那几条也会
  走 `/dev/lcd0`。真要连着敲，等上一条打印完。
- 驱动里会把 HAL 句柄的 `Lock` 位清成 `HAL_UNLOCKED`、把 `State` 强置回
  `HAL_LCDC_STATE_READY` 再重走初始化 —— 防的是"上一次传输把 HAL 锁留在 `LOCKED`、
  之后每次写寄存器都在 `HAL_LCDC_ASSERT(0)` 上死等"，以及"`State` 卡在 BUSY 导致
  之后每次寄存器读写都直接返回 `HAL_BUSY`"。这两条路径**不是靠猜**：STEP2 会把原值
  打出来，看到 `Lock 干净` / `State=READY` 就说明这次黑屏跟它们无关。
- **开机那三次面板复位**（`find_right_driver` → `lcd_init_thread_entry` →
  `lcd_hw_setup_thread_entry`）里，第三次是**在 `/dev/lcd0` 和 `/dev/fb0` 都存在之后**
  才做的。`putrun`/`putarea` 有 `s_lcd_hw_ready` 挡着，但 `setpower`
  （`LCDDEVIO_SETPOWER`）过去不挡 —— 上层在这期间调一次 SETPOWER，就会往"RESET 脚
  正被拉低、寄存器序列写到一半"的面板上插一个 `0x29`/`0x28`，面板停在半配置状态，
  **之后一直黑到真断电**。这正好对上"重插有概率黑屏起不来"：
  `board_late_initialize()` 里那句 2 秒 `usleep` 本来是防它的，但 `lcd_init` 的优先级
  是 95、`lcd_hw` 是 100，而 **robot_ui 是 110 —— 本树数值越大越优先**
  （`nuttx/sched/sched/sched.h:417` 按 `sched_priority` 降序排 ready-to-run 链表），
  所以 robot_ui 一就绪就会抢正在忙等的 `lcd_hw`，SETPOWER 就插进复位序列里了。
  现已让第三次复位和 `setpower` 拿同一把 `panel_lock`（`setpower` 用带 1s 超时的等，
  超时只打 WARN 后照原样下发），`s_lcd_hw_ready = true` 也挪进锁里。
  关掉开关即完全回退。

#### 3.4.6 面板 QSPI 时钟：48MHz → 24MHz（2026-09-16，降频救间歇黑屏）

**治的假设**：这块屏的现象是**间歇性**的 —— 偶尔闪出正常界面、多数时候"只能看到字、
很浅、整体是黑"、有时全黑。而黑屏时应用侧一切正常（LVGL 20~30 帧/s、每次 flush
0~10ms），`hw_test lcd 80` 下发亮度成功，面板 ID 读得回 `0x00331100`，LCDC
`起帧=完帧` —— 也就是**面板会显示、像素能写进去，只是大部分时候不对**。
形状是"大块背景丢、只剩细笔画"，最像**写像素时丢数据**，而不是面板丢了配置
（所以 3.4 那套重初始化救不回来）。

**为什么怀疑时钟**：厂商提交 `0a3cd0a` 自己的描述里写着 *"some panels do not respond
reliably to ID queries **on USB-only power where read timing is marginal**"* ——
而本板正是 USB 直供（不带电池）。那个提交的两处修复（`sf32lb_lcd.c:1053` 的显式
`Init()`、`co5300.c` 的 SWRESET+ReadID 降级）**都已经在树里**，所以问题不是"它没修"，
而是**边缘时序这件事本身还在**：48MHz 下这条 QSPI 在 USB 供电时贴着时序余量的边。

**时钟是怎么定的（唯一一条路径）**：

```
co5300.c  lcdc_int_cfg_qadspi.freq
  -> LCD_Init() memcpy 进 hlcdc->Init
  -> LCD_Drv_Init()  HAL_LCDC_Init(hlcdc)
  -> LCDC_HW_Init()  SetFreq(lcdc, init->freq)     (bf0_hal_lcdc.c:2284)
       clk_div = ceil(HCLK / freq)   (HAL 里那个除法向上取整，硬件最小分频 2)
       SPI_IF_CONF.CLK_DIV = clk_div
```

本板 HCLK = 240MHz（`board/contest_board/src/bsp_init.c:158` 的
`HAL_RCC_HCPU_EnableDLL1(240000000)`），于是：

| `.freq` | `CLK_DIV` | 实际面板时钟 |
|---------|-----------|-------------|
| `50000000`（改前，也是 `SF32LB_LCD_QSPI_CLK_SLOW 0`） | 5 | **48MHz** |
| `24000000`（现在默认） | 10 | **24MHz** |

所以"降一半"就是 `CLK_DIV` 5 → 10。**分频只在这一个地方写**：驱动侧没有任何代码
写 `SPI_IF_CONF`，`sf32lb_lcd.c` 不碰时钟，读寄存器时的降速/恢复
（`co5300.c` 的 `LCD_ReadMode()`，读 2MHz、恢复用 `lcdc_int_cfg.freq`）用的也是同一个值，
改一处就全对。

**一键回退**：`co5300.c` 顶部 `#define SF32LB_LCD_QSPI_CLK_SLOW 0`
→ 预处理器走 `#else` 分支，字面值 `50000000`，逐字回到 48MHz。

**上板怎么确认生效**：敲 `hw_test lcdreinit`，看这两行（见 3.4.3）——

```
[lcdreinit] STEP2 显示时钟: HCLK=240MHz CLK_DIV=10 ⇒ 面板时钟≈24MHz
[lcdreinit] PASS STEP7 SPI_IF_CONF.CLK_DIV=10 ⇒ 面板时钟≈24MHz
```

两行都是 10/24MHz = 降频已生效（STEP2 那行打印的 `SPI_IF_CONF` 也会从
`0x00000140` 变成 `0x00000280`，就是 `10 << 6`）；还是 `CLK_DIV=5` 就是没编进去。

**如果这真是边缘时序问题，屏幕应该变成什么样**：

- **正例**：开机后长时间不再出现"只剩字/很浅/全黑"，反复点菜单、亮暗切换、
  长时间运行都能稳定显示完整画面（背景大块填充不再丢）。48MHz 下闪一下就好的
  那些工况，24MHz 下应该**不闪**。
- **反例（说明时钟不是根因）**：黑屏/丢像素的出现频率与画质**没有变化**
  —— 那就把 `SF32LB_LCD_QSPI_CLK_SLOW` 置回 0（或继续降一档验证），
  回头去查背光 / 偏置（PA37 VADD_EN）/ 排线接触 / 面板本身，
  以及"闪出正常界面"那一刻和黑屏时**到底哪里不一样**。

**代价（为什么值得先试）**：推送带宽减半 —— 4 数据线 24MHz ≈ 12MB/s，
全屏 390×450×2B ≈ 29ms/帧（48MHz 时 ≈ 15ms）。**面板自刷新不受影响**
（CO5300 带 GRAM，自刷新由它内部振荡器驱动），只有"SoC 往面板推画面"变慢，
所以 LVGL 的帧率上界会从 ~60 帧/s 降到 ~34 帧/s。本机UI 实测在 20~30 帧/s
（且多数是局部刷新，不是整屏），24MHz 仍有余量；**若上板后觉得动画明显变慢**，
可把 `SF32LB_LCD_QSPI_CLK_HZ` 调成 30000000（`CLK_DIV=8` ⇒ 30MHz）或
40000000（`CLK_DIV=6` ⇒ 40MHz）再试 —— 值随你改，`CLK_DIV` 会自动跟着变，
但要记住 `CLK_DIV = ceil(240MHz / freq)`，**不是任意频率都取得准**。

## 4. 触摸：`/dev/input0`

### 4.1 推荐路线：LVGL indev

`CONFIG_LV_USE_NUTTX_TOUCHSCREEN=y` + `lv_nuttx_init()` 会按 `dsc.input_path`
（默认 `/dev/input0`）打开触摸并每 10ms 读一次（见 3.1）。**应用完全不碰 `/dev/input0`**，
只写"按钮点了就干什么"的回调。

### 4.2 直接读：`open` + 非阻塞 `read` 得 `struct touch_sample_s`

```c
#include <nuttx/input/touchscreen.h>
#include <fcntl.h>
#include <poll.h>

#define MAX_POINTS 16
uint8_t buf[sizeof(struct touch_sample_s) +
            MAX_POINTS * sizeof(struct touch_point_s)];

int fd = open("/dev/input0", O_RDONLY | O_NONBLOCK);
if (fd < 0) { /* FAIL，别阻塞等 */ }

for (;;)
  {
    struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };

    if (poll(&pfd, 1, 200) <= 0)      /* 超时/无事件：接着等，别死等 read */
      {
        continue;
      }

    memset(buf, 0, sizeof(buf));      /* 必须清零：npoints 为 0 时会用到旧值 */
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < (ssize_t)sizeof(struct touch_sample_s))
      {
        continue;                     /* EAGAIN：这次没数据 */
      }

    struct touch_sample_s *s = (struct touch_sample_s *)buf;
    for (int i = 0; i < s->npoints; i++)
      {
        printf("id=%d flags=0x%02x x=%d y=%d h=%d\n",
               s->point[i].id, s->point[i].flags,
               s->point[i].x, s->point[i].y, s->point[i].h);
      }
  }

close(fd);
```

`flags` 是位域：`TOUCH_DOWN` / `TOUCH_MOVE` / `TOUCH_UP` 表示动作，
`TOUCH_ID_VALID` / `TOUCH_POS_VALID` 表示 id/x/y 有效。

## 5. 按键：板级 `sf32lb52_boardbtn`（**不要用 `/dev/buttons`**）

### 5.1 先说结论：`/dev/buttons` 这条路在真机上会卡死整机

本板有一个 NuttX 标准按键驱动（`src/sf32lb52_buttons.c`，注册成
`/dev/buttons`，上层用 `poll()` + `read()` 读 `btn_buttonset_t`）。
**节点是好的、注册也是好的，但用它的 `poll()`/`read()` 在真机上会让整机静默卡死**：

- 现象：敲下 `hw_test button` 之后串口**一行输出都没有**，USB 设备同时从主机侧
  消失（RNDIS 网卡没了），整块板子不动了，**只能重新烧录**才能恢复；
- 不是断言、不是崩溃、也不是看门狗复位，就是彻底没反应；
- 嫌疑落在 `/dev/buttons` 的 poll/read 路径上 —— `hw_test` 里其它路径都验过，
  只有碰它的那一步把整机搞死。

所以板级**另给了一条 GPIO 路线**：`src/sf32lb52_boardbtn.h` / `.c` ——
按键脚当普通 GPIO 轮询 + 软件消抖，事件走回调，完全不碰那个字符设备。
参考实现是 RT-Thread 的小智固件（`/home/youdian/xiaozhi/xiaozhi-sf32/`）：
它同样不读 `/dev/buttons`，而是 PA11/PA34 当 GPIO、自己发事件。

> 那个驱动的注册**没有动**（`src/sifli_ap.c:420` 照旧调
> `sf32lb52_button_initialize("/dev/buttons")`），别的模块还能用；
> 只是**别再在新代码里 poll/read 它**。要真修得从驱动查起。

### 5.2 推荐用法：注册回调

```c
#include "sf32lb52_boardbtn.h"   /* 头文件在 board/src，app 的 CMakeLists 里加 INCLUDE_DIRECTORIES */

static volatile int g_key_pressed;   /* 回调里只置标志，由自己的循环去处理/刷界面 */

static void my_btn_cb(enum board_btn_e btn, enum board_btn_event_e ev,
                      uint32_t held_ms, FAR void *arg)
{
  /* ⚠️ 回调跑在 "boardbtn" 轮询任务上下文（优先级 115）：
     不要 sleep / 等信号量 / 开文件 / 打长日志 —— 会拖住另一个键的事件上报；
     更不要在这里调 lv_*（LVGL 只能在创建界面的那个线程里用）。
     只更新标志 / 投消息，剩下的交给自己的任务。 */
  (void)arg;
  (void)held_ms;

  if (btn == BOARD_BTN_KEY && ev == BOARD_BTN_PRESS)
    {
      g_key_pressed = 1;
    }
}

int my_init(void)
{
  /* 内部会懒初始化（配脚 + 起轮询任务），不需要先调 board_btn_init() */
  return board_btn_set_callback(my_btn_cb, NULL);   /* NULL = 注销 */
}
```

事件类型（`enum board_btn_event_e`）：

| 事件 | 含义 | `held_ms` |
|------|------|-----------|
| `BOARD_BTN_PRESS` | 按下（已消抖） | 0 |
| `BOARD_BTN_LONGPRESS` | 按住超过 **1000ms**，只报一次 | 已经按住的毫秒数 |
| `BOARD_BTN_RELEASE` | 松开 | 本次从按下到松开的毫秒数 |

不想用回调、只想偶发查一下状态：

```c
int pressed = board_btn_is_pressed(BOARD_BTN_KEY);  /* 1=按下 0=松开 <0=错误 */
```

**这个函数不需要先 init**：PA11 在板级 pinmux 里已经配成 GPIO 输入，直接读；
PA34 在 `board_btn_init()` 之前读会返回 `-ENODEV`（那时它还没被切成 GPIO）。

### 5.3 引脚 / 电平 / 消抖 / 任务

| 枚举 | 引脚 | wiki 表 1 的名字 | 有效电平 |
|------|------|------------------|----------|
| `BOARD_BTN_KEY` | PA11 | KEY / 功能按键 | 高（按下 = 高） |
| `BOARD_BTN_HOME` | PA34 | HOME / 长按复位 | 高（同上） |

- 轮询周期 **10ms**，**连续 3 次采样一致**才认电平变化（≈30ms 消抖）；
- 轮询任务 `"boardbtn"`：优先级 **115**（低于 robot_ui 的 110，高于系统
  工作队列），栈 3072，周期 `usleep(10ms)`；
- 板级 pinmux 给这两个脚都是**下拉**（`src/bsp_pinmux.c:173`），
  所以两个键空闲时都是低电平、按下才是高。
- 编号在 `enum board_btn_e` 里 (`KEY = 0` / `HOME = 1`)，
  `board_btn_name()` / `board_btn_event_name()` 给的是打印用的字符串。

### 5.4 PA34 特殊在哪（改按键相关代码前必看）

`src/bsp_pinmux.c:171` 那一行是**故意注释掉**的，注释原文大意是：
保持默认下拉不变 —— 关掉内部下拉，UART 下载驱动在没有外部下拉的板子上就不工作了。
也就是说：

- PA34 **没有外部下拉**，靠的是内部下拉；
- 它同时还是 **PMU 电源键 / 长按复位脚**（长按会走 PMU 的硬件复位逻辑），
  并且能当 AON 唤醒脚。

本模块的处理：只把它切成 `GPIO_A34` 输入并**保持下拉**（`PIN_PULLDOWN`
就是 pad 的复位默认值），**不申请中断、不独占、不写电平**，也不碰 PMU /
唤醒的任何寄存器。如果现场发现 PA34 读不准或者有别的副作用，把
`sf32lb52_boardbtn.h` 里的 `BOARD_BTN_ENABLE_HOME` 改成 0 重编，就退化成
"只支持 PA11"，其余代码不用动。

### 5.5 与 `/dev/buttons` 驱动的关系（两边读同一个 pad）

方向/功能都不冲突，可以并存：

- `sf32lb52_buttons.c` 用 `sifli_gpio_config()` 把 PA11 配成输入
  （那个函数对输入脚写死 `NOPULL`），再用 `sifli_gpio_set_event()` 挂中断；
- 本模块只调 `sifli_gpio_read()` 读电平，另外在 init 里用 `HAL_PIN_Set()`
  把 PA11 的下拉**重申一次**（重申的正是 `bsp_pinmux.c:173` 板级 pinmux
  本来就写的下拉）。之所以要重申：上面那个 `sifli_gpio_config()` 会把输入脚
  设成 `NOPULL`，而这两个键是**高有效**，悬空就会乱报"按下"。
  这一步不改方向、不改功能，对 `/dev/buttons` 没有影响。

### 5.6 自检：`hw_test button`

`hw_test button [秒]`（默认 15 秒）现在走的就是这条 GPIO 路线：按到 PA11/PA34
会打印 `哪个键 + 事件类型 + 按住时长` 并报 `[PASS] 按键`；
**超时没按到仍然是 `[FAIL] 按键（超时，没检测到按下）`**。
默认 `hw_test` 的第 4 步（`step_buttons`）也换成了这条路线，
那一步是"非交互"的（没人按也算 PASS，只证明读得动），
要验证按键本身请用 `hw_test button`。

## 6. GPIO：`/dev/gpio0` / `/dev/gpio1` / `/dev/gpio2`

先分清三个节点的引脚（注册顺序来自 `sifli_gpio.c:365`）：

| 节点 | 方向 | 引脚 | 定义位置 |
|------|------|------|---------|
| `/dev/gpio0` | 输入 | PA34 | `sf32lb52_devkit_lcd.h:33` `GPIO_IN1` |
| `/dev/gpio1` | 输出 | PA26 | `sf32lb52_devkit_lcd.h:34` `GPIO_OUT1` |
| `/dev/gpio2` | 中断输入 | PA34 | `sf32lb52_devkit_lcd.h:36` `GPIO_INT1` |

读引脚状态（`read()` 读 1 字节，0/1）：

```c
#include <nuttx/ioexpander/gpio.h>
#include <fcntl.h>

bool value = false;
int fd = open("/dev/gpio0", O_RDONLY);
if (fd < 0) { /* FAIL */ }

if (read(fd, &value, 1) == 1)
  {
    printf("PA34 = %d\n", (int)value);
  }

/* 引脚类型：GPIOIOC_GETPINTYPE 的参数是 enum gpio_pintype_e * */
enum gpio_pintype_e pintype;
if (ioctl(fd, GPIOIOC_GETPINTYPE, (unsigned long)&pintype) == OK)
  {
    printf("pintype=%d\n", (int)pintype);
  }

close(fd);
```

写引脚（**先确认这个脚是板级分配给你的输出脚**，本板就是 `/dev/gpio1` = PA26）：

```c
bool value = true;
int fd = open("/dev/gpio1", O_RDWR);      /* 写需要 O_WRONLY / O_RDWR */
if (fd < 0) { /* FAIL */ }

ioctl(fd, GPIOIOC_SETPINTYPE, (unsigned long)GPIO_OUTPUT_PIN);  /* 需要时改方向 */

write(fd, &value, 1);                     /* 电平置 1 */
read(fd, &value, 1);                      /* 回读确认 */

value = false;
write(fd, &value, 1);                     /* 用完拉回 0，别把电平留在 1 */
close(fd);
```

> 引脚类型相关的 ioctl 宏名随 NuttX 版本略有差别（新版本是
> `GPIOIOC_GETPINTYPE` / `GPIOIOC_SETPINTYPE`，老版本是 `GPIOIOC_CONFIG`）。
> 电平读写走文件 `read()` / `write()`，这部分是稳定的。
> `hw_test` 里用 `#if defined(...)` 做了兼容，不会因为宏名不同编不过。

## 7. 定时器 / PWM / I2C / ADC

### 7.1 定时器 `/dev/timer0`

```c
#include <nuttx/timers/timer.h>

struct timer_status_s status;
int fd = open("/dev/timer0", O_RDONLY);

ioctl(fd, TIMERIOC_SETTIMEOUT, (unsigned long)&timeout_us);  /* 微秒 */
ioctl(fd, TIMERIOC_START, 0);
ioctl(fd, TIMERIOC_GETSTATUS, (unsigned long)&status);
ioctl(fd, TIMERIOC_STOP, 0);
close(fd);
```

### 7.2 PWM `/dev/pwm0`

```c
#include <nuttx/timers/pwm.h>

struct pwm_info_s info;
info.frequency = 1000;        /* Hz */
info.duty      = 0x8000;      /* Q16.16 占空比，0x8000 = 50% */

int fd = open("/dev/pwm0", O_RDONLY);
ioctl(fd, PWMIOC_SETCHARACTERISTICS, (unsigned long)&info);
ioctl(fd, PWMIOC_START, 0);
ioctl(fd, PWMIOC_STOP, 0);    /* 用完停掉 */
close(fd);
```

本板 PWM0 默认挂在背光脚 PA01（`GPTIM1_CH4`，见 `bsp_pinmux.c:227`），
**调它等于调背光亮度**，别在演示中途改。

### 7.3 I2C：读一个寄存器（`/dev/i2c0`）

```c
#include <nuttx/i2c/i2c_master.h>
#include <sys/ioctl.h>
#include <fcntl.h>

uint8_t reg = 0xa8;      /* 要读的寄存器地址 */
uint8_t val = 0;

struct i2c_msg_s msg[2];
msg[0].frequency = 400000;
msg[0].addr      = 0x38;        /* 从设备 7bit 地址 */
msg[0].flags     = 0;           /* 写 */
msg[0].buffer    = &reg;
msg[0].length    = 1;

msg[1].frequency = 400000;
msg[1].addr      = 0x38;
msg[1].flags     = I2C_M_READ;  /* 读 */
msg[1].buffer    = &val;
msg[1].length    = 1;

struct i2c_transfer_s xfer = { msg, 2 };

int fd = open("/dev/i2c0", O_RDWR);
if (fd < 0) { /* FAIL */ }

if (ioctl(fd, I2C_TRANSFER, (unsigned long)&xfer) == OK)
  {
    printf("reg 0x%02x = 0x%02x\n", reg, val);
  }

close(fd);
```

- `/dev/i2c0` 就是触摸屏那条总线（FT6146，400kHz）；**别在上面乱发地址**，
  容易把触摸驱动正在进行的传输干扰掉。
- 想快速手测，本板还编了 NuttX 的 `i2c` 命令（`CONFIG_SYSTEM_I2CTOOL=y`）。

### 7.4 ADC `/dev/adc0`

```c
#include <nuttx/analog/adc.h>

struct adc_msg_s msg;
int fd = open("/dev/adc0", O_RDONLY);

ioctl(fd, ANIOC_TRIGGER, 0);              /* 触发一次采样（宏名以
                                             <nuttx/analog/ioctl.h> 为准） */
read(fd, &msg, sizeof(msg));              /* msg.am_channel / msg.am_data */
close(fd);
```

## 8. 引脚表（从板级源码核实，**不是**从 `app/hello_app/README.md` 抄的）

| 引脚 | 功能 | 来源（file:line） |
|------|------|------------------|
| PA00 | LCD 复位 `LCD_RESETB`（GPIO_A0） | `src/bsp_pinmux.c:156`、`src/bsp_lcd_tp.c:5` |
| PA01 | LCD 背光 PWM（`GPTIM1_CH4`） | `src/bsp_pinmux.c:227` |
| PA02 | LCDC1 SPI TE | `src/bsp_pinmux.c:230` |
| PA03 | LCDC1 SPI CS | `src/bsp_pinmux.c:231` |
| PA04 | LCDC1 SPI CLK | `src/bsp_pinmux.c:232` |
| PA05 | LCDC1 SPI DIO0 | `src/bsp_pinmux.c:233` |
| PA06 | LCDC1 SPI DIO1 | `src/bsp_pinmux.c:234` |
| PA07 | LCDC1 SPI DIO2 | `src/bsp_pinmux.c:235` |
| PA08 | LCDC1 SPI DIO3 | `src/bsp_pinmux.c:236` |
| PA37 | LCD VADD 使能（CO5300 屏的 `LCD_VADD_EN`，GPIO_A37） | `src/bsp_pinmux.c:243`、`src/bsp_lcd_tp.c:9,24,35` |
| PA10 | 音频功放使能 `AUDIO_PA_CTRL` / `AU_PA_EN`，高有效 | `src/bsp_pinmux.c:157`、`src/sf32lb52_audio.c:69-70,217-221` |
| PA09 | 触摸复位 `CTP_RESET`（GPIO_A9） | `src/bsp_pinmux.c:217`、`src/bsp_lcd_tp.c:6` |
| PA31 | 触摸中断 `CTP_INT` | `src/bsp_pinmux.c:218`、`defconfig:72`（`CONFIG_TOUCH_IRQ_PIN=31`） |
| PA30 | 触摸 I2C1 SCL | `src/bsp_pinmux.c:219`、`src/sifli_ap.c:485-486` |
| PA33 | 触摸 I2C1 SDA | `src/bsp_pinmux.c:220` |
| PA11 | Key2 用户按键（KEY）；板级按键模块的 `BOARD_BTN_KEY` | `src/bsp_pinmux.c:173`、`src/sf32lb52_buttons.c:43`、`src/sf32lb52_boardbtn.c`、`include/board.h:30-36` |
| PA34 | Key1 电源键脚（HOME，长按走 PMU 复位）；同时是 `GPIO_IN1` / `GPIO_INT1`（`/dev/gpio0`、`/dev/gpio2`）和板级按键模块的 `BOARD_BTN_HOME` | `src/sf32lb52_devkit_lcd.h:33,36`、`src/bsp_pinmux.c:167-171`（注释标注 Key1）、`src/sf32lb52_boardbtn.c` |
| PA26 | `GPIO_OUT1`：板级唯一的 GPIO 输出脚（`/dev/gpio1`），未分配给任何外设 | `src/sf32lb52_devkit_lcd.h:34`、`src/bsp_pinmux.c:206`、`src/sifli_gpio.c:122-125,387-403` |
| PA32 | RGB LED（`GPTIM2_CH1` / GPIO_A32） | `src/bsp_pinmux.c:202,207` |
| PA18 / PA19 | UART1 调试串口 RX / TX | `src/bsp_pinmux.c:160-161` |
| PA20 / PA27 | UART2 log RX / TX | `src/bsp_pinmux.c:164-165` |
| PA35 / PA36 | USB DP / DM（RNDIS 网卡） | `src/bsp_pinmux.c:179-180` |
| PA24 / PA25 / PA28 / PA29 | SPI1：TF 卡 DIO / DI / CLK / CS | `src/bsp_pinmux.c:183-186` |
| PA39 / PA40 | I2C2 SDA / SCL（挂 LSM6DS3 时；`CONFIG_SENSORS_LSM6DSL` 未开则只在驱动里出现） | `src/sifli_ap.c:246-254,521-522` |
| PA12–PA17 | SDIO 或 MPI2（二者互斥，同一批脚） | `src/bsp_pinmux.c:138-155` |
| PA21 / PA38 / PA44 | 普通 GPIO / VBUS 检测 | `src/bsp_pinmux.c:205,208,209` |
| SA00–SA12 | PSRAM（MPI1） | `src/bsp_pinmux.c:9-97` |

> 别抄 `app/hello_app/README.md:105-119` 那张表：它把 **PA37 写成麦克风**（错），
> 真实情况是 PA37 = CO5300 的 VADD 使能（QADSPI 配置下，见上表）；
> DBI 8080 配置下 PA37 才是 LCD 数据线 `LCDC1_8080_DIO2`（`bsp_pinmux.c:260`）。

## 9. 坑（改代码前务必看）

1. **音频节点带子目录**：是 `/dev/audio/audio0`。写成 `/dev/audio0` 会直接
   `open` 失败（驱动里 `audio_register("audio0")` 由内核拼成 `/dev/audio/audio0`）。
2. **触摸是"下降沿"中断**：FT6146 的 INT（PA31）只挂下降沿，驱动靠它唤醒后读 I2C，
   不是电平轮询。所以：
   - 采样周期由 LVGL 的 indev 读取定时器决定，`robot_ui/main.c:159` 把它设成 **10ms**；
     不改的话默认周期较长，界面点起来"迟钝"。
   - 触摸驱动用 GPIO 中断 + I2C1（400kHz），**直接读 `/dev/input0` 的同时 LVGL 也在读**，
     两个 reader 会分掉触摸样点（你会偶发丢一个点）。自检程序看完就关掉，别长期共存。
3. **LVGL 不是线程安全的**：`lv_*` 控件函数只能在**创建 UI 的那个线程**里调；
   别的任务（网络回调、按键任务、AI 模块）想刷新界面，只能改一个全局标志，
   由主循环统一刷 —— 现成例子见 `docs/network_api_usage.md` 第 4 节的 `net_tick`。
4. **显示是分块缓冲，会看到"横向分块"刷新**：
   `CONFIG_LV_NUTTX_LCD_CUSTOM_BUFFER=y` + `CONFIG_LV_NUTTX_LCD_BUFFER_SIZE=60`（行）
   走的是 PARTIAL 渲染、只推脏区。所以大面积改界面时能看到一条条横带刷新，
   这是正常现象，不是花屏；想一次性整屏刷会把 UI 线程卡住（整屏 390x450x2 = 351KB）。
5. **只改 `board/contest_board/src/`（以及同级的 `include/`、`configs/`）**：
   工作区是**直接编译这个目录**的 ——
   `vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd` 是指向它的**软链接**
   （manifest 里还额外暴露成 `vendor/openvela/boards/contest2026_233_board`，同一份文件）。
   不要在 `vendor/` 下另存一份板级代码，两边会不一致。
6. **`/dev/lcd0` 只有一个"当前画面"**：`robot_ui` 开机 2 秒后自动启动并接管屏幕
   （`sifli_ap.c:706-707`）。任何直接写 `/dev/lcd0` 的自测（如 `hw_test lcdcolor`）
   都会和 LVGL 抢屏幕：你刷完，LVGL 下一次 `lv_timer_handler()` 就会把它的界面盖回去，
   中间还会短暂花屏。**做完自测要复位板子或重跑 `robot_ui`**。
7. **不要随便拉 GPIO 电平**：本板大部分脚都分给了 LCD / 触摸 / 音频 / USB / TF 卡。
   板级唯一分配出去的"用户输出脚"是 PA26（`/dev/gpio1`），要测电平翻转就用它。
8. **不要 `poll()`/`read()` `/dev/buttons`**：真机上会让整机静默卡死（串口零输出、
   USB 设备从主机侧消失，只能重烧），现象和替代路线见第 5 节。
   按键一律走板级 `sf32lb52_boardbtn`（PA11/PA34 当 GPIO 轮询）。
9. **PA34 是"电源键 / 长按复位"脚，没有外部下拉**：`bsp_pinmux.c:171` 故意
   没配它（保住内部下拉，否则 UART 下载驱动会失效）。要读它只能保持下拉、
   只读不写、不申请中断，详见第 5.4 节；不行就把 `BOARD_BTN_ENABLE_HOME`
   置 0 退化成只支持 PA11。

## 10. 自测命令：`hw_test`

硬件对不对，先跑这个（NSH 里敲）：

```
nsh> hw_test
```

```
[1/6] 设备节点枚举      lcd0 / fb0 / input0 / buttons / gpio0..2 / timer0 / pwm0 /
                        i2c0 / i2c1 / adc0 / audio/audio0 逐个打印存在性，
                        eth0 用 netdev API 单独查（它不是 /dev 节点）
[2/6] 触摸 /dev/input0  非阻塞轮询，打印每次的 id/flags/x/y；10 秒内没摸到就打印
                        "未检测到触摸，请用手指点一下屏幕"并正常结束
[3/6] 显示 /dev/lcd0    GETVIDEOINFO / GETPLANEINFO / GETAREAALIGN，打印分辨率、
                        格式、bpp、stride、对齐要求
[4/6] 按键 PA11/PA34  板级 GPIO 轮询（不走 /dev/buttons），按一下就打印
                        键名 + 事件类型 + 按住时长；5 秒超时正常结束
[5/6] GPIO /dev/gpio0..2 打印每个脚的 pintype 和当前电平（只读，不拉电平）
[6/6] 汇总：             一行 PASS/FAIL 统计
```

> `pwm0` / `eth0` 缺失**不是故障**，也不计入"节点缺失"：
> `CONFIG_PWM=n` 时 `/dev/pwm0` 根本不注册（`sifli_ap.c` 里在 `#ifdef CONFIG_PWM` 内），
> `eth0` 则取决于 USB RNDIS 有没有枚举起来。`hw_test` 会直接标出
> "本配置未启用（CONFIG_PWM=n），不是故障" / "USB RNDIS 未枚举，不是故障"。

**默认（不带参数）是只读自检**：不动屏幕、不拉 GPIO 电平。
会改硬件的动作都放在子命令里：

| 命令 | 作用 |
|------|------|
| `hw_test` | 只读自检（不动屏幕、不拉电平） |
| `hw_test touch <秒>` | 指定触摸观察时长（`hw_test touch 0` = 跳过触摸步骤） |
| `hw_test lcdcolor` | 额外刷 4 条横向色带（红/绿/蓝/白），**退出前清屏成纯黑** |
| `hw_test gpio` | 额外对 `/dev/gpio1`（PA26）做一次 1→回读→0 的电平翻转 |
| `hw_test imu [帧数]` | 读 IMU(LSM6DS3) 加速度/陀螺，默认 10 帧。**单独运行**（不跑上面 5 步自检）；当前固件缺 `CONFIG_SENSORS_LSM6DSL` 会是 FAIL，见 `docs/sensor_rtc_usage.md` |
| `hw_test rtc [秒]` | 读 RTC 时间 + 设 N 秒后 alarm（默认 3），带超时。**单独运行**；见 `docs/sensor_rtc_usage.md` |
| `hw_test audio [秒]` | 录 N 秒到内存（默认 2），打印 peak/avg 与是否检测到声音，不写文件。**单独运行**；走板级封装 `audio_in_*`，见 `docs/audio_driver_usage.md` 第 9 节 |
| `hw_test button [秒]` | 等按键按下（默认 15 秒）：按到 `PA11(KEY)` 或 `PA34(HOME)` 会打印**键名 + 事件类型 + 按住时长**并报 `[PASS] 按键`，**超时 FAIL**。走板级 `sf32lb52_boardbtn`（GPIO），**不读 `/dev/buttons`**，见第 5 节。**单独运行** |
| `hw_test lcd [0..100]` | 设屏幕亮度（默认 100）再回读。**单独运行**；0..100 全档都应 PASS（板级封装走 `SETCONTRAST`，依赖 vendor 亮度补丁），见第 3.3 节 |
| `hw_test lcdreinit` | 面板重新初始化（**黑屏救回**）：重发面板初始化序列（含拉一次 RESET 脚）+ 重设像素格式/亮度/DisplayOn，再请界面全屏重绘一次。**单独运行**；整屏黑但串口还活着时敲它，见第 3.4 节 |
| `hw_test status` | 打一份统一外设状态（`board_status_get` / `board_status_dump`）：网络 / MQTT / ROM 素材 / `/data` / 音频 / 显示 / 触摸 / 按键 / RTC / 运行时间。**只有"网络拿到非回环 IPv4 地址"算 PASS/FAIL**，其它设备缺失只打印 `[提示]`，见 `docs/board_status_usage.md`。**单独运行** |

说明：

- 任一步失败都**不会卡死或崩**：所有 `open`/`ioctl`/`read` 都判返回值，
  读之前先用 `poll()` 带超时，失败只打印 `FAIL` 然后继续下一步。
- **按键不再碰 `/dev/buttons`**：第 4 步和 `hw_test button` 都走板级
  `sf32lb52_boardbtn`（PA11/PA34 当 GPIO 轮询）。那个节点的 poll/read 在真机上
  会让整机静默卡死，见第 5 节。第 4 步是"非交互"的（没人按也算 PASS），
  要真正验证按键请用 `hw_test button`（超时 FAIL）。
- `hw_test status` **只做轻量探测**：open/close、stat、往 `/data` 写一个立刻
  删掉的 0 字节探针、读一次 RTC；**不初始化任何设备**（LCD/触摸/音频可能正被
  别的模块开着）。判定上是"网络没拿到 IPv4 地址 = FAIL，其它只提示"，
  见 `docs/board_status_usage.md`。
- `lcdcolor` 会直接写屏，而 `robot_ui`（LVGL）可能正开着，两者会互相覆盖；
  跑完请**复位板子或重跑 `robot_ui`** 恢复界面。`hw_test` 自己**不启动 LVGL**。
- **必需**节点缺了才是 `--` + 一条 `FAIL`；`pwm0`（`CONFIG_PWM=n`）和 `eth0`
  （RNDIS 未枚举）是 `optional` 条目，缺失只打印提示，不计入缺失计数。
  看第 2 节的表对照是谁没注册上。

实现见 `app/hw_test/main.c`；配置开关 `CONFIG_EXAMPLES_HW_TEST`（`app/hw_test/Kconfig`）。
