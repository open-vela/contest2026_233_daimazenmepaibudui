/****************************************************************************
 * board/contest_board/src/lcd_mirror.h
 *
 * 屏幕镜像（板端 -> PC）：把界面像素发到电脑上显示。
 * 用途：屏幕硬件坏了，拿电脑当显示器继续开发和演示。
 *
 * 分工（为什么长这样，见 lcd_mirror.c 文件头）：
 *   - 本模块（板级，**不依赖 LVGL**）：影子帧缓冲、脏行记账、镜像任务、
 *     传输（TCP 默认 / 串口手动）、nsh 命令的后端。
 *   - app/robot_ui/lcd_mirror_glue.c：唯一碰 LVGL 的地方 —— 挂
 *     LV_EVENT_FLUSH_START，把每块脏矩形喂给 lcd_mirror_capture()。
 *   - app/hw_test/main.c：`hw_test lcdmirror ...` 子命令。
 *
 * 内存：影子帧缓冲 390*450*2 = 351000 B + RLE scratch（30 行 * 780 = 23400 B），
 * 都是运行时从堆上申请（本板 8 MB PSRAM 已纳入用户堆，静态 sram 一个字节都不加）。
 *
 ****************************************************************************/

#ifndef __BOARD_CONTEST_BOARD_SRC_LCD_MIRROR_H
#define __BOARD_CONTEST_BOARD_SRC_LCD_MIRROR_H

#include <stdint.h>
#include <stdbool.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 面板原生分辨率（CO5300，RGB565，QSPI）。协议 v2 就按这个写死。 */
#define LCD_MIRROR_PANEL_W        390
#define LCD_MIRROR_PANEL_H        450
#define LCD_MIRROR_ROW_BYTES      (LCD_MIRROR_PANEL_W * 2)          /* 780 */
#define LCD_MIRROR_FB_BYTES       (LCD_MIRROR_ROW_BYTES * LCD_MIRROR_PANEL_H)

/* 默认目标：PC / ICS 侧。想改就 `hw_test lcdmirror <ip> [port]`
 * （或者 `hw_test lcdmirror tcp <ip> [port]` 顺带切回 TCP），
 * 或在代码里改这两个默认值（掉电不保存，重启回默认）。 */
#define LCD_MIRROR_DEFAULT_IP     "192.168.137.1"
#define LCD_MIRROR_DEFAULT_PORT   5600

/* 传输方式。**默认 TCP**，开机不会自动切串口（见下面一段）。 */
#define LCD_MIRROR_TRANSPORT_TCP  0
#define LCD_MIRROR_TRANSPORT_UART 1

/* ==================== 串口(UART)传输：另一条腿 ====================
 * 网络（USB RNDIS）那一段反复挂的时候，镜像还有一条**从来没出过问题**的
 * 链路可用：控制台串口。代价是帧的二进制字节和控制台文字挤在同一条线上，
 * 所以串口模式**只手动开、随时能停**，绝不自动切：
 *   hw_test lcdmirror uart [dev]       切到串口（任务自己换 fd，不重启）
 *   hw_test lcdmirror tcp <ip> [port]  切回 TCP
 *   hw_test lcdmirror status           看当前传输 + 节点名 / 目标地址
 *
 * 帧格式**和 TCP 一模一样**（20 字节 v2 头 + flags bit0 的 RLE 或原样载荷），
 * 协议一个字节都没改：PC 端还是那套解析器，坏帧（日志插进来的字节）由 PC
 * 侧重同步丢掉，靠每 3 秒一次的整屏关键帧自愈。
 *
 * 默认节点是 /dev/console（控制台 UART1，就是 PC 上那个 COM 口）。依据：
 *   - 本板 CONFIG_BSP_USING_UART1=y / CONFIG_BSP_USING_UART2=y，
 *     `vendor/sifli/chips/sf32lb52/sifli_uart.c` 的 arm_serialinit() 把
 *     **第 CONSOLE_UART 个**（sf32lb_serial.h 里 CONSOLE_UART = 1，即 UART1）
 *     UART 注册成 /dev/console，剩下的才按 ttyS0、ttyS1 顺次编号；
 *   - 所以在这份配置里 /dev/ttyS0 是**第二个** UART（UART2，板级 README 里
 *     那条"调试 log"），不一定接在 PC 上。要试它就把节点名当参数给：
 *     `hw_test lcdmirror uart /dev/ttyS0`。
 *
 * 反向触摸在串口模式下**不走帧的字节流**（二进制包会被 NSH 的行输入吃掉），
 * 改成 PC 往串口里写一行文本 `hw_test lcdtap <x> <y> <0|1>`，由 NSH 执行。 */
