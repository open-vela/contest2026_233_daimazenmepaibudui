/**
 * robot_ui.c - 智爱陪伴机器人界面实现
 * SF32LB52-DevKit-LCD LVGL 界面开发
 */

#include "robot_ui.h"
#include "touch_ui.h"
#include "network_comm.h"     /* report_alarm()：报警要上报 MQTT + 手机推送 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>           /* malloc/free：询问页按钮的回答要先投一拍再处理 */
#include <time.h>             /* time() / localtime_r()：状态栏时钟用 */
#include <stdbool.h>
#include <stdint.h>           /* intptr_t：按钮 user_data 里塞"确认/不用" */
#include <unistd.h>           /* usleep()：等 hello_app 交出麦克风时的轮询/续租间隔 */
#include <pthread.h>          /* 报警出声线程（报警声不能做在 LVGL 线程里，见下面那节） */

/* 跨线程投递口（ui_async.c）：询问页按钮的回答也走它 —— 理由见下面询问页那一节
 * （在对象自己的事件回调里删自己，LVGL 的事件链表正在遍历的节点就没了）。 */
#include "ui_async.h"

/* 字库字形清洗（robot_ui_bridge.c 包装的 main.c sanitize_for_display）：
 * 询问页显示的原因串是外面传进来的任意文本，显示前要洗一遍。 */
#include "robot_ui_bridge.h"

/* 请 hello_app 干一件事的跨 app 入口（**非阻塞**：只登记请求，一个设备都不碰）。
 * 本文件现在只用它一个函数：ai_companion_ask_abort() —— 报警真的走起来了，
 * 让 hello_app 那条语音追问收摊（见 robot_ui_show_alarm）。
 * 头文件是自给自足的（只依赖 stdbool/stdint），实现在 app/hello_app 里，
 * 符号在最终链接时解析（单一大镜像）；头文件路径由 CMakeLists.txt 的
 * INCLUDE_DIRECTORIES `../hello_app` 提供，无需改构建脚本。
 * ★ 2026-09-21：原来的「让路 / 收回」整套删掉了，详见下面报警那一节。 */
#include "ai_companion_req.h"

/* 状态栏时钟：定义在本文件后面（ui_clock_refresh / ui_clock_timer_cb），
 * 状态栏创建时立刻刷新一次，并挂一个定时器周期刷新。 */
static void ui_clock_refresh(void);
static void ui_clock_timer_cb(lv_timer_t *t);
static lv_timer_t *clock_timer = NULL;

/* 板级报警模块（board/contest_board/src/sf32lb52_alarm.h）。
 * 头文件路径由 CMakeLists.txt 的 INCLUDE_DIRECTORIES ${NUTTX_BOARD_ABS_DIR}/src 提供。 */
#include "sf32lb52_alarm.h"

/* 中文字库（实现在 lv_font_ui_16/20/24.c，见 CMakeLists.txt 的 SRCS）。
 * 字符集是常用汉字全集（GB2312 6763 字 + ASCII + CJK 标点 + 全角），
 * 按改动前 montserrat 的字号分三档：
 *   14/16/18 -> lv_font_ui_16   20/22/24 -> lv_font_ui_20   >=28 -> lv_font_ui_24 */
LV_FONT_DECLARE(lv_font_ui_16);
LV_FONT_DECLARE(lv_font_ui_20);
LV_FONT_DECLARE(lv_font_ui_24);

/* ==================== 全局变量 ==================== */
static lv_obj_t *scr_main = NULL;      // 主屏幕
static lv_obj_t *scr_alarm = NULL;     // 报警屏幕

/* 主界面组件 */
/* lbl_status：**不再是状态栏上那个"[在线]"**（状态栏那三样 2026-09-14 已被用户
 * 要求删掉，见 create_status_bar() 的注释），2026-09-16 挪到主界面「AI 回复区」
 * 里当"语音状态小字"：用户原话「既然我们做了常态语音，不如在主页那个对话框加上
 * 状态」。所以它现在**是会显示的**，由 create_ai_reply_area() 建出来、
 * robot_ui_set_status() 写文字和颜色。
 *
 * lbl_net：仍然不在任何地方创建，恒为 NULL（robot_ui_set_net_status() 内部判 NULL
 * 后直接返回）。保留声明是为了不动 main.c 里那些调用点。 */
static lv_obj_t *lbl_status = NULL;    // 语音状态小标签（建在 AI 回复区里）
static lv_obj_t *lbl_time = NULL;      // 时间标签
static lv_obj_t *lbl_net = NULL;       // 网络状态标签（已不再创建）
static lv_obj_t *lbl_face = NULL;      // 表情标签
static lv_obj_t *lbl_ai_reply = NULL;  // AI回复标签
static lv_obj_t *lbl_reminder = NULL;  // 提醒标签

/* 按钮 */
static lv_obj_t *btn_remind = NULL;    // 提醒按钮
static lv_obj_t *btn_setting = NULL;   // 设置按钮
static lv_obj_t *btn_alarm = NULL;     // 报警按钮

/* 报警页的 "!!!" 图标：闪烁只动它一个（原来动的是整屏，见 alarm_blink_timer_cb） */
static lv_obj_t *lbl_alarm_icon = NULL;

/* 动画 */
static lv_anim_t anim_face = {0};      // 表情动画

/* 报警闪烁：低频定时器，创建后先暂停，报警时 resume（见 robot_ui_show_alarm） */
static lv_timer_t *alarm_blink_timer = NULL;

/* 询问页（「检测到异常声响，要报警吗？」）的那两个量。
 * 定义放在这里、而不是跟下面那一节的其它量放在一起：robot_ui_show_alarm() 要
 * 在报警页上来时把询问页让位（它比这一节早 200 行），得先看见这两个名字。
 * 询问页本身的实现和说明在文件后半「询问是否报警页」那一节。 */
static lv_obj_t *ask_panel = NULL;                       /* 顶层询问页（NULL = 没开） */
static char      ask_panel_reason[128];                  /* 这一条问的是什么 */

/* 当前状态 */
static robot_face_t current_face = ROBOT_FACE_HAPPY;
static robot_status_t current_status = ROBOT_STATUS_IDLE;

/* 样式 */
static lv_style_t style_bg;           // 背景样式
static lv_style_t style_btn;          // 按钮样式
static lv_style_t style_btn_alarm;    // 报警按钮样式
static lv_style_t style_text;         // 文字样式
static lv_style_t style_face;         // 表情样式

/* ==================== 表情数据 ==================== */
/* ASCII 表情 */
static const char *face_array[] = {
    "(^_^)",     // 开心
    "(=_=)",     // 思考
    "(~_~)zZZ", // 困倦
    "(O_O)",     // 惊讶
    "(>_<)",     // 担心
    "(! ! !)",   // 报警
};

/* ==================== 内部函数声明 ==================== */
static void init_styles(void);
static void create_main_screen(void);
static void create_alarm_screen(void);
static void create_status_bar(lv_obj_t *parent);
static void create_face_area(lv_obj_t *parent);
static void create_ai_reply_area(lv_obj_t *parent);
static void create_bottom_buttons(lv_obj_t *parent);
static void btn_event_handler(lv_event_t *e);
static void alarm_blink_timer_cb(lv_timer_t *t);

/* ==================== 初始化样式 ==================== */
static void init_styles(void)
{
    /* 背景样式 - 深蓝色 */
    lv_style_init(&style_bg);
    lv_style_set_bg_color(&style_bg, lv_color_hex(0x1A1A2E));
    lv_style_set_bg_opa(&style_bg, LV_OPA_COVER);
    lv_style_set_border_width(&style_bg, 0);
    lv_style_set_radius(&style_bg, 0);

    /* 按钮样式 - 绿色 */
    lv_style_init(&style_btn);
    lv_style_set_bg_color(&style_btn, lv_color_hex(0x4CAF50));
    lv_style_set_bg_opa(&style_btn, LV_OPA_COVER);
    lv_style_set_radius(&style_btn, 15);
    lv_style_set_shadow_width(&style_btn, 10);
    lv_style_set_shadow_color(&style_btn, lv_color_hex(0x388E3C));

    /* 报警按钮样式 - 红色 */
    lv_style_init(&style_btn_alarm);
    lv_style_set_bg_color(&style_btn_alarm, lv_color_hex(0xF44336));
    lv_style_set_bg_opa(&style_btn_alarm, LV_OPA_COVER);
    lv_style_set_radius(&style_btn_alarm, 15);
    lv_style_set_shadow_width(&style_btn_alarm, 10);
    lv_style_set_shadow_color(&style_btn_alarm, lv_color_hex(0xD32F2F));

    /* 文字样式 - 白色 */
    lv_style_init(&style_text);
    lv_style_set_text_color(&style_text, lv_color_hex(0xFFFFFF));
    lv_style_set_text_font(&style_text, &lv_font_ui_16);

    /* 表情样式 - 大号字体 */
    lv_style_init(&style_face);
    lv_style_set_text_color(&style_face, lv_color_hex(0xFFEB3B));
    lv_style_set_text_font(&style_face, &lv_font_ui_24);
}

