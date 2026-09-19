/****************************************************************************
 * board/contest_board/src/lcd_mirror.c
 *
 * 屏幕镜像（板端 -> PC）—— 屏幕硬件坏了，拿电脑当显示器继续开发和演示。
 *
 * ============================ 像素从哪儿取 ============================
 *
 * **没有 layer buffer 可读，所以走刷屏钩子。** 证据（都在 vendor/sifli 树里）：
 *
 *   - 本板的面板是 CO5300，走 QSPI 4 线 + DCX，显示控制器 LCDC 只当"搬运工"：
 *     `vendor/sifli/boards/sf32lb52/drivers/lcd/co5300.c` 的
 *     LCD_SetRegion / WriteMultiplePixels 直接用
 *     `HAL_LCDC_SendLayerData2Reg_IT()` 把像素推进面板 GRAM，**中间不落 RAM**。
 *   - 全树 grep 不到任何把 `LCDC_LayerCfgTypeDef.data`（或 `ulAddr` /
 *     `LayerSetAddress`）填起来的地方 —— 那是 RGB 并口模式才用的字段。
 *   - 驱动自己就把话说死了：`sf32lb_lcd.c` 的 `sf32lb_lcd_getplaneinfo()`
 *     里 `pinfo->buffer = NULL`，即 /dev/fb0 是"直写型"帧缓冲，没有内存副本。
 *
 * 所以"零拷贝读 layer buffer"这条路在本板上不成立。改在**刷屏路径挂 hook**，
 * 而 hook 点选在 **LVGL 的 LV_EVENT_FLUSH_START**（不是 vendor 驱动里）：
 *
 *   1. **不碰任何 vendor / openvela 文件**。`app/robot_ui/ui_perf.c` 已经证明
 *      这条路可行 —— LVGL 9.1 在调 flush_cb 的前后各发一个事件
 *      （`lv_refr.c` 的 `call_flush_cb()`），事件里能同时拿到"这块脏区"和
 *      "装着这块像素的 draw buffer"（`lv_display_get_buf_active()`）。
 *   2. 拿到的就是 LVGL 交给驱动的同一批像素，不经第二次格式转换。
 *   3. 脏区是 LVGL 自己算的，比"每 200ms 重发整屏"省得多 —— 静止画面
 *      几乎不发数据，动的地方才发。
 *
 * 代价是**只有走 LVGL 的刷屏才被镜像**（比如 hw_test lcdcolor 那种直接
 * ioctl 到 /dev/fb0 的刷色不进镜像）。本板的界面就是 robot_ui，够用。
 *
 * ======================= 为什么不是"小队列"方案 =======================
 *
 * 任务书里的备选方案是"有界小队列 + 切片"。这里换成了**影子帧缓冲**
 * （390*450*2 = 351000 B，运行时 malloc，本板 8 MB PSRAM 已纳入用户堆），
 * 理由：
 *
 *   - 队列方案要保证"一次整屏重绘不丢帧"，否则客户端会缺一块、只能再请
 *     界面重绘（丢帧 -> 重绘 -> 再丢，来回抖）。而一次整屏重绘就是 351 KB，
 *     队列**必须**能装下这 351 KB 才不丢 —— 内存上限和小队列一样，复杂度
 *     却高得多（切片、跨槽续传、溢出策略）。
 *   - 影子缓冲天然是"当前屏幕"，任何时刻客户端要整屏都能立刻给，断线重连
 *     不需要惊动 LVGL。队列做不到这一点（旧帧已经丢了）。
 *   - 影子缓冲不占 SRAM：**静态 BSS 一个字节都不加**，全部在堆上；而且
 *     351 KB 的单次请求超过程序末端到 SRAM 顶的那段堆，分配必然落在 PSRAM。
 *
 * 对刷屏路径的额外开销，两种方案是一样的（都是每块脏区一次 memcpy）；
 * 影子缓冲那边只是置几个脏行位，更少。
 *
 * 唯一的取舍：镜像任务读影子缓冲时 LVGL 线程可能正在写同一段，**快速变化的
 * 区域在客户端可能看到撕裂**（下一帧就自愈）。刷屏路径完全不阻塞 —— 这才
 * 是硬要求，撕裂可以接受。
 *
 * ============================== 线程与 fd ==============================
 *
 * 项目铁律：**谁 open 的 fd 只能由它自己那个 task group 的线程用**。
 * 所以 socket 由镜像任务**自己在任务里**创建、自己用；别人（NSH、LVGL
 * 线程）一个字节都不碰。镜像任务优先级 100 —— 高于 robot_ui(110)、
 * lpwork/net_task(100 同级)、hpwork(224)，不会被刷屏任务饿死；而它自己
 * 只在 socket 上做非阻塞 send，永远不会反过来卡住刷屏。
 *
 * 发送节奏：每 40 ms 一个窗口（LCD_MIRROR_TICK_MS），窗口内最多 16 段脏区、
 * 24576 字节。24576 / 40ms = 614 KB/s，落在实测 RNDIS 吞吐（0.3~0.7 MB/s）
 * 的区间里 —— 预算不比链路更松，绝不往 socket 里堆超过对面收得下的量。
 * 静止画面一个字节都不发。发不完的帧留到下一轮接着发（**绝不截断帧**，
 * 截断会让客户端解析错位）。
 *
 * ============================ 载荷压缩（协议 v2） ============================
 *
 * 界面上大块纯色/渐变很多，原样发 RGB565 太费（整屏 351 KB）。v2 的每帧载荷
 * 可以带 RLE：重复 { u8 run(1..255), u16 pixel_le }，按行优先覆盖 w*h 个像素，
 * run 之和正好 w*h。压不小就退回原样（flags 的 bit0=0），所以**任何画面都
 * 不会因为压缩而变大**。
 *
 * 压缩目标是一块**一次性 malloc** 的 scratch（LCD_MIRROR_RLE_SCRATCH_BYTES =
 * 一帧上限 30 行 x 780 字节 = 23400 B），不是每帧 malloc —— 刷屏路径上绝不能
 * 有动态分配。scratch 的大小反过来决定了**一帧最多 30 行**（见 lcd_mirror.h
 * 的 LCD_MIRROR_MAX_ROWS_PER_FRAME），行数更多的脏区由 lm_take_run 拆成多帧。
 *
 * 编码器对输出长度是**有界**的：写到"不小于原样长度"就立刻放弃、返回 0
 * （这时候压不压已经无所谓，结果一定是走原样），所以 scratch 只要原样那么大
 * 就绝不会写越界 —— 不需要按最坏 3 字节/像素预留。
 *
 * ========================= 反向通道：鼠标当触摸 =========================
 *
 * 现场的另一半问题：触摸 IC（FT6146，I2C 0x38）不应答、/dev/input0 都没了，
 * 所以"看得见（镜像）但点不了"。这里在**同一条 TCP 连接**上反向收 8 字节
 * 触摸消息（'L''T' ver1 + pressed + x/y），把 PC 上的鼠标当成手指。
 *
 *   - 收报和发帧在**同一个任务、同一个循环**里，非阻塞 recv，不加任务、
 *     不阻塞发送；空闲轮询 20 ms，鼠标点下去最多 20 ms 就进到全局状态。
 *   - 收报的中间状态（半条消息）是任务私有的局部变量，不占全局、不加锁。
 *   - 坏字节**绝不断连接**：一个字节一个字节地重新找 'L''T'（帧还要继续发）。
 *   - 最新状态放在全局，由 LVGL 侧那个虚拟 input device 的 read_cb 取走
 *     （见 lcd_mirror_get_touch 的注释：可以反复读，"按下"会锁存一次，
 *     停/断开一律强制抬起）。
 *
 * ============================== 串口传输（UART） ==============================
 *
 * 为什么加：网络走 USB RNDIS，一挂就是主机侧 miniport 起不来（Code 10），
 * 镜像跟着全黑；而**控制台串口这条链路从来没出过问题**。所以给镜像加一条
 * 串口腿：帧格式一个字都不改（还是 v2 的 20 字节头 + 载荷），PC 端解析器
 * 不用动，"板子 -> PC 一直线"就成立了。
 *
 * 设备节点：默认 /dev/console。依据（都是本板配置里能查到的）：
 *   - `vendor/sifli/chips/sf32lb52/include/sf32lb_serial.h`：CONSOLE_UART = 1；
 *   - `vendor/sifli/chips/sf32lb52/sifli_uart.c` 的 arm_serialinit()：先把
 *     uart_obj[CONSOLE_UART-1]（= UART1）注册成 /dev/console，**再跳过它**，
 *     剩下的 UART 从 minor 0 开始顺次注册成 /dev/ttyS0、/dev/ttyS1...；
 *   - `vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/configs/sf32lb52_ai/
 *     defconfig`（本工程实际用的配置）里只有 UART1 + UART2 —— 于是
 *     /dev/console = UART1（PA18/PA19，PC 上那个 COM 口），
 *     /dev/ttyS0  = UART2（PA20/PA27，README 里那条"调试 log"），
 *     所以默认走 /dev/console，要用 /dev/ttyS0 得自己把节点名当参数给。
 *   - `arm_lowputc()` 也写的是 hwp_usart1，和 /dev/console 是同一条。
 *
 * **不自动切**：串口和控制台日志共用一条线，帧的二进制字节会插进日志里，
 * 所以默认永远是 TCP，`hw_test lcdmirror uart [dev]` 手动开、`tcp` 切回。
 *
 * 三条策略（都是为了"绝不阻塞刷屏、绝不把自己挂在串口上"）：
 *
 *   ① 关键帧自愈：每 LCD_MIRROR_UART_KEYFRAME_MS(3000ms) 把所有行重新标脏，
 *      发一次整屏。串口上日志字节会插进帧里，那些帧 PC 端会重同步丢掉，
 *      所以必须定期有一张"从头到脚"的整屏，让画面自己长回来。
 *   ② 写不动就重发、绝不等待：一次只写 LCD_MIRROR_UART_CHUNK(320) 字节
 *      （串口 TX 环形缓冲只有 CONFIG_UART_BUFSZ = 1024 字节，一次丢 24 KB
 *      进去只会换来短写），写完睡 5ms —— 于是这条腿的速度是 320B/5ms =
 *      64 KB/s，1 Mbaud 线速的 64%，剩下的留给插在同一条线上的日志。
 *      真写不动（EAGAIN / 一个字节都没出去）时**就地放弃这一帧、把这几行
 *      重新标脏**，下一轮从头再发 —— 绝不 sleep 等缓冲，那会把镜像任务
 *      （和刷屏）一起拖住。
 *      注：写出去一部分（短写）不算"写不动"：那部分字节已经在线上，剩下的
 *      接着写（和 TCP 同一条"整帧不截断"的规矩）。**一帧最大 23420 字节、
 *      而缓冲只有 1024 字节，把短写当失败就等于大帧永远发不出去** —— 每次
 *      从头重来，客户端永远凑不出完整一帧。
 *   ③ 反向触摸不走串口字节流：往控制台串口里写二进制触摸包会被 NSH 的行
 *      输入解析吃掉。PC 改成写一行文本 `hw_test lcdtap <x> <y> <0|1>`，
 *      NSH 执行到 lcd_mirror_inject_touch()。所以串口模式下镜像任务
 *      **只开 O_WRONLY、一个字节都不读**（读了就是偷 NSH 的输入）。
 *
 * 和 TCP 的关系：两条腿共用同一个循环、同一套脏行记账/组帧/RLE，只有
 * "开哪种 fd、怎么写、限速多少、要不要收反包"这几点不同。默认永远是 TCP。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sched.h>
#include <syslog.h>
#include <stdint.h>
#include <stdbool.h>

#include <nuttx/clock.h>
#include <nuttx/mutex.h>
#include <nuttx/sched.h>

#include "lcd_mirror.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 开机自动起。置 0 就只留 `hw_test lcdmirror start` 手动开
 * （glue 里那次 attach 仍然会调 lcd_mirror_autostart()，但没有副作用）。 */