#define LCD_MIRROR_DEFAULT_UART_DEV   "/dev/console"
#define LCD_MIRROR_UART_DEV_MAX       32   /* 含结尾 '\0' */

/* 串口的整屏关键帧周期（毫秒）：每这么久把所有行重新标脏、发一次整屏。
 *
 * ⚠️ **别改小**（2026-09-17 真机教训）：原来用 3000ms，结果每 3 秒就要拿 1.4 秒的
 * 线时把整屏重发一遍，这段时间里用户点出来的更新全排在它后面 —— 实测载荷只有
 * 12 KB/s（线速的 1/5）却感觉 3~4 fps，就是这个原因。现在它只当**兜底自愈**：
 * 正常靠 PC 侧发现坏带子后发 `hw_test lcdmirror resend <y0> <rows>` 精确补那几条
 * （PC 已经解出了帧头，知道坏的是哪一段），路面上不再定期刮一遍全屏。 */
#define LCD_MIRROR_UART_KEYFRAME_MS   30000

/* 每帧的小端头（20 字节），和 PC 端 _flash/lcd_mirror.py 严格一致：
 *   u8 magic0='L' u8 magic1='M' u8 ver=2 u8 flags
 *   u16 x u16 y u16 w u16 h    (LE)
 *   u32 seq u32 payload_len    (LE)
 * flags bit0（LCD_MIRROR_FLAG_RLE）：
 *   0 = 载荷是原样 w*h*2 字节 RGB565 小端、行优先、无行间填充；
 *   1 = 载荷是 RLE 流：重复 { u8 run(1..255), u16 pixel_le }，按行优先覆盖
 *       w*h 个像素，所有 run 之和必须正好等于 w*h。
 * 压不小（RLE 不小于原样）时板端直接退回原样发，flags 的 bit0 就是 0。
 *
 * **一帧最多 LCD_MIRROR_MAX_ROWS_PER_FRAME 行**（w 恒为 390）：这个上限由板端
 * 那块 RLE scratch 缓冲的大小决定（见 lcd_mirror.c 的 lm_take_run / lm_build_frame）。
 * 脏区行数比它多时会被**拆成多帧**发，不是丢行 —— 客户端按 (x,y,w,h) 贴图，
 * 拆帧对画面没影响，只是多几个 20 字节的头。 */
#define LCD_MIRROR_MAGIC0         'L'
#define LCD_MIRROR_MAGIC1         'M'
#define LCD_MIRROR_VER            2
#define LCD_MIRROR_FRAME_HDR      20
#define LCD_MIRROR_FLAG_RLE       0x01
#define LCD_MIRROR_MAX_ROWS_PER_FRAME  30

/* ==================== 反向消息：PC -> 板，模拟触摸 ====================
 * 同一条 TCP 连接上反向发，固定 8 字节：
 *   u8 magic0='L' u8 magic1='T' u8 ver=1 u8 pressed
 *   u16 x u16 y                (LE，面板坐标 0..389 / 0..449)
 * pressed: 1 = 按下（含拖动中的每一次移动），0 = 抬起。
 *
 * 板端**不会因为收到坏字节就断连接**（帧还要继续发）：只在反向缓冲里
 * 一个字节一个字节地重新找 'L''T'，找不到就丢掉那个字节。 */