/* ==================== 创建主屏幕 ==================== */
static void create_main_screen(void)
{
    /* 创建主屏幕 */
    scr_main = lv_obj_create(NULL);
    lv_obj_add_style(scr_main, &style_bg, 0);

    /* 创建状态栏 */
    create_status_bar(scr_main);

    /* 创建表情区域 */
    create_face_area(scr_main);

    /* 创建 AI 回复区域 */
    create_ai_reply_area(scr_main);

    /* 创建底部按钮 */
    create_bottom_buttons(scr_main);

    /* 加载主屏幕 */
    lv_scr_load(scr_main);
}

/* ==================== 状态栏时钟 ==================== */

/* 时区偏移：本构建没开 CONFIG_LIBC_LOCALTIME，`localtime` 实际就是 `gmtime`，
 * 也就是系统时钟（RTC）给的是 **UTC**。这里手动加偏移显示北京时间（UTC+8）。
 * 换时区只改这一个值即可（负值表示西半球）。
 *
 * 注：更"正规"的做法是开 CONFIG_LIBC_LOCALTIME 并设 TZ，但那会换掉
 * mktime/localtime 的实现，而 `RTC_SET_RELATIVE`（sf32lb_rtc.c 的 add_timeout）
 * 正好依赖现在这套简化日历换算，**不建议现在动**；显示层加偏移最省事也最可控。 */
#define UI_TZ_OFFSET_SEC  (8 * 3600)

/* 从系统时钟（硬件 RTC）读一次，刷新状态栏上的时间标签（本地时间）。 */
static void ui_clock_refresh(void)
{
    time_t now = time(NULL);
    struct tm tm_buf;

    if (lbl_time == NULL) {
        return;
    }

    /* time() <= 0：说明时钟还没被设过（板上没有 RTC 备份电池，
     * 冷启动会读到 1900/2000 这类值）—— 用 "--:--" 明确表示"时间不可信"，
     * 比继续显示一个假的时间好。 */
    if (now <= 0) {
        lv_label_set_text(lbl_time, "--:--");
        return;
    }

    /* UTC -> 本地时间（跨天由加法自然进位） */
    if (UI_TZ_OFFSET_SEC != 0) {
        now += UI_TZ_OFFSET_SEC;
    }

    if (localtime_r(&now, &tm_buf) == NULL) {
        lv_label_set_text(lbl_time, "--:--");
        return;
    }

    lv_label_set_text_fmt(lbl_time, "%02d:%02d",
                          tm_buf.tm_hour, tm_buf.tm_min);
}

static void ui_clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    ui_clock_refresh();
}

/* ==================== 创建状态栏 ==================== */
/* 状态栏上的「菜单」按钮：主菜单的保底入口。
 * 主菜单只在开机时显示一次，关掉之后要么右滑（手势，见 touch_ui.c），
 * 要么点这里。两者都调 touch_ui_show_menu(MENU_TYPE_MAIN)。 */
static void menu_button_event_handler(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        touch_ui_show_menu(MENU_TYPE_MAIN);
    }
}

static void create_status_bar(lv_obj_t *parent)
{
    /* 状态栏容器。**现在只放两样：时钟 + 「菜单」按钮。**
     *
     * 2026-09-14 用户要求删掉原来那三样：「[在线]」状态、「WiFi 100%」、「NET --」——
     * 对老人没有任何用处（电量/信号本来就是写死的假值，网络状态只会让人困惑），
     * 挤在 40px 高的栏里还占地方。删掉之后：
     *   - 时间保留（它是真的，联网自动对时之后是准的）；
     *   - 「菜单」按钮**做大**（130×40，占满整栏高度）—— 它是手势失灵时进主菜单
     *     唯一的保底入口，老人手指粗，越大越好按。
     *
     * 注意：`robot_ui_set_status()` / `robot_ui_set_net_status()` 两个 setter
     * 都**保留**了（内部都有 NULL 检查），main.c 里那十几处调用点一行都不用改：
     *   - set_net_status()：对应标签仍然不创建，恒为 NULL，没有任何显示效果；
     *   - set_status()：2026-09-16 起**有显示效果了** —— 它写的不再是状态栏，
     *     而是主界面「AI 回复区」里那一行语音状态小字（见 create_ai_reply_area()
     *     和 robot_ui_set_status()）。状态栏这一块仍然只有时钟 + 菜单按钮。 */
    lv_obj_t *bar = lv_obj_create(parent);
    /* 高度 40 → **64**：菜单按钮要做到 52 高才够好点（40 的栏里塞不下），
     * 顺便让整块状态栏不那么挤。 */
    lv_obj_set_size(bar, LV_PCT(100), 64);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x2D2D44), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    /* ⚠️ 这块屏是 **390×450 的圆角屏**：四个角是物理圆角，贴边的控件既会被
     * 视觉裁掉、也不好点按（用户反馈"菜单栏不好点"就是这个原因）。
     * 所以这里**不用 flex 排布**，改成两个子对象各自对齐：
     *   时间  -> 居中（用户要求）
     *   按钮  -> 右对齐 + 内缩 28px（躲开右上圆角）
     * 圆角半径大约 20~30px，取 28 作为安全内缩。 */

    /* 时间标签（居中）：从系统时钟（硬件 RTC）读真实时间，并定时刷新。
     *
     * 原来这里是写死的 lv_label_set_text(lbl_time, "12:00")，所以主页面顶上
     * **永远显示 12:00**，跟板子时间毫无关系。
     *
     * 数据源用 time(NULL)：NuttX 启动时用 RTC 初始化系统时钟，
     * 而 RTC_SET_TIME（含 NSH 的 `date -s`）会同步系统时钟，所以它就是板子的时间。
     * 板上没有 RTC 备份电池，掉电后时间会丢，开机后靠联网自动对时校正
     * （见 time_sync.c）；也可以手动：date -s "Sep 13 12:00:00 2026"
     *
     * 注意：本构建没开 CONFIG_LIBC_LOCALTIME，`localtime` 实际就是 `gmtime`，
     * 所以**显示的是 UTC**，比北京时间少 8 小时。要显示本地时间得自己加偏移
     * （这里刻意不加，保持"屏幕上显示的就是 RTC 里的值"，免得排查时对不上）。 */
    lbl_time = lv_label_create(bar);
    lv_obj_set_style_text_color(lbl_time, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_time, &lv_font_ui_24, 0);
    lv_obj_align(lbl_time, LV_ALIGN_CENTER, 0, 0);           /* 时间居中 */
    ui_clock_refresh();                                      /* 先立刻显示一次 */
    clock_timer = lv_timer_create(ui_clock_timer_cb, 10000, NULL);  /* 每 10 秒刷一次 */

    /* 「菜单」按钮：主菜单的保底入口（手势不灵时也能进）。
     * 120×52 = 比原来的 LV_SIZE_CONTENT×36 大得多（宽度几乎翻倍、高度 +44%），
     * 而且**从右上圆角里挪出来**（右移 -28px）—— 之前贴在屏幕角上，
     * 圆角把可点区域切掉一块，所以"不好点"。 */
    lv_obj_t *btn_menu = lv_btn_create(bar);
    lv_obj_set_size(btn_menu, 120, 52);
    lv_obj_align(btn_menu, LV_ALIGN_RIGHT_MID, -28, 0);
    lv_obj_add_style(btn_menu, &style_btn, 0);
    lv_obj_add_event_cb(btn_menu, menu_button_event_handler,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_menu = lv_label_create(btn_menu);
    lv_label_set_text(lbl_menu, "菜单");
    lv_obj_set_style_text_font(lbl_menu, &lv_font_ui_24, 0);
    lv_obj_center(lbl_menu);
}

