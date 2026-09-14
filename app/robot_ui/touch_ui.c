/**
 * touch_ui.c - 老人友好触摸交互界面实现
 * 针对老人使用习惯优化
 */

#include "touch_ui.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 板级亮度封装（头文件在 board/contest_board/src，路径由 CMakeLists.txt 的
 * INCLUDE_DIRECTORIES ${NUTTX_BOARD_ABS_DIR}/src 提供）。亮度滑块只调它，
 * 不自己 open("/dev/lcd0") 拼 ioctl。 */
#include "sf32lb52_backlight.h"

/* 提醒的存与调度（app/robot_ui/reminder_sched.c）：
 * 提醒列表的数据放在那里，界面只在"加 / 删 / 查"时调它的接口，
 * 不再自己存一份（以前那份 static reminder_t reminders[] 只会显示、到点不响）。 */
#include "reminder_sched.h"

/* 中文字库（实现在 lv_font_ui_16/20/24.c，见 CMakeLists.txt 的 SRCS）。
 * 字符集是常用汉字全集（GB2312 6763 字 + ASCII + CJK 标点 + 全角），
 * 按改动前 montserrat 的字号分三档：
 *   14/16/18 -> lv_font_ui_16   20/22/24 -> lv_font_ui_20   >=28 -> lv_font_ui_24 */
LV_FONT_DECLARE(lv_font_ui_16);
LV_FONT_DECLARE(lv_font_ui_20);
LV_FONT_DECLARE(lv_font_ui_24);

/* ==================== 全局变量 ==================== */
static lv_obj_t *current_screen = NULL;
static lv_obj_t *menu_panel = NULL;
static lv_obj_t *setting_panel = NULL;
/* "新建提醒"面板（提醒列表上按「+ 新增提醒」弹出来的那一层）。
 * 它和 menu_panel / setting_panel 一样是活动屏的孩子，删的时候要一起删。 */
static lv_obj_t *new_reminder_panel = NULL;

/* 右滑手势状态 */
static int32_t swipe_start_x = 0;
static bool swipe_tracking = false;
/* 同一次滑动可能被屏幕和输入设备各投递一次，用它去重 */
static bool swipe_handled = false;

/* 当前菜单层级：决定"返回 / 右滑"该回哪一级。
 * 主菜单那一层再返回就关掉浮层（回到主界面），子菜单则回主菜单。
 * 注：touch_ui_hide_menu() 以前没有任何调用点，菜单浮层因此没有出口，
 *     现场表现就是"进了菜单回不去"。 */
static menu_type_t current_menu_type = MENU_TYPE_MAIN;

/* 当前状态 */
static robot_mode_t current_mode = MODE_NORMAL;
static settings_t user_settings = {
    .volume = 70,
    .brightness = 80,
    .auto_remind = true,
    .remind_interval = 60
};

/* 提醒数据都在 reminder_sched.c（reminder_item_t / reminder_sched_*），
 * 这里只留"新建提醒"面板正在编辑的那几个值。
 *
 * 面板布局（老人机，屏幕 390x450）：
 *   标题 [新建提醒]                                   [×]
 *   [吃药] [喝水]
 *   [散步] [起床]                 <- 预设标题，选中的绿色
 *   [-]   08时   [+]
 *   [-]   00分   [+]
 *   [ 保存 ]
 */
#define NEW_REMINDER_PRESET_COUNT 4
#define NEW_REMINDER_MIN_STEP     5      /* 分钟步进 5 分钟 */

static const char *new_reminder_presets[NEW_REMINDER_PRESET_COUNT] = {
    "吃药", "喝水", "散步", "起床"
};

static lv_obj_t *new_reminder_preset_btns[NEW_REMINDER_PRESET_COUNT];
/* [0] = 时的显示、[1] = 分的显示 */
static lv_obj_t *new_reminder_step_lbls[2];
static int new_reminder_hour = 8;
static int new_reminder_min  = 0;
static int new_reminder_preset = 0;

/* 关怀确认面板静态变量 */
static lv_obj_t *checkin_panel = NULL;
static lv_obj_t *checkin_status_lbl = NULL;
static lv_obj_t *checkin_btn_fine = NULL;
static lv_obj_t *checkin_btn_help = NULL;
static uint64_t checkin_current_id = 0;
static bool checkin_confirmed = false;
static checkin_btn_cb_t checkin_cb = NULL;
static void *checkin_cb_user_data = NULL;

/* 功能回调（由 main.c 注册） */
static voice_chat_start_cb_t g_voice_chat_cb = NULL;
static void *g_voice_chat_user_data = NULL;
static emergency_call_cb_t g_emergency_cb = NULL;
static void *g_emergency_user_data = NULL;
static volume_set_cb_t g_volume_cb = NULL;
static void *g_volume_user_data = NULL;

/* 语音聊天弹窗（见文件后半 "语音聊天弹窗" 一节） */
static lv_obj_t *voice_panel = NULL;
static lv_obj_t *voice_timer_lbl = NULL;
static lv_obj_t *voice_status_lbl = NULL;
static lv_obj_t *voice_reply_box = NULL;
static lv_obj_t *voice_reply_lbl = NULL;
static lv_obj_t *voice_submit_btn = NULL;
static lv_obj_t *voice_submit_lbl = NULL;
static lv_timer_t *voice_tick_timer = NULL;
static uint32_t voice_start_tick = 0;

/* 提交按钮的三态：可提交 -> 处理中(禁用) -> 可再来一轮 */
typedef enum {
    VOICE_BTN_SUBMIT = 0,
    VOICE_BTN_BUSY,
    VOICE_BTN_RETRY
} voice_btn_state_t;
static voice_btn_state_t voice_btn_state = VOICE_BTN_SUBMIT;

/* 会话世代号：开窗 /「再说一次」/ 关窗都 +1。
 * 工作线程在途的结果回来后一比对就知道该不该丢弃，不需要去 join 它，
 * 也不会往已经删掉的控件上写字。工作线程只读，声明成 volatile。 */
static volatile uint32_t voice_generation = 0;

static voice_submit_cb_t g_voice_submit_cb = NULL;
static void *g_voice_submit_user_data = NULL;
static voice_cancel_cb_t g_voice_cancel_cb = NULL;
static void *g_voice_cancel_user_data = NULL;

/* 设置持久化文件路径 */
#define SETTINGS_FILE_PATH "/data/zhi_ai_settings.dat"
#define SETTINGS_FILE_MAGIC 0x5A414953  /* "ZAIS" */
#define SETTINGS_FILE_VERSION 1

/* ==================== 样式定义 ==================== */

/* 老人友好样式 - 大字体、高对比度 */
static lv_style_t style_elder;
static lv_style_t style_big_btn;
static lv_style_t style_menu_item;
static lv_style_t style_back_btn;
static lv_style_t style_slider;
static lv_style_t style_switch;

/* ==================== 内部函数前向声明 ==================== */
static void init_elder_styles(void);
static void create_menu_panel(menu_type_t type);
static void create_setting_panel(void);
static void create_new_reminder_panel(void);
static void create_back_button(lv_obj_t *parent);
static void create_menu_item(lv_obj_t *parent, const char *icon_text,
                            const char *subtitle, int index);
static void create_reminder_add_button(lv_obj_t *parent);
static void create_reminder_list_items(lv_obj_t *parent);
static void create_reminder_item(lv_obj_t *parent, const char *title,
                                const char *time_str, int index);
static void create_step_row(lv_obj_t *parent, int field, int value,
                           const char *unit);
static void new_reminder_refresh_time(void);
static void new_reminder_refresh_preset(void);
static void reminder_add_event_handler(lv_event_t *e);
static void new_reminder_close_event_handler(lv_event_t *e);
static void new_reminder_preset_event_handler(lv_event_t *e);
static void new_reminder_step_event_handler(lv_event_t *e);
static void new_reminder_save_event_handler(lv_event_t *e);
static void create_slider_setting(lv_obj_t *parent, const char *title,
                                 int value, int index);
static void create_switch_setting(lv_obj_t *parent, const char *title,
                                 bool value, int index);
static void create_interval_setting(lv_obj_t *parent, const char *title,
                                   uint16_t interval, int index);
static void create_about_info(lv_obj_t *parent);
static void menu_item_event_handler(lv_event_t *e);
static void back_button_event_handler(lv_event_t *e);
static void setting_slider_event_handler(lv_event_t *e);
static void setting_switch_event_handler(lv_event_t *e);
static void reminder_item_event_handler(lv_event_t *e);
static void confirm_dialog_event_handler(lv_event_t *e);
static void setting_reset_event_handler(lv_event_t *e);
static void interval_button_event_handler(lv_event_t *e);
static void show_confirm_dialog(const char *title, const char *content,
                               lv_event_cb_t callback);
static void checkin_btn_fine_handler(lv_event_t *e);
static void checkin_btn_help_handler(lv_event_t *e);
static void reminder_delete_event_handler(lv_event_t *e);
static void settings_save_to_file(void);
static void settings_load_from_file(void);
static void screen_gesture_event_handler(lv_event_t *e);

/* 语音聊天弹窗内部函数 */
static void voice_close_event_handler(lv_event_t *e);
static void voice_submit_event_handler(lv_event_t *e);
static void voice_tick_timer_cb(lv_timer_t *t);
static void voice_set_btn_state(voice_btn_state_t state);
static void voice_begin_round(void);
static void voice_start_timer(void);
static void voice_stop_timer(void);
static void voice_post_text(const char *text, bool is_reply);
static void voice_round_done_async(void *arg);

