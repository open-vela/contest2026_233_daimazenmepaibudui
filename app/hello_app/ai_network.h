/****************************************************************************
 * AI Companion Network Integration Header
 * 智爱陪伴 - 网络通信集成模块
 ****************************************************************************/

#ifndef AI_NETWORK_H
#define AI_NETWORK_H

#include <stdint.h>
#include <stdbool.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* MQTT 主题定义 */
#define AI_MQTT_TOPIC_COMMAND      "zhi_ai/%s/command"
#define AI_MQTT_TOPIC_RESPONSE     "zhi_ai/%s/response"
#define AI_MQTT_TOPIC_STATUS       "zhi_ai/%s/status"
#define AI_MQTT_TOPIC_VOICE        "zhi_ai/%s/voice"
#define AI_MQTT_TOPIC_CHAT         "zhi_ai/%s/chat"
#define AI_MQTT_TOPIC_ALARM        "zhi_ai/%s/alarm"
#define AI_MQTT_TOPIC_HEALTH       "zhi_ai/%s/health"
#define AI_MQTT_TOPIC_REMINDER     "zhi_ai/%s/reminder"
#define AI_MQTT_TOPIC_SOUND_ALARM  "zhi_ai/%s/sound_alarm"

/* 默认 MQTT 服务器配置 */
#define AI_MQTT_DEFAULT_BROKER     "broker.emqx.io"
#define AI_MQTT_DEFAULT_PORT       1883
#define AI_MQTT_DEFAULT_CLIENT_ID  "zhi_ai_001"

/* 心跳间隔(毫秒) */
#define AI_HEARTBEAT_INTERVAL_MS   30000

/* 网络重连间隔(毫秒) */
#define AI_RECONNECT_INTERVAL_MS   5000

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* 网络状态枚举 */
typedef enum
{
  AI_NET_STATE_IDLE = 0,      /* 空闲状态 */
  AI_NET_STATE_WIFI_CONNECTING, /* WiFi 连接中 */
  AI_NET_STATE_WIFI_CONNECTED,  /* WiFi 已连接 */
  AI_NET_STATE_MQTT_CONNECTING, /* MQTT 连接中 */
  AI_NET_STATE_MQTT_CONNECTED,  /* MQTT 已连接 */
  AI_NET_STATE_ERROR          /* 错误状态 */
} ai_net_state_t;

/* 网络配置结构体 */
typedef struct
{
  /* WiFi 配置 */
  char wifi_ssid[64];         /* WiFi 名称 */
  char wifi_password[64];     /* WiFi 密码 */

  /* MQTT 配置 */
  char mqtt_broker[128];      /* MQTT 服务器地址 */
  uint16_t mqtt_port;         /* MQTT 端口号 */
  char mqtt_client_id[64];    /* MQTT 客户端 ID */
  char mqtt_username[64];     /* MQTT 用户名 */
  char mqtt_password[64];     /* MQTT 密码 */

  /* 推送配置 */
  bool push_enabled;          /* 推送开关 */
  char push_key[128];         /* 推送 key */
} ai_network_config_t;

/* 网络回调函数类型 */
typedef void (*ai_net_status_callback_t)(ai_net_state_t state, void *user_data);
typedef void (*ai_net_command_callback_t)(const char *action, const char *param, void *user_data);
typedef void (*ai_net_voice_callback_t)(const char *audio_url, int duration, void *user_data);
typedef void (*ai_net_chat_callback_t)(const char *message, void *user_data);

/* 网络上下文结构体 */
typedef struct
{
  ai_net_state_t state;               /* 当前网络状态 */
  ai_network_config_t config;         /* 网络配置 */
  void *user_data;                    /* 用户数据 */

  /* 回调函数 */
  ai_net_status_callback_t status_cb;   /* 状态变化回调 */
  ai_net_command_callback_t command_cb;  /* 命令接收回调 */
  ai_net_voice_callback_t voice_cb;      /* 语音数据回调 */
  ai_net_chat_callback_t chat_cb;        /* 聊天消息回调 */

  /* 内部状态 */
  bool initialized;                   /* 初始化标志 */
  uint32_t last_heartbeat_time;       /* 上次心跳时间 */
  uint32_t last_reconnect_time;       /* 上次重连时间 */
} ai_network_context_t;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/**
 * @brief  初始化网络模块
 * @param  ctx      网络上下文
 * @param  config   网络配置
 * @return 0 成功, 负值失败
 */

int ai_network_init(ai_network_context_t *ctx, const ai_network_config_t *config);

/**
 * @brief  反初始化网络模块
 * @param  ctx  网络上下文
 */

void ai_network_deinit(ai_network_context_t *ctx);

/**
 * @brief  连接 WiFi
 * @param  ctx       网络上下文
 * @param  ssid      WiFi 名称
 * @param  password  WiFi 密码
 * @return 0 成功, 负值失败
 */

int ai_network_connect_wifi(ai_network_context_t *ctx,
                            const char *ssid, const char *password);

/**
 * @brief  断开 WiFi
 * @param  ctx  网络上下文
 * @return 0 成功, 负值失败
 */

