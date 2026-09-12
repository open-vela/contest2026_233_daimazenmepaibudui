/****************************************************************************
 * AI Care Module Implementation
 * 智爱陪伴 - AI老人陪伴守护终端
 * 主动关怀模块 - 定时问候、健康提醒、生活提醒
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "ai_care.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 默认检查间隔 */
#define CARE_DEFAULT_CHECK_INTERVAL_MS  30000  /* 30秒 */

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void *care_thread_func(void *arg);
static void care_check_tasks(care_context_t *ctx);
static void care_execute_task(care_context_t *ctx, care_task_t *task);
static uint32_t care_get_current_time_minutes(void);
static uint32_t care_get_current_timestamp(void);
static int care_calc_next_trigger(care_task_t *task);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 关怀类型名称表 */
static const char *g_type_names[] =
{
  [CARE_TYPE_GREETING]  = "问候关怀",
  [CARE_TYPE_HEALTH]    = "健康提醒",
  [CARE_TYPE_LIFE]      = "生活提醒",
  [CARE_TYPE_EMOTION]   = "情绪陪伴",
  [CARE_TYPE_EXERCISE]  = "运动提醒",
  [CARE_TYPE_MEDICINE]  = "吃药提醒",
  [CARE_TYPE_MEAL]      = "吃饭提醒",
  [CARE_TYPE_SLEEP]     = "睡眠提醒",
  [CARE_TYPE_CUSTOM]    = "自定义关怀"
};

/* 任务状态名称表 */
static const char *g_state_names[] =
{
  [CARE_TASK_DISABLED] = "已禁用",
  [CARE_TASK_ENABLED]  = "已启用",
  [CARE_TASK_RUNNING]  = "执行中",
  [CARE_TASK_PAUSED]   = "已暂停"
};