#define LCD_MIRROR_AUTOSTART          1

/* 开机自动起时用哪种传输。
 *
 * ⚠️ **必须留 TCP**（2026-09-17 真机教训）：我一度把它改成 UART，结果一开机镜像
 * 就把控制台 UART 的 TX 环形缓冲占满，`nsh` 的 write() 永远排不上队 —— **连
 * `hw_test lcdmirror stop` 都发不进去**，控制台等于死了（只剩帧字节）。
 * 现在改成"**由 PC 端发起切换**"：板子开机还是 TCP（控制台干净），
 * `_flash/lcd_mirror.py --serial COMx` 起来时自己发一条 `hw_test lcdmirror uart`，
 * 退出时再发一条 `hw_test lcdmirror stop`。用户不用记命令，也不会淹死控制台。
 * 串口那条腿实测点一下 9ms 出画面、整屏 1~1.5 秒，断网/换地方都能用。 */
#define LCD_MIRROR_AUTOSTART_TRANSPORT  LCD_MIRROR_TRANSPORT_TCP

#define LCD_MIRROR_TASK_NAME          "lcd_mirror"

/* 高于 robot_ui(110)。数字越小优先级越高。栈只要 4 KB：任务里没有大局部变量，
 * 发出去的数据直接指向影子缓冲，不拷到栈上。 */
#define LCD_MIRROR_TASK_PRIORITY      100
#define LCD_MIRROR_TASK_STACK         4096

/* 等连接的最长时间（非阻塞 connect + select 超时） */
#define LCD_MIRROR_CONNECT_TIMEOUT_MS 3000

/* 断线重连间隔 */
#define LCD_MIRROR_RETRY_MS           2500

/* 空闲时的轮询间隔。**不能按发送窗口来睡**：反向的触摸消息也在这个循环里收，
 * 睡满一个窗口就等于鼠标点下去要等一个窗口那么久才轮到 LVGL，拖动手感直接
 * 废掉。20ms 远小于窗口（40ms），足够快，睡着时也不占 CPU。
 * （发送速率不是靠"睡多久"限的，是靠下面的发送窗口 + 字节预算。） */
#define LCD_MIRROR_POLL_MS            20

/* 发送窗口与窗口内的字节预算。窗口从 150 ms 压到 40 ms（帧率 ≈ 25 张/s 上限），
 * 但**字节预算反而收紧**：24576 B / 40 ms = 614 KB/s，落在实测的 USB RNDIS
 * 吞吐（0.3~0.7 MB/s）里面。窗口只是"多久重算一次预算"，真正的限速是字节数，
 * 所以调小窗口不会把链路打爆 —— 反而让脏区延迟从最多 150 ms 降到 40 ms。 */
#define LCD_MIRROR_TICK_MS            40
#define LCD_MIRROR_MAX_RECTS_PER_TICK 16
#define LCD_MIRROR_MAX_BYTES_PER_TICK 24576

/* RLE scratch：一帧最多 LCD_MIRROR_MAX_ROWS_PER_FRAME 行，每行 780 字节。
 * 原样长度就是编码器的上界（写满这么多还压不下去就直接放弃走原样）。 */
#define LCD_MIRROR_RLE_SCRATCH_BYTES \
  (LCD_MIRROR_MAX_ROWS_PER_FRAME * LCD_MIRROR_ROW_BYTES)

/* 上一帧还没发完时的短睡（保持链路吞吐，不空转） */
#define LCD_MIRROR_RETRY_SEND_MS      10

/* "起不来/发不出去"这类日志限频：每 N 次失败打一行 */
#define LCD_MIRROR_LOG_EVERY          10

/* 停止时等任务自己退出的上限 */
#define LCD_MIRROR_STOP_WAIT_MS       1000

/* ==================== 串口传输的三个数（见文件头"串口传输"） ==================== */

/* 关键帧周期在 lcd_mirror.h（LCD_MIRROR_UART_KEYFRAME_MS）：PC 端要知道
 * 自愈的节奏，所以它是公开常量。这里只剩下面这三个纯实现细节。 */

/* 一次 write() 最多写这么多字节，写完睡 LCD_MIRROR_UART_RETRY_SEND_MS，于是
 * 串口这条腿的速度就是 320B / 5ms = 64 KB/s —— 1 Mbaud 线速（100 KB/s）的
 * 64%，剩下的三分之一留给插在同一条线上的控制台日志。
 *
 * 上限本身还有第二个理由：串口 TX 环形缓冲只有 CONFIG_UART_BUFSZ = 1024 字节，
 * 一次丢 24 KB 进去，驱动只会收下缓冲装得下的那部分、返回短写，剩下全得靠
 * 重发（策略②）。320 字节远小于 1024，正常节奏下 write() 都是整段成功。 */
#define LCD_MIRROR_UART_CHUNK         320

/* 串口模式下**一帧最多几行**（TCP 仍用 LCD_MIRROR_MAX_ROWS_PER_FRAME=30）。
 *
 * 为什么要单独收窄（2026-09-17 真机）：30 行的带子压缩后最大 ~23 KB，PC 侧一旦
 * 被 GUI 重画占住（1.5 倍缩放那一下能占几十毫秒），Windows 串口接收缓冲（默认
 * 才 4096 字节）就溢出、这一帧被截断 —— 实测一批整屏关键帧全废、下半屏长期黑。
 * 砍到 8 行（压缩后 ~1~2 KB）之后，同样的停顿最多毁掉 8 行，而且下一轮就补回来。
 * 代价是整屏关键帧的帧数变多（450/8 = 57 帧），但每帧都很小，串口上反而更稳。 */
#define LCD_MIRROR_UART_MAX_ROWS      8

/* 串口模式下"定期整屏关键帧"的间隔**在 lcd_mirror.h**（LCD_MIRROR_UART_KEYFRAME_MS，
 * 因为 PC 端也要知道这个节奏）。这里只留一句话提醒：它已经从 3 秒放到 30 秒，
 * 别再改小 —— 每 3 秒一趟整屏会把用户的即时更新全排在后面（实测载荷只有线速的
 * 1/5 却感觉 3~4 fps）。精确自愈靠 PC 发 `hw_test lcdmirror resend <y0> <rows>`。 */

/* 串口上（一帧还没写完时）的短睡。它同时就是串口这条腿的**节奏阀**：
 * 一轮最多写 LCD_MIRROR_UART_CHUNK 字节，然后睡这么久 —— 速度 = 上面那个
 * 320B/5ms。不存在"窗口预算"这一层，因为串口只需要一个上限防止把自己
 * 堆在缓冲里，不需要 TCP 那种"多久重算一次预算"的窗口。 */
#define LCD_MIRROR_UART_RETRY_SEND_MS 5

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 影子帧缓冲：RGB565、行优先、每行 780 字节（无行间填充）。
 * 运行期 malloc，本板落在 PSRAM。**不在 BSS 里**。 */
static uint8_t *g_shadow;

/* RLE 压缩 scratch：一帧的压缩结果写这里（LCD_MIRROR_RLE_SCRATCH_BYTES）。
 * 同样运行期一次性 malloc（刷屏路径上不做动态分配），只由镜像任务写。
 * 没分配成功就永远走原样发 —— 压缩是优化，不是功能前提。 */
