/****************************************************************************
 * app/hw_test/main.c
 *
 * SF32LB52-DevKit-LCD 硬件自检（显示 / 触摸 / 按键 / GPIO /
 *                                  IMU / RTC / 音频输入）
 *
 * 用法：
 *   hw_test                 只读自检：不动屏幕、不拉 GPIO 电平
 *   hw_test touch <秒>       指定触摸观察时长（0 = 跳过触摸步骤）
 *   hw_test lcdcolor        额外做一次刷色测试（会改屏，退出前清屏）
 *   hw_test gpio            额外翻转一次板级 GPIO 输出脚 PA26（/dev/gpio1）
 *   hw_test imu [帧数]       读 LSM6DS3 加速度/陀螺（默认 10 帧）—— 单独运行
 *   hw_test rtc [秒]         读 RTC 时间 + 设一个 N 秒后的 alarm（默认 3 秒）—— 单独运行
 *   hw_test rtcday [时] [分]  每日定时提醒：到点回调一次（默认 = 当前时间 + 1 分钟）
 *                            —— 单独运行，最多等 90 秒
 *   hw_test audio [秒]       录 N 秒到内存，打印 peak/avg（默认 2 秒）—— 单独运行
 *   hw_test alarm [级别] [秒] 触发报警（1=提示 2=警告 3=紧急，默认 3），
 *                            持续 N 秒（默认 3）后解除 —— 单独运行
 *   hw_test lcd [0..100]     设屏幕亮度（默认 100）并回读 —— 单独运行；
 *                            本板只有 0 / 100 是真的，中间值会 FAIL
 *   hw_test button [秒]      等按键按下（默认 15 秒）：按到才 PASS，
 *                            超时是 FAIL —— 单独运行
 *   hw_test status           打印统一外设状态（board_status_get）：网络拿
 *                            到 IPv4 地址才算 PASS，其它设备只提示 —— 单独运行
 *   hw_test kws enroll <slot> [秒]  录唤醒词模板（命令词识别，MFCC+DTW）：
 *                            slot 0..3（0=「你好，openvela」、1=「Hello，openvela」），
 *                            秒数默认 4（钳到 2..8），存 /data/kws/slotN.tpl
 *                            —— 单独运行，且必须先停掉 ai_companion
 *   hw_test kws test         打印唤醒词模板数 / 阈值 / 每个槽位是否可用
 *                            —— 单独运行
 *   hw_test kws selftest     唤醒词模块自检（合成 1kHz 查 FFT/Mel 表、
 *                            模板自比对和互距离、实测 MFCC/DTW 耗时），
 *                            并打印算出来的**推荐阈值** —— 单独运行，
 *                            且**必须先停掉 ai_companion**（它和正在跑的
 *                            唤醒词共享 kws_dtw 的全局状态）
 *   hw_test kws threshold <值|default>  当场改判定阈值（默认 1800，单位见
 *                            kws_dtw.h），打印新旧值；`default` = 恢复 1800 ——
 *                            只对本次运行有效，重启回到默认；只写一个全局
 *                            变量、不开麦克风，可以趁着 ai_companion 在跑
 *                            直接改 —— 单独运行
 *   hw_test kws live [秒]    实时听唤醒词，命中就打一行（默认 10 秒）
 *                            —— 单独运行，且必须先停掉 ai_companion
 *   hw_test lcdreinit        面板重新初始化（黑屏救回）：重发一遍面板初始化
 *                            序列 + 拉一次 RESET 脚，再请界面全屏重绘一次
 *                            —— 单独运行；整屏黑但串口/触摸还活着时敲它
 *   hw_test lcdmirror [start|stop|status|uart|tcp] [ip|节点] [port]
 *                            屏幕镜像（板端 -> PC）+ **鼠标当触摸**（反向通道）：
 *                            把界面像素发到电脑上显示，同时把电脑上的鼠标
 *                            当成板子的触摸（本机触摸 IC 已经不应答、
 *                            /dev/input0 都没了，这是唯一能操作界面的路）。
 *                            协议 v2：20 字节头，载荷可 RLE（见 lcd_mirror.h）。
 *                            **开机自动起**（传输默认 TCP），目标默认
 *                            192.168.137.1:5600；不给参数就打印状态。
 *                            `uart [节点]` 切到控制台串口那条腿（不依赖 USB
 *                            网络，帧格式一模一样，默认 /dev/console）；
 *                            `tcp <ip> [port]` 切回来。PC 端跑
 *                            D:/apply/claw/_flash/lcd_mirror.py
 *                            —— 单独运行
 *   hw_test lcdtap <x> <y> <0|1>
 *                            注入一次触摸（x/y 是面板坐标，越界会钳住；
 *                            1=按下 0=抬起）。**串口模式下的触摸入口**：
 *                            PC 往串口里写这一行文本，NSH 执行它 ——
 *                            反向触摸不走帧的字节流（二进制包会被行输入吃掉）。
 *                            拖动就是连着发 `lcdtap x y 1`，最后 `lcdtap x y 0`
 *                            —— 单独运行，且镜像要在跑
 *
 * 设计约定：
 *   - 每一步失败都只打印 FAIL，不中断后面的步骤，也不会卡死
 *     （所有 open/ioctl/read 都判返回值，read 前先用 poll 等超时）
 *   - 默认（不带参数）不碰屏幕、不拉 GPIO 电平，只做只读自检
 *   - imu / rtc / audio / alarm / button 都会真的开外设（START 转换、
 *     设 alarm、开麦克风、响喇叭、等按键），所以放在子命令里；而且它们
 *     **不跑**上面那套 5 步自检，只跑自己，免得每次验 IMU 还要先等
 *     10 秒触摸 + 5 秒按键
 *   - kws 各子命令都会碰 kws_dtw 的全局状态（模块没有锁），enroll / live 还
 *     要**独占麦克风**：本板半双工，且整机是单一大镜像（ai_companion 开机
 *     自启后会一直持有麦克风、在自己的线程里喂 kws_feed），所以跑之前必须
 *     先停掉 ai_companion，否则 audio_in_start() 直接 -EBUSY。
 *     enroll / live / selftest 都**先把麦克风拿到手**，拿不到就整条子命令
 *     FAIL、一个 kws_dtw 接口都不碰（kws_init/kws_enroll/kws_selftest 会清或
 *     改那些全局状态，先动它会把正在跑的唤醒词链路悄悄搞坏，而 -EBUSY 要到
 *     数完倒计时才发现）。selftest 不录音，拿麦只是确认"没有别的会话在跑"，
 *     确认完立刻还回去
 *     例外是 `kws threshold`：它只写模块里那个阈值全局变量，不碰流式状态、
 *     也不开麦克风，**可以趁着 ai_companion 在跑**直接改 —— 同一镜像里共享
 *     那个变量，改完立刻对正在跑的唤醒词生效（现场标定最常用的就是它）
 *   - 默认自检里的按键步骤是"非交互"的（没人按也算 PASS，只证明能读）；
 *     要真的验证按键，跑 `hw_test button`，它超时会 FAIL
 *   - **按键一律走板级 GPIO 模块 `sf32lb52_boardbtn`，不碰 /dev/buttons**：
 *     那个节点的 poll()/read() 在真机上会让整机静默卡死（零输出、USB 设备
 *     消失、只能重烧），现象和原因写在 `sf32lb52_boardbtn.h` 文件头
 *
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/compiler.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <poll.h>

#include <nuttx/lcd/lcd_dev.h>
#include <nuttx/video/fb.h>
#include <nuttx/input/touchscreen.h>
#include <nuttx/input/buttons.h>
#include <nuttx/ioexpander/gpio.h>
#include <nuttx/timers/rtc.h>
#include <nuttx/sensors/ioctl.h>       /* SNIOC_START / SNIOC_STOP / ... */
#include <nuttx/clock.h>               /* clock_systime_ticks() + TICK2MSEC() */
#include <nuttx/sched.h>               /* task_create() */
#include <signal.h>                    /* SIGUSR1 + sigaction() */

/* 音频不再直接开 fd：录音走板级封装 sf32lb52_audio_in（它自己 include
 * <nuttx/audio/audio.h>），这里只保留设备路径用于打印。 */
#include "sf32lb52_alarm.h"            /* 板级报警模块（只出声，不驱灯） */
#include "sf32lb52_rtc_alarm.h"        /* 板级 RTC 每日定时提醒模块 */
#include "sf32lb52_audio_in.h"         /* 板级录音封装 audio_in_start/read/stop */
#include "sf32lb52_backlight.h"        /* 板级亮度封装 backlight_set/get */
#include "sf32lb52_boardbtn.h"         /* 板级按键：GPIO 轮询 + 回调（不用 /dev/buttons） */
#include "sf32lb52_status.h"           /* 板级统一外设状态 board_status_get/dump */
#include "lcd_mirror.h"                /* 板级屏幕镜像（屏幕坏了拿 PC 当显示器） */
/* 面板重新初始化（黑屏救回）：实现在 robot_ui 那个 app 的 robot_ui_bridge.c，
 * 它自己再去调 vendor 面板驱动的 sf32lb_lcd_panel_reinit()。
 * 走 robot_ui 这一跳而不是在 hw_test 里直接调驱动，是因为"重初始化之后要重绘"
 * 必须由 LVGL 线程去做（lv_async_call 投递），而 LVGL 只有 robot_ui 在跑。
 * 头文件路径由 CMakeLists.txt 的 `../robot_ui` 提供。 */
#include "robot_ui_bridge.h"

/* tts 子命令要**播**声音：板级只封了录音（sf32lb52_audio_in），所以这里直接开
 * /dev/audio/audio0，ioctl 顺序照 app/audio_test 和 ai_audio.c 真机验证过的那套
 * （open -> CONFIGURE(OUTPUT) -> CONFIGURE(FEATURE/VOLUME) -> START ->
 *  write -> STOP -> close）。 */
#include <nuttx/audio/audio.h>

/* tts / asr 子命令还要用 hello_app 的 MiMo 后端（mimo_voice.h）和 ai_agent 的
 * voice_asr / voice_tts 分发层 —— 三处都得编进同一个固件才有符号。
 * 这两个总开关与 app/hello_app、app/hw_test 的 CMakeLists 用的是同一对，
 * 所以没开的配置里这两个子命令只打印"本配置不支持"，其余子命令不受影响。 */

#if defined(CONFIG_HELLO_APP_LLM_AI_AGENT) \
    && defined(CONFIG_LVX_USE_CONTEST2026_233_HELLO_APP)
#  define HW_TEST_HAS_VOICE 1
#  include "voice/voice_asr.h"
#  include "voice/voice_tts.h"
#  include "mimo_voice.h"
#endif

/* 唤醒词自检（kws 子命令）要用 hello_app 的命令词识别模块 kws_dtw.c：
 * 它只在 hello_app 被编进固件时才有符号，而 hw_test 的 CMakeLists 里已经用
 * `../hello_app` 拿到了头文件（跨 app 的符号在最终链接时解析 —— 和 tts/asr
 * 借 hello_app 的 MiMo 后端是同一套做法），所以这里判一下总开关：
 * 没开 hello_app 时 kws 子命令只打印"本配置不支持"，其余子命令不受影响。 */

#if defined(CONFIG_LVX_USE_CONTEST2026_233_HELLO_APP)
#  define HW_TEST_HAS_KWS 1
#  include "kws_dtw.h"
#endif

/* IMU：本板的 LSM6DS3 走的是 NuttX **老式字符驱动**（不是 uORB），
 * 节点是 /dev/lsm6dsl0，接口是 read() + ioctl(SNIOC_*)。
 * 头文件本身被 CONFIG_SENSORS_LSM6DSL 包着，所以这里也要判一下，
 * 否则编不过（拿不到 struct lsm6dsl_sensor_data_s）。
 */

#if defined(CONFIG_I2C) && defined(CONFIG_SENSORS_LSM6DSL)
#  include <nuttx/sensors/lsm6dsl.h>
#  define HW_TEST_HAS_IMU 1
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define LCD_DEV        "/dev/lcd0"
#define INPUT_DEV      "/dev/input0"
#define GPIO_IN_DEV    "/dev/gpio0"
#define GPIO_OUT_DEV   "/dev/gpio1"
#define GPIO_INT_DEV   "/dev/gpio2"
#define IMU_DEV        "/dev/lsm6dsl0"
#define RTC_DEV        "/dev/rtc0"
#define AUDIO_DEV      "/dev/audio/audio0"   /* 注意：带 audio/ 子目录 */

#define TOUCH_MAX_POINTS   16    /* FT6146 注册上限，见 touch_register(...,16) */
#define TOUCH_SAMPLES_WANT 20    /* 读满这么多点就提前结束 */
#define TOUCH_DEFAULT_SEC  10    /* 默认观察时长 */
#define TOUCH_POLL_MS      200
#define BTN_TIMEOUT_MS     5000  /* 没人按键时的等待上限 */
#define BTN_POLL_MS        200   /* 主任务查"按键模块报了新事件没"的间隔 */
#define LCD_BAND_ROWS      60    /* 刷色时每次写多少行（与 LVGL 分块缓冲同量级） */

#define RGB565_RED         0xf800
#define RGB565_GREEN       0x07e0
#define RGB565_BLUE        0x001f
#define RGB565_WHITE       0xffff
#define RGB565_BLACK       0x0000

/* imu 子命令 */

#define IMU_DEFAULT_FRAMES   10    /* 默认打印帧数 */
#define IMU_FRAME_MS         100   /* 帧间隔（ms）；10 帧约 1 秒 */

/* rtc 子命令 */

#define RTC_DEFAULT_ALARM_SEC 3    /* 默认 alarm 延时（秒） */
#define RTC_WAIT_SLACK_SEC    2    /* 等 alarm 的额外宽限；超时就 FAIL 退出 */

/* audio 子命令（参数与板级封装 sf32lb52_audio_in 的约定一致：16k 单声道 16bit） */

#define AUDIO_SAMPLE_RATE     16000
#define AUDIO_CHANNELS        1
#define AUDIO_BITS            16
#define AUDIO_DEFAULT_SEC     2    /* 默认录音秒数 */
#define AUDIO_WAIT_SLACK_SEC  2    /* read 没在预期时间内返回就发 STOP 救场 */
#define AUDIO_SOUND_PEAK      500  /* peak 超过它算“检测到声音”（同 audio_test） */
#define AUDIO_READ_TASK_STACK 4096

/* 驱动下层一次 read 只等 5 秒（sf32lb52_audio.c:1130 的 rx_sem 超时），
 * 所以超过 1 秒的录音要拆成 1 秒一块地读（audio_in_read 的约定），
 * 否则 6 秒的录音必然返回 0。
 * 32000 字节 = 16k 单声道 16bit × 1 秒，正是 audio_test 验证过的大小。
 */
#define AUDIO_CHUNK_BYTES     (AUDIO_SAMPLE_RATE * 2)

/* tts / asr 子命令（小米 MiMo 云端 TTS / ASR）。
 * 这两个是**联网**子命令：要先拿到 IPv4、配置里要有 llm_host + api_key
 * （开机从 /etc/assets/agent_config.json 拷到 /data/ai_agent/config/config.json），
 * 缺了就 FAIL，不影响其它子命令。 */

#define TTS_PCM_CAP        (256 * 1024)  /* 合成缓冲：16k 单声道 16bit ≈ 8 秒 */
#define TTS_MAX_TEXT       512           /* tts 子命令拼起来的文本上限 */
#define PLAY_CHUNK_BYTES   3200          /* 100ms 一块，跟 ai_audio.c 的播放线程一致 */
#define PLAY_VOLUME        70            /* 0..100（换算到驱动的 0..1000） */
#define ASR_TEXT_CAP       1024          /* 识别结果缓冲（字节） */

/* alarm 子命令（设备级报警模块：响喇叭，持续到解除或超时；不驱指示灯） */

#define ALARM_DEFAULT_LEVEL   3     /* 1=NOTICE 2=WARNING 3=EMERGENCY */
#define ALARM_DEFAULT_SEC     3     /* 默认保持报警的秒数 */
#define ALARM_POLL_MS         100   /* 等报警结束的轮询间隔 */
#define ALARM_SETTLE_MS       500   /* 解除后等一小会，让工作线程停音频/灭灯 */

/* lcd 子命令（板级亮度封装 sf32lb52_backlight）。
 * 打了 patches/vendor_sifli-lcd-brightness.patch 之后 0..100 都能 PASS
 * （中间值走面板亮度寄存器）；没打补丁的树只有 0 和 100 能 PASS，
 * 中间值 backlight_set() 会返回 -ENOSYS（本子命令判为 FAIL）。 */

#define BACKLIGHT_DEFAULT_PCT 100   /* 不带参数时设成"全亮" */
#define BACKLIGHT_SETTLE_MS   200   /* 下发后等一小会，再回读 */
#define BACKLIGHT_POLL_MS     50

/* rtcday 子命令（RTC 每日定时提醒模块；只有一个 alarm 槽） */

#define RTCDAY_MAX_WAIT_MS    (90 * 1000)  /* 等回调的上限，绝不永久卡住 */
#define RTCDAY_POLL_MS        200          /* 等回调的轮询间隔 */

/* button 子命令：真的等一次按下，超时算 FAIL */

#define BTN_DEFAULT_WAIT_SEC  15    /* 默认等待秒数 */

/* kws 子命令（唤醒词「你好，openvela」/「Hello，openvela」的命令词识别，
 * 模块本体在 app/hello_app/kws_dtw.c）。
 *
 * 模板落盘约定（见 kws_dtw.h）：/data/kws/slotN.tpl，N = 0..3，
 * 其中 slot0 =「你好，openvela」、slot1 =「Hello，openvela」。
 * ⚠ 本板 /data 是 tmpfs —— 重启就丢，要长期保留得靠别的手段搬走/重录。
 *
 * ⚠ 半双工 + 单一大镜像：ai_companion 开机自启后会一直持有麦克风（听唤醒词）
 *   并在自己的线程里喂 kws_feed，所以跑 kws 子命令之前必须先把它停掉，
 *   否则 audio_in_start() 直接 -EBUSY。
 *   enroll / live / selftest 都**先确认麦克风拿到手**（kws_mic_acquire），
 *   拿不到立刻 FAIL、一个 kws_dtw 接口都不碰 —— 否则 kws_init/kws_enroll/
 *   kws_selftest 会先清掉/改掉 ai_companion 录音线程正在用的那些全局状态。
 *   **例外是 `kws threshold`**：它只写 kws_dtw 的阈值全局变量、不初始化模块
 *   也不开麦克风，可以趁 ai_companion 在跑时直接改（同一镜像共享那个变量，
 *   改完立刻对正在跑的唤醒词生效）—— 现场标定就该这么用。 */

#define KWS_ENROLL_DEFAULT_SEC 4    /* 录模板默认时长；1~2 秒的短语 + 尾静音够用 */
#define KWS_ENROLL_MIN_SEC     2    /* 再短就可能把短语截掉，录出个残模板 */
#define KWS_ENROLL_MAX_SEC     8    /* 再长没意义（整句上限 2 秒），而且缓冲会变大：
                                     * 8 秒 = 256 KB，和 TTS 缓冲同量级（那一块
                                     * 在真机上分得到） */
#define KWS_LIVE_DEFAULT_SEC   10   /* kws live 默认听多久 */
#define KWS_LIVE_MAX_SEC       30   /* 最长听多久：别把唯一的麦克风占太久 */
#define KWS_PREP_SEC           3    /* 录音前的准备时间（数 3..1 再开始录） */

/* 100ms 一块：远小于封装建议的 1 秒上限，而且 live 模式喂帧的粒度就是它
 * （拆成 1 秒一块的话，命中最多要等 1 秒才报出来） */

#define KWS_READ_CHUNK_BYTES   (AUDIO_SAMPLE_RATE * 2 / 10)
#define KWS_READ_TASK_STACK    4096   /* 同 AUDIO_READ_TASK_STACK */

/* 阈值的"再往上就没意义了"那条线，等于 kws_dtw.c 里的 KWS_DP_CAP_DIST_MIN。
 * 那个宏在 .c 里（没导出到 .h），这里照抄一份常量：
 * 距离被 DTW 的 DP 上限钉住，qn+tn=200 时最大也就 9690 —— 阈值设过它，
 * "最不像的东西"也会被判命中，唤醒词等于变成「随便什么都唤醒」。
 * kws_dtw.c 那边只警告不拒绝（标定的人可能故意），但这里是给人手敲的入口，
 * 多打一个 0（1800 → 18000）就会静默废掉判别力，所以本子命令直接拒绝设置。 */

#define KWS_TH_MAX_SAFE        9690

/* kws 子命令的动作（main 解析参数时用；0 = 没选 kws）。
 * 阈值标定占两个：selftest 算推荐值，threshold 当场改（见 step_kws_*）。 */