/* 默认问候消息 */
static const char *g_default_greetings[] =
{
  "早上好！新的一天开始了，祝您今天心情愉快！",
  "上午好！今天天气不错，适合出去走走。",
  "中午好！该吃午饭了，记得按时吃饭哦。",
  "下午好！下午茶时间到了，休息一下吧。",
  "晚上好！今天辛苦了，好好休息。",
  "晚安！祝您今晚做个好梦。"
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * @brief  获取当前时间(分钟数，从0点开始)
 */

static uint32_t care_get_current_time_minutes(void)
{
  time_t now = time(NULL);
  struct tm *tm = localtime(&now);
  if (tm == NULL)
    {
      return 0;
    }

  return tm->tm_hour * 60 + tm->tm_min;
}

/**
 * @brief  获取当前时间戳
 */

static uint32_t care_get_current_timestamp(void)
{
  return (uint32_t)time(NULL);
}

/**
 * @brief  计算下次触发时间
 */

static int care_calc_next_trigger(care_task_t *task)
{
  if (task == NULL)
    {
      return -EINVAL;
    }

  uint32_t now = care_get_current_timestamp();
  uint32_t current_minutes = care_get_current_time_minutes();
  uint32_t trigger_minutes = task->hour * 60 + task->minute;

  if (task->trigger == CARE_TRIGGER_TIME)
    {
      /* 定时触发 */

      if (task->repeat_daily)
        {
          /* 每天重复 */

          if (current_minutes < trigger_minutes)
            {
              /* 今天还未到触发时间 */

              task->next_trigger_time = now +
                (trigger_minutes - current_minutes) * 60;
            }
          else
            {
              /* 今天已过，设置为明天 */

              task->next_trigger_time = now +
                (CARE_MINUTES_PER_DAY - current_minutes + trigger_minutes) * 60;
            }
        }
      else
        {
          /* 单次触发 */

          if (current_minutes < trigger_minutes)
            {
              task->next_trigger_time = care_get_current_timestamp() +
                (trigger_minutes - current_minutes) * 60;
            }
          else
            {
              /* 已过触发时间，标记为完成 */

              task->state = CARE_TASK_DISABLED;
              task->next_trigger_time = 0;
            }
        }
    }
  else if (task->trigger == CARE_TRIGGER_INTERVAL)
    {
      /* 间隔触发 */

      if (task->last_trigger_time == 0)
        {
          /* 首次触发 */

          if (task->interval_minutes == 0)
            {
              return -EINVAL;
            }

          task->next_trigger_time = now + task->interval_minutes * 60;
        }
      else
        {
          task->next_trigger_time = task->last_trigger_time +
            task->interval_minutes * 60;
        }
    }

  return OK;
}

/**
 * @brief  执行关怀任务
 */

static void care_execute_task(care_context_t *ctx, care_task_t *task)
{
  if (ctx == NULL || task == NULL)
    {
      return;
    }

  CARE_DEBUG("执行关怀任务: %s (类型: %s)",
             task->name, care_get_type_name(task->type));

  /* 更新统计 */

  ctx->stats.total_triggers++;
  ctx->stats.last_care_time = care_get_current_timestamp();
  ctx->stats.last_care_type = task->type;

  switch (task->type)
    {
      case CARE_TYPE_GREETING:
        ctx->stats.greeting_count++;
        break;
      case CARE_TYPE_HEALTH:
        ctx->stats.health_count++;
        break;
      case CARE_TYPE_LIFE:
        ctx->stats.life_count++;
        break;
      case CARE_TYPE_EMOTION:
        ctx->stats.emotion_count++;
        break;
      default:
        break;
    }

  /* 更新任务统计 */

  task->trigger_count++;
  task->last_trigger_time = care_get_current_timestamp();

  /* 计算下次触发时间 */

  care_calc_next_trigger(task);

  /* 调用任务回调 */

  if (task->callback != NULL)
    {
      task->callback(task->type, task->message, task->user_data);
    }

  /* 调用全局回调 */

  if (ctx->config.callback != NULL)
    {
      ctx->config.callback(task->type, task->message,
                           ctx->config.user_data);
    }

  CARE_DEBUG("关怀任务完成: %s, 下次触发: %lu",
             task->name, (unsigned long)task->next_trigger_time);
}

/**
 * @brief  检查所有任务
 */

static void care_check_tasks(care_context_t *ctx)
{
  if (ctx == NULL)
    {
      return;
    }

  uint32_t current_time = care_get_current_timestamp();
  for (int i = 0; i < ctx->task_count; i++)
    {
      care_task_t *task = &ctx->tasks[i];

      /* 跳过禁用或暂停的任务 */

      if (task->state != CARE_TASK_ENABLED)
        {
          continue;
        }

      /* 检查是否需要执行 */

      bool should_execute = false;

      if ((task->trigger == CARE_TRIGGER_TIME ||
           task->trigger == CARE_TRIGGER_INTERVAL) &&
          task->next_trigger_time != 0 &&
          current_time >= task->next_trigger_time)
        {
          should_execute = true;
        }

      /* 执行任务 */

      if (should_execute)
        {
          care_execute_task(ctx, task);
        }
    }
}

/**
 * @brief  关怀线程函数
 */

static void *care_thread_func(void *arg)
{
  care_context_t *ctx = (care_context_t *)arg;

  CARE_DEBUG("关怀线程启动");

  while (!ctx->care_stop)
    {
      /* 检查所有任务 */

      care_check_tasks(ctx);

      /* 休眠等待 */

      uint32_t waited_ms = 0;
      while (!ctx->care_stop && waited_ms < ctx->check_interval_ms)
        {
          uint32_t slice_ms = ctx->check_interval_ms - waited_ms;
          if (slice_ms > 100)
            {
              slice_ms = 100;
            }

          usleep(slice_ms * 1000);
          waited_ms += slice_ms;
        }
    }

  CARE_DEBUG("关怀线程退出");
  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/**
 * @brief  初始化主动关怀模块
 */

int care_init(care_context_t *ctx, const care_config_t *config)
{
  if (ctx == NULL)
    {
      return -EINVAL;
    }

  CARE_DEBUG("初始化主动关怀模块");

  /* 清零上下文 */

  memset(ctx, 0, sizeof(care_context_t));

  /* 设置默认配置 */

  if (config != NULL)
    {
      memcpy(&ctx->config, config, sizeof(care_config_t));
    }
  else
    {
      /* 默认启用所有关怀类型 */

      ctx->config.enable_greeting = true;
      ctx->config.enable_health = true;
      ctx->config.enable_life = true;
      ctx->config.enable_emotion = true;
      ctx->config.enable_exercise = true;
      ctx->config.enable_medicine = true;
      ctx->config.enable_meal = true;
      ctx->config.enable_sleep = true;
      ctx->config.smart_mode = true;

      /* 设置默认用户习惯 */

      ctx->config.user_habit.wake_up_hour = 7;
      ctx->config.user_habit.wake_up_minute = 0;
      ctx->config.user_habit.sleep_hour = 22;
      ctx->config.user_habit.sleep_minute = 0;
      ctx->config.user_habit.breakfast_hour = 7;
      ctx->config.user_habit.lunch_hour = 12;
      ctx->config.user_habit.dinner_hour = 18;
      ctx->config.user_habit.exercise_minutes = 30;
    }

  /* 设置检查间隔 */

  ctx->check_interval_ms = CARE_DEFAULT_CHECK_INTERVAL_MS;

  /* 初始化默认任务 */

  care_init_default_tasks(ctx);

  ctx->initialized = true;

  CARE_DEBUG("主动关怀模块初始化完成");
  CARE_DEBUG("  任务数量: %d", ctx->task_count);

  return OK;
}

/**
 * @brief  反初始化主动关怀模块
 */

void care_deinit(care_context_t *ctx)
{
  if (ctx == NULL || !ctx->initialized)
    {
      return;
    }

  CARE_DEBUG("反初始化主动关怀模块");

  /* 停止关怀 */

  care_stop(ctx);

  ctx->initialized = false;

  CARE_DEBUG("主动关怀模块已反初始化");
}

/**
 * @brief  启动主动关怀
 */

int care_start(care_context_t *ctx)
{
  if (ctx == NULL || !ctx->initialized)
    {
      return -EINVAL;
    }

  if (ctx->running)
    {
      CARE_DEBUG("关怀已在运行中");
      return -EBUSY;
    }

  CARE_DEBUG("启动主动关怀");

  /* 计算所有任务的下次触发时间 */

  for (int i = 0; i < ctx->task_count; i++)
    {
      if (ctx->tasks[i].state == CARE_TASK_ENABLED)
        {
          care_calc_next_trigger(&ctx->tasks[i]);
        }
    }

  /* 启动关怀线程 */

  ctx->care_stop = false;
  int ret = pthread_create(&ctx->care_thread, NULL,
                           care_thread_func, ctx);
  if (ret != 0)
    {
      CARE_DEBUG("创建关怀线程失败: %d", ret);
      return -ret;
    }

  ctx->running = true;
  ctx->care_thread_valid = true;

  return OK;
}

/**
 * @brief  停止主动关怀
 */

void care_stop(care_context_t *ctx)
{
  if (ctx == NULL || !ctx->care_thread_valid)
    {
      return;
    }

  CARE_DEBUG("停止主动关怀");

  /* 设置停止标志 */

  ctx->care_stop = true;

  /* 等待线程退出 */

  pthread_join(ctx->care_thread, NULL);

  ctx->care_thread_valid = false;
  ctx->running = false;
}

/**
 * @brief  添加关怀任务
 */

int care_add_task(care_context_t *ctx, const care_task_t *task)
{
  if (ctx == NULL || task == NULL)
    {
      return -EINVAL;
    }

  if (ctx->task_count >= CARE_MAX_TASKS)
    {
      CARE_DEBUG("任务数量已达上限");
      return -ENOMEM;
    }

  if (task->type < CARE_TYPE_GREETING || task->type >= CARE_TYPE_MAX ||
      task->state < CARE_TASK_DISABLED ||
      task->state > CARE_TASK_PAUSED ||
      task->trigger < CARE_TRIGGER_TIME ||
      task->trigger > CARE_TRIGGER_SMART ||
      task->hour > 23 || task->minute > 59 ||
      (task->trigger == CARE_TRIGGER_INTERVAL &&
       task->interval_minutes == 0))
    {
      return -EINVAL;
    }

  /* 复制任务 */

  int id = ctx->task_count;
  memcpy(&ctx->tasks[id], task, sizeof(care_task_t));
  ctx->tasks[id].id = id;
  ctx->task_count++;

  /* 计算下次触发时间 */

  if (ctx->tasks[id].state == CARE_TASK_ENABLED)
    {
      care_calc_next_trigger(&ctx->tasks[id]);
    }

  CARE_DEBUG("添加关怀任务: id=%d, name=%s, type=%s",
             id, task->name, care_get_type_name(task->type));

  return id;
}

/**
 * @brief  删除关怀任务
 */

int care_remove_task(care_context_t *ctx, int task_id)
{
  if (ctx == NULL || task_id < 0 || task_id >= ctx->task_count)
    {
      return -EINVAL;
    }

  CARE_DEBUG("删除关怀任务: id=%d", task_id);

  /* 移动后续任务 */

  for (int i = task_id; i < ctx->task_count - 1; i++)
    {
      memcpy(&ctx->tasks[i], &ctx->tasks[i + 1], sizeof(care_task_t));
      ctx->tasks[i].id = i;
    }

  ctx->task_count--;
  memset(&ctx->tasks[ctx->task_count], 0, sizeof(care_task_t));

  return OK;
}

/**
 * @brief  启用关怀任务
 */

int care_enable_task(care_context_t *ctx, int task_id)
{
  if (ctx == NULL || task_id < 0 || task_id >= ctx->task_count)
    {
      return -EINVAL;
    }

  CARE_DEBUG("启用关怀任务: id=%d", task_id);

  ctx->tasks[task_id].state = CARE_TASK_ENABLED;
  care_calc_next_trigger(&ctx->tasks[task_id]);

  return OK;
}

/**
 * @brief  禁用关怀任务
 */

int care_disable_task(care_context_t *ctx, int task_id)
{
  if (ctx == NULL || task_id < 0 || task_id >= ctx->task_count)
    {
      return -EINVAL;
    }

  CARE_DEBUG("禁用关怀任务: id=%d", task_id);

  ctx->tasks[task_id].state = CARE_TASK_DISABLED;

  return OK;
}

/**
 * @brief  更新关怀任务
 */

int care_update_task(care_context_t *ctx, int task_id,
                     const care_task_t *task)
{
  if (ctx == NULL || task_id < 0 || task_id >= ctx->task_count ||
      task == NULL)
    {
      return -EINVAL;
    }

  CARE_DEBUG("更新关怀任务: id=%d", task_id);

  /* 保留原ID和统计信息 */

  int id = ctx->tasks[task_id].id;
  uint32_t trigger_count = ctx->tasks[task_id].trigger_count;
  uint32_t last_trigger_time = ctx->tasks[task_id].last_trigger_time;

  memcpy(&ctx->tasks[task_id], task, sizeof(care_task_t));
  ctx->tasks[task_id].id = id;
  ctx->tasks[task_id].trigger_count = trigger_count;
  ctx->tasks[task_id].last_trigger_time = last_trigger_time;

  /* 重新计算触发时间 */

  if (ctx->tasks[task_id].state == CARE_TASK_ENABLED)
    {
      care_calc_next_trigger(&ctx->tasks[task_id]);
    }

  return OK;
}

/**
 * @brief  获取关怀任务
 */

const care_task_t *care_get_task(care_context_t *ctx, int task_id)
{
  if (ctx == NULL || task_id < 0 || task_id >= ctx->task_count)
    {
      return NULL;
    }

  return &ctx->tasks[task_id];
}

/**
 * @brief  获取所有任务列表
 */

int care_get_task_list(care_context_t *ctx,
                       care_task_t *tasks, int max_count)
{
  if (ctx == NULL || tasks == NULL || max_count <= 0)
    {
      return 0;
    }

  int count = ctx->task_count < max_count ? ctx->task_count : max_count;
  memcpy(tasks, ctx->tasks, count * sizeof(care_task_t));

  return count;
}

/**
 * @brief  设置用户习惯
 */

void care_set_user_habit(care_context_t *ctx,
                         const care_user_habit_t *habit)
{
  if (ctx == NULL || habit == NULL)
    {
      return;
    }

  CARE_DEBUG("设置用户习惯");
  memcpy(&ctx->config.user_habit, habit, sizeof(care_user_habit_t));

  /* 根据用户习惯更新相关任务 */

  /* TODO: 智能更新任务时间 */
}

/**
 * @brief  触发关怀任务
 */

int care_trigger(care_context_t *ctx, care_type_t type)
{
  if (ctx == NULL || type < CARE_TYPE_GREETING || type >= CARE_TYPE_MAX)
    {
      return -EINVAL;
    }

  CARE_DEBUG("手动触发关怀: %s", care_get_type_name(type));

  /* 查找第一个匹配类型的任务 */

  for (int i = 0; i < ctx->task_count; i++)
    {
      if (ctx->tasks[i].type == type &&
          ctx->tasks[i].state == CARE_TASK_ENABLED)
        {
          care_execute_task(ctx, &ctx->tasks[i]);
          return OK;
        }
    }

  CARE_DEBUG("未找到匹配的关怀任务");
  return -ENODATA;
}

/**
 * @brief  手动触发指定任务
 */

int care_trigger_task(care_context_t *ctx, int task_id)
{
  if (ctx == NULL || task_id < 0 || task_id >= ctx->task_count)
    {
      return -EINVAL;
    }

  if (ctx->tasks[task_id].state != CARE_TASK_ENABLED)
    {
      CARE_DEBUG("任务未启用");
      return -EINVAL;
    }

  care_execute_task(ctx, &ctx->tasks[task_id]);

  return OK;
}

/**
 * @brief  获取关怀统计信息
 */

const care_stats_t *care_get_stats(care_context_t *ctx)
{
  if (ctx == NULL)
    {
      return NULL;
    }

  return &ctx->stats;
}

/**
 * @brief  重置统计信息
 */

void care_reset_stats(care_context_t *ctx)
{
  if (ctx == NULL)
    {
      return;
    }

  CARE_DEBUG("重置统计信息");
  memset(&ctx->stats, 0, sizeof(care_stats_t));
}

/**
 * @brief  获取关怀类型名称字符串
 */

const char *care_get_type_name(care_type_t type)
{
  if (type >= CARE_TYPE_GREETING && type < CARE_TYPE_MAX)
    {
      return g_type_names[type];
    }

  return "未知类型";
}

/**
 * @brief  获取任务状态名称字符串
 */

const char *care_get_task_state_name(care_task_state_t state)
{
  if (state >= CARE_TASK_DISABLED && state <= CARE_TASK_PAUSED)
    {
      return g_state_names[state];
    }

  return "未知状态";
}

/**
 * @brief  创建问候任务
 */

care_task_t *care_create_greeting_task(uint8_t hour, uint8_t minute,
                                       const char *message)
{
  care_task_t *task = (care_task_t *)malloc(sizeof(care_task_t));
  if (task == NULL)
    {
      return NULL;
    }

  memset(task, 0, sizeof(care_task_t));

  task->type = CARE_TYPE_GREETING;
  task->state = CARE_TASK_ENABLED;
  task->trigger = CARE_TRIGGER_TIME;
  task->hour = hour;
  task->minute = minute;
  task->repeat_daily = true;

  if (message != NULL)
    {
      strncpy(task->message, message, CARE_MAX_MESSAGE_LEN - 1);
    }
  else
    {
      /* 使用默认问候 */

      int idx = hour / 4; /* 根据时间选择问候 */
      if (idx >= 0 && idx < 6)
        {
          strncpy(task->message, g_default_greetings[idx],
                  CARE_MAX_MESSAGE_LEN - 1);
        }
    }

  snprintf(task->name, CARE_MAX_NAME_LEN, "问候_%02d:%02d", hour, minute);

  return task;
}

/**
 * @brief  创建健康提醒任务
 */

care_task_t *care_create_health_task(uint8_t hour, uint8_t minute,
                                     const char *message)
{
  care_task_t *task = (care_task_t *)malloc(sizeof(care_task_t));
  if (task == NULL)
    {
      return NULL;
    }

  memset(task, 0, sizeof(care_task_t));

  task->type = CARE_TYPE_HEALTH;
  task->state = CARE_TASK_ENABLED;
  task->trigger = CARE_TRIGGER_TIME;
  task->hour = hour;
  task->minute = minute;
  task->repeat_daily = true;

  if (message != NULL)
    {
      strncpy(task->message, message, CARE_MAX_MESSAGE_LEN - 1);
    }
  else
    {
      strncpy(task->message, "记得照顾好自己的身体哦！",
              CARE_MAX_MESSAGE_LEN - 1);
    }

  snprintf(task->name, CARE_MAX_NAME_LEN, "健康_%02d:%02d", hour, minute);

  return task;
}

/**
 * @brief  创建吃药提醒任务
 */

care_task_t *care_create_medicine_task(uint8_t hour, uint8_t minute,
                                       const char *medicine_name)
{
  care_task_t *task = (care_task_t *)malloc(sizeof(care_task_t));
  if (task == NULL)
    {
      return NULL;
    }

  memset(task, 0, sizeof(care_task_t));

  task->type = CARE_TYPE_MEDICINE;
  task->state = CARE_TASK_ENABLED;
  task->trigger = CARE_TRIGGER_TIME;
  task->hour = hour;
  task->minute = minute;
  task->repeat_daily = true;

  if (medicine_name != NULL)
    {
      snprintf(task->message, CARE_MAX_MESSAGE_LEN,
               "该吃%s了，记得按时服药！", medicine_name);
    }
  else
    {
      strncpy(task->message, "该吃药了，记得按时服药！",
              CARE_MAX_MESSAGE_LEN - 1);
    }

  snprintf(task->name, CARE_MAX_NAME_LEN, "吃药_%02d:%02d", hour, minute);

  return task;
}

/**
 * @brief  创建运动提醒任务
 */

care_task_t *care_create_exercise_task(uint8_t hour, uint8_t minute,
                                       const char *exercise_type)
{
  care_task_t *task = (care_task_t *)malloc(sizeof(care_task_t));
  if (task == NULL)
    {
      return NULL;
    }

  memset(task, 0, sizeof(care_task_t));

  task->type = CARE_TYPE_EXERCISE;
  task->state = CARE_TASK_ENABLED;
  task->trigger = CARE_TRIGGER_TIME;
  task->hour = hour;
  task->minute = minute;
  task->repeat_daily = true;

  if (exercise_type != NULL)
    {
      snprintf(task->message, CARE_MAX_MESSAGE_LEN,
               "该做%s了，活动一下身体吧！", exercise_type);
    }
  else
    {
      strncpy(task->message, "该运动了，活动一下身体吧！",
              CARE_MAX_MESSAGE_LEN - 1);
    }

  snprintf(task->name, CARE_MAX_NAME_LEN, "运动_%02d:%02d", hour, minute);

  return task;
}

/**
 * @brief  创建吃饭提醒任务
 */

care_task_t *care_create_meal_task(uint8_t hour, uint8_t minute,
                                   const char *meal_type)
{
  care_task_t *task = (care_task_t *)malloc(sizeof(care_task_t));
  if (task == NULL)
    {
      return NULL;
    }

  memset(task, 0, sizeof(care_task_t));

  task->type = CARE_TYPE_MEAL;
  task->state = CARE_TASK_ENABLED;
  task->trigger = CARE_TRIGGER_TIME;
  task->hour = hour;
  task->minute = minute;
  task->repeat_daily = true;

  if (meal_type != NULL)
    {
      snprintf(task->message, CARE_MAX_MESSAGE_LEN,
               "该吃%s了，记得按时吃饭！", meal_type);
    }
  else
    {
      strncpy(task->message, "该吃饭了，记得按时吃饭！",
              CARE_MAX_MESSAGE_LEN - 1);
    }

  snprintf(task->name, CARE_MAX_NAME_LEN, "吃饭_%02d:%02d", hour, minute);

  return task;
}

/**
 * @brief  创建睡眠提醒任务
 */

care_task_t *care_create_sleep_task(uint8_t hour, uint8_t minute)
{
  care_task_t *task = (care_task_t *)malloc(sizeof(care_task_t));
  if (task == NULL)
    {
      return NULL;
    }

  memset(task, 0, sizeof(care_task_t));

  task->type = CARE_TYPE_SLEEP;
  task->state = CARE_TASK_ENABLED;
  task->trigger = CARE_TRIGGER_TIME;
  task->hour = hour;
  task->minute = minute;
  task->repeat_daily = true;

  strncpy(task->message, "时间不早了，该休息了，晚安！",
          CARE_MAX_MESSAGE_LEN - 1);

  snprintf(task->name, CARE_MAX_NAME_LEN, "睡觉_%02d:%02d", hour, minute);

  return task;
}

/**
 * @brief  初始化默认关怀任务
 */

void care_init_default_tasks(care_context_t *ctx)
{
  if (ctx == NULL)
    {
      return;
    }

  CARE_DEBUG("初始化默认关怀任务");

  /* 早安问候 */

  care_task_t *greeting_am = care_create_greeting_task(7, 30,
    "早上好！新的一天开始了，祝您今天心情愉快！");
  if (greeting_am != NULL)
    {
      care_add_task(ctx, greeting_am);
      free(greeting_am);
    }

  /* 午餐提醒 */

  care_task_t *meal_lunch = care_create_meal_task(11, 30, "午餐");
  if (meal_lunch != NULL)
    {
      care_add_task(ctx, meal_lunch);
      free(meal_lunch);
    }

  /* 午后问候 */

  care_task_t *greeting_pm = care_create_greeting_task(14, 0,
    "下午好！下午茶时间到了，休息一下吧。");
  if (greeting_pm != NULL)
    {
      care_add_task(ctx, greeting_pm);
      free(greeting_pm);
    }

  /* 晚餐提醒 */

  care_task_t *meal_dinner = care_create_meal_task(17, 30, "晚餐");
  if (meal_dinner != NULL)
    {
      care_add_task(ctx, meal_dinner);
      free(meal_dinner);
    }

  /* 晚安问候 */

  care_task_t *greeting_night = care_create_greeting_task(21, 30,
    "晚上好！今天辛苦了，好好休息。");
  if (greeting_night != NULL)
    {
      care_add_task(ctx, greeting_night);
      free(greeting_night);
    }

  /* 喝水提醒 (每2小时) */

  care_task_t water = {0};
  water.type = CARE_TYPE_HEALTH;
  water.state = CARE_TASK_ENABLED;
  water.trigger = CARE_TRIGGER_INTERVAL;
  water.interval_minutes = 120; /* 2小时 */
  strncpy(water.name, "喝水提醒", CARE_MAX_NAME_LEN - 1);
  strncpy(water.message, "记得喝杯水，保持身体水分充足。",
          CARE_MAX_MESSAGE_LEN - 1);
  care_add_task(ctx, &water);

  /* 活动提醒 (每1小时) */

  care_task_t activity = {0};
  activity.type = CARE_TYPE_HEALTH;
  activity.state = CARE_TASK_ENABLED;
  activity.trigger = CARE_TRIGGER_INTERVAL;
  activity.interval_minutes = 60; /* 1小时 */
  strncpy(activity.name, "活动提醒", CARE_MAX_NAME_LEN - 1);
  strncpy(activity.message, "坐久了要站起来活动活动，对腰椎好。",
          CARE_MAX_MESSAGE_LEN - 1);
  care_add_task(ctx, &activity);

  CARE_DEBUG("默认关怀任务初始化完成: %d 个任务", ctx->task_count);
}
