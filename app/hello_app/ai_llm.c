/****************************************************************************
 * AI LLM Module Implementation
 * 智爱陪伴 - AI老人陪伴守护终端
 * AI对话模块 - 大模型API调用与会话管理
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "ai_llm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <time.h>

#ifdef CONFIG_HELLO_APP_LLM_AI_AGENT
#  include <velaclaw/client.h>
#endif

/* NuttX网络相关头文件 */

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* HTTP方法和头 */
#define HTTP_POST               "POST"
#define HTTP_GET                "GET"
#define HTTP_CONTENT_TYPE       "Content-Type: application/json"
#define HTTP_AUTH_BEARER        "Authorization: Bearer "

/* JSON模板 */
#define JSON_TEMPLATE_REQUEST   \
  "{" \
  "\"model\":\"%s\"," \
  "\"messages\":[%s]," \
  "\"max_tokens\":%lu," \
  "\"temperature\":%.2f," \
  "\"stream\":%s" \
  "}"

#define JSON_TEMPLATE_MESSAGE   \
  "{\"role\":\"%s\",\"content\":\"%s\"}"

/* 响应缓冲区大小 */
#define LLM_RESPONSE_BUF_SIZE   8192

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void *llm_request_thread(void *arg);
#ifndef CONFIG_HELLO_APP_LLM_AI_AGENT
static int llm_do_http_request(llm_context_t *ctx,
                               const char *url,
                               const char *api_key,
                               const char *body,
                               llm_response_t *response);
#endif
static int llm_send_to_api(llm_context_t *ctx,
                           const char *text,
                           llm_stream_cb_t stream_cb,
                           llm_complete_cb_t complete_cb,
                           void *user_data);
static void llm_add_to_history(llm_context_t *ctx,
                               llm_role_t role,
                               const char *content);
static int llm_build_messages_json(llm_context_t *ctx,
                                   const char *user_message,
                                   char *buf, size_t buf_size);
static int llm_escape_json_string(const char *input, char *output,
                                  size_t max_len);
static uint32_t llm_get_timestamp(void);

#ifdef CONFIG_HELLO_APP_LLM_AI_AGENT
static int llm_do_ai_agent_request(llm_context_t *ctx, const char *text,
                                   char *response, size_t response_size);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 状态名称表 */
static const char *g_state_names[] =
{
  [LLM_STATE_UNINIT]     = "UNINIT",
  [LLM_STATE_IDLE]       = "IDLE",
  [LLM_STATE_REQUESTING] = "REQUESTING",
  [LLM_STATE_STREAMING]  = "STREAMING",
  [LLM_STATE_DONE]       = "DONE",
  [LLM_STATE_ERROR]      = "ERROR"
};

/* 角色名称表 */
static const char *g_role_names[] =
{
  [LLM_ROLE_SYSTEM]    = "system",
  [LLM_ROLE_USER]      = "user",
  [LLM_ROLE_ASSISTANT] = "assistant"
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * @brief  获取当前时间戳
 */

static uint32_t llm_get_timestamp(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)ts.tv_sec;
}

/**
 * @brief  JSON字符串转义
 */

static int llm_escape_json_string(const char *input, char *output,
                                  size_t max_len)
{
  if (input == NULL || output == NULL || max_len == 0)
    {
      return -EINVAL;
    }

  size_t i = 0;
  size_t j = 0;

  while (input[i] != '\0')
    {
      size_t needed = input[i] == '"' || input[i] == '\\' ||
                      input[i] == '\n' || input[i] == '\r' ||
                      input[i] == '\t' ? 2 : 1;
      if (j + needed >= max_len)
        {
          output[j] = '\0';
          return -ENOSPC;
        }

      switch (input[i])
        {
          case '"':
            if (j + 2 < max_len)
              {
                output[j++] = '\\';
                output[j++] = '"';
              }
            break;
          case '\\':
            if (j + 2 < max_len)
              {
                output[j++] = '\\';
                output[j++] = '\\';
              }
            break;
          case '\n':
            if (j + 2 < max_len)
              {
                output[j++] = '\\';
                output[j++] = 'n';
              }
            break;
          case '\r':
            if (j + 2 < max_len)
              {
                output[j++] = '\\';
                output[j++] = 'r';
              }
            break;
          case '\t':
            if (j + 2 < max_len)
              {
                output[j++] = '\\';
                output[j++] = 't';
              }
            break;
          default:
            output[j++] = input[i];
            break;
        }
      i++;
    }

  output[j] = '\0';
  return OK;
}