static uint8_t *g_rle;

/* 脏行位图：一位一行。g_lock 保护。 */
static uint8_t  g_dirty[(LCD_MIRROR_PANEL_H + 7) / 8];

/* 取脏行的轮转游标（行号，g_lock 保护）：下一次从这一行开始往下找。
 * 为什么需要它见 lm_take_run 的说明（顶部状态栏被反复刷脏会把下面饿死）。 */
static uint16_t g_take_cursor;

/* "请界面整屏重绘一次"请求位，g_lock 保护。 */
static bool     g_redraw_req;

/* g_lock 保护：脏行位图、请求位、下面这组统计量。 */
static mutex_t  g_lock = NXMUTEX_INITIALIZER;

/* 统计量（给 `hw_test lcdmirror status` 看） */
static uint32_t g_stat_frames;     /* 已发出帧数 */
static uint32_t g_stat_bytes;      /* 已发出字节数 */
static uint32_t g_stat_connects;   /* 连上过几次 */
static uint32_t g_stat_drops;      /* 因为连接坏了丢掉的帧数 */
static uint32_t g_stat_capture;    /* 喂进来的脏区个数 */
static uint32_t g_stat_seq;        /* 下一个 seq */
static uint32_t g_stat_rle;        /* 其中走 RLE 的帧数（flags bit0=1） */
static uint32_t g_stat_raw;        /* 其中走原样的帧数 */
static uint32_t g_stat_rle_in;     /* 这些 RLE 帧原样要多少字节（算压缩比用） */
static uint32_t g_stat_rle_out;    /* 这些 RLE 帧实际发了多少字节 */
static uint32_t g_stat_touch;      /* 收到的合法触摸消息数（反向通道） */
static uint32_t g_stat_touch_bad;  /* 反向通道上丢掉的坏字节数 */
static uint32_t g_stat_keyframes;  /* 串口模式下主动发的整屏关键帧数 */
static uint32_t g_stat_uart_busy;  /* 串口写不动（EAGAIN/零字节）而重发的帧数 */

/* ==================== 反向通道：PC 鼠标模拟的触摸 ====================
 * 由镜像任务在**它自己那条 socket** 上收（别人不碰），收到就更新这里；
 * LVGL 侧那个虚拟 input device 的 read_cb 每个周期来这里取当前状态。
 * g_touch_lock 保护全部字段；语义见 lcd_mirror_get_touch() 的注释。 */
static mutex_t  g_touch_lock = NXMUTEX_INITIALIZER;
static uint16_t g_touch_x;
static uint16_t g_touch_y;
static bool     g_touch_pressed;
static bool     g_touch_ever;      /* 收到过合法触摸消息 */
static bool     g_touch_latch;     /* "按下"还没被 read_cb 看见（快速点一下要补一次） */

/* 下面这些只有镜像任务自己和自己人看：running/stop 是给 NSH 与 LVGL 线程
 * 读的，用 volatile 就够（一个字节的写是原子的，最坏晚一轮生效）。 */
static volatile bool g_running;
static volatile bool g_stop;
static volatile bool g_reconnect;   /* 目标地址变了：把当前连接掐掉重连（只有任务自己动 fd） */
static volatile int  g_target_port = LCD_MIRROR_DEFAULT_PORT;

/* 目标 IP。只在任务里读、从 NSH 里改；改的时候先把连接掐掉，
 * 所以不会出现"半个地址"。 */
static char g_target_ip[20] = LCD_MIRROR_DEFAULT_IP;

/* ==================== 传输方式：TCP（默认）/ 串口 ====================
 * 默认 LCD_MIRROR_TRANSPORT_TCP，**开机不会自动切串口**（见文件头）。
 * 由 NSH 侧改，镜像任务只在开连接时读它；换传输等于置 g_reconnect，
 * 任务下一轮自己把旧 fd 关掉、按新的重开（谁开的 fd 谁关）。 */
static volatile int  g_transport = LCD_MIRROR_AUTOSTART_TRANSPORT;

/* 串口节点名。改动时只允许从 NSH 写，任务读它去 open()。 */
static char g_uart_dev[LCD_MIRROR_UART_DEV_MAX] = LCD_MIRROR_DEFAULT_UART_DEV;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void lm_put16(FAR uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void lm_put32(FAR uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
  p[2] = (uint8_t)((v >> 16) & 0xff);
  p[3] = (uint8_t)((v >> 24) & 0xff);
}

/****************************************************************************
 * Name: lm_rle_encode
 *
 * Description:
 *   把一段**行优先、连续**的 RGB565 像素（每像素 2 字节小端）压成协议 v2 的
 *   RLE 流：重复 { u8 run(1..255), u16 pixel_le }，run 数就是像素数，
 *   全部 run 之和正好等于 npx。
 *
 *   limit 是"最多写多少字节"（调用方给原样长度）：**一边写一边判**，写到
 *   out + 3 > limit 立刻放弃返回 0 —— 此时结果必定 >= 原样长度，压了也没用，
 *   直接走原样。这样 dst 只要原样那么大就不会越界，不必按最坏 3 字节/像素预留。
 *
 *   Returned Value:
 *     压缩后的字节数（< limit）；0 = 放弃（写不下 / npx 为 0），调用方走原样。
 *
 ****************************************************************************/

static uint32_t lm_rle_encode(FAR const uint8_t *px, uint32_t npx,
                              uint32_t limit, FAR uint8_t *dst)
{
  uint32_t i   = 0;             /* 已经压过的像素数 */
  uint32_t out = 0;             /* 已经写出的字节数 */

  if (px == NULL || dst == NULL || npx == 0)
    {
      return 0;
    }

  while (i < npx)
    {
      uint32_t off = i * 2;
      uint16_t v   = (uint16_t)((uint16_t)px[off] |
                                ((uint16_t)px[off + 1] << 8));
      uint32_t run = 1;

      /* 同一个像素最多连 255 个（run 只有一个字节），多了就开新的一段 */
      while (run < 255 && i + run < npx)
        {
          uint32_t noff = (i + run) * 2;

          if (px[noff] != px[off] || px[noff + 1] != px[off + 1])
            {
              break;
            }

          run++;
        }

      if (out + 3 > limit)
        {
          return 0;             /* 压不下去了（结果不会比原样小），放弃 */
        }

      dst[out++] = (uint8_t)run;
      dst[out++] = (uint8_t)(v & 0xff);
      dst[out++] = (uint8_t)((v >> 8) & 0xff);

      i += run;
    }

  return out;
}

static void lm_mark_rows(uint16_t y0, uint16_t rows)
{
  uint32_t i;

  nxmutex_lock(&g_lock);
  for (i = 0; i < rows; i++)
    {
      uint32_t y = (uint32_t)y0 + i;

      if (y < LCD_MIRROR_PANEL_H)
        {
          g_dirty[y >> 3] |= (uint8_t)(1u << (y & 7));
        }
    }

  nxmutex_unlock(&g_lock);
}

static void lm_mark_all_dirty(void)
{
  nxmutex_lock(&g_lock);
  memset(g_dirty, 0xff, sizeof(g_dirty));
  nxmutex_unlock(&g_lock);
}

/* 前向声明：lm_take_run 要用它决定"一帧最多几行"（串口比 TCP 收窄得多），
 * 而它的定义在下面。 */

static bool lm_uart_mode(void);

/****************************************************************************
 * Name: lm_take_run
 *
 * Description:
 *   取走一段**连续的脏行**（顺手把位清掉），交给发送侧。一轮最多取一段，
 *   取到就发。返回 0 = 现在没有脏行。
 *
 *   一段最多 LCD_MIRROR_MAX_ROWS_PER_FRAME 行（RLE scratch 就那么大）：再长的
 *   连续脏行**只切一段走**，剩下的行位留在位图里，下一帧接着发。绝不丢行。
 *
 ****************************************************************************/

static int lm_take_run(FAR uint16_t *y0, FAR uint16_t *rows)
{
  int y;
  int y1;
  int first;
  int last;

  nxmutex_lock(&g_lock);

  /* 从**游标**开始往下找，找不到再从头绕回游标之前 —— 这是轮转（round-robin）。
   *
   * 为什么必须轮转（2026-09-17 真机踩到）：原来每次都从第 0 行开始找，而状态栏
   * （时钟/图标）每秒被 LVGL 重刷十几次、永远是最低那一段脏行。串口这条线只有
   * 64 KB/s（TCP 的十分之一），于是它把所有时间都花在重复发顶部三条带子上，
   * 下面的行**永远轮不到**：实测 10 秒里第 120 行以下一次都没发出去（覆盖图
   * [152,172,2100,990,0,0,0,...]），画面上半屏是好的、下半屏一直黑。TCP 因为快，
   * 一个窗口内能把所有脏行清完，所以从来没暴露这个问题。
   *
   * 游标只保证"公平"，不改变"一段最多 MAX_ROWS_PER_FRAME 行、剩下的留位图"的
   * 规矩：所有脏行迟早都会被发到，顺序变成轮转而已。 */

  for (first = -1, y = (int)g_take_cursor; y < LCD_MIRROR_PANEL_H; y++)
    {
      if (g_dirty[y >> 3] & (1u << (y & 7)))
        {
          first = y;
          break;
        }
    }

  if (first < 0)
    {
      for (y = 0; y < (int)g_take_cursor && y < LCD_MIRROR_PANEL_H; y++)
        {
          if (g_dirty[y >> 3] & (1u << (y & 7)))
            {
              first = y;
              break;
            }
        }
    }

  if (first < 0)
    {
      nxmutex_unlock(&g_lock);
      return 0;
    }

  y1 = first;

  while (y1 + 1 < LCD_MIRROR_PANEL_H &&
         (g_dirty[(y1 + 1) >> 3] & (1u << ((y1 + 1) & 7))) != 0)
    {
      y1++;
    }

  /* 一段最多这么多行；多出来的行这次不碰，留在脏位图里下一帧再取。
   * 串口模式下上限更小（见 LCD_MIRROR_UART_MAX_ROWS 的说明：大帧会被 PC 侧
   * 的串口缓冲溢出截断）。 */

  last = first + (lm_uart_mode() ? LCD_MIRROR_UART_MAX_ROWS :
                                   LCD_MIRROR_MAX_ROWS_PER_FRAME) - 1;
  if (y1 > last)
    {
      y1 = last;
    }

  for (y = first; y <= y1; y++)
    {
      g_dirty[y >> 3] &= (uint8_t)~(1u << (y & 7));
    }

  /* 游标指到这一段的下一个行（到尾了就绕回 0），下一次从这儿接着找。 */

  g_take_cursor = (uint16_t)((y1 + 1 >= LCD_MIRROR_PANEL_H) ? 0 : (y1 + 1));

  nxmutex_unlock(&g_lock);

  *y0   = (uint16_t)first;
  *rows = (uint16_t)(y1 - first + 1);
  return 1;
}

/****************************************************************************
 * Name: lm_uart_mode
 *
 * Description:
 *   当前传输是不是串口。一个函数调用而已 —— 但把 g_transport 的比较收敛到
 *   一处，省得哪天判定写歪（串口模式下**绝不能去 recv**，那会偷走 NSH 的
 *   键盘输入）。
 *
 ****************************************************************************/

static bool lm_uart_mode(void)
{
  return g_transport == LCD_MIRROR_TRANSPORT_UART;
}

/****************************************************************************
 * Name: lm_peer_desc
 *
 * Description:
 *   把"现在往哪儿发"写进调用方给的缓冲，给日志用（"TCP 192.168.137.1:5600" /
 *   "串口 /dev/console"）。日志场合不好直接拼两个格式串，所以单独一个函数。
 *
 ****************************************************************************/

static void lm_peer_desc(FAR char *buf, size_t cap)
{
  if (lm_uart_mode())
    {
      snprintf(buf, cap, "串口 %s", g_uart_dev);
    }
  else
    {
      snprintf(buf, cap, "TCP %s:%d", g_target_ip, (int)g_target_port);
    }
}

/****************************************************************************
 * Name: lm_connect
 *
 * Description:
 *   非阻塞 connect + select 超时（写法照 app/robot_ui/net_test.c）。
 *   **返回后 socket 仍然是非阻塞的** —— 发送侧靠这个绝不等待。
 *   成功返回 fd，失败返回 -1。
 *
 ****************************************************************************/

static int lm_connect(void)
{
  struct sockaddr_in addr;
  struct timeval     tv;
  fd_set             wset;
  socklen_t          elen;
  int                fd;
  int                ret;
  int                err;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    {
      return -1;
    }

  if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
    {
      close(fd);
      return -1;
    }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port   = htons((uint16_t)g_target_port);

  if (inet_pton(AF_INET, g_target_ip, &addr.sin_addr) != 1)
    {
      close(fd);
      return -1;
    }

  ret = connect(fd, (FAR struct sockaddr *)&addr, sizeof(addr));
  if (ret < 0 && errno != EINPROGRESS)
    {
      close(fd);
      return -1;
    }

  if (ret < 0)
    {
      FD_ZERO(&wset);
      FD_SET(fd, &wset);
      tv.tv_sec  = LCD_MIRROR_CONNECT_TIMEOUT_MS / 1000;
      tv.tv_usec = (LCD_MIRROR_CONNECT_TIMEOUT_MS % 1000) * 1000;

      ret = select(fd + 1, NULL, &wset, NULL, &tv);
      if (ret <= 0)
        {
          close(fd);
          return -1;
        }

      err  = 0;
      elen = sizeof(err);
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0)
        {
          close(fd);
          return -1;
        }
    }

  /* 小包多、等不了 Nagle：整屏 351 KB 里最后那几十字节要是被攒住，
   * 客户端会以为"这一帧还没收完"，画面停一拍。 */
  {
    int one = 1;

    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  }

  return fd;
}