/* ==================== 初始化老人友好样式 ==================== */
static void init_elder_styles(void)
{
    /* 老人友好基础样式 */
    lv_style_init(&style_elder);
    lv_style_set_bg_color(&style_elder, lv_color_hex(0x1A1A2E));
    lv_style_set_bg_opa(&style_elder, LV_OPA_COVER);
    lv_style_set_text_color(&style_elder, lv_color_hex(0xFFFFFF));
    lv_style_set_text_font(&style_elder, &lv_font_ui_20);
    lv_style_set_border_width(&style_elder, 0);
    lv_style_set_radius(&style_elder, 0);

    /* 大按钮样式 - 方便点击 */
    lv_style_init(&style_big_btn);
    lv_style_set_bg_color(&style_big_btn, lv_color_hex(0x4CAF50));
    lv_style_set_bg_opa(&style_big_btn, LV_OPA_COVER);
    lv_style_set_radius(&style_big_btn, 20);
    lv_style_set_shadow_width(&style_big_btn, 15);
    lv_style_set_shadow_color(&style_big_btn, lv_color_hex(0x388E3C));
    lv_style_set_text_color(&style_big_btn, lv_color_hex(0xFFFFFF));
    lv_style_set_text_font(&style_big_btn, &lv_font_ui_20);
    lv_style_set_pad_all(&style_big_btn, 20);

    /* 菜单项样式 */
    lv_style_init(&style_menu_item);
    lv_style_set_bg_color(&style_menu_item, lv_color_hex(0x2D2D44));
    lv_style_set_bg_opa(&style_menu_item, LV_OPA_COVER);
    lv_style_set_radius(&style_menu_item, 15);
    lv_style_set_border_width(&style_menu_item, 2);
    lv_style_set_border_color(&style_menu_item, lv_color_hex(0x4CAF50));
    lv_style_set_text_color(&style_menu_item, lv_color_hex(0xFFFFFF));
    lv_style_set_text_font(&style_menu_item, &lv_font_ui_20);
    lv_style_set_pad_all(&style_menu_item, 25);

    /* 返回按钮样式 */
    lv_style_init(&style_back_btn);
    lv_style_set_bg_color(&style_back_btn, lv_color_hex(0x607D8B));
    lv_style_set_bg_opa(&style_back_btn, LV_OPA_COVER);
    lv_style_set_radius(&style_back_btn, 25);
    lv_style_set_text_color(&style_back_btn, lv_color_hex(0xFFFFFF));
    lv_style_set_text_font(&style_back_btn, &lv_font_ui_16);

    /* 设置滑块样式 */
    lv_style_init(&style_slider);
    lv_style_set_bg_color(&style_slider, lv_color_hex(0x37474F));
    lv_style_set_radius(&style_slider, 10);

    /* 开关样式 */
    lv_style_init(&style_switch);
    lv_style_set_bg_color(&style_switch, lv_color_hex(0x4CAF50));
}

