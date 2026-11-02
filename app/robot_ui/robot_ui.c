/**
 * robot_ui.c - 智爱陪伴机器人界面实现
 * SF32LB52-DevKit-LCD LVGL 界面开发
 */

#include "robot_ui.h"
#include "touch_ui.h"
#include "ui_theme.h"         /* 清新绿色健康主题调色板：所有颜色从这里取 */
#include "network_comm.h"     /* report_alarm()：报警要上报 MQTT + 手机推送 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>           /* malloc/free：询问页按钮的回答要先投一拍再处理 */
#include <time.h>             /* time() / localtime_r()：状态栏时钟用 */
#include <stdbool.h>
#include <stdint.h>           /* intptr_t：按钮 user_data 里塞"确认/不用" */
#include <unistd.h>           /* usleep()：等 hello_app 交出麦克风时的轮询/续租间隔 */
#include <pthread.h>          /* 报警出声线程（报警声不能做在 LVGL 线程里，见下面那节） */
#include <semaphore.h>        /* sem_t / SEM_INITIALIZER */
#include <nuttx/semaphore.h>  /* nxsem_wait_uninterruptible / nxsem_post */

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
#include "voice_emerg.h"

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
 * 状态」。所以它现在**是会显示的**，由 create_reply_caption() 建出来、
 * robot_ui_set_status() 写文字和颜色。
 *
 * lbl_net：仍然不在任何地方创建，恒为 NULL（robot_ui_set_net_status() 内部判 NULL
 * 后直接返回）。保留声明是为了不动 main.c 里那些调用点。 */
static lv_obj_t *lbl_status = NULL;    // 语音状态小标签（建在 AI 回复区里）

/* 时钟标签注册表：主屏状态栏 + 各全屏子页面页眉的时间都走这里，一个 10s 定时器
 * 统一刷全部槽。槽位靠 LV_EVENT_DELETE 自动反注册（面板整删时标签随之销毁，
 * 不留悬垂指针）。主屏状态栏是槽 0，页面页眉是后加的槽，满 6 个后新建的标签
 * 只写一次时间、不再跟刷（板上同时开着的页面数远到不了这个上限）。 */
#define UI_CLOCK_MAX 6
static lv_obj_t *clock_slots[UI_CLOCK_MAX] = {NULL};
static lv_obj_t *lbl_net = NULL;       // 网络状态标签（已不再创建）
static lv_obj_t *lbl_ai_reply = NULL;  // AI 回复小字（对话框去掉后剩下的那行字幕）

/* 按钮 */
static lv_obj_t *btn_menu = NULL;      // 菜单按钮（小方形，2026-10-29 起）
static lv_obj_t *btn_alarm = NULL;     // 报警按钮（大圆形，红）

/* 报警页的 "!!!" 图标：闪烁只动它一个（原来动的是整屏，见 alarm_blink_timer_cb） */
static lv_obj_t *lbl_alarm_icon = NULL;

/* 报警页那句详情（"检测到异常/已通知家人"）。必须是文件级 static：
 * 文案在 robot_ui_show_alarm() 里按「手机推送到底能不能发出去」改写
 * （push_is_enabled()），原来它是 build 页面时写死的局部变量，事后改不了 ——
 * 于是推送密钥没配上时屏幕照样写着"已通知家人"，老人以为家人知道了。 */
static lv_obj_t *lbl_alarm_detail = NULL;

/* ==================== 会呼吸的笑脸（LVGL 画的，2026-10-29） ====================
 * 用户：「把主页面的对话框框去掉，换成大版的动画图案，ai说话的时候它也在动」
 * + 拍板「LVGL 画的会呼吸的笑脸」。所以脸不是图片素材（本工程一张图都没有），
 * 是几个 lv_obj 拼出来的圆 + 圆角条，尺寸/颜色全在这里改。
 *
 * 结构（从后往前画）：
 *   face_ring2 / face_ring1   两圈描边圆 —— 说话时当"声波涟漪"用（闪）
 *   face_core                 脸本体容器，整体上下浮动 = 呼吸
 *     face_disc               黄色圆脸盘（不透明，脸的底色）
 *     eye_l / eye_r           眼睛（圆角条）
 *     mouth                   嘴（圆角条，说话时一开一合）
 *
 * ★ 动画用 lv_timer 逐步改，**不用 lv_anim**：这里每帧要同时改好几件东西
 *   （嘴高、环的透明度、整脸的 y），lv_anim 一个动画只能驱动一个量，得开
 *   三四条并行动画互相抢；timer 一次 tick 全改完，也好按「说话/不说话」
 *   随时换节奏。代价是要自己维护相位 face_tick。
 * ★ 全屏二级页面盖上来时必须整个停（robot_ui_set_face_anim_paused）：那块
 *   重绘是白做的，而面板本来就慢 —— 这是老账（见那个函数的注释）。 */
static lv_obj_t *face_ring2 = NULL;    /* 最外圈（涟漪外圈） */
static lv_obj_t *face_ring1 = NULL;    /* 内圈（涟漪内圈） */
static lv_obj_t *face_core = NULL;     /* 脸本体容器（呼吸时整体上下挪） */
static lv_obj_t *face_disc = NULL;     /* 黄色圆脸盘 */
static lv_obj_t *eye_l = NULL;
static lv_obj_t *eye_r = NULL;
static lv_obj_t *mouth = NULL;
static lv_timer_t *face_timer = NULL;  /* 动画主定时器，100ms 一跳 */
static uint32_t face_tick = 0;         /* 相位（自由跑，取模用） */
static bool face_paused = false;       /* 被全屏页面盖住了 = 停 */
static int32_t face_core_base_y = 0;   /* 脸的中位 y，呼吸在这上下各 5px */
static bool face_was_speaking = false; /* 上一跳还在说？收尾时要归位嘴和涟漪 */

/* 嘴的"这个表情该有的样子"：robot_ui_set_face() 换表情时改这四个，
 * face_mouth_rest() 照着摆。说话动画期间嘴是被 timer 抢走的。 */
static int32_t face_mouth_w = 60;
static int32_t face_mouth_h = 14;
static int32_t face_mouth_r = 7;
static int32_t face_mouth_dy = 20;

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

/* ==================== 内部函数声明 ==================== */
static void init_styles(void);
static void create_main_screen(void);
static void create_alarm_screen(void);
static void create_status_bar(lv_obj_t *parent);
static void create_face_area(lv_obj_t *parent);
static void create_reply_caption(lv_obj_t *parent);
static void create_bottom_buttons(lv_obj_t *parent);
static void btn_event_handler(lv_event_t *e);
static void alarm_blink_timer_cb(lv_timer_t *t);