static int llm_append(char *buf, size_t buf_size, size_t *offset,
                      const char *format, ...)
{
  va_list args;
  int written;

  if (*offset >= buf_size)
    {
      return -ENOSPC;
    }

  va_start(args, format);
  written = vsnprintf(buf + *offset, buf_size - *offset, format, args);
  va_end(args);
  if (written < 0 || (size_t)written >= buf_size - *offset)
    {
      buf[buf_size - 1] = '\0';
      return -ENOSPC;
    }

  *offset += (size_t)written;
  return OK;
}

/**
 * @brief  构建消息JSON数组
 */

static int llm_build_messages_json(llm_context_t *ctx,
                                   const char *user_message,
                                   char *buf, size_t buf_size)
{
  if (ctx == NULL || buf == NULL || buf_size == 0)
    {
      return -EINVAL;
    }

  size_t offset = 0;
  char escaped_content[LLM_MAX_INPUT_LENGTH * 2];
  int ret;

  buf[0] = '\0';

  /* 添加系统提示词 */

  if (ctx->config.system_prompt[0] != '\0')
    {
      ret = llm_escape_json_string(ctx->config.system_prompt,
                                   escaped_content,
                                   sizeof(escaped_content));
      if (ret < 0)
        {
          return ret;
        }

      ret = llm_append(buf, buf_size, &offset, JSON_TEMPLATE_MESSAGE,
                       g_role_names[LLM_ROLE_SYSTEM], escaped_content);
      if (ret < 0)
        {
          return ret;
        }

      if (ctx->history.count > 0 || user_message != NULL)
        {
          if (llm_append(buf, buf_size, &offset, ",") < 0)
            {
              return -ENOSPC;
            }
        }
    }

  /* 添加历史消息 */

  int start_idx = ctx->history.head;

  for (int i = 0; i < ctx->history.count; i++)
    {
      int idx = (start_idx + i) % LLM_MAX_HISTORY_SIZE;
      llm_message_t *msg = &ctx->history.messages[idx];

      ret = llm_escape_json_string(msg->content, escaped_content,
                                   sizeof(escaped_content));
      if (ret < 0)
        {
          return ret;
        }

      ret = llm_append(buf, buf_size, &offset, JSON_TEMPLATE_MESSAGE,
                       g_role_names[msg->role], escaped_content);
      if (ret < 0)
        {
          return ret;
        }

      /* 添加逗号分隔 */

      if (i < ctx->history.count - 1 || user_message != NULL)
        {
          if (llm_append(buf, buf_size, &offset, ",") < 0)
            {
              return -ENOSPC;
            }
        }
    }

  /* 添加当前用户消息 */

  if (user_message != NULL)
    {
      ret = llm_escape_json_string(user_message, escaped_content,
                                   sizeof(escaped_content));
      if (ret < 0)
        {
          return ret;
        }

      ret = llm_append(buf, buf_size, &offset, JSON_TEMPLATE_MESSAGE,
                       g_role_names[LLM_ROLE_USER], escaped_content);
      if (ret < 0)
        {
          return ret;
        }
    }

  return OK;
}

/**
 * @brief  添加消息到历史
 */

