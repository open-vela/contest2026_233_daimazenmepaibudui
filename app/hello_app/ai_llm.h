/****************************************************************************
 * AI LLM (Large Language Model) Module Header
 * 智爱陪伴 - AI老人陪伴守护终端
 * AI对话模块 - 大模型API调用与会话管理
 ****************************************************************************/

#ifndef __AI_LLM_H
#define __AI_LLM_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <pthread.h>
#include <semaphore.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* AI对话调试日志宏 */
#ifdef CONFIG_DEBUG_AI_LLM
#  define LLM_DEBUG(...) do { printf("[LLM] "); printf(__VA_ARGS__); \
                              printf("\n"); } while (0)
#else
#  define LLM_DEBUG(...) do { } while (0)
#endif

/* API配置默认值 */
#define LLM_DEFAULT_API_URL       "ai-agent://router"
#define LLM_DEFAULT_MODEL         "mimo-v2.5"
#define LLM_DEFAULT_MAX_TOKENS    500
#define LLM_DEFAULT_TEMPERATURE   0.7f

/* 会话配置 */
#define LLM_MAX_HISTORY_SIZE      10      /* 最大历史对话轮数 */
#define LLM_MAX_INPUT_LENGTH      1024    /* 最大输入文本长度 */
#define LLM_MAX_OUTPUT_LENGTH     2048    /* 最大输出文本长度 */

/* HTTP配置 */
#define LLM_HTTP_TIMEOUT_MS       30000   /* HTTP请求超时30秒 */
#define LLM_HTTP_RETRY_COUNT      3       /* 重试次数 */
#define LLM_HTTP_RETRY_DELAY_MS   1000    /* 重试间隔 */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* LLM模块状态 */
typedef enum
{
  LLM_STATE_UNINIT = 0,     /* 未初始化 */
  LLM_STATE_IDLE,           /* 空闲状态 */
  LLM_STATE_REQUESTING,     /* 正在请求 */
  LLM_STATE_STREAMING,      /* 流式接收中 */
  LLM_STATE_DONE,           /* 完成 */
  LLM_STATE_ERROR           /* 错误状态 */
} llm_state_t;

/* 消息角色 */
typedef enum
{
  LLM_ROLE_SYSTEM = 0,      /* 系统消息 */
  LLM_ROLE_USER,            /* 用户消息 */
  LLM_ROLE_ASSISTANT        /* AI助手消息 */
} llm_role_t;

/* LLM提供商类型 */
typedef enum
{
  LLM_PROVIDER_OPENAI = 0,  /* OpenAI */
  LLM_PROVIDER_CLAUDE,      /* Claude */
  LLM_PROVIDER_CUSTOM       /* 自定义API */
} llm_provider_t;

/* 请求类型 */
typedef enum
{
  LLM_REQUEST_TEXT = 0,     /* 纯文本请求 */
  LLM_REQUEST_AUDIO,        /* 音频请求（ASR后转换为文本） */
  LLM_REQUEST_MULTIMODAL    /* 多模态请求（文本+图像等） */
} llm_request_type_t;

/* 流式回调 */
typedef void (*llm_stream_cb_t)(const char *token, void *user_data);

/* 请求完成回调 */
typedef void (*llm_complete_cb_t)(const char *response, int error,
                                  void *user_data);

/* 对话消息结构 */
typedef struct
{
  llm_role_t  role;                /* 角色 */
  char        content[LLM_MAX_INPUT_LENGTH]; /* 消息内容 */
  uint32_t    timestamp;           /* 时间戳 */
} llm_message_t;

/* 会话历史 */
typedef struct
{
  llm_message_t messages[LLM_MAX_HISTORY_SIZE]; /* 消息数组 */
  int           count;                          /* 消息数量 */
  int           head;                           /* 循环队列头部 */
  int           tail;                           /* 循环队列尾部 */
} llm_history_t;

/* LLM配置结构 */
typedef struct
{
  llm_provider_t provider;         /* API提供商 */
  char           api_url[256];     /* API地址 */
  char           api_key[128];     /* API密钥 */
  char           model[64];        /* 模型名称 */
  uint32_t       max_tokens;       /* 最大输出token数 */
  float          temperature;      /* 温度参数 */
  bool           enable_stream;    /* 启用流式响应 */
  bool           enable_history;   /* 启用会话历史 */
  char           system_prompt[512]; /* 系统提示词 */
} llm_config_t;

/* 请求参数 */
typedef struct
{
  llm_request_type_t type;         /* 请求类型 */
  const char        *text;         /* 文本内容 */
  const int16_t     *audio_data;   /* 音频数据 */
  size_t             audio_frames; /* 音频帧数 */
  llm_stream_cb_t    stream_cb;    /* 流式回调 */
  llm_complete_cb_t  complete_cb;  /* 完成回调 */
  void              *user_data;    /* 用户数据 */
} llm_request_t;

/* HTTP响应结构 */
typedef struct
{
  int         status_code;         /* HTTP状态码 */
  char       *body;                /* 响应体 */
  size_t      body_len;            /* 响应体长度 */
  char       *error;               /* 错误信息 */
} llm_response_t;