/* ==================== 初始化样式 ==================== */
static void init_styles(void)
{
    /* 背景样式 - 浅薄荷绿页面底（清新健康风） */
    lv_style_init(&style_bg);
    lv_style_set_bg_color(&style_bg, THEME_BG_PAGE);
    lv_style_set_bg_opa(&style_bg, LV_OPA_COVER);
    lv_style_set_border_width(&style_bg, 0);
    lv_style_set_radius(&style_bg, 0);

    /* 按钮样式 - 绿色 */
    lv_style_init(&style_btn);
    lv_style_set_bg_color(&style_btn, THEME_GREEN);
    lv_style_set_bg_opa(&style_btn, LV_OPA_COVER);
    lv_style_set_radius(&style_btn, 18);
    lv_style_set_shadow_width(&style_btn, 8);
    lv_style_set_shadow_color(&style_btn, THEME_GREEN_DARK);

    /* 报警按钮样式 - 红色 */
    lv_style_init(&style_btn_alarm);
    lv_style_set_bg_color(&style_btn_alarm, THEME_RED);
    lv_style_set_bg_opa(&style_btn_alarm, LV_OPA_COVER);
    lv_style_set_radius(&style_btn_alarm, 18);
    lv_style_set_shadow_width(&style_btn_alarm, 8);
    lv_style_set_shadow_color(&style_btn_alarm, THEME_RED_DARK);

    /* 文字样式 - 深绿灰（浅底上的主文字色） */
    lv_style_init(&style_text);
    lv_style_set_text_color(&style_text, THEME_TEXT_PRIMARY);
    lv_style_set_text_font(&style_text, &lv_font_ui_16);
}

/* ==================== 创建主屏幕 ==================== */
static void create_main_screen(void)
{
    /* 创建主屏幕 */
    scr_main = lv_obj_create(NULL);
    lv_obj_add_style(scr_main, &style_bg, 0);

    /* 创建状态栏 */
    create_status_bar(scr_main);

    /* 创建会呼吸的笑脸（主页面主角） */
    create_face_area(scr_main);

    /* 创建 AI 回复字幕（对话框框 2026-10-29 去掉，只剩这一行字） */
    create_reply_caption(scr_main);

    /* 创建底部按钮（小方形菜单 + 大圆形紧急呼叫） */
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

/* 格式化当前时间到 buf（"--:--" = RTC 没设过，不可信）。 */
static void ui_clock_format(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm tm_buf;

    /* time() <= 0：说明时钟还没被设过（板上没有 RTC 备份电池，
     * 冷启动会读到 1900/2000 这类值）—— 用 "--:--" 明确表示"时间不可信"，
     * 比继续显示一个假的时间好。 */
    if (now <= 0) {
        snprintf(buf, len, "--:--");
        return;
    }

    /* UTC -> 本地时间（跨天由加法自然进位） */
    if (UI_TZ_OFFSET_SEC != 0) {
        now += UI_TZ_OFFSET_SEC;
    }

    if (localtime_r(&now, &tm_buf) == NULL) {
        snprintf(buf, len, "--:--");
        return;
    }

    snprintf(buf, len, "%02d:%02d", tm_buf.tm_hour, tm_buf.tm_min);
}

/* 槽位反注册：标签被删（面板整删）时自动把槽清掉，防悬垂指针。 */
static void clock_slot_free_cb(lv_event_t *e)
{
    lv_obj_t *target = lv_event_get_target(e);
    for (int i = 0; i < UI_CLOCK_MAX; i++) {
        if (clock_slots[i] == target) {
            clock_slots[i] = NULL;
            return;
        }
    }
}

lv_obj_t *ui_clock_label_create(lv_obj_t *parent, lv_color_t color)
{
    char tbuf[8];

    lv_obj_t *lbl = lv_label_create(parent);
    ui_clock_format(tbuf, sizeof(tbuf));
    lv_label_set_text(lbl, tbuf);
    lv_obj_set_style_text_color(lbl, color, 0);
    /* 时钟统一 28 号（2026-10-14 用户要求"字体调大"）。时间只有 ASCII
     * 数字和冒号，montserrat_28 覆盖；自建 ui_* 字库最大 24 号。全板
     * 时钟（主屏栏/各页页眉/报警页/询问页）都走这里，一处生效。 */
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
    lv_obj_add_event_cb(lbl, clock_slot_free_cb, LV_EVENT_DELETE, NULL);

    for (int i = 0; i < UI_CLOCK_MAX; i++) {
        if (clock_slots[i] == NULL) {
            clock_slots[i] = lbl;
            return lbl;
        }
    }

    /* 槽满了：不注册（只显示创建时写上的这一次），打一行日志好排查。 */
    printf("[UI] 时钟槽已满(%d)，新标签不接入定时刷新\n", UI_CLOCK_MAX);
    return lbl;
}

/* 从系统时钟（硬件 RTC）读一次，刷新所有注册过的时钟标签（本地时间）。 */
static void ui_clock_refresh(void)
{
    char tbuf[8];

    ui_clock_format(tbuf, sizeof(tbuf));

    for (int i = 0; i < UI_CLOCK_MAX; i++) {
        /* 没变就不 set_text：lv_label_set_text 即使文本相同也会标脏等重绘，
         * 时钟每 10 秒打一次点、大多数分钟内文本没变，白做这些重绘纯属
         * 和滚动抢带宽（和表情动画是同一类问题）。 */
        if (clock_slots[i] != NULL &&
            strcmp(lv_label_get_text(clock_slots[i]), tbuf) != 0) {
            lv_label_set_text(clock_slots[i], tbuf);
        }
    }
}

static void ui_clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    ui_clock_refresh();
}

/* ==================== 全屏子页面页眉 ==================== */
/* 76px 全宽：左返回（back_cb==NULL 不建 = 本页必须作答）+ 顶部居中大时钟 +
 * 时钟下面一行小标题。
 * 2026-10-14 用户反馈"时间出框了，把时间放在顶部中间"：时钟原来是右上角
 * 右对齐，屏幕圆角半径大，右上角随时钟字高就可能被啃掉一角 —— 居中放在
 * 顶部天然躲开两个上圆角，顺便升为页眉的视觉主角（28 号字）。标题退居
 * 二行（20 号灰字），不再和时钟抢地方。 */
