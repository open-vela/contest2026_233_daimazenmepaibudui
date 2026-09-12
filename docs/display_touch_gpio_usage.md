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
| 做逻辑 | 直接读 `/dev/input0`（触摸）、`/dev/buttons`（按键）、`/dev/gpioN`，或走 LVGL 回调 |

屏幕是 **390x450、RGB565**，走 `LV_USE_NUTTX_LCD`（`/dev/lcd0`）+ 分块缓冲。

## 2. 设备节点总表

| 节点 | 用途 | 接口类型 | 头文件 |
|------|------|---------|--------|
| `/dev/lcd0` | AMOLED 显示（主接口） | NuttX LCD dev：`open` + `ioctl(LCDDEVIO_GETVIDEOINFO / GETPLANEINFO / GETAREAALIGN / PUTAREA)` | `<nuttx/lcd/lcd_dev.h>` |
| `/dev/fb0` | 同一块屏的 framebuffer 前端 | NuttX fb：`open` + `ioctl(FBIOGET_*)` | `<nuttx/video/fb.h>` |
| `/dev/input0` | FT6146 触摸（`touch_register(...,16)`） | touchscreen upper half：`open` + `read()` 得到 `struct touch_sample_s` | `<nuttx/input/touchscreen.h>` |
| `/dev/buttons` | 按键（Key2 = PA11） | buttons upper half：`open` + `read()` 得到 `btn_buttonset_t` | `<nuttx/input/buttons.h>` |
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

## 5. 按键：`/dev/buttons`

```c
#include <nuttx/input/buttons.h>
#include <fcntl.h>
#include <poll.h>

int fd = open("/dev/buttons", O_RDONLY | O_NONBLOCK);
if (fd < 0) { /* FAIL */ }

for (;;)
  {
    struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
    uint8_t buttons = 0;              /* btn_buttonset_t，按位表示按键集合 */

    if (poll(&pfd, 1, 500) <= 0)      /* 超时就退出，不要死等 */
      {
        break;
      }

    if (read(fd, &buttons, sizeof(buttons)) < (ssize_t)sizeof(buttons))
      {
        continue;
      }

    printf("buttonset=0x%02x, Key2 %s\n",
           (unsigned)buttons, (buttons & 1) ? "按下" : "松开");
  }

close(fd);
```

本板只注册了**一个**按键：`Key2 = PA11`，位 0（`BUTTON_KEY2 = 0`，见
`board/contest_board/include/board.h:33`）。驱动上层每 10ms 也轮询一次
（`sf32lb52_buttons.c:45`），中断/按键状态变化由 `btn_register()` 统一上报，所以
别的应用同时读也不会读坏驱动状态。

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
| PA11 | Key2 用户按键（`/dev/buttons` 唯一按键） | `src/bsp_pinmux.c:173`、`src/sf32lb52_buttons.c:43`、`include/board.h:30-36` |
| PA34 | Key1 电源键脚；同时是 `GPIO_IN1` / `GPIO_INT1`（`/dev/gpio0`、`/dev/gpio2`） | `src/sf32lb52_devkit_lcd.h:33,36`、`src/bsp_pinmux.c:167-171`（注释标注 Key1） |
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

## 10. 自测命令：`hw_test`

硬件对不对，先跑这个（NSH 里敲）：

```
nsh> hw_test
```

```
[1/6] 设备节点枚举      lcd0 / fb0 / input0 / buttons / gpio0..2 / timer0 / pwm0 /
                        i2c0 / i2c1 / adc0 / audio/audio0 / eth0 逐个打印存在性
[2/6] 触摸 /dev/input0  非阻塞轮询，打印每次的 id/flags/x/y；10 秒内没摸到就打印
                        "未检测到触摸，请用手指点一下屏幕"并正常结束
[3/6] 显示 /dev/lcd0    GETVIDEOINFO / GETPLANEINFO / GETAREAALIGN，打印分辨率、
                        格式、bpp、stride、对齐要求
[4/6] 按键 /dev/buttons 非阻塞读，按一下 Key2（PA11）就打印键值；5 秒超时正常结束
[5/6] GPIO /dev/gpio0..2 打印每个脚的 pintype 和当前电平（只读，不拉电平）
[6/6] 汇总：             一行 PASS/FAIL 统计
```

**默认（不带参数）是只读自检**：不动屏幕、不拉 GPIO 电平。
会改硬件的动作都放在子命令里：

| 命令 | 作用 |
|------|------|
| `hw_test` | 只读自检（不动屏幕、不拉电平） |
| `hw_test touch <秒>` | 指定触摸观察时长（`hw_test touch 0` = 跳过触摸步骤） |
| `hw_test lcdcolor` | 额外刷 4 条横向色带（红/绿/蓝/白），**退出前清屏成纯黑** |
| `hw_test gpio` | 额外对 `/dev/gpio1`（PA26）做一次 1→回读→0 的电平翻转 |

说明：

- 任一步失败都**不会卡死或崩**：所有 `open`/`ioctl`/`read` 都判返回值，
  读之前先用 `poll()` 带超时，失败只打印 `FAIL` 然后继续下一步。
- `lcdcolor` 会直接写屏，而 `robot_ui`（LVGL）可能正开着，两者会互相覆盖；
  跑完请**复位板子或重跑 `robot_ui`** 恢复界面。`hw_test` 自己**不启动 LVGL**。
- 每个节点缺了就是一行 `--` + 一条 `FAIL`，看第 2 节的表对照是谁没注册上。

实现见 `app/hw_test/main.c`；配置开关 `CONFIG_EXAMPLES_HW_TEST`（`app/hw_test/Kconfig`）。