int ai_network_disconnect_wifi(ai_network_context_t *ctx);

/**
 * @brief  连接 MQTT 服务器
 * @param  ctx  网络上下文
 * @return 0 成功, 负值失败
 */

int ai_network_connect_mqtt(ai_network_context_t *ctx);

/**
 * @brief  断开 MQTT 连接
 * @param  ctx  网络上下文
 * @return 0 成功, 负值失败
 */

int ai_network_disconnect_mqtt(ai_network_context_t *ctx);

/**
 * @brief  发送语音数据到云端
 * @param  ctx         网络上下文
 * @param  audio_data  音频数据
 * @param  len         数据长度
 * @return 0 成功, 负值失败
 */

int ai_network_send_voice(ai_network_context_t *ctx,
                          const uint8_t *audio_data, int len);

/**
 * @brief  发送文本到云端获取 AI 回复
 * @param  ctx      网络上下文
 * @param  text     输入文本
 * @return 0 成功, 负值失败
 */

int ai_network_send_text(ai_network_context_t *ctx, const char *text);

/**
 * @brief  上报设备状态
 * @param  ctx           网络上下文
 * @param  temperature   温度
 * @param  humidity      湿度
 * @param  battery_level 电量
 * @return 0 成功, 负值失败
 */

int ai_network_report_status(ai_network_context_t *ctx,
                             float temperature, float humidity,
                             int battery_level);

/**
 * @brief  上报异常声音检测结果
 * @param  ctx          网络上下文
 * @param  sound_type   声音类型
 * @param  confidence   置信度 0-100
 * @return 0 成功, 负值失败
 */

int ai_network_report_sound_alarm(ai_network_context_t *ctx,
                                  const char *sound_type, int confidence);

/**
 * @brief  发送主动关怀提醒
 * @param  ctx       网络上下文
 * @param  title     提醒标题
 * @param  content   提醒内容
 * @return 0 成功, 负值失败
 */

int ai_network_send_reminder(ai_network_context_t *ctx,
                             const char *title, const char *content);

/**
 * @brief  上报健康数据
 * @param  ctx          网络上下文
 * @param  heart_rate   心率
 * @param  blood_oxy    血氧
 * @return 0 成功, 负值失败
 */

int ai_network_report_health(ai_network_context_t *ctx,
                             int heart_rate, int blood_oxy);

/**
 * @brief  发送心跳包
 * @param  ctx  网络上下文
 * @return 0 成功, 负值失败
 */

int ai_network_send_heartbeat(ai_network_context_t *ctx);

/**
 * @brief  网络后台任务
 * @param  arg  网络上下文指针
 */

void ai_network_task(void *arg);

/**
 * @brief  注册状态变化回调
 * @param  ctx       网络上下文
 * @param  callback  回调函数
 * @param  user_data 用户数据
 */

void ai_network_set_status_callback(ai_network_context_t *ctx,
                                    ai_net_status_callback_t callback,
                                    void *user_data);

/**
 * @brief  注册命令接收回调
 * @param  ctx       网络上下文
 * @param  callback  回调函数
 * @param  user_data 用户数据
 */

void ai_network_set_command_callback(ai_network_context_t *ctx,
                                     ai_net_command_callback_t callback,
                                     void *user_data);

/**
 * @brief  注册语音数据回调
 * @param  ctx       网络上下文
 * @param  callback  回调函数
 * @param  user_data 用户数据
 */

void ai_network_set_voice_callback(ai_network_context_t *ctx,
                                   ai_net_voice_callback_t callback,
                                   void *user_data);

/**
 * @brief  注册聊天消息回调
 * @param  ctx       网络上下文
 * @param  callback  回调函数
 * @param  user_data 用户数据
 */

void ai_network_set_chat_callback(ai_network_context_t *ctx,
                                  ai_net_chat_callback_t callback,
                                  void *user_data);

/**
 * @brief  获取当前网络状态
 * @param  ctx  网络上下文
 * @return 网络状态
 */

ai_net_state_t ai_network_get_state(ai_network_context_t *ctx);

/**
 * @brief  检查 WiFi 是否已连接
 * @param  ctx  网络上下文
 * @return true 已连接, false 未连接
 */

bool ai_network_is_wifi_connected(ai_network_context_t *ctx);

/**
 * @brief  检查 MQTT 是否已连接
 * @param  ctx  网络上下文
 * @return true 已连接, false 未连接
 */

bool ai_network_is_mqtt_connected(ai_network_context_t *ctx);

/**
 * @brief  发送紧急报警
 * @param  ctx          网络上下文
 * @param  alarm_type   报警类型
 * @param  details      详细信息
 * @return 0 成功, 负值失败
 */

int ai_network_send_alarm(ai_network_context_t *ctx,
                          const char *alarm_type, const char *details);

/**
 * @brief  发送设备控制命令
 * @param  ctx         网络上下文
 * @param  device_id   目标设备 ID
 * @param  command     命令内容
 * @return 0 成功, 负值失败
 */

int ai_network_send_device_command(ai_network_context_t *ctx,
                                   const char *device_id, const char *command);

#endif /* AI_NETWORK_H */