lv_obj_t *ui_page_header_create(lv_obj_t *parent, const char *title,
                                lv_event_cb_t back_cb, void *user_data)
{
    lv_obj_t *header = lv_obj_create(parent);
    lv_obj_set_size(header, LV_PCT(100), 76);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    /* 左右 40：这块屏的物理圆角比预估大（主屏菜单按钮 28 内缩都被切过），
     * 返回按钮也往里收 40px 才不会被圆角啃掉一角。 */
    lv_obj_set_style_pad_left(header, 40, 0);
    lv_obj_set_style_pad_right(header, 40, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    /* 不用 flex：三块各自对齐 */
    lv_obj_set_style_pad_all(header, 0, 0);

    if (back_cb != NULL) {
        /* 48×48：46 太小难摁 → 56 又嫌大挡着页眉其它元素（用户两头都提过），
         * 折中 48，圆角 24。圆角安全性：按钮左缘 x=40，即使屏幕圆角到 r75，
         * 顶角侵入 ≈39px 也仍在按钮内缘之内。 */
        lv_obj_t *btn = lv_btn_create(header);
        lv_obj_set_size(btn, 48, 48);
        lv_obj_align(btn, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_bg_color(btn, THEME_GREEN_TINT, 0);
        lv_obj_set_style_radius(btn, 24, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, back_cb, LV_EVENT_CLICKED, user_data);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, "<");
        lv_obj_set_style_text_color(lbl, THEME_GREEN_DARK, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_ui_24, 0);
        lv_obj_center(lbl);
    }

    /* 时钟：顶部居中、28 号（字体在 ui_clock_label_create 里统一给） */
    lv_obj_t *lbl_time = ui_clock_label_create(header, THEME_TEXT_PRIMARY);
    lv_obj_align(lbl_time, LV_ALIGN_TOP_MID, 0, 4);

    /* 标题：时钟下面一行，20 号灰字 */
    lv_obj_t *lbl_title = lv_label_create(header);
    lv_label_set_text(lbl_title, (title != NULL) ? title : "");
    lv_obj_set_style_text_color(lbl_title, THEME_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(lbl_title, &lv_font_ui_20, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 46);

    return header;
}

/* ==================== 创建状态栏 ==================== */
/* 「菜单」按钮：主菜单的保底入口（2026-10-14 起在底部按钮排，不再挂顶栏）。
 * 主菜单只在开机时显示一次，关掉之后要么右滑（手势，见 touch_ui.c），
 * 要么点这里。两者都调 touch_ui_show_menu(MENU_TYPE_MAIN)。
 * 按键音和底部同排的提醒/报警保持一致。 */
static void menu_button_event_handler(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        touch_ui_play_sound("click");
        touch_ui_show_menu(MENU_TYPE_MAIN);
    }
}

static void create_status_bar(lv_obj_t *parent)
{
    /* 状态栏容器。**现在顶上只有时钟一样。**
     *
     * 演变史：最早有「[在线]/WiFi/NET」三样假状态（2026-09-14 删）→ 后来是
     * 时钟 + 「菜单」按钮 → 2026-10-14 用户拍板"顶部只有时间"：菜单按钮
     * 挪到底部按钮排里顶替设置（设置仍可从主菜单进），顶栏回归纯净。
     *
     * 注意：`robot_ui_set_status()` / `robot_ui_set_net_status()` 两个 setter
     * 都**保留**了（内部都有 NULL 检查），main.c 里那十几处调用点一行都不用改：
     *   - set_net_status()：对应标签仍然不创建，恒为 NULL，没有任何显示效果；
     *   - set_status()：写的是主界面字幕正下方那一行语音状态小字
     *     （见 create_reply_caption() 和 robot_ui_set_status()），与顶栏无关。 */
    lv_obj_t *bar = lv_obj_create(parent);
    /* 高度 40 → **64**：菜单按钮要做到 52 高才够好点（40 的栏里塞不下），
     * 顺便让整块状态栏不那么挤。 */
    lv_obj_set_size(bar, LV_PCT(100), 64);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, THEME_STATUSBAR_BG, 0);
    /* 白底状态栏：只留 1px 底部描边把栏和页面底分开 */
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, THEME_STATUSBAR_LINE, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
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
    /* 时间标签（居中）：走 ui_clock_label_create 注册进统一定时刷新槽
     * （创建时已立即写上当前时间，浅色栏用深绿灰字）。 */
    lv_obj_t *lbl_time = ui_clock_label_create(bar, THEME_TEXT_PRIMARY);
    lv_obj_align(lbl_time, LV_ALIGN_CENTER, 0, 0);           /* 时间居中 */
    clock_timer = lv_timer_create(ui_clock_timer_cb, 10000, NULL);  /* 每 10 秒刷一次 */

    /* 原来这里右上角还有一颗「菜单」按钮（右对齐内缩 40 躲圆角）。
     * 2026-10-14 用户拍板"顶部只有时间"：它挪到底部按钮排里顶替「设置」
     * 的位置（见 create_bottom_buttons），顶栏只剩时钟，右上圆角区彻底空出来。 */
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
                                    ? THEME_GREEN_DARK
                                    : THEME_ST_THINKING,
                                0);
}

/* ==================== 创建表情区域（会呼吸的笑脸） ==================== */

/* 呼吸相位表：24 跳 × 100ms = 2.4 秒一次呼吸，整张脸上下共 10px。
 * 查表不用三角函数 —— 板上没必要为这点动画引浮点。 */
static const int8_t s_breath_y[24] = {
     0,  1,  2,  3,  4,  5,  5,  5,  4,  3,  2,  1,
     0, -1, -2, -3, -4, -5, -5, -5, -4, -3, -2, -1,
};

/* 说话时嘴的开合高度：8 跳 × 100ms = 0.8 秒一张一合。
 * 最大 42 是按脸盘 152 算的 —— 再大嘴就顶到下巴了。 */
static const int8_t s_mouth_h[8] = { 14, 24, 34, 42, 38, 28, 20, 14 };

/* 说话时两圈涟漪的透明度，两圈错开 4 跳 = 看着像波从里往外推。 */
static const uint8_t s_ripple_opa[8] = { 255, 200, 150, 110, 90, 110, 150, 200 };

/* 把嘴摆回"这个表情该有的样子"（robot_ui_set_face 换表情后调一次，
 * 说话动画收尾也调一次 —— 否则嘴会停在张开的那一格）。 */
static void face_mouth_rest(void)
{
    if (mouth == NULL) {
        return;
    }
    lv_obj_set_size(mouth, face_mouth_w, face_mouth_h);
    lv_obj_set_style_radius(mouth, face_mouth_r, 0);
    lv_obj_align(mouth, LV_ALIGN_CENTER, 0, face_mouth_dy);
}