/* ==================== 初始化触摸交互 UI ==================== */
void touch_ui_init(void)
{
    /* 初始化样式 */
    init_elder_styles();

    /* 初始化提醒列表（数据在 reminder_sched.c：清空 + 复位调度状态） */
    reminder_sched_init();

    /* 新建提醒面板的编辑状态复位 */
    new_reminder_hour = 8;
    new_reminder_min = 0;
    new_reminder_preset = 0;
    for (int i = 0; i < NEW_REMINDER_PRESET_COUNT; i++) {
        new_reminder_preset_btns[i] = NULL;
    }
    new_reminder_step_lbls[0] = NULL;
    new_reminder_step_lbls[1] = NULL;

    /* 面板句柄复位: NuttX builtin 应用重新运行时 .bss 不清零,
     * 若残留上一次运行的面板指针, 之后 lv_obj_del() 会去删除已
     * 失效的 LVGL 对象而导致 hardfault */
    menu_panel = NULL;
    setting_panel = NULL;
    new_reminder_panel = NULL;

    /* 语音聊天弹窗同理：定时器与控件都只属于上一次那个 LVGL 实例，
     * 不复位就会删到野指针（这里只把句柄清空，不去 del 残留的） */
    voice_panel = NULL;
    voice_timer_lbl = NULL;
    voice_status_lbl = NULL;
    voice_reply_box = NULL;
    voice_reply_lbl = NULL;
    voice_submit_btn = NULL;
    voice_submit_lbl = NULL;
    voice_tick_timer = NULL;
    voice_btn_state = VOICE_BTN_SUBMIT;
    voice_generation = 0;

    /* 获取当前活动屏幕 */
    current_screen = lv_scr_act();
    lv_obj_add_style(current_screen, &style_elder, 0);

    /* 右滑手势：按下/抬起挂在**输入设备**上。
     *
     * 以前只挂在屏幕上，而主屏的子对象（表情区、各按钮、AI 回复框）在 LVGL 里
     * 默认会把触摸事件吃掉（不冒泡给父对象），所以手指落在那些区域上时，
     * 屏幕根本收不到 PRESSED/RELEASED —— 现象就是"怎么划都划不出菜单"。
     * indev 的事件只看按下/抬起，与命中的对象无关，因此一定能收到。
     * 屏幕上的注册保留着（同一次滑动可能两边都来，swipe_handled 去重）。 */
    lv_indev_t *indev = lv_indev_get_next(NULL);
    if (indev != NULL)
      {
        lv_indev_add_event_cb(indev, screen_gesture_event_handler,
                              LV_EVENT_PRESSED, NULL);
        lv_indev_add_event_cb(indev, screen_gesture_event_handler,
                              LV_EVENT_RELEASED, NULL);
        lv_indev_add_event_cb(indev, screen_gesture_event_handler,
                              LV_EVENT_CANCEL, NULL);
      }
    else
      {
        printf("[Gesture] 没找到输入设备，右滑只在屏幕上生效\n");
      }

    lv_obj_add_event_cb(current_screen, screen_gesture_event_handler,
                        LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(current_screen, screen_gesture_event_handler,
                        LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(current_screen, screen_gesture_event_handler,
                        LV_EVENT_CANCEL, NULL);

    /* 加载持久化设置 */
    settings_load_from_file();

    printf("touch_ui init done\n");
}

/* ==================== 显示菜单 ==================== */
void touch_ui_show_menu(menu_type_t type)
{
    /* 要出菜单了，说明用户已经离开语音聊天：按「×」同样的收尾走一遍
     * （停录音 + 丢弃这一轮），否则会留下一个还在录音、却被菜单盖住的弹窗
     * —— 麦克风被占着，谁也录不了。 */
    if (voice_panel != NULL) {
        if (g_voice_cancel_cb != NULL) {
            g_voice_cancel_cb(g_voice_cancel_user_data);
        }
        touch_ui_hide_voice_chat();
    }

    /* 清除所有旧面板. 注意 setting_panel 是活动屏的兄弟节点,
     * 不挂在 menu_panel 下, 只删 menu_panel 会遗留旧设置面板,
     * 导致新旧面板重叠且整棵对象树泄漏 */
    if (menu_panel) {
        lv_obj_del(menu_panel);
        menu_panel = NULL;
    }
    if (setting_panel) {
        lv_obj_del(setting_panel);
        setting_panel = NULL;
    }
    /* 新建提醒面板同理：它是提醒列表上叠的一层，重画列表时要一起清掉 */
    if (new_reminder_panel) {
        lv_obj_del(new_reminder_panel);
        new_reminder_panel = NULL;
        new_reminder_step_lbls[0] = NULL;
        new_reminder_step_lbls[1] = NULL;
        for (int i = 0; i < NEW_REMINDER_PRESET_COUNT; i++) {
            new_reminder_preset_btns[i] = NULL;
        }
    }

    /* 记住当前层级：返回按钮 / 右滑手势靠它决定回上一级还是关掉浮层 */
    current_menu_type = type;

    /* 创建菜单面板 */
    create_menu_panel(type);
}

/* ==================== 隐藏菜单 ==================== */
void touch_ui_hide_menu(void)
{
    if (menu_panel) {
        lv_obj_del(menu_panel);
        menu_panel = NULL;
    }
}

/* ==================== 返回上一级 ==================== */
void touch_ui_go_back(void)
{
    /* 隐藏所有面板 */
    touch_ui_hide_menu();

    if (setting_panel) {
        lv_obj_del(setting_panel);
        setting_panel = NULL;
    }

    if (new_reminder_panel) {
        lv_obj_del(new_reminder_panel);
        new_reminder_panel = NULL;
        new_reminder_step_lbls[0] = NULL;
        new_reminder_step_lbls[1] = NULL;
        for (int i = 0; i < NEW_REMINDER_PRESET_COUNT; i++) {
            new_reminder_preset_btns[i] = NULL;
        }
    }

    /* 播放返回音效 */
    touch_ui_play_sound("back");
}

/* ==================== 创建菜单面板 ==================== */
static void create_menu_panel(menu_type_t type)
{
    /* 菜单容器 */
    menu_panel = lv_obj_create(current_screen);
    lv_obj_set_size(menu_panel, LV_PCT(95), LV_PCT(85));
    lv_obj_align(menu_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_style(menu_panel, &style_elder, 0);
    lv_obj_set_flex_flow(menu_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(menu_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(menu_panel, 20, 0);
    lv_obj_set_style_pad_row(menu_panel, 15, 0);

    /* 标题 */
    lv_obj_t *title = lv_label_create(menu_panel);
    switch (type) {
        case MENU_TYPE_MAIN:
            lv_label_set_text(title, "[主] 主菜单");
            break;
        case MENU_TYPE_REMIND:
            lv_label_set_text(title, "[提] 提醒");
            break;
        case MENU_TYPE_SETTING:
            lv_label_set_text(title, "[设] 设置");
            break;
        case MENU_TYPE_ABOUT:
            lv_label_set_text(title, "[?] 关于");
            break;
        default:
            lv_label_set_text(title, "菜单");
            break;
    }
    lv_obj_set_style_text_font(title, &lv_font_ui_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFEB3B), 0);
    lv_obj_set_style_pad_bottom(title, 20, 0);

    /* 根据菜单类型创建内容 */
    switch (type) {
        case MENU_TYPE_MAIN:
            create_menu_item(menu_panel, "[语] 语音聊天", "与机器人对话", 0);
            create_menu_item(menu_panel, "[提] 查看提醒", "查看今日提醒", 1);
            create_menu_item(menu_panel, "[设] 设置", "音量、亮度等", 2);
            create_menu_item(menu_panel, "[急] 紧急呼叫", "联系家人", 3);
            create_menu_item(menu_panel, "[?] 关于", "版本信息", 4);
            break;

        case MENU_TYPE_REMIND:
            /* 新增按钮放**最上面**：老人不用先滚一圈才能找到它 */
            create_reminder_add_button(menu_panel);
            create_reminder_list_items(menu_panel);
            break;

        case MENU_TYPE_SETTING:
            create_setting_panel();
            break;

        case MENU_TYPE_ABOUT:
            create_about_info(menu_panel);
            break;

        default:
            break;
    }

    /* 返回按钮 */
    create_back_button(menu_panel);
}

/* ==================== 创建菜单项 ==================== */
static void create_menu_item(lv_obj_t *parent, const char *icon_text,
                            const char *subtitle, int index)
{
    /* 菜单项按钮 - 用 lv_btn 确保触摸事件可响应 */
    lv_obj_t *item = lv_btn_create(parent);
    lv_obj_set_size(item, LV_PCT(100), 80);
    lv_obj_add_style(item, &style_menu_item, 0);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(item, menu_item_event_handler, LV_EVENT_CLICKED, (void *)(intptr_t)index);

    /* 图标和标题 */
    lv_obj_t *icon_label = lv_label_create(item);
    lv_label_set_text(icon_label, icon_text);
    lv_obj_set_style_text_font(icon_label, &lv_font_ui_24, 0);

    /* 副标题 */
    lv_obj_t *sub_label = lv_label_create(item);
    lv_label_set_text(sub_label, subtitle);
    lv_obj_set_style_text_color(sub_label, lv_color_hex(0x9E9E9E), 0);
    lv_obj_set_style_text_font(sub_label, &lv_font_ui_24, 0);
    lv_obj_set_style_pad_left(sub_label, 15, 0);

    /* 右箭头 */
    lv_obj_t *arrow = lv_label_create(item);
    lv_label_set_text(arrow, ">");
    lv_obj_set_style_text_font(arrow, &lv_font_ui_24, 0);
    lv_obj_set_style_text_color(arrow, lv_color_hex(0x9E9E9E), 0);
}

/* ==================== 创建「新增提醒」按钮 ==================== */
/* 提醒列表最上面那条大绿色按钮 */
static void create_reminder_add_button(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, LV_PCT(100), 64);
    lv_obj_add_style(btn, &style_big_btn, 0);
    lv_obj_add_event_cb(btn, reminder_add_event_handler, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    /* 字库里没有 U+FF0B 全角加号，"+" 用 ASCII 的（字库里有） */
    lv_label_set_text(lbl, "+ 新增提醒");
    lv_obj_set_style_text_font(lbl, &lv_font_ui_24, 0);
    lv_obj_center(lbl);
}

/* ==================== 创建提醒列表 ==================== */
static void create_reminder_list_items(lv_obj_t *parent)
{
    reminder_item_t items[REMINDER_MAX_ITEMS];
    int count = reminder_sched_snapshot(items, REMINDER_MAX_ITEMS);

    if (count == 0) {
        /* 空提醒 */
        lv_obj_t *empty = lv_label_create(parent);
        lv_label_set_text(empty, "暂无提醒\n\n点上面的「+ 新增提醒」");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x9E9E9E), 0);
        lv_obj_set_style_text_font(empty, &lv_font_ui_24, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(empty, 30, 0);
    } else {
        /* 显示提醒列表：下标就是 reminder_sched_* 里的下标 */
        for (int i = 0; i < count; i++) {
            char time_str[8];

            if (!items[i].enabled) {
                continue;
            }

            snprintf(time_str, sizeof(time_str), "%02d:%02d",
                     items[i].hour, items[i].min);
            create_reminder_item(parent, items[i].title, time_str, i);
        }
    }
}

/* ==================== 创建提醒项 ==================== */
static void create_reminder_item(lv_obj_t *parent, const char *title,
                                const char *time_str, int index)
{
    lv_obj_t *item = lv_btn_create(parent);
    lv_obj_set_size(item, LV_PCT(100), 70);
    lv_obj_add_style(item, &style_menu_item, 0);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(item, reminder_item_event_handler, LV_EVENT_CLICKED, (void *)(intptr_t)index);

    /* 提醒时间 */
    lv_obj_t *time_label = lv_label_create(item);
    lv_label_set_text(time_label, time_str);
    lv_obj_set_style_text_color(time_label, lv_color_hex(0xFF9800), 0);
    lv_obj_set_style_text_font(time_label, &lv_font_ui_24, 0);

    /* 提醒标题 */
    lv_obj_t *title_label = lv_label_create(item);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, &lv_font_ui_24, 0);
    lv_obj_set_style_pad_left(title_label, 15, 0);

    /* 右侧删除按钮 "×" */
    lv_obj_t *del_btn = lv_btn_create(item);
    lv_obj_set_size(del_btn, 50, 50);
    lv_obj_set_style_bg_color(del_btn, lv_color_hex(0xF44336), 0);
    lv_obj_set_style_radius(del_btn, 25, 0);
    lv_obj_set_style_pad_all(del_btn, 0, 0);
    lv_obj_align(del_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(del_btn, reminder_delete_event_handler,
                        LV_EVENT_CLICKED, (void *)(intptr_t)index);

    lv_obj_t *del_label = lv_label_create(del_btn);
    /* U+00D7 "×"，字库里有；别用 "x"（看着像字母）也别用 LV_SYMBOL_CLOSE
     * （符号字体不在本工程的三档字库里，会画成空心方块） */
    lv_label_set_text(del_label, "×");
    lv_obj_set_style_text_font(del_label, &lv_font_ui_24, 0);
    lv_obj_set_style_text_color(del_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(del_label);
}

/* ==================== 创建「新建提醒」面板 ==================== */
/*
 * 提醒列表上按「+ 新增提醒」弹出来的这一层。三件事：选标题、定时刻、保存。
 * 面板是活动屏的兄弟节点（和设置/关怀面板一样），关掉它就走
 * touch_ui_show_menu(MENU_TYPE_REMIND) —— 那条路会把本面板删掉并重画列表，
 * 所以这里不用自己管"关窗"的清理。
 */

/* 一行步进按钮：[-] 大字 [+]；field 0 = 时、1 = 分 */
static void create_step_row(lv_obj_t *parent, int field, int value,
                           const char *unit)
{
    const char *labels[2] = { "-", "+" };
    const int   deltas[2] = { -1, 1 };

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 58);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 14, 0);

    /* [-] 和 [+]。中间插一个大字显示，顺序：- / 显示 / + */
    for (int i = 0; i < 2; i++) {
        lv_obj_t *btn;

        if (i == 1) {
            /* 中间的值（"08时" / "00分"） */
            lv_obj_t *value_lbl = lv_label_create(row);
            lv_label_set_text_fmt(value_lbl, "%02d%s", value, unit);
            lv_obj_set_width(value_lbl, 130);
            lv_obj_set_style_text_align(value_lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(value_lbl, &lv_font_ui_24, 0);
            lv_obj_set_style_text_color(value_lbl, lv_color_hex(0xFF9800), 0);
            new_reminder_step_lbls[field] = value_lbl;
        }

        btn = lv_btn_create(row);
        lv_obj_set_size(btn, 84, 54);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x455A64), 0);
        lv_obj_set_style_radius(btn, 12, 0);
        /* user_data 编码：高 8 位 = 字段，低 8 位 = ±1（见 step 回调） */
        lv_obj_add_event_cb(btn, new_reminder_step_event_handler,
                            LV_EVENT_CLICKED,
                            (void *)(intptr_t)((field << 8) |
                                               (0xff & (int)deltas[i])));

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, labels[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_ui_24, 0);
        lv_obj_center(lbl);
    }
}

/* 刷新大字时刻（08时 / 00分） */
static void new_reminder_refresh_time(void)
{
    if (new_reminder_step_lbls[0] != NULL) {
        lv_label_set_text_fmt(new_reminder_step_lbls[0], "%02d时", new_reminder_hour);
    }
    if (new_reminder_step_lbls[1] != NULL) {
        lv_label_set_text_fmt(new_reminder_step_lbls[1], "%02d分", new_reminder_min);
    }
}

/* 刷新标题预设按钮的选中态（选中的绿色，其余深灰） */
static void new_reminder_refresh_preset(void)
{
    for (int i = 0; i < NEW_REMINDER_PRESET_COUNT; i++) {
        if (new_reminder_preset_btns[i] == NULL) {
            continue;
        }
        lv_obj_set_style_bg_color(new_reminder_preset_btns[i],
            lv_color_hex(i == new_reminder_preset ? 0x4CAF50 : 0x37474F), 0);
    }
}

static void create_new_reminder_panel(void)
{
    lv_obj_t *header;
    lv_obj_t *title;
    lv_obj_t *btn_close;
    lv_obj_t *lbl_close;
    lv_obj_t *preset_grid;
    lv_obj_t *btn_save;
    lv_obj_t *lbl_save;

    /* 重复打开先把上一层收干净（这里不会递归：只删控件，不重进本函数） */
    if (new_reminder_panel != NULL) {
        lv_obj_del(new_reminder_panel);
        new_reminder_panel = NULL;
    }
    for (int i = 0; i < NEW_REMINDER_PRESET_COUNT; i++) {
        new_reminder_preset_btns[i] = NULL;
    }
    new_reminder_step_lbls[0] = NULL;
    new_reminder_step_lbls[1] = NULL;

    /* 默认时刻：现在往后取整到 5 分钟刻度（时间没对过时给 08:00）。
     * 标题默认第一条预设，老人按「保存」就是一个能用的提醒。 */
    new_reminder_preset = 0;
    reminder_sched_default_time(&new_reminder_hour, &new_reminder_min);

    new_reminder_panel = lv_obj_create(current_screen);
    lv_obj_set_size(new_reminder_panel, LV_PCT(95), LV_PCT(92));
    lv_obj_align(new_reminder_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(new_reminder_panel, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_bg_opa(new_reminder_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(new_reminder_panel, 20, 0);
    lv_obj_set_style_border_width(new_reminder_panel, 2, 0);
    lv_obj_set_style_border_color(new_reminder_panel, lv_color_hex(0x4CAF50), 0);
    lv_obj_set_flex_flow(new_reminder_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(new_reminder_panel, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(new_reminder_panel, 10, 0);
    lv_obj_set_style_pad_row(new_reminder_panel, 8, 0);

    /* 标题栏：左边"新建提醒"，右上角「×」 */
    header = lv_obj_create(new_reminder_panel);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, 44);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    title = lv_label_create(header);
    lv_label_set_text(title, "新建提醒");
    lv_obj_set_style_text_font(title, &lv_font_ui_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFEB3B), 0);

    btn_close = lv_btn_create(header);
    lv_obj_set_size(btn_close, 56, 40);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(0x607D8B), 0);
    lv_obj_set_style_radius(btn_close, 12, 0);
    lv_obj_add_event_cb(btn_close, new_reminder_close_event_handler,
                        LV_EVENT_CLICKED, NULL);

    lbl_close = lv_label_create(btn_close);
    lv_label_set_text(lbl_close, "×");   /* U+00D7，字库里有 */
    lv_obj_set_style_text_font(lbl_close, &lv_font_ui_24, 0);
    lv_obj_center(lbl_close);

    /* 标题预设：2x2 大按钮（老人点得中）。字库里没有"压"，
     * 所以第四条是"起床"而不是"量血压"。 */
    preset_grid = lv_obj_create(new_reminder_panel);
    lv_obj_set_size(preset_grid, LV_PCT(100), 122);
    lv_obj_set_style_bg_opa(preset_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(preset_grid, 0, 0);
    lv_obj_set_style_pad_all(preset_grid, 0, 0);
    lv_obj_set_style_pad_row(preset_grid, 6, 0);
    lv_obj_set_style_pad_column(preset_grid, 8, 0);
    lv_obj_remove_flag(preset_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(preset_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(preset_grid, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < NEW_REMINDER_PRESET_COUNT; i++) {
        lv_obj_t *btn = lv_btn_create(preset_grid);
        lv_obj_t *lbl;

        lv_obj_set_size(btn, LV_PCT(48), 56);
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_set_style_text_font(btn, &lv_font_ui_24, 0);
        lv_obj_add_event_cb(btn, new_reminder_preset_event_handler,
                            LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lbl = lv_label_create(btn);
        lv_label_set_text(lbl, new_reminder_presets[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_ui_24, 0);
        lv_obj_center(lbl);
        new_reminder_preset_btns[i] = btn;
    }
    new_reminder_refresh_preset();

    /* 时刻：小时一行、分钟一行，各两个大号 +/- */
    create_step_row(new_reminder_panel, 0, new_reminder_hour, "时");
    create_step_row(new_reminder_panel, 1, new_reminder_min, "分");

    /* 保存（大绿按钮） */
    btn_save = lv_btn_create(new_reminder_panel);
    lv_obj_set_size(btn_save, LV_PCT(100), 58);
    lv_obj_add_style(btn_save, &style_big_btn, 0);
    lv_obj_add_event_cb(btn_save, new_reminder_save_event_handler,
                        LV_EVENT_CLICKED, NULL);

    lbl_save = lv_label_create(btn_save);
    lv_label_set_text(lbl_save, "保存");
    lv_obj_set_style_text_font(lbl_save, &lv_font_ui_24, 0);
    lv_obj_center(lbl_save);

    printf("[Reminder] 新建提醒面板已打开（默认 %02d:%02d）\n",
           new_reminder_hour, new_reminder_min);
}

/* ==================== 创建设置面板 ==================== */
static void create_setting_panel(void)
{
    /* 设置容器 */
    setting_panel = lv_obj_create(current_screen);
    lv_obj_set_size(setting_panel, LV_PCT(95), LV_PCT(85));
    lv_obj_align(setting_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_style(setting_panel, &style_elder, 0);
    lv_obj_set_flex_flow(setting_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(setting_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(setting_panel, 20, 0);
    lv_obj_set_style_pad_row(setting_panel, 20, 0);

    /* 标题 */
    lv_obj_t *title = lv_label_create(setting_panel);
    lv_label_set_text(title, "[设] 设置");
    lv_obj_set_style_text_font(title, &lv_font_ui_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFEB3B), 0);
    lv_obj_set_style_pad_bottom(title, 20, 0);

    /* 音量设置 */
    create_slider_setting(setting_panel, "[音] 音量", user_settings.volume, 0);

    /* 亮度设置 */
    create_slider_setting(setting_panel, "[亮] 亮度", user_settings.brightness, 1);

    /* 自动提醒开关 */
    create_switch_setting(setting_panel, "[自] 自动提醒", user_settings.auto_remind, 2);

    /* 提醒间隔 */
    create_interval_setting(setting_panel, "[间] 提醒间隔", user_settings.remind_interval, 3);

    /* 恢复默认设置按钮 */
    lv_obj_t *btn_reset = lv_btn_create(setting_panel);
    lv_obj_set_size(btn_reset, LV_PCT(80), 60);
    lv_obj_add_style(btn_reset, &style_back_btn, 0);
    lv_obj_add_event_cb(btn_reset, setting_reset_event_handler, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_reset = lv_label_create(btn_reset);
    lv_label_set_text(lbl_reset, "[重] 恢复默认");
    lv_obj_set_style_text_font(lbl_reset, &lv_font_ui_24, 0);
    lv_obj_center(lbl_reset);

    /* 返回按钮 */
    create_back_button(setting_panel);
}

/* ==================== 创建滑块设置 ==================== */
static void create_slider_setting(lv_obj_t *parent, const char *title,
                                 int value, int index)
{
    /* 容器 */
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_set_size(container, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container, 0, 0);

    /* 标题行 */
    lv_obj_t *title_row = lv_obj_create(container);
    lv_obj_set_size(title_row, LV_PCT(100), 40);
    lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_row, 0, 0);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title_label = lv_label_create(title_row);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, &lv_font_ui_24, 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t *value_label = lv_label_create(title_row);
    lv_label_set_text_fmt(value_label, "%d%%", value);
    lv_obj_set_style_text_font(value_label, &lv_font_ui_24, 0);
    lv_obj_set_style_text_color(value_label, lv_color_hex(0x4CAF50), 0);

    /* 滑块 */
    lv_obj_t *slider = lv_slider_create(container);
    lv_obj_set_size(slider, LV_PCT(100), 30);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_add_style(slider, &style_slider, 0);
    lv_obj_add_event_cb(slider, setting_slider_event_handler,
                       LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)index);
}

/* ==================== 创建开关设置 ==================== */
static void create_switch_setting(lv_obj_t *parent, const char *title,
                                 bool value, int index)
{
    /* 容器 */
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_set_size(container, LV_PCT(100), 60);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 标题 */
    lv_obj_t *title_label = lv_label_create(container);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, &lv_font_ui_24, 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xFFFFFF), 0);

    /* 开关 */
    lv_obj_t *sw = lv_switch_create(container);
    lv_obj_set_size(sw, 80, 40);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x4CAF50), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x9E9E9E), LV_PART_INDICATOR);
    if (value) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, setting_switch_event_handler,
                       LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)index);
}

/* ==================== 创建间隔设置 ==================== */
static void create_interval_setting(lv_obj_t *parent, const char *title,
                                   uint16_t interval, int index)
{
    /* 容器 */
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_set_size(container, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container, 0, 0);

    /* 标题 */
    lv_obj_t *title_label = lv_label_create(container);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, &lv_font_ui_24, 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_pad_bottom(title_label, 10, 0);

    /* 间隔选择按钮组 */
    lv_obj_t *btn_group = lv_obj_create(container);
    lv_obj_set_size(btn_group, LV_PCT(100), 60);
    lv_obj_set_style_bg_opa(btn_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_group, 0, 0);
    lv_obj_set_flex_flow(btn_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_group, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 间隔选项 */
    uint16_t intervals[] = {30, 60, 120, 180};
    const char *interval_texts[] = {"30分钟", "1小时", "2小时", "3小时"};

    for (int i = 0; i < 4; i++) {
        lv_obj_t *btn = lv_btn_create(btn_group);
        lv_obj_set_size(btn, 70, 45);

        /* 选中的按钮高亮 */
        if (intervals[i] == interval) {
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x4CAF50), 0);
        } else {
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x37474F), 0);
        }
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_add_event_cb(btn, interval_button_event_handler,
                           LV_EVENT_CLICKED, (void *)(intptr_t)intervals[i]);

        lv_obj_t *btn_label = lv_label_create(btn);
        lv_label_set_text(btn_label, interval_texts[i]);
        lv_obj_set_style_text_font(btn_label, &lv_font_ui_24, 0);
        lv_obj_center(btn_label);
    }
}

