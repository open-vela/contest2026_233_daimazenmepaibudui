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
 * Public Functions
 ****************************************************************************/

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

  /* 调用 network_comm 上报异常声音。
   * 走排队口：本函数被 hello_app 的线程调用（"两轮都没听清就报未确认"），
   * 直发会打在本组里一个无效/别人的 fd 上（理由见 ai_network_publish_command）。 */
  return report_abnormal_sound_queued(sound_type, confidence);
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

  /* 调用 network_comm 上报报警。
   * 走排队口：本函数被 hello_app 的线程调用（紧急追问判定 + 上报警页），
   * 直发会打在本组里一个无效/别人的 fd 上（理由见 ai_network_publish_command）。
   * topic（.../alarm）、payload 结构、QoS1 和本地回调/手机推送都由
   * network_comm 那侧的同一段代码负责，行为不变。 */
  return report_alarm_queued(alarm_type, details);
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

  /* 调用 network_comm 发送设备命令。
   * 走排队口：本函数被 hello_app 的线程调用（灯控工具 ai_tools_provider），
   * 直发必然失败 —— 真机日志里那条
   *   [意图] 命中灯控: 开灯，但 device_cmd 发送失败(-107)，兜底回话「网络没连上，灯没打开」
   * 就是这里；而 -107 的直接原因是前面那条 hello_app 的 publish 把 connected
   * 标成了 false（理由见 ai_network_publish_command）。
   * topic（.../device_cmd）、payload {"type":"device_command","device_id":...,
   * "command":...}、QoS1 都不变，只是改由 network_task 那条 socket 发。 */
  return send_device_command_queued(device_id, command);
}

/****************************************************************************
 * 结果回传：publish 到 zhi_ai/<client_id>/command (W1)
 *
 * 背景：语音入口统一到框架侧（ai_companion）之后，界面 robot_ui 不再开麦，
 * 只认 MQTT。所以"要显示的东西"（AI 回复正文、表情、报警页）都从这里发出去。
 *
 * 一个必须知道的事实：network_comm.c 会被编进**同一个固件**，
 * 界面那边（robot_ui）和这里用的是同一份 mqtt_config / mqtt_socket。
 * 所以这里刻意做得很保守：
 *   - 不调 network_comm_init()（它会把界面那边已配好的状态 memset 掉）；
 *   - 不调 network_set_mqtt_callback()（MQTT 收包回调只有一个槽，
 *     占了它就等于把界面的 ai_reply 派发顶掉，界面再也收不到消息）；
 *   - 不重复订阅（界面 mqtt_connect() 里已经订阅了 command 主题）。
 ****************************************************************************/

/**
 * @brief  把一段 UTF-8 文本转成能塞进 JSON 字符串的字节
 *
 * 只转义 JSON 必须转义的（" 和 \），把换行/制表压成空格，丢掉其余控制字符；
 * 中文等 UTF-8 多字节序列原样透传。截断落在某段 UTF-8 序列中间时，
 * 把这段不完整的字节丢掉 —— 界面是直接拿去显示的，半个字会变方块。
 *
 * @param  in        输入（NULL 当空串）
 * @param  out       输出缓冲
 * @param  out_size  输出缓冲大小（含结尾 '\0'）
 * @return 写进 out 的字节数（不含结尾 '\0'）
 */

static size_t ai_network_json_escape(const char *in, char *out, size_t out_size)
{
  const unsigned char *p = (const unsigned char *)(in != NULL ? in : "");
  size_t n = 0;
  size_t i;

  if (out_size == 0)
    {
      return 0;
    }

  while (*p != '\0')
    {
      unsigned char c = *p;
      size_t seq;      /* 这个字符占几个输入字节 */
      size_t need;     /* 转义后占几个输出字节 */

      if (c == '"' || c == '\\')
        {
          seq = 1;
          need = 2;
        }
      else if (c == '\n' || c == '\r' || c == '\t')
        {
          seq = 1;
          need = 1;
        }
      else if (c < 0x20 || c == 0x7f)
        {
          seq = 1;
          need = 0;    /* 其余控制字符直接丢 */
        }
      else if ((c & 0x80) == 0x00)
        {
          seq = 1;
          need = 1;
        }
      else if ((c & 0xe0) == 0xc0)
        {
          seq = 2;
          need = 2;
        }
      else if ((c & 0xf0) == 0xe0)
        {
          seq = 3;
          need = 3;
        }
      else if ((c & 0xf8) == 0xf0)
        {
          seq = 4;
          need = 4;
        }
      else
        {
          seq = 1;
          need = 0;    /* 非法首字节 */
        }

      /* 多字节序列必须凑齐续字节，否则说明尾巴被截断了 */
      for (i = 1; i < seq; i++)
        {
          if ((p[i] & 0xc0) != 0x80)
            {
              seq = 0;
              break;
            }
        }

      if (seq == 0 || n + need + 1 > out_size)
        {
          break;
        }

      if (need == 2)
        {
          out[n++] = '\\';
        }

      if (need != 0)
        {
          if (c == '\n' || c == '\r' || c == '\t')
            {
              out[n++] = ' ';
            }
          else
            {
              memcpy(&out[n], p, seq);
              n += seq;
            }
        }

      p += seq;
    }

  out[n] = '\0';
  return n;
}