/* 动画主定时器。100ms 一跳，一次跳把该变的都变了（见文件头部那段说明）。 */
static void face_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (face_paused || face_core == NULL) {
        return;
    }

    face_tick++;

    if (current_status == ROBOT_STATUS_SPEAKING) {
        uint32_t s = face_tick % 8;
        int32_t h = s_mouth_h[s];

        /* 嘴一张一合：高变了就跟着把圆角改成 h/2，一直保持椭圆/胶囊形 */
        if (mouth != NULL && lv_obj_get_height(mouth) != h) {
            lv_obj_set_height(mouth, h);
            lv_obj_set_style_radius(mouth, h / 2, 0);
            lv_obj_align(mouth, LV_ALIGN_CENTER, 0, face_mouth_dy);
        }

        if (face_ring1 != NULL) {
            lv_obj_set_style_border_opa(face_ring1, s_ripple_opa[s], 0);
        }
        if (face_ring2 != NULL) {
            lv_obj_set_style_border_opa(face_ring2, s_ripple_opa[(s + 4) % 8], 0);
        }

        /* 说话时**不**上下飘：嘴已经在动，再飘会晃眼。
         * 停在呼吸的中位（base_y），别停在半空。 */
        if (lv_obj_get_y(face_core) != face_core_base_y) {
            lv_obj_set_y(face_core, face_core_base_y);
        }
        face_was_speaking = true;
        return;
    }

    /* 不说话：整张脸慢慢呼吸（上下 10px / 2.4 秒） */
    int32_t y = face_core_base_y + s_breath_y[face_tick % 24];
    if (lv_obj_get_y(face_core) != y) {
        lv_obj_set_y(face_core, y);
    }

    /* 刚从"在说"退回来：把嘴和涟漪一次性归位，之后就不再碰它们
     * （每跳都写同一次值会让 LVGL 白白标脏重绘） */
    if (face_was_speaking) {
        face_mouth_rest();
        if (face_ring1 != NULL) {
            lv_obj_set_style_border_opa(face_ring1, 150, 0);
        }
        if (face_ring2 != NULL) {
            lv_obj_set_style_border_opa(face_ring2, 210, 0);
        }
        face_was_speaking = false;
    }
}

