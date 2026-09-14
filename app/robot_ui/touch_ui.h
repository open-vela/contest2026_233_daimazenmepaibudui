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

/* 下面这些是给工作线程用的（内部走 lv_async_call 投到 LVGL 线程，
 * 所以任何线程都能调，但不要在 LVGL 线程里忙等） */

/* 弹窗里的状态行（"正在识别…" / "识别失败" / "AI 无回复" ...） */
void touch_ui_set_voice_status(const char *text);

/* 对话区的文字（识别结果与 AI 回复，可换行、可滚动） */
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

#endif /* TOUCH_UI_H */