/**
 * @brief  改 client_id
 */

int ai_network_set_client_id(ai_network_context_t *ctx, const char *client_id)
{
  if (ctx == NULL)
    {
      return -EINVAL;
    }

  if (client_id == NULL || client_id[0] == '\0')
    {
      client_id = AI_MQTT_DEFAULT_CLIENT_ID;
    }

  if (strlen(client_id) >= sizeof(ctx->config.mqtt_client_id))
    {
      ai_network_err("client_id 太长: %s", client_id);
      return -EINVAL;
    }

  strncpy(ctx->config.mqtt_client_id, client_id,
          sizeof(ctx->config.mqtt_client_id) - 1);
  ctx->config.mqtt_client_id[sizeof(ctx->config.mqtt_client_id) - 1] = '\0';

  return OK;
}

/**
 * @brief  取当前 client_id
 */

const char *ai_network_get_client_id(ai_network_context_t *ctx)
{
  if (ctx == NULL || ctx->config.mqtt_client_id[0] == '\0')
    {
      return AI_MQTT_DEFAULT_CLIENT_ID;
    }

  return ctx->config.mqtt_client_id;
}

/**
 * @brief  接上网络（复用界面已经建好的那条连接，没人管时兜底连一次）
 */

int ai_network_start_shared(ai_network_context_t *ctx, const char *client_id)
{
  int ret;
  int i_;

  if (ctx == NULL)
    {
      return -EINVAL;
    }

  /* 只清自己的上下文，不碰 network_comm 的全局状态 */

  memset(ctx, 0, sizeof(ai_network_context_t));

  ctx->config.mqtt_port = AI_MQTT_DEFAULT_PORT;
  strncpy(ctx->config.mqtt_broker, AI_MQTT_DEFAULT_BROKER,
          sizeof(ctx->config.mqtt_broker) - 1);

  ret = ai_network_set_client_id(ctx, client_id);
  if (ret < 0)
    {
      return ret;
    }

  ctx->state = AI_NET_STATE_IDLE;
  ctx->initialized = true;

  /* 已经连着（界面那边的 network_task 连的）：直接复用，别再开第二条。
   * 两个 client_id 相同的连接同时挂在一个 broker 上，broker 会把先来的踢掉。 */

  if (ai_network_is_mqtt_connected(ctx))
    {
      ctx->state = AI_NET_STATE_MQTT_CONNECTED;
      ai_network_info("复用已有的 MQTT 连接, client_id=%s",
                      ai_network_get_client_id(ctx));
      return OK;
    }

  /* 界面（robot_ui）在不在？
   * 本板走 USB RNDIS，wifi_connect() 在这个板级配置下只是把"已联网"标志置上
   * （和 robot_ui/main.c 的写法一致），不调用它 MQTT 那边会因为"WiFi 未连接"
   * 直接拒绝连接。
   * 这个标志顺带还是个"界面来过"的信号：界面一开机就调它、别的地方都不调，
   * 所以我们自己单跑时它是 false。 */

  if (!ai_network_is_wifi_connected(ctx))
    {
      /* 只有自己在跑：没人会去连 MQTT，直接连，不用等 */

      wifi_connect("RNDIS", "");
    }
  else
    {
      /* 界面在跑：它的 network_task 开机后第一轮就会去连 MQTT，
       * 先等它一会儿，别跟它抢同一条 client_id 的连接。 */

      for (i_ = 0; i_ < AI_MQTT_SHARED_WAIT_MS / 100; i_++)
        {
          if (ai_network_is_mqtt_connected(ctx))
            {
              ctx->state = AI_NET_STATE_MQTT_CONNECTED;
              ai_network_info("等到了界面建立的 MQTT 连接, client_id=%s",
                              ai_network_get_client_id(ctx));
              return OK;
            }

          usleep(100000);
        }
    }

  /* 没人管连接（比如只跑了 ai_companion，没起界面）：自己兜底连一次。
   * 注意 socket 建起来之后，收包/重连仍然是界面的 network_task 在做；
   * 只跑 ai_companion 时没人收包（命令主题收不到），但回传照样能发。 */

  ai_network_info("自己建立 MQTT 连接, client_id=%s",
                  ai_network_get_client_id(ctx));
  ret = ai_network_connect_mqtt(ctx);
  if (ret < 0)
    {
      ai_network_err("连接 MQTT 失败: %d", ret);
      return ret;
    }

  return OK;
}

