/**
 * robot_ui.h - 智爱陪伴机器人界面头文件
 * SF32LB52-DevKit-LCD LVGL 界面开发
 */

#ifndef ROBOT_UI_H
#define ROBOT_UI_H

#include <lvgl.h>

/* 机器人表情类型 */
typedef enum {
    ROBOT_FACE_HAPPY,      // 开心
    ROBOT_FACE_THINKING,   // 思考
    ROBOT_FACE_SLEEPY,     // 困倦
    ROBOT_FACE_SURPRISED,  // 惊讶
    ROBOT_FACE_WORRIED,    // 担心
    ROBOT_FACE_ALARM,      // 报警
    ROBOT_FACE_MAX
} robot_face_t;

/* 机器人状态。
 *
 * ⚠️ 这是个**显示用**的档位，不是新的一路状态来源：语音那四档（听/想/说/空闲）
 * 全部来自 hello_app 的 voice_state_probe()，经 ui_post() 投到 LVGL 线程后
 * 由 robot_ui_set_status() 落到主界面「AI 回复区」那一行小字上。
 *
 * THINKING 是 2026-09-16 补的一档：原来"在想"借的是 LISTENING（只靠表情区分），
 * 主界面那一行小字就没法把"在听"和"在想"分开 —— 用户要的正是这四档各显各的。
 * 加在 LISTENING 后面只是为了读起来顺；这些值不落盘、不上网、也没有任何数组按
 * 它下标，所以挪位置是安全的（全仓库只有 robot_ui.c 和 main.c 那几处 switch/投递）。 */
typedef enum {
    ROBOT_STATUS_IDLE,      // 待机（空闲）
    ROBOT_STATUS_LISTENING, // 监听（在听）
    ROBOT_STATUS_THINKING,  // 识别/思考中（在想）
    ROBOT_STATUS_SPEAKING,  // 说话（在说）
    ROBOT_STATUS_REMINDING, // 提醒中
    ROBOT_STATUS_ALARM,     // 报警
    ROBOT_STATUS_MAX
} robot_status_t;

/* 界面类型 */
typedef enum {
    UI_VIEW_MAIN,       // 主界面
    UI_VIEW_REMIND,     // 提醒界面
    UI_VIEW_ALARM,      // 报警界面
    UI_VIEW_SETTING,    // 设置界面
    UI_VIEW_MAX
} ui_view_t;

/* 初始化 UI */
void robot_ui_init(void);

/* 更新机器人表情 */
void robot_ui_set_face(robot_face_t face);

/* 更新 AI 回复 */
void robot_ui_set_ai_reply(const char *text);

/* 更新状态：写的是**主界面「AI 回复区」里那一行语音状态小字**
 * （create_ai_reply_area() 建的 lbl_status），不在状态栏上 ——
 * 状态栏那三样 2026-09-14 已经被用户要求删掉了。
 *
 * 空闲 -> 「空闲」灰 / 在听 -> 「在听…」蓝 / 在想 -> 「在想…」橙 /
 * 在说 -> 「在说…」绿；提醒中、报警两档借用同一行，颜色见 robot_ui.c。
 *
 * ⚠️ 只能在 LVGL 线程里调（它直接改控件）。别的线程一律走 main.c 的 ui_post() /
 * ui_post_*，内部是 lv_async_call —— 跨线程碰 LVGL 会踩 rendering_in_progress
 * 断言把 app 打死（2026-09-14 实测）。
 * ⚠️ 认不出来的档位一个字都不改（robot_ui.c 里那条 default 分支），
 * 宁可少刷一次，也不要把这一行停在一个错的字上。 */
void robot_ui_set_status(robot_status_t status);

/* 更新状态栏上的网络状态（ASCII，如 "NET OK" / "NET --"）。
 * 必须从 LVGL 所在的任务调用（main 的主循环），不要从 network_task 直接调。
 */
void robot_ui_set_net_status(const char *text);

/* 显示提醒 */
void robot_ui_show_reminder(const char *title, const char *content);

/* 报错提示页（顶层覆盖，一个大按钮点掉）。
 * ⚠️ 只能在 LVGL 线程里调：别的线程（语音工作线程 / MQTT 线程 / 播放线程）
 * 用 main.c 的 ui_post_error()，内部 lv_async_call 投过来 —— 直接跨线程碰
 * LVGL 会踩 "_lv_inv_area: Invalidate area is not allowed during rendering"
 * 断言，把整个 app 打死（2026-09-14 实测）。 */
void robot_ui_show_error(const char *title, const char *content);

/* 显示报警 */
void robot_ui_show_alarm(const char *content);

/* 关闭报警 */
void robot_ui_close_alarm(void);

/* 刷新状态栏时钟（从 main 主循环调用，与 LVGL 定时器双重保障） */
void robot_ui_update_time(void);

/* 切换界面 */
void robot_ui_switch_view(ui_view_t view);

