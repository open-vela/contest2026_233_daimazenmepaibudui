/**
 * touch_ui.h - 老人友好触摸交互界面
 * SF32LB52-DevKit-LCD LVGL 界面开发
 */

#ifndef TOUCH_UI_H
#define TOUCH_UI_H

#include <lvgl.h>
#include <stdint.h>
#include <stdbool.h>

/* ==================== 菜单类型 ==================== */
typedef enum {
    MENU_TYPE_MAIN,         // 主菜单
    MENU_TYPE_REMIND,       // 提醒菜单
    MENU_TYPE_SETTING,      // 设置菜单
    MENU_TYPE_ABOUT,        // 关于
    MENU_TYPE_MAX
} menu_type_t;

/* ==================== 模式类型 ==================== */
typedef enum {
    MODE_NORMAL,            // 正常模式
    MODE_LISTENING,         // 监听模式
    MODE_SLEEP,             // 休眠模式
    MODE_ALARM,             // 报警模式
    MODE_MAX
} robot_mode_t;

/* ==================== 设置项 ==================== */
typedef struct {
    uint8_t volume;         // 音量 0-100
    uint8_t brightness;     // 亮度 0-100
    bool auto_remind;       // 自动提醒开关
    uint16_t remind_interval; // 提醒间隔（分钟）
} settings_t;

/* ==================== 初始化 ==================== */
void touch_ui_init(void);

/* ==================== 菜单操作 ==================== */
void touch_ui_show_menu(menu_type_t type);
void touch_ui_hide_menu(void);
void touch_ui_go_back(void);

/* ==================== 模式切换 ==================== */
void touch_ui_set_mode(robot_mode_t mode);
robot_mode_t touch_ui_get_mode(void);

/* ==================== 设置操作 ==================== */
settings_t* touch_ui_get_settings(void);
void touch_ui_save_settings(void);
void touch_ui_show_setting_detail(const char *title, const char *content);

/* ==================== 提醒操作 ==================== */
void touch_ui_add_reminder(const char *title, const char *time);
void touch_ui_show_reminder_list(void);
void touch_ui_clear_reminders(void);

/* 提醒列表被"不是界面"的入口改了（语音的 add_reminder 工具、MQTT 下发）：
 * 如果用户此刻正停在提醒菜单上，就地重画一遍列表。其它画面一律不动 ——
 * 尤其不能把语音聊天弹窗当"用户离开了"收掉（touch_ui_show_menu 会那么做）。
 * **任何线程都能调**：内部走 lv_async_call 投到 LVGL 线程。 */
void touch_ui_notify_reminders_changed(void);

/* ==================== 触摸反馈 ==================== */
void touch_ui_vibrate(int duration_ms);
void touch_ui_play_sound(const char *sound_type);

/* ==================== 功能回调（由 main.c 注册） ==================== */
typedef void (*voice_chat_start_cb_t)(void *user_data);
typedef void (*emergency_call_cb_t)(void *user_data);

void touch_ui_set_voice_chat_cb(voice_chat_start_cb_t cb, void *user_data);
void touch_ui_set_emergency_cb(emergency_call_cb_t cb, void *user_data);

/* 设置里拖动「音量」滑块时回调（在 LVGL 线程里被调，里面不能阻塞）。
 * 音量本身是音频硬件的事，界面这层只存了数字 —— 必须由 main.c 接过去
 * 调 audio_set_volume()，否则滑块拖了等于没拖。 */
typedef void (*volume_set_cb_t)(int volume, void *user_data);

void touch_ui_set_volume_cb(volume_set_cb_t cb, void *user_data);

/* ==================== 语音聊天弹窗（AI 对话闭环） ==================== */

/* 点「提交」：main.c 去跑 ASR -> LLM -> TTS（本回调在 LVGL 线程里调，
 * 里面不能做网络请求，要自己开工作线程）。 */
typedef void (*voice_submit_cb_t)(void *user_data);

/* 点「×」：关窗前回调，main.c 在这里停录音、丢弃这一轮的音频。 */
typedef void (*voice_cancel_cb_t)(void *user_data);

/* 打开弹窗（同时把界面重置成"正在录音"），并回调 voice_chat_start_cb_t
 * 让 main.c 去 audio_record_start()。重复调用会先把上一个弹窗收干净。 */
void touch_ui_show_voice_chat(void);

/* 关窗（不回调取消）。关闭后当前会话的世代号会变，在途的工作线程结果自动作废。 */
void touch_ui_hide_voice_chat(void);

/* 弹窗是否开着（可跨线程读，用于录音线程/主循环判断） */
bool touch_ui_voice_chat_active(void);

/* 当前语音会话的世代号。每次开窗 / 「再说一次」/ 关窗都会 +1。
 * 工作线程在开工时取一次，之后每次要动控件或放音频前都对一下：
 * 对不上说明用户已经关窗或又开了一轮，这一轮的结果必须整份丢掉。 */
