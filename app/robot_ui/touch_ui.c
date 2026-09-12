/**
 * touch_ui.c - 老人友好触摸交互界面实现
 * 针对老人使用习惯优化
 */

#include "touch_ui.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ==================== 全局变量 ==================== */
static lv_obj_t *current_screen = NULL;
static lv_obj_t *menu_panel = NULL;
static lv_obj_t *setting_panel = NULL;
static lv_obj_t *reminder_panel = NULL;

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

/* ==================== 初始化老人友好样式 ==================== */
static void init_elder_styles(void)
{
    /* 老人友好基础样式 */
    lv_style_init(&style_elder);
    lv_style_set_bg_color(&style_elder, lv_color_hex(0x1A1A2E));
    lv_style_set_bg_opa(&style_elder, LV_OPA_COVER);
    lv_style_set_text_color(&style_elder, lv_color_hex(0xFFFFFF));
    lv_style_set_text_font(&style_elder, &lv_font_montserrat_24);
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
    lv_style_set_text_font(&style_big_btn, &lv_font_montserrat_20);
    lv_style_set_pad_all(&style_big_btn, 20);

    /* 菜单项样式 */
    lv_style_init(&style_menu_item);
    lv_style_set_bg_color(&style_menu_item, lv_color_hex(0x2D2D44));
    lv_style_set_bg_opa(&style_menu_item, LV_OPA_COVER);
    lv_style_set_radius(&style_menu_item, 15);
    lv_style_set_border_width(&style_menu_item, 2);
    lv_style_set_border_color(&style_menu_item, lv_color_hex(0x4CAF50));
    lv_style_set_text_color(&style_menu_item, lv_color_hex(0xFFFFFF));
    lv_style_set_text_font(&style_menu_item, &lv_font_montserrat_22);
    lv_style_set_pad_all(&style_menu_item, 25);

    /* 返回按钮样式 */
    lv_style_init(&style_back_btn);
    lv_style_set_bg_color(&style_back_btn, lv_color_hex(0x607D8B));
    lv_style_set_bg_opa(&style_back_btn, LV_OPA_COVER);
    lv_style_set_radius(&style_back_btn, 25);
    lv_style_set_text_color(&style_back_btn, lv_color_hex(0xFFFFFF));
    lv_style_set_text_font(&style_back_btn, &lv_font_montserrat_18);

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
            lv_label_set_text(title, "[M] Main Menu");
            break;
        case MENU_TYPE_REMIND:
            lv_label_set_text(title, "[T] Reminders");
            break;
        case MENU_TYPE_SETTING:
            lv_label_set_text(title, "[S] Settings");
            break;
        case MENU_TYPE_ABOUT:
            lv_label_set_text(title, "[?] About");
            break;
        default:
            lv_label_set_text(title, "Menu");
            break;
    }
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFEB3B), 0);
    lv_obj_set_style_pad_bottom(title, 20, 0);

    /* 根据菜单类型创建内容 */
    switch (type) {
        case MENU_TYPE_MAIN:
            create_menu_item(menu_panel, "[V] Voice Chat", "Talk with robot", 0);
            create_menu_item(menu_panel, "[T] View Reminders", "Check today's reminders", 1);
            create_menu_item(menu_panel, "[S] Settings", "Volume, brightness etc.", 2);
            create_menu_item(menu_panel, "[C] Emergency Call", "Contact family", 3);
            create_menu_item(menu_panel, "[?] About", "Version info", 4);
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
    lv_obj_set_style_text_font(icon_label, &lv_font_montserrat_28, 0);

    /* 副标题 */
    lv_obj_t *sub_label = lv_label_create(item);
    lv_label_set_text(sub_label, subtitle);
    lv_obj_set_style_text_color(sub_label, lv_color_hex(0x9E9E9E), 0);
    lv_obj_set_style_text_font(sub_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_left(sub_label, 15, 0);

    /* 右箭头 */
    lv_obj_t *arrow = lv_label_create(item);
    lv_label_set_text(arrow, ">");
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(arrow, lv_color_hex(0x9E9E9E), 0);
}

/* ==================== 创建提醒列表 ==================== */
static void create_reminder_list_items(lv_obj_t *parent)
{
    if (reminder_count == 0) {
        /* 空提醒 */
        lv_obj_t *empty = lv_label_create(parent);
        lv_label_set_text(empty, "No reminders\n\nTap + to add");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x9E9E9E), 0);
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
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_18, 0);

    /* 提醒标题 */
    lv_obj_t *title_label = lv_label_create(item);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_pad_left(title_label, 15, 0);
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
    lv_label_set_text(title, "[S] Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFEB3B), 0);
    lv_obj_set_style_pad_bottom(title, 20, 0);

    /* 音量设置 */
    create_slider_setting(setting_panel, "[V] Volume", user_settings.volume, 0);

    /* 亮度设置 */
    create_slider_setting(setting_panel, "[B] Brightness", user_settings.brightness, 1);

    /* 自动提醒开关 */
    create_switch_setting(setting_panel, "[T] Auto Remind", user_settings.auto_remind, 2);

    /* 提醒间隔 */
    create_interval_setting(setting_panel, "[I] Remind Interval", user_settings.remind_interval, 3);

    /* 恢复默认设置按钮 */
    lv_obj_t *btn_reset = lv_btn_create(setting_panel);
    lv_obj_set_size(btn_reset, LV_PCT(80), 60);
    lv_obj_add_style(btn_reset, &style_back_btn, 0);
    lv_obj_add_event_cb(btn_reset, setting_reset_event_handler, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_reset = lv_label_create(btn_reset);
    lv_label_set_text(lbl_reset, "[R] Reset Default");
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
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_22, 0);

    lv_obj_t *value_label = lv_label_create(title_row);
    lv_label_set_text_fmt(value_label, "%d%%", value);
    lv_obj_set_style_text_font(value_label, &lv_font_montserrat_18, 0);
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
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_22, 0);

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
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_22, 0);
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
    const char *interval_texts[] = {"30min", "1hr", "2hr", "3hr"};

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
        lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_14, 0);
        lv_obj_center(btn_label);
    }
}