#define KWS_CMD_NONE      0
#define KWS_CMD_ENROLL    1
#define KWS_CMD_TEST      2
#define KWS_CMD_LIVE      3
#define KWS_CMD_SELFTEST  4
#define KWS_CMD_THRESHOLD 5

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct dev_node_s
{
  FAR const char *path;       /* 设备节点 */
  FAR const char *desc;       /* 用途 */
  FAR const char *optional;   /* 非 NULL：本配置下可能不注册，缺失不算故障 */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static int g_pass;
static int g_total;

/* rtc 子命令：alarm 到点后由信号处理函数置位 */

static volatile sig_atomic_t g_rtc_alarm;

/* audio 子命令：阻塞的 read() 放在独立任务里，主任务带超时等它。
 * fd 由板级封装 sf32lb52_audio_in 自己持有，这里不再存一份。 */

static volatile int     g_arec_done;
static volatile ssize_t g_arec_n;
static FAR int16_t     *g_arec_buf;
static int              g_arec_len;

/* kws 子命令：录音读任务和主任务之间的交接状态。
 * 和 audio 子命令同一套写法（task_create 只能带一个参数，所以用文件级全局 +
 * volatile 标志）。g_krec_buf 只在 enroll 模式（整段 PCM 要交给 kws_enroll）
 * 里用 malloc 拿；live 模式用下面那块静态缓冲边读边喂，不攒 PCM。 */

static volatile int  g_krec_done;    /* 读任务结束 */
static volatile int  g_krec_got;     /* 已读字节数 */
static volatile int  g_krec_err;     /* 读任务停下时 read 的返回值 */
static volatile int  g_krec_hits;    /* live 模式：命中次数（每命中一次打一行） */
static FAR int16_t  *g_krec_buf;     /* enroll 模式：攒 PCM 的缓冲（live 模式为 NULL） */
static int           g_krec_len;     /* 一共要读多少字节 */
static int           g_krec_live;    /* 1 = 边读边喂 kws_feed */

static int16_t g_krec_live_buf[AUDIO_SAMPLE_RATE / 10];   /* 100ms（KWS_READ_CHUNK_BYTES） */

/* alarm 子命令：模块工作线程里回调，这里只累计事件给主任务打印 */

static volatile int g_alarm_events;
static volatile int g_alarm_last_level;

/* rtcday 子命令：模块工作线程里回调，这里只置标志给主任务打印 */

static volatile sig_atomic_t g_rtcday_fired;
static volatile int          g_rtcday_calls;

/* 按键（button 子命令 + 默认自检第 4 步）：回调跑在板级按键模块的轮询
 * 任务里，只能写 volatile 变量；主任务靠 g_btn_ev_seq 发现"有新事件"，
 * 再把最近一次事件的字段打印出来（中间的事件可能被合并，自检够用）。 */

static volatile int      g_btn_ev_seq;
static volatile enum board_btn_e       g_btn_ev_btn;
static volatile enum board_btn_event_e g_btn_ev_type;
static volatile uint32_t g_btn_ev_held;
static volatile int      g_btn_press_seen;
static volatile int      g_btn_lp_seen;
static volatile enum board_btn_e g_btn_press_which;
static volatile uint32_t g_btn_press_held;

/* 与本板硬件相关的设备节点（枚举顺序 = 打印顺序）。
 * optional 非 NULL 的条目：本配置下就是不注册，缺失**不**计入"节点缺失"。 */

static const struct dev_node_s g_nodes[] =
{
  { "/dev/lcd0",         "AMOLED (CO5300), NuttX LCD dev", NULL },
  { "/dev/fb0",          "framebuffer (LCD 的 fb 前端)",   NULL },
  { "/dev/input0",       "touchscreen (FT6146)",           NULL },
  { "/dev/buttons",      "buttons upper half (Key2 = PA11) —— 节点还在，"
                         "但 poll/read 会卡死整机，按键别用它",  NULL },
  { "/dev/gpio0",        "GPIO input  (PA34, Key1/power key)", NULL },
  { "/dev/gpio1",        "GPIO output (PA26, 板级用户输出)",   NULL },
  { "/dev/gpio2",        "GPIO interrupt (PA34)",          NULL },
  { "/dev/timer0",       "timer",                          NULL },
  /* /dev/pwm0 由 sifli_ap.c 在 #ifdef CONFIG_PWM 里注册。本配置
   * CONFIG_PWM=n，节点必然不存在 —— 这是配置选择，不是故障。 */
#if defined(CONFIG_PWM)
  { "/dev/pwm0",         "pwm (背光 PA01 = GPTIM1_CH4)",   NULL },
#else
  { "/dev/pwm0",         "pwm (背光 PA01 = GPTIM1_CH4)",   "本配置未启用（CONFIG_PWM=n），不是故障" },
#endif
  { "/dev/i2c0",         "i2c master (触摸 FT6146: SCL=PA30 SDA=PA33)", NULL },
  { "/dev/i2c1",         "i2c master (I2C2)",              NULL },
  { "/dev/adc0",         "adc",                            NULL },
  { "/dev/audio/audio0", "audio (NS4150B 功放 + MEMS MIC)", NULL },
  /* 注意：网络接口**不是** /dev 节点。NuttX 的网卡是 netdev，
   * 走 socket + ioctl(SIOCGIF*) 访问，标准做法见下面 netdev_probe()。
   * 之前这里写成 "/dev/eth0" 是错的；eth0 在不在取决于 USB RNDIS
   * 有没有枚举起来，所以那条结论也不计入 PASS/FAIL。 */
};

#define NNODES ((int)(sizeof(g_nodes) / sizeof(g_nodes[0])))

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t mono_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void report(FAR const char *name, int ok, FAR const char *detail)
{
  g_total++;
  if (ok)
    {
      g_pass++;
    }

  printf("      [%s] %s", ok ? "PASS" : "FAIL", name);
  if (detail != NULL && detail[0] != '\0')
    {
      printf("  (%s)", detail);
    }

  printf("\n");
}

static void usage(void)
{
  printf("用法:\n");
  printf("  hw_test              只读自检（不动屏幕、不拉 GPIO 电平）\n");
  printf("  hw_test touch <秒>    指定触摸观察时长，0 = 跳过\n");
  printf("  hw_test lcdcolor     额外刷色测试（会改屏，退出前清屏）\n");
  printf("  hw_test gpio         额外翻转一次 /dev/gpio1 (PA26)\n");
  printf("  hw_test imu [帧数]   IMU(LSM6DS3) 加速度/陀螺，默认 10 帧（单独运行）\n");
  printf("  hw_test rtc [秒]     RTC 时间 + N 秒后的 alarm，默认 3 秒（单独运行）\n");
  printf("  hw_test rtcday [时] [分]  每日定时提醒，默认当前时间 + 1 分钟，"
         "最多等 90 秒（单独运行）\n");
  printf("  hw_test audio [秒]   录音电平 peak/avg，默认 2 秒（单独运行）\n");
  printf("  hw_test alarm [级别] [秒]  报警：1=提示 2=警告 3=紧急(默认)，"
         "持续秒数默认 3（单独运行）\n");
  printf("  hw_test lcd [0..100]  设屏幕亮度并回读，默认 100（单独运行）；"
         "中间值走面板亮度寄存器（需 vendor 补丁，见 patches/README.md）\n");
  printf("  hw_test lcdreinit    面板重新初始化（黑屏救回）：重发面板初始化"
         "序列 + 拉一次 RESET 脚，\n"
         "                       再请界面全屏重绘一次。整屏黑、"
         "但串口还活着时敲它（单独运行）\n");
  printf("  hw_test lcdmirror [start|stop|status|uart|tcp] [ip|节点] [port]\n"
         "                       屏幕镜像（屏幕坏了拿 PC 当显示器）：开机"
         "**自动起**，传输默认 TCP，目标默认\n"
         "                       %s:%d；不给参数就打印状态（含当前传输、"
         "节点名/目标地址、收到多少条触摸）。\n"
         "                       协议 v2（20 字节头 + RLE 载荷）。`uart [节点]` "
         "切控制台串口那条腿（默认\n"
         "                       /dev/console，帧格式一模一样，不依赖 USB 网络；"
         "每 3 秒发一次整屏关键帧），\n"
         "                       `tcp <ip> [port]` 切回来。PC 端 "
         "_flash/lcd_mirror.py 里按住鼠标 = 点屏幕\n",
         LCD_MIRROR_DEFAULT_IP, LCD_MIRROR_DEFAULT_PORT);
  printf("  hw_test lcdtap <x> <y> <0|1>   注入一次触摸（面板坐标，1=按下 "
         "0=抬起）。**串口模式下的触摸入口**：\n"
         "                       PC 往串口里写这行文本、由 NSH 执行；"
         "拖动就连续发 `lcdtap x y 1`，\n"
         "                       最后一条 `lcdtap x y 0` 抬手（镜像要在跑）\n");
  printf("  hw_test button [秒]  等按键按下（板级 GPIO：PA11=KEY / PA34=HOME），"
         "默认 15 秒，超时算 FAIL（单独运行）\n");
  printf("  hw_test status       打印统一外设状态（board_status_get/dump）；"
         "网络没拿到 IPv4 地址算 FAIL，其它设备只提示（单独运行）\n");
  printf("  hw_test tts <文本>   MiMo 云端 TTS 合成，打印字节数并直接从喇叭"
         "放出来；带空格要加引号，如 hw_test tts \"你好，今天天气不错\""
         "（要联网 + 配好 api_key/llm_host，单独运行）\n");
  printf("  hw_test asr <文件>   读一个 WAV（16bit PCM，采样率不限，内部转 16k）"
         "做 MiMo 云端语音识别并打印结果（单独运行）\n");
  printf("  hw_test kws enroll <slot> [秒]  录唤醒词模板：slot 0..3"
         "（0=「你好，openvela」、1=「Hello，openvela」），秒数默认 4（2..8），"
         "存到 /data/kws/slotN.tpl（单独运行）\n");
  printf("  hw_test kws test     打印唤醒词模板数/阈值/每个槽位是否可用"
         "（单独运行）\n");
  printf("  hw_test kws selftest 唤醒词模块自检（合成 1kHz 查 FFT/Mel 表、"
         "模板自比对/互距离、\n"
         "                       实测 MFCC/DTW 耗时）并打印推荐阈值"
         "（单独运行）\n"
         "                       **必须先停 ai_companion**：它和正在跑的"
         "唤醒词共享 kws_dtw\n"
         "                       的全局状态（没模板时不报 PASS，会提示先 enroll）\n");
  printf("  hw_test kws threshold <值|default>  当场改判定阈值，打印新旧值；"
         "默认 1800，只对本次运行有效、\n"
         "                       重启回默认（可以趁 ai_companion 在跑时改，"
         "改完立刻生效）（单独运行）\n"
         "                       大于 9690 会让任何输入都判命中，直接拒绝；"
         "`threshold default` 恢复 1800\n");
  printf("  hw_test kws live [秒]  实时听唤醒词，命中就打一行，默认 10 秒"
         "（单独运行）\n");
  printf("      注意：kws 各子命令都碰 kws_dtw 的全局状态，enroll/live/selftest"
         " 还会先开一下麦克风确认它\n"
         "            空闲（拿不到就整条命令 FAIL，一个 kws 接口都不动）——"
         " ai_companion 开机自启后会\n"
         "            一直持有它（半双工），跑之前必须先停掉 ai_companion，"
         "否则 audio_in_start 直接 -EBUSY\n"
         "            （例外：threshold 只写一个全局变量，不用停 ai_companion）\n");
}

/****************************************************************************
 * Name: node_exists
 ****************************************************************************/

static int node_exists(FAR const char *path)
{
  struct stat st;

  return stat(path, &st) == 0;
}

/****************************************************************************
 * Name: netdev_probe
 *
 * Description:
 *   用标准 netdev API 查一个网络接口的 IP / 网关。
 *   NuttX 的网卡是 netdev，不是 /dev 节点，必须用 socket + ioctl(SIOCGIF*)
 *   访问（这也是标准 openvela/NuttX 的用法）。
 *
 *   返回 0 表示接口已配置好，-1 表示没有这个接口或没配 IP。
 *   注意：这条**不计入 PASS/FAIL**——eth0 存不存在取决于 USB RNDIS
 *   有没有在主机侧枚举起来，没插 USB 时接口不存在是正常的，不是故障。
 *
 ****************************************************************************/

static int netdev_probe(FAR const char *ifname)
{
  struct ifreq ifr;
  struct sockaddr_in *sin;
  int fd;
  int ret = -1;

  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    {
      printf("      %-8s --    创建 socket 失败: %d\n", ifname, errno);
      return -1;
    }

  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

  if (ioctl(fd, SIOCGIFADDR, (unsigned long)&ifr) < 0)
    {
      printf("      %-8s --    接口未配置（USB RNDIS 未枚举，不是故障）\n",
             ifname);
      close(fd);
      return -1;
    }

  sin = (struct sockaddr_in *)&ifr.ifr_addr;
  printf("      %-8s OK    IP = %s", ifname, inet_ntoa(sin->sin_addr));
  ret = 0;

  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
  if (ioctl(fd, SIOCGIFDSTADDR, (unsigned long)&ifr) == 0)
    {
      sin = (struct sockaddr_in *)&ifr.ifr_dstaddr;
      printf("  网关 = %s", inet_ntoa(sin->sin_addr));
    }

  printf("\n");
  close(fd);
  return ret;
}

/****************************************************************************
 * Name: step_nodes
 *
 * Description:
 *   第 1 步：枚举与本板硬件相关的设备节点，打印存在性。
 *   网络接口用 netdev API 单独查（它不是 /dev 节点）。
 *
 *   optional 条目（如 CONFIG_PWM=n 时的 /dev/pwm0）缺失是正常配置差异，
 *   不计入"节点缺失"，打印时也会明确标出。
 *
 ****************************************************************************/

static int step_nodes(void)
{
  int missing = 0;
  int i;

  printf("[1/6] 设备节点枚举\n");

  for (i = 0; i < NNODES; i++)
    {
      int ok = node_exists(g_nodes[i].path);

      if (!ok && g_nodes[i].optional == NULL)
        {
          missing++;
        }

      printf("      %-18s %-3s  %s", g_nodes[i].path, ok ? "OK" : "--",
             g_nodes[i].desc);

      if (!ok && g_nodes[i].optional != NULL)
        {
          printf("（%s）", g_nodes[i].optional);
        }

      printf("\n");
    }

  /* 网卡不是 /dev 节点，单独用 netdev API 查（不计入 PASS/FAIL） */
  netdev_probe("eth0");

  if (missing == 0)
    {
      report("必需节点存在", 1, NULL);
    }
  else
    {
      char detail[32];

      snprintf(detail, sizeof(detail), "%d 个节点缺失", missing);
      report("必需节点存在", 0, detail);
    }

  return missing == 0 ? OK : -1;
}

/****************************************************************************
 * Name: print_touch_point
 ****************************************************************************/

static void print_touch_point(int idx, FAR const struct touch_point_s *pt)
{
  printf("      #%-2d id=%d flags=0x%02x x=%d y=%d h=%d%s%s%s\n",
         idx, pt->id, pt->flags, pt->x, pt->y, pt->h,
         (pt->flags & TOUCH_DOWN) != 0 ? " DOWN" : "",
         (pt->flags & TOUCH_MOVE) != 0 ? " MOVE" : "",
         (pt->flags & TOUCH_UP) != 0 ? " UP" : "");
}

/****************************************************************************
 * Name: step_touch
 *
 * Description:
 *   第 2 步：非阻塞读 /dev/input0，轮询 seconds 秒或读满 TOUCH_SAMPLES_WANT 个
 *   样点就退出。没摸屏幕不算失败（没人碰而已），打印提示后正常结束。
 *
 ****************************************************************************/

static int step_touch(int seconds)
{
  uint8_t buf[sizeof(struct touch_sample_s) +
              TOUCH_MAX_POINTS * sizeof(struct touch_point_s)];
  uint32_t t0;
  int samples = 0;
  int fd;

  printf("[2/6] 触摸 %s\n", INPUT_DEV);

  if (seconds <= 0)
    {
      report("触摸观察", 1, "已跳过");
      return 0;
    }

  fd = open(INPUT_DEV, O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    {
      printf("      open 失败: %d\n", errno);
      report("打开触摸设备", 0, "open /dev/input0 失败");
      return -1;
    }

  printf("      观察 %d 秒（最多 %d 个样点），请用手指点一下屏幕...\n",
         seconds, TOUCH_SAMPLES_WANT);

  t0 = mono_ms();
  while (samples < TOUCH_SAMPLES_WANT)
    {
      struct pollfd pfd;
      ssize_t n;
      int ret;
      int i;

      if ((int)(mono_ms() - t0) >= seconds * 1000)
        {
          break;
        }

      pfd.fd      = fd;
      pfd.events  = POLLIN;
      pfd.revents = 0;

      ret = poll(&pfd, 1, TOUCH_POLL_MS);
      if (ret < 0)
        {
          usleep(TOUCH_POLL_MS * 1000);   /* poll 不可用时退化成定时轮询 */
        }
      else if (ret == 0)
        {
          continue;
        }

      memset(buf, 0, sizeof(buf));
      n = read(fd, buf, sizeof(buf));
      if (n < (ssize_t)sizeof(struct touch_sample_s))
        {
          /* EAGAIN / 被别的 reader 抢走：继续等 */
          continue;
        }

      {
        FAR struct touch_sample_s *sample = (FAR struct touch_sample_s *)buf;
        int npoints = sample->npoints;

        if (npoints > TOUCH_MAX_POINTS)
          {
            npoints = TOUCH_MAX_POINTS;
          }

        for (i = 0; i < npoints && samples < TOUCH_SAMPLES_WANT; i++)
          {
            print_touch_point(samples + 1, &sample->point[i]);
            samples++;
          }
      }
    }

  close(fd);

  if (samples == 0)
    {
      printf("      未检测到触摸，请用手指点一下屏幕\n");
      report("触摸读样点", 1, "0 个样点（这不算失败）");
    }
  else
    {
      char detail[32];

      snprintf(detail, sizeof(detail), "%d 个样点", samples);
      report("触摸读样点", 1, detail);
    }

  return 0;
}

/****************************************************************************
 * Name: lcd_fill
 *
 * Description:
 *   用 LCDDEVIO_PUTAREA 把 [row0, row1] 行刷成一个颜色（RGB565）。
 *   每次只提交 LCD_BAND_ROWS 行，避免一次 malloc 整屏 351KB。
 *
 ****************************************************************************/

static int lcd_fill(int fd, uint8_t fmt, uint16_t xres, uint16_t yres,
                    uint16_t row0, uint16_t row1, uint16_t color,
                    FAR uint8_t *buf, size_t bufsize)
{
  struct lcddev_area_s area;
  uint32_t row;

  if (row1 > yres)
    {
      row1 = yres;
    }

  for (row = row0; row < row1; row += LCD_BAND_ROWS)
    {
      uint32_t rows = (uint32_t)(row1 - row);
      size_t npixel;
      size_t need;
      size_t i;
      int ret;

      if (rows > LCD_BAND_ROWS)
        {
          rows = LCD_BAND_ROWS;
        }

      npixel = (size_t)xres * rows;
      need   = npixel * 2;
      if (need > bufsize)
        {
          return -ENOMEM;
        }

      /* RGB565 小端：低字节在前 */

      for (i = 0; i < npixel; i++)
        {
          buf[i * 2]     = (uint8_t)(color & 0xff);
          buf[i * 2 + 1] = (uint8_t)(color >> 8);
        }

      memset(&area, 0, sizeof(area));
      area.row_start = (uint16_t)row;
      area.row_end   = (uint16_t)(row + rows - 1);
      area.col_start = 0;
      area.col_end   = (uint16_t)(xres - 1);
      area.stride    = (uint32_t)xres * 2;
      area.data      = buf;

      /* 注意：struct lcddev_area_s 里**没有** fmt 字段
       * （只有 row_start/row_end/col_start/col_end/stride/data），
       * 像素格式由驱动自己的 bpp 决定，所以这里不需要传 fmt。 */
      (void)fmt;

      ret = ioctl(fd, LCDDEVIO_PUTAREA, (unsigned long)&area);
      if (ret < 0)
        {
          printf("      PUTAREA(row %u..%u) 失败: %d\n",
                 (unsigned)row, (unsigned)(row + rows - 1), ret);
          return ret;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: lcd_color_test
 *
 * Description:
 *   刷色测试：4 条横向色带（红/绿/蓝/白）-> 停一下 -> 清屏成纯黑。
 *   本程序不启动 LVGL，退出前只保证"清屏"，界面恢复靠重启或重跑 robot_ui。
 *
 ****************************************************************************/

static int lcd_color_test(int fd, FAR const struct fb_videoinfo_s *vinfo)
{
  uint16_t xres = vinfo->xres;
  uint16_t yres = vinfo->yres;
  uint16_t q    = (uint16_t)(yres / 4);
  FAR uint8_t *buf;
  size_t bufsize;
  int ret = OK;

  if (xres == 0 || yres < 4)
    {
      printf("      分辨率异常(%dx%d)，跳过刷色\n", xres, yres);
      return -1;
    }

  bufsize = (size_t)xres * LCD_BAND_ROWS * 2;
  buf = (FAR uint8_t *)malloc(bufsize);
  if (buf == NULL)
    {
      printf("      malloc %u 字节失败，跳过刷色\n", (unsigned)bufsize);
      return -ENOMEM;
    }

  printf("      刷 4 条横向色带：红/绿/蓝/白（每次 %d 行）\n",
         LCD_BAND_ROWS);

  if (lcd_fill(fd, vinfo->fmt, xres, yres, 0,
               q, RGB565_RED, buf, bufsize) < 0 ||
      lcd_fill(fd, vinfo->fmt, xres, yres, q,
               (uint16_t)(2 * q), RGB565_GREEN, buf, bufsize) < 0 ||
      lcd_fill(fd, vinfo->fmt, xres, yres, (uint16_t)(2 * q),
               (uint16_t)(3 * q), RGB565_BLUE, buf, bufsize) < 0 ||
      lcd_fill(fd, vinfo->fmt, xres, yres, (uint16_t)(3 * q),
               yres, RGB565_WHITE, buf, bufsize) < 0)
    {
      ret = -1;
    }

  sleep(1);

  printf("      清屏（纯黑）...\n");
  if (lcd_fill(fd, vinfo->fmt, xres, yres, 0, yres,
               RGB565_BLACK, buf, bufsize) < 0)
    {
      ret = -1;
    }

  free(buf);

  printf("      注意：本程序不启动 LVGL；屏幕已被直接改写。\n");
  printf("      要恢复界面请复位板子，或重新跑 robot_ui。\n");
  return ret;
}

/****************************************************************************
 * Name: step_lcd
 *
 * Description:
 *   第 3 步：读 /dev/lcd0 的显示信息（分辨率/格式/对齐要求）；
 *   带 lcdcolor 参数时再做一次刷色测试。
 *
 ****************************************************************************/

static int step_lcd(int do_color)
{
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  struct lcddev_area_align_s align;
  int ok = 1;
  int fd;
  int ret;

  printf("[3/6] 显示 %s\n", LCD_DEV);

  fd = open(LCD_DEV, O_RDWR);
  if (fd < 0)
    {
      printf("      open 失败: %d\n", errno);
      report("打开显示设备", 0, "open /dev/lcd0 失败");
      return -1;
    }

  /* 显示信息：分辨率 / 像素格式 / 平面数 */

  memset(&vinfo, 0, sizeof(vinfo));
  ret = ioctl(fd, LCDDEVIO_GETVIDEOINFO, (unsigned long)&vinfo);
  if (ret < 0)
    {
      printf("      GETVIDEOINFO 失败: %d\n", ret);
      report("GETVIDEOINFO", 0, "ioctl 失败");
      ok = 0;
    }
  else
    {
      printf("      分辨率   : %dx%d, fmt=%d, planes=%d\n",
             vinfo.xres, vinfo.yres, vinfo.fmt, vinfo.nplanes);
      report("GETVIDEOINFO", 1, NULL);
    }

  /* 当前平面信息：framebuffer 指针 / 行跨距 / 位深 */

  memset(&pinfo, 0, sizeof(pinfo));
  ret = ioctl(fd, LCDDEVIO_GETPLANEINFO, (unsigned long)&pinfo);
  if (ret < 0)
    {
      printf("      GETPLANEINFO 失败: %d\n", ret);
      report("GETPLANEINFO", 0, "ioctl 失败");
      ok = 0;
    }
  else
    {
      printf("      平面     : bpp=%d stride=%u fblen=%u fbmem=%p\n",
             pinfo.bpp, (unsigned)pinfo.stride,
             (unsigned)pinfo.fblen, pinfo.fbmem);
      report("GETPLANEINFO", 1, NULL);
    }

  /* 区域对齐要求（PUTAREA 的 row/col/buf 对齐） */

  memset(&align, 0, sizeof(align));
  ret = ioctl(fd, LCDDEVIO_GETAREAALIGN, (unsigned long)&align);
  if (ret < 0)
    {
      printf("      GETAREAALIGN 失败: %d\n", ret);
      report("GETAREAALIGN", 0, "ioctl 失败");
      ok = 0;
    }
  else
    {
      printf("      对齐要求 : row_start_align=%u height_align=%u "
             "width_align=%u buf_align=%u\n",
             align.row_start_align, align.height_align,
             align.width_align, align.buf_align);
      report("GETAREAALIGN", 1, NULL);
    }

  if (do_color)
    {
      if (vinfo.xres == 0 || vinfo.yres == 0)
        {
          report("刷色测试", 0, "拿不到分辨率");
          ok = 0;
        }
      else if (lcd_color_test(fd, &vinfo) < 0)
        {
          report("刷色测试", 0, "PUTAREA 失败");
          ok = 0;
        }
      else
        {
          report("刷色测试", 1, "已清屏");
        }
    }

  close(fd);
  return ok ? OK : -1;
}

/****************************************************************************
 * Name: hw_test_btn_reset
 *
 * Description:
 *   把按键模块的"事件信箱"清空（两个按键用例在注册回调之前各调一次）。
 *
 ****************************************************************************/

static void hw_test_btn_reset(void)
{
  g_btn_ev_seq      = 0;
  g_btn_ev_btn      = BOARD_BTN_KEY;
  g_btn_ev_type     = BOARD_BTN_PRESS;
  g_btn_ev_held     = 0;
  g_btn_press_seen  = 0;
  g_btn_lp_seen     = 0;
  g_btn_press_which = BOARD_BTN_KEY;
  g_btn_press_held  = 0;
}

/****************************************************************************
 * Name: hw_test_btn_cb
 *
 * Description:
 *   按键事件回调。**跑在板级按键模块的轮询任务里**（任务的优先级 115），
 *   所以这里只写几个 volatile 变量：不 printf、不 sleep、不碰 LVGL。
 *   打印留给主任务 —— 下面两个函数靠 g_btn_ev_seq 发现"有新事件"。
 *
 ****************************************************************************/

static void hw_test_btn_cb(enum board_btn_e btn, enum board_btn_event_e ev,
                           uint32_t held_ms, FAR void *arg)
{
  (void)arg;

  g_btn_ev_btn  = btn;
  g_btn_ev_type = ev;
  g_btn_ev_held = held_ms;

  if (ev == BOARD_BTN_PRESS)
    {
      g_btn_press_seen  = 1;
      g_btn_press_which = btn;
    }
  else if (ev == BOARD_BTN_LONGPRESS)
    {
      g_btn_lp_seen = 1;
    }
  else if (ev == BOARD_BTN_RELEASE)
    {
      g_btn_press_held = held_ms;
    }

  g_btn_ev_seq++;
}

/****************************************************************************
 * Name: step_buttons
 *
 * Description:
 *   第 4 步：看板级按键模块（PA11=KEY / PA34=HOME）的事件，按了就打印
 *   事件类型和按住时长；等到超时正常结束 —— 没人按也算 PASS
 *   （这一步只证明按键**能读**，不能证明它没坏）。
 *
 *   以前这里读的是 /dev/buttons（poll + read），那个路径在真机上会让整机
 *   静默卡死；现在走 sf32lb52_boardbtn 的 GPIO 轮询，见它的头文件说明。
 *
 ****************************************************************************/

static int step_buttons(int timeout_ms)
{
  uint32_t t0;
  int seen = 0;
  int events;
  int ret;

  printf("[4/6] 按键 %s / %s（板级 GPIO，不走 /dev/buttons）\n",
         board_btn_name(BOARD_BTN_KEY), board_btn_name(BOARD_BTN_HOME));

  ret = board_btn_init();
  if (ret < 0)
    {
      printf("      board_btn_init 失败: %d\n", ret);
      report("按键读取", 0, "board_btn_init 失败（起不了轮询任务）");
      return -1;
    }

  hw_test_btn_reset();
  if (board_btn_set_callback(hw_test_btn_cb, NULL) < 0)
    {
      report("按键读取", 0, "board_btn_set_callback 失败");
      return -1;
    }

  printf("      最多等 %d 秒，按一下 %s 就能看到事件（没人按也算 PASS）\n",
         timeout_ms / 1000, board_btn_name(BOARD_BTN_KEY));

  t0 = mono_ms();
  while ((int)(mono_ms() - t0) < timeout_ms)
    {
      if (seen < g_btn_ev_seq)
        {
          seen = g_btn_ev_seq;
          printf("      事件 %d: %s %s  held=%u ms\n",
                 seen, board_btn_name(g_btn_ev_btn),
                 board_btn_event_name(g_btn_ev_type),
                 (unsigned)g_btn_ev_held);
        }

      usleep(BTN_POLL_MS * 1000);
    }

  events = g_btn_ev_seq;
  board_btn_set_callback(NULL, NULL);

  if (events == 0)
    {
      report("按键读取", 1, "超时未按键（这不算失败）");
    }
  else
    {
      char detail[32];

      snprintf(detail, sizeof(detail), "%d 个事件", events);
      report("按键读取", 1, detail);
    }

  return 0;
}

/****************************************************************************
 * Name: step_button_wait
 *
 * Description:
 *   button 子命令：真的等一次按键按下（默认 15 秒）。
 *
 *   与默认自检里的 step_buttons() 不同：这里"没按到"是**失败**。
 *   默认自检那一步只证明"按键能读"，所以没人按也算 PASS；要验证按键
 *   本身必须用本命令，按到才 PASS。
 *
 *   按下由板级按键模块的回调通知（回调跑在按键任务上下文里，只置标志），
 *   主任务每 BTN_POLL_MS 醒一次看有没有新事件，所以这里**不碰任何设备节点**。
 *
 ****************************************************************************/

static int step_button_wait(int seconds)
{
  uint32_t t0;
  int seen = 0;
  int ret;

  if (seconds < 1)
    {
      seconds = BTN_DEFAULT_WAIT_SEC;
    }

  printf("[BUTTON] 板级 GPIO 按键（%s / %s）等一次按下（最多 %d 秒）\n",
         board_btn_name(BOARD_BTN_KEY), board_btn_name(BOARD_BTN_HOME),
         seconds);

  ret = board_btn_init();
  if (ret < 0)
    {
      printf("      board_btn_init 失败: %d\n", ret);
      report("按键", 0, "board_btn_init 失败（起不了轮询任务）");
      return -1;
    }

  hw_test_btn_reset();
  if (board_btn_set_callback(hw_test_btn_cb, NULL) < 0)
    {
      report("按键", 0, "board_btn_set_callback 失败");
      return -1;
    }

  printf("      请按一下 %s 或 %s ...\n",
         board_btn_name(BOARD_BTN_KEY), board_btn_name(BOARD_BTN_HOME));

  t0 = mono_ms();

  while ((int)(mono_ms() - t0) < seconds * 1000)
    {
      if (seen < g_btn_ev_seq)
        {
          seen = g_btn_ev_seq;
          printf("      事件 %d: %s %s  held=%u ms\n",
                 seen, board_btn_name(g_btn_ev_btn),
                 board_btn_event_name(g_btn_ev_type),
                 (unsigned)g_btn_ev_held);
        }

      if (g_btn_press_seen)
        {
          char detail[96];

          snprintf(detail, sizeof(detail), "%s 按下（%d ms 后检测到%s）",
                   board_btn_name(g_btn_press_which), (int)(mono_ms() - t0),
                   g_btn_lp_seen ? "，含长按" : "");
          report("按键", 1, detail);

          board_btn_set_callback(NULL, NULL);
          return OK;
        }

      usleep(BTN_POLL_MS * 1000);
    }

  board_btn_set_callback(NULL, NULL);

  printf("      等了 %d ms 没检测到按下（两个脚一直是松开）\n",
         (int)(mono_ms() - t0));
  report("按键", 0, "超时，没检测到按下");
  return -1;
}

/****************************************************************************
 * Name: gpio_pintype
 *
 * Description:
 *   读 GPIOIOC_GETPINTYPE，失败返回 -1。宏名随 NuttX 版本略有不同，
 *   这里做兼容判断。
 *
 ****************************************************************************/

static int gpio_pintype(int fd)
{
#if defined(GPIOIOC_GETPINTYPE)
  enum gpio_pintype_e pintype = (enum gpio_pintype_e)-1;

  if (ioctl(fd, GPIOIOC_GETPINTYPE, (unsigned long)&pintype) < 0)
    {
      return -1;
    }

  return (int)pintype;
#else
  /* 旧内核只有 GPIOIOC_CONFIG/GET/SET，没有 pintype 查询 */

  return -1;
#endif
}

static FAR const char *gpio_pintype_name(int pintype)
{
  switch (pintype)
    {
      case GPIO_INPUT_PIN:
        return "input";
      case GPIO_OUTPUT_PIN:
        return "output";
      case GPIO_INTERRUPT_BOTH_PIN:
        return "interrupt-both";
      default:
        return "unknown";
    }
}

static int gpio_read_value(int fd, FAR bool *value)
{
  return read(fd, value, 1) == 1 ? OK : -1;
}

/****************************************************************************
 * Name: step_gpio
 *
 * Description:
 *   第 5 步：枚举 /dev/gpio0..2，打印引脚类型和当前电平（只读）。
 *   带 gpio 参数时对 /dev/gpio1（PA26，板级输出脚）做一次电平翻转。
 *
 ****************************************************************************/

static int step_gpio(int do_toggle)
{
  static FAR const char *const paths[3] =
  {
    GPIO_IN_DEV, GPIO_OUT_DEV, GPIO_INT_DEV
  };
  int ok = 1;
  int i;

  printf("[5/6] GPIO %s %s %s\n", GPIO_IN_DEV, GPIO_OUT_DEV, GPIO_INT_DEV);

  for (i = 0; i < 3; i++)
    {
      bool value = false;
      int pintype;
      int fd;

      fd = open(paths[i], O_RDONLY);
      if (fd < 0)
        {
          printf("      %-12s open 失败: %d\n", paths[i], errno);
          ok = 0;
          continue;
        }

      pintype = gpio_pintype(fd);
      if (gpio_read_value(fd, &value) == OK)
        {
          printf("      %-12s pintype=%s(%d) value=%d\n", paths[i],
                 gpio_pintype_name(pintype), pintype, (int)value);
        }
      else
        {
          printf("      %-12s pintype=%s(%d) value=读取失败\n", paths[i],
                 gpio_pintype_name(pintype), pintype);
          ok = 0;
        }

      close(fd);
    }

  if (ok)
    {
      report("GPIO 只读检查", 1, NULL);
    }
  else
    {
      report("GPIO 只读检查", 0, "见上面的失败项");
    }

  if (!do_toggle)
    {
      printf("      提示：要测输出脚电平翻转请跑 `hw_test gpio`\n");
      return ok ? OK : -1;
    }

  /* 输出脚电平翻转：只碰 /dev/gpio1（PA26，板级唯一的 GPIO 输出脚） */

  {
    bool value = false;
    int fd = open(GPIO_OUT_DEV, O_RDWR);

    if (fd < 0)
      {
        printf("      %s open 失败: %d\n", GPIO_OUT_DEV, errno);
        report("GPIO 输出翻转", 0, "open 失败");
        return -1;
      }

#if defined(GPIOIOC_SETPINTYPE)
    if (ioctl(fd, GPIOIOC_SETPINTYPE, (unsigned long)GPIO_OUTPUT_PIN) < 0)
      {
        printf("      SETPINTYPE(output) 失败（板级 bringup 已配成输出，继续）\n");
      }
#elif defined(GPIOIOC_CONFIG)
    if (ioctl(fd, GPIOIOC_CONFIG, (unsigned long)GPIO_OUTPUT_PIN) < 0)
      {
        printf("      CONFIG(output) 失败（板级 bringup 已配成输出，继续）\n");
      }
#endif

    value = true;
    if (write(fd, &value, 1) != 1)
      {
        printf("      写 1 失败: %d\n", errno);
        close(fd);
        report("GPIO 输出翻转", 0, "write 失败");
        return -1;
      }

    if (gpio_read_value(fd, &value) == OK)
      {
        printf("      %s 置 1 -> 回读 %d\n", GPIO_OUT_DEV, (int)value);
      }
    else
      {
        printf("      %s 置 1（回读失败）\n", GPIO_OUT_DEV);
      }

    value = false;
    if (write(fd, &value, 1) != 1)
      {
        printf("      写 0 失败: %d，电平可能停在 1\n", errno);
        close(fd);
        report("GPIO 输出翻转", 0, "写 0 失败");
        return -1;
      }

    if (gpio_read_value(fd, &value) == OK)
      {
        printf("      %s 置 0 -> 回读 %d\n", GPIO_OUT_DEV, (int)value);
      }
    else
      {
        printf("      %s 置 0（回读失败）\n", GPIO_OUT_DEV);
      }

    close(fd);
    report("GPIO 输出翻转", 1, "PA26 已回到 0");
  }

  return OK;
}

/****************************************************************************
 * Name: step_imu
 *
 * Description:
 *   imu 子命令：读 LSM6DS3 的加速度 / 陀螺 / 温度。
 *
 *   本板在树里的 LSM6DSL 驱动是 **NuttX 老式字符驱动**
 *   （nuttx/drivers/sensors/lsm6dsl.c，走 register_driver(devpath, ...)），
 *   注册出来的节点就是 "/dev/lsm6dsl0"，接口是：
 *     ioctl(fd, SNIOC_START)                 -- 开始转换（写 CTRL1_XL/CTRL2_G）
 *     ioctl(fd, SNIOC_LSM6DSLSENSORREAD, &s) -- 一次拿到 acc/gyro/temp/timestamp
 *     read(fd, buf, len)                     -- 只要 acc，len/6 个 int16 xyz 原始码
 *     ioctl(fd, SNIOC_STOP)                  -- 停转换
 *   它**不是** uORB 传感器（没有 /dev/uorb/sensor_accel0），
 *   所以这里没有用 orb_subscribe()。详见 docs/sensor_rtc_usage.md。
 *
 ****************************************************************************/

static int step_imu(int frames)
{
  printf("[IMU] LSM6DS3 %s\n", IMU_DEV);

#ifndef HW_TEST_HAS_IMU
  (void)frames;
  printf("      本板**没有加速度计/陀螺**：模组 SF32LB52-MOD-1 的 BOM 里\n");
  printf("      只有 MCU + 128Mb NOR Flash + 晶振 + 天线，没有任何 IMU 器件。\n");
  printf("      所以这不是\"驱动没编\"，是硬件不存在，开了也读不到。\n");
  printf("      /dev/lsm6dsl0 不会出现；详见 docs/sensor_rtc_usage.md。\n");
  report("IMU 读数", 0, "本板无 IMU 硬件");
  return -1;
#else
  struct lsm6dsl_sensor_data_s sdata;
  int got = 0;
  int fd;
  int i;

  if (frames <= 0)
    {
      frames = IMU_DEFAULT_FRAMES;
    }

  fd = open(IMU_DEV, O_RDONLY);
  if (fd < 0)
    {
      printf("      open 失败: %d\n", errno);
      report("IMU 读数", 0, "open /dev/lsm6dsl0 失败");
      return -1;
    }

  /* 注册时驱动只做了 WHO_AM_I 校验；不 START 就是 POWER_DOWN，读出来全是 0 */

  if (ioctl(fd, SNIOC_START, 0) < 0)
    {
      printf("      SNIOC_START 失败: %d（IMU 没焊 / 地址不对？）\n", errno);
      close(fd);
      report("IMU 读数", 0, "SNIOC_START 失败");
      return -1;
    }

  for (i = 0; i < frames; i++)
    {
      memset(&sdata, 0, sizeof(sdata));

      if (ioctl(fd, SNIOC_LSM6DSLSENSORREAD, (unsigned long)&sdata) < 0)
        {
          printf("      第 %d 帧读取失败: %d\n", i + 1, errno);
          break;
        }

      /* 单位：acc 是 mg（驱动内部已按 ±16g 的 0.488 mg/LSB 换算好），
       *       gyro 是 mdps（±2000dps 的 70 mdps/LSB），
       *       temp 是摄氏度，timestamp 是传感器自己的计数器（不是毫秒）。
       */

      printf("      #%-2d acc=(%5d,%5d,%5d) mg  gyro=(%6d,%6d,%6d) mdps  "
             "temp=%d C  ts=%u\n",
             i + 1,
             (int)sdata.x_data, (int)sdata.y_data, (int)sdata.z_data,
             (int)sdata.g_x_data, (int)sdata.g_y_data, (int)sdata.g_z_data,
             (int)sdata.temperature, (unsigned)sdata.timestamp);
      got++;
      usleep(IMU_FRAME_MS * 1000);
    }

  ioctl(fd, SNIOC_STOP, 0);
  close(fd);

  if (got == frames)
    {
      char detail[32];

      snprintf(detail, sizeof(detail), "%d 帧", got);
      report("IMU 读数", 1, detail);
      return OK;
    }

  printf("      只读到 %d/%d 帧\n", got, frames);
  report("IMU 读数", 0, "读取中途失败，见上面");
  return -1;
#endif
}

/****************************************************************************
 * Name: rtc_alarm_handler
 *
 * Description:
 *   RTC alarm 到点后，rtc upper half 用 nxsig_notification() 给任务发信号，
 *   这里只置一个标志位，真正的等待在主循环里带超时做。
 *
 ****************************************************************************/

static void rtc_alarm_handler(int signo)
{
  (void)signo;
  g_rtc_alarm = 1;
}

/****************************************************************************
 * Name: step_rtc
 *
 * Description:
 *   rtc 子命令：读当前时间 -> 设一个 N 秒后的 alarm -> 等它触发（带超时）。
 *
 *   关键事实（都从源码核实过，别按 Linux 直觉写）：
 *   - /dev/rtc0 由 board 的 rtc_initialize(0, ...) 建出来
 *     （sifli_ap.c:391 -> nuttx/drivers/timers/rtc.c:853）。
 *   - rtc upper half 的 read() 直接 return 0（EOF），**不会阻塞等 alarm**
 *     （rtc.c:318-321）；而且 g_rtc_fops 里 poll 是 NULL（rtc.c:136）。
 *   - alarm 到点走的是 **信号**：ioctl 参数里带 struct sigevent，
 *     upper half 用 nxsig_notification() 通知 pid（rtc.c:198-199）。
 *
 ****************************************************************************/

static int step_rtc(int alarm_sec)
{
  struct rtc_setrelative_s rel;
  struct rtc_rdalarm_s query;
  struct sigaction sa;
  struct rtc_time rt;
  clock_t t0;
  uint32_t elapsed_ms;
  int fd;

  printf("[RTC] %s\n", RTC_DEV);

  if (alarm_sec < 1)
    {
      alarm_sec = RTC_DEFAULT_ALARM_SEC;
    }

  fd = open(RTC_DEV, O_RDONLY);
  if (fd < 0)
    {
      printf("      open 失败: %d\n", errno);
      report("打开 RTC", 0, "open /dev/rtc0 失败");
      return -1;
    }

  memset(&rt, 0, sizeof(rt));
  if (ioctl(fd, RTC_RD_TIME, (unsigned long)&rt) < 0)
    {
      printf("      RTC_RD_TIME 失败: %d\n", errno);
      report("读 RTC 时间", 0, "RTC_RD_TIME 失败");
      close(fd);
      return -1;
    }

  /* tm_year 是从 1900 起的年数，tm_mon 是 0..11（和 struct tm 完全一样） */

  printf("      当前时间 : %04d-%02d-%02d %02d:%02d:%02d\n",
         rt.tm_year + 1900, rt.tm_mon + 1, rt.tm_mday,
         rt.tm_hour, rt.tm_min, rt.tm_sec);

  if (rt.tm_year < 100)
    {
      printf("      提示：RTC 时间看着没设过（年 %d），"
             "先用 RTC_SET_TIME 或 NSH 的 `date -s` 对时\n",
             rt.tm_year + 1900);
    }

  report("读 RTC 时间", 1, NULL);

  /* 用 SIGUSR1 + 标志位等 alarm；主循环有超时，绝不永久卡住 */

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = rtc_alarm_handler;
  sigemptyset(&sa.sa_mask);

  if (sigaction(SIGUSR1, &sa, NULL) < 0)
    {
      printf("      sigaction(SIGUSR1) 失败: %d\n", errno);
      report("RTC alarm", 0, "装信号处理失败");
      close(fd);
      return -1;
    }

  g_rtc_alarm = 0;

  memset(&rel, 0, sizeof(rel));
  rel.id                 = 0;
  rel.pid                = 0;          /* 0 = 通知调用者自己 */
  rel.event.sigev_notify = SIGEV_SIGNAL;
  rel.event.sigev_signo  = SIGUSR1;
  rel.reltime            = alarm_sec;  /* 相对当前 RTC 时间的秒数 */

  if (ioctl(fd, RTC_SET_RELATIVE, (unsigned long)&rel) < 0)
    {
      printf("      RTC_SET_RELATIVE(%d 秒) 失败: %d\n", alarm_sec, errno);
      report("RTC alarm", 0, "RTC_SET_RELATIVE 失败");
      close(fd);
      return -1;
    }

  printf("      已设 %d 秒后的 alarm，最多等 %d 秒...\n",
         alarm_sec, alarm_sec + RTC_WAIT_SLACK_SEC);

  /* 计时必须用单调时钟。原来这里是"每轮 usleep(50ms) 就把 waited_ms 加 50"，
   * 循环体本身的开销不计入，板子一忙（app 初始化/网络重连）就系统性偏小
   * （实测：设 3 秒报 1600ms、设 5 秒报 3500ms），这种读数会让人误以为
   * "alarm 提前触发了"。 */

  t0 = clock_systime_ticks();
  elapsed_ms = 0;

  while (!g_rtc_alarm &&
         elapsed_ms < (alarm_sec + RTC_WAIT_SLACK_SEC) * 1000)
    {
      usleep(50 * 1000);
      elapsed_ms = (uint32_t)TICK2MSEC(clock_systime_ticks() - t0);
    }

  if (!g_rtc_alarm)
    {
      ioctl(fd, RTC_CANCEL_ALARM, 0);
      printf("      超时：等了 %u ms 没收到 SIGUSR1\n",
             (unsigned)elapsed_ms);
      report("RTC alarm", 0, "超时未收到 alarm");
      close(fd);
      return -1;
    }

  printf("      收到 SIGUSR1：alarm 触发了（%u ms，设定 %d 秒）\n",
         (unsigned)elapsed_ms, alarm_sec);

  /* 注意：RTC_RD_ALARM 只能拿到**时间**字段。HAL_RTC_GetAlarm() 只读 ALRMTR、
   * 不读 ALRMDR（bf0_hal_rtc.c 的 GetAlarm 实现），所以闹钟的日/月/年
   * 永远是结构体里的 0，打出来是 "2000-00-00"。这里只打时间，别误导。 */

  memset(&query, 0, sizeof(query));
  query.id = 0;
  if (ioctl(fd, RTC_RD_ALARM, (unsigned long)&query) == 0)
    {
      printf("      RTC_RD_ALARM: active=%d 时间 %02d:%02d:%02d"
             "（日期字段 HAL 不填，见源码注释）\n",
             (int)query.active,
             query.time.tm_hour, query.time.tm_min, query.time.tm_sec);
    }

  report("RTC alarm", 1, "alarm 已触发");
  close(fd);
  return OK;
}

/****************************************************************************
 * Name: rtcday_cb
 *
 * Description:
 *   rtcday 子命令的回调。**它在每日提醒模块的工作线程里执行**，
 *   所以这里只置标志 + 打印，绝不做 sleep / 等锁之类的阻塞动作，
 *   更不能在这里播 TTS 或碰 LVGL（这是模块头文件里明确写的约束）。
 *
 ****************************************************************************/

static void rtcday_cb(FAR void *arg)
{
  (void)arg;

  g_rtcday_calls++;
  g_rtcday_fired = 1;

  printf("      [RTCDAY] 每日提醒回调触发（模块工作线程上下文）\n");
}

/****************************************************************************
 * Name: step_rtcday
 *
 * Description:
 *   rtcday 子命令：注册一个"每天 hh:mm 到点回调一次"的提醒，然后带超时
 *   地等一次回调（最多 RTCDAY_MAX_WAIT_MS，绝不永久卡住）。不传参数时用
 *   "当前时间 + 1 分钟"，这样不用等到明天也能验。
 *
 *   注意：全板只有 1 个 RTC alarm 槽，所以这和 `hw_test rtc` 不能同时跑；
 *   本命令结束前一定会 rtc_alarm_cancel() 收尾，把槽还回去。
 *
 ****************************************************************************/

static int step_rtcday(int hour, int minute, int time_given)
{
  struct rtc_time now;
  clock_t   t0;
  uint32_t  elapsed_ms;
  int       valid;
  int       ret;

  printf("[RTCDAY] 每日 RTC 提醒 %s\n", RTC_DEV);

  memset(&now, 0, sizeof(now));
  ret = rtc_alarm_now(&now);
  if (ret < 0)
    {
      printf("      读 RTC 时间失败: %d\n", ret);
      report("读 RTC 时间", 0, "rtc_alarm_now 失败");
      return -1;
    }

  printf("      当前时间 : %04d-%02d-%02d %02d:%02d:%02d\n",
         now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
         now.tm_hour, now.tm_min, now.tm_sec);
  report("读 RTC 时间", 1, NULL);

  /* 只打印"有没有对时"，不计 PASS/FAIL：没对时不是本命令的错，
   * 而是现场必须先 `date -s`（NSH 只认 "MMM DD HH:MM:SS YYYY" 格式）。 */

  valid = rtc_alarm_time_valid();
  printf("      时间已对时 : %s（rtc_alarm_time_valid()=%d）\n",
         valid ? "是" : "否，RTC 还没对时；模块会每 30 秒重试等对时", valid);

  if (!time_given)
    {
      hour   = now.tm_hour;
      minute = now.tm_min + 1;
      if (minute >= 60)
        {
          minute -= 60;
          hour = (hour + 1) % 24;
        }

      printf("      未指定时刻：用当前时间 + 1 分钟 = %02d:%02d\n",
             hour, minute);
    }

  if (hour < 0 || hour > 23 || minute < 0 || minute > 59)
    {
      printf("      时刻非法: %02d:%02d（应在 00:00..23:59）\n", hour, minute);
      report("每日提醒", 0, "时刻参数非法");
      return -1;
    }

  if (valid)
    {
      int now_min  = now.tm_hour * 60 + now.tm_min;
      int want_min = hour * 60 + minute;

      printf("      下一个提醒 : %s %02d:%02d\n",
             now_min >= want_min ? "明天" : "今天", hour, minute);
    }
  else
    {
      printf("      下一个提醒 : 待 RTC 对时后才能确定（对时前模块不排 alarm）\n");
    }

  g_rtcday_fired = 0;
  g_rtcday_calls = 0;

  ret = rtc_alarm_at_daily(hour, minute, rtcday_cb, NULL);
  if (ret != OK)
    {
      printf("      rtc_alarm_at_daily 失败: %d\n", ret);
      report("注册每日提醒", 0, "rtc_alarm_at_daily 失败");
      return -1;
    }

  report("注册每日提醒", 1, "已交给模块工作线程排 alarm");
  printf("      等回调（最多 %d 秒）...\n", RTCDAY_MAX_WAIT_MS / 1000);

  /* 计时用单调时钟：clock_systime_ticks() + TICK2MSEC()，
   * 不用"每轮 sleep 就累加固定值"（工程刚修过这个计时 bug）。 */

  t0 = clock_systime_ticks();
  elapsed_ms = 0;

  while (!g_rtcday_fired && elapsed_ms < RTCDAY_MAX_WAIT_MS)
    {
      usleep(RTCDAY_POLL_MS * 1000);
      elapsed_ms = (uint32_t)TICK2MSEC(clock_systime_ticks() - t0);
    }

  /* 不管成功还是超时都收尾：别让 worker 一直占着唯一的 alarm 槽 */

  (void)rtc_alarm_cancel();

  if (!g_rtcday_fired)
    {
      printf("      超时：等了 %u ms 没收到回调\n", (unsigned)elapsed_ms);
      report("每日提醒", 0, "超时未回调（已 rtc_alarm_cancel 收尾）");
      return -1;
    }

  printf("      等了 %u ms 收到回调（共 %d 次）\n",
         (unsigned)elapsed_ms, g_rtcday_calls);
  report("每日提醒", 1, "回调已触发");
  return OK;
}

/****************************************************************************
 * Name: audio_reader_task
 *
 * Description:
 *   audio 子命令的读任务：audio_in_read() 会阻塞到读满，或被
 *   audio_in_stop() 发出的 AUDIOIOC_STOP 唤醒。
 *   按 AUDIO_CHUNK_BYTES（1 秒）分块读，避免踩驱动下层 5 秒的 read 超时。
 *
 ****************************************************************************/

static int audio_reader_task(int argc, FAR char *argv)
{
  int offset = 0;

  (void)argc;
  (void)argv;

  while (offset < g_arec_len)
    {
      int chunk = g_arec_len - offset;
      ssize_t n;

      if (chunk > AUDIO_CHUNK_BYTES)
        {
          chunk = AUDIO_CHUNK_BYTES;
        }

      n = audio_in_read((FAR char *)g_arec_buf + offset, chunk);
      if (n <= 0)
        {
          /* <=0 必须跳出：0 = 被 STOP 打断或下层 5 秒超时（不是
           * "再读一次就有数据"），负值 = 未 start / fd 已失效。 */
          break;
        }

      offset += (int)n;
    }

  g_arec_n    = offset;
  g_arec_done = 1;
  return 0;
}

/****************************************************************************
 * Name: audio_level
 *
 * Description:
 *   统计 peak / avg，判断有没有声音。返回 peak。
 *
 ****************************************************************************/

static int audio_level(FAR const int16_t *buf, int nsamples)
{
  int peak = 0;
  long sum = 0;
  int i;

  for (i = 0; i < nsamples; i++)
    {
      int v = buf[i];

      if (v < 0)
        {
          v = -v;
        }

      if (v > peak)
        {
          peak = v;
        }

      sum += v;
    }

  printf("      peak=%d avg=%ld (16k mono 16bit)\n",
         peak, nsamples > 0 ? sum / nsamples : 0);
  printf("      声音检测 : %s\n",
         peak > AUDIO_SOUND_PEAK ? "有声音" : "静音（麦克风没信号/没说话）");
  return peak;
}

/****************************************************************************
 * Name: step_audio
 *
 * Description:
 *   audio 子命令：录 N 秒到内存，打印 peak/avg，不写文件。
 *
 *   走的是板级封装 sf32lb52_audio_in（audio_in_start/read/stop），它内部
 *   就是 app/audio_test 真机验证过的那套接口：
 *     open(/dev/audio/audio0, O_RDONLY)
 *     ioctl(AUDIOIOC_CONFIGURE, AUDIO_TYPE_INPUT 16k mono 16bit)
 *     ioctl(AUDIOIOC_START) -> read() 阻塞读满 -> ioctl(AUDIOIOC_STOP)
 *   所以这一步同时也是对那个封装的自检。
 *
 *   阻塞的 audio_in_read() 放独立任务，主任务带超时；超时就调
 *   audio_in_stop()（内部发 AUDIOIOC_STOP，驱动已修：STOP 能唤醒阻塞中的
 *   read），保证不永久卡住。
 *
 ****************************************************************************/

static int step_audio(int seconds)
{
  FAR int16_t *buf;
  int nsamples;
  clock_t t0;
  uint32_t elapsed_ms;
  int ret;

  printf("[AUDIO] %s 录音 %d 秒\n", AUDIO_DEV, seconds);

  if (seconds < 1)
    {
      seconds = AUDIO_DEFAULT_SEC;
    }

  nsamples = AUDIO_SAMPLE_RATE * seconds;
  buf = (FAR int16_t *)malloc((size_t)nsamples * sizeof(int16_t));
  if (buf == NULL)
    {
      printf("      malloc %d 字节失败\n", nsamples * 2);
      report("录音", 0, "内存不足");
      return -1;
    }

  /* open + CONFIGURE + START 都收在封装里，失败看返回的负 errno */

  ret = audio_in_start(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BITS);
  if (ret < 0)
    {
      printf("      audio_in_start(%d, %d, %d) 失败: %d\n",
             AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BITS, ret);
      printf("      （录音封装的用法见 docs/audio_driver_usage.md 第 9 节）\n");
      report("启动录音", 0, "audio_in_start 失败");
      free(buf);
      return -1;
    }

  g_arec_buf  = buf;
  g_arec_len  = nsamples * 2;
  g_arec_done = 0;
  g_arec_n    = -999;

  if (task_create("hwtest_rec", 100, AUDIO_READ_TASK_STACK,
                  (main_t)audio_reader_task, NULL) < 0)
    {
      printf("      task_create 失败: %d\n", errno);
      audio_in_stop();
      report("录音", 0, "起读任务失败");
      free(buf);
      return -1;
    }

  /* 计时同样用单调时钟（理由见 step_rtc 里的注释） */

  t0 = clock_systime_ticks();
  elapsed_ms = 0;

  while (!g_arec_done &&
         elapsed_ms < (uint32_t)(seconds + AUDIO_WAIT_SLACK_SEC) * 1000)
    {
      usleep(100 * 1000);
      elapsed_ms = (uint32_t)TICK2MSEC(clock_systime_ticks() - t0);
    }

  if (!g_arec_done)
    {
      /* audio_in_stop() 里的 AUDIOIOC_STOP 能让阻塞在 read() 里的任务返回 */

      audio_in_stop();
      t0 = clock_systime_ticks();
      while (!g_arec_done &&
             (uint32_t)TICK2MSEC(clock_systime_ticks() - t0) < 1000)
        {
          usleep(100 * 1000);
        }

      printf("      read 没在 %d 秒内返回，已调 audio_in_stop()\n",
             seconds + AUDIO_WAIT_SLACK_SEC);
      report("录音", 0, g_arec_done ? "read 被 STOP 唤醒" : "read 卡住");

      if (g_arec_done)
        {
          free(buf);
        }
      else
        {
          /* 读任务还活着：**故意不释放缓冲**。它可能仍挂在驱动的 read()
           * 里持有这块地址，一 free 就是 use-after-free。
           * fd 已由 audio_in_stop() 关闭，读任务下次进 audio_in_read()
           * 会直接拿到 -EINVAL 跳出，不会再往这块内存写。
           * 一次失败的自检，漏一块缓冲是可以接受的代价。 */
          printf("      （读任务还活着，故意不释放缓冲，避免它醒来写已释放内存）\n");
        }

      return -1;
    }

  printf("      read 返回 %zd 字节（期望 %d）\n", g_arec_n, nsamples * 2);
  audio_in_stop();

  if (g_arec_n <= 0)
    {
      report("录音", 0, "read 返回 <= 0");
      free(buf);
      return -1;
    }

  {
    int peak = audio_level(buf, (int)(g_arec_n / 2));
    char detail[64];

    snprintf(detail, sizeof(detail), "%d 字节, peak=%d%s",
             (int)g_arec_n, peak,
             peak > AUDIO_SOUND_PEAK ? " 有声音" : " 静音");
    report("录音", 1, detail);
  }

  free(buf);
  return OK;
}

/****************************************************************************
 * kws 子命令：唤醒词「你好，openvela」/「Hello，openvela」的命令词识别
 *            （MFCC + DTW，模块本体在 app/hello_app/kws_dtw.c）
 *
 * 模块本身是纯计算：不开麦、不起线程、不碰设备。所以"录模板"这一步必须由
 * 外面来做 —— 就是这里：用板级录音封装（sf32lb52_audio_in，和 audio 子命令
 * 同一套）把 PCM 录出来交给 kws_enroll()。**本子命令是唯一的录模板入口**，
 * 没有它，唤醒词功能等于开不了。
 *
 * 模板约定（kws_dtw.h）：/data/kws/slotN.tpl，N = 0..3，
 *   slot0 =「你好，openvela」、slot1 =「Hello，openvela」（slot2/3 空着备用）。
 * ⚠ /data 是 tmpfs：模板重启就丢，长期保留得把它搬走或重新录。
 *
 * ⚠ 麦克风是半双工、整机又是单一大镜像：ai_companion 开机自启后会一直持有
 *   麦克风（听唤醒词），而且它和这里共享 kws_dtw 的全局状态（模块没有锁，
 *   见 .h 的"线程安全"）。所以跑 kws 子命令之前**必须先停掉 ai_companion**，
 *   否则 audio_in_start() 直接返回 -EBUSY，并且两边同时调 KWS 接口会互相踩。
 ****************************************************************************/

#ifdef HW_TEST_HAS_KWS

/* 槽位个数必须和 kws_dtw.h 的 KWS_MAX_TEMPLATES 一致（下面按槽位写死了提示词） */

#if KWS_MAX_TEMPLATES != 4
#  error "kws 槽位提示词表是 4 条，KWS_MAX_TEMPLATES 改了要一起改"
#endif

/****************************************************************************
 * Name: kws_slot_word
 *
 * Description:
 *   slot 对应的唤醒词（方便提示用户"这一槽该念什么"）。
 *   slot2/slot3 是 .h 里留的实验槽（比如改成只念 "openvela"），没有固定词。
 *
 ****************************************************************************/

static FAR const char *kws_slot_word(int slot)
{
  switch (slot)
    {
      case 0:
        return "你好，openvela";

      case 1:
        return "Hello，openvela";

      case 2:
        return "（自定义槽位，想录什么就录什么）";

      case 3:
        return "（自定义槽位，想录什么就录什么）";

      default:
        return "（未知槽位）";
    }
}

/****************************************************************************
 * Name: kws_mic_acquire
 *
 * Description:
 *   把麦克风拿到手：audio_in_start()，成功返回 OK 并**保持持有**，调用方
 *   负责在收尾前 audio_in_stop()（幂等）。
 *
 *   为什么要单独一步、而且要排在所有 kws_dtw 调用之前：
 *   kws_dtw 的全局状态（预滚环 / 待判句子 / 自适应本底 / 冷却、模板、以及
 *   selftest 和判定共用的 DTW 暂存）正是 ai_companion 录音线程在用的，而
 *   kws_init() 一进去就 kws_reset() + 重载模板、kws_enroll() 还会再 reset。
 *   以前是"先动 kws 再去开麦"，所以 ai_companion 还跑着时敲一嗓子
 *   enroll/selftest，会把正在跑的唤醒词链路先搞坏，最后才拿到 -EBUSY。
 *   反过来先开麦就干净：驱动只放行"持有者已消失的残留会话"，能开成 = 现在
 *   确实没有别的会话在用麦克风；开不成 = 整个子命令 FAIL，一个 kws_dtw
 *   接口都不碰，正在跑的唤醒词链路一个字节都不会被动到。
 *
 *   ⚠ 拿不到时 audio_in_start() **不会**发 AUDIOIOC_STOP（板级封装的约定：
 *     对面还活着就不打断），所以这条路径也不会踩别人正在录/正在放的通路。
 *
 * Returned Value:
 *   OK = 麦克风已持有（调用方必须 audio_in_stop）；-1 = 拿不到（原因已打印）。
 *
 ****************************************************************************/

static int kws_mic_acquire(void)
{
  int ret;

  ret = audio_in_start(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BITS);

  if (ret < 0)
    {
      printf("      audio_in_start(%d, %d, %d) 失败: %d\n",
             AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BITS, ret);
      printf("      （-EBUSY = 麦克风被别人占着：ai_companion 开机自启后会一直"
             "持有它；\n");
      printf("        跑 kws 子命令前先停掉它 —— 这里到这一步就收手，"
             "不会去碰 kws_dtw）\n");
      return -1;
    }

  return OK;
}

/****************************************************************************
 * Name: kws_reader_task
 *
 * Description:
 *   kws 子命令的读任务：按 KWS_READ_CHUNK_BYTES（100ms）分块读。
 *   live 模式顺手在**本任务**里喂 kws_feed（模块没有锁，只允许一个线程调它，
 *   所以喂帧和 kws_enroll 都在这个线程里，主任务只管计时/收尾）。
 *
 ****************************************************************************/

static int kws_reader_task(int argc, FAR char *argv)
{
  int offset = 0;

  (void)argc;
  (void)argv;

  while (offset < g_krec_len)
    {
      int chunk = g_krec_len - offset;
      FAR int16_t *dst;
      ssize_t n;

      if (chunk > KWS_READ_CHUNK_BYTES)
        {
          chunk = KWS_READ_CHUNK_BYTES;
        }

      if (g_krec_live)
        {
          dst = g_krec_live_buf;
        }
      else
        {
          dst = g_krec_buf + offset / 2;    /* offset 是字节，缓冲按采样点算 */
        }

      n = audio_in_read((FAR char *)dst, (size_t)chunk);

      if (n <= 0)
        {
          /* <=0 必须跳出：0 = 被 STOP 打断或下层 5 秒超时（不是"再读一次
           * 就有数据"），负值 = fd 已失效（没 start / 已被 stop）。 */

          g_krec_err = (int)n;
          break;
        }

      /* kws_feed 的负返回值只有"没初始化(-ENOSYS)/参数非法(-EINVAL)"两种，
       * 这里前面一定 kws_init 过、参数也是固定的合法 buffer，所以不会发生
       * （真发生了也只是这一路不喂帧，不影响录音/还设备）。 */

      if (g_krec_live && kws_feed(dst, (size_t)n / 2) == 1)
        {
          /* kws_feed 命中时它自己会打一条 "[KWS] 命中唤醒词（slotN…）"，
           * 这里再补一条带"第几次/第几毫秒"的，方便当验收判据 */

          g_krec_hits++;
          printf("      [命中] 第 %d 次（已录 %d ms）\n", g_krec_hits,
                 (int)((long)offset * 1000 / (AUDIO_SAMPLE_RATE * 2)));
        }

      offset += (int)n;
    }

  g_krec_got  = offset;
  g_krec_done = 1;
  return 0;
}

/****************************************************************************
 * Name: kws_record
 *
 * Description:
 *   kws 子命令的录音。live != 0 时边读边喂 kws_feed（不攒 PCM）；
 *   live == 0 时攒进一块 malloc 的缓冲，由调用方拿去 kws_enroll。
 *   超时救场和缓冲归属的判断与 step_audio 完全一致（同一套驱动的坑）：
 *   read 没在预期时间内返回就 audio_in_stop()（它会唤醒阻塞在 read 里的任务），
 *   读任务真卡死时**故意不释放缓冲**（免得它醒来写已释放内存）。
 *
 *   mic_held != 0：麦克风已经由 kws_mic_acquire() 拿到手了，本函数**不再
 *   audio_in_start()**（同线程重入会被板级封装判成 -EBUSY），直接开始读。
 *   这样调用方就能把"确认拿到麦克风"这一步提到所有 kws_dtw 调用之前。
 *
 *   ⚠ 麦克风是单一大镜像里共享的一份全局：**只要麦克风是我们的，本函数每个
 *     return 之前都调了 audio_in_stop()**（幂等），一次泄漏就会把后面所有
 *     录音都锁死。反过来，start 本身失败的路径**故意不调 stop** —— 那意味着
 *     这次会话不是我们的，stop 会把别人（比如 ai_companion）正在录的会话
 *     一起 STOP + close 掉；start 自己的失败路径已经把它 open 出来的 fd 关干净。
 *
 * Returned Value:
 *   0 = 录满（*pcm_out 拿到 malloc 的缓冲，调用方负责 free；live 模式给 NULL）；
 *   -1 = 失败（原因已经打印，设备已经还给板级封装）。
 *
 ****************************************************************************/

static int kws_record(int seconds, int live, int mic_held,
                      FAR int16_t **pcm_out)
{
  int nsamples = AUDIO_SAMPLE_RATE * seconds;
  FAR int16_t *buf = NULL;
  clock_t t0;
  uint32_t elapsed_ms;
  int ret;

  *pcm_out = NULL;

  if (!live)
    {
      buf = (FAR int16_t *)malloc((size_t)nsamples * sizeof(int16_t));
      if (buf == NULL)
        {
          printf("      malloc %d 字节失败（把录音秒数调小一点再试）\n",
                 nsamples * 2);

          /* 这条路径在 start 之前，但 mic_held 模式下设备是调用方交到我们
           * 手上的 —— 必须在这儿还回去，漏一次后面所有录音就都别想开了 */

          if (mic_held)
            {
              audio_in_stop();
            }

          return -1;
        }
    }

  if (!mic_held)
    {
      ret = audio_in_start(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BITS);
      if (ret < 0)
        {
          printf("      audio_in_start(%d, %d, %d) 失败: %d\n",
                 AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BITS, ret);
          printf("      （-EBUSY = 麦克风被别人占着：ai_companion 开机自启后会一直"
                 "持有它，跑 kws 子命令前先停掉它）\n");
          free(buf);
          return -1;
        }
    }

  /* 到这里麦克风才是我们的：下面每个 return 之前都必须 audio_in_stop() */

  g_krec_buf  = buf;
  g_krec_len  = nsamples * 2;
  g_krec_live = live;
  g_krec_done = 0;
  g_krec_got  = 0;
  g_krec_err  = 0;
  g_krec_hits = 0;

  if (task_create("kws_rec", 100, KWS_READ_TASK_STACK,
                  (main_t)kws_reader_task, NULL) < 0)
    {
      printf("      task_create 失败: %d\n", errno);
      audio_in_stop();
      free(buf);
      return -1;
    }

  /* 计时同样用单调时钟（理由见 step_rtc 里的注释） */

  t0 = clock_systime_ticks();
  elapsed_ms = 0;

  while (!g_krec_done &&
         elapsed_ms < (uint32_t)(seconds + AUDIO_WAIT_SLACK_SEC) * 1000)
    {
      usleep(100 * 1000);
      elapsed_ms = (uint32_t)TICK2MSEC(clock_systime_ticks() - t0);
    }

  if (!g_krec_done)
    {
      /* audio_in_stop() 里的 AUDIOIOC_STOP 能让阻塞在 read() 里的任务返回 */

      audio_in_stop();
      t0 = clock_systime_ticks();
      while (!g_krec_done &&
             (uint32_t)TICK2MSEC(clock_systime_ticks() - t0) < 1000)
        {
          usleep(100 * 1000);
        }

      printf("      read 没在 %d 秒内返回，已调 audio_in_stop()\n",
             seconds + AUDIO_WAIT_SLACK_SEC);

      if (g_krec_done)
        {
          free(buf);
        }
      else
        {
          /* 读任务还活着：**故意不释放缓冲**。它可能仍挂在驱动的 read()
           * 里持有这块地址，一 free 就是 use-after-free。
           * fd 已由 audio_in_stop() 关闭，读任务下次进 audio_in_read()
           * 会直接拿到 -EINVAL 跳出，不会再往这块内存写。
           * 一次失败的自检，漏一块缓冲是可以接受的代价。 */
          printf("      （读任务还活着，故意不释放缓冲，避免它醒来写已释放内存）\n");
        }

      return -1;
    }

  /* 读任务已经结束（正常或 read 出错），把设备还回去 */

  audio_in_stop();
  g_krec_buf = NULL;

  if (g_krec_got < g_krec_len)
    {
      printf("      只读到 %d/%d 字节（read 返回 %d）\n", (int)g_krec_got,
             g_krec_len, (int)g_krec_err);
      free(buf);
      return -1;
    }

  *pcm_out = buf;
  return 0;
}

/****************************************************************************
 * Name: step_kws_enroll
 *
 * Description:
 *   `hw_test kws enroll <slot> [秒]`：录一段麦克风音频交给 kws_enroll()
 *   做成模板，并落盘到 /data/kws/slotN.tpl。
 *
 *   参数（非法一律打中文提示 + FAIL，绝不越界写）：
 *     slot   必给，0..KWS_MAX_TEMPLATES-1（越界或没给都直接 FAIL）；
 *     秒数   可选，默认 KWS_ENROLL_DEFAULT_SEC，钳到
 *            [KWS_ENROLL_MIN_SEC, KWS_ENROLL_MAX_SEC]。
 *
 *   顺序**必须是**：参数检查 → kws_mic_acquire() → kws_init() → 倒计时 →
 *   录音（kws_record，麦已在手）→ kws_enroll()。麦克风拿到之前不碰任何
 *   kws_dtw 接口，拿不到就整条 FAIL（理由见 kws_mic_acquire）。
 *   录音期间设备一直握在手里，kws_record 返回时已经 audio_in_stop() 还回去了。
 *
 * Returned Value:
 *   OK = 模板已经在 RAM 里（落盘成功与否见打印）；
 *   -1 = 参数非法 / 拿不到麦克风 / 初始化失败 / 录音失败 / kws_enroll 失败。
 *
 ****************************************************************************/

static int step_kws_enroll(int slot, int seconds)
{
  FAR int16_t *pcm = NULL;
  char detail[128];                 /* 失败原因里带中文，留够（避免 snprintf 截在
                                     * 一个多字节字符中间变成乱码） */
  int peak;
  int got;
  int n;
  int ret;
  int i;

  if (slot < 0)
    {
      printf("hw_test kws enroll: 缺 slot 参数。用法 "
             "hw_test kws enroll <slot> [秒]，slot 取 0..%d"
             "（slot0 =「你好，openvela」、slot1 =「Hello，openvela」）\n",
             KWS_MAX_TEMPLATES - 1);
      report("唤醒词录模板", 0, "缺 slot 参数");
      return -1;
    }

  if (slot >= KWS_MAX_TEMPLATES)
    {
      printf("hw_test kws enroll: slot %d 越界，只允许 0..%d"
             "（slot0 =「你好，openvela」、slot1 =「Hello，openvela」）\n",
             slot, KWS_MAX_TEMPLATES - 1);
      report("唤醒词录模板", 0, "slot 越界（只允许 0..3）");
      return -1;
    }

  if (seconds < KWS_ENROLL_MIN_SEC || seconds > KWS_ENROLL_MAX_SEC)
    {
      int clamp = seconds;

      if (clamp < KWS_ENROLL_MIN_SEC)
        {
          clamp = KWS_ENROLL_MIN_SEC;
        }

      if (clamp > KWS_ENROLL_MAX_SEC)
        {
          clamp = KWS_ENROLL_MAX_SEC;
        }

      printf("      提示：录音秒数 %d 不在 %d..%d 里，按 %d 秒录\n",
             seconds, KWS_ENROLL_MIN_SEC, KWS_ENROLL_MAX_SEC, clamp);
      seconds = clamp;
    }

  printf("[KWS] 录模板 slot%d：请念「%s」；录 %d 秒（%d Hz/单声道/16bit）\n",
         slot, kws_slot_word(slot), seconds, AUDIO_SAMPLE_RATE);
  printf("      前提：麦克风必须空闲 —— ai_companion 开机自启后会一直占着它\n");
  printf("            （半双工 + 单一大镜像），跑之前先停掉 ai_companion\n");

  /* 先把麦克风拿到手，再碰 kws_dtw：kws_init() 会 kws_reset()，kws_enroll()
   * 还会再 reset 一次，而 ai_companion 的录音线程正在用那些全局状态。
   * 拿不到就在这里收手（一个 kws 接口都不调），而不是数完 3 秒才发现 -EBUSY。 */

  if (kws_mic_acquire() < 0)
    {
      report("KWS 麦克风", 0, "拿不到麦克风（先停 ai_companion）");
      return -1;
    }

  ret = kws_init();
  if (ret < 0)
    {
      audio_in_stop();
      printf("      kws_init() 失败: %d（内部表建不起来，属异常）\n", ret);
      report("KWS 模块初始化", 0, "kws_init 失败");
      return -1;
    }

  snprintf(detail, sizeof(detail), "模板 %d 条，目录 " KWS_DATA_DIR,
           kws_ready_count());
  report("KWS 模块初始化", 1, detail);

  /* 给用户一点准备时间（录音是"数完才开始"的，免得开头几个字被吃掉） */

  printf("      请准备：%d 秒后开始录音，然后把这一整句清楚地念一遍\n",
         KWS_PREP_SEC);

  for (i = KWS_PREP_SEC; i > 0; i--)
    {
      printf("      %d...\n", i);
      usleep(1000 * 1000);
    }

  printf("      >>> 开始录音，请说：「%s」 <<<\n", kws_slot_word(slot));

  ret = kws_record(seconds, 0, 1, &pcm);    /* mic_held=1：麦已经在手里 */
  if (ret < 0)
    {
      report("KWS 录音", 0, "录音失败（原因见上面）");
      return -1;
    }

  got  = (int)(g_krec_got / 2);             /* 采样点个数 */
  peak = audio_level(pcm, got);             /* 打印 peak/avg + 有没有声音 */

  snprintf(detail, sizeof(detail), "%d 采样点（%dms），peak=%d%s", got,
           got * 1000 / AUDIO_SAMPLE_RATE, peak,
           peak > AUDIO_SOUND_PEAK ? " 有声音" : " 静音");
  report("KWS 录音", 1, detail);

  /* 特征提取 + 端点检测走的是识别那一套（kws_dtw.h 保证口径一致） */

  ret = kws_enroll(pcm, (size_t)got, slot);
  n   = kws_template_frames(slot);

  if (ret == 0)
    {
      snprintf(detail, sizeof(detail),
               "%d 个特征点（%dms），已落盘 " KWS_DATA_DIR "/slot%d.tpl",
               n, n * KWS_FEAT_MS, slot);
      report("唤醒词模板", 1, detail);
      printf("      提示：跑 `hw_test kws test` 看全部槽位状态，"
             "`hw_test kws live 10` 当场试唤醒\n");
    }
  else if (ret == 1)
    {
      /* 模板进 RAM 了，只是 /data 写失败：现在能用，重启会丢（kws_dtw.h
       * 明确说落盘失败不算致命，所以这里算 PASS，但要把话说明白） */

      snprintf(detail, sizeof(detail),
               "%d 个特征点（%dms），但没落盘（重启会丢）",
               n, n * KWS_FEAT_MS);
      report("唤醒词模板", 1, detail);
      printf("      注意：本次唤醒词已经能用（模板在 RAM 里），但写 " KWS_DATA_DIR
             " 失败（原因见上面 [KWS] 行）—— 重启后要重录\n");
    }
  else
    {
      FAR const char *why;

      if (ret == -EINVAL)
        {
          why = "有效语音太短/太安静（整句念完整、前后留一点安静再试）";
        }
      else if (ret == -ENOSPC)
        {
          why = "有效语音超过 2.0 秒上限（短语说短一点）";
        }
      else if (ret == -ENOSYS)
        {
          why = "模块没初始化";
        }
      else
        {
          why = "kws_enroll 返回负值";
        }

      snprintf(detail, sizeof(detail), "kws_enroll=%d：%s", ret, why);
      report("唤醒词模板", 0, detail);
    }

  free(pcm);

  return (ret == 0 || ret == 1) ? OK : -1;
}

/****************************************************************************
 * Name: step_kws_test
 *
 * Description:
 *   `hw_test kws test`：打印当前已登记的模板数、判定阈值、每个槽位是否可用
 *   （数据都来自 kws_dtw.c 的查询接口）。
 *
 *   没有模板算 FAIL —— 那种情况下 kws_feed() 永远不会返回 1，唤醒词等于没开。
 *
 ****************************************************************************/

static int step_kws_test(void)
{
  char detail[96];
  int ready;
  int s;
  int n;
  int ret;

  printf("[KWS] 模板状态（目录 %s，约定 %d Hz/单声道/16bit）\n",
         KWS_DATA_DIR, AUDIO_SAMPLE_RATE);

  ret = kws_init();
  if (ret < 0)
    {
      printf("      kws_init() 失败: %d（内部表建不起来，属异常）\n", ret);
      report("KWS 模块初始化", 0, "kws_init 失败");
      return -1;
    }

  ready = kws_ready_count();

  printf("      阈值          : %d（每维每帧 RMS 距离 ×1000；"
         "kws_set_threshold 可改）\n", kws_get_threshold());
  printf("      一个特征点    : %dms\n", KWS_FEAT_MS);

  /* 这里不打印 kws_get_last_distance()：kws_init() 内部会 kws_reset()，
   * 那个值必然是 -1（要看"差多少"请用 kws live，它跑完会打出来） */

  for (s = 0; s < KWS_MAX_TEMPLATES; s++)
    {
      n = kws_template_frames(s);

      if (n > 0)
        {
          printf("      slot%d : 可用 —— %d 个特征点（%dms），应念「%s」\n",
                 s, n, n * KWS_FEAT_MS, kws_slot_word(s));
        }
      else
        {
          printf("      slot%d : 空   —— 应念「%s」\n", s, kws_slot_word(s));
        }
    }

  snprintf(detail, sizeof(detail), "模板 %d/%d 条可用，阈值 %d", ready,
           KWS_MAX_TEMPLATES, kws_get_threshold());
  report("唤醒词模板", ready > 0, detail);

  if (ready == 0)
    {
      printf("      提示：还没有模板，kws_feed 永远不会返回 1 —— 先录一条：\n");
      printf("            hw_test kws enroll 0 4   （念「你好，openvela」）\n");
      printf("            hw_test kws enroll 1 4   （念「Hello，openvela」）\n");
    }
  else
    {
      printf("      提示：/data 是 tmpfs —— 模板重启就丢，"
             "重启后要重录（或从别处拷回 " KWS_DATA_DIR "）\n");
    }

  return ready > 0 ? OK : -1;
}

/****************************************************************************
 * Name: step_kws_live
 *
 * Description:
 *   `hw_test kws live [秒]`：边录音边实时喂 kws_feed，命中就打印一行。
 *   用途是**在没有语音应用的情况下单独验收唤醒词** —— 因为它不依赖
 *   ai_companion/LVGL，只看"说了唤醒词会不会报命中"。
 *
 *   ⚠ 只能在 ai_companion 没跑的时候用：它开机自启时会独占麦克风（半双工），
 *     而且和这里共享 KWS 的全局状态。使用提示里也打了这句话。
 *   和 enroll 一样先 kws_mic_acquire()：拿不到麦克风就整条 FAIL，
 *   kws_init()/kws_reset() 一个都不调（否则先把正在跑的唤醒词链路清掉了，
 *   最后才拿到 -EBUSY）。
 *
 ****************************************************************************/

static int step_kws_live(int seconds)
{
  char detail[96];
  FAR int16_t *pcm = NULL;
  int hits;
  int ret;

  if (seconds < 1 || seconds > KWS_LIVE_MAX_SEC)
    {
      int clamp = (seconds < 1) ? KWS_LIVE_DEFAULT_SEC : KWS_LIVE_MAX_SEC;

      printf("      提示：听音秒数 %d 不在 1..%d 里，按 %d 秒听\n",
             seconds, KWS_LIVE_MAX_SEC, clamp);
      seconds = clamp;
    }

  printf("[KWS] 实时听 %d 秒：请说「你好，openvela」或「Hello，openvela」\n",
         seconds);
  printf("      注意：本板半双工 + 整机单一大镜像 —— ai_companion 开机自启后\n");
  printf("            会一直独占麦克风，所以本命令只能在它没跑的时候用，\n");
  printf("            否则 audio_in_start() 直接失败 -EBUSY。\n");
  printf("      命中延迟：说完最后一个字后还要等 ~300ms 的尾静音才判（正常）\n");

  /* 先拿麦克风：拿不到就一个 kws_dtw 接口都不碰（见 kws_mic_acquire） */

  if (kws_mic_acquire() < 0)
    {
      report("KWS 麦克风", 0, "拿不到麦克风（先停 ai_companion）");
      return -1;
    }

  ret = kws_init();
  if (ret < 0)
    {
      audio_in_stop();
      printf("      kws_init() 失败: %d（内部表建不起来，属异常）\n", ret);
      report("KWS 模块初始化", 0, "kws_init 失败");
      return -1;
    }

  report("KWS 模块初始化", 1, "见上面的 [KWS] 初始化行");

  if (kws_ready_count() <= 0)
    {
      audio_in_stop();
      printf("      还没有模板（kws_feed 永远不会返回 1）—— 先跑 "
             "hw_test kws enroll 0 4\n");
      report("唤醒词实时听音", 0, "还没有模板（先 kws enroll）");
      return -1;
    }

  /* 交给过别人（ASR）之后再听，预滚环/自适应本底/冷却都是旧的，清一下 */

  kws_reset();

  ret = kws_record(seconds, 1, 1, &pcm);    /* mic_held=1：麦已经在手里 */
  hits = (int)g_krec_hits;

  if (ret < 0)
    {
      report("唤醒词实时听音", 0, "录音失败（原因见上面）");
      return -1;
    }

  snprintf(detail, sizeof(detail), "命中 %d 次 / 听 %d 秒，最近距离 %d（阈值 %d）",
           hits, seconds, kws_get_last_distance(), kws_get_threshold());
  report("唤醒词实时听音", hits > 0, detail);

  if (hits == 0)
    {
      printf("      没命中怎么查：距离 -1 = 那句话被长度差挡在 DTW 带宽外；\n");
      printf("      距离明显大于阈值 = 模板和现场口音/音量差得远，重录一次模板\n");
    }
  else
    {
      printf("      命中过 %d 次 —— 唤醒词功能在这台板子上转起来了\n", hits);
    }

  return hits > 0 ? OK : -1;
}

/****************************************************************************
 * Name: step_kws_selftest
 *
 * Description:
 *   `hw_test kws selftest`：跑 kws_dtw.c 的自检 —— 它拿合成 1kHz 查 FFT/Mel
 *   表，再拿模板 0 自己跟自己、跟"时间拉伸 1.25 倍"的副本、跟"帧序打乱"的
 *   假句子算 DTW 距离，最后打印**推荐阈值**（推荐值 = 同句侧上限和结构打乱侧
 *   下限的中点，有第二条模板再往"不同词"那侧拉一半）。
 *
 *   自检本身只吃合成数据 + /data 里的模板，不录音；但模块没初始化时它直接
 *   返回 KWS_ERR_STATE，所以这里要 kws_init()（它顺便把模板读进来）。
 *
 *   ⚠ 会 kws_reset()（kws_init 内部）—— 会和正在听的 ai_companion 抢模块的
 *     流式状态，所以这里**先** kws_mic_acquire() 确认麦克风没人用：拿不到就
 *     整条 FAIL、一个 kws_dtw 接口都不碰。确认到了立刻 audio_in_stop() 还回去
 *     （本子命令不录音，开机只为拿到"没有别的会话在跑"这个证据）。
 *
 *   返回 OK / -1：每个失败项的原因由 kws_selftest() 自己 printf，这里只把它
 *   折成 PASS/FAIL 记进总账。**没有模板时不报 PASS**：那种情况下自检会跳过
 *   第 2/3 项、没有推荐阈值，标定根本做不下去 —— 报 PASS 会误导现场。
 *
 ****************************************************************************/

static int step_kws_selftest(void)
{
  char detail[96];
  int ready;
  int ret;

  /* 先确认麦克风没人用，再碰 kws_dtw（见 kws_mic_acquire） */

  if (kws_mic_acquire() < 0)
    {
      report("KWS 麦克风", 0, "拿不到麦克风（先停 ai_companion）");
      return -1;
    }

  ret = kws_init();
  if (ret < 0)
    {
      audio_in_stop();
      printf("      kws_init() 失败: %d（内部表建不起来，属异常）\n", ret);
      report("KWS 模块初始化", 0, "kws_init 失败");
      return -1;
    }

  /* 麦只是拿来当"没有别的会话"的证据，后面的自检全是纯计算，早点还回去 */

  audio_in_stop();

  ret   = kws_selftest();
  ready = kws_ready_count();            /* kws_dtw.h 的现成接口：可用模板条数 */

  if (ret != 0)
    {
      printf("      自检有失败项（原因见上面的 [KWS] 行）\n");
      snprintf(detail, sizeof(detail), "有失败项（当前阈值 %d）",
               kws_get_threshold());
      report("唤醒词自检", 0, detail);
      return -1;
    }

  if (ready == 0)
    {
      /* 没有模板时 kws_selftest() 跳过第 2/3 项、fails 仍是 0 → 返回 0。
       * 那个 0 只说明"能查的几项没查出问题"，不代表唤醒词能用：没有模板就
       * 没有推荐阈值，标定无从谈起（kws_feed 也永远不会返回 1）。 */

      printf("      无法自检：一条模板都没有 —— 自检会跳过「模板自比对」和"
             "「互距离」两项，\n");
      printf("      也就算不出推荐阈值，标定做不下去。先录一条：\n");
      printf("            hw_test kws enroll 0 4   （念「你好，openvela」）\n");
      snprintf(detail, sizeof(detail), "没有模板，无法自检（先 kws enroll 0）");
      report("唤醒词自检", 0, detail);
      return -1;
    }

  printf("      提示：推荐阈值在上面的 [KWS] 推荐阈值 那一行，"
         "当场改就 `hw_test kws threshold <值>`\n");

  snprintf(detail, sizeof(detail), "自检全通过（当前阈值 %d）",
           kws_get_threshold());
  report("唤醒词自检", 1, detail);

  return OK;
}

/****************************************************************************
 * Name: step_kws_threshold
 *
 * Description:
 *   `hw_test kws threshold <值|default>`：当场改判定阈值（现场标定的入口）。
 *   单位是"每维每帧 RMS 距离 ×1000"，默认 KWS_THRESHOLD_MILLI = 1800；
 *   参数写 `default`（大小写敏感，和子命令同一个小写风格）就是恢复 1800 ——
 *   免去为了回默认重启一次（重启还会把刚标定的一起清掉）。
 *
 *   这里**故意不调 kws_init()**：阈值就是 kws_dtw.c 里那个全局变量，同一
 *   镜像里 ai_companion 和这里共享它 —— 不初始化就等于不碰流式状态，所以
 *   可以在 ai_companion 正常跑着（正在听唤醒词）的时候改，改完下一次判定
 *   就用新值。标定时本来就该这么用：一边试唤醒，一边调阈值。
 *
 *   值合法不合法交给 kws_set_threshold() 自己判（它会 printf 原因，
 *   非法的直接忽略），这里只按"读回来的值是不是我要的那个数"判断有没有生效。
 *
 *   额外多一道闸：**大于 KWS_TH_MAX_SAFE（9690）时本子命令直接拒绝设置**，
 *   报 FAIL 并说清"这个值会让任何输入都判命中"，阈值保持原来的不动。
 *   理由：kws_dtw.c 那边只警告不拒绝（标定的人可能故意），但这里是给人手敲
 *   的入口，`1800` 多打一个 0 就静默把唤醒词变成"随便什么都唤醒" —— 现场
 *   演示时这种"改完还报 PASS"的事故代价太大。真想试饱和区，得走模块接口。
 *
 *   ⚠ 只改 RAM：kws_dtw.c 不落盘，重启回默认 —— 打印里明确说了这句。
 *
 ****************************************************************************/

static int step_kws_threshold(int th)
{
  char detail[96];
  int old;
  int now;

  old = kws_get_threshold();

  if (th > KWS_TH_MAX_SAFE)
    {
      printf("      拒绝设置：%d 大于 %d（DTW 饱和距离的下限）——\n",
             th, KWS_TH_MAX_SAFE);
      printf("      那个区间里距离被 DTW 的上限钉住，**任何输入都会被判命中**，\n");
      printf("      也就是唤醒词变成「随便什么都唤醒」（阈值实际失效）。\n");
      printf("      阈值保持 %d 不变；要回默认值用："
             "hw_test kws threshold default\n", old);
      snprintf(detail, sizeof(detail), "%d 会让任何输入都命中，已拒绝（仍是 %d）",
               th, old);
      report("唤醒词阈值", 0, detail);
      return -1;
    }

  kws_set_threshold(th);
  now = kws_get_threshold();

  if (now != th)
    {
      printf("      设置失败：阈值还是 %d（模块拒了这个值，"
             "原因见上面 [KWS] 那行）\n", now);
      snprintf(detail, sizeof(detail), "%d 被拒绝（仍是 %d）", th, now);
      report("唤醒词阈值", 0, detail);
      return -1;
    }

  printf("      阈值 %d -> %d（每维每帧 RMS 距离 ×1000）\n", old, now);
  printf("      注意：只对本次运行有效 —— kws_dtw 不落盘，重启回到默认 %d，"
         "要长期生效得把 %d 写进代码/开机脚本\n", KWS_THRESHOLD_MILLI, now);
  printf("      提示：越高越容易命中、也越容易误唤醒；当场看效果就读"
         " ai_companion 打的\n");
  printf("            `[KWS] 命中唤醒词` 那行日志；要单独听唤醒词"
         "（kws live）得先停掉\n");
  printf("            ai_companion，否则 audio_in_start 直接 -EBUSY\n");

  snprintf(detail, sizeof(detail), "%d -> %d（重启回 %d）", old, now,
           KWS_THRESHOLD_MILLI);
  report("唤醒词阈值", 1, detail);

  return OK;
}

/****************************************************************************
 * Name: step_kws
 *
 * Description:
 *   kws 子命令的入口（main 里只认 cmd/slot/seconds/threshold 四个数）。
 *   没编 hello_app 的配置下只打印一句"本配置不支持"。
 *
 ****************************************************************************/

static int step_kws(int cmd, int slot, int seconds, int threshold)
{
  switch (cmd)
    {
      case KWS_CMD_ENROLL:
        return step_kws_enroll(slot, seconds);

      case KWS_CMD_TEST:
        return step_kws_test();

      case KWS_CMD_LIVE:
        return step_kws_live(seconds);

      case KWS_CMD_SELFTEST:
        return step_kws_selftest();

      case KWS_CMD_THRESHOLD:
        return step_kws_threshold(threshold);

      default:
        return -1;
    }
}

#else  /* !HW_TEST_HAS_KWS */

static int step_kws(int cmd, int slot, int seconds, int threshold)
{
  (void)cmd;
  (void)slot;
  (void)seconds;
  (void)threshold;

  printf("[KWS] 本配置没有启用 hello_app"
         "（CONFIG_LVX_USE_CONTEST2026_233_HELLO_APP），"
         "命令词识别模块 kws_dtw.c 不在固件里\n");
  report("唤醒词自检", 0, "编译时未启用");
  return -1;
}

#endif /* HW_TEST_HAS_KWS */

/****************************************************************************
 * Name: audio_out_play
 *
 * Description:
 *   tts 子命令的播放：直接开 /dev/audio/audio0 写 PCM。
 *   ioctl 顺序照 app/audio_test 与 app/hello_app/ai_audio.c 真机验证过的那套：
 *     open -> CONFIGURE(AUDIO_TYPE_OUTPUT 16k/1ch/16bit)
 *          -> CONFIGURE(AUDIO_TYPE_FEATURE + AUDIO_FU_VOLUME)
 *          -> START -> write(100ms 一块) -> STOP -> close
 *
 *   本板是半双工（AUDIOIOC_STOP 会把录放两条通路一起停），所以这里只放不录。
 *   音量下发失败不当致命错误：NuttX 上层没实现 AUDIOIOC_SETVOLUME，
 *   板级 alarm 模块 / audio_test 走的也是这个 CONFIGURE 口径。
 *
 * Returned Value:
 *   OK = 全部字节都写下去了；-1 = 中途失败（原因见 printf）。
 *
 ****************************************************************************/

static int audio_out_play(FAR const int16_t *pcm, size_t bytes)
{
  struct audio_caps_desc_s capdesc;
  size_t done = 0;
  int fd;

  fd = open(AUDIO_DEV, O_WRONLY);
  if (fd < 0)
    {
      printf("      open %s 失败: %d\n", AUDIO_DEV, errno);
      return -1;
    }

  memset(&capdesc, 0, sizeof(capdesc));
  capdesc.caps.ac_len            = sizeof(struct audio_caps_s);
  capdesc.caps.ac_type           = AUDIO_TYPE_OUTPUT;
  capdesc.caps.ac_channels       = AUDIO_CHANNELS;
  capdesc.caps.ac_controls.hw[0] = AUDIO_SAMPLE_RATE;
  capdesc.caps.ac_controls.b[2]  = AUDIO_BITS;

  if (ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&capdesc) < 0)
    {
      printf("      CONFIGURE(OUTPUT %dHz %dch %dbit) 失败: %d\n",
             AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BITS, errno);
      close(fd);
      return -1;
    }

  memset(&capdesc, 0, sizeof(capdesc));
  capdesc.caps.ac_len            = sizeof(struct audio_caps_s);
  capdesc.caps.ac_type           = AUDIO_TYPE_FEATURE;
  capdesc.caps.ac_format.hw      = AUDIO_FU_VOLUME;
  capdesc.caps.ac_controls.hw[0] = PLAY_VOLUME * AUDIO_VOLUME_MAX / 100;

  if (ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&capdesc) < 0)
    {
      printf("      音量 %d 下发失败: %d（继续，用驱动当前音量）\n",
             PLAY_VOLUME, errno);
    }

  if (ioctl(fd, AUDIOIOC_START, 0) < 0)
    {
      printf("      START 失败: %d\n", errno);
      close(fd);
      return -1;
    }

  while (done < bytes)
    {
      size_t chunk = bytes - done;
      ssize_t n;

      if (chunk > PLAY_CHUNK_BYTES)
        {
          chunk = PLAY_CHUNK_BYTES;
        }

      n = write(fd, (FAR const char *)pcm + done, chunk);
      if (n <= 0)
        {
          printf("      write 失败（已写 %zu/%zu 字节）: %d\n", done, bytes,
                 errno);
          break;
        }

      done += (size_t)n;
    }

  /* 顺序和录音一样：先 STOP 再 close（STOP 顺带停掉录音通路，半双工） */

  ioctl(fd, AUDIOIOC_STOP, 0);
  close(fd);

  printf("      写完 %zu/%zu 字节\n", done, bytes);
  return done == bytes ? OK : -1;
}

/****************************************************************************
 * Name: step_tts
 *
 * Description:
 *   tts 子命令：注册 MiMo TTS 后端 -> voice_tts_speak() 合成 -> 打印字节数
 *   -> 用板级音频通路放出来。
 *
 *   合成走云端（HTTPS + TLS），所以必须在有网、且 /data/ai_agent/config/
 *   config.json 里有 llm_host + api_key 时才通（配置来源见 agent_config.h）。
 *
 ****************************************************************************/

static int step_tts(FAR const char *text)
{
#ifndef HW_TEST_HAS_VOICE
  (void)text;

  printf("[TTS] 本配置没有同时启用 hello_app 和 ai_agent，MiMo TTS 不可用\n");
  report("MiMo TTS", 0, "编译时未启用");
  return -1;
#else
  FAR unsigned char *pcm;
  char detail[64];
  size_t pcm_len = 0;
  int ret;

  printf("[TTS] MiMo 云端合成: \"%s\"\n", text);

  /* 后端注册只要一次：NuttX 是 flat 地址空间，同一个 app 反复跑
   * （`hw_test tts a` 跑几次）会往分发层的静态表里重复登记，表满（4 个）
   * 之后 register 返回 -ENOMEM。所以先 set_backend 试一下，没找到才注册。 */

  if (voice_tts_set_backend("mimo") != OK)
    {
      if (mimo_tts_register() != OK || voice_tts_set_backend("mimo") != OK)
        {
          printf("      注册/选择 mimo 后端失败"
                 "（配置里有 llm_host + api_key 吗？）\n");
          report("注册 MiMo TTS 后端", 0, "register/set_backend 失败");
          return -1;
        }
    }

  report("注册 MiMo TTS 后端", 1, voice_tts_get_backend());

  pcm = malloc(TTS_PCM_CAP);
  if (pcm == NULL)
    {
      report("MiMo TTS 合成", 0, "缓冲分配失败");
      return -1;
    }

  ret = voice_tts_speak(text, pcm, TTS_PCM_CAP, &pcm_len);
  if (ret != OK || pcm_len == 0)
    {
      snprintf(detail, sizeof(detail), "voice_tts_speak=%d, len=%zu",
               ret, pcm_len);
      report("MiMo TTS 合成", 0, detail);
      free(pcm);
      return -1;
    }

  printf("      合成 %zu 字节 PCM（16kHz/单声道/16bit，约 %zu ms）\n",
         pcm_len, pcm_len * 1000 / (AUDIO_SAMPLE_RATE * 2));

  snprintf(detail, sizeof(detail), "%zu 字节", pcm_len);
  report("MiMo TTS 合成", 1, detail);

  printf("      播放（%s，音量 %d）...\n", AUDIO_DEV, PLAY_VOLUME);
  ret = audio_out_play((FAR const int16_t *)pcm, pcm_len);
  report("播放 TTS 语音", ret == OK, ret == OK ? "已写完" : "播放失败");

  free(pcm);
  return ret;
#endif
}

/****************************************************************************
 * Name: step_asr
 *
 * Description:
 *   asr 子命令：从任意路径读一个 WAV（16bit PCM，采样率不限，内部转 16k）
 *   -> 注册 MiMo ASR 后端 -> voice_asr_recognize() -> 打印识别结果。
 *
 *   文件路径是**板子上**的路径（比如 /data/test.wav 或 U 盘挂载点），
 *   不是 PC 上的路径。
 *
 ****************************************************************************/

static int step_asr(FAR const char *path)
{
#ifndef HW_TEST_HAS_VOICE
  (void)path;

  printf("[ASR] 本配置没有同时启用 hello_app 和 ai_agent，MiMo ASR 不可用\n");
  report("MiMo ASR", 0, "编译时未启用");
  return -1;
#else
  FAR unsigned char *pcm = NULL;
  char text[ASR_TEXT_CAP];
  char detail[64];
  size_t pcm_len = 0;
  int ret;

  printf("[ASR] MiMo 云端识别: %s\n", path);

  ret = mimo_wav_load_16k(path, &pcm, &pcm_len);
  if (ret != OK || pcm == NULL)
    {
      printf("      读 WAV 失败: %d（要 16bit PCM 的 WAV；路径是板子上的，"
             "比如 /data/test.wav）\n", ret);
      report("读 WAV 文件", 0, "见上面");
      return -1;
    }

  printf("      WAV -> %zu 字节 PCM（16kHz/单声道/16bit，约 %zu ms）\n",
         pcm_len, pcm_len * 1000 / (AUDIO_SAMPLE_RATE * 2));

  snprintf(detail, sizeof(detail), "%zu 字节 PCM", pcm_len);
  report("读 WAV 文件", 1, detail);

  /* 同 step_tts：注册只做一次，重复跑 `hw_test asr` 不会把静态表撑满 */

  if (voice_asr_set_backend("mimo") != OK)
    {
      if (mimo_asr_register() != OK || voice_asr_set_backend("mimo") != OK)
        {
          printf("      注册/选择 mimo 后端失败"
                 "（配置里有 llm_host + api_key 吗？）\n");
          report("注册 MiMo ASR 后端", 0, "register/set_backend 失败");
          free(pcm);
          return -1;
        }
    }

  report("注册 MiMo ASR 后端", 1, voice_asr_get_backend());

  memset(text, 0, sizeof(text));
  ret = voice_asr_recognize(pcm, pcm_len, text, sizeof(text));
  free(pcm);

  if (ret != OK)
    {
      snprintf(detail, sizeof(detail), "voice_asr_recognize=%d", ret);
      report("MiMo ASR 识别", 0, detail);
      return -1;
    }

  printf("      识别结果: %s\n", text);
  report("MiMo ASR 识别", text[0] != '\0', text[0] != '\0' ? text : "结果为空");

  return text[0] != '\0' ? OK : -1;
#endif
}

/****************************************************************************
 * Name: alarm_level_name / alarm_event_name
 ****************************************************************************/

static FAR const char *alarm_level_name(enum alarm_level_e level)
{
  switch (level)
    {
      case ALARM_LEVEL_NOTICE:    return "NOTICE";
      case ALARM_LEVEL_WARNING:   return "WARNING";
      case ALARM_LEVEL_EMERGENCY: return "EMERGENCY";
      default:                    return "NONE";
    }
}

static FAR const char *alarm_event_name(enum alarm_event_e event)
{
  switch (event)
    {
      case ALARM_EVENT_TRIGGERED: return "TRIGGERED";
      case ALARM_EVENT_CLEARED:   return "CLEARED";
      case ALARM_EVENT_TIMEOUT:   return "TIMEOUT";
      default:                    return "?";
    }
}

/****************************************************************************
 * Name: alarm_test_cb
 *
 * Description:
 *   alarm 子命令的回调。**它在报警模块的工作线程里执行**，
 *   所以这里只打印 + 计数，绝不做 sleep / 等锁之类的阻塞动作。
 *
 ****************************************************************************/

static void alarm_test_cb(enum alarm_event_e event,
                          FAR const struct alarm_status_s *status,
                          FAR void *arg)
{
  (void)arg;

  g_alarm_events++;
  if (status != NULL)
    {
      g_alarm_last_level = (int)status->level;
    }

  printf("      [ALARM] event=%s level=%s reason=%s\n",
         alarm_event_name(event),
         alarm_level_name(status != NULL ? status->level : ALARM_LEVEL_NONE),
         status != NULL ? status->reason : "");
}

/****************************************************************************
 * Name: step_alarm
 *
 * Description:
 *   alarm 子命令：注册回调 -> 触发报警 -> 保持 N 秒（期间能听到声音）
 *   -> alarm_clear() -> 打印状态并用 report() 给 PASS/FAIL。
 *
 *   计时用 clock_systime_ticks() + TICK2MSEC()，不用"每轮累加固定值"。
 *
 ****************************************************************************/

static int step_alarm(int level_num, int seconds)
{
  struct alarm_status_s st;
  enum alarm_level_e    level;
  clock_t               t0;
  uint32_t              elapsed_ms;

  switch (level_num)
    {
      case 1:  level = ALARM_LEVEL_NOTICE;    break;
      case 2:  level = ALARM_LEVEL_WARNING;   break;
      case 3:  level = ALARM_LEVEL_EMERGENCY; break;
      default: level = ALARM_LEVEL_EMERGENCY; break;
    }

  if (seconds < 1)
    {
      seconds = ALARM_DEFAULT_SEC;
    }

  printf("[ALARM] 级别=%d(%s) 保持 %d 秒\n",
         (int)level, alarm_level_name(level), seconds);

  g_alarm_events     = 0;
  g_alarm_last_level = 0;

  if (alarm_set_callback(alarm_test_cb, NULL) != OK)
    {
      report("注册报警回调", 0, "alarm_set_callback 失败");
      return -1;
    }

  if (alarm_trigger(level, "hw_test", "hw_test alarm 子命令") != OK)
    {
      report("触发报警", 0, "alarm_trigger 失败");
      alarm_set_callback(NULL, NULL);
      return -1;
    }

  report("触发报警", 1, alarm_level_name(level));

  /* 单调时钟计时（同 step_rtc / step_audio，别自己累加固定值） */

  t0 = clock_systime_ticks();
  elapsed_ms = 0;
  while (elapsed_ms < (uint32_t)seconds * 1000)
    {
      usleep(ALARM_POLL_MS * 1000);
      elapsed_ms = (uint32_t)TICK2MSEC(clock_systime_ticks() - t0);
    }

  memset(&st, 0, sizeof(st));
  if (alarm_get_status(&st) == OK)
    {
      printf("      状态: active=%d level=%s repeat=%u reason=%s text=%s\n",
             (int)st.active, alarm_level_name(st.level),
             (unsigned)st.repeat_count, st.reason, st.text);
      report("报警持续", st.active ? 1 : 0,
             st.active ? "仍在报警中" : "提前结束");
    }
  else
    {
      report("读报警状态", 0, "alarm_get_status 失败");
    }

  alarm_clear();

  /* 等一小会，让工作线程把音频 STOP 掉，并回调 CLEARED */

  t0 = clock_systime_ticks();
  while ((uint32_t)TICK2MSEC(clock_systime_ticks() - t0) < ALARM_SETTLE_MS)
    {
      usleep(ALARM_POLL_MS * 1000);
    }

  memset(&st, 0, sizeof(st));
  alarm_get_status(&st);
  printf("      解除后: active=%d level=%s 回调事件=%d 次\n",
         (int)st.active, alarm_level_name(st.level), g_alarm_events);
  report("解除报警", st.active ? 0 : 1, st.active ? "仍在报警" : "已解除");

  report("报警回调", g_alarm_events >= 1 ? 1 : 0,
         g_alarm_events >= 1 ? "收到回调" : "没有收到任何回调");

  alarm_set_callback(NULL, NULL);
  return OK;
}

/****************************************************************************
 * Name: arg_is_number
 *
 * Description:
 *   判断一个命令行参数是不是纯十进制数字。
 *   子命令的可选数值参数用它来决定"要不要吃掉下一个 argv"—— 否则
 *   `hw_test lcd gpio` 会把 atoi("gpio") = 0 当成亮度 0。
 *
 ****************************************************************************/

static bool arg_is_number(FAR const char *s)
{
  if (s == NULL || *s == '\0')
    {
      return false;
    }

  if (*s == '-' || *s == '+')
    {
      s++;
    }

  if (*s == '\0')
    {
      return false;
    }

  for (; *s != '\0'; s++)
    {
      if (*s < '0' || *s > '9')
        {
          return false;
        }
    }

  return true;
}

/****************************************************************************
 * Name: arg_tail_ws / arg_is_word / arg_is_number_ws / arg_copy_trimmed
 *
 * Description:
 *   **串口那条路必须容忍尾随空白**，尤其是 '\r'：PC 端写 nsh 命令时习惯发
 *   "\r\n" 结束一行（板子自己的 _flash/raw_cap.py 也是这么敲回车的），而 NSH
 *   的 readline 只把 '\n' 当行尾 —— 前面那个 '\r' 会留在行缓冲里、粘在
 *   **最后一个参数**后面（`hw_test lcdtap 100 200 1\r\n` 的最后一个参数就成了
 *   "1\r"）。不处理的话，PC 端 --serial 模式下一按鼠标，板子就会回一行
 *   "用法 hw_test lcdtap ..."，看着像命令名写错了。
 *
 *   所以 lcdmirror / lcdtap 这两个子命令的参数判定都走下面这几个（子命令名、
 *   数字都容忍尾随空白；节点名/IP 先拷进本地缓冲再去掉尾随空白）。
 *   其它子命令没这问题：它们是人在终端里敲的，不经过 PC 端那个脚本。
 *
 ****************************************************************************/

static size_t arg_tail_ws(FAR const char *s)
{
  size_t n = strlen(s);

  while (n > 0 &&
         (s[n - 1] == '\r' || s[n - 1] == '\n' ||
          s[n - 1] == ' ' || s[n - 1] == '\t'))
    {
      n--;
    }

  return n;
}

static bool arg_is_word(FAR const char *s, FAR const char *word)
{
  size_t n = arg_tail_ws(s);

  return strlen(word) == n && strncmp(s, word, n) == 0;
}

static bool arg_is_number_ws(FAR const char *s)
{
  size_t n = arg_tail_ws(s);
  size_t i = 0;

  if (n == 0)
    {
      return false;
    }

  if (s[0] == '-' || s[0] == '+')
    {
      if (n == 1)
        {
          return false;
        }

      i = 1;
    }

  for (; i < n; i++)
    {
      if (s[i] < '0' || s[i] > '9')
        {
          return false;
        }
    }

  return true;
}

static void arg_copy_trimmed(FAR char *dst, size_t cap, FAR const char *src)
{
  size_t n = arg_tail_ws(src);

  if (n >= cap)
    {
      n = cap - 1;
    }

  memcpy(dst, src, n);
  dst[n] = '\0';
}

/****************************************************************************
 * Name: step_backlight
 *
 * Description:
 *   lcd 子命令：读当前亮度 -> backlight_set(percent) -> 等一小会 -> 回读，
 *   每步都打印，并用 report() 给 PASS/FAIL。
 *
 *   计时用 clock_systime_ticks() + TICK2MSEC()（同 step_alarm / step_rtc，
 *   别自己累加固定值）。
 *
 *   打了 vendor 补丁后 0..100 都能 PASS；没打补丁的树上 SETCONTRAST 是
 *   -ENOSYS，只有 0 和 100 能 PASS，中间值这里如实报 FAIL（不掩盖）。
 *   详见 docs/display_touch_gpio_usage.md 第 3.3 节。
 *
 ****************************************************************************/

static int step_backlight(int percent)
{
  clock_t  t0;
  uint32_t elapsed_ms;
  int      before;
  int      after;
  int      ret;

  printf("[LCD ] 屏幕亮度 /dev/lcd0，目标 = %d%%\n", percent);

  before = backlight_get();
  if (before < 0)
    {
      printf("      设置前读亮度失败: %d\n", before);
    }
  else
    {
      printf("      设置前: %d%%\n", before);
    }

  t0 = clock_systime_ticks();
  ret = backlight_set(percent);
  elapsed_ms = (uint32_t)TICK2MSEC(clock_systime_ticks() - t0);

  printf("      backlight_set(%d) -> %d，耗时 %u ms\n",
         percent, ret, (unsigned)elapsed_ms);

  if (ret != OK)
    {
      report("设置亮度", 0,
             ret == -ENOSYS
                 ? "本树不支持百分比亮度（vendor 补丁没打？）"
                 : "backlight_set 失败");
      return -1;
    }

  if (percent == 0)
    {
      report("设置亮度", 1, "面板已关（最暗）");
    }
  else if (percent >= BACKLIGHT_MAX_PERCENT)
    {
      report("设置亮度", 1, "面板全亮");
    }
  else
    {
      report("设置亮度", 1, "已下发百分比亮度");
    }

  /* 等一下再回读，别刚下发就去要状态 */

  t0 = clock_systime_ticks();
  while ((uint32_t)TICK2MSEC(clock_systime_ticks() - t0) < BACKLIGHT_SETTLE_MS)
    {
      usleep(BACKLIGHT_POLL_MS * 1000);
    }

  after = backlight_get();
  if (after < 0)
    {
      report("回读亮度", 0, "backlight_get 失败");
      return -1;
    }

  printf("      设置后: %d%%\n", after);
  report("回读亮度", after == percent ? 1 : 0,
         after == percent ? NULL : "回读值与设置值不一致");

  return OK;
}

/****************************************************************************
 * Name: step_panel_reinit
 *
 * Description:
 *   lcdreinit 子命令：面板重新初始化（黑屏救回）。
 *
 *   救的是哪一种黑屏：**整屏黑，但应用完全正常** —— LVGL 还在响应触摸、
 *   还在 20~30 帧/s 往面板推画面（`hw_test status` / [ui] 仪表都能证明），
 *   `hw_test lcd 80` 下发亮度也成功，可屏幕就是不亮；reset 无效，只有真断电
 *   才恢复。那是"面板自己丢了配置"，不是 CPU/LCDC 挂了，所以重发一遍面板
 *   初始化序列就该回来。
 *
 *   这个子命令做的就是：调 robot_ui_bridge_panel_reinit() —— 面板侧重发配置
 *   （含拉一次 RESET 脚）+ 请 LVGL 线程把整屏判脏重推一次。
 *
 *   打印里那句"看屏幕"是给用户看的判断依据：这个命令**没法自己知道**屏幕
 *   亮没亮（面板没有回读"我在显示"的寄存器），所以只能报告下发是否成功，
 *   亮不亮得用户自己看。
 *
 ****************************************************************************/

static int step_panel_reinit(void)
{
  clock_t  t0;
  uint32_t elapsed_ms;
  int      ret;

  printf("[LCD ] 面板重新初始化（整屏黑的救回动作）\n");
  printf("      会做的事：重发一遍面板初始化序列（含拉一次 RESET 脚）"
         "+ 重设像素格式/亮度/DisplayOn，\n");
  printf("                然后请 LVGL 线程把整屏重绘一次。"
         "期间画面会闪一下、触摸停约 0.5 秒，正常。\n");

  t0  = clock_systime_ticks();
  ret = robot_ui_bridge_panel_reinit();
  elapsed_ms = (uint32_t)TICK2MSEC(clock_systime_ticks() - t0);

  printf("      robot_ui_bridge_panel_reinit() -> %d，耗时 %u ms\n",
         ret, (unsigned)elapsed_ms);

  if (ret != OK)
    {
      report("面板重新初始化", 0,
             ret == -ENODEV
                 ? "面板驱动还没绑上（开机 lcd_init 线程没跑完？）"
                 : (ret == -ENOSYS
                        ? "本固件把 SF32LB_LCD_PANEL_REINIT 编成了 0"
                        : "驱动侧拒绝了这次调用（见上面 [Bridge] 日志）"));
      return -1;
    }

  report("面板重新初始化", 1, "初始化序列已下发（亮不亮要看屏幕）");

  printf("      看屏幕：\n");
  printf("        救回来了 -> 1~2 秒内整屏闪一下然后恢复画面，"
         "触摸也恢复响应；\n");
  printf("                   这时不用再敲别的，界面自己会继续刷。\n");
  printf("        没救回来 -> 屏幕仍然全黑。串口里应该能看到\n");
  printf("                   `[Bridge] 面板已重初始化，已投一次全屏重绘`\n");
  printf("                   + 驱动侧的 `panel reinit: 完成，亮度 N%%`；\n");
  printf("                   要是这两行也在、屏幕还是黑的，"
         "那基本是硬件侧（面板供电/排线）。\n");

  return OK;
}

/****************************************************************************
 * Name: step_status
 *
 * Description:
 *   status 子命令：打一份统一外设状态（board_status_get/dump），
 *   并判 PASS/FAIL。
 *
 *   判定规则（和默认自检里 optional 节点的思路一致）：
 *     - **必需**只有一条：网络拿到非回环 IPv4 地址。没地址是 FAIL ——
 *       板子的 MQTT / AI 全链路都依赖它，是"真故障"；
 *     - 其它（/etc/assets、/data、音频、LCD、触摸、RTC、MQTT）**只提示**，
 *       不计入 PASS/FAIL：它们可能被别的模块占着、也可能是配置差异，
 *       拿来判死刑会误报（和 eth0 未枚举、CONFIG_PWM=n 的处理一样）。
 *
 *   本子命令**只读**：board_status_get() 只做 open/close、stat、
 *   写一个 0 字节探针文件（立刻删）、读一次 RTC，不初始化任何设备。
 *
 ****************************************************************************/

static int step_status(void)
{
  struct board_status_s st;
  int ret;

  printf("[STATUS] 统一外设状态（board_status_get）\n\n");

  ret = board_status_get(&st);
  if (ret < 0)
    {
      printf("      board_status_get 失败: %d\n", ret);
      report("状态查询", 0, "board_status_get 失败");
      return -1;
    }

  board_status_dump();
  printf("\n");

  /* 必需项：网络 */

  if (st.net_link_up)
    {
      char detail[24];

      snprintf(detail, sizeof(detail), "%s", st.net_ip);
      report("网络已获取 IPv4 地址", 1, detail);
    }
  else
    {
      report("网络已获取 IPv4 地址", 0,
             "没有非回环 IPv4 地址（USB RNDIS 没枚举起来？）");
    }

  /* 其余只提示，不影响 PASS/FAIL */

  printf("      [提示] ROM 素材 /etc/assets : %s（不计入判定）\n",
         st.rom_assets_ready ? "可读" : "读不到");
  printf("      [提示] /data 可写           : %s（不计入判定）\n",
         st.data_ready ? "是" : "否");
  printf("      [提示] 音频播放 / 录音      : %s / %s（不计入判定）\n",
         st.audio_play_ready ? "可打开" : "打不开",
         st.audio_rec_ready ? "可打开" : "打不开");
  printf("      [提示] 显示 / 触摸          : %s / %s（不计入判定）\n",
         st.lcd_ready ? "可打开" : "打不开",
         st.touch_ready ? "可打开" : "打不开");
  printf("      [提示] 按键 PA11            : %s（不计入判定）\n",
         st.btn_key_pressed ? "按下" : "松开");
  printf("      [提示] RTC 时间             : %s（不计入判定）\n",
         st.rtc_valid ? "有效" : "无效（年份 < 2000）");
  printf("      [提示] MQTT                 : %s（不计入判定）\n",
         st.mqtt_connected ? "已连接"
                           : "未连接（要由网络模块 board_status_set_mqtt 喂）");
  printf("      [提示] 运行时间             : %u s\n", (unsigned)st.uptime_s);

  return OK;
}

/****************************************************************************
 * Name: step_lcdmirror
 *
 * Description:
 *   lcdmirror 子命令：屏幕镜像（板子 -> PC）的开关与状态，
 *   以及"PC 鼠标当触摸"这条反向通道（都跟着镜像任务走）。
 *
 *   `hw_test lcdmirror [start|stop|status|uart|tcp] [ip|节点] [port]`
 *     - 不给子命令、或给 status：打印状态（只读）；会打印**当前传输**、
 *       节点名/目标地址、收到多少条触摸消息、最后坐标、当前是按下还是抬起
 *       —— 现场判断"鼠标到底通没通"看它；
 *     - uart [dev]：切到**控制台串口**那条腿（默认 /dev/console）。帧格式
 *       和 TCP 一模一样，所以 PC 端不用改解析器 —— 只是不再依赖 USB 网络。
 *       **不会自动开**：串口和控制台日志共用一条线，必须手动切；
 *     - tcp <ip> [port]：切回 TCP（顺带换目标地址）；
 *     - start [ip] [port]：可选先换目标地址，再起镜像任务；
 *       **开机本来就是自动起的**，所以正常情况这里只会打印"已经在跑"；
 *     - stop：停掉镜像任务。**鼠标模拟触摸也一起停**（反向通道在镜像是哪个
 *       循环里，镜像不在跑就不再注入），刷屏路径同时完全绕开、一行开销都不留。
 *
 *   ⚠ 本命令**不烧屏、不碰 LVGL**：镜像任务自己开 socket/串口、自己收自己发。
 *   像素是 robot_ui 的 flush 钩子喂进来的，所以只有界面在刷的东西才会出现在
 *   镜像里（比如 `hw_test lcdcolor` 那种直接写 /dev/fb0 的刷色不会进镜像）。
 *   反过来，鼠标注入是喂给 robot_ui 里那个虚拟输入设备的，也只有界面在跑才
 *   点得动。
 *
 *   协议是 **v2**（见 board/contest_board/src/lcd_mirror.h 的文件头注释）：
 *   20 字节小端头 + 载荷，flags bit0=1 时载荷是 RLE 压缩流，压不小就退回原样；
 *   一帧最多 LCD_MIRROR_MAX_ROWS_PER_FRAME 行，更长的脏区拆成多帧。
 *   status 会把协议版本、RLE/原样帧数、压掉多少字节一起打出来。
 *   ⚠ PC 端脚本要跟着 v2：老的只认 16 字节头（ver1）的客户端收不下现在的帧。
 *
 ****************************************************************************/

static int step_lcdmirror(FAR const char *sub, FAR const char *ip, int port,
                          FAR const char *dev, int resend_y0, int resend_rows)
{
  int ret;

  printf("[LCDMIRROR] 屏幕镜像（板端 -> PC）\n\n");

  /* `resend <y0> <rows>`：把这几行重新标脏、下一轮再发一遍。
   * PC 侧发现某条带子被日志字节插坏（它已经解出帧头、知道是哪一段）时发它 —— 
   * 这是"精确自愈"，取代了原来每 3 秒一趟的整屏关键帧（那趟会把 1.4 秒的线时
   * 全占掉，用户的即时更新只能排队）。 */

  if (sub != NULL && strcmp(sub, "resend") == 0)
    {
      lcd_mirror_resend_rows((uint16_t)resend_y0, (uint16_t)resend_rows);
      printf("      已请求补发 y=%d 起 %d 行\n", resend_y0, resend_rows);
      return OK;
    }

  if (sub != NULL && strcmp(sub, "keyframe") == 0)
    {
      lcd_mirror_resend_all();
      printf("      已请求整屏关键帧（全部行重新标脏）\n");
      return OK;
    }

  /* 先看子命令是不是换传输：这两条都只是置标志，任务下一轮自己重开。 */

  if (sub != NULL && strcmp(sub, "uart") == 0)
    {
      ret = lcd_mirror_use_uart(dev);
      if (ret < 0)
        {
          printf("      串口节点名太长: %d\n", ret);
          report("切换到串口传输", 0, "lcd_mirror_use_uart 失败");
          return -1;
        }

      printf("      传输已切到串口 %s（帧格式和 TCP 完全一样；"
             "反向触摸改用 `hw_test lcdtap <x> <y> <0|1>`）\n",
             lcd_mirror_uart_dev());
      printf("      ⚠ 串口上帧的二进制字节会和控制台日志交错，"
             "PC 端会丢掉坏帧、靠每 %d 秒一次的整屏关键帧自愈；\n"
             "        想停就 `hw_test lcdmirror tcp %s %d` 或 "
             "`hw_test lcdmirror stop`\n",
             LCD_MIRROR_UART_KEYFRAME_MS / 1000,
             LCD_MIRROR_DEFAULT_IP, LCD_MIRROR_DEFAULT_PORT);

      /* 没在跑就顺手起一个：切了传输却什么都没发生最容易被当成"没生效"。
       * （已经在跑的话 start 是幂等的，不重启任务。） */
      ret = lcd_mirror_start();
      if (ret < 0)
        {
          report("切换到串口传输", 0, "镜像任务起不来");
          return -1;
        }

      report("切换到串口传输", 1, lcd_mirror_uart_dev());
      lcd_mirror_status();
      return OK;
    }

  if (sub != NULL && strcmp(sub, "tcp") == 0)
    {
      ret = lcd_mirror_use_tcp(ip, (uint16_t)port);
      if (ret < 0)
        {
          printf("      目标地址设置失败: %d\n", ret);
          report("切换回 TCP 传输", 0, "lcd_mirror_use_tcp 失败");
          return -1;
        }

      printf("      传输已切回 TCP，目标 %s:%d\n",
             lcd_mirror_target_ip(), (int)lcd_mirror_target_port());
      report("切换回 TCP 传输", 1, NULL);
      lcd_mirror_status();
      return OK;
    }

  if (ip != NULL || port > 0)
    {
      ret = lcd_mirror_set_target(ip, (uint16_t)port);
      if (ret < 0)
        {
          printf("      目标地址设置失败: %d\n", ret);
          report("设置目标地址", 0, "lcd_mirror_set_target 失败");
          return -1;
        }

      printf("      目标已设为 %s:%d\n", lcd_mirror_target_ip(),
             (int)lcd_mirror_target_port());
    }

  if (sub != NULL && strcmp(sub, "stop") == 0)
    {
      ret = lcd_mirror_stop();
      if (ret < 0)
        {
          printf("      停止请求已发但任务还没退完: %d\n", ret);
          report("屏幕镜像已停止", 0, "任务退出超时（1 秒）");
          lcd_mirror_status();
          return -1;
        }

      report("屏幕镜像已停止", 1, NULL);
      lcd_mirror_status();
      return OK;
    }

  if (sub != NULL && strcmp(sub, "start") == 0)
    {
      ret = lcd_mirror_start();
      if (ret < 0)
        {
          printf("      启动失败: %d\n", ret);
          report("屏幕镜像已启动", 0, "task_create / 影子缓冲分配失败");
          return -1;
        }

      report("屏幕镜像已启动", 1, NULL);
      lcd_mirror_status();
      return OK;
    }

  if (sub != NULL && strcmp(sub, "status") != 0)
    {
      printf("      lcdmirror: 未知子命令 '%s'"
             "（start / stop / status / uart [dev] / tcp <ip> [port]）\n", sub);
      usage();
      return -1;
    }

  /* status：只读。镜像没在跑不算 FAIL —— 它就是可以关的。 */

  lcd_mirror_status();
  printf("      在 PC 上跑：py -3.10 D:/apply/claw/_flash/lcd_mirror.py\n");
  report("屏幕镜像状态查询", 1, lcd_mirror_is_running() ? "运行中" : "已停止");
  return OK;
}

/****************************************************************************
 * Name: step_lcdtap
 *
 * Description:
 *   `hw_test lcdtap <x> <y> <0|1>`：往镜像的触摸状态里注一次触摸。
 *
 *   这是**串口模式下的触摸入口**：串口那条线上，板子的帧是二进制、PC 的命令
 *   是文本，反向触摸没法像 TCP 那样塞一个 8 字节二进制包进去（会被 NSH 的行
 *   输入解析吃掉），所以改成 PC 写一行命令、由 NSH 执行。
 *
 *   参数必给（缺了直接报错退出，不靠 atoi 的 0 蒙过去）；x/y 是面板坐标，
 *   负数和越界都会被钳到 0..389 / 0..449（钳位在 lcd_mirror_inject_touch 里）。
 *   **镜像没在跑时注入是无效的**（界面那边根本不会来取），所以这里提示一句，
 *   但不算 FAIL —— 这条命令本身是成功的。
 *
 ****************************************************************************/

static int step_lcdtap(int x, int y, int down)
{
  printf("[LCDTAP] 注入触摸 (%d, %d) %s\n", x, y, down ? "按下" : "抬起");

  if (x < 0)
    {
      x = 0;
    }

  if (y < 0)
    {
      y = 0;
    }

  lcd_mirror_inject_touch((uint16_t)x, (uint16_t)y, down != 0);

  if (!lcd_mirror_is_running())
    {
      printf("      注意：镜像没在跑，这条触摸到不了界面"
             "（先 `hw_test lcdmirror uart` 或 `hw_test lcdmirror start`）\n");
    }

  report("注入触摸", 1, NULL);
  return OK;
}

/****************************************************************************
 * Name: append_arg
 *
 * Description:
 *   `hw_test tts <文本>` 用：NSH 按空格把命令切成多个参数，这里把 "tts"
 *   后面的参数用空格重新拼成一段文本。装不下就丢弃剩余参数并提示。
 *
 ****************************************************************************/

static void append_arg(FAR char *dst, size_t cap, FAR const char *arg)
{
  size_t len = strlen(dst);
  size_t add = strlen(arg);

  if (len > 0)
    {
      if (len + 1 + add >= cap)
        {
          printf("      警告：文本超过 %zu 字节，后面的参数被丢弃\n", cap - 1);
          return;
        }

      dst[len++] = ' ';
      dst[len] = '\0';
    }

  if (len + add >= cap)
    {
      printf("      警告：文本超过 %zu 字节，后面的参数被丢弃\n", cap - 1);
      return;
    }

  strcat(dst, arg);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  int do_color      = 0;
  int do_gpio       = 0;
  int do_imu        = 0;
  int do_rtc        = 0;
  int do_rtcday     = 0;
  int do_audio      = 0;
  int do_alarm      = 0;
  int do_button     = 0;
  int do_backlight  = 0;
  int do_lcdreinit  = 0;
  int do_status     = 0;
  int do_tts        = 0;
  int do_asr        = 0;
  int do_kws        = KWS_CMD_NONE;
  int standalone;
  int imu_frames    = IMU_DEFAULT_FRAMES;
  int rtc_sec       = RTC_DEFAULT_ALARM_SEC;
  int rtcday_hour   = 0;
  int rtcday_min    = 0;
  int rtcday_set    = 0;
  int audio_sec     = AUDIO_DEFAULT_SEC;
  int alarm_level   = ALARM_DEFAULT_LEVEL;
  int alarm_sec     = ALARM_DEFAULT_SEC;
  int button_sec    = BTN_DEFAULT_WAIT_SEC;
  int lcd_percent   = BACKLIGHT_DEFAULT_PCT;
  int touch_sec     = TOUCH_DEFAULT_SEC;
  int touch_set     = 0;
  char tts_text[TTS_MAX_TEXT];
  FAR const char *asr_path = NULL;
  int kws_slot      = -1;             /* -1 = 没给（enroll 必给，缺了要 FAIL） */
  int kws_sec       = KWS_ENROLL_DEFAULT_SEC;
  int kws_th        = 0;              /* threshold 子命令要在解析时就给全，
                                       * 0 不是合法阈值，漏掉一眼能看出来 */
  int do_lcdmirror  = 0;
  int lcdmirror_resend_y0   = 0;   /* `lcdmirror resend <y0> <rows>` 用 */
  int lcdmirror_resend_rows = 0;
  FAR const char *lcdmirror_sub  = NULL;   /* NULL = 只打印状态 */
  FAR const char *lcdmirror_ip   = NULL;
  FAR const char *lcdmirror_dev  = NULL;   /* 只给 uart 用：串口节点名 */
  int lcdmirror_port = 0;                  /* 0 = 不改，用板端默认 */
  /* IP / 节点名先拷进这两个缓冲再去掉尾随 '\r'（串口上 PC 发来的命令行带
   * "\r\n"，详细原因见 arg_tail_ws 的注释）。 */
  char lcdmirror_ip_buf[32];
  char lcdmirror_dev_buf[LCD_MIRROR_UART_DEV_MAX];
  int do_lcdtap     = 0;
  int lcdtap_x      = 0;
  int lcdtap_y      = 0;
  int lcdtap_down   = 0;
  int i;

  g_pass  = 0;
  g_total = 0;
  tts_text[0] = '\0';

  for (i = 1; i < argc; i++)
    {
      if (strcmp(argv[i], "lcdcolor") == 0)
        {
          do_color = 1;
        }
      else if (strcmp(argv[i], "gpio") == 0)
        {
          do_gpio = 1;
        }
      else if (strcmp(argv[i], "touch") == 0)
        {
          touch_sec = (i + 1 < argc) ? atoi(argv[++i]) : TOUCH_DEFAULT_SEC;
          touch_set = 1;
        }
      else if (strcmp(argv[i], "imu") == 0)
        {
          do_imu     = 1;
          imu_frames = (i + 1 < argc) ? atoi(argv[++i]) : IMU_DEFAULT_FRAMES;
        }
      else if (strcmp(argv[i], "rtc") == 0)
        {
          do_rtc  = 1;
          rtc_sec = (i + 1 < argc) ? atoi(argv[++i]) : RTC_DEFAULT_ALARM_SEC;
        }
      else if (strcmp(argv[i], "rtcday") == 0)
        {
          do_rtcday = 1;
          if (i + 1 < argc)
            {
              rtcday_hour = atoi(argv[++i]);
              rtcday_set  = 1;
              rtcday_min  = (i + 1 < argc) ? atoi(argv[++i]) : 0;
            }
        }
      else if (strcmp(argv[i], "audio") == 0)
        {
          do_audio  = 1;
          audio_sec = (i + 1 < argc) ? atoi(argv[++i]) : AUDIO_DEFAULT_SEC;
        }
      else if (strcmp(argv[i], "alarm") == 0)
        {
          do_alarm    = 1;
          alarm_level = (i + 1 < argc) ? atoi(argv[++i]) : ALARM_DEFAULT_LEVEL;
          alarm_sec   = (i + 1 < argc) ? atoi(argv[++i]) : ALARM_DEFAULT_SEC;
        }
      else if (strcmp(argv[i], "lcd") == 0)
        {
          /* 只有下一个参数确实是数字才吃掉它，免得把 `hw_test lcd gpio`
           * 里的 "gpio" 当成亮度 0（atoi("gpio") == 0）。 */

          do_backlight = 1;
          if (i + 1 < argc && arg_is_number(argv[i + 1]))
            {
              lcd_percent = atoi(argv[++i]);
            }
        }
      else if (strcmp(argv[i], "button") == 0)
        {
          do_button  = 1;
          button_sec = (i + 1 < argc) ? atoi(argv[++i]) : BTN_DEFAULT_WAIT_SEC;
        }
      else if (strcmp(argv[i], "lcdreinit") == 0)
        {
          do_lcdreinit = 1;
        }
      else if (strcmp(argv[i], "status") == 0)
        {
          do_status = 1;
        }
      else if (strcmp(argv[i], "lcdmirror") == 0)
        {
          /* `hw_test lcdmirror [start|stop|status|uart|tcp] [ip|节点] [port]`
           * 位置参数都可省：不给子命令 = 只打印状态；给了 IP 就顺手换目标
           * （start 会重连到新地址）；`uart [节点]` 切控制台串口那条腿
           * （不依赖 USB 网络），`tcp <ip> [port]` 切回来。
           * 参数判定一律走 arg_is_word / arg_is_number_ws：串口那条路上 PC
           * 发来的命令行末尾带着 '\r'（见那几个函数的注释）。 */

          do_lcdmirror = 1;

          if (i + 1 < argc)
            {
              /* 命中就钉一个**字面量**给 step_lcdmirror：argv 里那个可能带着
               * 尾随 '\r'（"uart\r"），直接往下传的话后面 strcmp 全对不上。 */
              if (arg_is_word(argv[i + 1], "start"))
                {
                  lcdmirror_sub = "start";
                  i++;
                }
              else if (arg_is_word(argv[i + 1], "stop"))
                {
                  lcdmirror_sub = "stop";
                  i++;
                }
              else if (arg_is_word(argv[i + 1], "status"))
                {
                  lcdmirror_sub = "status";
                  i++;
                }
              else if (arg_is_word(argv[i + 1], "uart"))
                {
                  lcdmirror_sub = "uart";
                  i++;
                }
              else if (arg_is_word(argv[i + 1], "tcp"))
                {
                  lcdmirror_sub = "tcp";
                  i++;
                }
              else if (arg_is_word(argv[i + 1], "resend"))
                {
                  /* `lcdmirror resend <y0> <rows>`：PC 侧报"这条带子坏了"时发它。
                   * 两个参数都必给、必须是数字（末尾可能粘着 PC 发来的 '\r'）。 */

                  if (i + 3 >= argc || !arg_is_number_ws(argv[i + 2]) ||
                      !arg_is_number_ws(argv[i + 3]))
                    {
                      printf("hw_test lcdmirror resend: 用法 "
                             "hw_test lcdmirror resend <y0> <rows>\n");
                      usage();
                      return EXIT_FAILURE;
                    }

                  lcdmirror_sub = "resend";
                  i++;
                  lcdmirror_resend_y0   = atoi(argv[++i]);
                  lcdmirror_resend_rows = atoi(argv[++i]);
                }
              else if (arg_is_word(argv[i + 1], "keyframe"))
                {
                  lcdmirror_sub = "keyframe";
                  i++;
                }
            }

          if (lcdmirror_sub != NULL && strcmp(lcdmirror_sub, "uart") == 0)
            {
              /* uart 后面那个位置是**设备节点**，只认以 '/' 开头的 —— 免得
               * `hw_test lcdmirror uart status` 把 "status" 当成节点名去 open。
               * 不给就用板端默认（/dev/console）。 */
              if (i + 1 < argc && argv[i + 1][0] == '/')
                {
                  arg_copy_trimmed(lcdmirror_dev_buf, sizeof(lcdmirror_dev_buf),
                                   argv[++i]);
                  lcdmirror_dev = lcdmirror_dev_buf;
                }
            }
          else if (i + 1 < argc && !arg_is_number_ws(argv[i + 1]))
            {
              arg_copy_trimmed(lcdmirror_ip_buf, sizeof(lcdmirror_ip_buf),
                               argv[++i]);
              lcdmirror_ip = lcdmirror_ip_buf;
            }

          if (i + 1 < argc && arg_is_number_ws(argv[i + 1]))
            {
              lcdmirror_port = atoi(argv[++i]);
            }

          if (i + 1 < argc)
            {
              printf("hw_test lcdmirror: 多余的参数 '%s'"
                     "（用法 start|stop|status|uart [dev]|tcp <ip> [port]）\n",
                     argv[i + 1]);
              usage();
              return EXIT_FAILURE;
            }
        }
      else if (strcmp(argv[i], "lcdtap") == 0)
        {
          /* `hw_test lcdtap <x> <y> <0|1>`：注入一次触摸。
           * **串口模式下的触摸入口** —— PC 往串口里写这一行文本（_flash/
           * lcd_mirror.py 的 touch_cmd()，结尾是 "\r\n"），NSH 执行它
           * （反向触摸不走帧的字节流，二进制包会被行输入解析吃掉）。
           * 三个参数都必给：缺了/不是数字直接报错退出，不能拿 atoi 的 0
           * 蒙过去（照 asr 缺文件路径那套写法）；数字判定用 arg_is_number_ws,
           * 因为最后一个参数后面粘着 PC 发来的那个 '\r'。 */

          if (i + 3 >= argc || !arg_is_number_ws(argv[i + 1]) ||
              !arg_is_number_ws(argv[i + 2]) || !arg_is_number_ws(argv[i + 3]))
            {
              printf("hw_test lcdtap: 用法 hw_test lcdtap <x> <y> <0|1>\n");
              usage();
              return EXIT_FAILURE;
            }

          do_lcdtap   = 1;
          lcdtap_x    = atoi(argv[++i]);
          lcdtap_y    = atoi(argv[++i]);
          lcdtap_down = atoi(argv[++i]);
        }
      else if (strcmp(argv[i], "tts") == 0)
        {
          do_tts = 1;

          /* "tts" 后面所有参数拼成一段文本（NSH 按空格切参数）。
           * 文本里有空格建议直接加引号：hw_test tts "你好 世界" */

          while (i + 1 < argc)
            {
              append_arg(tts_text, sizeof(tts_text), argv[++i]);
            }

          if (tts_text[0] == '\0')
            {
              printf("hw_test tts: 缺要合成的文本\n");
              usage();
              return EXIT_FAILURE;
            }
        }
      else if (strcmp(argv[i], "asr") == 0)
        {
          if (i + 1 >= argc)
            {
              printf("hw_test asr: 缺 WAV 文件路径\n");
              usage();
              return EXIT_FAILURE;
            }

          do_asr   = 1;
          asr_path = argv[++i];
        }
      else if (strcmp(argv[i], "kws") == 0)
        {
          /* `hw_test kws <enroll|test|live|selftest|threshold> [...]`。
           * slot / 秒数都只在"下一个参数是数字"时才吃掉 —— 和 lcd 子命令
           * 同一个理由：`hw_test kws enroll abc` 要报"缺 slot"，
           * 不能把 atoi("abc") = 0 当成 slot0 照录。
           * threshold 是反过来的：值**必须**给，缺了/不是数字直接报错退出
           * （照 asr 缺文件路径那套写法），不能拿 atoi("abc") = 0 去设阈值。
           * 只多认一个字面量 `default`（换成默认阈值），别的一律走数字那条。 */

          if (i + 1 >= argc)
            {
              printf("hw_test kws: 缺子命令"
                     "（enroll <slot> [秒] / test / selftest / "
                     "threshold <值|default> / live [秒]）\n");
              usage();
              return EXIT_FAILURE;
            }

          i++;

          if (strcmp(argv[i], "enroll") == 0)
            {
              do_kws = KWS_CMD_ENROLL;

              if (i + 1 < argc && arg_is_number(argv[i + 1]))
                {
                  kws_slot = atoi(argv[++i]);
                }

              if (i + 1 < argc && arg_is_number(argv[i + 1]))
                {
                  kws_sec = atoi(argv[++i]);
                }
            }
          else if (strcmp(argv[i], "test") == 0)
            {
              do_kws = KWS_CMD_TEST;
            }
          else if (strcmp(argv[i], "selftest") == 0)
            {
              do_kws = KWS_CMD_SELFTEST;
            }
          else if (strcmp(argv[i], "threshold") == 0)
            {
              /* `default` = 一键恢复默认阈值（现场标定完想回默认，不用重启） */

              if (i + 1 < argc && strcmp(argv[i + 1], "default") == 0)
                {
                  i++;
                  kws_th = KWS_THRESHOLD_MILLI;
                }
              else if (i + 1 < argc && arg_is_number(argv[i + 1]))
                {
                  kws_th = atoi(argv[++i]);
                }
              else
                {
                  printf("hw_test kws: threshold 要一个数字或 default。用法 "
                         "hw_test kws threshold <值|default>"
                         "（default = 恢复 %d）\n", KWS_THRESHOLD_MILLI);
                  usage();
                  return EXIT_FAILURE;
                }

              do_kws = KWS_CMD_THRESHOLD;
            }
          else if (strcmp(argv[i], "live") == 0)
            {
              do_kws = KWS_CMD_LIVE;
              kws_sec = KWS_LIVE_DEFAULT_SEC;

              if (i + 1 < argc && arg_is_number(argv[i + 1]))
                {
                  kws_sec = atoi(argv[++i]);
                }
            }
          else
            {
              printf("hw_test kws: 未知子命令 '%s'"
                     "（enroll <slot> [秒] / test / selftest / "
                     "threshold <值|default> / live [秒]）\n", argv[i]);
              usage();
              return EXIT_FAILURE;
            }
        }
      else
        {
          printf("hw_test: 未知参数 '%s'\n", argv[i]);
          usage();
          return EXIT_FAILURE;
        }
    }

  /* imu / rtc / rtcday / audio / alarm / lcd / lcdreinit / button / status /
   * tts / asr / kws / lcdmirror / lcdtap 是各自独立的子命令：只跑自己，
   * 不跑那套 5 步自检（tts / asr 要联网，是全自检里唯一会等网络的，
   * 所以也放单独模式）。 */

  standalone = do_imu || do_rtc || do_rtcday || do_audio || do_alarm ||
               do_backlight || do_lcdreinit || do_button || do_status ||
               do_tts || do_asr || do_kws || do_lcdmirror || do_lcdtap;

  /* lcdcolor 只是刷个屏，别让它再干等 10 秒触摸 */

  if (do_color && !touch_set)
    {
      touch_sec = 0;
    }

  printf("\n");
  printf("========================================\n");
  printf("   SF32LB52-DevKit-LCD 硬件自检 (hw_test)\n");
  if (standalone)
    {
      printf("   子命令模式：只跑 imu/rtc/rtcday/audio/alarm/lcd/lcdreinit/"
             "button/status/tts/asr/kws/lcdmirror/lcdtap，不做 5 步自检\n");
    }
  else
    {
      printf("   默认只做只读检查；lcdcolor/gpio 才会改硬件\n");
    }

  printf("========================================\n\n");

  if (standalone)
    {
      if (do_imu)
        {
          step_imu(imu_frames);
          printf("\n");
        }

      if (do_rtc)
        {
          step_rtc(rtc_sec);
          printf("\n");
        }

      if (do_rtcday)
        {
          step_rtcday(rtcday_hour, rtcday_min, rtcday_set);
          printf("\n");
        }

      if (do_audio)
        {
          step_audio(audio_sec);
          printf("\n");
        }

      if (do_alarm)
        {
          step_alarm(alarm_level, alarm_sec);
          printf("\n");
        }

      if (do_backlight)
        {
          step_backlight(lcd_percent);
          printf("\n");
        }

      if (do_lcdreinit)
        {
          step_panel_reinit();
          printf("\n");
        }

      if (do_button)
        {
          step_button_wait(button_sec);
          printf("\n");
        }

      if (do_status)
        {
          step_status();
          printf("\n");
        }

      if (do_tts)
        {
          step_tts(tts_text);
          printf("\n");
        }

      if (do_asr)
        {
          step_asr(asr_path);
          printf("\n");
        }

      if (do_kws != KWS_CMD_NONE)
        {
          step_kws(do_kws, kws_slot, kws_sec, kws_th);
          printf("\n");
        }

      if (do_lcdmirror)
        {
          step_lcdmirror(lcdmirror_sub, lcdmirror_ip, lcdmirror_port,
                         lcdmirror_dev, lcdmirror_resend_y0,
                         lcdmirror_resend_rows);
          printf("\n");
        }

      if (do_lcdtap)
        {
          step_lcdtap(lcdtap_x, lcdtap_y, lcdtap_down);
          printf("\n");
        }
    }
  else
    {
      step_nodes();
      printf("\n");
      step_touch(touch_sec);
      printf("\n");
      step_lcd(do_color);
      printf("\n");
      step_buttons(BTN_TIMEOUT_MS);
      printf("\n");
      step_gpio(do_gpio);
    }

  printf("\n========================================\n");
  printf("   结果: %d/%d PASS", g_pass, g_total);
  if (g_pass == g_total)
    {
      printf("   >>> 硬件自检通过 <<<\n");
    }
  else
    {
      printf("   >>> 有 FAIL 项，见上面标记 <<<\n");
    }

  printf("========================================\n\n");

  return g_pass == g_total ? EXIT_SUCCESS : EXIT_FAILURE;
}