uint32_t touch_ui_voice_chat_generation(void);

void touch_ui_set_voice_submit_cb(voice_submit_cb_t cb, void *user_data);
void touch_ui_set_voice_cancel_cb(voice_cancel_cb_t cb, void *user_data);

/* ==================== 语音镜像面板（纯显示，不开麦克风） ==================== */
/*
 * 语音入口已经迁到框架侧（hello_app 的 ai_companion：常开麦克风跑 VAD -> ASR ->
 * 大模型 -> TTS），robot_ui 这边**不再开麦**，只负责把"它在不在听、什么时候回答"
 * 显示出来 —— 用户原话：「没有提交按钮？我怎么知道他在听他什么时候回复」。
 *
 * 这个面板和 touch_ui_show_voice_chat() 长得一模一样，区别是：
 *   1) 没有录音计时（那一行根本不创建）；
 *   2) 底部大按钮是「**提交**」：请框架侧 ai_companion 立刻收尾当前这一段录音
 *      送去识别（不等 VAD 那 3 秒静音超时），**不会**回调 voice_chat_start_cb_t
 *      —— 那条路是去 audio_record_start() 的，半双工设备上和 ai_companion 抢麦。
 *      关闭在右上角那个「×」上（voice_close_event_handler，本来就有的）；
 *   3) 状态行默认「录制中」；
 *   4) 对话区是**双方对话历史**：用户的发言（"你说：…"，小一号浅蓝）和智爱的
 *      回复（"智爱：…"，白色）一行行往下排，只保留最近几轮，能往上划回看
 *      —— 用户原话：「又看不到回复又看不到自己说了什么」。
 *
 * 状态行 / 对话区用 touch_ui_set_voice_state() / touch_ui_set_voice_user_text() /
 * touch_ui_set_voice_reply() 更新，都走 lv_async_call，任何线程可调。
 * 任何线程也能调这个打开函数（内部投递）。
 */
void touch_ui_show_voice_mirror(void);

/* 镜像面板底部「提交」：main.c 在这里请框架侧 ai_companion 立刻收尾当前这一段
 * 录音（它才是常开麦那一方，面板只是显示器）。
 *
 * ⚠️ 回调在 **LVGL 线程**里被调，里面**只能登记请求、立刻返回**：音频设备归
 * hello_app，跨 app 同步动设备出过整组死掉的事故（见
 * app/hello_app/ai_companion_req.h 头上那一节）。没有可提交的语音时，由
 * main.c 在回调里顺手在面板状态行提示一句（touch_ui_set_voice_status()），
 * 这个分支里什么都不用管。 */
typedef void (*voice_mirror_submit_cb_t)(void *user_data);

void touch_ui_set_voice_mirror_submit_cb(voice_mirror_submit_cb_t cb,
                                         void *user_data);

/* 底部「重置」：卡死时用户唯一的自救入口。
 *
 * 语义、以及"为什么只投一个请求、剩下交给 board 侧看护"写在
 * app/hello_app/ai_companion_req.h 里那段（自动接管已经删掉，机器不猜"它卡了没"，
 * 由人按）。回调同样在 **LVGL 线程**里被调：里面只许 `ai_companion_request_takeover()`
 * 那一下（置一个位）然后立刻返回 —— 不碰设备、不等任何人。 */
typedef void (*voice_mirror_reset_cb_t)(void *user_data);

void touch_ui_set_voice_mirror_reset_cb(voice_mirror_reset_cb_t cb,
                                        void *user_data);

/* 镜像面板状态行的四种状态：听=蓝、想=橙、说=绿、空闲=灰（颜色比字更早看出在干什么） */
typedef enum {
    TOUCH_VOICE_STATE_IDLE = 0,     /* 「录制中」灰 */
    TOUCH_VOICE_STATE_LISTENING,    /* 「检测到声音」蓝 */
    TOUCH_VOICE_STATE_THINKING,     /* 「正在想…」橙 */
    TOUCH_VOICE_STATE_SPEAKING      /* 「正在说话…」绿 */
} touch_voice_state_t;

/* 改状态行的文字 + 颜色（其它行不动）。面板没开着时什么都不做，任何线程可调。
 * 与 touch_ui_set_voice_status() 的关系：那个是"写任意一句话、颜色不变"，
 * 这个是按状态写固定的话 + 换颜色；两个都能用，后写的盖前写的。 */
void touch_ui_set_voice_state(touch_voice_state_t state);

/* 下面这些是给工作线程用的（内部走 lv_async_call 投到 LVGL 线程，
 * 所以任何线程都能调，但不要在 LVGL 线程里忙等） */

/* 弹窗里的状态行（"正在识别…" / "识别失败" / "AI 无回复" ...） */
void touch_ui_set_voice_status(const char *text);

