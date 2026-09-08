/**
 * main.c - 智爱陪伴应用入口
 * 在 openvela 中运行
 */

#include <nuttx/config.h>
#include <stdio.h>
#include <unistd.h>

/* 头文件 */
#include "robot_ui.h"
#include "touch_ui.h"
#include "network_comm.h"
#include <cjson/cJSON.h>
#include <string.h>

/* LVGL 定时器 */
static void lvgl_timer_handler(void)
{
    lv_timer_handler();
}

/* ==================== AI 命令回调处理 ==================== */

/**
 * 处理来自手机端或云端的 AI 命令
 * 成员二的 AI 模块可以通过此接口接收控制命令
 */
static void on_ai_command_received(const char *action, const char *param)
{
    printf("AI command: action=%s, param=%s\n", action, param);

    if (strcmp(action, "start_voice") == 0) {
        /* 开始语音监听 */
        robot_ui_set_status(ROBOT_STATUS_LISTENING);
        robot_ui_set_face(ROBOT_FACE_THINKING);
        robot_ui_set_ai_reply("Listening...\nPlease speak.");
        // TODO: 调用成员二的 AI 模块开始录音
    }
    else if (strcmp(action, "stop_voice") == 0) {
        /* 停止语音监听 */
        robot_ui_set_status(ROBOT_STATUS_IDLE);
        robot_ui_set_face(ROBOT_FACE_HAPPY);
        // TODO: 调用成员二的 AI 模块停止录音
    }
    else if (strcmp(action, "ai_reply") == 0) {
        /* 显示 AI 回复 */
        robot_ui_set_status(ROBOT_STATUS_SPEAKING);
        robot_ui_set_face(ROBOT_FACE_HAPPY);
        robot_ui_set_ai_reply(param);
        // TODO: 播放语音回复
    }
    else if (strcmp(action, "start_remind") == 0) {
        /* 开始提醒 */
        robot_ui_set_status(ROBOT_STATUS_REMINDING);
        robot_ui_set_face(ROBOT_FACE_WORRIED);
        robot_ui_show_reminder("Reminder", param);
    }
    else if (strcmp(action, "start_alarm") == 0) {
        /* 开始报警 */
        robot_ui_show_alarm(param);
        // TODO: 调用成员二的 AI 模块触发报警
    }
    else if (strcmp(action, "stop_alarm") == 0) {
        /* 停止报警 */
        robot_ui_close_alarm();
    }
    else if (strcmp(action, "set_face") == 0) {
        /* 设置表情 */
        if (strcmp(param, "happy") == 0) {
            robot_ui_set_face(ROBOT_FACE_HAPPY);
        } else if (strcmp(param, "thinking") == 0) {
            robot_ui_set_face(ROBOT_FACE_THINKING);
        } else if (strcmp(param, "sleepy") == 0) {
            robot_ui_set_face(ROBOT_FACE_SLEEPY);
        } else if (strcmp(param, "surprised") == 0) {
            robot_ui_set_face(ROBOT_FACE_SURPRISED);
        } else if (strcmp(param, "worried") == 0) {
            robot_ui_set_face(ROBOT_FACE_WORRIED);
        }
    }
}

/* ==================== MQTT 消息回调处理 ==================== */

/**
 * 处理来自 MQTT 的消息
 * 解析命令并转发给 AI 命令处理函数
 */
static void on_mqtt_message_received(const char *topic, const char *payload)
{
    printf("MQTT received: topic=%s\n", topic);

    /* 解析 JSON 命令 */
    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        printf("JSON parse failed\n");
        return;
    }

    cJSON *action = cJSON_GetObjectItem(root, "action");
    cJSON *param = cJSON_GetObjectItem(root, "param");

    if (action && action->valuestring) {
        const char *param_str = (param && param->valuestring) ? param->valuestring : "";
        on_ai_command_received(action->valuestring, param_str);
    }

    cJSON_Delete(root);
}

/* 主函数 */
int main(int argc, char *argv[])
{
    printf("ZhiAi Companion starting...\n");

    /* ===== 初始化网络通信 ===== */
    network_comm_init();

    /* ===== 连接 WiFi ===== */
    wifi_connect("魔王城", "sjmbahczdszjj");

    /* ===== 初始化手机推送服务 ===== */
    /* PushPlus (Android 微信推送) */
    push_init(PUSH_SERVICE_PUSHPLUS, "1043ad84f9ba4dbb921756173d36277a");
    /* Bark (iPad iOS 推送) */
    push_init(PUSH_SERVICE_BARK, "726d1da9c292efcf947a85897c38310f6200a45c60ec8683813ae4d06fe67be9");

    /* ===== 注册回调函数 ===== */
    network_set_mqtt_callback(on_mqtt_message_received);
    network_set_ai_command_callback(on_ai_command_received);

    /* ===== 初始化触摸交互 UI（先初始化） ===== */
    touch_ui_init();

    /* ===== 初始化机器人 UI ===== */
    robot_ui_init();

    /* ===== 添加默认提醒 ===== */
    touch_ui_add_reminder("Medicine", "08:00");
    touch_ui_add_reminder("Drink Water", "10:00");
    touch_ui_add_reminder("Take a Walk", "16:00");

    /* ===== 设置初始状态 ===== */
    robot_ui_set_status(ROBOT_STATUS_IDLE);
    robot_ui_set_face(ROBOT_FACE_HAPPY);
    robot_ui_set_ai_reply("Hello! I am ZhiAi.\nHow can I help you?");

    /* ===== 显示主菜单 ===== */
    touch_ui_show_menu(MENU_TYPE_MAIN);

    printf("ZhiAi Companion started!\n");

    /* 主循环 */
    while (1) {
        lvgl_timer_handler();
        usleep(5000); // 5ms 刷新周期
    }

    return 0;
}