/* ==================== 创建关于信息 ==================== */
static void create_about_info(lv_obj_t *parent)
{
    /* 关于信息 */
    lv_obj_t *info = lv_label_create(parent);
    lv_label_set_text(info,
        "智爱陪伴\n"
        "版本: v1.0.0\n\n"
        "AI 老人陪伴\n"
        "守护终端\n\n"
        "开发板: SF32LB52-DevKit-LCD\n"
        "界面: LVGL\n\n"
        "2026 智爱团队");
    lv_obj_set_style_text_color(info, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(info, &lv_font_ui_24, 0);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(info, 20, 0);
}

/* ==================== 创建返回按钮 ==================== */
static void create_back_button(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 120, 50);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_style(btn, &style_back_btn, 0);
    lv_obj_add_event_cb(btn, back_button_event_handler, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "< 返回");
    lv_obj_set_style_text_font(lbl, &lv_font_ui_24, 0);
    lv_obj_center(lbl);
}

/* ==================== 事件处理函数 ==================== */

/* 紧急呼叫确认对话框回调 */
static void emergency_confirm_handler(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    int action = (int)(intptr_t)lv_event_get_user_data(e);

    /* 向上遍历找到 msgbox 对象 */
    lv_obj_t *mbox = lv_obj_get_parent(btn);
    while (mbox && lv_obj_check_type(mbox, &lv_msgbox_class) == false) {
        mbox = lv_obj_get_parent(mbox);
    }
    if (mbox) {
        lv_msgbox_close(mbox);
    }

    if (action == 1 && g_emergency_cb) {
        printf("[Emergency] User confirmed emergency call\n");
        touch_ui_show_setting_detail("紧急呼叫", "正在联系家人...\n请稍候");
        g_emergency_cb(g_emergency_user_data);
    }

    touch_ui_play_sound("click");
}

/* 提醒删除按钮回调 */
static void reminder_delete_event_handler(lv_event_t *e)
{
    reminder_item_t items[REMINDER_MAX_ITEMS];
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    int count = reminder_sched_snapshot(items, REMINDER_MAX_ITEMS);

    if (index < 0 || index >= count) return;

    printf("[Reminder] Delete: %s %02d:%02d\n",
           items[index].title, items[index].hour, items[index].min);

    reminder_sched_remove(index);

    /* 删完必须重挂：被删的那条如果正是"下一条"，不重挂就会照旧响一次 */
    reminder_sched_reload();

    touch_ui_play_sound("back");

    /* 刷新提醒列表 */
    touch_ui_show_menu(MENU_TYPE_REMIND);
}

/* 「+ 新增提醒」：弹出新建面板 */
static void reminder_add_event_handler(lv_event_t *e)
{
    touch_ui_play_sound("click");
    create_new_reminder_panel();
}

/* 新建面板的「×」：什么都不存，回提醒列表 */
static void new_reminder_close_event_handler(lv_event_t *e)
{
    touch_ui_play_sound("back");
    /* show_menu 会删掉新建面板并重画列表，不用自己 del */
    touch_ui_show_menu(MENU_TYPE_REMIND);
}

/* 选标题预设 */
static void new_reminder_preset_event_handler(lv_event_t *e)
{
    new_reminder_preset = (int)(intptr_t)lv_event_get_user_data(e);

    if (new_reminder_preset < 0 || new_reminder_preset >= NEW_REMINDER_PRESET_COUNT) {
        new_reminder_preset = 0;
    }

    new_reminder_refresh_preset();
    touch_ui_play_sound("click");
}