#define LCD_MIRROR_TOUCH_MAGIC0   'L'
#define LCD_MIRROR_TOUCH_MAGIC1   'T'
#define LCD_MIRROR_TOUCH_VER      1
#define LCD_MIRROR_TOUCH_MSG      8

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* 开关：开机自动起。可被 lcd_mirror.c 里的 LCD_MIRROR_AUTOSTART 关掉
 * （关掉后 `hw_test lcdmirror start` 仍然可用）。返回 OK / 负错误码，
 * 已经在跑时返回 OK（幂等）。 */
int lcd_mirror_autostart(void);

int lcd_mirror_start(void);

/* 优雅停：置停止标志等任务自己退出（不跨任务 delete），最多等 1 秒。 */
int lcd_mirror_stop(void);

bool lcd_mirror_is_running(void);

/* 打印一行状态（NSH 里调，printf 出到串口）。 */
void lcd_mirror_status(void);

/* 改目标地址。ip 传 NULL / 空串 = 只改端口。 */
int lcd_mirror_set_target(const char *ip, uint16_t port);

const char *lcd_mirror_target_ip(void);
uint16_t lcd_mirror_target_port(void);

/****************************************************************************
 * Name: lcd_mirror_use_uart / lcd_mirror_use_tcp
 *
 * Description:
 *   换传输方式。两个都**只置一个标志**，当前那条连接由镜像任务自己收尸
 *   （铁律：fd 是谁开的谁关），任务下一轮就把旧 fd 关掉、按新传输重开 ——
 *   所以调用方不会被阻塞，也不会出现"两个人抢一个 fd"。
 *
 *   dev 传 NULL / 空串 = 用默认节点（/dev/console）；ip 传 NULL / 空串 =
 *   只改端口。传输方式**掉电不保存**：默认永远是 TCP，开机绝不自动切串口。
 *   返回 OK / 负错误码（节点名或 IP 太长）。
 *
 ****************************************************************************/

int lcd_mirror_use_uart(const char *dev);
int lcd_mirror_use_tcp(const char *ip, uint16_t port);

bool lcd_mirror_is_uart(void);
const char *lcd_mirror_uart_dev(void);

/****************************************************************************
 * Name: lcd_mirror_capture
 *
 * Description:
 *   刷屏钩子：把一块脏矩形（RGB565，行优先、行间按 stride 跨步）拷进影子
 *   帧缓冲，并把对应行标脏。**在 LVGL 线程里被调**，只做一次 memcpy +
 *   置位，拿锁时间以微秒计，不会等 socket、不会等镜像任务。
 *
 *   x/y/w/h 是面板坐标；px 指向这块矩形的第一个像素；stride 是一行的字节
 *   数（LVGL 部分刷新模式下就是 w*2，但按设备传进来的值算，不猜）。
 *   越界部分会被裁掉；w/h 为 0 直接返回。影子缓冲没申请成功时是空操作。
 *
 ****************************************************************************/

void lcd_mirror_capture(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                        const uint8_t *px, uint32_t stride);

/* 把整屏标脏（下次发送就是一整张贴图），并请界面侧整屏重绘一次。 */
void lcd_mirror_request_full_redraw(void);

/****************************************************************************
 * Name: lcd_mirror_take_redraw_request
 *
 * Description:
 *   取走"请整屏重绘一次"这个请求（取走即清零）。**只能由 LVGL 线程调**
 *   （glue 里那个 lv_timer），拿到 true 就 lv_obj_invalidate(lv_screen_active())。
 *   为什么要它：影子缓冲在界面起来之前是空的，必须让 LVGL 把整屏重画一遍
 *   才会有内容；而且 LVGL 只重画脏区，不给这一下，镜像里会一直缺一块。
 *
 ****************************************************************************/

bool lcd_mirror_take_redraw_request(void);