/**
 * @brief  往 zhi_ai/<client_id>/command 发一条 {action, param}
 *
 * ⚠️ 为什么这里必须用 mqtt_publish_queued() 而不是 mqtt_publish()：
 *
 * NuttX 的 fd 属于 task group。mqtt_socket 是 robot_ui 的 network_task 建的，
 * 只存在于那个组的 fd 表里；本函数跑在 **hello_app 的线程**（语音状态机 /
 * 工具执行）里，拿同一个数字去 send()，要么 EBADF、要么发到本组里恰好占了
 * 这个编号的别的文件上。
 *
 * 真机日志（一轮完整语音之后，网络本身是好的 —— 同一时刻 HTTPS / ASR 都成功、
 * free 还有 5.4 MB 空闲内存）：
 *
 *   MQTT connected
 *   MQTT publish failed: -1, marked disconnected for reconnect   <- 本条（voice_state）
 *   MQTT disconnected
 *   [语音] user_said MQTT 回传失败(-107)（界面已由直调刷过，手机端看不到）: 帮我打开灯。
 *   [AI_NET ERR] MQTT not connected, cannot send device command
 *   ...
 *   MQTT connecting: broker.emqx.io:1883 -> MQTT connected
 *   Publish to zhi_ai/zhi_ai_001/heartbeat: {...}                <- network_task 自己发，成功
 *
 * 判据：**心跳（network_task 自己发）永远成功，凡是 hello_app 线程发起的
 * （voice_state / user_said / device_cmd）全部失败**。而且第一条失败会把
 * connected 标成 false（那是 mqtt_publish 里正确的重连逻辑），于是后面几条
 * 连试都不试，直接 -ENOTCONN(-107) —— 那两行 -107 不是"网络断了"。
 *
 * 排队之后这三条走的是同一条已带重连/失败标记的发送路径（network_task 里），
 * 没有第二套逻辑；topic 名、payload 结构、QoS 一个字节都没改。
 */

int ai_network_publish_command(ai_network_context_t *ctx,
                               const char *action, const char *param)
{
  char topic[128];
  char escaped[AI_CMD_PARAM_MAX];
  char payload[AI_CMD_PARAM_MAX + 64];
  int ret;

  if (ctx == NULL || action == NULL || action[0] == '\0')
    {
      return -EINVAL;
    }

  if (!ai_network_is_mqtt_connected(ctx))
    {
      ai_network_warn("MQTT 未连接，命令发不出去: action=%s", action);
      return -ENOTCONN;
    }

  if (ai_network_json_escape(param, escaped, sizeof(escaped)) == 0
      && param != NULL && param[0] != '\0')
    {
      ai_network_warn("命令参数放不下或不可显示，已丢空: action=%s", action);
    }

  snprintf(payload, sizeof(payload), "{\"action\":\"%s\",\"param\":\"%s\"}",
           action, escaped);
  snprintf(topic, sizeof(topic), AI_MQTT_TOPIC_COMMAND,
           ai_network_get_client_id(ctx));

  ret = mqtt_publish_queued(topic, payload, 0, false);
  if (ret < 0)
    {
      ai_network_err("命令下发失败: %s -> %d", topic, ret);
      return ret;
    }

  ai_network_info("命令已下发: %s %s", topic, payload);
  return OK;
}

/**
 * @brief  把 AI 回复正文发给界面显示
 */

int ai_network_send_ai_reply(ai_network_context_t *ctx, const char *text)
{
  if (text == NULL || text[0] == '\0')
    {
      return -EINVAL;
    }

  return ai_network_publish_command(ctx, AI_CMD_ACTION_AI_REPLY, text);
}

/**
 * @brief  让界面换个表情
 */

int ai_network_send_face(ai_network_context_t *ctx, const char *face)
{
  if (face == NULL || face[0] == '\0')
    {
      return -EINVAL;
    }

  return ai_network_publish_command(ctx, AI_CMD_ACTION_SET_FACE, face);
}

/**
 * @brief  让界面弹报警页
 */

int ai_network_send_start_alarm(ai_network_context_t *ctx, const char *text)
{
  return ai_network_publish_command(ctx, AI_CMD_ACTION_START_ALARM,
                                    text != NULL ? text : "");
}