/* 时刻的 +/- 步进。user_data 高 8 位 = 字段（0 时 / 1 分），低 8 位 = ±1 */
static void new_reminder_step_event_handler(lv_event_t *e)
{
    int code  = (int)(intptr_t)lv_event_get_user_data(e);
    int field = (code >> 8) & 0xff;
    int delta = (int)(int8_t)(code & 0xff);

    if (field == 0) {
        /* 小时：24 小时制循环 */
        new_reminder_hour = (new_reminder_hour + delta + 24) % 24;
    } else {
        /* 分钟：按 5 分钟步进循环（0,5,...,55） */
        new_reminder_min = (new_reminder_min + delta * NEW_REMINDER_MIN_STEP + 60) % 60;
    }

    new_reminder_refresh_time();
    touch_ui_play_sound("click");
}

/* 保存：进列表 + 重挂 RTC 闹钟 + 回列表刷新 */
static void new_reminder_save_event_handler(lv_event_t *e)
{
    const char *title;
    int ret;

    if (new_reminder_preset < 0 || new_reminder_preset >= NEW_REMINDER_PRESET_COUNT) {
        new_reminder_preset = 0;
    }
    title = new_reminder_presets[new_reminder_preset];

    ret = reminder_sched_add(title, new_reminder_hour, new_reminder_min);
    if (ret < 0) {
        /* 列表满了：明确告诉用户，别按了没反应 */
        char buf[48];

        printf("[Reminder] 新增失败: %d（列表最多 %d 条）\n", ret, REMINDER_MAX_ITEMS);
        snprintf(buf, sizeof(buf), "最多 %d 条提醒\n请先删掉一条", REMINDER_MAX_ITEMS);
        touch_ui_show_setting_detail("提醒已满", buf);
        return;
    }

    printf("[Reminder] 新增: %s %02d:%02d (index=%d)\n",
           title, new_reminder_hour, new_reminder_min, ret);

    /* 重算"下一条"并重挂闹钟：新加的这条如果比原来那条更近，就换成它 */
    reminder_sched_reload();

    touch_ui_play_sound("click");
    touch_ui_show_menu(MENU_TYPE_REMIND);
}

/* 菜单项点击事件 */
static void menu_item_event_handler(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);

    /* 触摸反馈 */
    touch_ui_play_sound("click");

    switch (index) {
        case 0: // 语音聊天
            /* 走新弹窗（touch_ui_show_voice_chat 内部会回调 g_voice_chat_cb
             * 让 main.c 开始录音）。旧的 touch_ui_show_setting_detail 入口已去掉。 */
            touch_ui_set_mode(MODE_LISTENING);
            touch_ui_show_voice_chat();
            break;
        case 1: // 查看提醒
            touch_ui_show_menu(MENU_TYPE_REMIND);
            break;
        case 2: // 系统设置
            touch_ui_show_menu(MENU_TYPE_SETTING);
            break;
        case 3: // 紧急联系 - 弹确认框
            show_confirm_dialog("紧急呼叫",
                               "确定要紧急联系家人吗？",
                               emergency_confirm_handler);
            break;
        case 4: // 关于
            touch_ui_show_menu(MENU_TYPE_ABOUT);
            break;
        default:
            break;
    }
}

/* 返回上一级：主菜单 -> 关掉浮层回主界面；子菜单 -> 回主菜单。
 * 原来这里无条件 show_menu(MENU_TYPE_MAIN)，而主菜单上的"返回"也是它，
 * 于是浮层永远关不掉 —— 菜单没有出口。 */
static void menu_go_back_one_level(void)
{
    /* 三种情况，一条规则：没有浮层就唤出主菜单，子菜单回主菜单，主菜单关浮层。
     * 关键点：menu_panel == NULL 时不能什么都不做 —— 那样关掉菜单后就再没有
     * 入口了（菜单只在上电时由 main() 显示一次）。参见 touch_ui_hide_menu()。 */
    if (menu_panel != NULL && current_menu_type == MENU_TYPE_MAIN) {
        touch_ui_hide_menu();
        return;
    }

    /* 子菜单、或者只剩设置/提醒面板（它们不挂在 menu_panel 下）：
     * show_menu() 会先删掉这三块面板，所以这里直接唤主菜单即可。 */
    touch_ui_show_menu(MENU_TYPE_MAIN);
}

/* 返回按钮点击事件 */
static void back_button_event_handler(lv_event_t *e)
{
    touch_ui_play_sound("back");
    menu_go_back_one_level();
}

/* 滑块值改变事件 */
static void setting_slider_event_handler(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    int value = lv_slider_get_value(slider);
    int ret;

    switch (index) {
        case 0: // 音量
            user_settings.volume = value;
            settings_save_to_file();

            /* 界面只存数字，真正改音量是音频硬件的事：转给 main.c 接的
             * audio_set_volume()。以前这里只写字段、从不碰硬件，所以
             * 滑块拖了没反应。 */
            if (g_volume_cb != NULL) {
                g_volume_cb(value, g_volume_user_data);
            }
            break;
        case 1: // 亮度
            user_settings.brightness = value;

            /* 回调里只设值，不做重活：backlight_set() 内部是懒打开的 fd +
             * 两个 ioctl。失败只打一行。未打 vendor 补丁的树上 1..99 会回
             * -ENOSYS（见 sf32lb52_backlight.h 文件头）。 */
            ret = backlight_set(value);
            if (ret != OK)
                printf("touch_ui: backlight_set(%d) failed: %d\n", value, ret);
            settings_save_to_file();
            break;
        default:
            break;
    }
}

/* 开关改变事件 */
static void setting_switch_event_handler(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);

    switch (index) {
        case 2: // 自动提醒
            user_settings.auto_remind = checked;
            settings_save_to_file();
            break;
        default:
            break;
    }

    touch_ui_play_sound("toggle");
}

/* 提醒项点击事件 */
static void reminder_item_event_handler(lv_event_t *e)
{
    reminder_item_t items[REMINDER_MAX_ITEMS];
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    int count = reminder_sched_snapshot(items, REMINDER_MAX_ITEMS);
    char time_str[8];

    touch_ui_play_sound("click");

    if (index < 0 || index >= count) return;

    snprintf(time_str, sizeof(time_str), "%02d:%02d", items[index].hour, items[index].min);

    /* 显示提醒详情 */
    touch_ui_show_setting_detail(items[index].title, time_str);
}

/* 恢复默认设置事件 */
static void setting_reset_event_handler(lv_event_t *e)
{
    show_confirm_dialog("重置", "确定恢复默认设置?",
                       confirm_dialog_event_handler);
}

/* 间隔按钮点击事件 */
static void interval_button_event_handler(lv_event_t *e)
{
    int interval = (int)(intptr_t)lv_event_get_user_data(e);
    user_settings.remind_interval = interval;
    settings_save_to_file();
    touch_ui_play_sound("click");

    /* 刷新界面以显示新的选中状态 */
    touch_ui_show_menu(MENU_TYPE_SETTING);
}

/* 确认对话框事件 */
static void confirm_dialog_event_handler(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    int action = (int)(intptr_t)lv_event_get_user_data(e);

    /* 关闭对话框：通过 btn 向上遍历找到 msgbox 对象 */
    lv_obj_t *mbox = lv_obj_get_parent(btn);
    while (mbox && lv_obj_check_type(mbox, &lv_msgbox_class) == false) {
        mbox = lv_obj_get_parent(mbox);
    }
    if (mbox) {
        lv_msgbox_close(mbox);
    }

    if (action == 1) { // 确认
        /* 恢复默认设置 */
        user_settings.volume = 70;
        user_settings.brightness = 80;
        user_settings.auto_remind = true;
        user_settings.remind_interval = 60;
        settings_save_to_file();

        /* 刷新设置界面 */
        touch_ui_show_menu(MENU_TYPE_SETTING);
    }

    touch_ui_play_sound("click");
}

/* ==================== 辅助函数 ==================== */

/* 显示确认对话框 */
static void show_confirm_dialog(const char *title, const char *content,
                               lv_event_cb_t callback)
{
    lv_obj_t *mbox = lv_msgbox_create(NULL);
    if (mbox == NULL) return;

    lv_msgbox_add_title(mbox, title);
    lv_msgbox_add_text(mbox, content);

    /* 添加确认和取消按钮 */
    lv_obj_t *btn_confirm = lv_msgbox_add_footer_button(mbox, "确定");
    lv_obj_t *btn_cancel = lv_msgbox_add_footer_button(mbox, "取消");

    lv_obj_center(mbox);
    lv_obj_set_style_bg_color(mbox, lv_color_hex(0x2D2D44), 0);
    lv_obj_set_style_text_color(mbox, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(mbox, &lv_font_ui_24, 0);

    /* 添加按钮事件 */
    lv_obj_add_event_cb(btn_cancel, callback, LV_EVENT_CLICKED, (void *)(intptr_t)0);
    lv_obj_add_event_cb(btn_confirm, callback, LV_EVENT_CLICKED, (void *)(intptr_t)1);
}

/* ==================== 右滑手势处理 ==================== */

/* 在主屏幕上检测右滑手势，滑动超过 80px 显示主菜单 */
static void screen_gesture_event_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        lv_indev_t *indev = lv_indev_get_act();
        if (indev) {
            lv_point_t p;
            lv_indev_get_point(indev, &p);
            swipe_start_x = p.x;
            swipe_tracking = true;
            swipe_handled = false;
        }
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_CANCEL) {
        if (!swipe_tracking) return;
        swipe_tracking = false;

        /* 屏幕和 indev 可能各投递一次，只处理第一次 */
        if (swipe_handled) return;

        lv_indev_t *indev = lv_indev_get_act();
        if (!indev) return;

        lv_point_t p;
        lv_indev_get_point(indev, &p);
        int32_t dx = p.x - swipe_start_x;

        /* 向右滑动超过 80px = 返回上一级（与"返回"按钮同语义） */
        if (dx > 80) {
            swipe_handled = true;

            /* 语音聊天弹窗是模态的：录音/识别在跑的时候右滑切菜单，
             * 会让菜单盖在弹窗上、录音没人收尾。要退请按弹窗里的 ×。 */
            if (voice_panel != NULL) {
                printf("[Gesture] 语音聊天弹窗开着，忽略右滑\n");
                return;
            }

            printf("[Gesture] Swipe right detected (dx=%d), go back one level\n", (int)dx);
            touch_ui_play_sound("click");
            menu_go_back_one_level();
        }
    }
}

static void msgbox_close_x_event_handler(lv_event_t *e)
{
    lv_obj_t *mbox = (lv_obj_t *)lv_event_get_user_data(e);

    if (mbox != NULL) {
        lv_msgbox_close_async(mbox);
    }
}

/* ==================== 弹窗关闭按钮 ==================== */