static void create_face_area(lv_obj_t *parent)
{
    /* 脸区：390×206，贴着状态栏下面。
     * ⚠️ 状态栏是 **64px** 高（create_status_bar 里写死的），不是更早那版注释
     * 说的 42 —— 竖向预算按 64 重排过：状态栏 64 + 脸 206 + 字幕两行 52 +
     * 状态小字 22 + 按钮排 96 ≈ 450，一屏刚好塞下，谁也不挤谁。 */
    lv_obj_t *area = lv_obj_create(parent);
    lv_obj_set_size(area, LV_PCT(100), 206);
    lv_obj_align(area, LV_ALIGN_TOP_MID, 0, 66);
    lv_obj_set_style_bg_opa(area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(area, 0, 0);
    lv_obj_set_style_pad_all(area, 0, 0);
    lv_obj_remove_flag(area, LV_OBJ_FLAG_SCROLLABLE);

    /* 两圈涟漪（说话时闪，平时当脸的光晕） */
    face_ring2 = lv_obj_create(area);
    lv_obj_set_size(face_ring2, 200, 200);
    lv_obj_align(face_ring2, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(face_ring2, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(face_ring2, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(face_ring2, 3, 0);
    lv_obj_set_style_border_color(face_ring2, THEME_GREEN_BORDER, 0);
    lv_obj_set_style_border_opa(face_ring2, 210, 0);
    lv_obj_remove_flag(face_ring2, LV_OBJ_FLAG_SCROLLABLE);

    face_ring1 = lv_obj_create(area);
    lv_obj_set_size(face_ring1, 178, 178);
    lv_obj_align(face_ring1, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(face_ring1, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(face_ring1, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(face_ring1, 3, 0);
    lv_obj_set_style_border_color(face_ring1, THEME_GREEN, 0);
    lv_obj_set_style_border_opa(face_ring1, 150, 0);
    lv_obj_remove_flag(face_ring1, LV_OBJ_FLAG_SCROLLABLE);

    /* 脸本体容器：眼睛/嘴是它的孩子，整体上下飘 = 呼吸 */
    face_core = lv_obj_create(area);
    lv_obj_set_size(face_core, 152, 152);
    lv_obj_align(face_core, LV_ALIGN_CENTER, 0, 0);
    face_core_base_y = lv_obj_get_y(face_core);   /* 记下中位，动画在这上下各 5px */
    lv_obj_set_style_bg_opa(face_core, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(face_core, 0, 0);
    lv_obj_set_style_pad_all(face_core, 0, 0);
    lv_obj_remove_flag(face_core, LV_OBJ_FLAG_SCROLLABLE);

    /* 黄色圆脸盘（不透明，压在眼睛/嘴下面） */
    face_disc = lv_obj_create(face_core);
    lv_obj_set_size(face_disc, 152, 152);
    lv_obj_set_pos(face_disc, 0, 0);
    lv_obj_set_style_bg_color(face_disc, THEME_GREEN, 0);
    lv_obj_set_style_bg_opa(face_disc, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(face_disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(face_disc, 0, 0);
    lv_obj_remove_flag(face_disc, LV_OBJ_FLAG_SCROLLABLE);

    /* 眼睛：两根圆角条。先建的 disc 在下面，后建的眼睛在上面。 */
    eye_l = lv_obj_create(face_core);
    lv_obj_set_size(eye_l, 22, 28);
    lv_obj_align(eye_l, LV_ALIGN_CENTER, -26, -22);
    lv_obj_set_style_bg_color(eye_l, THEME_TEXT_PRIMARY, 0);
    lv_obj_set_style_bg_opa(eye_l, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(eye_l, 11, 0);
    lv_obj_set_style_border_width(eye_l, 0, 0);
    lv_obj_remove_flag(eye_l, LV_OBJ_FLAG_SCROLLABLE);

    eye_r = lv_obj_create(face_core);
    lv_obj_set_size(eye_r, 22, 28);
    lv_obj_align(eye_r, LV_ALIGN_CENTER, 26, -22);
    lv_obj_set_style_bg_color(eye_r, THEME_TEXT_PRIMARY, 0);
    lv_obj_set_style_bg_opa(eye_r, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(eye_r, 11, 0);
    lv_obj_set_style_border_width(eye_r, 0, 0);
    lv_obj_remove_flag(eye_r, LV_OBJ_FLAG_SCROLLABLE);

    /* 嘴：圆角条，宽高和圆角由表情/说话动画改 */
    mouth = lv_obj_create(face_core);
    lv_obj_set_size(mouth, face_mouth_w, face_mouth_h);
    lv_obj_align(mouth, LV_ALIGN_CENTER, 0, face_mouth_dy);
    lv_obj_set_style_bg_color(mouth, THEME_TEXT_PRIMARY, 0);
    lv_obj_set_style_bg_opa(mouth, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mouth, face_mouth_r, 0);
    lv_obj_set_style_border_width(mouth, 0, 0);
    lv_obj_remove_flag(mouth, LV_OBJ_FLAG_SCROLLABLE);

    /* 开机动画：100ms 一跳。全屏页面盖上来时由
     * robot_ui_set_face_anim_paused() 整个停掉。 */
    face_timer = lv_timer_create(face_timer_cb, 100, NULL);
    face_paused = false;
}

/* 全屏二级页面打开/关闭时暂停/恢复笑脸动画。
 *
 * 为什么必须暂停：这个动画每 100ms 都把脸那块区域标脏等重绘。页面全屏盖住
 * 主屏之后，那块重绘完全是白做的，而滚动页面时 LVGL 要把滚动的大块重绘和
 * 这块"冤枉"重绘拼在一起送显 —— 面板接口本来就慢，叠加出来就是用户看到的
 * "上下滑动卡顿"。现在是 lv_timer，直接 lv_timer_pause/resume，比当年删建
 * lv_anim 干净得多。
 * 只能在 LVGL 线程里调（touch_ui 的面板开关路径都在 LVGL 线程）。 */
void robot_ui_set_face_anim_paused(bool paused)
{
    face_paused = paused;

    if (face_timer != NULL) {
        if (paused) {
            lv_timer_pause(face_timer);
        } else {
            lv_timer_resume(face_timer);
        }
    }

    /* 停在中位，别停在呼吸的半空 */
    if (paused && face_core != NULL) {
        lv_obj_set_y(face_core, face_core_base_y);
    }
}

/* ==================== AI 回复字幕 + 语音状态小字 ==================== */

/* 语音状态小标签（lbl_status）的四档配色：听=蓝、想=橙、说=绿、空闲=灰。
 *
 * 和语音镜像面板（touch_ui.c 的 voice_state_color）**用的是同一套色值** ——
 * 老人在主界面和面板里看到的"蓝色=在听、橙色=在想、绿色=在说"是同一个意思，
 * 换个屏幕不用重新学。
 *
 * 写成宏是因为它有两处用：下面建标签时的初始颜色，和 robot_ui_set_status()
 * 里每一档的写入（两处必须一致，散着写迟早会漂）。 */
#define UI_STATUS_COLOR_IDLE      THEME_ST_IDLE        /* 灰：空闲，不抢眼 */
#define UI_STATUS_COLOR_LISTENING THEME_ST_LISTENING   /* 蓝：在听 */
#define UI_STATUS_COLOR_THINKING  THEME_ST_THINKING    /* 橙：在想 */
#define UI_STATUS_COLOR_SPEAKING  THEME_ST_SPEAKING    /* 绿：在说 */

static void create_reply_caption(lv_obj_t *parent)
{
    /* ★ 2026-10-29 用户点名「把主页面的对话框框去掉」—— 那张白色圆角卡连同
     * 里面的「智爱」署名一起拆了，主页面主角换成上面那个大笑脸。
     *
     * 但 AI 说的那句话不能凭空消失：老人要看它在说啥，家属调试也要看。
     * 所以留成**脸下面的一行字幕**，不带框、不带底色，靠页面奶油底衬着。
     * 这是本工程有意为之的一处偏差（用户原话只说换掉框，没说把字也删掉），
     * 不想要这一行随时可以再拆，删掉本函数体即可，别处不受影响。 */

    /* AI 回复字幕：最多两行，超出截断（LV_LABEL_LONG_DOT）——
     * 这一屏的主角是脸，长篇大论不该压在它下面挤掉按钮。 */
    lbl_ai_reply = lv_label_create(parent);
    lv_label_set_text(lbl_ai_reply, "你好！我是智爱\n有什么可以帮你的吗？");
    lv_obj_set_style_text_color(lbl_ai_reply, THEME_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(lbl_ai_reply, &lv_font_ui_24, 0);
    lv_label_set_long_mode(lbl_ai_reply, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_ai_reply, 350);
    lv_obj_align(lbl_ai_reply, LV_ALIGN_TOP_MID, 0, 276);
    lv_obj_set_style_text_align(lbl_ai_reply, LV_TEXT_ALIGN_CENTER, 0);

    /* 语音状态小标签：主界面上「空闲 / 在听… / 在想… / 在说…」那一行。
     *
     * 用户原话：「既然我们做了常态语音，不如在主页那个对话框加上状态」——
     * 不弹开语音面板，扫一眼主界面就知道它现在在干什么。对话框没了，
     * 这一行挪到字幕正下方，位置还在"主页面正中偏下"，老人照旧一眼看到。
     *
     * 谁写它：只有 robot_ui_set_status()（四档的文字和颜色都在那边）。
     * 这里只把控件建出来挂到 lbl_status 上，**不另开一条状态来源** ——
     * 语音状态从 hello_app 的 voice_state_probe() 出来，一路经
     * robot_ui_bridge_voice_state() / MQTT 的 voice_state 分支 → ui_post()
     * （内部 lv_async_call）投到 LVGL 线程，最后落到那一个 setter 上。
     *
     * 字号取 20（比字幕的 24 小一档）：它是状态提示，不该跟 AI 的正文抢
     * 注意力；又没有小到看不清 —— 这一屏是给老人用的，16 那一档本工程只用在
     * 辅助信息上。颜色由 setter 按状态给，初始是"空闲"的灰。 */
    lbl_status = lv_label_create(parent);
    lv_label_set_text(lbl_status, "空闲");
    lv_obj_set_style_text_color(lbl_status, UI_STATUS_COLOR_IDLE, 0);
    lv_obj_set_style_text_font(lbl_status, &lv_font_ui_20, 0);
    lv_obj_align(lbl_status, LV_ALIGN_TOP_MID, 0, 332);
}

/* ==================== 创建底部按钮 ==================== */
static void create_bottom_buttons(lv_obj_t *parent)
{
    /* 按钮容器。
     * ⚠️ 这块屏是圆角屏（390×450）：容器铺满 100% 宽的话，按钮会压在
     * **左下/右下圆角**上 —— 视觉被切一块，触摸也可能点不到。
     * 所以左右各内缩 40px，两颗按钮整体往里收。 */
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_set_size(container, LV_PCT(100), 96);
    lv_obj_align(container, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_left(container, 40, 0);
    lv_obj_set_style_pad_right(container, 40, 0);
    /* 底部内缩 8：给屏幕下圆角让出余量，按钮不再贴边。
     * 内容区 88 高，正好放下那颗 86 的圆。 */
    lv_obj_set_style_pad_bottom(container, 8, 0);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 菜单按钮：**小方形**（72×72，圆角 16 = 一眼看得出是"方"，但不硌手）。
     * 2026-10-29 用户拍板「大动画下面改成方形小的方形菜单和大的圆形紧急呼叫」。
     * 它只是个入口，不是安全功能，所以刻意比报警小一圈 —— 别跟报警抢眼睛。
     * 设置页/提醒页/语音聊天都在它开出的主菜单四宫格里，功能一个没丢。 */
    btn_menu = lv_btn_create(container);
    lv_obj_set_size(btn_menu, 72, 72);
    lv_obj_add_style(btn_menu, &style_btn, 0);
    lv_obj_set_style_radius(btn_menu, 16, 0);
    lv_obj_add_event_cb(btn_menu, menu_button_event_handler, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_btn2 = lv_label_create(btn_menu);
    lv_label_set_text(lbl_btn2, "菜单");
    lv_obj_set_style_text_font(lbl_btn2, &lv_font_ui_24, 0);
    /* 黄底上**不能用白字**（对比度不够，老人更看不清）—— 用深棕，
     * 和 ui_theme.h 里 THEME_TEXT_ON_GREEN 是同一套道理。 */
    lv_obj_set_style_text_color(lbl_btn2, THEME_TEXT_ON_GREEN, 0);
    lv_obj_center(lbl_btn2);

    /* 紧急呼叫按钮：**大圆形**（86×86），红 —— 全屏最大的控件。
     * 用户要求"报警按钮要清晰"，老人紧急时一眼找到、一按就中。
     * 红色是约定不是配色（ui_theme.h 里那段注释），换色调时**没动**。
     * 文案分两行（紧急 / 呼叫）：四个字一行在 86 的圆里放不下，硬塞会顶到圆边。 */
    btn_alarm = lv_btn_create(container);
    lv_obj_set_size(btn_alarm, 86, 86);
    lv_obj_add_style(btn_alarm, &style_btn_alarm, 0);
    lv_obj_set_style_radius(btn_alarm, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(btn_alarm, btn_event_handler, LV_EVENT_CLICKED, (void *)UI_VIEW_ALARM);
    lv_obj_t *lbl_btn3 = lv_label_create(btn_alarm);
    lv_label_set_text(lbl_btn3, "紧急\n呼叫");
    lv_obj_set_style_text_font(lbl_btn3, &lv_font_ui_24, 0);
    lv_obj_set_style_text_align(lbl_btn3, LV_TEXT_ALIGN_CENTER, 0);
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
                /* 报警界面 Back 按钮:关闭报警，返回主界面。
                 * ★ 2026-11-01：还要停 voice_emerg 的铃（原来只调
                 *   robot_ui_close_alarm 停板级警音，voice_emerg 的
                 *   响-停-听循环还在跑，铃声不断、页面却没了）。 */
                voice_emerg_cancel("手动停止");
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
    /* 创建报警屏幕（保持全红：报警页要一眼看出是"出事了"） */
    scr_alarm = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_alarm, THEME_RED, 0);
    lv_obj_set_style_bg_opa(scr_alarm, LV_OPA_COVER, 0);

    /* 报警页也是全屏页：顶部自带时钟（白字），不靠主屏状态栏 */
    lv_obj_t *lbl_time_alarm = ui_clock_label_create(scr_alarm, THEME_TEXT_ON_GREEN);
    lv_obj_align(lbl_time_alarm, LV_ALIGN_TOP_MID, 0, 12);

    /* 报警图标（闪烁只动它，不透明度的切换见 alarm_blink_timer_cb） */
    lbl_alarm_icon = lv_label_create(scr_alarm);
    lv_label_set_text(lbl_alarm_icon, "!!!");
    lv_obj_set_style_text_font(lbl_alarm_icon, &lv_font_ui_24, 0);
    lv_obj_set_style_text_color(lbl_alarm_icon, THEME_TEXT_ON_GREEN, 0);
    lv_obj_align(lbl_alarm_icon, LV_ALIGN_CENTER, 0, -60);

    /* 报警文字 */
    lv_obj_t *text = lv_label_create(scr_alarm);
    lv_label_set_text(text, "紧急");
    lv_obj_set_style_text_color(text, THEME_TEXT_ON_GREEN, 0);
    lv_obj_set_style_text_font(text, &lv_font_ui_24, 0);
    lv_obj_align(text, LV_ALIGN_CENTER, 0, 0);

    /* 报警详情（文案由 robot_ui_show_alarm() 按推送能不能发出去改写） */
    lbl_alarm_detail = lv_label_create(scr_alarm);
    lv_label_set_text(lbl_alarm_detail, "检测到异常\n已通知家人");
    lv_obj_set_style_text_color(lbl_alarm_detail, THEME_TEXT_ON_GREEN, 0);
    lv_obj_set_style_text_font(lbl_alarm_detail, &lv_font_ui_24, 0);
    lv_obj_align(lbl_alarm_detail, LV_ALIGN_CENTER, 0, 40);

    /* 返回按钮：加大到 180×64（用户要求报警相关按钮清晰好点） */
    lv_obj_t *btn_back = lv_btn_create(scr_alarm);
    lv_obj_set_size(btn_back, 180, 64);
    lv_obj_align(btn_back, LV_ALIGN_CENTER, 0, 100);
    lv_obj_set_style_bg_color(btn_back, THEME_BG_CARD, 0);
    lv_obj_set_style_radius(btn_back, 32, 0);
    lv_obj_add_event_cb(btn_back, btn_event_handler, LV_EVENT_CLICKED, (void *)UI_VIEW_MAIN);

    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "返回");
    lv_obj_set_style_text_color(lbl_back, THEME_RED, 0);
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
/* 表情不再是那行 ASCII 字，是**画出来的脸**：换表情 = 换脸盘颜色 + 眼睛形状
 * + 嘴形。嘴的"这个表情该有的样子"存在 face_mouth_w/h/r/dy 里，
 * face_mouth_rest() 照着摆；正在说话时嘴被动画抢走（face_timer_cb），
 * 说完那一跳会自动归位，不会停在张开的那一格。 */
void robot_ui_set_face(robot_face_t face)
{
    if (face >= ROBOT_FACE_MAX) return;

    current_face = face;
    if (face_disc == NULL) return;

    lv_color_t disc = THEME_GREEN;   /* 脸盘底色，默认就是那张黄脸 */
    int32_t eye_w = 22, eye_h = 28, eye_dx = 26, eye_dy = -22, eye_rad = 11;

    face_mouth_w = 60;
    face_mouth_h = 14;
    face_mouth_r = 7;
    face_mouth_dy = 20;

    switch (face) {
        case ROBOT_FACE_HAPPY:
            break;                                  /* 默认就是笑脸 */

        case ROBOT_FACE_THINKING:
            /* 眼睛往上看 + 嘴抿成一小团 */
            eye_dy = -28;
            face_mouth_w = 26;
            face_mouth_h = 26;
            face_mouth_r = 13;
            face_mouth_dy = 22;
            break;

        case ROBOT_FACE_SLEEPY:
            /* 眼睛眯成两条缝 + 嘴小小的 */
            disc = THEME_FACE_SLEEPY;
            eye_w = 26;
            eye_h = 7;
            eye_rad = 3;
            face_mouth_w = 30;
            face_mouth_h = 10;
            face_mouth_r = 5;
            break;

        case ROBOT_FACE_SURPRISED:
            /* 眼睛瞪圆 + 嘴张成椭圆 */
            eye_w = 26;
            eye_h = 32;
            eye_rad = 13;
            face_mouth_w = 30;
            face_mouth_h = 36;
            face_mouth_r = 15;
            face_mouth_dy = 24;
            break;

        case ROBOT_FACE_WORRIED:
            disc = THEME_ORANGE;
            eye_w = 22;
            eye_h = 22;
            eye_rad = 11;
            face_mouth_w = 40;
            face_mouth_h = 10;
            face_mouth_r = 5;
            face_mouth_dy = 24;
            break;

        case ROBOT_FACE_ALARM:
            /* 红脸 + 张大嘴。报警页整体也是红的，两边一致 ——
             * 红是"出事了"的约定，换色调时一个字节都没动。 */
            disc = THEME_FACE_ALARM;
            eye_w = 24;
            eye_h = 9;
            eye_rad = 4;
            face_mouth_w = 36;
            face_mouth_h = 32;
            face_mouth_r = 16;
            face_mouth_dy = 22;
            break;

        default:
            break;
    }

    lv_obj_set_style_bg_color(face_disc, disc, 0);

    lv_obj_set_size(eye_l, eye_w, eye_h);
    lv_obj_set_style_radius(eye_l, eye_rad, 0);
    lv_obj_align(eye_l, LV_ALIGN_CENTER, -eye_dx, eye_dy);

    lv_obj_set_size(eye_r, eye_w, eye_h);
    lv_obj_set_style_radius(eye_r, eye_rad, 0);
    lv_obj_align(eye_r, LV_ALIGN_CENTER, eye_dx, eye_dy);

    face_mouth_rest();
}

/* ==================== 设置 AI 回复 ==================== */
void robot_ui_set_ai_reply(const char *text)
{
    if (lbl_ai_reply && text) {
        lv_label_set_text(lbl_ai_reply, text);
    }
}

/* ==================== 设置状态 ==================== */
/* 写的是主界面字幕正下方那一行语音状态小字（create_reply_caption() 建的
 * lbl_status）。**状态栏上没有它** —— 状态栏那三样 2026-09-14 已被用户删掉。
 *
 * 四档和语音链路的四档一一对应（hello_app 的 voice_state_probe()）：
 *   空闲 IDLE       「空闲」  灰   不抢眼，它只是"没事，我在"
 *   在听 LISTENING  「在听…」 蓝
 *   在想 THINKING   「在想…」 橙   识别 / 等大模型这一段
 *   在说 SPEAKING   「在说…」 绿
 * 文案比语音镜像面板（touch_ui.c 的 voice_state_text：「检测到声音」「正在想…」
 * 「正在说话…」）**更短**：面板里那行是大字、有整行的地方，主界面这一行是字幕
 * 下面的小字，短一点才不会撑宽。颜色两边是同一套（见文件上面那几个
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
                lv_obj_set_style_text_color(lbl_status, THEME_ST_REMINDING, 0);
                break;
            case ROBOT_STATUS_ALARM:
                lv_label_set_text(lbl_status, "报警");
                lv_obj_set_style_text_color(lbl_status, THEME_RED, 0);
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
    lv_obj_set_style_bg_color(mbox, THEME_BG_CARD, 0);
    lv_obj_set_style_text_color(mbox, THEME_TEXT_PRIMARY, 0);

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
    lv_obj_set_style_bg_color(err_panel, THEME_RED_BG, 0);
    lv_obj_set_style_bg_opa(err_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(err_panel, 3, 0);
    lv_obj_set_style_border_color(err_panel, THEME_RED_BORDER, 0);
    lv_obj_set_style_radius(err_panel, 20, 0);
    lv_obj_set_flex_flow(err_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(err_panel, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(err_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(err_panel);
    lv_label_set_text(t, (title != NULL && title[0] != '\0') ? title : "出错了");
    lv_obj_set_style_text_font(t, &lv_font_ui_24, 0);
    lv_obj_set_style_text_color(t, THEME_RED_DARK, 0);

    lv_obj_t *c = lv_label_create(err_panel);
    lv_label_set_text(c, (content != NULL) ? content : "");
    lv_obj_set_style_text_font(c, &lv_font_ui_20, 0);
    lv_obj_set_style_text_color(c, THEME_TEXT_PRIMARY, 0);
    lv_obj_set_width(c, LV_PCT(95));
    lv_label_set_long_mode(c, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *btn = lv_btn_create(err_panel);
    lv_obj_set_size(btn, LV_PCT(70), 70);
    lv_obj_set_style_bg_color(btn, THEME_RED, 0);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_add_event_cb(btn, err_close_event_handler, LV_EVENT_CLICKED, NULL);

    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "知道了");
    lv_obj_set_style_text_color(bl, THEME_TEXT_ON_GREEN, 0);
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
/* ★ 2026-11-01：**不用 mutex，用二值信号量**（同 g_audio_in_lock 的教训）。
 *   这颗内核的 mutex 持有者跟踪不可靠：sem_post.c:63 断言（"解锁线程不是
 *   持锁者"）在门控锁、录音锁上都栽过。alarm_audio_lock 保护的只是几个标志
 *   的短临界区，不需要优先级继承；信号量没有持有者概念，post 永远不走断言。
 *   判据：报警页不再"弹出就卡"。 */
static sem_t alarm_audio_lock = SEM_INITIALIZER(1);
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
    } else {
        /* ★ 语音急救模块挂上来（2026-10-31）：补上"10 秒内说「没事」就能
         *   停铃"这件事。铃和推送上面这条路都已经做了，voice_emerg 探到
         *   板级报警在响会自己转**辅助模式**：不重复响铃、不重复推送，
         *   只盯「没事」好把铃掐掉 —— 老人误触这个大红按钮的退路，
         *   今天没有（铃会一直响到 60 秒自停）。
         *
         *   顺序是钉死的：**必须在 alarm_trigger 之后**，因为辅助模式的
         *   判据就是"触发这一刻板级报警在不在响"。 */
        voice_emerg_trigger("sos_button", "用户按下报警按钮");
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

        nxsem_wait_uninterruptible(&alarm_audio_lock);

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
            nxsem_post(&alarm_audio_lock);
            return NULL;
        }

        nxsem_post(&alarm_audio_lock);

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

    nxsem_wait_uninterruptible(&alarm_audio_lock);

    alarm_sound_active  = true;
    alarm_sound_pending = true;

    if (!alarm_worker_up) {
        alarm_worker_up = true;
        need_start = true;
    }

    nxsem_post(&alarm_audio_lock);

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

        nxsem_wait_uninterruptible(&alarm_audio_lock);
        alarm_worker_up     = false;
        alarm_sound_pending = false;
        nxsem_post(&alarm_audio_lock);

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
    /* ★ 2026-11-01：不持锁直接写标志 —— 单核上写 bool 是原子的，
     *   而这里从 LVGL 线程调（按钮回调），拿锁可能被工作线程短暂阻塞。
     *   worker 每 1 秒检查 alarm_sound_active，最多晚 1 秒收摊。 */
    alarm_sound_active  = false;      /* 出声线程据此收摊 */
    alarm_sound_pending = false;
}

/* ==================== 显示报警 ==================== */
void robot_ui_show_alarm(const char *content)
{
    /* ★ 2026-11-01：入口无条件打一行 —— 原来本函数只在 ask_panel 存在时才打日志，
     *   报警页弹出后 UI 卡死时串口里看不到任何触发记录，排查无从下手。 */
    printf("[robot_ui] show_alarm: %s\n", content != NULL ? content : "(null)");
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

    /* 报警详情那句按「手机推送到底发不发得出去」改写。
     * push_is_enabled() = push_init() 真拿到了 key（/etc/assets/push_key.txt
     * 在 ROMFS 里）—— 没拿到时整条 Bark 推送是**必然发不出去**的
     * （push_send_notification 会直接返回 -1），这时候还写"已通知家人"
     * 等于让老人以为家人已经在路上，是最坏的一种失败。 */
    if (lbl_alarm_detail != NULL)
    {
        lv_label_set_text(lbl_alarm_detail,
                          push_is_enabled() ? "检测到异常\n已通知家人"
                                            : "检测到异常\n请直接联系家人");
    }

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

/* ★ 统一报警路径（2026-10-31）：提示行要跟着状态改口
 *   「10 秒内说"没事"或点下面按钮取消」 -> 「已通知家人，30 秒后重发」。
 *   建页面时是局部量，改成文件级量才能事后改（页面活着期间它一直有效）。 */
static lv_obj_t *ask_panel_hint = NULL;

/* 改提示行（**只在 LVGL 线程里**调；别的线程走 main.c 的 ui_post_ask_hint） */
void robot_ui_ask_hint(const char *hint)
{
    if (ask_panel == NULL || ask_panel_hint == NULL || hint == NULL) {
        return;
    }

    lv_label_set_text(ask_panel_hint, hint);
    printf("[Ask] 提示行改为：%s\n", hint);
}

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
        return "我没事，取消";
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
    ask_panel_hint = NULL;          /* 控件已经删了，别留野指针 */
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
    /* ★ 2026-10-31：原来这里自己拼了一句「不用了」，而页面上那个按钮早就改叫
     * 「我没事，取消」了 —— 日志和屏幕对不上，排查时看着像点了别的东西。
     * 改成直接问 ask_answer_name()（唯一出处），按钮文案再改也不会漏。 */

    printf("[Ask] 屏幕按钮：%s\n", ask_answer_name(m->confirmed));

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
    lv_obj_set_style_bg_color(ask_panel, THEME_RED, 0);
    lv_obj_set_style_bg_opa(ask_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ask_panel, 0, 0);
    lv_obj_set_style_radius(ask_panel, 0, 0);
    lv_obj_set_style_pad_all(ask_panel, 16, 0);
    lv_obj_remove_flag(ask_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ask_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ask_panel, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(ask_panel, 14, 0);

    /* 询问页盖满全屏：顶部自带时钟（白字），不靠主屏状态栏。
     * FLOATING 让它脱离 column flex 流，不挤压下面的内容
     * （本版本 LVGL 没有 LV_POSITION_ABSOLUTE，FLOATING 是等效手段）。 */
    lv_obj_t *lbl_time_ask = ui_clock_label_create(ask_panel, THEME_TEXT_ON_GREEN);
    lv_obj_add_flag(lbl_time_ask, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(lbl_time_ask, LV_ALIGN_TOP_MID, 0, 12);

    /* 大字标题 ★ 2026-10-31 统一报警路径：这一页不再是"要不要报警"的询问，
     * 而是**报警正在进行**的确认页 —— 铃已经在响了，标题就得说实话。 */
    title = lv_label_create(ask_panel);
    lv_label_set_text(title, "正在报警");
    lv_obj_set_style_text_font(title, &lv_font_ui_24, 0);
    lv_obj_set_style_text_color(title, THEME_TEXT_ON_GREEN, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    /* 小字原因（清洗过的那份） */
    detail = lv_label_create(ask_panel);
    lv_label_set_text(detail, ask_panel_reason);
    lv_obj_set_style_text_font(detail, &lv_font_ui_20, 0);
    lv_obj_set_style_text_color(detail, THEME_TEXT_ON_GREEN, 0);
    lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(detail, LV_PCT(90));
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);

    /* ★ 时限说明（2026-10-31 换口径）：原来是"20 秒内请回答，否则按「不用了」
     * 处理" —— 那等于"没人答就算了、不通知家人"，和用户要的正好相反。
     * 现在明写：**10 秒内说「没事」或点下面按钮取消，否则通知家人**。
     * 10 这个数取自 robot_ui.h 的 ROBOT_ASK_ALARM_TIMEOUT_MS，和 voice_emerg
     * 真正发 Bark 的时刻是同一个数（改一处就够）。 */
    snprintf(hint, sizeof(hint),
             "%u 秒内说「没事」或点下面按钮取消\n"
             "否则通知家人",
             (unsigned)(ROBOT_ASK_ALARM_TIMEOUT_MS / 1000));
    hint_lbl = lv_label_create(ask_panel);
    lv_label_set_text(hint_lbl, hint);
    lv_obj_set_style_text_font(hint_lbl, &lv_font_ui_16, 0);
    lv_obj_set_style_text_color(hint_lbl, THEME_RED_BG, 0);
    lv_obj_set_style_text_align(hint_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(hint_lbl, LV_PCT(90));
    lv_label_set_long_mode(hint_lbl, LV_LABEL_LONG_WRAP);
    ask_panel_hint = hint_lbl;   /* 提示行要跟着状态改口（robot_ui_ask_hint） */

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
    lv_obj_set_style_bg_color(btn_yes, THEME_BG_CARD, 0);
    lv_obj_set_style_bg_opa(btn_yes, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_yes, 32, 0);
    lv_obj_add_event_cb(btn_yes, ask_btn_handler, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lbl = lv_label_create(btn_yes);
    lv_label_set_text(lbl, "是的，报警");
    lv_obj_set_style_text_font(lbl, &lv_font_ui_24, 0);
    lv_obj_set_style_text_color(lbl, THEME_RED, 0);
    lv_obj_center(lbl);

    /* 「不用了」：半透明白底白字（视觉上比上面那个"轻"，避免误点） */
    btn_no = lv_btn_create(row);
    lv_obj_set_size(btn_no, 150, 64);
    lv_obj_set_style_bg_color(btn_no, THEME_BG_CARD, 0);
    lv_obj_set_style_bg_opa(btn_no, LV_OPA_30, 0);
    lv_obj_set_style_border_width(btn_no, 2, 0);
    lv_obj_set_style_border_color(btn_no, THEME_BG_CARD, 0);
    lv_obj_set_style_radius(btn_no, 32, 0);
    lv_obj_add_event_cb(btn_no, ask_btn_handler, LV_EVENT_CLICKED, (void *)(intptr_t)0);
    lbl = lv_label_create(btn_no);
    lv_label_set_text(lbl, "我没事，取消");
    lv_obj_set_style_text_font(lbl, &lv_font_ui_24, 0);
    lv_obj_set_style_text_color(lbl, THEME_TEXT_ON_GREEN, 0);
    lv_obj_center(lbl);

    printf("[Ask] 报警确认页已弹出：原因=%s（%u 秒内说「没事」/点「我没事，取消」"
           "可停；到点就通知家人，铃不许断）\n",
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
