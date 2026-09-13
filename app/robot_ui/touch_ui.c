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

/* 中文字库（实现在 lv_font_ui_16/20/24.c，见 CMakeLists.txt 的 SRCS）。
 * 原来这里用的是 LVGL 自带的 16px 中文点阵字体（字形不够，汉字一半是方块）。
 * 现在按改动前 montserrat 的字号分三档：
 *   14/16/18 -> lv_font_ui_16   20/22/24 -> lv_font_ui_20   >=28 -> lv_font_ui_24 */
LV_FONT_DECLARE(lv_font_ui_16);
LV_FONT_DECLARE(lv_font_ui_20);
LV_FONT_DECLARE(lv_font_ui_24);

/* ==================== 全局变量 ==================== */
static lv_obj_t *current_screen = NULL;
static lv_obj_t *menu_panel = NULL;
static lv_obj_t *setting_panel = NULL;
static lv_obj_t *reminder_panel = NULL;

/* 右滑手势状态 */
static int32_t swipe_start_x = 0;
static bool swipe_tracking = false;

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

/* 提醒列表 */
#define MAX_REMINDERS 10
typedef struct {
    char title[64];
    char time[32];
    bool active;
} reminder_t;

static reminder_t reminders[MAX_REMINDERS];
static int reminder_count = 0;

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
static void create_reminder_panel(void);
static void create_back_button(lv_obj_t *parent);
static void create_menu_item(lv_obj_t *parent, const char *icon_text,
                            const char *subtitle, int index);
static void create_reminder_list_items(lv_obj_t *parent);
static void create_reminder_item(lv_obj_t *parent, const char *title,
                                const char *time_str, int index);
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

    /* 初始化提醒列表 */
    memset(reminders, 0, sizeof(reminders));
    reminder_count = 0;

    /* 面板句柄复位: NuttX builtin 应用重新运行时 .bss 不清零,
     * 若残留上一次运行的面板指针, 之后 lv_obj_del() 会去删除已
     * 失效的 LVGL 对象而导致 hardfault */
    menu_panel = NULL;
    setting_panel = NULL;
    reminder_panel = NULL;

    /* 获取当前活动屏幕 */
    current_screen = lv_scr_act();
    lv_obj_add_style(current_screen, &style_elder, 0);

    /* 注册右滑手势：在主屏幕上任意位置向右滑动即可进入主菜单 */
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
    if (reminder_panel) {
        lv_obj_del(reminder_panel);
        reminder_panel = NULL;
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

    if (reminder_panel) {
        lv_obj_del(reminder_panel);
        reminder_panel = NULL;
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

/* ==================== 创建提醒列表 ==================== */
static void create_reminder_list_items(lv_obj_t *parent)
{
    if (reminder_count == 0) {
        /* 空提醒 */
        lv_obj_t *empty = lv_label_create(parent);
        lv_label_set_text(empty, "暂无提醒\n\n点击 + 添加");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x9E9E9E), 0);
        lv_obj_set_style_text_font(empty, &lv_font_ui_24, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(empty, 50, 0);
    } else {
        /* 显示提醒列表 */
        for (int i = 0; i < reminder_count; i++) {
            if (reminders[i].active) {
                create_reminder_item(parent, reminders[i].title, reminders[i].time, i);
            }
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
    lv_label_set_text(del_label, "x");
    lv_obj_set_style_text_font(del_label, &lv_font_ui_24, 0);
    lv_obj_set_style_text_color(del_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(del_label);
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
    int index = (int)(intptr_t)lv_event_get_user_data(e);

    if (index < 0 || index >= reminder_count) return;

    printf("[Reminder] Delete: %s %s\n",
           reminders[index].title, reminders[index].time);

    /* 用最后一项覆盖被删除项 */
    if (index < reminder_count - 1) {
        reminders[index] = reminders[reminder_count - 1];
    }
    reminder_count--;

    touch_ui_play_sound("back");

    /* 刷新提醒列表 */
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
            touch_ui_set_mode(MODE_LISTENING);
            if (g_voice_chat_cb) {
                g_voice_chat_cb(g_voice_chat_user_data);
            }
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
    if (current_menu_type == MENU_TYPE_MAIN) {
        touch_ui_hide_menu();
    } else {
        touch_ui_show_menu(MENU_TYPE_MAIN);
    }
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
    int index = (int)(intptr_t)lv_event_get_user_data(e);

    touch_ui_play_sound("click");

    /* 显示提醒详情 */
    touch_ui_show_setting_detail(reminders[index].title,
                                reminders[index].time);
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
        }
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_CANCEL) {
        if (!swipe_tracking) return;
        swipe_tracking = false;

        lv_indev_t *indev = lv_indev_get_act();
        if (!indev) return;

        lv_point_t p;
        lv_indev_get_point(indev, &p);
        int32_t dx = p.x - swipe_start_x;

        /* 向右滑动超过 80px = 返回上一级（与"返回"按钮同语义） */
        if (dx > 80) {
            printf("[Gesture] Swipe right detected (dx=%d), go back one level\n", (int)dx);
            touch_ui_play_sound("click");
            menu_go_back_one_level();
        }
    }
}

/* ==================== 公共接口实现 ==================== */

/* 设置模式 */
void touch_ui_set_mode(robot_mode_t mode)
{
    current_mode = mode;

    /* 根据模式更新界面 */
    switch (mode) {
        case MODE_LISTENING:
            touch_ui_show_setting_detail("语音聊天", "聆听中...\n请说话");
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
    lv_msgbox_add_close_button(mbox);
    lv_obj_center(mbox);
    lv_obj_set_style_bg_color(mbox, lv_color_hex(0x2D2D44), 0);
    lv_obj_set_style_text_color(mbox, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(mbox, &lv_font_ui_24, 0);
}

/* 添加提醒 */
void touch_ui_add_reminder(const char *title, const char *time)
{
    if (reminder_count < MAX_REMINDERS) {
        strncpy(reminders[reminder_count].title, title, sizeof(reminders[0].title) - 1);
        strncpy(reminders[reminder_count].time, time, sizeof(reminders[0].time) - 1);
        reminders[reminder_count].active = true;
        reminder_count++;

        printf("Reminder added: %s %s\n", title, time);
    }
}

/* 显示提醒列表 */
void touch_ui_show_reminder_list(void)
{
    touch_ui_show_menu(MENU_TYPE_REMIND);
}

/* 清空提醒 */
void touch_ui_clear_reminders(void)
{
    memset(reminders, 0, sizeof(reminders));
    reminder_count = 0;
    printf("Reminders cleared\n");
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
            lv_label_set_text(checkin_status_lbl, "通知成功 ✓");
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