static void llm_add_to_history(llm_context_t *ctx,
                               llm_role_t role,
                               const char *content)
{
  if (ctx == NULL || content == NULL)
    {
      return;
    }

  /* 如果历史已满，覆盖最旧的消息 */

  if (ctx->history.count >= LLM_MAX_HISTORY_SIZE)
    {
      ctx->history.head = (ctx->history.head + 1) % LLM_MAX_HISTORY_SIZE;
    }
  else
    {
      ctx->history.count++;
    }

  /* 写入消息 */

  int idx = ctx->history.tail;
  ctx->history.messages[idx].role = role;
  strncpy(ctx->history.messages[idx].content, content,
          LLM_MAX_INPUT_LENGTH - 1);
  ctx->history.messages[idx].content[LLM_MAX_INPUT_LENGTH - 1] = '\0';
  ctx->history.messages[idx].timestamp = llm_get_timestamp();

  /* 更新尾部指针 */

  ctx->history.tail = (ctx->history.tail + 1) % LLM_MAX_HISTORY_SIZE;

  LLM_DEBUG("添加历史消息: role=%s, len=%zu",
            g_role_names[role], strlen(content));
}

#ifdef CONFIG_HELLO_APP_LLM_AI_AGENT
static void llm_ai_agent_callback(int status, const char *text, void *cookie)
{
  llm_context_t *ctx = (llm_context_t *)cookie;

  if (ctx == NULL || !ctx->backend_sem_valid)
    {
      return;
    }

  ctx->backend_reply_status = status;
  if (text != NULL)
    {
      strncpy(ctx->backend_reply, text, sizeof(ctx->backend_reply) - 1);
      ctx->backend_reply[sizeof(ctx->backend_reply) - 1] = '\0';
    }
  else
    {
      ctx->backend_reply[0] = '\0';
    }

  sem_post(&ctx->backend_sem);
}

static int llm_do_ai_agent_request(llm_context_t *ctx, const char *text,
                                   char *response, size_t response_size)
{
  velaclaw_ask_req_t req;
  struct timespec deadline;
  int ret;

  if (ctx->backend_client == NULL)
    {
      ctx->backend_client = velaclaw_client_open("elder_companion");
      if (ctx->backend_client == NULL)
        {
          fprintf(stderr, "[LLM] ai_agent未运行，请先执行 ai_agent &\n");
          return -ENOTCONN;
        }
    }

  while (sem_trywait(&ctx->backend_sem) == 0)
    {
    }

  ctx->backend_reply[0] = '\0';
  ctx->backend_reply_status = -EIO;
  req.text = text;
  req.timeout_ms = LLM_HTTP_TIMEOUT_MS;

  ret = velaclaw_ask((velaclaw_client_t *)ctx->backend_client, &req,
                     llm_ai_agent_callback, ctx);
  if (ret < 0)
    {
      return ret;
    }

  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += LLM_HTTP_TIMEOUT_MS / 1000;
  deadline.tv_nsec += (LLM_HTTP_TIMEOUT_MS % 1000) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L)
    {
      deadline.tv_sec++;
      deadline.tv_nsec -= 1000000000L;
    }

  do
    {
      ret = sem_timedwait(&ctx->backend_sem, &deadline);
    }
  while (ret < 0 && errno == EINTR && !ctx->request_cancel);

  if (ret < 0)
    {
      return errno == ETIMEDOUT ? -ETIMEDOUT : -errno;
    }
  if (ctx->backend_reply_status < 0)
    {
      return ctx->backend_reply_status;
    }
  if (ctx->backend_reply[0] == '\0')
    {
      return -ENODATA;
    }

  strncpy(response, ctx->backend_reply, response_size - 1);
  response[response_size - 1] = '\0';
  return OK;
}
#endif

/**
 * @brief  执行HTTP请求
 * @note   实际实现需要使用NuttX的网络接口
 */

