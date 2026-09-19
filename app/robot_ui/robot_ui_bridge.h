/**
 * robot_ui_bridge.h - 语音链路推界面用的跨 app 直调接口（同进程，不绕 MQTT）
 *
 * 背景（用户原话：「语音聊天卡死了？说话没有反应啊」）：
 *   语音入口在 hello_app 的 ai_companion 里（常开麦跑 VAD -> ASR -> 大模型 -> TTS），
 *   界面在 robot_ui 里。整机是单一大镜像（CONFIG_BUILD_FLAT），两边在同一个地址
 *   空间、符号最终链接时解析 —— 本来可以直接函数调用。但界面反馈原来是绕公网
 *   MQTT 实现的（publish 到 zhi_ai/<client_id>/command，再由 robot_ui 的
 *   network_task 收回来才刷屏）。这条路上任何一环慢/丢/断，用户看到的就是
 *   "完全没反应"：
 *     - ai_companion 开机那一刻 RNDIS/DNS 常常还没就绪，`ai_network_start_shared()`
 *       连不上 broker -> `g_net_started = false`，而它只在 main 里赋值一次、没有
 *       重试路径，于是**整个开机周期里一条界面反馈都发不出去**（真机日志：
 *       `MQTT DNS: broker.emqx.io 解析失败` -> `connect: Error 101` ->
 *       `[警告] 网络初始化失败`）；
 *     - publisher 和 subscriber 用的是同一个 client_id（两边都默认 zhi_ai_001），
 *       谁先连上谁被后连上的踢掉；
 *     - 本机出一点网络抖动，publish 就按失败处理（mqtt_publish 会把 connected
 *       置 false 等重连）。
 *   本机自己的界面反馈没有任何理由依赖公网一个来回。所以这里给 ai_companion
 *   一组**同进程直调**的入口：本地立刻刷屏，MQTT 照发不误（手机/PC 端还在用）。
 *
 * 为什么单独开一个文件：robot_ui.h / touch_ui.h 都要 <lvgl.h>，而 hello_app
 * 的编译命令里**没有**（也不该有）LVGL 的头文件路径 —— 它不碰界面控件。
 * 所以这个头文件必须自给自足：只依赖 <stddef.h> / <stdbool.h>，不引任何
 * robot_ui 或 LVGL 的头文件。实现（robot_ui_bridge.c）编在 robot_ui 那个 app 里，
 * 只有它需要 lvgl.h / touch_ui.h / robot_ui.h。
 *
 * ⚠️ 三条铁律（调用方必须守）：
 *   1) 这些函数**不是**线程安全的 LVGL 操作本身，它们内部只做
 *      「参数清洗 + 投递」，真正的控件操作全部在 LVGL 线程里执行
 *      （走 touch_ui 的 lv_async_call / main.c 的 ui_post）。所以任何线程都能调；
 *   2) **不许在中断/音频回调里调**（lv_async_call 会取锁、可能 malloc）。
 *      现有调用点全都在任务上下文：ai_companion 的主循环任务、录音线程、
 *      以及大模型/播放完成回调；
 *   3) robot_ui 没起来时（LVGL 没初始化）调用要安全空转 —— 所以有
 *      robot_ui_bridge_set_ready() 这道闸门，见下。
 *
 * 唯一一个不在这三条里的例外是 robot_ui_bridge_panel_reinit()：
 * 它要能在"界面看着已经死了"的时候调（黑屏救回），所以**不看**那道闸门
 * —— 面板侧照修不误，只有"投一次全屏重绘"这一步要求界面就绪。
 */

#ifndef ROBOT_UI_BRIDGE_H
#define ROBOT_UI_BRIDGE_H

#include <stddef.h>
#include <stdbool.h>

/* 显示文本的缓冲上限（字节，含结尾 '\0'）。
 * 与界面那一侧一致：robot_ui 的对话区显示缓冲就是 256。 */
#define ROBOT_UI_BRIDGE_TEXT_MAX   256

/* 语音状态档位。
 * 这里**刻意不复用** touch_ui.h 的 touch_voice_state_t：那个头文件要 <lvgl.h>，
 * hello_app 编不了；而且把 LVGL 依赖引到一个纯逻辑的调用方头上毫无收益。
 * 取值顺序和 touch_voice_state_t 一一对应，实现里只做一次映射。 */
typedef enum
{
  ROBOT_UI_BRIDGE_VOICE_IDLE = 0,   /* 空闲（面板那行显示「录制中」） */
  ROBOT_UI_BRIDGE_VOICE_LISTENING,  /* 正在听用户说话 */
  ROBOT_UI_BRIDGE_VOICE_THINKING,   /* 送 ASR / 等大模型 */
  ROBOT_UI_BRIDGE_VOICE_SPEAKING    /* TTS 正在出声 */
} robot_ui_bridge_voice_state_t;

/****************************************************************************
 * 闸门：界面就绪了没有
 *
 * robot_ui 的 main() 在 LVGL 初始化、控件建好之后调
 * robot_ui_bridge_set_ready(true)；没调之前（robot_ui 没在跑、或者它还在
 * 初始化路上）所有推送都是空操作 —— 单一大镜像里 ai_companion 完全可能比
 * 界面先跑起来，那时候 lv_async_call 还没有 LVGL 定时器去消费它，
 * 真投进去只会攒在队列里（更坏的情况是撞未初始化的锁）。
 *
 * 只有"就绪"这一位，没有"取消就绪"的用法：界面从不主动退出。
 ****************************************************************************/