/****************************************************************************
 * Name: lcd_mirror_get_touch
 *
 * Description:
 *   取"电脑上鼠标模拟的触摸"的**当前状态**（由镜像任务在反向通道上收到）。
 *   **只在镜像任务里收，这里只负责读** —— 谁 open 的 fd 谁用，别人不碰 socket。
 *
 *   语义（故意不是"取走即清空"）：LVGL 的 read_cb 每个周期都要问一次
 *   "现在按着没有、在哪"，所以这里给的是**最新状态**，可以反复读：
 *     - 返回 false：镜像没在跑，或者从来没收到过合法的触摸消息。
 *       此时 *x / *y / *pressed **一个字都不改**，调用方自己给默认值。
 *     - 返回 true ：*x / *y 是面板坐标（已经钳在 0..389 / 0..449），
 *       *pressed 是当前是"按下"还是"抬起"。
 *   断开/停止之后会强制变成"抬起"，但坐标还留着最后那一次（诊断用）；
 *   所以"返回 true 且 pressed=false"是正常状态，不是错误。
 *
 *   另外做了一件"锁存"：如果"按下"和"抬起"都发生在两次读取之间
 *   （鼠标快速点一下，PC 只发了两条消息），第一次读会先补一个 PRESSED，
 *   下一次读才给 RELEASED。不做这个的话 LVGL 只看到 RELEASED，
 *   这一下点击就被吞掉了 —— 现场"点了没反应"最容易是这么来的。
 *
 *   抬起语义是绝对的：镜像停掉、连接断开时都会强制清成"抬起"，
 *   绝不留在"按下不放"（那会让界面永远卡在一个按下状态）。
 *
 ****************************************************************************/

bool lcd_mirror_get_touch(uint16_t *x, uint16_t *y, bool *pressed);

/****************************************************************************
 * Name: lcd_mirror_inject_touch
 *
 * Description:
 *   手动注入一次触摸（面板坐标，越界钳到面板内）。内部就是反向通道那条
 *   路（lm_touch_store），语义和 PC 从 TCP 反向上发来的 'L''T' 消息完全
 *   一样："按下"会锁存一次（快速点一下不丢），镜像停/断开一律清成抬起。
 *
 *   为什么要它：**串口模式下反向触摸不能走帧的字节流** —— 往控制台串口里
 *   写二进制触摸包会被 NSH 的行输入解析吃掉。所以 PC 改成往串口里写一行
 *   文本命令 `hw_test lcdtap <x> <y> <0|1>`，由 NSH 执行、走到这里。
 *   拖动就是连续发 `lcdtap x y 1`，最后一次 `lcdtap x y 0` 抬手。
 *
 *   镜像没在跑时调用它是安全的（状态照存，但 lcd_mirror_get_touch 不给出去）。
 *
 ****************************************************************************/

void lcd_mirror_inject_touch(uint16_t x, uint16_t y, bool pressed);

/****************************************************************************
 * Name: lcd_mirror_resend_rows / lcd_mirror_resend_all
 *
 * Description:
 *   把某些行重新标脏，让镜像下一轮再发一遍。
 *
 *   为什么要它（2026-09-17 真机）：串口上控制台日志会插进帧字节里，被插坏的帧
 *   PC 侧只能丢掉 —— 那一块画面就旧了。原来靠"每 3 秒重发整屏"自愈，结果那趟
 *   整屏把 1.4 秒的线时全占了，用户点出来的更新全排在它后面（实测载荷只有线速的
 *   1/5 却感觉 3~4 fps）。现在改成**精确认领**：PC 已经解出了帧头，知道坏的是
 *   哪一段（y0, rows），于是命令行发一条 `hw_test lcdmirror resend <y0> <rows>`，
 *   板子只补这几行。
 *
 *   - rows 会被钳到 1..LCD_MIRROR_PANEL_H，y0 超出面板范围就什么都不做；
 *   - 镜像没在跑时是空操作（不报错）；
 *   - resend_all 等价于"发一次整屏关键帧"，只在 PC 刚连上或人工诊断时用。
 ****************************************************************************/

void lcd_mirror_resend_rows(uint16_t y0, uint16_t rows);
void lcd_mirror_resend_all(void);

#endif /* __BOARD_CONTEST_BOARD_SRC_LCD_MIRROR_H */
