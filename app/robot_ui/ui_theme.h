/* ui_theme.h — 清新绿色健康主题调色板（浅色底 + 白色圆角卡片 + 柔和绿）
 * 390×450 圆角 AMOLED。所有 UI 颜色统一从这里取，禁止再散落 lv_color_hex 字面量。
 */
#ifndef __UI_THEME_H
#define __UI_THEME_H

#include "lvgl.h"

/* 页面/卡片 */
#define THEME_BG_PAGE        lv_color_hex(0xF3F8F4)  /* 浅薄荷绿页面底 */
#define THEME_BG_CARD        lv_color_hex(0xFFFFFF)  /* 白色圆角卡片 */
#define THEME_BORDER_SOFT    lv_color_hex(0xDCEBE0)  /* 卡片描边(浅绿灰) */

/* 主色 */
#define THEME_GREEN          lv_color_hex(0x4CAF50)  /* 主按钮/强调 */
#define THEME_GREEN_DARK     lv_color_hex(0x2E7D32)  /* 阴影/浅底上的深绿文字 */
#define THEME_GREEN_TINT     lv_color_hex(0xE8F5E9)  /* 浅绿底(次级按钮/页眉返回) */
#define THEME_GREEN_BORDER   lv_color_hex(0xA5D6A7)  /* 菜单项描边 */

/* 文字 */
#define THEME_TEXT_PRIMARY   lv_color_hex(0x2E3A32)  /* 深绿灰主文字 */
#define THEME_TEXT_SECONDARY lv_color_hex(0x78867C)  /* 副标题灰绿 */
#define THEME_TEXT_ON_GREEN  lv_color_hex(0xFFFFFF)  /* 绿底上的白字 */

/* 状态栏 */
#define THEME_STATUSBAR_BG   lv_color_hex(0xFFFFFF)
#define THEME_STATUSBAR_LINE lv_color_hex(0xDCEBE0)

/* 报警系(红色系保持) */
#define THEME_RED            lv_color_hex(0xF44336)
#define THEME_RED_DARK       lv_color_hex(0xD32F2F)
#define THEME_RED_BG         lv_color_hex(0xFDECEA)  /* 浅红底(报错卡) */
#define THEME_RED_BORDER     lv_color_hex(0xE53935)

/* 语音/状态四档(浅底可读版) */
#define THEME_ST_IDLE        lv_color_hex(0x9E9E9E)
#define THEME_ST_LISTENING   lv_color_hex(0x1E88E5)
#define THEME_ST_THINKING    lv_color_hex(0xFFB300)
#define THEME_ST_SPEAKING    lv_color_hex(0x43A047)
#define THEME_ST_REMINDING   lv_color_hex(0x8E24AA)  /* 提醒中(紫) */

/* 辅助 */
#define THEME_ORANGE         lv_color_hex(0xFF9800)  /* 关怀/摔倒描边 */
#define THEME_TRACK          lv_color_hex(0xCFD8DC)  /* 滑块轨道/中性按钮 */

/* 表情(浅底用深色，黄在浅底看不清) */
#define THEME_FACE_HAPPY     lv_color_hex(0x2E7D32)
#define THEME_FACE_THINKING  lv_color_hex(0x1E88E5)
#define THEME_FACE_SLEEPY    lv_color_hex(0x9E9E9E)
#define THEME_FACE_ALARM     THEME_RED

#endif /* __UI_THEME_H */
