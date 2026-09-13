/**
 * main.c - 智爱陪伴应用入口
 * 在 openvela 中运行
 */

#include <nuttx/config.h>
#include <stdio.h>
#include <unistd.h>
#include <sched.h>
#include <lvgl/lvgl.h>

/* UI 模块头文件 */
#include "robot_ui.h"
#include "touch_ui.h"
#include "network_comm.h"
#include <netutils/cJSON.h>

/* AI 模块头文件 (成员二) */
#include "ai_state_machine.h"
#include "ai_audio.h"
#include "ai_llm.h"
#include "ai_sound_detect.h"
#include "ai_care.h"
#include <string.h>

/* LVGL 定时器 */
static void lvgl_timer_handler(void)
{
    lv_timer_handler();
}

/* ==================== AI 模块全局上下文 ==================== */
static sm_context_t g_sm_ctx;           // 状态机
static audio_context_t g_audio_ctx;     // 音频
static llm_context_t g_llm_ctx;        // 大模型
static sound_detect_context_t g_sound_ctx;  // 声音检测
static care_context_t g_care_ctx;       // 主动关怀

/* AI 模块是否初始化成功 */
static bool g_ai_initialized = false;

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
        robot_ui_set_ai_reply("聆听中...\n请说话。");

        /* 调用成员二的 AI 模块开始录音 */
        if (g_ai_initialized) {
            audio_record_start(&g_audio_ctx, NULL);
            sm_handle_event(&g_sm_ctx, SM_EVENT_WAKEUP);
        }
    }
    else if (strcmp(action, "stop_voice") == 0) {
        /* 停止语音监听 */
        robot_ui_set_status(ROBOT_STATUS_IDLE);
        robot_ui_set_face(ROBOT_FACE_HAPPY);

        /* 调用成员二的 AI 模块停止录音 */
        if (g_ai_initialized) {
            audio_record_stop(&g_audio_ctx);
            sm_handle_event(&g_sm_ctx, SM_EVENT_VOICE_COMPLETE);
        }
    }
    else if (strcmp(action, "ai_reply") == 0) {
        /* 显示 AI 回复 */
        robot_ui_set_status(ROBOT_STATUS_SPEAKING);
        robot_ui_set_face(ROBOT_FACE_HAPPY);
        robot_ui_set_ai_reply(param);

        /* 播放语音回复 */
        if (g_ai_initialized) {
            audio_play_start(&g_audio_ctx, NULL, 0, NULL, NULL);
        }
    }
    else if (strcmp(action, "start_remind") == 0) {
        /* 开始提醒 */
        robot_ui_set_status(ROBOT_STATUS_REMINDING);
        robot_ui_set_face(ROBOT_FACE_WORRIED);
        robot_ui_show_reminder("提醒", param);
    }
    else if (strcmp(action, "start_alarm") == 0) {
        /* 开始报警 */
        robot_ui_show_alarm(param);

        /* 触发报警 */
        if (g_ai_initialized) {
            sm_handle_event(&g_sm_ctx, SM_EVENT_ALARM_DETECTED);
            report_alarm("sound_abnormal", param);
        }
    }
    else if (strcmp(action, "stop_alarm") == 0) {
        /* 停止报警 */
        robot_ui_close_alarm();

        if (g_ai_initialized) {
            sm_handle_event(&g_sm_ctx, SM_EVENT_ALARM_CLEARED);
        }
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
    else if (strcmp(action, "send_text") == 0) {
        /* 发送文本到 AI */
        if (g_ai_initialized) {
            robot_ui_set_status(ROBOT_STATUS_LISTENING);
            robot_ui_set_face(ROBOT_FACE_THINKING);
            llm_send_text(&g_llm_ctx, param, NULL, NULL, NULL);
        }
    }
}

/* ==================== AI 模块回调函数 ==================== */

/**
 * VAD 回调 - 语音活动检测
 */
static void vad_callback(bool speech_detected, void *user_data)
{
    if (speech_detected) {
        printf("[VAD] Speech detected\n");
        sm_handle_event(&g_sm_ctx, SM_EVENT_WAKEUP);
    } else {
        printf("[VAD] Speech ended\n");
        sm_handle_event(&g_sm_ctx, SM_EVENT_VOICE_COMPLETE);
    }
}

/**
 * LLM 回调 - AI 回复
 */
static void llm_response_callback(const char *response, void *user_data)
{
    printf("[LLM] Response: %s\n", response);

    /* 更新 UI 显示 AI 回复 */
    robot_ui_set_status(ROBOT_STATUS_SPEAKING);
    robot_ui_set_face(ROBOT_FACE_HAPPY);
    robot_ui_set_ai_reply(response);

    /* 播放语音 */
    if (g_ai_initialized) {
        audio_play_start(&g_audio_ctx, NULL, 0, NULL, NULL);
    }

    /* 通知状态机 */
    sm_handle_event(&g_sm_ctx, SM_EVENT_AI_RESPONSE);
}

/**
 * 声音检测回调 - 异常声音
 */
static void sound_alarm_callback(const char *sound_type, int confidence, void *user_data)
{
    printf("[SoundDetect] Alarm: %s (confidence: %d)\n", sound_type, confidence);

    /* 触发报警 */
    robot_ui_show_alarm(sound_type);
    report_alarm(sound_type, "Abnormal sound detected");

    /* 通知状态机 */
    sm_handle_event(&g_sm_ctx, SM_EVENT_ALARM_DETECTED);
}

/**
 * 主动关怀回调
 */