/* ==================== 创建关于信息 ==================== */
static void create_about_info(lv_obj_t *parent)
{
    /* 关于信息 */
    lv_obj_t *info = lv_label_create(parent);
    lv_label_set_text(info,
        "ZhiAi Companion\n"
        "Version: v1.0.0\n\n"
        "AI Elder Companion\n"
        "Guardian Terminal\n\n"
        "Board: SF32LB52-DevKit-LCD\n"
        "GUI: LVGL\n\n"
        "2026 ZhiAi Team");
    lv_obj_set_style_text_color(info, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_18, 0);
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
    lv_label_set_text(lbl, "< Back");
    lv_obj_center(lbl);
}

/* ==================== 事件处理函数 ==================== */

/* 菜单项点击事件 */
static void menu_item_event_handler(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);

    /* 触摸反馈 */
    touch_ui_play_sound("click");

    switch (index) {
        case 0: // 语音聊天
            touch_ui_set_mode(MODE_LISTENING);
            break;
        case 1: // 查看提醒
            touch_ui_show_menu(MENU_TYPE_REMIND);
            break;
        case 2: // 系统设置
            touch_ui_show_menu(MENU_TYPE_SETTING);
            break;
        case 3: // 紧急联系
            touch_ui_show_setting_detail("Emergency", "Calling family...\nPlease wait");
            break;
        case 4: // 关于
            touch_ui_show_menu(MENU_TYPE_ABOUT);
            break;
        default:
            break;
    }
}

/* 返回按钮点击事件 */
static void back_button_event_handler(lv_event_t *e)
{
    touch_ui_play_sound("back");
    touch_ui_go_back();
}

/* 滑块值改变事件 */
static void setting_slider_event_handler(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    int value = lv_slider_get_value(slider);

    switch (index) {
        case 0: // 音量
            user_settings.volume = value;
            break;
        case 1: // 亮度
            user_settings.brightness = value;
            // TODO: 实际调整屏幕亮度
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
    show_confirm_dialog("Reset", "Reset to default settings?",
                       confirm_dialog_event_handler);
}

/* 间隔按钮点击事件 */
static void interval_button_event_handler(lv_event_t *e)
{
    int interval = (int)(intptr_t)lv_event_get_user_data(e);
    user_settings.remind_interval = interval;
    touch_ui_play_sound("click");

    /* 刷新界面以显示新的选中状态 */
    touch_ui_show_menu(MENU_TYPE_SETTING);
}

/* 确认对话框事件 */
static void confirm_dialog_event_handler(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    int action = (int)(intptr_t)lv_event_get_user_data(e);

    /* 关闭对话框 */
    lv_obj_t *mbox = lv_obj_get_parent(btn);
    lv_msgbox_close(mbox);

    if (action == 1) { // 确认
        /* 恢复默认设置 */
        user_settings.volume = 70;
        user_settings.brightness = 80;
        user_settings.auto_remind = true;
        user_settings.remind_interval = 60;

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
    lv_obj_t *btn_confirm = lv_msgbox_add_footer_button(mbox, "OK");
    lv_obj_t *btn_cancel = lv_msgbox_add_footer_button(mbox, "Cancel");

    lv_obj_center(mbox);
    lv_obj_set_style_bg_color(mbox, lv_color_hex(0x2D2D44), 0);
    lv_obj_set_style_text_color(mbox, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(mbox, &lv_font_montserrat_18, 0);

    /* 添加按钮事件 */
    lv_obj_add_event_cb(btn_cancel, callback, LV_EVENT_CLICKED, (void *)(intptr_t)0);
    lv_obj_add_event_cb(btn_confirm, callback, LV_EVENT_CLICKED, (void *)(intptr_t)1);
}

/* ==================== 公共接口实现 ==================== */

/* 设置模式 */
void touch_ui_set_mode(robot_mode_t mode)
{
    current_mode = mode;

    /* 根据模式更新界面 */
    switch (mode) {
        case MODE_LISTENING:
            touch_ui_show_setting_detail("Voice Chat", "Listening...\nPlease speak");
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
    /* TODO: 保存到 Flash/文件系统 */
    printf("Settings saved: vol=%d, bright=%d\n",
           user_settings.volume, user_settings.brightness);
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
    lv_obj_set_style_text_font(mbox, &lv_font_montserrat_20, 0);
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
