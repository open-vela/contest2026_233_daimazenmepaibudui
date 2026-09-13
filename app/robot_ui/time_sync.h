/*
 * time_sync.h —— 联网后自动对时（见 time_sync.c）
 */

#ifndef __APP_ROBOT_UI_TIME_SYNC_H
#define __APP_ROBOT_UI_TIME_SYNC_H

/* 成功返回 0；失败返回负值。会阻塞（TLS 握手），别在 LVGL 线程里调。 */
int time_sync_once(void);

#endif /* __APP_ROBOT_UI_TIME_SYNC_H */