/****************************************************************************
 * Name: lm_uart_open
 *
 * Description:
 *   打开控制台串口当发送口。**O_WRONLY | O_NONBLOCK**：
 *     - O_WRONLY：只写。**一个字节都不读** —— 这个节点的读方向是 NSH 的
 *       键盘输入，我们读一口就是偷一条命令（策略③就是这么来的）。
 *     - O_NONBLOCK：缓冲满了 write() 立刻返回 EAGAIN / 短写，不会把镜像
 *       任务挂在 uart_write 的信号量上（见策略②）。
 *
 *   成功返回 fd，失败返回 -1（调用方按"打不开"重试）。
 *
 ****************************************************************************/

static int lm_uart_open(void)
{
  return open(g_uart_dev, O_WRONLY | O_NONBLOCK);
}

/****************************************************************************
 * Name: lm_write_bytes
 *
 * Description:
 *   传输无关的一小段写出：TCP 走 send()、串口走 write()。返回值和 errno
 *   的语义两边一致（>=0 已写字节数，<0 错误；缓冲满 = EAGAIN）。
 *
 ****************************************************************************/

static ssize_t lm_write_bytes(int fd, FAR const uint8_t *buf, uint32_t len)
{
  if (lm_uart_mode())
    {
      return write(fd, buf, len);
    }

  return send(fd, buf, len, 0);
}

/****************************************************************************
 * Name: lm_send_frame
 *
 * Description:
 *   把"当前这一帧"尽量推出去。帧 = 20 字节头 + payload_len 字节载荷，
 *   **要么整帧发完，要么一个字节都不多发**（发一半就退出，下轮从 pend_off
 *   接着发），这样客户端永远看到的是完整帧。
 *
 *   limit = 这一次调用最多写多少字节（0xffffffff = 不限）。
 *   TCP 不限长（行为和一个字节都没改以前完全一样）；串口传
 *   LCD_MIRROR_UART_CHUNK，一次只往 TX 缓冲里放一小段 —— 详见文件头"串口
 *   传输"的策略②。分段点落在头里还是载荷里都无所谓，串口本来就是字节流。
 *
 *   载荷由 lm_build_frame 决定指向哪儿：原样帧直接指着影子缓冲（零拷贝，
 *   客户端可能看到撕裂，下一帧自愈）；RLE 帧指着固定的压缩 scratch。
 *   写出的源缓冲**不能在重发同一帧期间被改写**，所以 scratch 只有
 *   "当前帧发完 / 被丢弃"之后才会被下一帧覆盖。
 *
 *   Returned Value:
 *     0  这一帧发完了
 *     1  这一帧还没发完（EAGAIN / 这次写满了 limit），下轮接着发
 *    -1  socket 坏了，调用方应该断开重连
 *
 *   注意 1 有**两种成因**：链路背压（一个字节都没写出去）和"这次配额用完"。
 *   调用方靠"喂进来前后 *off 有没有变"区分（见 lm_task 里的 stall）。
 *
 ****************************************************************************/

static int lm_send_frame(int fd, FAR const uint8_t *hdr,
                         FAR const uint8_t *payload, uint32_t plen,
                         FAR uint32_t *off, uint32_t limit)
{
  uint32_t total = (uint32_t)LCD_MIRROR_FRAME_HDR + plen;
  uint32_t sent  = 0;

  while (*off < total && sent < limit)
    {
      FAR const uint8_t *src;
      uint32_t           remain;
      ssize_t            n;

      if (*off < LCD_MIRROR_FRAME_HDR)
        {
          src    = hdr + *off;
          remain = (uint32_t)LCD_MIRROR_FRAME_HDR - *off;
        }
      else
        {
          src    = payload + (*off - (uint32_t)LCD_MIRROR_FRAME_HDR);
          remain = total - *off;
        }

      /* 不超过这次调用允许写的字节数（串口那个 256 字节的上限就是这里生效的） */
      if (remain > limit - sent)
        {
          remain = limit - sent;
        }

      n = lm_write_bytes(fd, src, remain);
      if (n < 0)
        {
          if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ||
              errno == ENOMEM)
            {
              return 1;
            }

          return -1;
        }

      if (n == 0)
        {
          return 1;
        }

      *off += (uint32_t)n;
      sent += (uint32_t)n;
    }

  return *off < total ? 1 : 0;
}

/* 任务私有的"正在发的那一帧"状态。只有镜像任务碰。 */
static uint8_t        g_pend_hdr[LCD_MIRROR_FRAME_HDR];
static FAR const uint8_t *g_pend_payload;
static uint32_t       g_pend_plen;
static uint32_t       g_pend_off;
static uint16_t       g_pend_y0;      /* 只为了发送失败时把这几行重新标脏 */
static uint16_t       g_pend_rows;
static bool           g_pend_valid;

/****************************************************************************
 * Name: lm_build_frame
 *
 * Description:
 *   给 [y0, y0+rows) 这几行组一帧：填 20 字节头（v2），再决定载荷。
 *
 *   载荷先试着压：RLE 结果严格小于原样才用 RLE（flags bit0=1），否则退回
 *   原样（flags=0，载荷直接指影子缓冲，零拷贝）。所以压缩永远不会让这一帧
 *   变大，最多白花一次编码的时间（有界，写满原样长度立刻放弃）。
 *
 *   顺带把统计量记上（帧数 / 字节数 / RLE 帧数与省下的字节），status 里要显示。
 *
 ****************************************************************************/

