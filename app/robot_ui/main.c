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
/* [临时改动.仅本次联调] 原来这行是 audio_record_start(&g_audio_ctx, NULL, 0)，但成员二当前头文件里的签名是
             *   int audio_record_start(audio_context_t *ctx, const audio_record_config_t *config)
             * 参数对不上、编不过;这几处本来是按猜测写的占位桩（传 NULL/0 帧，
             * 实际什么也不做）。先注释掉以便把 UI 编出来验证，等你按真实
             * 接口改对再放开。 */
            /* audio_record_start(&g_audio_ctx, NULL, 0); */
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
/* [临时改动.仅本次联调] 原来这行是 audio_play_start(&g_audio_ctx, NULL, 0)，但成员二当前头文件里的签名是
             *   int audio_play_start(audio_context_t *ctx, const int16_t *data, size_t frames, audio_play_complete_cb_t callback, void *user_data)
             * 参数对不上、编不过;这几处本来是按猜测写的占位桩（传 NULL/0 帧，
             * 实际什么也不做）。先注释掉以便把 UI 编出来验证，等你按真实
             * 接口改对再放开。 */
            /* audio_play_start(&g_audio_ctx, NULL, 0); */
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
/* [临时改动.仅本次联调] 原来这行是 llm_send_request(&g_llm_ctx, param, NULL)，但成员二当前头文件里的签名是
             *   ai_llm.h 里没有 llm_send_request，只有 llm_send_text(ctx, ...) / llm_send_audio(ctx, ...)
             * 参数对不上、编不过;这几处本来是按猜测写的占位桩（传 NULL/0 帧，
             * 实际什么也不做）。先注释掉以便把 UI 编出来验证，等你按真实
             * 接口改对再放开。 */
            /* llm_send_request(&g_llm_ctx, param, NULL); */
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
/* [临时改动.仅本次联调] 原来这行是 audio_play_start(&g_audio_ctx, NULL, 0)，但成员二当前头文件里的签名是
             *   int audio_play_start(audio_context_t *ctx, const int16_t *data, size_t frames, audio_play_complete_cb_t callback, void *user_data)
             * 参数对不上、编不过;这几处本来是按猜测写的占位桩（传 NULL/0 帧，
             * 实际什么也不做）。先注释掉以便把 UI 编出来验证，等你按真实
             * 接口改对再放开。 */
        /* audio_play_start(&g_audio_ctx, NULL, 0); */
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

    /* 触摸采样周期:默认跟随 LV_DEF_REFR_PERIOD（33ms ~ 30Hz），手感偏迟钝。
     * 这里只把输入设备读取定时器提到 10ms，屏幕刷新节奏不变。 */
    if (lv_result.indev != NULL)
    {
        lv_timer_set_period(lv_indev_get_read_timer(lv_result.indev), 10);
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
    /* key 传 NULL：由 network_comm.c 统一从 /etc/assets/push_key.txt
     * 或那里的 PUSH_KEY_DEFAULT* 取，源码里不再硬编码密钥。 */
    /* PushPlus (Android 微信推送) */
    push_init(PUSH_SERVICE_PUSHPLUS, NULL);
    /* Bark (iPad iOS 推送) */
    push_init(PUSH_SERVICE_BARK, NULL);

    /* ===== 注册回调函数 ===== */
    network_set_mqtt_callback(on_mqtt_message_received);
    network_set_ai_command_callback(on_ai_command_received);

    /* ===== 初始化 AI 模块 (成员二) ===== */
    printf("Initializing AI modules...\n");

#if 0
    /* [临时改动.仅本次联调] 这整段是照一套**当前树里不存在**的 API 写的，编不过:
     *   - audio_config_t 没有 bits_per_sample 成员
     *   - sound_detect_init() 需要 (ctx, config) 两个参数，这里是 1 个
     *   - sound_detect_set_alarm_callback() 不存在
     *   - care_init() 需要 (ctx, config) 两个参数，这里是 1 个
     *   - care_set_remind_callback() 不存在
     * 为了先把 UI 编出来看界面，整段停用。请按成员二 ai_audio.h / ai_sound_detect.h /
     * ai_care.h 的真实签名改对后放开，并删掉这个 #if 0。 */

    /* 初始化状态机 */
    if (sm_init(&g_sm_ctx) == 0) {
        printf("State machine initialized\n");
    }

    /* 初始化音频模块 */
    audio_config_t audio_config = {
        .sample_rate = AUDIO_RATE_16K,
        .channels = AUDIO_CH_MONO,
        .bits_per_sample = 16
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
    if (sound_detect_init(&g_sound_ctx) == 0) {
        printf("Sound detect initialized\n");
        /* 注册报警回调 */
        sound_detect_set_alarm_callback(&g_sound_ctx, sound_alarm_callback, NULL);
    }

    /* 初始化主动关怀 */
    if (care_init(&g_care_ctx) == 0) {
        printf("Care module initialized\n");
        /* 注册提醒回调 */
        care_set_remind_callback(&g_care_ctx, care_remind_callback, NULL);
    }
#endif /* 临时停用的 AI 初始化 */

    g_ai_initialized = false;
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
    robot_ui_set_ai_reply("你好！我是智爱陪伴\n有什么可以帮你的吗?");

    /* ===== 显示主菜单 ===== */
    touch_ui_show_menu(MENU_TYPE_MAIN);

    printf("ZhiAi Companion started!\n");

    /* 主循环 */
    while (1) {
        static int  net_tick = 0;
        static bool net_ok   = false;

        lvgl_timer_handler();

        /* 每 ~200ms 刷新一次状态栏上的网络状态。
         * LVGL 不是线程安全的，所以只在这个任务里改控件;network_task 那边
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
