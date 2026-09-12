/****************************************************************************
 * AI Companion Network Integration Implementation
 * 智爱陪伴 - 网络通信集成模块
 *
 * 该模块封装了 network_comm 接口，为 AI 陪伴系统提供网络通信功能
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "ai_network.h"

/* 包含 robot_ui 的 network_comm 接口 */
#include "../robot_ui/network_comm.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 调试输出宏 */
#ifdef CONFIG_DEBUG_AI_NETWORK
#  define ai_network_info(fmt, ...)  printf("[AI_NET] " fmt "\n", ##__VA_ARGS__)
#  define ai_network_warn(fmt, ...)  printf("[AI_NET WARN] " fmt "\n", ##__VA_ARGS__)
#  define ai_network_err(fmt, ...)   printf("[AI_NET ERR] " fmt "\n", ##__VA_ARGS__)
#else
#  define ai_network_info(fmt, ...)
#  define ai_network_warn(fmt, ...)
#  define ai_network_err(fmt, ...)   printf("[AI_NET ERR] " fmt "\n", ##__VA_ARGS__)
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 全局网络上下文（用于回调） */
static ai_network_context_t *g_net_ctx = NULL;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * @brief  MQTT 消息接收回调
 */

static void on_mqtt_message(const char *topic, const char *payload)
{
  ai_network_context_t *ctx = g_net_ctx;
  if (!ctx)
    {
      return;
    }

  ai_network_info("MQTT message: topic=%s", topic);

  /* 解析命令主题 */
  char cmd_topic[128];
  snprintf(cmd_topic, sizeof(cmd_topic), AI_MQTT_TOPIC_COMMAND, ctx->config.mqtt_client_id);

  if (strstr(topic, "/command"))
    {
      /* 解析命令 JSON */
      /* TODO: 使用 cJSON 解析 payload */
      if (ctx->command_cb)
        {
          ctx->command_cb("unknown", payload, ctx->user_data);
        }
    }
  else if (strstr(topic, "/voice"))
    {
      /* 语音数据回调 */
      if (ctx->voice_cb)
        {
          ctx->voice_cb(payload, 0, ctx->user_data);
        }
    }
  else if (strstr(topic, "/chat"))
    {
      /* 聊天消息回调 */
      if (ctx->chat_cb)
        {
          ctx->chat_cb(payload, ctx->user_data);
        }
    }
}

/**
 * @brief  WiFi 状态变化回调
 */

static void on_wifi_status(bool connected)
{
  ai_network_context_t *ctx = g_net_ctx;
  if (!ctx)
    {
      return;
    }

  if (connected)
    {
      ai_network_info("WiFi connected");
      ctx->state = AI_NET_STATE_WIFI_CONNECTED;
    }
  else
    {
      ai_network_info("WiFi disconnected");
      ctx->state = AI_NET_STATE_IDLE;
    }

  /* 触发状态回调 */
  if (ctx->status_cb)
    {
      ctx->status_cb(ctx->state, ctx->user_data);
    }
}

/**
 * @brief  报警回调
 */

static void on_alarm(const char *alarm_type, const char *details)
{
  ai_network_context_t *ctx = g_net_ctx;
  if (!ctx)
    {
      return;
    }

  ai_network_info("Alarm: type=%s, details=%s", alarm_type, details);

  /* 发送推送通知 */
  push_send_alarm(alarm_type, details);
}

/**
 * @brief  AI 命令回调
 */