static void lm_build_frame(uint16_t y0, uint16_t rows)
{
  FAR const uint8_t *src = g_shadow + (size_t)y0 * LCD_MIRROR_ROW_BYTES;
  uint32_t           raw = (uint32_t)rows * LCD_MIRROR_ROW_BYTES;
  uint32_t           npx = (uint32_t)rows * LCD_MIRROR_PANEL_W;
  uint32_t           plen;
  uint8_t            flags = 0;

  plen = 0;
  if (g_rle != NULL)
    {
      plen = lm_rle_encode(src, npx, raw, g_rle);
    }

  if (plen > 0 && plen < raw)
    {
      flags = LCD_MIRROR_FLAG_RLE;
      g_pend_payload = g_rle;

      nxmutex_lock(&g_lock);
      g_stat_rle++;
      g_stat_rle_in  += raw;
      g_stat_rle_out += plen;
      nxmutex_unlock(&g_lock);
    }
  else
    {
      plen = raw;
      g_pend_payload = src;

      nxmutex_lock(&g_lock);
      g_stat_raw++;
      nxmutex_unlock(&g_lock);
    }

  g_pend_plen  = plen;
  g_pend_y0    = y0;
  g_pend_rows  = rows;

  g_pend_hdr[0] = LCD_MIRROR_MAGIC0;
  g_pend_hdr[1] = LCD_MIRROR_MAGIC1;
  g_pend_hdr[2] = LCD_MIRROR_VER;
  g_pend_hdr[3] = flags;
  lm_put16(&g_pend_hdr[4],  0);                    /* x */
  lm_put16(&g_pend_hdr[6],  y0);                   /* y */
  lm_put16(&g_pend_hdr[8],  LCD_MIRROR_PANEL_W);   /* w */
  lm_put16(&g_pend_hdr[10], rows);                 /* h */

  nxmutex_lock(&g_lock);
  lm_put32(&g_pend_hdr[12], g_stat_seq);           /* seq */
  lm_put32(&g_pend_hdr[16], plen);                 /* payload_len */
  g_stat_seq++;
  nxmutex_unlock(&g_lock);

  g_pend_off   = 0;
  g_pend_valid = true;
}

/* 反向通道的收报缓冲。也**只有镜像任务碰**（谁 open 的 fd 谁用，
 * 收报的中间状态自然也只属于它），所以不加锁。 */
struct lm_rx_s
{
  uint8_t  buf[LCD_MIRROR_TOUCH_MSG];
  uint32_t len;                 /* 已经攒了多少字节（永远 < 8，见 lm_rx_touch） */
};

/****************************************************************************
 * Name: lm_touch_release
 *
 * Description:
 *   强制"抬起"（并把"按下锁存"清掉）。**任何让"电脑那头可能不在发"的时刻
 *   都要调**：连上、断开、停止。不这么做的话，鼠标在拖动中间（按下状态还没抬
 *   起）关掉窗口或者拔网线，板子就会永远停在"按着不放"，界面从此卡在一个
 *   按下状态、点什么都没反应。
 *
 *   只清"活着的状态"，**保留最后的坐标和"收到过消息"**——status 里那两行
 *   要拿来显示"最后坐标是哪儿"，诊断时很有用；被清掉的话现场只能看到
 *   "还没收到过事件"，反而看不出鼠标到底通没通。
 *
 ****************************************************************************/

static void lm_touch_release(void)
{
  nxmutex_lock(&g_touch_lock);
  g_touch_pressed = false;
  g_touch_latch   = false;
  nxmutex_unlock(&g_touch_lock);
}

/* 反向通道上丢掉一个不认识/对不上的字节。只计数，不断连接。 */
static void lm_touch_bad_byte(void)
{
  nxmutex_lock(&g_touch_lock);
  g_stat_touch_bad++;
  nxmutex_unlock(&g_touch_lock);
}

/****************************************************************************
 * Name: lm_touch_store
 *
 * Description:
 *   收全一条触摸消息，记下最新状态。
 *   置 g_touch_latch 让"按下"至少被 read_cb 看见一次（快速点一下不丢）。
 *
 ****************************************************************************/

static void lm_touch_store(uint16_t x, uint16_t y, bool pressed)
{
  if (x >= LCD_MIRROR_PANEL_W)
    {
      x = LCD_MIRROR_PANEL_W - 1;
    }

  if (y >= LCD_MIRROR_PANEL_H)
    {
      y = LCD_MIRROR_PANEL_H - 1;
    }

  nxmutex_lock(&g_touch_lock);
  g_touch_x       = x;
  g_touch_y       = y;
  g_touch_pressed = pressed;
  g_touch_ever    = true;

  if (pressed)
    {
      g_touch_latch = true;
    }

  g_stat_touch++;
  nxmutex_unlock(&g_touch_lock);
}

/****************************************************************************
 * Name: lm_rx_touch
 *
 * Description:
 *   反向通道：非阻塞收一条 8 字节触摸消息（PC -> 板）。
 *
 *   **坏字节绝不会断连接**（帧还要继续发）：把缓冲当成一条字节流，
 *   一个字节一个字节地找 'L''T'，找不到就丢掉那一个字节继续找。
 *   TCP 会把 8 字节拆成几片送来，所以必须能跨次调用续着攒。
 *
 *   Returned Value:
 *     0   正常（收到一条 / 收到半条 / 这次什么也没收到）
 *    -1   socket 真的坏了，调用方该断开重连
 *    -2   对端关了（EOF / 读回 0），调用方也该断开重连 —— 和 -1 分开是为了
 *         日志别把上一次遗留的 errno 当成这次的错因（EAGAIN 打印出来很误导）
 *
 ****************************************************************************/

static int lm_rx_touch(int fd, FAR struct lm_rx_s *rx)
{
  ssize_t n;

  n = recv(fd, rx->buf + rx->len, LCD_MIRROR_TOUCH_MSG - rx->len, 0);
  if (n < 0)
    {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ||
          errno == ENOMEM)
        {
          return 0;
        }

      return -1;
    }

  if (n == 0)
    {
      return -2;                /* 对端关了 */
    }

  rx->len += (uint32_t)n;

  for (;;)
    {
      while (rx->len > 0 && rx->buf[0] != LCD_MIRROR_TOUCH_MAGIC0)
        {
          memmove(rx->buf, rx->buf + 1, --rx->len);
          lm_touch_bad_byte();
        }

      if (rx->len < 2)
        {
          return 0;
        }

      if (rx->buf[1] != LCD_MIRROR_TOUCH_MAGIC1)
        {
          memmove(rx->buf, rx->buf + 1, --rx->len);
          lm_touch_bad_byte();
          continue;
        }

      if (rx->len < LCD_MIRROR_TOUCH_MSG)
        {
          return 0;             /* 还没攒够 8 字节，等下一轮 */
        }

      if (rx->buf[2] != LCD_MIRROR_TOUCH_VER)
        {
          memmove(rx->buf, rx->buf + 1, --rx->len);
          lm_touch_bad_byte();
          continue;
        }

      lm_touch_store((uint16_t)((uint16_t)rx->buf[4] |
                                ((uint16_t)rx->buf[5] << 8)),
                     (uint16_t)((uint16_t)rx->buf[6] |
                                ((uint16_t)rx->buf[7] << 8)),
                     rx->buf[3] != 0);

      rx->len = 0;
      return 0;
    }
}

/****************************************************************************
 * Name: lm_disconnect
 *
 * Description:
 *   把连接收掉。**必须由镜像任务自己调**（fd 是它开的，也只有它能关）。
 *   顺手把注入的触摸强制抬起来 —— 断开时对面多半还在"按着"。
 *   err 传 0 表示"这次不是 errno 类错误"（比如对端正常关闭），日志里就不打 errno。
 *
 *   两种传输都走这里：TCP 是"断开重连"，串口是"关掉重开"。日志分开写，
 *   现场看一行就知道挂在哪儿。
 *
 ****************************************************************************/

static void lm_disconnect(FAR int *fd, FAR struct lm_rx_s *rx,
                          FAR const char *why, int err)
{
  if (lm_uart_mode())
    {
      if (err != 0)
        {
          syslog(LOG_WARNING, "LCDMIRROR: %s（串口 %s，errno=%d），关掉重开\n",
                 why, g_uart_dev, err);
        }
      else
        {
          syslog(LOG_WARNING, "LCDMIRROR: %s（串口 %s），关掉重开\n",
                 why, g_uart_dev);
        }
    }
  else if (err != 0)
    {
      syslog(LOG_WARNING, "LCDMIRROR: %s（errno=%d），断开重连\n", why, err);
    }
  else
    {
      syslog(LOG_WARNING, "LCDMIRROR: %s，断开重连\n", why);
    }

  if (*fd >= 0)
    {
      close(*fd);
      *fd = -1;
    }

  rx->len = 0;
  lm_touch_release();
}

/****************************************************************************
 * Name: lm_task
 *
 * Description:
 *   镜像任务主体。自己建连接（socket 或串口）、自己用、自己收尸 ——
 *   不碰别人的 fd。串口模式下**一个字节都不读**（读方向是 NSH 的输入）。
 *
 ****************************************************************************/

