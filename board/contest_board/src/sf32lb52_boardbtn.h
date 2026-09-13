/****************************************************************************
 * board/contest_board/src/sf32lb52_boardbtn.h
 *
 * SF32LB52 板级按键模块：直接读 GPIO，**不经过 /dev/buttons**
 *
 * ---------------------------------------------------------------------------
 * 为什么不用 /dev/buttons（重要，先看这段）
 * ---------------------------------------------------------------------------
 * 本板已经有一个 NuttX 标准按键驱动（`sf32lb52_buttons.c`，注册成
 * `/dev/buttons`，上层用 `poll()` + `read()` 读 `btn_buttonset_t`）。
 * 实测在真机上，`app/hw_test` 里那段 `poll()` + `read()` 会让**整机静默
 * 卡死**：没有任何输出、USB 设备从主机侧消失，只能重新烧录才能恢复。
 * 卡死位置在 /dev/buttons 的 poll/read 路径上（不是断言、也不是崩溃，
 * 是彻底不动了），所以本模块**另起一条路**：
 *
 *   - 按键脚按普通 GPIO 配置 + 轮询 + 软件消抖；
 *   - 事件由本模块的轮询任务通过回调发给使用方，不经过任何字符设备。
 *
 * `sf32lb52_buttons.c` 的注册**保持原样**（别的模块可能在用），本模块只是
 * 并行提供另一条读法，见下面"与 /dev/buttons 驱动的关系"。
 *
 * ---------------------------------------------------------------------------
 * 引脚（板子上就这两个按键）
 * ---------------------------------------------------------------------------
 *   BOARD_BTN_KEY  = PA11，wiki 表 1 的 KEY / 功能按键，高电平有效
 *   BOARD_BTN_HOME = PA34，wiki 表 1 的 HOME / 长按复位按键，高电平有效
 *
 * PA11：`bsp_pinmux.c:173` 已经把它配成 `GPIO_A11` 输入 + 下拉，本模块
 *       只在 `board_btn_init()` 里**重申一次**同样的 pad 配置（见 .c 里
 *       的说明），方向、功能都没变。
 *
 * PA34：**特殊脚，注意**。`bsp_pinmux.c:171` 那一行是**故意注释掉**的
 *       （注释原文：保持默认下拉，关掉内部下拉会让 UART 下载驱动在没有
 *       外部下拉的板子上失效）。也就是说这块板子 PA34 没有外部下拉，
 *       而它同时还兼"长按复位"（PMU 电源键逻辑）和 AON 唤醒脚。
 *       本模块的处理：**保持下拉不变**，只把它切成 `GPIO_A34` 输入来读；
 *       不申请中断、不独占、不写电平。如果现场发现读 PA34 有问题，把
 *       `BOARD_BTN_ENABLE_HOME` 定义成 0 重编即可退化成"只支持 PA11"，
 *       其余代码不用动。
 *
 * ---------------------------------------------------------------------------
 * 与 /dev/buttons 驱动的关系（两边的 pad 配置）
 * ---------------------------------------------------------------------------
 * 两边读的是**同一个 pad**，但互不写对方的寄存器：
 *   - `sf32lb52_buttons.c` 只调 `sifli_gpio_config(pin, GPIO_INPUT)`
 *     （那是个固定 NOPULL 的输入配置）和 `sifli_gpio_set_event()`；
 *   - 本模块只调 `sifli_gpio_read()` 读电平，另外在 init 里用
 *     `HAL_PIN_Set()` 把 PA11/PA34 的 pad 恢复成板级 pinmux 的下拉。
 * 两者都不改对方的方向/功能，可以并存（细节见 .c 文件头的说明）。
 * 反过来，如果哪天不要 `/dev/buttons` 了，直接删本模块的 init 也不会
 * 影响任何东西。
 *
 * ---------------------------------------------------------------------------
 * 用法（完整例子见 docs/display_touch_gpio_usage.md 第 5 节）
 * ---------------------------------------------------------------------------
 *   static void my_btn_cb(enum board_btn_e btn, enum board_btn_event_e ev,
 *                         uint32_t held_ms, FAR void *arg)
 *   {
 *     // ⚠️ 这里跑在轮询任务上下文：**不要阻塞**（sleep/等信号量/开文件/
 *     //    打长日志都算），**不要碰 LVGL**（lv_* 只能在建 UI 的线程里调）。
 *     //    只更新一个标志/投递消息，剩下的交给自己的任务做。
 *   }
 *
 *   board_btn_set_callback(my_btn_cb, NULL);   // 内部会懒初始化
 *   ...
 *   board_btn_set_callback(NULL, NULL);        // 注销
 *
 * 不想用回调、只想偶发查一下状态时，用 `board_btn_is_pressed()`
 * （它是即时值，不排队；初始化前也能查 PA11）。
 *
 ****************************************************************************/

#ifndef __BOARDS_CONTEST_BOARD_SRC_SF32LB52_BOARDBTN_H
#define __BOARDS_CONTEST_BOARD_SRC_SF32LB52_BOARDBTN_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/compiler.h>

#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 轮询周期与消抖：连续 BOARD_BTN_DEBOUNCE_SAMPLES 次采样一致才算电平变化，
 * 也就是最坏 ~30ms 的消抖时间（按下/松开会晚 2 个周期上报）。 */

#define BOARD_BTN_POLL_MS          10
#define BOARD_BTN_DEBOUNCE_SAMPLES 3

/* 按住超过它就报一次 BOARD_BTN_LONGPRESS（只报一次，松手时照常报 RELEASE）。 */