static void on_ai_command(const char *action, const char *param)
{
  ai_network_context_t *ctx = g_net_ctx;
  if (!ctx)
    {
      return;
    }

  ai_network_info("AI command: action=%s, param=%s", action, param);

  if (ctx->command_cb)
    {
      ctx->command_cb(action, param, ctx->user_data);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/**
 * @brief  初始化网络模块
 */

int ai_network_init(ai_network_context_t *ctx, const ai_network_config_t *config)
{
  int ret;

  if (!ctx || !config)
    {
      return -EINVAL;
    }

  /* 保存上下文到全局变量（用于回调） */
  g_net_ctx = ctx;

  /* 初始化上下文 */
  memset(ctx, 0, sizeof(ai_network_context_t));
  memcpy(&ctx->config, config, sizeof(ai_network_config_t));
  ctx->state = AI_NET_STATE_IDLE;
  ctx->initialized = true;

  /* 初始化 network_comm 模块 */
  ret = network_comm_init();
  if (ret < 0)
    {
      ai_network_err("network_comm_init failed: %d", ret);
      return ret;
    }

  /* 注册回调函数 */
  network_set_mqtt_callback(on_mqtt_message);
  network_set_wifi_callback(on_wifi_status);
  network_set_alarm_callback(on_alarm);
  network_set_ai_command_callback(on_ai_command);

  /* 初始化推送服务 */
  if (config->push_enabled && config->push_key[0] != '\0')
    {
      ret = push_init(PUSH_SERVICE_BARK, config->push_key);
      if (ret < 0)
        {
          ai_network_warn("push_init failed: %d", ret);
          /* 推送初始化失败不阻止启动 */
        }
    }

  ai_network_info("Network module initialized");
  ai_network_info("  MQTT broker: %s:%d", config->mqtt_broker, config->mqtt_port);
  ai_network_info("  Client ID: %s", config->mqtt_client_id);

  return OK;
}

/**
 * @brief  反初始化网络模块
 */

void ai_network_deinit(ai_network_context_t *ctx)
{
  if (!ctx || !ctx->initialized)
    {
      return;
    }

  /* 断开 MQTT */
  ai_network_disconnect_mqtt(ctx);

  /* 断开 WiFi */
  ai_network_disconnect_wifi(ctx);

  /* 反初始化 network_comm */
  network_comm_deinit();

  ctx->initialized = false;
  g_net_ctx = NULL;

  ai_network_info("Network module deinitialized");
}

/**
 * @brief  连接 WiFi
 */

int ai_network_connect_wifi(ai_network_context_t *ctx,
                            const char *ssid, const char *password)
{
  int ret;

  if (!ctx || !ssid || !password)
    {
      return -EINVAL;
    }

  ai_network_info("Connecting to WiFi: %s", ssid);

  /* 保存 WiFi 配置 */
  strncpy(ctx->config.wifi_ssid, ssid, sizeof(ctx->config.wifi_ssid) - 1);
  strncpy(ctx->config.wifi_password, password, sizeof(ctx->config.wifi_password) - 1);

  /* 更新状态 */
  ctx->state = AI_NET_STATE_WIFI_CONNECTING;
  if (ctx->status_cb)
    {
      ctx->status_cb(ctx->state, ctx->user_data);
    }

  /* 调用 network_comm 连接 WiFi */
  ret = wifi_connect(ssid, password);
  if (ret < 0)
    {
      ai_network_err("WiFi connect failed: %d", ret);
      ctx->state = AI_NET_STATE_ERROR;
      if (ctx->status_cb)
        {
          ctx->status_cb(ctx->state, ctx->user_data);
        }
      return ret;
    }

  ctx->state = AI_NET_STATE_WIFI_CONNECTED;
  if (ctx->status_cb)
    {
      ctx->status_cb(ctx->state, ctx->user_data);
    }

  ai_network_info("WiFi connected successfully");
  return OK;
}

/**
 * @brief  断开 WiFi
 */

int ai_network_disconnect_wifi(ai_network_context_t *ctx)
{
  if (!ctx)
    {
      return -EINVAL;
    }

  int ret = wifi_disconnect();
  ctx->state = AI_NET_STATE_IDLE;

  if (ctx->status_cb)
    {
      ctx->status_cb(ctx->state, ctx->user_data);
    }

  return ret;
}

/**
 * @brief  连接 MQTT 服务器
 */

int ai_network_connect_mqtt(ai_network_context_t *ctx)
{
  int ret;

  if (!ctx)
    {
      return -EINVAL;
    }

  if (!ai_network_is_wifi_connected(ctx))
    {
      ai_network_err("WiFi not connected, cannot connect MQTT");
      return -ENOTCONN;
    }

  ai_network_info("Connecting to MQTT: %s:%d",
                  ctx->config.mqtt_broker, ctx->config.mqtt_port);

  /* 更新状态 */
  ctx->state = AI_NET_STATE_MQTT_CONNECTING;
  if (ctx->status_cb)
    {
      ctx->status_cb(ctx->state, ctx->user_data);
    }

  /* 调用 network_comm 连接 MQTT */
  ret = mqtt_connect(ctx->config.mqtt_broker, ctx->config.mqtt_port,
                     ctx->config.mqtt_client_id,
                     ctx->config.mqtt_username,
                     ctx->config.mqtt_password);
  if (ret < 0)
    {
      ai_network_err("MQTT connect failed: %d", ret);
      ctx->state = AI_NET_STATE_WIFI_CONNECTED;
      if (ctx->status_cb)
        {
          ctx->status_cb(ctx->state, ctx->user_data);
        }
      return ret;
    }

  ctx->state = AI_NET_STATE_MQTT_CONNECTED;
  if (ctx->status_cb)
    {
      ctx->status_cb(ctx->state, ctx->user_data);
    }

  /* 订阅命令主题 */
  char topic[128];
  snprintf(topic, sizeof(topic), AI_MQTT_TOPIC_COMMAND, ctx->config.mqtt_client_id);
  mqtt_subscribe(topic, 1);

  /* 订阅语音主题 */
  snprintf(topic, sizeof(topic), AI_MQTT_TOPIC_VOICE, ctx->config.mqtt_client_id);
  mqtt_subscribe(topic, 1);

  /* 订阅聊天主题 */
  snprintf(topic, sizeof(topic), AI_MQTT_TOPIC_CHAT, ctx->config.mqtt_client_id);
  mqtt_subscribe(topic, 1);

  ai_network_info("MQTT connected successfully");
  return OK;
}

/**
 * @brief  断开 MQTT 连接
 */

int ai_network_disconnect_mqtt(ai_network_context_t *ctx)
{
  if (!ctx)
    {
      return -EINVAL;
    }

  int ret = mqtt_disconnect();
  ctx->state = AI_NET_STATE_WIFI_CONNECTED;

  if (ctx->status_cb)
    {
      ctx->status_cb(ctx->state, ctx->user_data);
    }

  return ret;
}

/**
 * @brief  发送语音数据到云端
 */

int ai_network_send_voice(ai_network_context_t *ctx,
                          const uint8_t *audio_data, int len)
{
  if (!ctx || !audio_data || len <= 0)
    {
      return -EINVAL;
    }

  if (!ai_network_is_mqtt_connected(ctx))
    {
      ai_network_err("MQTT not connected, cannot send voice");
      return -ENOTCONN;
    }

  /* 调用 network_comm 发送语音 */
  return ai_send_voice_data(audio_data, len, NULL);
}

/**
 * @brief  发送文本到云端获取 AI 回复
 */

int ai_network_send_text(ai_network_context_t *ctx, const char *text)
{
  if (!ctx || !text)
    {
      return -EINVAL;
    }

  if (!ai_network_is_mqtt_connected(ctx))
    {
      ai_network_err("MQTT not connected, cannot send text");
      return -ENOTCONN;
    }

  /* 调用 network_comm 发送文本 */
  return ai_send_text(text, NULL);
}

/**
 * @brief  上报设备状态
 */

int ai_network_report_status(ai_network_context_t *ctx,
                             float temperature, float humidity,
                             int battery_level)
{
  if (!ctx)
    {
      return -EINVAL;
    }

  if (!ai_network_is_mqtt_connected(ctx))
    {
      ai_network_err("MQTT not connected, cannot report status");
      return -ENOTCONN;
    }

  /* 构建设备状态 */
  device_status_t status;
  memset(&status, 0, sizeof(status));
  status.temperature = temperature;
  status.humidity = humidity;
  status.battery_level = battery_level;
  strncpy(status.status, "online", sizeof(status.status) - 1);
  status.alarm_active = false;

  /* 调用 network_comm 上报状态 */
  return report_device_status(&status);
}

/**
 * @brief  上报异常声音检测结果
 */

int ai_network_report_sound_alarm(ai_network_context_t *ctx,
                                  const char *sound_type, int confidence)
{
  if (!ctx || !sound_type)
    {
      return -EINVAL;
    }

  if (!ai_network_is_mqtt_connected(ctx))
    {
      ai_network_err("MQTT not connected, cannot report sound alarm");
      return -ENOTCONN;
    }

  /* 调用 network_comm 上报异常声音 */
  return report_abnormal_sound(sound_type, confidence);
}

/**
 * @brief  发送主动关怀提醒
 */

int ai_network_send_reminder(ai_network_context_t *ctx,
                             const char *title, const char *content)
{
  if (!ctx || !title || !content)
    {
      return -EINVAL;
    }

  if (!ai_network_is_mqtt_connected(ctx))
    {
      ai_network_err("MQTT not connected, cannot send reminder");
      return -ENOTCONN;
    }

  /* 调用 network_comm 发送提醒 */
  return send_proactive_reminder(title, content);
}

/**
 * @brief  上报健康数据
 */

int ai_network_report_health(ai_network_context_t *ctx,
                             int heart_rate, int blood_oxy)
{
  if (!ctx)
    {
      return -EINVAL;
    }

  if (!ai_network_is_mqtt_connected(ctx))
    {
      ai_network_err("MQTT not connected, cannot report health");
      return -ENOTCONN;
    }

  /* 调用 network_comm 上报健康数据 */
  return report_health_data(heart_rate, blood_oxy);
}

/**
 * @brief  发送心跳包
 */

int ai_network_send_heartbeat(ai_network_context_t *ctx)
{
  if (!ctx)
    {
      return -EINVAL;
    }

  if (!ai_network_is_mqtt_connected(ctx))
    {
      return -ENOTCONN;
    }

  /* 调用 network_comm 发送心跳 */
  return report_heartbeat();
}

/**
 * @brief  网络后台任务
 */

void ai_network_task(void *arg)
{
  ai_network_context_t *ctx = (ai_network_context_t *)arg;
  if (!ctx || !ctx->initialized)
    {
      return;
    }

  ai_network_info("Network task started");

  while (ctx->initialized)
    {
      uint32_t now = (uint32_t)time(NULL) * 1000;

      /* 检查 WiFi 状态 */
      if (ctx->state == AI_NET_STATE_IDLE ||
          ctx->state == AI_NET_STATE_ERROR)
        {
          /* 尝试重连 WiFi */
          if (now - ctx->last_reconnect_time >= AI_RECONNECT_INTERVAL_MS)
            {
              ai_network_info("Attempting WiFi reconnection...");
              ai_network_connect_wifi(ctx, ctx->config.wifi_ssid,
                                      ctx->config.wifi_password);
              ctx->last_reconnect_time = now;
            }
        }

      /* 检查 MQTT 状态 */
      if (ctx->state == AI_NET_STATE_WIFI_CONNECTED)
        {
          /* 尝试连接 MQTT */
          ai_network_connect_mqtt(ctx);
        }

      /* 发送心跳 */
      if (ctx->state == AI_NET_STATE_MQTT_CONNECTED)
        {
          if (now - ctx->last_heartbeat_time >= AI_HEARTBEAT_INTERVAL_MS)
            {
              ai_network_send_heartbeat(ctx);
              ctx->last_heartbeat_time = now;
            }
        }

      /* 休眠 100ms */
      usleep(100000);
    }

  ai_network_info("Network task exited");
}

/**
 * @brief  注册状态变化回调
 */

void ai_network_set_status_callback(ai_network_context_t *ctx,
                                    ai_net_status_callback_t callback,
                                    void *user_data)
{
  if (ctx)
    {
      ctx->status_cb = callback;
      ctx->user_data = user_data;
    }
}

/**
 * @brief  注册命令接收回调
 */

void ai_network_set_command_callback(ai_network_context_t *ctx,
                                     ai_net_command_callback_t callback,
                                     void *user_data)
{
  if (ctx)
    {
      ctx->command_cb = callback;
      ctx->user_data = user_data;
    }
}

/**
 * @brief  注册语音数据回调
 */

void ai_network_set_voice_callback(ai_network_context_t *ctx,
                                   ai_net_voice_callback_t callback,
                                   void *user_data)
{
  if (ctx)
    {
      ctx->voice_cb = callback;
      ctx->user_data = user_data;
    }
}

/**
 * @brief  注册聊天消息回调
 */

void ai_network_set_chat_callback(ai_network_context_t *ctx,
                                  ai_net_chat_callback_t callback,
                                  void *user_data)
{
  if (ctx)
    {
      ctx->chat_cb = callback;
      ctx->user_data = user_data;
    }
}

/**
 * @brief  获取当前网络状态
 */

ai_net_state_t ai_network_get_state(ai_network_context_t *ctx)
{
  if (ctx)
    {
      return ctx->state;
    }

  return AI_NET_STATE_IDLE;
}

/**
 * @brief  检查 WiFi 是否已连接
 */

bool ai_network_is_wifi_connected(ai_network_context_t *ctx)
{
  if (ctx)
    {
      return wifi_is_connected();
    }

  return false;
}

/**
 * @brief  检查 MQTT 是否已连接
 */

bool ai_network_is_mqtt_connected(ai_network_context_t *ctx)
{
  if (ctx)
    {
      return mqtt_is_connected();
    }

  return false;
}

/**
 * @brief  发送紧急报警
 */

int ai_network_send_alarm(ai_network_context_t *ctx,
                          const char *alarm_type, const char *details)
{
  if (!ctx || !alarm_type || !details)
    {
      return -EINVAL;
    }

  if (!ai_network_is_mqtt_connected(ctx))
    {
      ai_network_err("MQTT not connected, cannot send alarm");
      return -ENOTCONN;
    }

  /* 调用 network_comm 上报报警 */
  return report_alarm(alarm_type, details);
}

/**
 * @brief  发送设备控制命令
 */

int ai_network_send_device_command(ai_network_context_t *ctx,
                                   const char *device_id, const char *command)
{
  if (!ctx || !device_id || !command)
    {
      return -EINVAL;
    }

  if (!ai_network_is_mqtt_connected(ctx))
    {
      ai_network_err("MQTT not connected, cannot send device command");
      return -ENOTCONN;
    }

  /* 调用 network_comm 发送设备命令 */
  return send_device_command(device_id, command);
}