/* 给 lv_msgbox 加一个右上角的关闭按钮，标签是真正的 `×`(U+00D7)。
 *
 * 不要用 lv_msgbox_add_close_button()：它画的是 LVGL 内置符号字体（LV_SYMBOL_CLOSE），
 * 而本工程的字库只有 ASCII + 常用符号，那个符号不在里面 —— 屏幕上会显示成一个
 * 白边空心方块（用户反馈过两次）。`×` 在 lv_font_ui_16/20/24 里都有。 */
void touch_ui_msgbox_add_close_x(lv_obj_t *mbox)
{
    lv_obj_t *btn;

    if (mbox == NULL) {
        return;
    }

    btn = lv_msgbox_add_header_button(mbox, "×");
    if (btn == NULL) {
        return;
    }

    lv_obj_set_size(btn, 60, 52);
    lv_obj_set_style_text_font(btn, &lv_font_ui_24, 0);
    lv_obj_add_event_cb(btn, msgbox_close_x_event_handler,
                        LV_EVENT_CLICKED, mbox);
}

/* ==================== 公共接口实现 ==================== */

/* 设置模式 */
void touch_ui_set_mode(robot_mode_t mode)
{
    current_mode = mode;

    /* 根据模式更新界面 */
    switch (mode) {
        case MODE_LISTENING:
            /* 语音聊天现在由 touch_ui_show_voice_chat() 弹窗负责（带×关闭 /
             * 计时 / 提交按钮）。这里不再弹 touch_ui_show_setting_detail ——
             * 那个弹窗只有消息框自带的关闭按钮，没有提交入口，留着会多出一个
             * 关不掉的框。 */
            break;
        case MODE_SLEEP:
            /* 降低亮度，显示休眠界面 */
            break;
        case MODE_ALARM:
            /* 显示报警界面 */
            break;
        default:
            break;
    }
}

/* 获取当前模式 */
robot_mode_t touch_ui_get_mode(void)
{
    return current_mode;
}

/* 获取设置 */
settings_t* touch_ui_get_settings(void)
{
    return &user_settings;
}

/* 保存设置 */
void touch_ui_save_settings(void)
{
    settings_save_to_file();
}

/* 显示设置详情 */
void touch_ui_show_setting_detail(const char *title, const char *content)
{
    /* 创建详情弹窗 */
    lv_obj_t *mbox = lv_msgbox_create(NULL);
    if (mbox == NULL) return;

    lv_msgbox_add_title(mbox, title);
    lv_msgbox_add_text(mbox, content);
    touch_ui_msgbox_add_close_x(mbox);
    lv_obj_center(mbox);
    lv_obj_set_style_bg_color(mbox, lv_color_hex(0x2D2D44), 0);
    lv_obj_set_style_text_color(mbox, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(mbox, &lv_font_ui_24, 0);
}

/* 添加提醒（"HH:MM" 字符串入口，main.c 开机放默认提醒、别处下发提醒都用它） */
void touch_ui_add_reminder(const char *title, const char *time)
{
    int hour = 0;
    int min = 0;
    int ret;

    if (title == NULL || time == NULL) {
        return;
    }

    if (sscanf(time, "%d:%d", &hour, &min) != 2 ||
        hour < 0 || hour > 23 || min < 0 || min > 59) {
        printf("Reminder: 时间格式不对（要 HH:MM）: %s\n", time);
        return;
    }

    ret = reminder_sched_add(title, hour, min);
    if (ret < 0) {
        printf("Reminder: 加不进去 (%d)，最多 %d 条\n", ret, REMINDER_MAX_ITEMS);
        return;
    }

    printf("Reminder added: %s %02d:%02d\n", title, hour, min);

    /* 加完就重挂闹钟：这样从任何入口（开机默认提醒 / MQTT 下发 / 界面）
     * 进来的提醒都是"立刻生效"的，不用调用方自己记得调 */
    reminder_sched_reload();
}

/* 显示提醒列表 */
void touch_ui_show_reminder_list(void)
{
    touch_ui_show_menu(MENU_TYPE_REMIND);
}

/* 清空提醒（界面暂时没有入口，留给调试/上层命令用） */
void touch_ui_clear_reminders(void)
{
    reminder_sched_clear();
    reminder_sched_reload();
    printf("Reminders cleared\n");
}

/* 提醒列表被外部改了（语音的 add_reminder 工具 / MQTT 下发）：投到 LVGL 线程
 * 重画。static 函数在下面，所以先声明一下（LVGL 的 lv_async_call 只认函数
 * 指针，不需要在这里定义完整）。 */
static void reminders_changed_async(void *unused);

void touch_ui_notify_reminders_changed(void)
{
    if (lv_async_call(reminders_changed_async, NULL) != LV_RESULT_OK) {
        /* 投递失败（内存紧）：列表下次打开时本来就是重新读的，不刷也不会错 */
        printf("[Reminder] 列表刷新投递失败，稍后打开菜单时自然是最新的\n");
    }
}

/* LVGL 线程：只在"用户正看着提醒列表"时就地重画 */
static void reminders_changed_async(void *unused)
{
    (void)unused;

    /* 没在看列表：什么都不用做，下次打开菜单是重新读列表的 */

    if (menu_panel == NULL || current_menu_type != MENU_TYPE_REMIND) {
        return;
    }

    /* 正在手动新建 / 语音弹窗开着：别动画面（重画会把叠在上面的那层清掉） */

    if (new_reminder_panel != NULL || voice_panel != NULL) {
        return;
    }

    printf("[Reminder] 列表有变，重画提醒菜单\n");

    /* show_menu 会删掉旧面板重建；voice_panel 已在上面挡掉，不会触发
     * "用户离开了语音聊天"那套收尾 */
    touch_ui_show_menu(MENU_TYPE_REMIND);
}

/* 触摸震动反馈 */
void touch_ui_vibrate(int duration_ms)
{
    /* TODO: 调用硬件震动马达 */
    printf("Vibrate: %dms\n", duration_ms);
}

/* 播放音效 */
void touch_ui_play_sound(const char *sound_type)
{
    /* TODO: 播放对应音效 */
    printf("Sound: %s\n", sound_type);
}

/* ==================== 功能回调注册 ==================== */

void touch_ui_set_voice_chat_cb(voice_chat_start_cb_t cb, void *user_data)
{
    g_voice_chat_cb = cb;
    g_voice_chat_user_data = user_data;
}

void touch_ui_set_emergency_cb(emergency_call_cb_t cb, void *user_data)
{
    g_emergency_cb = cb;
    g_emergency_user_data = user_data;
}

void touch_ui_set_volume_cb(volume_set_cb_t cb, void *user_data)
{
    g_volume_cb = cb;
    g_volume_user_data = user_data;

    /* 注册时就把当前值下发一次：设置是从文件读回来的（touch_ui_init 里
     * settings_load_from_file），开机时硬件那边还是默认音量，不补这一下
     * 会出现"滑块显示 30，声音还是 70"。 */
    if (g_volume_cb != NULL) {
        g_volume_cb(user_settings.volume, g_volume_user_data);
    }
}

/* ==================== 设置持久化 ==================== */

static void settings_save_to_file(void)
{
    FILE *fp = fopen(SETTINGS_FILE_PATH, "wb");
    if (!fp) {
        printf("settings: save failed to open %s\n", SETTINGS_FILE_PATH);
        return;
    }

    uint32_t header[2] = { SETTINGS_FILE_MAGIC, SETTINGS_FILE_VERSION };
    fwrite(header, sizeof(uint32_t), 2, fp);
    fwrite(&user_settings, sizeof(settings_t), 1, fp);
    fclose(fp);
    printf("settings: saved (vol=%d bright=%d auto=%d interval=%d)\n",
           user_settings.volume, user_settings.brightness,
           user_settings.auto_remind, user_settings.remind_interval);
}

static void settings_load_from_file(void)
{
    FILE *fp = fopen(SETTINGS_FILE_PATH, "rb");
    if (!fp) {
        printf("settings: no saved file, using defaults\n");
        return;
    }

    uint32_t header[2];
    if (fread(header, sizeof(uint32_t), 2, fp) != 2 ||
        header[0] != SETTINGS_FILE_MAGIC ||
        header[1] != SETTINGS_FILE_VERSION) {
        printf("settings: invalid file header, using defaults\n");
        fclose(fp);
        return;
    }

    settings_t loaded;
    if (fread(&loaded, sizeof(settings_t), 1, fp) != 1) {
        printf("settings: read failed, using defaults\n");
        fclose(fp);
        return;
    }
    fclose(fp);

    /* 校验范围 */
    if (loaded.volume <= 100 && loaded.brightness <= 100 &&
        loaded.remind_interval > 0 && loaded.remind_interval <= 720) {
        user_settings = loaded;
        printf("settings: loaded (vol=%d bright=%d auto=%d interval=%d)\n",
               user_settings.volume, user_settings.brightness,
               user_settings.auto_remind, user_settings.remind_interval);
    } else {
        printf("settings: invalid values, using defaults\n");
    }
}

/* ==================== 语音聊天弹窗 ==================== */
/*
 * 一键对话的闭环界面：点开就开始录音，「提交」结束录音并把这一轮交给 main.c
 * 去跑 ASR -> LLM -> TTS，「×」丢弃录音关窗。
 *
 * 为什么不用 lv_msgbox：
 *   lv_msgbox_add_close_button() 画的是 LVGL 内置符号字体里的 LV_SYMBOL_CLOSE，
 *   而本工程用的字库（lv_font_ui_16/20/24）里根本没有符号字体那一套，界面上
 *   就是一个白边空心方块（现场看到的就是这个）。这里自己搭面板：关闭按钮的
 *   标签用 U+00D7 "×"（三个字号的字库 cmap 里都有这个码位），顺便把计时、
 *   可滚动对话区、大号提交按钮都按老人的手感摆好。
 *
 * 线程约定：
 *   - 控件只在 LVGL 线程（事件回调 / 主循环里的 lv_timer_handler）里创建、
 *     删除、写字。CONFIG_LV_USE_OS=0，LVGL 自己没有锁，别的线程碰控件必崩。
 *   - ASR/LLM/TTS 在工作线程里跑，回结果只走 touch_ui_set_voice_status() /
 *     touch_ui_set_voice_reply() / touch_ui_voice_chat_round_done()，它们用
 *     lv_async_call 把消息投回 LVGL 线程，并带上投递时刻的世代号。
 *   - 世代号对不上（用户已关窗、或点了「再说一次」）的消息整条丢弃，既不会
 *     写到已删除的控件上，关窗时也不需要去 join 在工作的工作线程。
 */

/* 投递到 LVGL 线程的一段文字（堆上分配，回调里释放） */
typedef struct {
    char *text;
    uint32_t gen;
    bool is_reply;
} voice_text_msg_t;

static void voice_text_async(void *arg)
{
    voice_text_msg_t *msg = (voice_text_msg_t *)arg;

    /* 世代对不上 = 弹窗已经关了或者又开了一轮，这条消息作废 */
    if (msg->gen == voice_generation && voice_panel != NULL) {
        lv_obj_t *lbl = msg->is_reply ? voice_reply_lbl : voice_status_lbl;

        if (lbl != NULL) {
            lv_label_set_text(lbl, msg->text);
        }

        /* 对话区有新内容就滚到最新一行，老人不用手动划 */
        if (msg->is_reply && voice_reply_box != NULL) {
            lv_obj_scroll_to_y(voice_reply_box, LV_COORD_MAX, LV_ANIM_OFF);
        }
    }

    free(msg->text);
    free(msg);
}

/* 把一段文字投到弹窗里（状态行或对话区）。任何线程都能调。 */
static void voice_post_text(const char *text, bool is_reply)
{
    voice_text_msg_t *msg;
    char *copy;

    if (text == NULL || voice_panel == NULL) {
        return;
    }

    msg = malloc(sizeof(voice_text_msg_t));
    copy = malloc(strlen(text) + 1);
    if (msg == NULL || copy == NULL) {
        free(msg);
        free(copy);
        return;
    }

    strcpy(copy, text);
    msg->text = copy;
    msg->gen = voice_generation;
    msg->is_reply = is_reply;

    if (lv_async_call(voice_text_async, msg) != LV_RESULT_OK) {
        free(copy);
        free(msg);
    }
}

/* 录音计时：每秒把 mm:ss 刷到计时标签上 */
static void voice_tick_timer_cb(lv_timer_t *t)
{
    uint32_t sec;

    (void)t;

    if (voice_timer_lbl == NULL) {
        return;
    }

    sec = lv_tick_elaps(voice_start_tick) / 1000;
    lv_label_set_text_fmt(voice_timer_lbl, "%02u:%02u",
                          (unsigned int)(sec / 60), (unsigned int)(sec % 60));
}

static void voice_stop_timer(void)
{
    if (voice_tick_timer != NULL) {
        lv_timer_delete(voice_tick_timer);
        voice_tick_timer = NULL;
    }
}

static void voice_start_timer(void)
{
    voice_stop_timer();
    voice_start_tick = lv_tick_get();
    voice_tick_timer = lv_timer_create(voice_tick_timer_cb, 1000, NULL);
}

/* 底部大按钮的三个状态：提交 -> 处理中(禁用) -> 再说一次 */
static void voice_set_btn_state(voice_btn_state_t state)
{
    voice_btn_state = state;

    if (voice_submit_btn == NULL || voice_submit_lbl == NULL) {
        return;
    }

    switch (state) {
        case VOICE_BTN_SUBMIT:
            lv_label_set_text(voice_submit_lbl, "提交");
            lv_obj_clear_state(voice_submit_btn, LV_STATE_DISABLED);
            break;
        case VOICE_BTN_BUSY:
            lv_label_set_text(voice_submit_lbl, "处理中…");
            lv_obj_add_state(voice_submit_btn, LV_STATE_DISABLED);
            break;
        case VOICE_BTN_RETRY:
        default:
            lv_label_set_text(voice_submit_lbl, "再说一次");
            lv_obj_clear_state(voice_submit_btn, LV_STATE_DISABLED);
            break;
    }
}

/* 开一轮新的说话：世代 +1（上一轮在途的结果就此作废），界面归零，计时重开。
 * 录音本身不在这里启动 —— main.c 的 g_voice_chat_cb 负责。 */
static void voice_begin_round(void)
{
    voice_generation++;

    voice_set_btn_state(VOICE_BTN_SUBMIT);

    if (voice_reply_lbl != NULL) {
        lv_label_set_text(voice_reply_lbl, "");
    }
    if (voice_status_lbl != NULL) {
        lv_label_set_text(voice_status_lbl, "正在录音…\n说完点「提交」");
    }
    if (voice_reply_box != NULL) {
        lv_obj_scroll_to_y(voice_reply_box, 0, LV_ANIM_OFF);
    }
    if (voice_timer_lbl != NULL) {
        lv_label_set_text(voice_timer_lbl, "00:00");
    }

    voice_start_timer();
}

/* × ：结束录音、丢弃这一轮、关窗 */
static void voice_close_event_handler(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    touch_ui_play_sound("click");
    printf("[VoiceChat] 关闭弹窗，丢弃这一轮\n");

    /* 先让 main.c 停录音、丢缓冲，再关窗：关窗会把世代号推进，
     * 在途的 ASR/LLM/TTS 结果回来时会发现对不上，自己丢掉 */
    if (g_voice_cancel_cb != NULL) {
        g_voice_cancel_cb(g_voice_cancel_user_data);
    }

    touch_ui_hide_voice_chat();
}

/* 底部大按钮：「提交」（结束录音、交出去）或「再说一次」 */
static void voice_submit_event_handler(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || voice_panel == NULL) {
        return;
    }

    touch_ui_play_sound("click");

    if (voice_btn_state == VOICE_BTN_RETRY) {
        printf("[VoiceChat] 再说一次\n");
        voice_begin_round();

        if (g_voice_chat_cb != NULL) {
            g_voice_chat_cb(g_voice_chat_user_data);
        } else {
            voice_post_text("录音功能未就绪", false);
            voice_stop_timer();
            voice_set_btn_state(VOICE_BTN_RETRY);
        }
        return;
    }

    if (voice_btn_state != VOICE_BTN_SUBMIT) {
        return;   /* BUSY：按钮是禁用的，正常点不到 */
    }

    /* 提交：停表，后面全交给 main.c（它开工作线程跑 ASR->LLM->TTS，
     * 网络请求绝不能压在这个 LVGL 线程里） */
    printf("[VoiceChat] 提交这一轮\n");
    voice_set_btn_state(VOICE_BTN_BUSY);
    voice_stop_timer();

    if (g_voice_submit_cb != NULL) {
        g_voice_submit_cb(g_voice_submit_user_data);
    } else {
        voice_post_text("语音功能未就绪", false);
        voice_set_btn_state(VOICE_BTN_RETRY);
    }
}