#define BOARD_BTN_LONGPRESS_MS     1000

/* 轮询任务：优先级 115 —— 比 robot_ui（110）低，不会抢 UI 的 CPU；
 * 比系统工作队列（HPWORK 224）高，按键不会被网络/音频的回调拖后。
 * 任务里只做"读两个 GPIO + 比较"，3072 字节栈足够。 */

#define BOARD_BTN_TASK_NAME        "boardbtn"
#define BOARD_BTN_TASK_PRIORITY    115
#define BOARD_BTN_TASK_STACK       3072

/* 1 = 把 PA34(HOME) 也当按键读；0 = 完全不动 PA34，只支持 PA11。
 * 现场如果发现 PA34 有问题（例如它在这块板子上根本没接按键、或被 PMU
 * 逻辑占着读不准），把它改成 0 重编即可，接口不用变。 */

#ifndef BOARD_BTN_ENABLE_HOME
#  define BOARD_BTN_ENABLE_HOME 1
#endif

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* 板子上的按键编号（本板两个） */

enum board_btn_e
{
  BOARD_BTN_KEY = 0,      /* PA11，功能按键 */
  BOARD_BTN_HOME,         /* PA34，HOME / 长按复位脚（见文件头说明） */
  BOARD_BTN_COUNT
};

/* 按键事件 */

enum board_btn_event_e
{
  BOARD_BTN_PRESS = 0,    /* 按下（已消抖） */
  BOARD_BTN_RELEASE,      /* 松开，held_ms = 本次从按下到松开的毫秒数 */
  BOARD_BTN_LONGPRESS,    /* 按住超过 BOARD_BTN_LONGPRESS_MS，只报一次 */
};

/* 事件回调。
 *
 * ⚠️ **回调在轮询任务（"boardbtn"）的上下文里被调用**：
 *    - 不要阻塞：sleep / 等信号量 / 开文件 / 打很长的日志都不行，
 *      会拖住另一个按键的事件上报（严重时让按键看起来"没反应"）；
 *    - 不要碰 LVGL：`lv_*` 只能在创建界面的那个线程里调，
 *      跨线程调会花屏或崩。回调里只更新标志 / 投消息，让 UI 线程去刷；
 *    - 回调返回后本模块才继续轮询，所以回调要短。
 *
 * 参数：btn = 哪个键；ev = 事件类型；held_ms = 按住时长（PRESS 时是 0，
 * LONGPRESS/RELEASE 时是从按下算起的毫秒数）；arg = 注册时传进来的用户参数。
 */

typedef void (*board_btn_cb_t)(enum board_btn_e btn,
                               enum board_btn_event_e ev,
                               uint32_t held_ms,
                               FAR void *arg);

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: board_btn_init
 *
 * Description:
 *   初始化按键模块：配置 PA11（必要时还有 PA34）为 GPIO 输入，并起一个
 *   轮询任务（周期 BOARD_BTN_POLL_MS，优先级 BOARD_BTN_TASK_PRIORITY）。
 *
 *   **幂等**：重复调用只会返回 OK，不会再起一个任务。也可以不调 ——
 *   board_btn_set_callback() 会懒初始化。
 *
 *   起任务之后不要用 `board_btn_init()` 之外的途径再配这两个脚。
 *
 * Returned Value:
 *   OK on success; task_create 失败返回负 errno。
 *
 ****************************************************************************/

int board_btn_init(void);

/****************************************************************************
 * Name: board_btn_set_callback
 *
 * Description:
 *   注册（或注销）按键事件回调。一个模块只有一个回调槽，后注册的覆盖
 *   前面的；`cb == NULL` 表示注销（arg 一并清掉）。
 *
 *   第一次注册会顺手把模块初始化起来（等价于先调 board_btn_init()）。
 *   **不要在中断里调**（内部可能建任务）。
 *
 * Returned Value:
 *   OK on success; 初始化失败返回负 errno。
 *
 ****************************************************************************/

int board_btn_set_callback(board_btn_cb_t cb, FAR void *arg);

/****************************************************************************
 * Name: board_btn_is_pressed
 *
 * Description:
 *   查一个键当前是不是按下的。
 *
 *   - 模块已经跑起来（init 过）：返回**消抖后**的状态（最多晚两个轮询周期）；
 *   - 还没 init：返回**即时**的一次 GPIO 读值（PA11 一直可读；PA34 在
 *     init 之前没被本模块切成 GPIO，所以返回 -ENODEV）；
 *   - 本函数**不需要**先 board_btn_init()，也不会去碰设备 / 睡眠；内部只是
 *     读一个状态字（用一把很短的锁保护，不会等长）。
 *
 * Returned Value:
 *   1 = 按下，0 = 松开，< 0 = 错误（-EINVAL 编号越界；-ENODEV 这个脚现在
 *   还不能读）。
 *
 ****************************************************************************/

int board_btn_is_pressed(enum board_btn_e btn);

/****************************************************************************
 * Name: board_btn_name / board_btn_event_name
 *
 * Description:
 *   给日志/自检打印用的名字（"KEY(PA11)" / "HOME(PA34)"、"PRESS" /
 *   "RELEASE" / "LONGPRESS"）。参数越界时返回 "?"。
 *
 ****************************************************************************/

FAR const char *board_btn_name(enum board_btn_e btn);
FAR const char *board_btn_event_name(enum board_btn_event_e ev);

#endif /* __BOARDS_CONTEST_BOARD_SRC_SF32LB52_BOARDBTN_H */
