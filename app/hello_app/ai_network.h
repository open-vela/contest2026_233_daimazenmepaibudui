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

/* 回传给界面的命令动作名（robot_ui 的 on_ai_command_received() 认这几个） */
#define AI_CMD_ACTION_AI_REPLY     "ai_reply"     /* param = 要显示的文字 */
#define AI_CMD_ACTION_SET_FACE     "set_face"     /* param = 表情名 */
#define AI_CMD_ACTION_START_ALARM  "start_alarm"  /* param = 报警页上的文字 */

/* set_face 的表情名 */
#define AI_CMD_FACE_HAPPY          "happy"
#define AI_CMD_FACE_THINKING       "thinking"
#define AI_CMD_FACE_WORRIED        "worried"

/* 一条命令里 param 最多放多少字节（JSON 转义前的上限，超出截断） */
#define AI_CMD_PARAM_MAX           512

/* 复用已有 MQTT 连接时的等待时长（毫秒）：
 * 界面（robot_ui）的 network_task 会自己连 broker，先等它一会儿，
 * 免得两边各开一条同 client_id 的连接。详见 ai_network_start_shared()。 */
#define AI_MQTT_SHARED_WAIT_MS     3000

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
} ai_network_context_t;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

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
 * @brief  上报异常声音检测结果
 * @param  ctx          网络上下文
 * @param  sound_type   声音类型
 * @param  confidence   置信度 0-100
 * @return 0 成功, 负值失败
 */

int ai_network_report_sound_alarm(ai_network_context_t *ctx,
                                  const char *sound_type, int confidence);

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

/****************************************************************************
 * 结果回传（语音入口在框架侧之后，界面只认 MQTT，需要下面这组接口）
 ****************************************************************************/

/**
 * @brief  接上网络并把 AI 的结果回传给界面
 *
 * 为什么不用 network_comm_init()：network_comm.c 在一个固件里只有一份实例，
 * 界面（robot_ui）的 network_task 已经拥有那条 MQTT socket。
 * 所以这里**不重跑 network_comm_init()、不抢回调、不重复订阅**，
 * 只在没人连的时候兜底连一次，之后所有 publish 都走那条共用连接。
 *
 * @param  ctx        网络上下文
 * @param  client_id  MQTT 客户端 ID（决定 publish 的主题），NULL/空串用默认值
 * @return 0 成功(含"已经在连"的情况), 负值失败
 */

int ai_network_start_shared(ai_network_context_t *ctx, const char *client_id);

/**
 * @brief  改 client_id（主题名跟着变，要在 publish 之前调）
 * @param  ctx        网络上下文
 * @param  client_id  新的客户端 ID，NULL/空串回落默认值
 * @return 0 成功, 负值失败(太长)
 */

int ai_network_set_client_id(ai_network_context_t *ctx, const char *client_id);

/**
 * @brief  取当前 client_id
 * @param  ctx  网络上下文
 * @return 字符串（ctx 为空时返回默认值）
 */

const char *ai_network_get_client_id(ai_network_context_t *ctx);

/**
 * @brief  往 zhi_ai/<client_id>/command 发一条 {action, param}
 * @param  ctx     网络上下文
 * @param  action  动作名，见 AI_CMD_ACTION_*
 * @param  param   参数（UTF-8 文本，超长截断；NULL 当空串）
 * @return 0 成功, -ENOTCONN 没连上, 负值失败
 */

int ai_network_publish_command(ai_network_context_t *ctx,
                               const char *action, const char *param);

/**
 * @brief  把 AI 回复正文发给界面显示（action=ai_reply）
 * @param  ctx   网络上下文
 * @param  text  回复正文
 * @return 0 成功, 负值失败
 */

int ai_network_send_ai_reply(ai_network_context_t *ctx, const char *text);

/**
 * @brief  让界面换个表情（action=set_face）
 * @param  ctx   网络上下文
 * @param  face  表情名，见 AI_CMD_FACE_*
 * @return 0 成功, 负值失败
 */

int ai_network_send_face(ai_network_context_t *ctx, const char *face);

/**
 * @brief  让界面弹报警页（action=start_alarm）
 * @param  ctx   网络上下文
 * @param  text  报警页上的文字
 * @return 0 成功, 负值失败
 */

int ai_network_send_start_alarm(ai_network_context_t *ctx, const char *text);

#endif /* AI_NETWORK_H */