void robot_ui_bridge_set_ready(bool ready);
bool robot_ui_bridge_is_ready(void);

/****************************************************************************
 * 四个动作（任何线程可调；robot_ui 没就绪时空操作）
 ****************************************************************************/

/**
 * @brief  显示用户刚说的话（ASR 原文）
 *
 * 会顺手按界面那边的抑制规则自动弹出语音镜像面板（用户手动关过就不再弹）。
 * @param  text  ASR 原文；NULL / 空串 / 清洗后为空 -> 界面不动（只打日志）
 */
void robot_ui_bridge_voice_user_said(const char *text);

/**
 * @brief  显示 AI 的回复正文
 *
 * 同时更新状态栏/表情（与 MQTT 的 ai_reply 动作一致）和镜像面板的对话区。
 * @param  text  回复正文；NULL / 空串 / 清洗后为空 -> 界面不动
 */
void robot_ui_bridge_voice_reply(const char *text);

/**
 * @brief  更新语音状态（状态栏 + 表情 + 镜像面板状态行）
 * @param  state  见 robot_ui_bridge_voice_state_t
 */
void robot_ui_bridge_voice_state(int state);

/**
 * @brief  按界面的抑制规则自动弹出语音镜像面板
 *
 * 比语音状态更"轻"的一个入口：只弹面板，不改任何文字。
 */
void robot_ui_bridge_voice_panel_open(void);

/****************************************************************************
 * 面板重新初始化（黑屏救回）
 *
 * 干的是"整屏黑、但 LVGL 还在刷、驱动还在推屏"那种故障的救回动作：
 *   ① 在 NSH 任务里直接调驱动侧的 sf32lb_lcd_panel_reinit()（重发面板初始化
 *      序列 + 拉一次 RESET 脚 + 重设像素格式/亮度/DisplayOn；下发期间靠驱动里
 *      的 panel_lock 和刷新路径互斥）；
 *   ② 面板侧好了之后，往 LVGL 线程投一次**全屏失效** —— 面板被复位过，
 *      GRAM 里是随机的，光修面板它不会自己重画。
 *
 * 为什么这个函数在 robot_ui 这侧而不是全塞驱动里：② 要碰 LVGL 的屏幕对象，
 * 而 LVGL 对象只有 LVGL 线程能碰（`lv_obj_invalidate()` 在别的线程里调会撞
 * `Invalidate area is not allowed during rendering` 断言，2026-09-14 就被这个
 * 断言打死过），所以必须过 ui_async_call 投递。
 *
 * 参数：无。返回 0 = 面板重新初始化下发完成；负数 = errno
 *       （-ENODEV 面板驱动没起来 / -EINVAL 中断上下文 / -ENOSYS 编译期关了）。
 *       注意返回 0 **不代表肉眼已经看到画面**，画面要到 LVGL 刷完那一帧才有。
 ****************************************************************************/

int robot_ui_bridge_panel_reinit(void);

/****************************************************************************
 * 每帧的"面板内容对不上"兜底（丢帧 / 面板复位之后自动整屏重绘）
 *
 * 驱动侧 putrun/putarea 在面板就绪之前是**静默丢弃**的（开机时 LVGL 的整屏
 * 首帧就落在那个窗口里），面板被 SWRESET 重初始化也会清掉 GRAM。两种情况
 * 驱动都会挂一个标志，本函数把它取走、把整屏判脏重画一次 —— LVGL 只重画
 * 脏区，没人告诉它"整屏都没了"，它就一直不画。
 *
 * 调用位置：**必须**是 LVGL 线程（robot_ui/main.c 的主循环里，每帧调一次）。
 * 内部自己限速 250ms（面板还没就绪时不能每 5ms 画一整屏），所以调用点不用管频率。
 *
 * 返回：这次有没有真的投一次整屏重绘。
 ****************************************************************************/

bool robot_ui_bridge_lcd_check(void);

/****************************************************************************
 * 内部桥接原语（定义在 robot_ui/main.c，不对外使用）
 *
 * 为什么这三个函数在 main.c 而不在 robot_ui_bridge.c 里：
 *   它们要够到 main.c 的三个 static —— sanitize_for_display()（字库字形清洗）、
 *   ui_post()（跨线程状态栏/表情/对话区的投递）、voice_mirror_autoshow()
 *   （"用户关过就别再弹"的抑制状态）。把 static 去掉当然也能编，但 main.c 里
 *   这几个名字有二十多个调用点，改一处就得跟着动一片；加三个薄门面代价最小，
 *   也保证清洗规则和抑制规则**只有一份**（桥接和 MQTT 两条路走的是同一段逻辑，
 *   不会各自漂移）。
 ****************************************************************************/

/* 把显示文本清洗成字库能渲染的纯文本（包装 sanitize_for_display） */
size_t robot_ui_bridge_sanitize_text(const char *in, char *out, size_t out_cap);

/* 投递一次"状态栏 + 表情 + 对话区"更新（包装 ui_post，内部 lv_async_call）。
 * status / face 传 -1 表示这一项不动。 */
void robot_ui_bridge_post_status(int status, int face, const char *reply);

/* 语音链路有动静时按抑制规则弹镜像面板（包装 voice_mirror_autoshow） */
void robot_ui_bridge_panel_autoshow(void);

#endif /* ROBOT_UI_BRIDGE_H */
