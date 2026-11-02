/* ui_theme.h — 黄白色暖调主题（奶油底 + 白色圆角卡片 + 暖阳黄）
 * 390×450 圆角 AMOLED。所有 UI 颜色统一从这里取，禁止再散落 lv_color_hex 字面量。
 *
 * ★ 2026-10-29 整体色调从「清新绿」换成「黄白色」（用户点名「所有 ui 整体色调
 *   换成黄白色」）。改法是**只动这一个文件**：主色/页面底/描边/文字全部换成
 *   暖黄系，散落的字面量早就被收在这里了，所以换色不用满仓库找。
 *
 * ★ 红色系**一个字节都不动**（用户：「报警按钮和页面依旧保持红色」）。
 *   报警是安全功能，它的红是"约定"不是"配色" —— 老人和家属已经把
 *   「红=出事了」学会了，换掉等于重新教一遍，还教错了。
 *
 * ★ 黄底上**不能用白字**（对比度不够，老人更看不清），所以
 *   THEME_TEXT_ON_GREEN（主按钮上的文字）是深棕。名字里那个 GREEN 是历史
 *   遗留，指"主色"而不是"绿" —— 换名要动十几处调用点，不值当。 */
#ifndef __UI_THEME_H
#define __UI_THEME_H

#include "lvgl.h"

/* 页面/卡片 —— 黄白色 */
#define THEME_BG_PAGE        lv_color_hex(0xFFF8E8)  /* 奶油黄白页面底 */
#define THEME_BG_CARD        lv_color_hex(0xFFFFFF)  /* 白色圆角卡片 */
#define THEME_BORDER_SOFT    lv_color_hex(0xF1E2C2)  /* 卡片描边(浅黄灰) */

/* 主色 —— 暖阳黄 */
#define THEME_GREEN          lv_color_hex(0xF6B910)  /* 主按钮/强调（暖阳黄） */
#define THEME_GREEN_DARK     lv_color_hex(0x9A6A00)  /* 阴影/浅底上的深金棕文字 */
#define THEME_GREEN_TINT     lv_color_hex(0xFFF2D2)  /* 浅黄底(次级按钮/页眉返回) */
#define THEME_GREEN_BORDER   lv_color_hex(0xE9C77A)  /* 菜单项描边 */

/* 文字 —— 暖棕（黄白底上读得清） */
#define THEME_TEXT_PRIMARY   lv_color_hex(0x4A3A20)  /* 深棕主文字 */
#define THEME_TEXT_SECONDARY lv_color_hex(0x9C8A66)  /* 暖灰副标题 */
#define THEME_TEXT_ON_GREEN  lv_color_hex(0x4A3200)  /* 黄底上的深棕字（见头上注释） */

/* 状态栏 */
#define THEME_STATUSBAR_BG   lv_color_hex(0xFFFFFF)
#define THEME_STATUSBAR_LINE lv_color_hex(0xF1E2C2)

/* 报警系(红色系保持) —— ★ 2026-10-29 换色调时**故意不碰** */
#define THEME_RED            lv_color_hex(0xF44336)
#define THEME_RED_DARK       lv_color_hex(0xD32F2F)
#define THEME_RED_BG         lv_color_hex(0xFDECEA)  /* 浅红底(报错卡) */
#define THEME_RED_BORDER     lv_color_hex(0xE53935)

/* 语音/状态四档（浅底可读版）。
 * 2026-10-29 换成暖调：空闲暖灰、在听深琥珀、在想主黄。
 * 「在说」保留绿色**不是舍不得配色** —— 这一档是老人唯一要一眼认出的
 * （"它在跟我说话"），黄白主题里只有绿/红还能和主黄拉开距离，
 * 而红已经被报警占了。 */
#define THEME_ST_IDLE        lv_color_hex(0xB0A288)  /* 暖灰 */
#define THEME_ST_LISTENING   lv_color_hex(0xD98324)  /* 深琥珀 */
#define THEME_ST_THINKING    lv_color_hex(0xF6B910)  /* 主黄 */
#define THEME_ST_SPEAKING    lv_color_hex(0x3D9A66)  /* 绿：在说，必须一眼认出 */
#define THEME_ST_REMINDING   lv_color_hex(0x9457C4)  /* 提醒中(紫) */

/* 辅助 */
#define THEME_ORANGE         lv_color_hex(0xE07B20)  /* 关怀/摔倒描边 */
#define THEME_TRACK          lv_color_hex(0xE8D9BC)  /* 滑块轨道/中性按钮 */

/* 表情(浅底用深色) */
#define THEME_FACE_HAPPY     lv_color_hex(0x9A6A00)  /* 深金 */
#define THEME_FACE_THINKING  lv_color_hex(0xD98324)  /* 深琥珀 */
#define THEME_FACE_SLEEPY    lv_color_hex(0xB0A288)  /* 暖灰 */
#define THEME_FACE_ALARM     THEME_RED

#endif /* __UI_THEME_H */