static int lm_task(int argc, FAR char *argv[])
{
  struct lm_rx_s rx;
  int      fd        = -1;
  uint32_t fail_cnt  = 0;
  uint32_t win_start = clock_systime_ticks();
  uint32_t win_bytes = 0;
  uint32_t win_frames = 0;
  uint32_t kf_start  = clock_systime_ticks();   /* 串口关键帧计时 */

  (void)argc;
  (void)argv;

  memset(&rx, 0, sizeof(rx));
  lm_touch_release();

  {
    char peer[48];

    lm_peer_desc(peer, sizeof(peer));
    syslog(LOG_INFO, "LCDMIRROR: 任务已起（prio %d，栈 %d），%s，"
           "影子缓冲 %d 字节，协议 ver%d（RLE scratch %s），窗口 %d ms / %d 字节\n",
           LCD_MIRROR_TASK_PRIORITY, LCD_MIRROR_TASK_STACK,
           peer, (int)LCD_MIRROR_FB_BYTES,
           LCD_MIRROR_VER, g_rle ? "有" : "无（退回原样）",
           LCD_MIRROR_TICK_MS, LCD_MIRROR_MAX_BYTES_PER_TICK);
  }

  while (!g_stop)
    {
      /* ---- 传输/目标地址被改了：自己把连接掐掉重来（fd 只能由本任务关） ---- */

      if (g_reconnect)
        {
          g_reconnect = false;
          if (fd >= 0)
            {
              close(fd);
              fd = -1;
              rx.len = 0;
              lm_touch_release();

              {
                char peer[48];

                lm_peer_desc(peer, sizeof(peer));
                syslog(LOG_INFO, "LCDMIRROR: 传输已改为 %s，重开\n", peer);
              }
            }

          g_pend_valid = false;
        }

      /* ---- 没连上：连（TCP）或开串口，不行就等一会儿再来 ---- */

      if (fd < 0)
        {
          char peer[48];

          fd = lm_uart_mode() ? lm_uart_open() : lm_connect();
          if (fd < 0)
            {
              fail_cnt++;
              if (fail_cnt == 1 || (fail_cnt % LCD_MIRROR_LOG_EVERY) == 0)
                {
                  if (lm_uart_mode())
                    {
                      syslog(LOG_WARNING, "LCDMIRROR: 打不开串口 %s（第 %u 次，"
                             "errno=%d），%d ms 后重试\n", g_uart_dev,
                             (unsigned)fail_cnt, errno, LCD_MIRROR_RETRY_MS);
                    }
                  else
                    {
                      syslog(LOG_WARNING, "LCDMIRROR: 连不上 %s:%d（第 %u 次），"
                             "%d ms 后重试\n", g_target_ip, (int)g_target_port,
                             (unsigned)fail_cnt, LCD_MIRROR_RETRY_MS);
                    }
                }

              g_pend_valid = false;

              /* 分段睡：stop 的时候能立刻退，不用干等满 2.5 秒
               * （不然 `hw_test lcdmirror stop` 会报"任务还没退完"，
               * 现场看着像没停干净）。g_reconnect 也当成"醒一下"的理由：
               * 网络连不上正等着重试的当口切串口（现场最常见的动作），
               * 不该再白等 2.5 秒。 */
              {
                int left = LCD_MIRROR_RETRY_MS;

                while (left > 0 && !g_stop && !g_reconnect)
                  {
                    usleep(100 * 1000);
                    left -= 100;
                  }
              }

              continue;
            }

          fail_cnt     = 0;
          g_pend_valid = false;
          win_start    = clock_systime_ticks();
          win_bytes    = 0;
          win_frames   = 0;
          kf_start     = clock_systime_ticks();
          rx.len       = 0;

          nxmutex_lock(&g_lock);
          g_stat_connects++;
          nxmutex_unlock(&g_lock);

          /* 新客户端/新开的串口：整屏重发一次，别让它从"只有后来那块脏区"
           * 开始拼。触摸也强制抬起：对面还没发过任何东西。 */
          lm_mark_all_dirty();
          lm_touch_release();

          lm_peer_desc(peer, sizeof(peer));
          syslog(LOG_INFO, "LCDMIRROR: 已连上 %s，开始推屏\n", peer);
        }

      /* ---- 反向通道：收 PC 发来的鼠标模拟触摸（非阻塞，收不到就算了） ----
       * 和发帧在同一个循环里、同一个任务里：不加任务、不阻塞发送。
       * 放在发送之前，鼠标点下去的本轮就能进到全局状态，少等一个窗口。
       * **串口模式下这一整段跳过**：这个 fd 是 O_WRONLY 打开的，而且它的
       * 读方向是 NSH 的键盘输入 —— 读一口就是偷一条命令。串口模式的触摸
       * 入口是 nsh 命令 `hw_test lcdtap <x> <y> <0|1>`（策略③）。 */

      if (!lm_uart_mode())
        {
          int rrc = lm_rx_touch(fd, &rx);

          if (rrc != 0)
            {
              lm_disconnect(&fd, &rx,
                            rrc == -2 ? "反向通道对端关闭" : "反向通道读失败",
                            rrc == -2 ? 0 : errno);
              g_pend_valid = false;
              continue;
            }
        }

      /* ---- 串口：每 LCD_MIRROR_UART_KEYFRAME_MS 发一次整屏关键帧（策略①）----
       * 串口线上日志字节会插进帧里，那些帧 PC 侧只能丢掉；不给一张定期从
       * 头到脚的整屏，画面就会一直缺那一块。标脏只是置位（微秒级），真正
       * 发多少还是由字节预算管着。 */

      if (lm_uart_mode() &&
          TICK2MSEC(clock_systime_ticks() - kf_start) >=
          LCD_MIRROR_UART_KEYFRAME_MS)
        {
          kf_start = clock_systime_ticks();
          lm_mark_all_dirty();

          nxmutex_lock(&g_lock);
          g_stat_keyframes++;
          nxmutex_unlock(&g_lock);
        }

      /* ---- 新窗口：重置字节/帧预算 ---- */

      if (TICK2MSEC(clock_systime_ticks() - win_start) >= LCD_MIRROR_TICK_MS)
        {
          win_start  = clock_systime_ticks();
          win_bytes  = 0;
          win_frames = 0;
        }

      /* ---- 本轮发送 ---- */

      {
        const bool uart = lm_uart_mode();

        /* 串口那条腿不用 TCP 的"窗口 + 字节预算"：它的限速就是下面那个
         * "一轮最多写 LCD_MIRROR_UART_CHUNK 字节、写完睡 5ms"（= 64 KB/s），
         * 一次丢太多只会换来短写（TX 环形缓冲才 1024 字节）。
         * TCP 的窗口/预算一个数都没改。 */
        uint32_t iter_out = 0;    /* 本轮这一小段已经写出去多少字节 */
        int      brk      = 0;

        while (!brk && win_frames < LCD_MIRROR_MAX_RECTS_PER_TICK &&
               (uart || win_bytes < LCD_MIRROR_MAX_BYTES_PER_TICK))
          {
            uint32_t limit;
            uint32_t before;
            bool     stall;
            int      rc;

            if (!g_pend_valid)
              {
                uint16_t y0;
                uint16_t rows;

                if (!lm_take_run(&y0, &rows))
                  {
                    break;      /* 没脏行，收工 */
                  }

                /* 组帧：填 v2 头 + 决定载荷（RLE 还是原样） */
                lm_build_frame(y0, rows);
              }

            /* 串口这一轮只再写这么多字节（写完就出去睡一下，见循环末尾的
             * 5ms）。TCP 不限长：一次把整帧写完，行为和一个字节都没改以前
             * 完全一样。 */
            limit = uart ? (LCD_MIRROR_UART_CHUNK - iter_out) : 0xffffffffu;
            if (limit == 0)
              {
                brk = 1;
                break;
              }

            before = g_pend_off;
            rc     = lm_send_frame(fd, g_pend_hdr, g_pend_payload, g_pend_plen,
                                   &g_pend_off, limit);

            /* 一个字节都没写出去 = 真写不动（EAGAIN / 写回 0）。这一条
             * 对 TCP 没有额外语义（那边本来就把没发完的帧留着），串口拿它
             * 触发策略②。 */
            stall = (rc == 1 && g_pend_off == before);
            iter_out += g_pend_off - before;

            if (rc < 0)
              {
                /* 连接坏了：这一帧没发完，把它的行重新标脏，
                 * 重连之后整屏本来也会重发，但标脏能让客户端早点对上。 */
                lm_mark_rows(g_pend_y0, g_pend_rows);
                g_pend_valid = false;

                nxmutex_lock(&g_lock);
                g_stat_drops++;
                nxmutex_unlock(&g_lock);

                /* lm_disconnect 里会 close+置 -1（fd 是本任务开的，也只能本任务关），
                 * 顺便把注入的触摸强制抬起 —— 对面多半还在"按着"。 */
                lm_disconnect(&fd, &rx, "发送失败", errno);
                break;
              }

            /* ---- 策略②：串口写不动就地重发，绝不等待 ----
             * 缓冲满了只说明"我们比这条线跑得快"（或者日志正把口占住）。
             * 在这一帧上接着等，会把镜像任务和刷屏一起拖住；所以**放弃这一帧、
             * 把这几行重新标脏**，下一轮从头再发（含每 3 秒一次的关键帧兜底）。
             * PC 侧收到半截帧就当坏帧重同步丢掉，本来就是这么设计的。 */
            if (stall && uart)
              {
                lm_mark_rows(g_pend_y0, g_pend_rows);
                g_pend_valid = false;

                nxmutex_lock(&g_lock);
                g_stat_uart_busy++;
                nxmutex_unlock(&g_lock);

                brk = 1;
                break;
              }

            if (rc == 1)
              {
                brk = 1;        /* 没发完（配额用完 / 缓冲满），退出本轮，短睡后再来 */
                break;
              }

            /* 整帧发完：字节数按**实际发出去的载荷**算（RLE 之后就是压缩后的
             * 长度），预算才不会虚账。 */
            win_bytes += (uint32_t)LCD_MIRROR_FRAME_HDR + g_pend_plen;
            win_frames++;
            g_pend_valid = false;

            nxmutex_lock(&g_lock);
            g_stat_frames++;
            g_stat_bytes += (uint32_t)LCD_MIRROR_FRAME_HDR + g_pend_plen;
            nxmutex_unlock(&g_lock);
          }
      }

      /* ---- 睡一会儿 ---- */

      if (g_pend_valid)
        {
          /* 上一帧还没发完，快点接着发。串口比 TCP 更需要"勤着推"：一次只写
           * LCD_MIRROR_UART_CHUNK 字节，这条腿的节奏全靠这个睡眠定
           * （320B / 5ms ≈ 64 KB/s）。 */
          usleep((lm_uart_mode() ? LCD_MIRROR_UART_RETRY_SEND_MS
                                 : LCD_MIRROR_RETRY_SEND_MS) * 1000);
        }
      else
        {
          /* 空闲也睡得很短：反向的触摸消息要在这个循环里收，
           * 睡久了鼠标点下去就迟钝。发送速率由上面的窗口+字节预算管着，
           * 跟这个睡眠时长无关。 */
          usleep(LCD_MIRROR_POLL_MS * 1000);
        }
    }

  if (fd >= 0)
    {
      close(fd);
    }

  g_pend_valid = false;
  rx.len       = 0;
  lm_touch_release();          /* 退出后绝不留"按着不放" */
  g_running    = false;

  syslog(LOG_INFO, "LCDMIRROR: 任务已退出（发过 %u 帧）\n",
         (unsigned)g_stat_frames);
  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int lcd_mirror_start(void)
{
  int pid;

  if (g_running)
    {
      return OK;
    }

  if (g_shadow == NULL)
    {
      g_shadow = (FAR uint8_t *)malloc(LCD_MIRROR_FB_BYTES);
      if (g_shadow == NULL)
        {
          syslog(LOG_ERR, "LCDMIRROR: 影子缓冲 %d 字节分配失败\n",
                 (int)LCD_MIRROR_FB_BYTES);
          return -ENOMEM;
        }

      memset(g_shadow, 0, LCD_MIRROR_FB_BYTES);
      syslog(LOG_INFO, "LCDMIRROR: 影子缓冲已就位 %p（%d 字节，堆上）\n",
             g_shadow, (int)LCD_MIRROR_FB_BYTES);
    }

  /* RLE 压缩 scratch：一次性 malloc，之后每帧复用（刷屏路径上不做动态分配）。
   * **分配失败不是错误**：g_rle == NULL 时每帧都走原样发，功能一点不少，
   * 只是链路费一点 —— 所以这里只提示，绝不阻止镜像起来。 */
  if (g_rle == NULL)
    {
      g_rle = (FAR uint8_t *)malloc(LCD_MIRROR_RLE_SCRATCH_BYTES);
      if (g_rle == NULL)
        {
          syslog(LOG_WARNING, "LCDMIRROR: RLE scratch %d 字节分配失败，"
                 "本机退回原样发（能用，就是费流量）\n",
                 (int)LCD_MIRROR_RLE_SCRATCH_BYTES);
        }
      else
        {
          syslog(LOG_INFO, "LCDMIRROR: RLE scratch 已就位 %p（%d 字节 = %d 行）\n",
                 g_rle, (int)LCD_MIRROR_RLE_SCRATCH_BYTES,
                 LCD_MIRROR_MAX_ROWS_PER_FRAME);
        }
    }

  g_stop       = false;
  g_reconnect  = false;
  g_pend_valid = false;

  /* 上一次跑剩下的"按着"不带过来（坐标留着，status 里要显示最后位置） */
  lm_touch_release();

  /* 整屏标脏 + 请界面整屏重绘一次：影子缓冲在界面起来之前是空的，
   * 不重绘一次，客户端上就永远缺那块。 */
  lcd_mirror_request_full_redraw();

  g_running = true;
  pid = task_create(LCD_MIRROR_TASK_NAME, LCD_MIRROR_TASK_PRIORITY,
                    LCD_MIRROR_TASK_STACK, (main_t)lm_task, NULL);
  if (pid < 0)
    {
      g_running = false;
      syslog(LOG_ERR, "LCDMIRROR: task_create failed: %d\n", pid);
      return pid;
    }

  return OK;
}

int lcd_mirror_stop(void)
{
  int waited = 0;

  if (!g_running)
    {
      return OK;
    }

  g_stop = true;
  lm_touch_release();          /* 立刻不再注入触摸；任务退出时还会再清一次 */

  /* 等它自己退（不跨任务 delete）：最多 1 秒。任务最长一轮睡 2.5 秒
   * （重连等待），所以这里等不到就返回，让它自己慢慢退。 */
  while (g_running && waited < LCD_MIRROR_STOP_WAIT_MS)
    {
      usleep(20 * 1000);
      waited += 20;
    }

  if (g_running)
    {
      syslog(LOG_WARNING, "LCDMIRROR: 停止请求已发，任务还没退完（等了 %d ms）\n",
             waited);
      return -EAGAIN;
    }

  syslog(LOG_INFO, "LCDMIRROR: 已停止\n");
  return OK;
}

bool lcd_mirror_is_running(void)
{
  return g_running;
}

int lcd_mirror_set_target(const char *ip, uint16_t port)
{
  if (ip != NULL && ip[0] != '\0')
    {
      if (strlen(ip) >= sizeof(g_target_ip))
        {
          return -EINVAL;
        }

      strncpy(g_target_ip, ip, sizeof(g_target_ip) - 1);
      g_target_ip[sizeof(g_target_ip) - 1] = '\0';
    }

  if (port != 0)
    {
      g_target_port = port;
    }

  /* 改地址就得把现在的连接掐掉，不然要等到下一次断开才生效。
   * **不直接 close(fd)** —— 铁律：fd 是开它的那个 task group 的，
   * 只有镜像任务自己能关。这里只置个标志，任务下一轮自己收尸重连。 */
  g_reconnect = true;

  if (g_running)
    {
      syslog(LOG_INFO, "LCDMIRROR: 目标改为 %s:%d\n",
             g_target_ip, (int)g_target_port);
    }
  else
    {
      /* 没在跑就顺带把影子缓冲标满，起来时第一帧就是整屏 */
      lcd_mirror_request_full_redraw();
    }

  return OK;
}

const char *lcd_mirror_target_ip(void)
{
  return g_target_ip;
}

uint16_t lcd_mirror_target_port(void)
{
  return (uint16_t)g_target_port;
}

/* ==================== 传输方式（默认 TCP，串口手动开） ====================
 * 两个 switch 都只置标志 + 让任务重开连接，见 lcd_mirror.h 的注释。 */

int lcd_mirror_use_uart(const char *dev)
{
  if (dev != NULL && dev[0] != '\0')
    {
      if (strlen(dev) >= sizeof(g_uart_dev))
        {
          return -EINVAL;
        }

      strncpy(g_uart_dev, dev, sizeof(g_uart_dev) - 1);
      g_uart_dev[sizeof(g_uart_dev) - 1] = '\0';
    }

  g_transport = LCD_MIRROR_TRANSPORT_UART;

  /* 只置标志：当前这条连接由镜像任务自己 close（fd 是它开的，铁律）。
   * 任务下一轮就会按新传输重开。 */
  g_reconnect = true;

  if (g_running)
    {
      syslog(LOG_INFO, "LCDMIRROR: 传输改为串口 %s（手动开，"
             "`hw_test lcdmirror tcp <ip>` 或 `stop` 随时能停）\n", g_uart_dev);
    }
  else
    {
      /* 没在跑就顺带把影子缓冲标满，起第一帧就是整屏 */
      lcd_mirror_request_full_redraw();
    }

  return OK;
}

int lcd_mirror_use_tcp(const char *ip, uint16_t port)
{
  /* 先把传输改回 TCP，再走 set_target() 那一套（它负责改地址 + 置重连标志 +
   * 没在跑时标脏整屏）。 */
  g_transport = LCD_MIRROR_TRANSPORT_TCP;
  return lcd_mirror_set_target(ip, port);
}

bool lcd_mirror_is_uart(void)
{
  return g_transport == LCD_MIRROR_TRANSPORT_UART;
}

const char *lcd_mirror_uart_dev(void)
{
  return g_uart_dev;
}

void lcd_mirror_inject_touch(uint16_t x, uint16_t y, bool pressed)
{
  /* 和反向通道收到 'L''T' 消息走的是同一条路（越界钳位、按下锁存都在里面）。 */
  lm_touch_store(x, y, pressed);
}

void lcd_mirror_resend_rows(uint16_t y0, uint16_t rows)
{
  uint32_t end;

  /* 镜像没跑就什么都不做：标脏没意义（没人发），而且下一次 start 本来就会
   * 把整屏标满。 */
  if (!g_running || rows == 0u || y0 >= LCD_MIRROR_PANEL_H)
    {
      return;
    }

  end = (uint32_t)y0 + (uint32_t)rows;

  if (end > LCD_MIRROR_PANEL_H)
    {
      rows = (uint16_t)(LCD_MIRROR_PANEL_H - y0);
    }

  lm_mark_rows(y0, rows);
}

void lcd_mirror_resend_all(void)
{
  if (!g_running)
    {
      return;
    }

  lm_mark_all_dirty();
}

void lcd_mirror_status(void)
{
  uint32_t frames;
  uint32_t bytes;
  uint32_t connects;
  uint32_t drops;
  uint32_t capture;
  uint32_t rle;
  uint32_t raw;
  uint32_t rle_in;
  uint32_t rle_out;
  uint32_t touch;
  uint32_t touch_bad;
  uint32_t keyframes;
  uint32_t uart_busy;
  uint16_t tx;
  uint16_t ty;
  bool     tpressed;
  bool     tever;

  nxmutex_lock(&g_lock);
  frames     = g_stat_frames;
  bytes      = g_stat_bytes;
  connects   = g_stat_connects;
  drops      = g_stat_drops;
  capture    = g_stat_capture;
  rle        = g_stat_rle;
  raw        = g_stat_raw;
  rle_in     = g_stat_rle_in;
  rle_out    = g_stat_rle_out;
  keyframes  = g_stat_keyframes;
  uart_busy  = g_stat_uart_busy;
  nxmutex_unlock(&g_lock);

  nxmutex_lock(&g_touch_lock);
  tx        = g_touch_x;
  ty        = g_touch_y;
  tpressed  = g_touch_pressed;
  tever     = g_touch_ever;
  touch     = g_stat_touch;
  touch_bad = g_stat_touch_bad;
  nxmutex_unlock(&g_touch_lock);

  printf("lcdmirror: %s\n", g_running ? "运行中" : "已停止");

  /* 当前传输 + 往哪儿发：串口看节点名，TCP 看目标地址（要求明确显示这两个）。 */
  if (lcd_mirror_is_uart())
    {
      printf("  传输        串口 %s（手动开的；`hw_test lcdmirror tcp <ip> [port]` 切回）\n",
             g_uart_dev);
      printf("  串口策略    每 %d ms 一次整屏关键帧（已发 %u 次）；"
             "一次写 %d 字节 / 睡 %d ms（约 64 KB/s），"
             "写不动就地重发（已重发 %u 帧）\n",
             LCD_MIRROR_UART_KEYFRAME_MS, (unsigned)keyframes,
             LCD_MIRROR_UART_CHUNK, LCD_MIRROR_UART_RETRY_SEND_MS,
             (unsigned)uart_busy);
      printf("  反向触摸    `hw_test lcdtap <x> <y> <0|1>`（文本命令，"
             "不走帧的字节流）\n");
    }
  else
    {
      printf("  传输        TCP（默认；`hw_test lcdmirror uart [dev]` 才切串口）\n");
    }

  printf("  目标        %s:%d（默认 %s:%d）\n",
         g_target_ip, (int)g_target_port,
         LCD_MIRROR_DEFAULT_IP, LCD_MIRROR_DEFAULT_PORT);
  printf("  影子缓冲    %s，%d 字节（%d x %d x 2）\n",
         g_shadow ? "已分配" : "未分配", (int)LCD_MIRROR_FB_BYTES,
         LCD_MIRROR_PANEL_W, LCD_MIRROR_PANEL_H);
  printf("  RLE scratch %s，%d 字节（一帧最多 %d 行，%s）\n",
         g_rle ? "已分配" : "未分配", (int)LCD_MIRROR_RLE_SCRATCH_BYTES,
         LCD_MIRROR_MAX_ROWS_PER_FRAME,
         g_rle ? "压缩在跑" : "**压缩没起**，退回原样发");
  printf("  喂进来的脏区 %u 个，已发 %u 帧 / %u 字节，连上过 %u 次，丢过 %u 帧\n",
         (unsigned)capture, (unsigned)frames, (unsigned)bytes,
         (unsigned)connects, (unsigned)drops);
  printf("  压缩        RLE %u 帧 / 原样 %u 帧", (unsigned)rle, (unsigned)raw);
  if (rle > 0 && rle_out > 0)
    {
      /* 这些 RLE 帧：原样本来要 rle_in 字节，实际载荷 rle_out 字节 */
      printf("，这些帧 %u -> %u 字节（压掉 %u%%）\n",
             (unsigned)rle_in, (unsigned)rle_out,
             (unsigned)(100u - (rle_out * 100u) / rle_in));
    }
  else
    {
      printf("（一帧都还没压过）\n");
    }
  printf("  鼠标触摸    收到 %u 条（坏字节丢 %u 个）",
         (unsigned)touch, (unsigned)touch_bad);
  if (tever)
    {
      printf("，最后坐标 (%u, %u)，当前 %s\n",
             (unsigned)tx, (unsigned)ty, tpressed ? "按下" : "抬起");
    }
  else if (lcd_mirror_is_uart())
    {
      printf("，**一条都没注入过**（串口模式下 PC 要发 "
             "`hw_test lcdtap <x> <y> <0|1>`，不是发 'L''T' 二进制包）\n");
    }
  else
    {
      printf("，**一条都没收到过**（PC 端 lcd_mirror.py 连上了没？"
             "鼠标要在画布上按住）\n");
    }

  printf("  协议        'L''M' ver2 + x/y/w/h + seq + payload_len（20 字节头），"
         "flags bit0=1 时载荷是 RLE（{u8 run, u16 像素}）；"
         "TCP 反向 'L''T' ver1 + pressed + x/y，8 字节\n");
  if (lcd_mirror_is_uart())
    {
      printf("  客户端      同一个 _flash/lcd_mirror.py（PC 从串口读同一套帧；"
             "触摸改成往串口写 `hw_test lcdtap ...` 文本）\n");
    }
  else
    {
      printf("  客户端      _flash/lcd_mirror.py（画布上按住鼠标 = 模拟触摸）\n");
    }
}

bool lcd_mirror_get_touch(uint16_t *x, uint16_t *y, bool *pressed)
{
  bool     got;
  bool     pr     = false;
  uint16_t tx     = 0;
  uint16_t ty     = 0;

  nxmutex_lock(&g_touch_lock);

  /* 镜像没在跑就一定不给：这是"跟镜像走"的开关，也是最后一道保险 ——
   * 停止/断开时触摸已经被清成抬起，这里再挡一次，界面永远不会卡在按下。 */
  got = g_running && g_touch_ever;
  if (got)
    {
      tx = g_touch_x;
      ty = g_touch_y;

      if (g_touch_latch)
        {
          /* "按下"还没被看见过：先补一次，下一次读才给真实状态。
           * 鼠标快速点一下（按下和抬起落在同一个读取周期里）靠这个不丢。 */
          g_touch_latch = false;
          pr = true;
        }
      else
        {
          pr = g_touch_pressed;
        }
    }

  nxmutex_unlock(&g_touch_lock);

  if (got)
    {
      if (x != NULL)
        {
          *x = tx;
        }

      if (y != NULL)
        {
          *y = ty;
        }

      if (pressed != NULL)
        {
          *pressed = pr;
        }
    }

  return got;
}

void lcd_mirror_capture(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                        const uint8_t *px, uint32_t stride)
{
  uint32_t row;
  uint32_t src_row = 0;

  /* 关了镜像就完全绕开：刷屏路径只多读一次这个 bool。 */
  if (!g_running || g_shadow == NULL)
    {
      return;
    }

  if (px == NULL || w == 0 || h == 0)
    {
      return;
    }

  if (x >= LCD_MIRROR_PANEL_W || y >= LCD_MIRROR_PANEL_H)
    {
      return;
    }

  if ((uint32_t)x + w > LCD_MIRROR_PANEL_W)
    {
      w = (uint16_t)(LCD_MIRROR_PANEL_W - x);
    }

  if ((uint32_t)y + h > LCD_MIRROR_PANEL_H)
    {
      h = (uint16_t)(LCD_MIRROR_PANEL_H - y);
    }

  if (stride == 0)
    {
      stride = (uint32_t)w * 2;
    }

  if (x == 0 && w == LCD_MIRROR_PANEL_W && stride == LCD_MIRROR_ROW_BYTES)
    {
      /* 整行对齐的快路径：整块一次拷完 */
      memcpy(g_shadow + (size_t)y * LCD_MIRROR_ROW_BYTES, px,
             (size_t)h * LCD_MIRROR_ROW_BYTES);
    }
  else
    {
      FAR uint8_t *dst = g_shadow + (size_t)y * LCD_MIRROR_ROW_BYTES +
                         (size_t)x * 2;

      for (row = 0; row < h; row++)
        {
          memcpy(dst, px + (size_t)src_row, (size_t)w * 2);
          dst     += LCD_MIRROR_ROW_BYTES;
          src_row += stride;
        }
    }

  lm_mark_rows(y, h);

  nxmutex_lock(&g_lock);
  g_stat_capture++;
  nxmutex_unlock(&g_lock);
}

void lcd_mirror_request_full_redraw(void)
{
  nxmutex_lock(&g_lock);
  memset(g_dirty, 0xff, sizeof(g_dirty));
  g_redraw_req = true;
  nxmutex_unlock(&g_lock);
}

bool lcd_mirror_take_redraw_request(void)
{
  bool req;

  nxmutex_lock(&g_lock);
  req          = g_redraw_req;
  g_redraw_req = false;
  nxmutex_unlock(&g_lock);
  return req;
}

int lcd_mirror_autostart(void)
{
#if LCD_MIRROR_AUTOSTART
  return lcd_mirror_start();
#else
  return OK;
#endif
}
