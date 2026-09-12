/****************************************************************************
 * AI Companion State Machine Implementation
 * 智爱陪伴 - AI老人陪伴守护终端
 * 状态机模块 - 管理机器人运行状态
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "ai_state_machine.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* 状态进入回调 */
static void sm_idle_enter(void *ctx);
static void sm_listening_enter(void *ctx);
static void sm_ai_talking_enter(void *ctx);
static void sm_care_remind_enter(void *ctx);
static void sm_alarm_enter(void *ctx);

/* 状态退出回调 */
static void sm_idle_exit(void *ctx);
static void sm_listening_exit(void *ctx);
static void sm_ai_talking_exit(void *ctx);
static void sm_care_remind_exit(void *ctx);
static void sm_alarm_exit(void *ctx);

/* 状态转换处理 */
static sm_state_t sm_idle_transition(void *ctx, sm_event_t event);
static sm_state_t sm_listening_transition(void *ctx, sm_event_t event);
static sm_state_t sm_ai_talking_transition(void *ctx, sm_event_t event);
static sm_state_t sm_care_remind_transition(void *ctx, sm_event_t event);
static sm_state_t sm_alarm_transition(void *ctx, sm_event_t event);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 状态名称表 - 用于调试输出 */
static const char *g_state_names[SM_STATE_MAX] =
{
  [SM_STATE_IDLE]        = "IDLE",
  [SM_STATE_LISTENING]   = "LISTENING",
  [SM_STATE_AI_TALKING]  = "AI_TALKING",
  [SM_STATE_CARE_REMIND] = "CARE_REMIND",
  [SM_STATE_ALARM]       = "ALARM"
};

/* 事件名称表 - 用于调试输出 */
static const char *g_event_names[SM_EVENT_MAX] =
{
  [SM_EVENT_NONE]            = "NONE",
  [SM_EVENT_WAKEUP]          = "WAKEUP",
  [SM_EVENT_VOICE_DETECTED]  = "VOICE_DETECTED",
  [SM_EVENT_VOICE_COMPLETE]  = "VOICE_COMPLETE",
  [SM_EVENT_AI_RESPONSE]     = "AI_RESPONSE",
  [SM_EVENT_AI_ERROR]        = "AI_ERROR",
  [SM_EVENT_CARE_TIMER]      = "CARE_TIMER",
  [SM_EVENT_ALARM_DETECTED]  = "ALARM_DETECTED",
  [SM_EVENT_ALARM_CONFIRMED] = "ALARM_CONFIRMED",
  [SM_EVENT_ALARM_CLEARED]   = "ALARM_CLEARED",
  [SM_EVENT_TIMEOUT]         = "TIMEOUT",
  [SM_EVENT_SLEEP]           = "SLEEP"
};