/* LLM模块上下文 */
typedef struct
{
  llm_state_t      state;          /* 当前状态 */
  llm_config_t     config;         /* LLM配置 */
  llm_history_t    history;        /* 对话历史 */
  bool             initialized;    /* 是否已初始化 */

  /* 请求相关 */
  llm_request_t    current_request; /* 当前请求 */
  llm_response_t   current_response; /* 当前响应 */

  /* 缓冲区 */
  char            *request_buf;    /* 请求缓冲区 */
  char            *response_buf;   /* 响应缓冲区 */
  size_t           request_buf_size;
  size_t           response_buf_size;

  /* 线程相关 */
  pthread_t        request_thread; /* 请求线程 */
  volatile bool    request_cancel; /* 取消请求标志 */
  bool             request_thread_valid; /* 请求线程需要回收 */

  /* openvela ai_agent本地客户端 */
  void            *backend_client;
  sem_t            backend_sem;
  bool             backend_sem_valid;
  int              backend_reply_status;
  char             backend_reply[LLM_MAX_OUTPUT_LENGTH];

  /* 用户数据 */
  void            *user_data;      /* 用户自定义数据 */
} llm_context_t;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/**
 * @brief  初始化LLM模块
 * @param  ctx: LLM上下文指针
 * @param  config: LLM配置
 * @return 0成功, 负值失败
 */

int llm_init(llm_context_t *ctx, const llm_config_t *config);

/**
 * @brief  反初始化LLM模块
 * @param  ctx: LLM上下文指针
 */

void llm_deinit(llm_context_t *ctx);

/**
 * @brief  发送文本请求
 * @param  ctx: LLM上下文指针
 * @param  text: 用户输入文本
 * @param  stream_cb: 流式回调(可选)
 * @param  complete_cb: 完成回调(可选)
 * @param  user_data: 用户数据
 * @return 0成功, 负值失败
 */

int llm_send_text(llm_context_t *ctx,
                  const char *text,
                  llm_stream_cb_t stream_cb,
                  llm_complete_cb_t complete_cb,
                  void *user_data);

/**
 * @brief  发送音频请求（会先进行ASR转文本）
 * @param  ctx: LLM上下文指针
 * @param  audio_data: 音频数据
 * @param  frames: 帧数
 * @param  stream_cb: 流式回调
 * @param  complete_cb: 完成回调
 * @param  user_data: 用户数据
 * @return 0成功, 负值失败
 */

int llm_send_audio(llm_context_t *ctx,
                   const int16_t *audio_data, size_t frames,
                   llm_stream_cb_t stream_cb,
                   llm_complete_cb_t complete_cb,
                   void *user_data);

/**
 * @brief  取消当前请求
 * @param  ctx: LLM上下文指针
 */

void llm_cancel_request(llm_context_t *ctx);

/**
 * @brief  清空对话历史
 * @param  ctx: LLM上下文指针
 */

void llm_clear_history(llm_context_t *ctx);

/**
 * @brief  添加系统提示词
 * @param  ctx: LLM上下文指针
 * @param  prompt: 系统提示词
 * @return 0成功, 负值失败
 */

int llm_set_system_prompt(llm_context_t *ctx, const char *prompt);

/**
 * @brief  获取对话历史消息数
 * @param  ctx: LLM上下文指针
 * @return 消息数量
 */

int llm_get_history_count(llm_context_t *ctx);

/**
 * @brief  获取最后一次AI回复
 * @param  ctx: LLM上下文指针
 * @return AI回复文本, NULL表示无
 */

const char *llm_get_last_response(llm_context_t *ctx);

/**
 * @brief  获取LLM模块状态
 * @param  ctx: LLM上下文指针
 * @return 状态枚举
 */

llm_state_t llm_get_state(llm_context_t *ctx);

/**
 * @brief  获取状态名称字符串
 * @param  state: 状态枚举
 * @return 状态名称字符串
 */

const char *llm_get_state_name(llm_state_t state);

/**
 * @brief  设置API配置
 * @param  ctx: LLM上下文指针
 * @param  api_url: API地址
 * @param  api_key: API密钥
 * @param  model: 模型名称
 * @return 0成功, 负值失败
 */

int llm_set_api_config(llm_context_t *ctx,
                       const char *api_url,
                       const char *api_key,
                       const char *model);

/**
 * @brief  测试API连接
 * @param  ctx: LLM上下文指针
 * @return 0成功, 负值失败
 */

int llm_test_connection(llm_context_t *ctx);

/**
 * @brief  解析JSON响应（内部使用）
 * @param  json: JSON字符串
 * @param  response: 输出响应结构
 * @return 0成功, 负值失败
 */

int llm_parse_response(const char *json, llm_response_t *response);

/**
 * @brief  构建请求JSON（内部使用）
 * @param  ctx: LLM上下文指针
 * @param  text: 用户输入
 * @param  buf: 输出缓冲区
 * @param  buf_size: 缓冲区大小
 * @return 0成功, 负值失败
 */

int llm_build_request_json(llm_context_t *ctx,
                           const char *text,
                           char *buf, size_t buf_size);

#endif /* __AI_LLM_H */
