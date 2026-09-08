/**
 * network_comm.h - 网络通信模块头文件
 * WiFi 连接、MQTT 通信、云端交互
 */

#ifndef NETWORK_COMM_H
#define NETWORK_COMM_H

#include <stdint.h>
#include <stdbool.h>

/* ==================== WiFi 配置 ==================== */
typedef struct {
    char ssid[64];          // WiFi 名称
    char password[64];      // WiFi 密码
    bool connected;         // 连接状态
    int rssi;               // 信号强度
} wifi_config_t;

/* ==================== MQTT 配置 ==================== */
typedef struct {
    char broker[128];       // MQTT 服务器地址
    uint16_t port;          // 端口号
    char client_id[64];     // 客户端 ID
    char username[64];      // 用户名
    char password[64];      // 密码
    bool connected;         // 连接状态
} mqtt_config_t;

/* ==================== 设备状态 ==================== */
typedef struct {
    float temperature;      // 温度
    float humidity;         // 湿度
    int battery_level;      // 电量
    char status[32];        // 状态
    bool alarm_active;      // 报警状态
} device_status_t;

/* ==================== 消息类型 ==================== */
typedef enum {
    MSG_TYPE_STATUS,        // 状态上报
    MSG_TYPE_ALARM,         // 报警消息
    MSG_TYPE_REMINDER,      // 提醒消息
    MSG_TYPE_COMMAND,       // 控制命令
    MSG_TYPE_HEARTBEAT,     // 心跳
    MSG_TYPE_MAX
} msg_type_t;

/* ==================== 回调函数类型 ==================== */
typedef void (*mqtt_msg_callback_t)(const char *topic, const char *payload);
typedef void (*wifi_status_callback_t)(bool connected);
typedef void (*alarm_callback_t)(const char *alarm_type, const char *details);
typedef void (*ai_reply_callback_t)(const char *reply);
typedef void (*ai_command_callback_t)(const char *action, const char *param);

/* ==================== 初始化函数 ==================== */
int network_comm_init(void);
void network_comm_deinit(void);

/* ==================== WiFi 函数 ==================== */
int wifi_connect(const char *ssid, const char *password);
int wifi_disconnect(void);
bool wifi_is_connected(void);
int wifi_get_rssi(void);

/* ==================== MQTT 函数 ==================== */
int mqtt_connect(const char *broker, uint16_t port,
                const char *client_id, const char *username, const char *password);
int mqtt_disconnect(void);
bool mqtt_is_connected(void);
int mqtt_subscribe(const char *topic, int qos);
int mqtt_unsubscribe(const char *topic);
int mqtt_publish(const char *topic, const char *payload, int qos, bool retain);

/* ==================== 数据上报函数 ==================== */
int report_device_status(const device_status_t *status);
int report_alarm(const char *alarm_type, const char *details);
int report_heartbeat(void);

/* ==================== 远程控制函数 ==================== */
int send_command_response(const char *cmd_id, bool success, const char *message);

/* ==================== 回调注册函数 ==================== */
void network_set_mqtt_callback(mqtt_msg_callback_t callback);
void network_set_wifi_callback(wifi_status_callback_t callback);
void network_set_alarm_callback(alarm_callback_t callback);
void network_set_ai_command_callback(ai_command_callback_t callback);

/* ==================== AI 语音交互接口 ==================== */

/**
 * 发送语音数据到云端并获取 AI 回复
 * @param audio_data  音频数据
 * @param len         数据长度
 * @param callback    AI 回复回调
 * @return 0 成功, -1 失败
 */
int ai_send_voice_data(const uint8_t *audio_data, int len,
                       ai_reply_callback_t callback);

/**
 * 发送文本到云端获取 AI 回复（用于非语音交互）
 * @param text        输入文本
 * @param callback    AI 回复回调
 * @return 0 成功, -1 失败
 */
int ai_send_text(const char *text, ai_reply_callback_t callback);

/* ==================== 异常声音检测接口 ==================== */

/**
 * 上报异常声音检测结果
 * @param sound_type  声音类型: "scream", "fall", "knock", "help"
 * @param confidence  置信度 0-100
 * @return 0 成功, -1 失败
 */
int report_abnormal_sound(const char *sound_type, int confidence);

/* ==================== 主动关怀接口 ==================== */

/**
 * 发送定时提醒到手机端
 * @param title   提醒标题
 * @param content 提醒内容
 * @return 0 成功, -1 失败
 */
int send_proactive_reminder(const char *title, const char *content);

/**
 * 发送健康数据到云端
 * @param heart_rate  心率
 * @param blood_oxy   血氧
 * @return 0 成功, -1 失败
 */
int report_health_data(int heart_rate, int blood_oxy);

/* ==================== 设备联动接口 ==================== */

/**
 * 发送设备控制命令
 * @param device_id  目标设备 ID
 * @param command    命令内容 (JSON)
 * @return 0 成功, -1 失败
 */
int send_device_command(const char *device_id, const char *command);

/* ==================== 手机推送接口 ==================== */

/* 推送服务类型 */
typedef enum {
    PUSH_SERVICE_BARK,      // Bark (iOS)
    PUSH_SERVICE_PUSHPLUS,  // PushPlus (Android/iOS, 微信推送)
    PUSH_SERVICE_MAX
} push_service_t;

/* 推送配置 */
typedef struct {
    push_service_t service;     // 推送服务类型
    char push_key[128];         // 推送 key
    bool enabled;               // 推送开关
} push_config_t;

/**
 * 初始化推送服务
 * @param service  推送服务类型
 * @param key      推送 key (从 Bark/PushPlus 获取)
 * @return 0 成功, -1 失败
 */
int push_init(push_service_t service, const char *key);

/**
 * 发送推送通知
 * @param title    通知标题
 * @param content  通知内容
 * @param group    分组 (可选，用于分类通知)
 * @return 0 成功, -1 失败
 */
int push_send_notification(const char *title, const char *content, const char *group);

/**
 * 发送紧急报警推送
 * @param alarm_type  报警类型
 * @param details     详细信息
 * @return 0 成功, -1 失败
 */
int push_send_alarm(const char *alarm_type, const char *details);

/**
 * 发送健康提醒推送
 * @param title   提醒标题
 * @param content 提醒内容
 * @return 0 成功, -1 失败
 */
int push_send_health_reminder(const char *title, const char *content);

/**
 * 开关推送功能
 * @param enabled  true 开启, false 关闭
 */
void push_set_enabled(bool enabled);

/**
 * 检查推送功能是否开启
 */
bool push_is_enabled(void);

/* ==================== 网络后台任务 ==================== */
void network_task(void *arg);

#endif /* NETWORK_COMM_H */