/* 各状态默认超时时间(毫秒) */
static const uint32_t g_default_timeouts[SM_STATE_MAX] =
{
  [SM_STATE_IDLE]        = 0,        /* 待机状态不超时 */
  [SM_STATE_LISTENING]   = 10000,    /* 监听10秒超时 */
  [SM_STATE_AI_TALKING]  = 30000,    /* AI对话30秒超时 */
  [SM_STATE_CARE_REMIND] = 5000,     /* 提醒播放5秒 */
  [SM_STATE_ALARM]       = 60000     /* 报警状态60秒 */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * @brief  获取当前时间戳(毫秒)
 */

static uint32_t sm_get_tick_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/**
 * @brief  执行状态切换
 */

static int sm_switch_state(sm_context_t *ctx, sm_state_t new_state)
{
  if (ctx == NULL || (int)new_state < 0 || new_state >= SM_STATE_MAX ||
      (int)ctx->current_state < 0 || ctx->current_state >= SM_STATE_MAX)
    {
      return -EINVAL;
    }

  sm_state_t old_state = ctx->current_state;

  /* 调用旧状态退出回调 */

  if (ctx->state_table[old_state].exit_func != NULL)
    {
      SM_DEBUG("Exit state: %s", g_state_names[old_state]);
      ctx->state_table[old_state].exit_func(ctx);
    }

  /* 更新状态 */

  ctx->prev_state = old_state;
  ctx->current_state = new_state;
  ctx->state_enter_tick = sm_get_tick_ms();
  ctx->timeout_ms = ctx->state_timeouts[new_state];

  SM_DEBUG("State change: %s -> %s (timeout=%lu ms)",
           g_state_names[old_state],
           g_state_names[new_state],
           (unsigned long)ctx->timeout_ms);

  /* 调用新状态进入回调 */

  if (ctx->state_table[new_state].enter_func != NULL)
    {
      SM_DEBUG("Enter state: %s", g_state_names[new_state]);
      ctx->state_table[new_state].enter_func(ctx);
    }

  return OK;
}

/****************************************************************************
 * State Enter Callbacks - 状态进入回调
 ****************************************************************************/

/**
 * @brief  待机状态进入
 */

static void sm_idle_enter(void *ctx)
{
  (void)ctx;
  SM_DEBUG("进入待机状态: 低功耗模式");
  /* TODO: 通知成员一降低功耗, 关闭不必要的外设 */
  /* TODO: 通知成员三显示待机动画 */

  /* 启动语音唤醒监听（低功耗） */
  /* audio_start_wakeup_listen(); */
}

/**
 * @brief  语音监听状态进入
 */

static void sm_listening_enter(void *ctx)
{
  (void)ctx;
  SM_DEBUG("进入监听状态: 开始采集语音");
  /* TODO: 启动麦克风录音 */
  /* audio_start_recording(); */

  /* TODO: 通知成员三显示监听动画 */
}

/**
 * @brief  AI对话状态进入
 */

static void sm_ai_talking_enter(void *ctx)
{
  (void)ctx;
  SM_DEBUG("进入AI对话状态: 等待云端回复");
  /* TODO: 停止录音, 开始上传音频数据 */
  /* audio_stop_recording(); */
  /* network_send_audio(); */

  /* TODO: 通知成员三显示思考动画 */
}

/**
 * @brief  主动提醒状态进入
 */

static void sm_care_remind_enter(void *ctx)
{
  (void)ctx;
  SM_DEBUG("进入主动提醒状态: 播放提醒内容");
  /* TODO: 生成提醒语音并播放 */
  /* audio_play_reminder(); */

  /* TODO: 通知成员三显示提醒内容 */
}

/**
 * @brief  异常报警状态进入
 */

static void sm_alarm_enter(void *ctx)
{
  (void)ctx;
  SM_DEBUG("进入报警状态: 发送异常通知");
  /* TODO: 立即通知家人/护理人员 */
  /* network_send_alarm(); */

  /* TODO: 通知成员三显示报警界面 */
  /* TODO: 持续录音并上传作为证据 */
}

/****************************************************************************
 * State Exit Callbacks - 状态退出回调
 ****************************************************************************/

static void sm_idle_exit(void *ctx)
{
  (void)ctx;
  SM_DEBUG("退出待机状态");
}

static void sm_listening_exit(void *ctx)
{
  (void)ctx;
  SM_DEBUG("退出监听状态");
  /* TODO: 停止麦克风录音 */
  /* audio_stop_recording(); */
}

static void sm_ai_talking_exit(void *ctx)
{
  (void)ctx;
  SM_DEBUG("退出AI对话状态");
  /* TODO: 清理AI请求资源 */
  /* network_cancel_request(); */
}

static void sm_care_remind_exit(void *ctx)
{
  (void)ctx;
  SM_DEBUG("退出主动提醒状态");
  /* TODO: 停止语音播放 */
  /* audio_stop_playback(); */
}

static void sm_alarm_exit(void *ctx)
{
  (void)ctx;
  SM_DEBUG("退出报警状态");
  /* TODO: 停止报警相关操作 */
}

/****************************************************************************
 * State Transition Handlers - 状态转换处理
 ****************************************************************************/

/**
 * @brief  待机状态事件处理
 */

static sm_state_t sm_idle_transition(void *ctx, sm_event_t event)
{
  (void)ctx;
  switch (event)
    {
      case SM_EVENT_WAKEUP:
        /* 唤醒词检测到, 切换到监听状态 */

        return SM_STATE_LISTENING;

      case SM_EVENT_CARE_TIMER:
        /* 定时关怀触发 */

        return SM_STATE_CARE_REMIND;

      case SM_EVENT_ALARM_DETECTED:
        /* 异常声音检测到 */

        return SM_STATE_ALARM;

      case SM_EVENT_SLEEP:
        /* 保持待机状态 */

        return SM_STATE_IDLE;

      default:
        SM_DEBUG("IDLE: 忽略事件 %s", g_event_names[event]);
        return SM_STATE_IDLE;
    }
}

/**
 * @brief  监听状态事件处理
 */

static sm_state_t sm_listening_transition(void *ctx, sm_event_t event)
{
  (void)ctx;
  switch (event)
    {
      case SM_EVENT_VOICE_DETECTED:
        /* 检测到语音, 继续监听 */

        return SM_STATE_LISTENING;

      case SM_EVENT_VOICE_COMPLETE:
        /* 语音输入完成, 切换到AI对话状态 */

        return SM_STATE_AI_TALKING;

      case SM_EVENT_TIMEOUT:
        /* 监听超时, 返回待机 */

        SM_DEBUG("监听超时, 返回待机");
        return SM_STATE_IDLE;

      case SM_EVENT_ALARM_DETECTED:
        /* 监听中检测到异常, 切换到报警 */

        return SM_STATE_ALARM;

      default:
        SM_DEBUG("LISTENING: 忽略事件 %s", g_event_names[event]);
        return SM_STATE_LISTENING;
    }
}

/**
 * @brief  AI对话状态事件处理
 */

static sm_state_t sm_ai_talking_transition(void *ctx, sm_event_t event)
{
  (void)ctx;
  switch (event)
    {
      case SM_EVENT_AI_RESPONSE:
        /* 收到AI回复, 播放完毕后返回监听 */

        SM_DEBUG("AI回复完成, 继续监听");
        return SM_STATE_LISTENING;

      case SM_EVENT_AI_ERROR:
        /* AI处理错误, 返回待机 */

        SM_DEBUG("AI处理出错, 返回待机");
        return SM_STATE_IDLE;

      case SM_EVENT_TIMEOUT:
        /* AI响应超时 */

        SM_DEBUG("AI响应超时");
        return SM_STATE_IDLE;

      case SM_EVENT_ALARM_DETECTED:
        /* 对话中检测到异常 */

        return SM_STATE_ALARM;

      default:
        SM_DEBUG("AI_TALKING: 忽略事件 %s", g_event_names[event]);
        return SM_STATE_AI_TALKING;
    }
}

/**
 * @brief  主动提醒状态事件处理
 */

static sm_state_t sm_care_remind_transition(void *ctx, sm_event_t event)
{
  (void)ctx;
  switch (event)
    {
      case SM_EVENT_VOICE_DETECTED:
        /* 用户响应提醒, 切换到监听 */

        return SM_STATE_LISTENING;

      case SM_EVENT_WAKEUP:
        /* 用户唤醒, 切换到监听 */

        return SM_STATE_LISTENING;

      case SM_EVENT_TIMEOUT:
        /* 提醒播放完成 */

        SM_DEBUG("提醒播放完成, 返回待机");
        return SM_STATE_IDLE;

      case SM_EVENT_ALARM_DETECTED:
        /* 提醒中检测到异常 */

        return SM_STATE_ALARM;

      default:
        SM_DEBUG("CARE_REMIND: 忽略事件 %s", g_event_names[event]);
        return SM_STATE_CARE_REMIND;
    }
}

/**
 * @brief  异常报警状态事件处理
 */

static sm_state_t sm_alarm_transition(void *ctx, sm_event_t event)
{
  (void)ctx;
  switch (event)
    {
      case SM_EVENT_ALARM_CLEARED:
        /* 异常解除, 返回待机 */

        SM_DEBUG("异常解除, 返回待机");
        return SM_STATE_IDLE;

      case SM_EVENT_ALARM_CONFIRMED:
        /* 异常确认, 继续报警直到人工处理 */

        SM_DEBUG("异常已确认, 等待人工处理");
        return SM_STATE_ALARM;

      case SM_EVENT_TIMEOUT:
        /* 报警超时, 强制解除 */

        SM_DEBUG("报警超时, 强制解除");
        return SM_STATE_IDLE;

      default:
        SM_DEBUG("ALARM: 忽略事件 %s", g_event_names[event]);
        return SM_STATE_ALARM;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/**
 * @brief  初始化状态机
 */

int sm_init(sm_context_t *ctx)
{
  if (ctx == NULL)
    {
      return -EINVAL;
    }

  SM_DEBUG("初始化状态机");

  /* 清零上下文 */

  memset(ctx, 0, sizeof(sm_context_t));

  /* 设置默认状态 */

  ctx->current_state = SM_STATE_IDLE;
  ctx->prev_state = SM_STATE_IDLE;
  ctx->initialized = true;
  ctx->state_enter_tick = sm_get_tick_ms();
  memcpy(ctx->state_timeouts, g_default_timeouts,
         sizeof(ctx->state_timeouts));
  ctx->timeout_ms = ctx->state_timeouts[SM_STATE_IDLE];

  /* 注册默认状态处理函数 */

  sm_register_state(ctx, SM_STATE_IDLE,
                    sm_idle_enter, sm_idle_exit, sm_idle_transition);

  sm_register_state(ctx, SM_STATE_LISTENING,
                    sm_listening_enter, sm_listening_exit,
                    sm_listening_transition);

  sm_register_state(ctx, SM_STATE_AI_TALKING,
                    sm_ai_talking_enter, sm_ai_talking_exit,
                    sm_ai_talking_transition);

  sm_register_state(ctx, SM_STATE_CARE_REMIND,
                    sm_care_remind_enter, sm_care_remind_exit,
                    sm_care_remind_transition);

  sm_register_state(ctx, SM_STATE_ALARM,
                    sm_alarm_enter, sm_alarm_exit, sm_alarm_transition);

  SM_DEBUG("状态机初始化完成, 初始状态: %s",
           g_state_names[ctx->current_state]);

  return OK;
}

/**
 * @brief  反初始化状态机
 */

void sm_deinit(sm_context_t *ctx)
{
  if (ctx == NULL)
    {
      return;
    }

  SM_DEBUG("反初始化状态机");

  /* 退出当前状态 */

  if ((int)ctx->current_state >= 0 &&
      ctx->current_state < SM_STATE_MAX &&
      ctx->state_table[ctx->current_state].exit_func != NULL)
    {
      ctx->state_table[ctx->current_state].exit_func(ctx);
    }

  ctx->initialized = false;
}

/**
 * @brief  处理事件
 */

int sm_handle_event(sm_context_t *ctx, sm_event_t event)
{
  if (ctx == NULL || !ctx->initialized)
    {
      return -EINVAL;
    }

  if ((int)event < 0 || event >= SM_EVENT_MAX ||
      (int)ctx->current_state < 0 || ctx->current_state >= SM_STATE_MAX)
    {
      return -EINVAL;
    }

  SM_DEBUG("处理事件: %s (当前状态: %s)",
           g_event_names[event],
           g_state_names[ctx->current_state]);

  /* 记录事件 */

  ctx->last_event = event;

  /* 调用当前状态的转换处理函数 */

  sm_transition_handler_t handler =
    ctx->state_table[ctx->current_state].transition_func;

  if (handler != NULL)
    {
      sm_state_t next_state = handler(ctx, event);

      /* 如果状态发生变化, 执行切换 */

      if (next_state != ctx->current_state)
        {
          return sm_switch_state(ctx, next_state);
        }
    }
  else
    {
      SM_DEBUG("警告: 状态 %s 无转换处理函数",
               g_state_names[ctx->current_state]);
    }

  return OK;
}

/**
 * @brief  状态机主循环处理
 */

void sm_run(sm_context_t *ctx)
{
  if (ctx == NULL || !ctx->initialized)
    {
      return;
    }

  /* 检查超时 */

  if (ctx->timeout_ms > 0)
    {
      uint32_t now = sm_get_tick_ms();
      uint32_t elapsed = now - ctx->state_enter_tick;

      if (elapsed >= ctx->timeout_ms)
        {
          SM_DEBUG("状态 %s 超时 (%lu ms)",
                   g_state_names[ctx->current_state],
                   (unsigned long)ctx->timeout_ms);

          /* 触发超时事件 */

          sm_handle_event(ctx, SM_EVENT_TIMEOUT);
        }
    }
}

/**
 * @brief  获取当前状态
 */

sm_state_t sm_get_state(sm_context_t *ctx)
{
  if (ctx == NULL)
    {
      return SM_STATE_IDLE;
    }

  return ctx->current_state;
}

/**
 * @brief  获取状态名称字符串
 */

const char *sm_get_state_name(sm_state_t state)
{
  if ((int)state >= 0 && state < SM_STATE_MAX)
    {
      return g_state_names[state];
    }

  return "UNKNOWN";
}

/**
 * @brief  获取事件名称字符串
 */

const char *sm_get_event_name(sm_event_t event)
{
  if ((int)event >= 0 && event < SM_EVENT_MAX)
    {
      return g_event_names[event];
    }

  return "UNKNOWN";
}

/**
 * @brief  注册状态处理回调
 */

int sm_register_state(sm_context_t *ctx,
                      sm_state_t state,
                      sm_state_handler_t enter,
                      sm_state_handler_t exit,
                      sm_transition_handler_t transition)
{
  if (ctx == NULL || (int)state < 0 || state >= SM_STATE_MAX)
    {
      return -EINVAL;
    }

  ctx->state_table[state].enter_func = enter;
  ctx->state_table[state].exit_func = exit;
  ctx->state_table[state].transition_func = transition;

  SM_DEBUG("注册状态 %s 处理函数", g_state_names[state]);

  return OK;
}

/**
 * @brief  设置状态超时时间
 */

void sm_set_timeout(sm_context_t *ctx, sm_state_t state,
                    uint32_t timeout_ms)
{
  if (ctx == NULL || (int)state < 0 || state >= SM_STATE_MAX)
    {
      return;
    }

  ctx->state_timeouts[state] = timeout_ms;

  /* 如果是当前状态, 立即更新 */

  if (ctx->current_state == state)
    {
      ctx->timeout_ms = timeout_ms;
    }

  SM_DEBUG("设置状态 %s 超时: %lu ms",
           g_state_names[state], (unsigned long)timeout_ms);
}

/**
 * @brief  强制切换状态
 */

void sm_force_state(sm_context_t *ctx, sm_state_t new_state)
{
  if (ctx == NULL || (int)new_state < 0 || new_state >= SM_STATE_MAX ||
      (int)ctx->current_state < 0 || ctx->current_state >= SM_STATE_MAX)
    {
      return;
    }

  SM_DEBUG("强制切换状态: %s -> %s",
           g_state_names[ctx->current_state],
           g_state_names[new_state]);

  sm_switch_state(ctx, new_state);
}

/**
 * @brief  设置用户数据
 */

void sm_set_user_data(sm_context_t *ctx, void *user_data)
{
  if (ctx != NULL)
    {
      ctx->user_data = user_data;
    }
}

/**
 * @brief  获取用户数据
 */

void *sm_get_user_data(sm_context_t *ctx)
{
  if (ctx == NULL)
    {
      return NULL;
    }

  return ctx->user_data;
}
