/****************************************************************************
 * AI Companion State Machine Header
 * 智爱陪伴 - AI老人陪伴守护终端
 * 状态机模块 - 管理机器人运行状态
 ****************************************************************************/

#ifndef __AI_STATE_MACHINE_H
#define __AI_STATE_MACHINE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 状态机调试日志宏 */
#ifdef CONFIG_DEBUG_AI_COMPANION
#  define SM_DEBUG(...) do { printf("[SM] "); printf(__VA_ARGS__); \
                             printf("\n"); } while (0)
#else
#  define SM_DEBUG(...) do { } while (0)
#endif

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* 机器人运行状态枚举 */
typedef enum
{
  SM_STATE_IDLE = 0,        /* 待机状态 - 低功耗等待 */
  SM_STATE_LISTENING,       /* 语音监听状态 - 监听用户语音 */
  SM_STATE_AI_TALKING,      /* AI对话状态 - 与大模型交互中 */
  SM_STATE_CARE_REMIND,     /* 主动提醒状态 - 定时提醒/问候 */
  SM_STATE_ALARM,           /* 异常报警状态 - 检测到异常 */
  SM_STATE_MAX              /* 状态数量上限 */
} sm_state_t;

/* 触发事件枚举 */
typedef enum
{
  SM_EVENT_NONE = 0,        /* 无事件 */
  SM_EVENT_WAKEUP,          /* 唤醒事件 - 语音唤醒词触发 */
  SM_EVENT_VOICE_DETECTED,  /* 检测到语音输入 */
  SM_EVENT_VOICE_COMPLETE,  /* 语音输入完成 */
  SM_EVENT_AI_RESPONSE,     /* 收到AI回复 */
  SM_EVENT_AI_ERROR,        /* AI处理错误 */
  SM_EVENT_CARE_TIMER,      /* 主动关怀定时触发 */
  SM_EVENT_ALARM_DETECTED,  /* 异常声音检测到 */
  SM_EVENT_ALARM_CONFIRMED, /* 异常确认（需人工确认） */
  SM_EVENT_ALARM_CLEARED,   /* 异常解除 */
  SM_EVENT_TIMEOUT,         /* 超时事件 */
  SM_EVENT_SLEEP,           /* 进入休眠 */
  SM_EVENT_MAX              /* 事件数量上限 */
} sm_event_t;

/* 状态处理函数原型 */
typedef void (*sm_state_handler_t)(void *ctx);

/* 状态转换函数原型 - 返回下一个状态 */
typedef sm_state_t (*sm_transition_handler_t)(void *ctx, sm_event_t event);

/* 状态机配置结构 */
typedef struct
{
  sm_state_handler_t     enter_func;      /* 进入状态回调 */
  sm_state_handler_t     exit_func;       /* 退出状态回调 */
  sm_transition_handler_t transition_func; /* 状态转换处理 */
} sm_state_config_t;

/* 状态机上下文结构 */
typedef struct
{
  sm_state_t          current_state;   /* 当前状态 */
  sm_state_t          prev_state;      /* 上一个状态 */
  sm_event_t          last_event;      /* 最后处理的事件 */
  uint32_t            state_enter_tick; /* 进入当前状态的时间戳 */
  uint32_t            timeout_ms;      /* 当前状态超时时间(ms), 0表示不超时 */
  uint32_t            state_timeouts[SM_STATE_MAX]; /* 每个实例的状态超时 */
  bool                initialized;     /* 状态机是否已初始化 */
  void               *user_data;       /* 用户自定义数据指针 */

  /* 状态处理函数表 */
  sm_state_config_t   state_table[SM_STATE_MAX];
} sm_context_t;

/* 状态回调上下文 - 传递给回调函数的额外信息 */
typedef struct
{
  sm_state_t  state;
  sm_event_t  event;
  sm_state_t  next_state;
  uint32_t    timestamp;
} sm_callback_ctx_t;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/**
 * @brief  初始化状态机
 * @param  ctx: 状态机上下文指针
 * @return 0成功, 负值失败
 */

int sm_init(sm_context_t *ctx);

/**
 * @brief  反初始化状态机
 * @param  ctx: 状态机上下文指针
 */

void sm_deinit(sm_context_t *ctx);

/**
 * @brief  处理事件
 * @param  ctx: 状态机上下文指针
 * @param  event: 要处理的事件
 * @return 0成功, 负值失败
 */

int sm_handle_event(sm_context_t *ctx, sm_event_t event);

/**
 * @brief  状态机主循环处理（检查超时等）
 * @param  ctx: 状态机上下文指针
 */

void sm_run(sm_context_t *ctx);

/**
 * @brief  获取当前状态
 * @param  ctx: 状态机上下文指针
 * @return 当前状态枚举值
 */

sm_state_t sm_get_state(sm_context_t *ctx);

/**
 * @brief  获取状态名称字符串（用于调试）
 * @param  state: 状态枚举值
 * @return 状态名称字符串
 */

const char *sm_get_state_name(sm_state_t state);

/**
 * @brief  获取事件名称字符串（用于调试）
 * @param  event: 事件枚举值
 * @return 事件名称字符串
 */

const char *sm_get_event_name(sm_event_t event);

/**
 * @brief  注册状态处理回调
 * @param  ctx: 状态机上下文指针
 * @param  state: 要注册的状态
 * @param  enter: 进入状态回调
 * @param  exit: 退出状态回调
 * @param  transition: 状态转换处理回调
 * @return 0成功, 负值失败
 */

int sm_register_state(sm_context_t *ctx,
                      sm_state_t state,
                      sm_state_handler_t enter,
                      sm_state_handler_t exit,
                      sm_transition_handler_t transition);

/**
 * @brief  设置状态超时时间
 * @param  ctx: 状态机上下文指针
 * @param  state: 要设置的状态
 * @param  timeout_ms: 超时时间(毫秒), 0表示不超时
 */

void sm_set_timeout(sm_context_t *ctx, sm_state_t state, uint32_t timeout_ms);

/**
 * @brief  强制切换状态（绕过转换处理）
 * @param  ctx: 状态机上下文指针
 * @param  new_state: 目标状态
 */

void sm_force_state(sm_context_t *ctx, sm_state_t new_state);

/**
 * @brief  设置用户数据
 * @param  ctx: 状态机上下文指针
 * @param  user_data: 用户数据指针
 */

void sm_set_user_data(sm_context_t *ctx, void *user_data);

/**
 * @brief  获取用户数据
 * @param  ctx: 状态机上下文指针
 * @return 用户数据指针
 */

void *sm_get_user_data(sm_context_t *ctx);

#endif /* __AI_STATE_MACHINE_H */