/* 一轮结束（成功或失败都用它收尾）：按钮变回可点的「再说一次」，
 * 用户不用关窗重开就能接着聊。 */
static void voice_round_done_async(void *arg)
{
    uint32_t gen = (uint32_t)(uintptr_t)arg;

    if (gen != voice_generation || voice_panel == NULL) {
        return;
    }

    voice_set_btn_state(VOICE_BTN_RETRY);
}

/* ==================== 语音聊天弹窗：公共接口 ==================== */

/* 打开弹窗，并回调 g_voice_chat_cb 让 main.c 开始录音 */
void touch_ui_show_voice_chat(void)
{
    lv_obj_t *header;
    lv_obj_t *title;
    lv_obj_t *btn_close;
    lv_obj_t *lbl_close;

    /* 重复打开：先把上一轮的控件和定时器收干净，免得对象/lv_timer 泄漏 */
    touch_ui_hide_voice_chat();

    voice_panel = lv_obj_create(current_screen);
    lv_obj_set_size(voice_panel, LV_PCT(92), LV_PCT(92));
    lv_obj_align(voice_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(voice_panel, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_bg_opa(voice_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(voice_panel, 20, 0);
    lv_obj_set_style_border_width(voice_panel, 2, 0);
    lv_obj_set_style_border_color(voice_panel, lv_color_hex(0x4CAF50), 0);
    lv_obj_remove_flag(voice_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(voice_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(voice_panel, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(voice_panel, 14, 0);
    lv_obj_set_style_pad_row(voice_panel, 10, 0);

    /* 标题栏：左边标题，右上角关闭按钮（标签是真正的 ×） */
    header = lv_obj_create(voice_panel);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, 56);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    title = lv_label_create(header);
    lv_label_set_text(title, "语音聊天");
    lv_obj_set_style_text_font(title, &lv_font_ui_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFEB3B), 0);

    btn_close = lv_btn_create(header);
    lv_obj_set_size(btn_close, 60, 52);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(0x607D8B), 0);
    lv_obj_set_style_radius(btn_close, 12, 0);
    lv_obj_add_event_cb(btn_close, voice_close_event_handler,
                        LV_EVENT_CLICKED, NULL);

    lbl_close = lv_label_create(btn_close);
    lv_label_set_text(lbl_close, "×");   /* U+00D7，字库里有，不是 LV_SYMBOL_CLOSE */
    lv_obj_set_style_text_font(lbl_close, &lv_font_ui_24, 0);
    lv_obj_center(lbl_close);

    /* 录音计时（mm:ss，voice_tick_timer_cb 每秒刷新） */
    voice_timer_lbl = lv_label_create(voice_panel);
    lv_label_set_text(voice_timer_lbl, "00:00");
    lv_obj_set_style_text_font(voice_timer_lbl, &lv_font_ui_24, 0);
    lv_obj_set_style_text_color(voice_timer_lbl, lv_color_hex(0x4CAF50), 0);

    /* 状态行 */
    voice_status_lbl = lv_label_create(voice_panel);
    lv_label_set_text(voice_status_lbl, "正在录音…\n说完点「提交」");
    lv_obj_set_width(voice_status_lbl, LV_PCT(100));
    lv_label_set_long_mode(voice_status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(voice_status_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(voice_status_lbl, &lv_font_ui_20, 0);
    lv_obj_set_style_text_color(voice_status_lbl, lv_color_hex(0xCCCCCC), 0);

    /* 对话区：识别文字 + AI 回复，能换行、能滚动 */
    voice_reply_box = lv_obj_create(voice_panel);
    lv_obj_set_width(voice_reply_box, LV_PCT(100));
    lv_obj_set_flex_grow(voice_reply_box, 1);
    lv_obj_set_style_bg_color(voice_reply_box, lv_color_hex(0x101020), 0);
    lv_obj_set_style_bg_opa(voice_reply_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(voice_reply_box, 1, 0);
    lv_obj_set_style_border_color(voice_reply_box, lv_color_hex(0x37474F), 0);
    lv_obj_set_style_radius(voice_reply_box, 12, 0);
    lv_obj_set_style_pad_all(voice_reply_box, 10, 0);
    lv_obj_set_flex_flow(voice_reply_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(voice_reply_box, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    voice_reply_lbl = lv_label_create(voice_reply_box);
    lv_label_set_text(voice_reply_lbl, "");
    lv_obj_set_width(voice_reply_lbl, LV_PCT(100));
    lv_label_set_long_mode(voice_reply_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(voice_reply_lbl, &lv_font_ui_20, 0);
    lv_obj_set_style_text_color(voice_reply_lbl, lv_color_hex(0xFFFFFF), 0);

    /* 大号提交按钮（≥60px，老人好按；绿色 = style_big_btn） */
    voice_submit_btn = lv_btn_create(voice_panel);
    lv_obj_set_width(voice_submit_btn, LV_PCT(90));
    lv_obj_set_height(voice_submit_btn, 72);
    lv_obj_add_style(voice_submit_btn, &style_big_btn, 0);
    lv_obj_add_event_cb(voice_submit_btn, voice_submit_event_handler,
                        LV_EVENT_CLICKED, NULL);

    voice_submit_lbl = lv_label_create(voice_submit_btn);
    lv_label_set_text(voice_submit_lbl, "提交");
    lv_obj_set_style_text_font(voice_submit_lbl, &lv_font_ui_24, 0);
    lv_obj_center(voice_submit_lbl);

    voice_begin_round();

    printf("[VoiceChat] 弹窗已打开，开始录音\n");

    if (g_voice_chat_cb != NULL) {
        g_voice_chat_cb(g_voice_chat_user_data);   /* main.c: audio_record_start() */
    } else {
        voice_post_text("录音功能未就绪", false);
        voice_stop_timer();
        voice_set_btn_state(VOICE_BTN_RETRY);
    }
}

/* 关窗：控件和定时器都在这里释放，世代号推进让在途的工作线程结果作废 */
void touch_ui_hide_voice_chat(void)
{
    voice_generation++;

    voice_stop_timer();

    if (voice_panel != NULL) {
        lv_obj_del(voice_panel);
        voice_panel = NULL;
    }

    voice_timer_lbl = NULL;
    voice_status_lbl = NULL;
    voice_reply_box = NULL;
    voice_reply_lbl = NULL;
    voice_submit_btn = NULL;
    voice_submit_lbl = NULL;
    voice_btn_state = VOICE_BTN_SUBMIT;
}

bool touch_ui_voice_chat_active(void)
{
    return voice_panel != NULL;
}

uint32_t touch_ui_voice_chat_generation(void)
{
    return voice_generation;
}

void touch_ui_set_voice_submit_cb(voice_submit_cb_t cb, void *user_data)
{
    g_voice_submit_cb = cb;
    g_voice_submit_user_data = user_data;
}

void touch_ui_set_voice_cancel_cb(voice_cancel_cb_t cb, void *user_data)
{
    g_voice_cancel_cb = cb;
    g_voice_cancel_user_data = user_data;
}

void touch_ui_set_voice_status(const char *text)
{
    voice_post_text(text, false);
}

void touch_ui_set_voice_reply(const char *text)
{
    voice_post_text(text, true);
}

void touch_ui_voice_chat_round_done(void)
{
    lv_async_call(voice_round_done_async, (void *)(uintptr_t)voice_generation);
}

void touch_ui_voice_chat_stop_timer(void)
{
    voice_stop_timer();
}

/* ==================== 关怀确认面板 ==================== */

/* "我没事" 按钮回调 */
static void checkin_btn_fine_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) return;
    if (checkin_confirmed) return;

    printf("[Checkin] User: I'm fine (id=%lu)\n", (unsigned long)checkin_current_id);
    checkin_confirmed = true;
    lv_obj_add_state(checkin_btn_fine, LV_STATE_DISABLED);
    lv_obj_add_state(checkin_btn_help, LV_STATE_DISABLED);
    lv_label_set_text(checkin_status_lbl, "正在通知...");
    lv_obj_set_style_text_color(checkin_status_lbl, lv_color_hex(0xFFC107), 0);
    touch_ui_play_sound("click");

    if (checkin_cb) {
        checkin_cb(checkin_current_id, false, checkin_cb_user_data);
    }
}

/* "需要帮助" 按钮回调 */
static void checkin_btn_help_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) return;
    if (checkin_confirmed) return;

    printf("[Checkin] User: Need help (id=%lu)\n", (unsigned long)checkin_current_id);
    checkin_confirmed = true;
    lv_obj_add_state(checkin_btn_fine, LV_STATE_DISABLED);
    lv_obj_add_state(checkin_btn_help, LV_STATE_DISABLED);
    lv_label_set_text(checkin_status_lbl, "正在通知...");
    lv_obj_set_style_text_color(checkin_status_lbl, lv_color_hex(0xFFC107), 0);
    touch_ui_play_sound("click");

    if (checkin_cb) {
        checkin_cb(checkin_current_id, true, checkin_cb_user_data);
    }
}

/* 显示关怀确认面板 */
void touch_ui_show_checkin(uint64_t checkin_id, uint32_t timeout_ms,
                           checkin_btn_cb_t cb, void *user_data)
{
    touch_ui_hide_checkin();
    checkin_current_id = checkin_id;
    checkin_confirmed = false;
    checkin_cb = cb;
    checkin_cb_user_data = user_data;

    checkin_panel = lv_obj_create(current_screen);
    lv_obj_set_size(checkin_panel, LV_PCT(90), LV_PCT(70));
    lv_obj_align(checkin_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(checkin_panel, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_bg_opa(checkin_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(checkin_panel, 20, 0);
    lv_obj_set_style_border_width(checkin_panel, 2, 0);
    lv_obj_set_style_border_color(checkin_panel, lv_color_hex(0xFF9800), 0);
    lv_obj_set_flex_flow(checkin_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(checkin_panel, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(checkin_panel, 20, 0);
    lv_obj_set_style_pad_row(checkin_panel, 15, 0);

    lv_obj_t *title = lv_label_create(checkin_panel);
    lv_label_set_text(title, "关怀提醒");
    lv_obj_set_style_text_font(title, &lv_font_ui_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFEB3B), 0);

    lv_obj_t *hint = lv_label_create(checkin_panel);
    lv_label_set_text(hint, "您还好吗？请确认状态");
    lv_obj_set_style_text_font(hint, &lv_font_ui_24, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0xFFFFFF), 0);

    checkin_status_lbl = lv_label_create(checkin_panel);
    lv_label_set_text(checkin_status_lbl, "等待确认");
    lv_obj_set_style_text_font(checkin_status_lbl, &lv_font_ui_24, 0);
    lv_obj_set_style_text_color(checkin_status_lbl, lv_color_hex(0x4CAF50), 0);

    /* "我没事" 按钮 - 绿色 */
    checkin_btn_fine = lv_btn_create(checkin_panel);
    lv_obj_set_size(checkin_btn_fine, 200, 60);
    lv_obj_set_style_bg_color(checkin_btn_fine, lv_color_hex(0x4CAF50), 0);
    lv_obj_set_style_radius(checkin_btn_fine, 30, 0);
    lv_obj_add_event_cb(checkin_btn_fine, checkin_btn_fine_handler,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_fine = lv_label_create(checkin_btn_fine);
    lv_label_set_text(lbl_fine, "我没事");
    lv_obj_set_style_text_font(lbl_fine, &lv_font_ui_24, 0);
    lv_obj_center(lbl_fine);

    /* "需要帮助" 按钮 - 红色 */
    checkin_btn_help = lv_btn_create(checkin_panel);
    lv_obj_set_size(checkin_btn_help, 200, 60);
    lv_obj_set_style_bg_color(checkin_btn_help, lv_color_hex(0xF44336), 0);
    lv_obj_set_style_radius(checkin_btn_help, 30, 0);
    lv_obj_add_event_cb(checkin_btn_help, checkin_btn_help_handler,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_help = lv_label_create(checkin_btn_help);
    lv_label_set_text(lbl_help, "需要帮助");
    lv_obj_set_style_text_font(lbl_help, &lv_font_ui_24, 0);
    lv_obj_center(lbl_help);

    (void)timeout_ms;
    printf("[Checkin] UI shown: id=%lu\n", (unsigned long)checkin_id);
}

/* 更新关怀确认状态（lv_async_call 投递到 LVGL 线程） */
static void update_checkin_state_async(void *state_ptr)
{
    touch_checkin_state_t state = (touch_checkin_state_t)(intptr_t)state_ptr;
    if (!checkin_status_lbl) return;

    switch (state) {
        case TOUCH_CHECKIN_WAITING:
            lv_label_set_text(checkin_status_lbl, "等待确认");
            lv_obj_set_style_text_color(checkin_status_lbl, lv_color_hex(0x4CAF50), 0);
            if (checkin_btn_fine) lv_obj_clear_state(checkin_btn_fine, LV_STATE_DISABLED);
            if (checkin_btn_help) lv_obj_clear_state(checkin_btn_help, LV_STATE_DISABLED);
            checkin_confirmed = false;
            break;
        case TOUCH_CHECKIN_SENDING:
            lv_label_set_text(checkin_status_lbl, "正在通知...");
            lv_obj_set_style_text_color(checkin_status_lbl, lv_color_hex(0xFFC107), 0);
            break;
        case TOUCH_CHECKIN_SENT:
            /* 这里原来写的是"通知成功 ✓"：U+2713 是 dingbat，不在字库的字符集
             * （GB2312 + ASCII + CJK 标点 + 全角）里，显示出来就是方块，去掉。 */
            lv_label_set_text(checkin_status_lbl, "通知成功");
            lv_obj_set_style_text_color(checkin_status_lbl, lv_color_hex(0x4CAF50), 0);
            break;
        case TOUCH_CHECKIN_FAILED:
            lv_label_set_text(checkin_status_lbl, "通知失败，请重试");
            lv_obj_set_style_text_color(checkin_status_lbl, lv_color_hex(0xF44336), 0);
            if (checkin_btn_fine) lv_obj_clear_state(checkin_btn_fine, LV_STATE_DISABLED);
            if (checkin_btn_help) lv_obj_clear_state(checkin_btn_help, LV_STATE_DISABLED);
            checkin_confirmed = false;
            break;
        default:
            break;
    }
}

void touch_ui_update_checkin_state(touch_checkin_state_t state)
{
    lv_async_call(update_checkin_state_async, (void *)(intptr_t)state);
}

/* 隐藏关怀确认面板 */
void touch_ui_hide_checkin(void)
{
    if (checkin_panel) {
        lv_obj_del(checkin_panel);
        checkin_panel = NULL;
        checkin_status_lbl = NULL;
        checkin_btn_fine = NULL;
        checkin_btn_help = NULL;
    }
    checkin_current_id = 0;
    checkin_confirmed = false;
    checkin_cb = NULL;
    checkin_cb_user_data = NULL;
}