#ifndef CONFIG_HELLO_APP_LLM_AI_AGENT
static int llm_do_http_request(llm_context_t *ctx,
                               const char *url,
                               const char *api_key,
                               const char *body,
                               llm_response_t *response)
{
  if (ctx == NULL || url == NULL || body == NULL || response == NULL)
    {
      return -EINVAL;
    }

  (void)api_key;

  LLM_DEBUG("发送HTTP请求到: %s", url);
  LLM_DEBUG("请求体长度: %zu", strlen(body));

  /* TODO: 使用NuttX + OpenSSL实现HTTPS请求 */
  /*
   * 步骤:
   * 1. 解析URL获取主机名和端口
   * 2. 创建socket连接
   * 3. 创建SSL上下文并连接
   * 4. 构建HTTP请求头
   * 5. 发送请求体
   * 6. 接收响应
   * 7. 解析响应
   */

  /* 示例代码框架 */

  /*
  SSL_CTX *ssl_ctx = SSL_CTX_new(TLS_client_method());
  int sock = socket(AF_INET, SOCK_STREAM, 0);

  struct hostent *host = gethostbyname(hostname);
  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(443);
  addr.sin_addr = *((struct in_addr *)host->h_addr);

  connect(sock, (struct sockaddr *)&addr, sizeof(addr));

  SSL *ssl = SSL_new(ssl_ctx);
  SSL_set_fd(ssl, sock);
  SSL_connect(ssl);

  // 构建请求
  char request[4096];
  snprintf(request, sizeof(request),
    "POST %s HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Content-Type: application/json\r\n"
    "Authorization: Bearer %s\r\n"
    "Content-Length: %zu\r\n"
    "Connection: close\r\n"
    "\r\n"
    "%s",
    path, hostname, api_key, strlen(body), body);

  SSL_write(ssl, request, strlen(request));

  // 接收响应
  char buf[LLM_RESPONSE_BUF_SIZE];
  int len = SSL_read(ssl, buf, sizeof(buf) - 1);
  buf[len] = '\0';

  // 解析HTTP响应
  // ...

  SSL_free(ssl);
  close(sock);
  SSL_CTX_free(ssl_ctx);
  */

  /* 模拟响应 */

  sleep(1); /* 模拟网络延迟 */

  /* 模拟AI回复 */

  const char *mock_response = "{"
    "\"choices\":[{"
    "\"message\":{"
    "\"role\":\"assistant\","
    "\"content\":\"您好！我是智爱陪伴机器人，很高兴为您服务。请问有什么可以帮助您的吗？\""
    "},"
    "\"finish_reason\":\"stop\""
    "}],"
    "\"usage\":{"
    "\"prompt_tokens\":50,"
    "\"completion_tokens\":30,"
    "\"total_tokens\":80"
    "}"
    "}";

  response->status_code = 200;
  response->body_len = strlen(mock_response);
  response->body = (char *)malloc(response->body_len + 1);
  if (response->body == NULL)
    {
      return -ENOMEM;
    }

  strcpy(response->body, mock_response);

  LLM_DEBUG("HTTP响应: status=%d, body_len=%zu",
            response->status_code, response->body_len);

  return OK;
}

/**
 * @brief  解析API响应
 */

static int llm_parse_api_response(llm_context_t *ctx,
                                  llm_response_t *http_response,
                                  char *content, size_t content_size)
{
  if (ctx == NULL || http_response == NULL || content == NULL)
    {
      return -EINVAL;
    }

  /* TODO: 使用JSON解析库解析响应 */
  /* 这里简化处理，直接从模拟响应中提取内容 */

  /* 查找content字段 */

  const char *content_start = strstr(http_response->body, "\"content\":\"");
  if (content_start == NULL)
    {
      LLM_DEBUG("未找到content字段");
      return -ENOENT;
    }

  content_start += strlen("\"content\":\"");

  /* 查找结束引号 */

  const char *content_end = strchr(content_start, '\"');
  if (content_end == NULL)
    {
      LLM_DEBUG("content格式错误");
      return -EINVAL;
    }

  /* 复制内容 */

  size_t len = content_end - content_start;
  if (len >= content_size)
    {
      len = content_size - 1;
    }

  strncpy(content, content_start, len);
  content[len] = '\0';

  /* 反转义JSON字符串 */

  /* TODO: 实现完整的JSON字符串反转义 */

  LLM_DEBUG("解析AI回复: len=%zu", len);

  return OK;
}
#endif

/**
 * @brief  发送到API
 */