/* ==================== 「询问是否报警」页（异常声响二次确认） ==================== */
/*
 * 用户拍板：检测到异常**不直接报警**，先问一句，二次确认了才报警。
 * 这一页只是三条确认路里的**第一条**（板子的屏幕可能坏了/被拆下来，确认不能只靠
 * 屏幕），另外两条是 MQTT 下行 confirm_alarm 和 20 秒超时。谁给的回答都算数，
 * 三条最后都收口到 main.c 的 ask_finish()。详细说明见 robot_ui.c 里那一节。
 *
 * ⚠️ 这几个都是"建/删控件"，只能在 LVGL 线程里调；别的线程一律走
 * ui_post_ask_alarm()（main.c 里，内部 lv_async_call 投递）。
 */

/* 无人应答时自动按「不用了」处理的时限（毫秒）。定义在这里而不是 main.c 私有，
 * 是因为对话框上要如实写给用户看（"20 秒内请回答"），两处必须是同一个数。 */
#define ROBOT_ASK_ALARM_TIMEOUT_MS   20000

/* 用户否认后，同一类原因的静默期（毫秒）：这期间不再重复询问。
 * "同一类"= 把 reason 尾部的置信度数字摘掉之后相等（见 main.c 的 ask_reason_key()），
 * 不是整条字符串相等 —— 置信度每次都不一样，拿整条比等于没有静默期。 */
#define ROBOT_ASK_ALARM_DENY_HOLD_MS 60000

/* 回答回调（**在 LVGL 线程里**被调，不许阻塞、不许做网络/音频动作）：
 *   confirmed = 1   用户点了「是的，报警」（或 MQTT 下了 confirm:true）
 *   confirmed = 0   用户点了「不用了」，或者超时无人应答
 *   confirmed = -1  这一页被**作废**了（不是谁回答的）：报警页要盖上来，询问页让位
 *                   —— 既不算确认、也不能记成"用户否认"（那会让同一原因 60 秒内
 *                   问不出来），只把待答状态清掉。
 *   reason         这次询问的原因原文，回调期间有效，回调返回后失效（要留就自己拷）
 * 真正的动作（报警 / 记账 / 静默期）在 main.c 的收口函数里，这边只报"回答是哪个"。 */
typedef void (*robot_ask_result_cb_t)(int confirmed, const char *reason, void *arg);

/* 登记回答回调（全局一个槽，和别的 *_set_cb 一样只登记一次） */
void robot_ui_set_ask_result_cb(robot_ask_result_cb_t cb, void *arg);

/* 弹出询问页（reason 可为 NULL，那就显示"异常声响"）。
 * 已经开着一条时**不重建**，只打一行日志（同一时间只允许一个 pending 询问）。 */
void robot_ui_show_ask_alarm(const char *reason);

/* 撤下询问页（幂等：没有页面时什么都不做，不回调） */
void robot_ui_close_ask_alarm(void);

/* 等价于用户在询问页上点了按钮：撤页面 + 回调 ask_result_cb()。
 * MQTT 的 confirm_alarm 和 20 秒超时都走这里（"等价于点按钮"）。
 * 返回 0 = 已受理；-1 = 当前没有询问页。 */
int robot_ui_ask_alarm_answer(int confirmed);

/* 询问页现在开着没有（跨线程只读一个指针） */
bool robot_ui_ask_alarm_active(void);

/* ★ 跨线程入口：任何线程（声音检测线程 / MQTT 线程 / hello_app）都能调它来
 * "检测到异常，先问用户一句"。实现在 app/robot_ui/main.c，内部：
 *   ① 记账（同一时间只留一条 pending；同一 reason 在否认静默期内直接丢弃）
 *   ② 把询问页投到 LVGL 线程（ui_post_panel -> robot_ui_show_ask_alarm）
 *   ③ 起一条 20 秒超时看门狗（到点按「不用了」处理，避免演示卡死在询问页）
 * 返回值刻意不做：调用方（检测器）不需要关心界面到底弹没弹出来 ——
 * 屏幕坏了的时候它本来就不该关心，MQTT 和超时那两条路照样生效。 */
void ui_post_ask_alarm(const char *reason);

/* ★ 报警去重闸（实现在 main.c）：一次异常事件只允许报警一次。
 *
 * 同一次异常有两条确认入口，两条都要保留：屏幕按钮 / MQTT confirm_alarm（报
 * sound_abnormal），以及 hello_app 的语音追问（"救命/疼"这类回答，报
 * sound_emergency）。用户先点屏幕再喊一句"救命"，同一次异常就会上报两遍。
 * 所以每条确认入口在**真正执行报警之前**先来这里领票：
 *   true  = 这次报警由本路执行；
 *   false = 另一条确认入口已经报过了，本路整块放弃（页面/铃声/上报都不做）
 *           并打日志。
 * "一次事件"= 从 ui_post_ask_alarm() 那次询问算起，下一次新异常来了闸门自动重开
 * （内部是事件序号 + 已领票序号，见 main.c 里那一节）。任何线程可调。 */
bool robot_ui_alarm_claim(const char *src);

#endif /* ROBOT_UI_H */