/* ==================== 更新网络状态 ==================== */
void robot_ui_set_net_status(const char *text)
{
    if (lbl_net == NULL || text == NULL) {
        return;
    }

    if (strcmp(lv_label_get_text(lbl_net), text) == 0) {
        return;     /* 没变就不动，省一次重绘 */
    }

    lv_label_set_text(lbl_net, text);
    lv_obj_set_style_text_color(lbl_net,
                                strncmp(text, "NET OK", 6) == 0
                                    ? lv_color_hex(0x4CAF50)
                                    : lv_color_hex(0xFFC107),
                                0);
}

/* ==================== 创建表情区域 ==================== */
static void create_face_area(lv_obj_t *parent)
{
    /* 表情容器 */
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_set_size(container, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(container, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 表情标签 */
    lbl_face = lv_label_create(container);
    lv_label_set_text(lbl_face, face_array[ROBOT_FACE_HAPPY]);
    lv_obj_add_style(lbl_face, &style_face, 0);
    lv_obj_set_style_text_font(lbl_face, &lv_font_ui_24, 0);

    /* 状态文字 */
    lbl_reminder = lv_label_create(container);
    lv_label_set_text(lbl_reminder, "Hello! I am ZhiAi.");
    lv_obj_set_style_text_color(lbl_reminder, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(lbl_reminder, &lv_font_ui_24, 0);
    lv_obj_set_style_pad_top(lbl_reminder, 10, 0);

    /* 启动表情动画 - 上下浮动 */
    lv_anim_init(&anim_face);
    lv_anim_set_var(&anim_face, lbl_face);
    lv_anim_set_values(&anim_face, -10, 10);
    lv_anim_set_time(&anim_face, 1000);
    lv_anim_set_playback_time(&anim_face, 1000);
    lv_anim_set_repeat_count(&anim_face, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&anim_face, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_start(&anim_face);
}

/* ==================== 创建 AI 回复区域 ==================== */

/* 语音状态小标签（lbl_status）的四档配色：听=蓝、想=橙、说=绿、空闲=灰。
 *
 * 和语音镜像面板（touch_ui.c 的 voice_state_color）**用的是同一套色值** ——
 * 老人在主界面和面板里看到的"蓝色=在听、橙色=在想、绿色=在说"是同一个意思，
 * 换个屏幕不用重新学。
 *
 * 写成宏是因为它有两处用：下面建标签时的初始颜色，和 robot_ui_set_status()
 * 里每一档的写入（两处必须一致，散着写迟早会漂）。 */
#define UI_STATUS_COLOR_IDLE      lv_color_hex(0xBDBDBD)   /* 灰：空闲，不抢眼 */
#define UI_STATUS_COLOR_LISTENING lv_color_hex(0x42A5F5)   /* 蓝：在听 */
#define UI_STATUS_COLOR_THINKING  lv_color_hex(0xFFA726)   /* 橙：在想 */
#define UI_STATUS_COLOR_SPEAKING  lv_color_hex(0x66BB6A)   /* 绿：在说 */

static void create_ai_reply_area(lv_obj_t *parent)
{
    /* AI 回复容器。
     *
     * 高度 120 -> **150**（2026-09-16）：下面那一行语音状态小字（20 号字 21px
     * + 6px 上边距 ≈ 27px）要有地方放，不然它会反过来把上面两行（[AI] 图标 +
     * 回复正文）往上挤 —— 要求就是"别挤掉现有内容"。
     * 容器本身仍然是"居中 + 向下 20"，所以这 30px 是上下各长 15px：
     * 上沿离表情区还有 40 多像素、下沿离底部按钮栏还有 50 像素，都不碰。 */
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_set_size(container, LV_PCT(90), 150);
    lv_obj_align(container, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_bg_color(container, lv_color_hex(0x2D2D44), 0);
    lv_obj_set_style_radius(container, 20, 0);
    lv_obj_set_style_border_width(container, 2, 0);
    lv_obj_set_style_border_color(container, lv_color_hex(0x4CAF50), 0);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(container, 15, 0);

    /* AI 说话图标 */
    lv_obj_t *lbl_icon = lv_label_create(container);
    lv_label_set_text(lbl_icon, "[AI]");
    lv_obj_set_style_text_color(lbl_icon, lv_color_hex(0x4CAF50), 0);
    lv_obj_set_style_text_font(lbl_icon, &lv_font_ui_24, 0);

    /* AI 回复内容 */
    lbl_ai_reply = lv_label_create(container);
    lv_label_set_text(lbl_ai_reply, "Hello!\nHow can I help you?");
    lv_obj_set_style_text_color(lbl_ai_reply, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_ai_reply, &lv_font_ui_24, 0);
    lv_label_set_long_mode(lbl_ai_reply, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_ai_reply, LV_PCT(100));
    lv_obj_set_style_text_align(lbl_ai_reply, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(lbl_ai_reply, 5, 0);

    /* 语音状态小标签：主界面上「空闲 / 在听… / 在想… / 在说…」那一行。
     *
     * 用户原话：「既然我们做了常态语音，不如在主页那个对话框加上状态」——
     * 不弹开语音面板，扫一眼主界面就知道它现在在干什么。
     *
     * 谁写它：只有 robot_ui_set_status()（四档的文字和颜色都在那边）。
     * 这里只把控件建出来挂到 lbl_status 上，**不另开一条状态来源** ——
     * 语音状态从 hello_app 的 voice_state_probe() 出来，一路经
     * robot_ui_bridge_voice_state() / MQTT 的 voice_state 分支 → ui_post()
     * （内部 lv_async_call）投到 LVGL 线程，最后落到那一个 setter 上。
     *
     * 字号取 20（比上面两行的 24 小一档）：它是状态提示，不该跟 AI 的正文抢
     * 注意力；又没有小到看不清 —— 这一屏是给老人用的，16 那一档本工程只用在
     * 辅助信息上。颜色由 setter 按状态给，初始是"空闲"的灰。 */
    lbl_status = lv_label_create(container);
    lv_label_set_text(lbl_status, "空闲");
    lv_obj_set_style_text_color(lbl_status, UI_STATUS_COLOR_IDLE, 0);
    lv_obj_set_style_text_font(lbl_status, &lv_font_ui_20, 0);
    lv_obj_set_style_pad_top(lbl_status, 6, 0);
}

/* ==================== 创建底部按钮 ==================== */
static void create_bottom_buttons(lv_obj_t *parent)
{
    /* 按钮容器 */
    /* ⚠️ 这块屏是圆角屏（390×450）：容器铺满 100% 宽的话，最左/最右那个按钮
     * 会压在**左下/右下圆角**上 —— 视觉被切一块，触摸也可能点不到。
     * 所以左右各内缩 28px，三个按钮整体往里收（用户反馈"不好点"）。 */
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_set_size(container, LV_PCT(100), 80);
    lv_obj_align(container, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_left(container, 28, 0);
    lv_obj_set_style_pad_right(container, 28, 0);
    lv_obj_set_style_pad_bottom(container, 6, 0);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 提醒按钮 */
    btn_remind = lv_btn_create(container);
    lv_obj_set_size(btn_remind, 90, 50);
    lv_obj_add_style(btn_remind, &style_btn, 0);
    lv_obj_add_event_cb(btn_remind, btn_event_handler, LV_EVENT_CLICKED, (void *)UI_VIEW_REMIND);
    lv_obj_t *lbl_btn1 = lv_label_create(btn_remind);
    /* 中文标签（字库已经是 GB2312 全集，"提醒"两个字有字形）——
     * 原来写的 "Remind"，主屏其它按钮（报警）都是中文，统一一下更认得出。 */
    lv_label_set_text(lbl_btn1, "提醒");
    lv_obj_set_style_text_font(lbl_btn1, &lv_font_ui_24, 0);
    lv_obj_center(lbl_btn1);

    /* 设置按钮 */
    btn_setting = lv_btn_create(container);
    lv_obj_set_size(btn_setting, 90, 50);
    lv_obj_add_style(btn_setting, &style_btn, 0);
    lv_obj_add_event_cb(btn_setting, btn_event_handler, LV_EVENT_CLICKED, (void *)UI_VIEW_SETTING);
    lv_obj_t *lbl_btn2 = lv_label_create(btn_setting);
    lv_label_set_text(lbl_btn2, "Setting");
    lv_obj_set_style_text_font(lbl_btn2, &lv_font_ui_24, 0);
    lv_obj_center(lbl_btn2);

    /* 报警按钮 */
    btn_alarm = lv_btn_create(container);
    lv_obj_set_size(btn_alarm, 90, 50);
    lv_obj_add_style(btn_alarm, &style_btn_alarm, 0);
    lv_obj_add_event_cb(btn_alarm, btn_event_handler, LV_EVENT_CLICKED, (void *)UI_VIEW_ALARM);
    lv_obj_t *lbl_btn3 = lv_label_create(btn_alarm);
    lv_label_set_text(lbl_btn3, "报警");
    lv_obj_set_style_text_font(lbl_btn3, &lv_font_ui_24, 0);
    lv_obj_center(lbl_btn3);
}

/* ==================== 按钮事件处理 ==================== */
static void btn_event_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    ui_view_t view = (ui_view_t)lv_event_get_user_data(e);

    if (code == LV_EVENT_CLICKED) {
        switch (view) {
            case UI_VIEW_REMIND:
                /* 主屏左下角这个按钮以前只弹一句写死的英文示例（"Time to take
                 * medicine!"），和真正的提醒列表毫无关系。现在直接走**主菜单
                 * 「查看提醒」同一个入口** —— 多一个冗余入口，老人不用先点开主菜单。 */
                touch_ui_play_sound("click");
                touch_ui_show_menu(MENU_TYPE_REMIND);
                break;
            case UI_VIEW_SETTING:
                touch_ui_show_menu(MENU_TYPE_SETTING);
                break;
            case UI_VIEW_ALARM:
                robot_ui_show_alarm("Abnormal detected!\nPlease confirm if help is needed.");
                break;
            case UI_VIEW_MAIN:
                /* 报警界面 Back 按钮:关闭报警，返回主界面 */
                robot_ui_close_alarm();
                break;
            default:
                break;
        }
    }
}

/* ==================== 创建报警屏幕 ==================== */

/* 报警闪烁回调：在"满不透明"和"半透明"之间切换 "!!!" 图标的整体不透明度。
 *
 * 原来这里是绑在 **整个 scr_alarm** 上的无限 lv_anim（改 bg_opa）：每帧都让
 * 整屏（390x450）失效，实测一秒 80 次全屏重绘（正常空闲 20~30），用户看到的
 * 就是报警页一直在闪、而且整屏半透明时画面发暗。现在失效面积只剩一个小图标，
 * 频率也从 80Hz 降到 2Hz（500ms 一次）。 */
static void alarm_blink_timer_cb(lv_timer_t *t)
{
    (void)t;

    if (lbl_alarm_icon == NULL)
    {
        return;
    }

    lv_opa_t opa = lv_obj_get_style_opa(lbl_alarm_icon, 0);
    lv_obj_set_style_opa(lbl_alarm_icon, (opa == LV_OPA_COVER) ? LV_OPA_50 : LV_OPA_COVER, 0);
}

static void create_alarm_screen(void)
{
    /* 创建报警屏幕 */
    scr_alarm = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_alarm, lv_color_hex(0xF44336), 0);
    lv_obj_set_style_bg_opa(scr_alarm, LV_OPA_COVER, 0);

    /* 报警图标（闪烁只动它，不透明度的切换见 alarm_blink_timer_cb） */
    lbl_alarm_icon = lv_label_create(scr_alarm);
    lv_label_set_text(lbl_alarm_icon, "!!!");
    lv_obj_set_style_text_font(lbl_alarm_icon, &lv_font_ui_24, 0);
    lv_obj_set_style_text_color(lbl_alarm_icon, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(lbl_alarm_icon, LV_ALIGN_CENTER, 0, -60);

    /* 报警文字 */
    lv_obj_t *text = lv_label_create(scr_alarm);
    lv_label_set_text(text, "紧急");
    lv_obj_set_style_text_color(text, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(text, &lv_font_ui_24, 0);
    lv_obj_align(text, LV_ALIGN_CENTER, 0, 0);

    /* 报警详情 */
    lv_obj_t *detail = lv_label_create(scr_alarm);
    lv_label_set_text(detail, "检测到异常\n已通知家人");
    lv_obj_set_style_text_color(detail, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(detail, &lv_font_ui_24, 0);
    lv_obj_align(detail, LV_ALIGN_CENTER, 0, 40);

    /* 返回按钮 */
    lv_obj_t *btn_back = lv_btn_create(scr_alarm);
    lv_obj_set_size(btn_back, 120, 50);
    lv_obj_align(btn_back, LV_ALIGN_CENTER, 0, 100);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_radius(btn_back, 25, 0);
    lv_obj_add_event_cb(btn_back, btn_event_handler, LV_EVENT_CLICKED, (void *)UI_VIEW_MAIN);

    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "返回");
    lv_obj_set_style_text_color(lbl_back, lv_color_hex(0xF44336), 0);
    lv_obj_set_style_text_font(lbl_back, &lv_font_ui_24, 0);
    lv_obj_center(lbl_back);

    /* 报警闪烁：2Hz 低频定时器，创建后先暂停（报警页还没显示，不必闪）。
     * 报警时由 robot_ui_show_alarm() resume。 */
    alarm_blink_timer = lv_timer_create(alarm_blink_timer_cb, 500, NULL);
    lv_timer_pause(alarm_blink_timer);
}

/* ==================== 初始化 UI ==================== */
void robot_ui_init(void)
{
    /* 初始化样式 */
    init_styles();

    /* 创建屏幕 */
    create_main_screen();
    create_alarm_screen();

    /* 设置默认表情 */
    robot_ui_set_face(ROBOT_FACE_HAPPY);
}

/* ==================== 设置表情 ==================== */
void robot_ui_set_face(robot_face_t face)
{
    if (face >= ROBOT_FACE_MAX) return;

    current_face = face;

    if (lbl_face) {
        lv_label_set_text(lbl_face, face_array[face]);

        /* 根据表情改变颜色 */
        switch (face) {
            case ROBOT_FACE_HAPPY:
                lv_obj_set_style_text_color(lbl_face, lv_color_hex(0xFFEB3B), 0); // 黄色
                break;
            case ROBOT_FACE_THINKING:
                lv_obj_set_style_text_color(lbl_face, lv_color_hex(0x2196F3), 0); // 蓝色
                break;
            case ROBOT_FACE_SLEEPY:
                lv_obj_set_style_text_color(lbl_face, lv_color_hex(0x9E9E9E), 0); // 灰色
                break;
            case ROBOT_FACE_ALARM:
                lv_obj_set_style_text_color(lbl_face, lv_color_hex(0xF44336), 0); // 红色
                break;
            default:
                lv_obj_set_style_text_color(lbl_face, lv_color_hex(0xFFEB3B), 0);
                break;
        }
    }
}

/* ==================== 设置 AI 回复 ==================== */
void robot_ui_set_ai_reply(const char *text)
{
    if (lbl_ai_reply && text) {
        lv_label_set_text(lbl_ai_reply, text);
    }
}

/* ==================== 设置状态 ==================== */
/* 写的是主界面「AI 回复区」里那一行语音状态小字（create_ai_reply_area() 建的
 * lbl_status）。**状态栏上没有它** —— 状态栏那三样 2026-09-14 已被用户删掉。
 *
 * 四档和语音链路的四档一一对应（hello_app 的 voice_state_probe()）：
 *   空闲 IDLE       「空闲」  灰   不抢眼，它只是"没事，我在"
 *   在听 LISTENING  「在听…」 蓝
 *   在想 THINKING   「在想…」 橙   识别 / 等大模型这一段
 *   在说 SPEAKING   「在说…」 绿
 * 文案比语音镜像面板（touch_ui.c 的 voice_state_text：「检测到声音」「正在想…」
 * 「正在说话…」）**更短**：面板里那行是大字、有整行的地方，主界面这一行是夹在
 * 对话框里的小字，短一点才不会撑宽。颜色两边是同一套（见文件上面那几个
 * UI_STATUS_COLOR_*），老人换个屏幕不用重新学。
 *
 * 提醒中 / 报警两档沿用原来的措辞（它们是别的流程在借这一行，不是语音状态），
 * 只把原来的方括号去掉 —— 这一行现在是纯文字状态行，不是状态栏那种标签。
 *
 * ⚠️ 只能在 LVGL 线程里调（直接碰控件）。非 LVGL 线程一律走 main.c 的 ui_post()
 *    （内部 lv_async_call），见那里"跨线程改界面"那一段。
 * ⚠️ `default` 分支一个字都不改是**纪律**：认不出来的档位宁可少刷一次，也不要
 *    把这一行停在一个错的字上（touch_ui_set_voice_state 和 main.c 的 voice_state
 *    分支是同一条规矩）。 */
void robot_ui_set_status(robot_status_t status)
{
    current_status = status;

    if (lbl_status) {
        switch (status) {
            case ROBOT_STATUS_IDLE:
                lv_label_set_text(lbl_status, "空闲");
                lv_obj_set_style_text_color(lbl_status, UI_STATUS_COLOR_IDLE, 0);
                break;
            case ROBOT_STATUS_LISTENING:
                lv_label_set_text(lbl_status, "在听…");
                lv_obj_set_style_text_color(lbl_status, UI_STATUS_COLOR_LISTENING, 0);
                break;
            case ROBOT_STATUS_THINKING:
                lv_label_set_text(lbl_status, "在想…");
                lv_obj_set_style_text_color(lbl_status, UI_STATUS_COLOR_THINKING, 0);
                break;
            case ROBOT_STATUS_SPEAKING:
                lv_label_set_text(lbl_status, "在说…");
                lv_obj_set_style_text_color(lbl_status, UI_STATUS_COLOR_SPEAKING, 0);
                break;
            case ROBOT_STATUS_REMINDING:
                lv_label_set_text(lbl_status, "提醒中");
                lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x9C27B0), 0);
                break;
            case ROBOT_STATUS_ALARM:
                lv_label_set_text(lbl_status, "报警");
                lv_obj_set_style_text_color(lbl_status, lv_color_hex(0xF44336), 0);
                break;
            default:
                break;
        }
    }
}

/* ==================== 显示提醒 ==================== */
void robot_ui_show_reminder(const char *title, const char *content)
{
    /* 创建提醒弹窗 */
    lv_obj_t *mbox = lv_msgbox_create(NULL);
    if (mbox == NULL) return;

    lv_msgbox_add_title(mbox, title);
    lv_msgbox_add_text(mbox, content);
    touch_ui_msgbox_add_close_x(mbox);
    lv_obj_center(mbox);
    lv_obj_set_style_bg_color(mbox, lv_color_hex(0x2D2D44), 0);
    lv_obj_set_style_text_color(mbox, lv_color_hex(0xFFFFFF), 0);

    /* 字体必须显式指定：msgbox 默认吃 LVGL 主题字体，而本工程的默认字体是
     * 16px 的 simsun（约 1436 个字形），界面用到的一些汉字（"散""嘱"…）
     * 它会画成方块。这里统一成 build 里真正的三档字库之一，和 touch_ui 的
     * 弹窗一致（20px：比 24px 少占地方，长一点的提醒语不容易顶出屏幕）。 */
    lv_obj_set_style_text_font(mbox, &lv_font_ui_20, 0);
}

/* ==================== 报错提示页 ==================== */
/*
 * 为什么单独做一页：以前网络不通/请求失败时，界面只是"没反应"或者停在
 * "处理中…"，用户根本不知道出了什么事（现场原话："卡死不好看"）。
 * 这一页是**顶层覆盖**（建在当前活动屏上，不切屏、不动报警页），
 * 一个大按钮，点一下就关。
 *
 * ⚠️ 只能在 LVGL 线程里调；别的线程（语音工作线程 / MQTT 线程 / 播放线程）
 * 要弹它请用 main.c 的 ui_post_error()，那条路会 lv_async_call 投过来。
 */

static lv_obj_t *err_panel = NULL;

static void err_close_event_handler(lv_event_t *e)
{
    (void)e;

    if (err_panel != NULL) {
        lv_obj_del(err_panel);
        err_panel = NULL;
    }

    touch_ui_play_sound("back");
}

void robot_ui_show_error(const char *title, const char *content)
{
    lv_obj_t *scr = lv_scr_act();

    if (scr == NULL) {
        return;
    }

    /* 同一时刻只留一页：又报错了就换内容，不叠罗汉 */

    if (err_panel != NULL) {
        lv_obj_del(err_panel);
        err_panel = NULL;
    }

    err_panel = lv_obj_create(scr);
    lv_obj_set_size(err_panel, LV_PCT(92), LV_PCT(78));
    lv_obj_center(err_panel);
    lv_obj_set_style_bg_color(err_panel, lv_color_hex(0x3A1F1F), 0);
    lv_obj_set_style_bg_opa(err_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(err_panel, 3, 0);
    lv_obj_set_style_border_color(err_panel, lv_color_hex(0xE53935), 0);
    lv_obj_set_style_radius(err_panel, 20, 0);
    lv_obj_set_flex_flow(err_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(err_panel, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(err_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(err_panel);
    lv_label_set_text(t, (title != NULL && title[0] != '\0') ? title : "出错了");
    lv_obj_set_style_text_font(t, &lv_font_ui_24, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xFFCDD2), 0);

    lv_obj_t *c = lv_label_create(err_panel);
    lv_label_set_text(c, (content != NULL) ? content : "");
    lv_obj_set_style_text_font(c, &lv_font_ui_20, 0);
    lv_obj_set_style_text_color(c, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_width(c, LV_PCT(95));
    lv_label_set_long_mode(c, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *btn = lv_btn_create(err_panel);
    lv_obj_set_size(btn, LV_PCT(70), 70);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xE53935), 0);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_add_event_cb(btn, err_close_event_handler, LV_EVENT_CLICKED, NULL);

    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "知道了");
    lv_obj_set_style_text_font(bl, &lv_font_ui_24, 0);
    lv_obj_center(bl);

    printf("[UI] 报错页: %s | %s\n",
           (title != NULL) ? title : "", (content != NULL) ? content : "");
}

/* ================= 报警出声（2026-09-21：让路协议已删） =================
 *
 * 2026-09-15 定案过一条根因：报警当初是**唯一一条没做让路**的出声路径。
 *   - hello_app 的 ai_companion 是常开麦：开机起就占着 /dev/audio/audio0 的录音
 *     通路；本板音频是**半双工**、驱动里只有一个方向标志。
 *   - 板级报警模块 alarm_audio_open() 走 open -> CONFIGURE -> CONFIGURE(音量)
 *     -> START（board/contest_board/src/sf32lb52_alarm.c）。录音会话还是 RUNNING
 *     时，NuttX 音频上层（nuttx/audio/audio.c）那两处判断会**静默跳过** ——
 *     只有 state==OPEN 才把 CONFIGURE 转给驱动、只有 PREPARED/XRUN 才调下层
 *     start —— 最后照样 `return OK`：报警模块以为成功了，一行日志都没有。
 *   - 于是报警的 PCM 全写在录音会话上，DAC/功放从未打开、DMA 永不完成 →
 *     驱动每 5 秒打一条 `AUDIO: 等播放完成超时（buflen=1600），本块丢弃`，
 *     报警音一块都出不去，**全程无声**（现场日志实证）。
 *   - 对照：robot_ui 自己的提醒/播报有完整让路，所以它们正常出声 ——
 *     只有报警这条漏了。于是当时的对策是"出声前先请 hello_app 交出麦克风"。
 *
 * ★ 那套让路握手 2026-09-21 **整套删掉了**（用户拍板：「关闭让路…不需要做
 *   麦克风转让，因为我们的麦是常开的」；真机故障形状和取舍见
 *   app/hello_app/ai_companion_req.h 头上）：一个"整机一份、没有持有者概念"的
 *   方向电平，靠调用方自己守配对纪律（yield 之后到 reclaim 之间不许有任何
 *   return），真机上漏一次就是对方永久聋 —— 而麦克风本来不需要转让：
 *   放音时驱动会停掉录音通路并唤醒对方阻塞的 read（见 sf32lb52_audio 的
 *   `AUDIO: stop 唤醒 read`），对方（hello_app）自己的监听守护会在录音链路
 *   死了之后把它重开。
 *   所以报警这条路现在和提醒那条一样：**直接出声**，一个跨 app 调用都不需要；
 *   "报警期间续租让路"那件事也一起消失了。
 *
 * ⚠️ 为什么"出声 + 上报"仍然要放进一条**工作线程**，而不是就地做在
 * robot_ui_show_alarm() 里：
 *   show_alarm() 跑在 **LVGL 线程**（报警按钮的回调 / main.c 里 ui_post_panel 的
 *   投递）。alarm_trigger() 要碰板级音频设备，这一串动作没有理由压在那条线程上
 *   —— 这个项目因为"在 LVGL 线程里碰设备"冻过界面。所以拆成两半：
 *     ① LVGL 线程（robot_ui_show_alarm）：切页面 -> 置"要出声"标记 + 起出声线程；
 *     ② 工作线程（alarm_sound_worker）：出声 + 上报，报警期间每 1 秒看一次
 *        有没有新的触发，直到报警页关掉才收摊。
 *   出声/上报因此只晚一次线程调度（毫秒级）：页面仍然是第一步切出来的
 *   （"先切页面"那条踩坑教训不动）。
 */

/* 出声线程每拍等多久（毫秒）：认领"报警页还开着时又来的新触发"，顺带让出 CPU。
 * 报警最长 60 秒，这条线程大部分时间是睡着的。 */
#define ALARM_SOUND_POLL_MS        1000

/* 出声线程的栈：它只做 printf + 两个非阻塞调用（alarm_trigger 会碰板级音频设备、
 * report_alarm_queued 只入队），但 printf 和板级调用都要吃一些栈，照 main.c 里
 * 等价的那条线程给 16 KB，创建写法也一致（pthread_attr + DETACHED）。 */
#define ALARM_WORKER_STACK_SIZE    16384

/* 报警出声的共享状态。写者有两条线程：LVGL 线程（show_alarm / close_alarm）和
 * 报警出声线程，所以用一把锁护住，判据只看这三个布尔量。
 *   alarm_sound_active  —— 报警还没结束（出声线程据此决定继续还是收摊）
 *   alarm_sound_pending —— 有一次"出声 + 上报"还没做（show_alarm 每次调用置位）
 *   alarm_worker_up     —— 出声线程活着（活着就不再起第二条） */
static pthread_mutex_t alarm_audio_lock = PTHREAD_MUTEX_INITIALIZER;
static bool alarm_sound_active  = false;
static bool alarm_sound_pending = false;
static bool alarm_worker_up     = false;

/**
 * @brief  出声 + 上报（原样搬过来的两句，参数、顺序一个字都没改）
 *
 * 顺序是踩坑换来的：**先出声、再上报**（上报走网络 MQTT/TLS，慢或卡住时声音
 * 必须已经出去了）。页面那一步在调用方（robot_ui_show_alarm）里，仍然排在最前面。
 *
 * ⚠️ 只许工作线程调（出声要动设备，见本节头上那段）。
 */
static void alarm_sound_do(void)
{
    int ret;

    ret = alarm_trigger(ALARM_LEVEL_EMERGENCY, "ui",
                        "报警已触发，请尽快确认");
    if (ret != OK) {
        printf("robot_ui: alarm_trigger failed: %d\n", ret);
    }

    /* 上报：MQTT 发到 zhi_ai/<client_id>/alarm（+ 手机推送）。
     * 用**排队版** report_alarm_queued()，不能用 report_alarm()：本线程不是
     * network_task 的 task group，跨组直接 send() 那个 fd 必然失败（真机日志：
     * `[ALARM] report_alarm type=ui … ret=-1` 紧跟 `MQTT publish failed: -1`）。
     * 排队版只把消息拷进队列、由 network_task 去发，可从任意线程调。 */
    ret = report_alarm_queued("ui", "用户按下报警按钮");
    if (ret < 0) {
        /* 排队口的负值只表示**这条没进队列**：-EINVAL topic 空、-ENOSPC 队列满、
         * -EMSGSIZE 载荷超长（语义见 mqtt_publish_queued()）。跟 MQTT 连没连上
         * 无关 —— 连接由 network_task 自己维持，连不上是它那边重连的事。 */
        printf("robot_ui: report_alarm_queued 没入队: %d"
               "（队列满或载荷超长，这条不会发出去）\n", ret);
    }
}

/**
 * @brief  报警出声线程：出声 + 上报，直到报警结束
 *
 * 存活期 = 一次报警：close_alarm 会把 alarm_sound_active 清掉，本线程据此收摊。
 * 出声、上报**全在这一条线程里串行**（设备动作只有一条线程在做）。
 */
static void *alarm_sound_worker(void *arg)
{
    (void)arg;

    for (;;) {
        bool pending;

        pthread_mutex_lock(&alarm_audio_lock);

        /* 出声 + 上报。show_alarm 每调用一次置一次 pending，这里认领；
         * 报警页还开着时又来新的触发（声音检测 / MQTT 反复调 show_alarm）
         * 就再走一遍 —— alarm_trigger 对同级重复触发只更新 reason/text，
         * 不会响两遍（见 board/contest_board/src/sf32lb52_alarm.c）。
         * 判据带上 alarm_sound_active：用户很快点了"返回"（页面已经关了）
         * 就不要在这时候再补一声铃出来。两个判据在同一把锁里读，不会看串。 */
        pending = alarm_sound_pending && alarm_sound_active;
        alarm_sound_pending = false;

        if (!alarm_sound_active) {
            /* 报警页已经关了：收摊。和 show_alarm 共用这把锁 —— "收摊"和
             * "刚关掉就又被 show_alarm 拉起来"只差一步，两边都在锁里改状态，
             * 就不会出现"线程走了、新的触发挂在那里没人做"（那一声报警就永远
             * 不响了）：show_alarm 要么先看到 worker_up 还是真（那它就把 active
             * 置回去，本线程下面那一圈照常出声），要么看到 worker_up 已经是假
             * （那它自己会起一条新线程）。 */
            alarm_worker_up = false;
            pthread_mutex_unlock(&alarm_audio_lock);
            return NULL;
        }

        pthread_mutex_unlock(&alarm_audio_lock);

        if (pending) {
            alarm_sound_do();
        }

        /* 等下一个触发（顺带让出 CPU）。 */
        usleep(ALARM_SOUND_POLL_MS * 1000);
    }
}

/**
 * @brief  LVGL 线程侧：置"要出声"标记并保证出声线程活着
 *
 * 幂等：每次调用只把"这一次要出声 + 上报"挂到 pending，报警页已经开着时不会再起
 * 第二条线程。线程起不来时在这里就地出声兜底 —— 不能因为线程起不来就让这次报警
 * 一声不响。
 *
 * ★ 2026-09-21：原来这里还要登记一次跨 app 的让路请求
 *   （ai_companion_audio_yield(true)），那套协议已整套删除（见本节头上那段）。
 *   现在这里**一个跨 app 调用都没有**，只在本进程里置标志 + 起线程。
 */
static void alarm_sound_begin(void)
{
    pthread_attr_t attr;
    pthread_t      tid;
    bool           need_start = false;

    pthread_mutex_lock(&alarm_audio_lock);

    alarm_sound_active  = true;
    alarm_sound_pending = true;

    if (!alarm_worker_up) {
        alarm_worker_up = true;
        need_start = true;
    }

    pthread_mutex_unlock(&alarm_audio_lock);

    if (!need_start) {
        return;
    }

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, ALARM_WORKER_STACK_SIZE);

    if (pthread_create(&tid, &attr, alarm_sound_worker, NULL) != 0) {
        pthread_attr_destroy(&attr);

        /* 线程起不来：报警**绝不能因此变成一声不响** —— 就地出声 + 上报
         * （页面、上报都在，只是这一串动作压了一小下 LVGL 线程）。 */
        printf("[Alarm] 出声线程起不来（栈/资源不足），就地出声 + 上报\n");

        pthread_mutex_lock(&alarm_audio_lock);
        alarm_worker_up     = false;
        alarm_sound_pending = false;
        pthread_mutex_unlock(&alarm_audio_lock);

        alarm_sound_do();
        return;
    }

    pthread_attr_destroy(&attr);
}

/**
 * @brief  LVGL 线程侧：报警结束，出声线程可以收摊了（幂等）
 *
 * 出声线程看到 alarm_sound_active 变假就自己收摊（最多晚 1 秒那一拍）。
 * ★ 2026-09-21：原来这里还要把麦克风"还给 hello_app"
 *   （ai_companion_mic_reclaim()），那套协议已整套删除（见本节头上那段）。
 */
static void alarm_sound_end(void)
{
    pthread_mutex_lock(&alarm_audio_lock);
    alarm_sound_active  = false;      /* 出声线程据此收摊 */
    alarm_sound_pending = false;
    pthread_mutex_unlock(&alarm_audio_lock);
}

/* ==================== 显示报警 ==================== */
void robot_ui_show_alarm(const char *content)
{
    /* ⓪' 报警一旦真的走起来，**另一条确认路**（hello_app 的语音追问：大字
     *     「检测到声音」那一套）就该停下：它是同一件事的另一半，两边都在问的时候
     *     报警声和它的 TTS 会抢同一台半双工音频设备，追问问完还会再问第二轮，
     *     屏幕上就是两套确认在打架。
     *     这里只登记一次收摊请求（非阻塞、不碰设备、不碰界面），真正的收尾在
     *     hello_app 主循环的 ask_flow_tick() 里（见 ai_companion_req.c）。
     *     放在这里是因为本函数是所有报警来源唯一的汇合点（报警按钮 / MQTT 的
     *     start_alarm / 声音检测回调 / ui_post_alarm），改一处就全覆盖。 */
    ai_companion_ask_abort();

    /* ⓪ 报警页要盖在最上面（全屏报警 + 响铃，用户必须看得见）。
     *
     * 询问页是建在**顶层**（lv_layer_top）的模态覆盖层，比任何屏幕都高：
     * 这时候还留着它，就会把正在响铃的报警页挡在下面 —— 那是不能接受的。
     * 所以让它位：页面撤下，并按"作废"回调给 main.c（-1：既不算确认，也不能
     * 当成"用户否认"，否则同一原因 60 秒内问不出来）。
     * 这里不需要先判断：robot_ui_ask_alarm_answer() 对"没有询问页"是空操作。 */
    if (ask_panel != NULL) {
        printf("[Ask] 报警页要上来，询问页让位（原因=%s）\n", ask_panel_reason);
        robot_ui_ask_alarm_answer(-1);
    }

    /* ① 先把红色报警页面切出来（**必须第一步**）。
     *
     * 这一步原来排在报警声和上报后面，实测踩了坑：上报走网络（MQTT + TLS 推送），
     * 一旦这条路上出问题/卡住，红色页面就永远切不出来 —— 现场看到的现象就是
     * "按了报警，声音在响但屏幕上没有报警页，也退不出来"。
     * 先刷页面，后面无论网络怎么慢，用户至少能看到报警界面并点"返回"。
     */
    lv_scr_load(scr_alarm);

    /* 设置报警表情 */
    robot_ui_set_face(ROBOT_FACE_ALARM);
    robot_ui_set_status(ROBOT_STATUS_ALARM);

    /* 启动报警闪烁：先复位成满不透明，再让 2Hz 定时器跑起来 */
    if (lbl_alarm_icon != NULL)
    {
        lv_obj_set_style_opa(lbl_alarm_icon, LV_OPA_COVER, 0);
    }
    if (alarm_blink_timer != NULL)
    {
        lv_timer_resume(alarm_blink_timer);
    }

    /* ② 设备级动作：让喇叭真的响起来（板级报警模块）+ 上报。
     *
     * 放在这个函数里、而不是各个调用点，是因为界面上的"报警"按钮走的是
     *   btn_event_handler() -> robot_ui_show_alarm()
     * 根本不经过 main.c；而这里是所有报警入口（按钮 / MQTT 的 start_alarm /
     * 声音检测回调）唯一的汇合点，改一处就全接上了。也不会重复触发：
     * alarm_trigger() 对同级或更低的重复触发只更新 reason/text，不重来。
     *
     * 这里只做两件**非阻塞**的事：置"要出声"标记 + 起一条出声工作线程（在那条
     * 线程里：alarm_trigger + report_alarm_queued）。出声 + 上报（原来的 ②③）
     * 参数和顺序一个字都没改，见 alarm_sound_do()。
     *
     * ★ 2026-09-21：原来出声之前必须先请 hello_app 交出它常开麦占着的设备
     *   （ai_companion_audio_yield + 有界轮询 + close_alarm 时 reclaim），
     *   那套跨 app 的让路协议**整套删掉了**（用户拍板：「关闭让路…不需要做
     *   麦克风转让，因为我们的麦是常开的」）。现在报警**直接出声**，半双工
     *   交给设备那一层（放音时驱动会停掉录音通路并唤醒对方阻塞的 read）。
     *   于是这里再没有"等让路"那一段，也就不需要工作线程去等什么了 ——
     *   线程留着只为不把设备动作压在 LVGL 线程上。
     *
     * 这里**不受 main.c 里 g_ai_initialized / #if 0 的影响**：robot_ui.c
     * 完全不引用那个标志，所以 AI 初始化整块停用也照样出声。
     */
    alarm_sound_begin();
}

/* ==================== 关闭报警 ==================== */
void robot_ui_close_alarm(void)
{
    int ret;

    /* 设备级动作：停掉报警声（没有在报警时是安全空操作） */
    ret = alarm_clear();
    if (ret != OK) {
        printf("robot_ui: alarm_clear failed: %d\n", ret);
    }

    /* 报警结束：出声线程可以收摊了（它看到 alarm_sound_active 变假就自己退出，
     * 最多晚 1 秒那一拍）。
     * ★ 2026-09-21：原来这里还要把麦克风"还给 hello_app"
     *   （ai_companion_mic_reclaim()），那套让路协议整套删掉了，见本节头上那段。 */
    alarm_sound_end();

    /* 停止报警闪烁（并复位成满不透明，免得下次进报警页时停在半透明的状态） */
    if (alarm_blink_timer != NULL)
    {
        lv_timer_pause(alarm_blink_timer);
    }
    if (lbl_alarm_icon != NULL)
    {
        lv_obj_set_style_opa(lbl_alarm_icon, LV_OPA_COVER, 0);
    }

    /* 返回主界面 */
    lv_scr_load(scr_main);

    /* 恢复正常状态 */
    robot_ui_set_face(ROBOT_FACE_HAPPY);
    robot_ui_set_status(ROBOT_STATUS_IDLE);
}

/* ==================== 「询问是否报警」页（异常声响二次确认） ==================== */
/*
 * 用户拍板：**先询问，等用户二次确定后再报警**（不要一检测到就直接报警）。
 *
 * 这一页只是三条确认路里的**第一条** —— 板子的屏幕现在可能是坏的/被拆下来的，
 * 所以"有没有人点这一页"绝不能是报警的前提：
 *   ① 屏幕：本页两个按钮（走 robot_ui_ask_alarm_answer）
 *   ② 网络：MQTT 下行 {"action":"confirm_alarm","confirm":true|false}（main.c 分发）
 *   ③ 兜底：20 秒无人应答自动按「不用了」处理（main.c 的超时看门狗）
 * 三条都收口到 main.c 的 ask_finish()，这一页**不做任何报警动作**，只回答"用户选了
 * 哪个"（回调 robot_ask_result_cb_t）。这样"报警页 + 响铃 + MQTT + 手机推送"仍然
 * 只有现有那一个入口（ui_post_alarm / robot_ui_show_alarm），不会多出第二条报警路。
 *
 * 建在**顶层**（lv_layer_top()）而不是某个 scr_xxx：报警页、提醒、语音面板都会
 * lv_scr_load 切屏；挂在层上就永远压在"当前那一屏"之上，撤下时也不用管下面现在是
 * 哪一屏（删掉它就露出下面的内容，不需要再切回去）。
 *
 * 样式沿用报警页（满屏红底 + 白字 + 大圆角按钮）：一眼能看出"这是个要你回答的
 * 安全提示"，不是普通弹窗。
 *
 * ⚠️ 只能在 LVGL 线程里调（它建/删控件）；别的线程一律走 main.c 的
 * ui_post_ask_alarm()（内部 ui_async_call 投递）。
 *
 * ⚠️ 为什么按钮回调里不直接调 robot_ui_ask_alarm_answer()，而要再投一次
 * lv_async_call：那个函数会 lv_obj_del() 掉**正在处理这个事件的按钮**所在的
 * 整棵对象树（本页是模态覆盖层，整页一起撤）。在对象自己的事件回调里删自己，
 * LVGL 事件链表正在遍历的节点就没了 —— 本工程对"跨线程碰 LVGL"这么小心，
 * 没理由在这里赌一把；投一拍再撤，页面下一帧消失，用户看不出来。
 */

#define ASK_ALARM_REASON_MAX  128

/* ask_panel / ask_panel_reason 两个量定义在文件开头（robot_ui_show_alarm 要用） */
static robot_ask_result_cb_t ask_result_cb = NULL;       /* 回答往哪送（main.c 登记） */
static void     *ask_result_arg = NULL;

bool robot_ui_ask_alarm_active(void)
{
    return (ask_panel != NULL);
}

void robot_ui_set_ask_result_cb(robot_ask_result_cb_t cb, void *arg)
{
    ask_result_cb  = cb;
    ask_result_arg = arg;
}

/* 回答的名字（日志里要能看出走的是哪条路，"作废"和"否认"是两回事） */
static const char *ask_answer_name(int confirmed)
{
    if (confirmed == 1) {
        return "是的，报警";
    }
    if (confirmed == 0) {
        return "不用了";
    }
    return "作废（报警页让位）";
}

/* 撤下询问页（幂等；不回调 —— 回调只有"回答"那一条路，见 robot_ui_ask_alarm_answer） */
void robot_ui_close_ask_alarm(void)
{
    if (ask_panel == NULL) {
        return;
    }

    lv_obj_del(ask_panel);
    ask_panel = NULL;
    ask_panel_reason[0] = '\0';

    printf("[Ask] 询问页已撤下\n");
}

/* 屏幕按钮的回答：**不直接**处理，先投一拍再处理
 * （理由见本节头上那段"为什么按钮回调里不直接调 robot_ui_ask_alarm_answer"）。
 * user_data 就是"确认(1)还是不用(0)"。 */
typedef struct {
    int confirmed;
} ask_btn_msg_t;

static void ask_btn_apply(void *arg)
{
    ask_btn_msg_t *m = (ask_btn_msg_t *)arg;
    int confirmed = m->confirmed;

    free(m);

    robot_ui_ask_alarm_answer(confirmed);
}

static void ask_btn_handler(lv_event_t *e)
{
    ask_btn_msg_t *m;

    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    m = malloc(sizeof(ask_btn_msg_t));
    if (m == NULL) {
        printf("[Ask] 内存不够，这次的按钮回答丢了（MQTT / 超时那两条路仍然有效）\n");
        return;
    }

    m->confirmed = (int)(intptr_t)lv_event_get_user_data(e);

    touch_ui_play_sound("click");
    printf("[Ask] 屏幕按钮：%s\n", m->confirmed ? "是的，报警" : "不用了");

    if (ui_async_call(ask_btn_apply, m) != LV_RESULT_OK) {
        free(m);
        printf("[Ask] 回答投递失败（内存紧），这一页只能靠 MQTT / 超时收尾\n");
    }
}

int robot_ui_ask_alarm_answer(int confirmed)
{
    char reason[ASK_ALARM_REASON_MAX];
    robot_ask_result_cb_t cb;
    void *cb_arg;

    if (ask_panel == NULL) {
        /* 已经回答过 / 已经超时 / 本来就没弹起来：都不能变成第二次报警 */
        printf("[Ask] 现在没有询问页，这次的回答（%s）忽略\n",
               ask_answer_name(confirmed));
        return -1;
    }

    /* 先把这一条整个摘下来，再回调：回调里可能**紧接着又发起**一次新的询问
     * （ask_finish 之后下一秒又检测到一个异常），那时候 ask_panel 必须是空的。 */
    strncpy(reason, ask_panel_reason, sizeof(reason) - 1);
    reason[sizeof(reason) - 1] = '\0';
    cb     = ask_result_cb;
    cb_arg = ask_result_arg;

    robot_ui_close_ask_alarm();

    if (cb == NULL) {
        printf("[Ask] 没有登记回答回调，这一页只撤下、不做动作\n");
        return -1;
    }

    cb(confirmed, reason, cb_arg);
    return 0;
}

void robot_ui_show_ask_alarm(const char *reason)
{
    char clean[ASK_ALARM_REASON_MAX];
    char hint[96];
    lv_obj_t *title;
    lv_obj_t *detail;
    lv_obj_t *hint_lbl;
    lv_obj_t *row;
    lv_obj_t *btn_yes;
    lv_obj_t *btn_no;
    lv_obj_t *lbl;

    if (reason == NULL || reason[0] == '\0') {
        reason = "异常声响";
    }

    if (ask_panel != NULL) {
        /* 同一时间只允许一个 pending 询问（main.c 那边也有同样一道闸门，
         * 这里再拦一次是防"直接调本函数"的调用点） */
        printf("[Ask] 页面上已经有一条询问（原因=%s），本次 %s 不重建\n",
               ask_panel_reason, reason);
        return;
    }

    /* reason 是外面传进来的任意文本（中文备注、ASR 片段、可能夹 emoji）：显示前
     * 按字库清洗一遍，免得到屏幕上是一排方块 —— 和对话区用的是**同一份**规则
     * （robot_ui_bridge_sanitize_text 包的就是 main.c 的 sanitize_for_display）。 */
    if (robot_ui_bridge_sanitize_text(reason, clean, sizeof(clean)) == 0) {
        snprintf(clean, sizeof(clean), "%s", "异常声响");
    }

    snprintf(ask_panel_reason, sizeof(ask_panel_reason), "%s", clean);

    ask_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ask_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ask_panel, lv_color_hex(0xF44336), 0);
    lv_obj_set_style_bg_opa(ask_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ask_panel, 0, 0);
    lv_obj_set_style_radius(ask_panel, 0, 0);
    lv_obj_set_style_pad_all(ask_panel, 16, 0);
    lv_obj_remove_flag(ask_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ask_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ask_panel, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(ask_panel, 14, 0);

    /* 大字标题 */
    title = lv_label_create(ask_panel);
    lv_label_set_text(title, "检测到异常声响");
    lv_obj_set_style_text_font(title, &lv_font_ui_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    /* 小字原因（清洗过的那份） */
    detail = lv_label_create(ask_panel);
    lv_label_set_text(detail, ask_panel_reason);
    lv_obj_set_style_text_font(detail, &lv_font_ui_20, 0);
    lv_obj_set_style_text_color(detail, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(detail, LV_PCT(90));
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);

    /* 时限说明：把 20 秒明写在屏幕上（数的来源是 robot_ui.h 的宏，改一处就够），
     * 顺带告诉现场"手机上也能确认"——屏幕可能已经坏了，这句话得让评审看见。 */
    snprintf(hint, sizeof(hint), "%u 秒内请回答，否则按「不用了」处理\n手机上也可以确认",
             (unsigned)(ROBOT_ASK_ALARM_TIMEOUT_MS / 1000));
    hint_lbl = lv_label_create(ask_panel);
    lv_label_set_text(hint_lbl, hint);
    lv_obj_set_style_text_font(hint_lbl, &lv_font_ui_16, 0);
    lv_obj_set_style_text_color(hint_lbl, lv_color_hex(0xFFEBEE), 0);
    lv_obj_set_style_text_align(hint_lbl, LV_TEXT_ALIGN_CENTER, 0);

    /* 两个大按钮：左右并排，横着一排 150×64（老人好点；圆角屏左右各内缩 28，
     * 和主屏那排按钮同一个理由） */
    row = lv_obj_create(ask_panel);
    lv_obj_set_size(row, LV_PCT(100), 84);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_left(row, 28, 0);
    lv_obj_set_style_pad_right(row, 28, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 「是的，报警」：白底红字（唯一会真的报警的那一个，做得最醒目） */
    btn_yes = lv_btn_create(row);
    lv_obj_set_size(btn_yes, 150, 64);
    lv_obj_set_style_bg_color(btn_yes, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(btn_yes, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_yes, 32, 0);
    lv_obj_add_event_cb(btn_yes, ask_btn_handler, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lbl = lv_label_create(btn_yes);
    lv_label_set_text(lbl, "是的，报警");
    lv_obj_set_style_text_font(lbl, &lv_font_ui_24, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xF44336), 0);
    lv_obj_center(lbl);

    /* 「不用了」：半透明白底白字（视觉上比上面那个"轻"，避免误点） */
    btn_no = lv_btn_create(row);
    lv_obj_set_size(btn_no, 150, 64);
    lv_obj_set_style_bg_color(btn_no, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(btn_no, LV_OPA_30, 0);
    lv_obj_set_style_border_width(btn_no, 2, 0);
    lv_obj_set_style_border_color(btn_no, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_radius(btn_no, 32, 0);
    lv_obj_add_event_cb(btn_no, ask_btn_handler, LV_EVENT_CLICKED, (void *)(intptr_t)0);
    lbl = lv_label_create(btn_no);
    lv_label_set_text(lbl, "不用了");
    lv_obj_set_style_text_font(lbl, &lv_font_ui_24, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(lbl);

    printf("[Ask] 询问页已弹出：原因=%s（%u 秒无人应答按「不用了」处理；"
           "MQTT 也可回答）\n",
           ask_panel_reason, (unsigned)(ROBOT_ASK_ALARM_TIMEOUT_MS / 1000));
}

/* ==================== 切换界面 ==================== */
void robot_ui_switch_view(ui_view_t view)
{
    switch (view) {
        case UI_VIEW_MAIN:
            lv_scr_load(scr_main);
            break;
        case UI_VIEW_ALARM:
            lv_scr_load(scr_alarm);
            break;
        default:
            break;
    }
}

/* ==================== 时钟刷新（主循环调用） ==================== */
void robot_ui_update_time(void)
{
    ui_clock_refresh();
}