static int llm_send_to_api(llm_context_t *ctx,
                           const char *text,
                           llm_stream_cb_t stream_cb,
                           llm_complete_cb_t complete_cb,
                           void *user_data)
{
  if (ctx == NULL || text == NULL)
    {
      return -EINVAL;
    }

  int ret;
  char response_content[LLM_MAX_OUTPUT_LENGTH];

#ifdef CONFIG_HELLO_APP_LLM_AI_AGENT
  ret = llm_do_ai_agent_request(ctx, text, response_content,
                                sizeof(response_content));
#else
  char request_json[4096];

  ret = llm_build_request_json(ctx, text, request_json, sizeof(request_json));
  if (ret < 0)
    {
      LLM_DEBUG("构建请求JSON失败: %d", ret);
      return ret;
    }

  LLM_DEBUG("请求JSON:\n%s", request_json);

  /* 发送HTTP请求 */

  llm_response_t http_response;
  memset(&http_response, 0, sizeof(http_response));

  ret = llm_do_http_request(ctx, ctx->config.api_url,
                            ctx->config.api_key,
                            request_json, &http_response);
  if (ret < 0)
    {
      LLM_DEBUG("HTTP请求失败: %d", ret);

      if (complete_cb)
        {
          complete_cb(NULL, ret, user_data);
        }

      return ret;
    }

  /* 检查HTTP状态码 */

  if (http_response.status_code != 200)
    {
      LLM_DEBUG("HTTP错误: status=%d", http_response.status_code);

      if (http_response.body)
        {
          free(http_response.body);
        }

      if (complete_cb)
        {
          complete_cb(NULL, -http_response.status_code, user_data);
        }

      return -http_response.status_code;
    }

  /* 解析响应 */

  ret = llm_parse_api_response(ctx, &http_response,
                               response_content, sizeof(response_content));

  if (http_response.body)
    {
      free(http_response.body);
    }

  if (ret < 0)
    {
      LLM_DEBUG("解析响应失败: %d", ret);

      if (complete_cb)
        {
          complete_cb(NULL, ret, user_data);
        }

      return ret;
    }
#endif

  if (ret < 0)
    {
      if (complete_cb != NULL)
        {
          complete_cb(NULL, ret, user_data);
        }

      return ret;
    }

  if (ctx->request_cancel)
    {
      return -ECANCELED;
    }

  /* 添加到历史 */

  if (ctx->config.enable_history)
    {
      llm_add_to_history(ctx, LLM_ROLE_USER, text);
      llm_add_to_history(ctx, LLM_ROLE_ASSISTANT, response_content);
    }

  if (stream_cb != NULL)
    {
      stream_cb(response_content, user_data);
    }

  /* 调用完成回调 */

  if (complete_cb)
    {
      complete_cb(response_content, 0, user_data);
    }

  LLM_DEBUG("AI回复: %s", response_content);

  return OK;
}

/**
 * @brief  请求线程函数
 */