/* 显示用户刚才说的话（ASR 原文）。面板没开时空操作。
 * 语义：它开一条新的"你说：…"（一轮对话的开始），随后的
 * touch_ui_set_voice_reply() 收尾成这一轮的"智爱：…"，一轮一轮往下堆成历史 ——
 * 镜像面板只保留最近几轮（写满就丢最老的），所以行数有上限、不会撑破面板。
 * 入参约定：必须是**清洗过的显示文本**（先过 robot_ui/main.c 的
 * sanitize_for_display()）。那个函数是 main.c 里的 static，touch_ui.c 够不到，
 * 字形过滤这一层只能在调用方做；touch_ui.c 只负责长度封顶和控制字符。 */
void touch_ui_set_voice_user_text(const char *text);

/* 对话区里"智爱：…"那一行：收尾上面那一轮；没有在等收尾的轮就自己开一轮。
 * 可换行、可滚动。面板没开着时空操作（内部 lv_async_call 投到 LVGL 线程）。
 * 入参同 touch_ui_set_voice_user_text()：要是清洗过的显示文本。 */
void touch_ui_set_voice_reply(const char *text);

/* 一轮对话结束（成功或失败）：按钮变回可点的「再说一次」，
 * 用户不必关窗重开就能接着聊。 */
void touch_ui_voice_chat_round_done(void);

/* 停掉录音计时（例如录满 10 秒自动收尾时） */
void touch_ui_voice_chat_stop_timer(void);

/* ==================== 关怀确认面板 ==================== */
typedef enum {
    TOUCH_CHECKIN_WAITING = 2,
    TOUCH_CHECKIN_SENDING = 5,
    TOUCH_CHECKIN_SENT    = 6,
    TOUCH_CHECKIN_FAILED  = 7
} touch_checkin_state_t;

typedef void (*checkin_btn_cb_t)(uint64_t checkin_id, bool needs_help, void *user_data);

void touch_ui_show_checkin(uint64_t checkin_id, uint32_t timeout_ms,
                           checkin_btn_cb_t cb, void *user_data);
void touch_ui_update_checkin_state(touch_checkin_state_t state);
void touch_ui_hide_checkin(void);

/* 给 lv_msgbox 加右上角「×」关闭按钮（不用 LVGL 内置符号字体，那不在本工程字库里） */
void touch_ui_msgbox_add_close_x(lv_obj_t *mbox);

/* ==================== 摔倒询问面板（「您摔到了吗？」+ 有/没有） ==================== */
/*
 * 出处：main.c 的「疑似摔倒事件链」（fall_alarm_trigger()）。检测器报"疑似摔倒"
 * 之后，板子要同时：弹窗问一句、语音问一句、等用户回答（点按钮或说话）。
 *
 * 和上面「关怀确认面板」的关系：那是给主动关怀用的（我没事 / 需要帮助），
 * 这一层是给摔倒确认用的（没有 / 有）。**刻意不复用同一个面板**：
 *   - 文案和语义不同（关怀是"您还好吗"，摔倒是"您摔到了吗"），
 *     共用一个 label 会让两处都要按参数分叉；
 *   - 关怀那条路在 companion 侧还有 ai_checkin_* 要落地，动它会牵一片。
 * 结构（面板 + 两个大按钮 + 状态行）是从关怀面板抄的，那套写法已经上板验证过
 * 「点得动、关得掉」：**面板本身是活动屏上的普通容器**，没有全屏 backdrop
 * 去吃触摸，按钮就是面板的直接孩子。
 *
 * 三个入口都**任何线程可调**（内部走 ui_async_call 投到 LVGL 线程，见 ui_async.h），
 * 但都不许在中断上下文调用。
 */

/* 用户点的是哪个按钮。
 * 语义对应 main.c 的三条回答路径：点按钮、语音说"没有"、语音说"有"。
 * 命名不用 yes/no 而用 NO/YES 对应中文的「没有 / 有」。 */
typedef enum {
    TOUCH_FALL_ANSWER_NO  = 0,   /* 「没有」-> 取消警报 */
    TOUCH_FALL_ANSWER_YES = 1    /* 「有」  -> 正式报警 */
} touch_fall_answer_t;

/* 点按钮的回调。
 * ⚠️ **在 LVGL 线程里被调**：里面只能置标志、立刻返回，不许阻塞
 * （真正的动作在 main.c 的摔倒链工作线程里做）。 */
typedef void (*fall_answer_cb_t)(touch_fall_answer_t answer, void *user_data);

void touch_ui_set_fall_answer_cb(fall_answer_cb_t cb, void *user_data);

/* 弹面板（任何线程可调）。重复调用会先把上一块收干净，不会叠罗汉。 */
void touch_ui_show_fall_ask(void);

/* 撤面板（任何线程可调）。取消警报、进了报警页都要调它。 */
void touch_ui_hide_fall_ask(void);

/* 改状态行（任何线程可调；面板没开着时空操作）。 */
void touch_ui_set_fall_status(const char *text);

/* 面板此刻开着没有（可跨线程读，用于日志/判重） */
bool touch_ui_fall_ask_active(void);

#endif /* TOUCH_UI_H */