static void care_remind_callback(const char *title, const char *content, void *user_data)
{
    printf("[Care] Reminder: %s - %s\n", title, content);

    /* 显示提醒 */
    robot_ui_set_status(ROBOT_STATUS_REMINDING);
    robot_ui_set_face(ROBOT_FACE_WORRIED);
    robot_ui_show_reminder(title, content);

    /* 发送推送 */
    push_send_health_reminder(title, content);
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

    /* ===== 初始化 LVGL 与显示设备（必须在 UI 创建之前） ===== */
    lv_init();
    lv_nuttx_dsc_t lv_info = {0};
    lv_nuttx_result_t lv_result = {0};
    lv_nuttx_dsc_init(&lv_info);
    lv_nuttx_init(&lv_info, &lv_result);
    if (lv_result.disp == NULL)
    {
        printf("LVGL display init failed!\n");
        return 1;
    }

    /* ===== 初始化网络通信 ===== */
    network_comm_init();

    /* ===== 连接 WiFi ===== */
    wifi_connect("魔王城", "sjmbahczdszjj");

    /* ===== 启动网络后台任务 =====
     * 负责 MQTT 连接 broker.emqx.io:1883、收消息、发心跳。
     * network_task() 一直存在但从来没被创建过，所以 MQTT 一次都没连上过。
     * 它自己会轮询 wifi_config.connected，所以放在 wifi_connect() 之后创建。
     */
    if (task_create("net_task", 100, 12288, (main_t)network_task, NULL) < 0) {
        printf("net_task create failed\n");
    }

    /* ===== 初始化手机推送服务 ===== */
    /* PushPlus (Android 微信推送) */
    push_init(PUSH_SERVICE_PUSHPLUS, "1043ad84f9ba4dbb921756173d36277a");
    /* Bark (iPad iOS 推送) */
    push_init(PUSH_SERVICE_BARK, "726d1da9c292efcf947a85897c38310f6200a45c60ec8683813ae4d06fe67be9");

    /* ===== 注册回调函数 ===== */
    network_set_mqtt_callback(on_mqtt_message_received);
    network_set_ai_command_callback(on_ai_command_received);

    /* ===== 初始化 AI 模块 (成员二) ===== */
    printf("Initializing AI modules...\n");

    /* 初始化状态机 */
    if (sm_init(&g_sm_ctx) == 0) {
        printf("State machine initialized\n");
    }

    /* 初始化音频模块 */
    audio_config_t audio_config = {
        .sample_rate = AUDIO_RATE_16K,
        .channels = AUDIO_CH_MONO,
        .format = AUDIO_FORMAT_S16_LE
    };
    if (audio_init(&g_audio_ctx, &audio_config) == 0) {
        printf("Audio module initialized\n");
        /* 启用 VAD 检测 */
        audio_vad_enable(&g_audio_ctx, vad_callback, NULL);
    }

    /* 初始化 LLM 模块 */
    if (llm_init(&g_llm_ctx, NULL) == 0) {
        printf("LLM module initialized\n");
    }

    /* 初始化声音检测 */
    {
        sound_detect_config_t detect_cfg = {
            .mode = DETECT_MODE_REALTIME,
            .threshold = SOUND_DETECT_THRESHOLD_DEFAULT,
            .sample_rate = SOUND_DETECT_SAMPLE_RATE,
            .frame_ms = SOUND_DETECT_FRAME_MS,
            .enable_vad = true,
            .enable_feedback = true,
            .callback = NULL,
            .user_data = NULL
        };
        if (sound_detect_init(&g_sound_ctx, &detect_cfg) == 0) {
            printf("Sound detect initialized\n");
        }
    }

    /* 初始化主动关怀 */
    {
        care_config_t care_cfg = {
            .enable_greeting = true,
            .enable_health = true,
            .enable_life = true,
            .enable_exercise = true,
            .callback = NULL,
            .user_data = NULL
        };
        if (care_init(&g_care_ctx, &care_cfg) == 0) {
            printf("Care module initialized\n");
        }
    }

    g_ai_initialized = true;
    printf("AI modules initialization done\n");

    /* ===== 初始化机器人 UI（先创建主屏并 lv_scr_load，成为活动屏） ===== */
    robot_ui_init();

    /* ===== 初始化触摸交互 UI（须在活动屏 = 主屏之后, 菜单才可见） ===== */
    touch_ui_init();

    /* ===== 添加默认提醒 ===== */
    touch_ui_add_reminder("吃药", "08:00");
    touch_ui_add_reminder("喝水", "10:00");
    touch_ui_add_reminder("散步", "16:00");

    /* ===== 设置初始状态 ===== */
    robot_ui_set_status(ROBOT_STATUS_IDLE);
    robot_ui_set_face(ROBOT_FACE_HAPPY);
    robot_ui_set_ai_reply("你好！我是智爱陪伴\n有什么可以帮你的吗？");

    /* ===== 显示主菜单 ===== */
    touch_ui_show_menu(MENU_TYPE_MAIN);

    printf("ZhiAi Companion started!\n");

    /* 主循环 */
    while (1) {
        static int  net_tick = 0;
        static bool net_ok   = false;

        lvgl_timer_handler();

        /* 每 ~200ms 刷新一次状态栏上的网络状态。
         * LVGL 不是线程安全的，所以只在这个任务里改控件；network_task 那边
         * 只维护 mqtt_config.connected，由这里轮询。
         */
        if (++net_tick >= 40) {
            bool ok = mqtt_is_connected();

            net_tick = 0;
            if (ok != net_ok) {
                net_ok = ok;
                robot_ui_set_net_status(ok ? "NET OK" : "NET --");
            }
        }

        /* 运行 AI 模块 */
        if (g_ai_initialized) {
            sm_run(&g_sm_ctx);
        }

        usleep(5000); // 5ms 刷新周期
    }

    return 0;
}