static void *llm_request_thread(void *arg)
{
  llm_context_t *ctx = (llm_context_t *)arg;

  LLM_DEBUG("请求线程启动");

  /* 发送到API */

  int ret = llm_send_to_api(ctx,
                            ctx->current_request.text,
                            ctx->current_request.stream_cb,
                            ctx->current_request.complete_cb,
                            ctx->current_request.user_data);

  if (ret < 0)
    {
      ctx->state = LLM_STATE_ERROR;
      LLM_DEBUG("请求失败: %d", ret);
    }
  else
    {
      ctx->state = LLM_STATE_DONE;
    }

  LLM_DEBUG("请求线程退出");

  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/**
 * @brief  初始化LLM模块
 */

int llm_init(llm_context_t *ctx, const llm_config_t *config)
{
  if (ctx == NULL)
    {
      return -EINVAL;
    }

  LLM_DEBUG("初始化LLM模块");

  /* 清零上下文 */

  memset(ctx, 0, sizeof(llm_context_t));

  /* 设置默认配置 */

  if (config != NULL)
    {
      memcpy(&ctx->config, config, sizeof(llm_config_t));
    }
  else
    {
      /* 默认配置 */

      strncpy(ctx->config.api_url, LLM_DEFAULT_API_URL,
              sizeof(ctx->config.api_url) - 1);
      strncpy(ctx->config.model, LLM_DEFAULT_MODEL,
              sizeof(ctx->config.model) - 1);
      ctx->config.max_tokens = LLM_DEFAULT_MAX_TOKENS;
      ctx->config.temperature = LLM_DEFAULT_TEMPERATURE;
      ctx->config.enable_stream = false;
      ctx->config.enable_history = true;

      /* 默认系统提示词 - 老人陪伴角色 */

      strncpy(ctx->config.system_prompt,
              "你是一个温暖、耐心、有爱心的AI陪伴机器人，专门为老年人提供陪伴服务。"
              "你的特点是：1）说话温和亲切；2）回答简洁易懂；3）善于倾听和关心；"
              "4）能提供健康建议和生活提醒；5）能讲笑话、故事来陪伴老人。"
              "请用友善、关心的语气回复。",
              sizeof(ctx->config.system_prompt) - 1);
    }

  ctx->config.api_url[sizeof(ctx->config.api_url) - 1] = '\0';
  ctx->config.api_key[sizeof(ctx->config.api_key) - 1] = '\0';
  ctx->config.model[sizeof(ctx->config.model) - 1] = '\0';
  ctx->config.system_prompt[sizeof(ctx->config.system_prompt) - 1] = '\0';

  /* 分配缓冲区 */

  ctx->request_buf_size = 4096;
  ctx->request_buf = (char *)malloc(ctx->request_buf_size);
  if (ctx->request_buf == NULL)
    {
      LLM_DEBUG("分配请求缓冲区失败");
      return -ENOMEM;
    }

  ctx->response_buf_size = LLM_RESPONSE_BUF_SIZE;
  ctx->response_buf = (char *)malloc(ctx->response_buf_size);
  if (ctx->response_buf == NULL)
    {
      LLM_DEBUG("分配响应缓冲区失败");
      free(ctx->request_buf);
      return -ENOMEM;
    }

#ifdef CONFIG_HELLO_APP_LLM_AI_AGENT
  if (sem_init(&ctx->backend_sem, 0, 0) < 0)
    {
      int error = errno;
      free(ctx->response_buf);
      free(ctx->request_buf);
      return -error;
    }

  ctx->backend_sem_valid = true;
#endif

  /* 初始化历史 */

  ctx->history.count = 0;
  ctx->history.head = 0;
  ctx->history.tail = 0;

  ctx->state = LLM_STATE_IDLE;
  ctx->initialized = true;

  LLM_DEBUG("LLM模块初始化完成");
  LLM_DEBUG("  API: %s", ctx->config.api_url);
  LLM_DEBUG("  模型: %s", ctx->config.model);
  LLM_DEBUG("  最大token: %d", ctx->config.max_tokens);

  return OK;
}

/**
 * @brief  反初始化LLM模块
 */

void llm_deinit(llm_context_t *ctx)
{
  if (ctx == NULL || !ctx->initialized)
    {
      return;
    }

  LLM_DEBUG("反初始化LLM模块");

  /* 取消当前请求 */

  llm_cancel_request(ctx);

#ifdef CONFIG_HELLO_APP_LLM_AI_AGENT
  if (ctx->backend_client != NULL)
    {
      velaclaw_client_close((velaclaw_client_t *)ctx->backend_client);
      ctx->backend_client = NULL;
    }

  if (ctx->backend_sem_valid)
    {
      sem_destroy(&ctx->backend_sem);
      ctx->backend_sem_valid = false;
    }
#endif

  /* 释放缓冲区 */

  if (ctx->request_buf != NULL)
    {
      free(ctx->request_buf);
      ctx->request_buf = NULL;
    }

  if (ctx->response_buf != NULL)
    {
      free(ctx->response_buf);
      ctx->response_buf = NULL;
    }

  ctx->initialized = false;
  ctx->state = LLM_STATE_UNINIT;

  LLM_DEBUG("LLM模块已反初始化");
}

/**
 * @brief  发送文本请求
 */

int llm_send_text(llm_context_t *ctx,
                  const char *text,
                  llm_stream_cb_t stream_cb,
                  llm_complete_cb_t complete_cb,
                  void *user_data)
{
  if (ctx == NULL || !ctx->initialized || text == NULL)
    {
      return -EINVAL;
    }

  if (ctx->state == LLM_STATE_REQUESTING ||
      ctx->state == LLM_STATE_STREAMING)
    {
      LLM_DEBUG("已有请求在处理中");
      return -EBUSY;
    }

  size_t text_len = strlen(text);
  if (text_len == 0 || text_len >= ctx->request_buf_size)
    {
      LLM_DEBUG("输入文本为空");
      return -EINVAL;
    }

  LLM_DEBUG("发送文本请求: len=%zu", text_len);

  if (ctx->request_thread_valid)
    {
      pthread_join(ctx->request_thread, NULL);
      ctx->request_thread_valid = false;
    }

  /* 保存请求参数 */

  ctx->current_request.type = LLM_REQUEST_TEXT;
  memcpy(ctx->request_buf, text, text_len + 1);
  ctx->current_request.text = ctx->request_buf;
  ctx->current_request.stream_cb = stream_cb;
  ctx->current_request.complete_cb = complete_cb;
  ctx->current_request.user_data = user_data;
  ctx->request_cancel = false;
  ctx->state = LLM_STATE_REQUESTING;

  /* 启动请求线程 */

  int ret = pthread_create(&ctx->request_thread, NULL,
                           llm_request_thread, ctx);
  if (ret != 0)
    {
      LLM_DEBUG("创建请求线程失败: %d", ret);
      ctx->state = LLM_STATE_ERROR;
      return -ret;
    }

  ctx->request_thread_valid = true;

  return OK;
}

/**
 * @brief  发送音频请求
 */

int llm_send_audio(llm_context_t *ctx,
                   const int16_t *audio_data, size_t frames,
                   llm_stream_cb_t stream_cb,
                   llm_complete_cb_t complete_cb,
                   void *user_data)
{
  if (ctx == NULL || audio_data == NULL || frames == 0)
    {
      return -EINVAL;
    }

  LLM_DEBUG("收到音频数据: frames=%zu", frames);

  /* TODO: 实现ASR（自动语音识别） */
  /* 1. 将音频数据发送到ASR服务 */
  /* 2. 获取识别结果文本 */
  /* 3. 调用llm_send_text发送文本 */

  /* 模拟ASR结果 */

  const char *asr_result = "你好，请问今天天气怎么样？";

  LLM_DEBUG("ASR识别结果: %s", asr_result);

  /* 发送文本请求 */

  return llm_send_text(ctx, asr_result, stream_cb, complete_cb, user_data);
}

/**
 * @brief  取消当前请求
 */

void llm_cancel_request(llm_context_t *ctx)
{
  if (ctx == NULL)
    {
      return;
    }

  if (ctx->request_thread_valid)
    {
      LLM_DEBUG("取消当前请求");

      ctx->request_cancel = true;

      /* 等待请求线程退出 */

      pthread_join(ctx->request_thread, NULL);
      ctx->request_thread_valid = false;

      ctx->state = LLM_STATE_IDLE;
    }
}

/**
 * @brief  清空对话历史
 */

void llm_clear_history(llm_context_t *ctx)
{
  if (ctx == NULL)
    {
      return;
    }

  LLM_DEBUG("清空对话历史");

  ctx->history.count = 0;
  ctx->history.head = 0;
  ctx->history.tail = 0;
}

/**
 * @brief  设置系统提示词
 */

int llm_set_system_prompt(llm_context_t *ctx, const char *prompt)
{
  if (ctx == NULL || prompt == NULL)
    {
      return -EINVAL;
    }

  LLM_DEBUG("设置系统提示词: len=%zu", strlen(prompt));

  strncpy(ctx->config.system_prompt, prompt,
          sizeof(ctx->config.system_prompt) - 1);
  ctx->config.system_prompt[sizeof(ctx->config.system_prompt) - 1] = '\0';

  return OK;
}

/**
 * @brief  获取对话历史消息数
 */

int llm_get_history_count(llm_context_t *ctx)
{
  if (ctx == NULL)
    {
      return 0;
    }

  return ctx->history.count;
}

/**
 * @brief  获取最后一次AI回复
 */

const char *llm_get_last_response(llm_context_t *ctx)
{
  if (ctx == NULL || ctx->history.count == 0)
    {
      return NULL;
    }

  /* 获取最后一条助手消息 */

  int idx = (ctx->history.tail - 1 + LLM_MAX_HISTORY_SIZE) %
            LLM_MAX_HISTORY_SIZE;

  for (int i = 0; i < ctx->history.count; i++)
    {
      if (ctx->history.messages[idx].role == LLM_ROLE_ASSISTANT)
        {
          return ctx->history.messages[idx].content;
        }
      idx = (idx - 1 + LLM_MAX_HISTORY_SIZE) % LLM_MAX_HISTORY_SIZE;
    }

  return NULL;
}

/**
 * @brief  获取LLM模块状态
 */

llm_state_t llm_get_state(llm_context_t *ctx)
{
  if (ctx == NULL)
    {
      return LLM_STATE_UNINIT;
    }

  return ctx->state;
}

/**
 * @brief  获取状态名称字符串
 */

const char *llm_get_state_name(llm_state_t state)
{
  if (state >= LLM_STATE_UNINIT && state <= LLM_STATE_ERROR)
    {
      return g_state_names[state];
    }

  return "UNKNOWN";
}

/**
 * @brief  设置API配置
 */

int llm_set_api_config(llm_context_t *ctx,
                       const char *api_url,
                       const char *api_key,
                       const char *model)
{
  if (ctx == NULL)
    {
      return -EINVAL;
    }

  if (api_url != NULL)
    {
      strncpy(ctx->config.api_url, api_url,
              sizeof(ctx->config.api_url) - 1);
      ctx->config.api_url[sizeof(ctx->config.api_url) - 1] = '\0';
    }

  if (api_key != NULL)
    {
      strncpy(ctx->config.api_key, api_key,
              sizeof(ctx->config.api_key) - 1);
      ctx->config.api_key[sizeof(ctx->config.api_key) - 1] = '\0';
    }

  if (model != NULL)
    {
      strncpy(ctx->config.model, model,
              sizeof(ctx->config.model) - 1);
      ctx->config.model[sizeof(ctx->config.model) - 1] = '\0';
    }

  LLM_DEBUG("更新API配置");

  return OK;
}

/**
 * @brief  测试API连接
 */

int llm_test_connection(llm_context_t *ctx)
{
  if (ctx == NULL || !ctx->initialized)
    {
      return -EINVAL;
    }

  LLM_DEBUG("测试API连接");

  /* 发送测试请求 */

  return llm_send_text(ctx, "ping", NULL, NULL, NULL);
}

/**
 * @brief  构建请求JSON
 */

int llm_build_request_json(llm_context_t *ctx,
                           const char *text,
                           char *buf, size_t buf_size)
{
  if (ctx == NULL || text == NULL || buf == NULL)
    {
      return -EINVAL;
    }

  /* 构建消息数组JSON */

  char messages_json[4096];
  int ret = llm_build_messages_json(ctx, text,
                                    messages_json, sizeof(messages_json));
  if (ret < 0)
    {
      return ret;
    }

  /* 构建完整请求JSON */

  ret = snprintf(buf, buf_size, JSON_TEMPLATE_REQUEST,
                 ctx->config.model,
                 messages_json,
                 (unsigned long)ctx->config.max_tokens,
                 ctx->config.temperature,
                 ctx->config.enable_stream ? "true" : "false");
  if (ret < 0 || (size_t)ret >= buf_size)
    {
      return -ENOSPC;
    }

  return OK;
}

/**
 * @brief  解析JSON响应（公开接口）
 */

int llm_parse_response(const char *json, llm_response_t *response)
{
  if (json == NULL || response == NULL)
    {
      return -EINVAL;
    }

  /* TODO: 使用JSON解析库实现 */

  return -ENOSYS;
}
