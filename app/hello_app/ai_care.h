/****************************************************************************
 * AI Care Module Header
 * 智爱陪伴 - AI老人陪伴守护终端
 * 主动关怀模块 - 定时问候、健康提醒、生活提醒
 ****************************************************************************/

#ifndef __AI_CARE_H
#define __AI_CARE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <pthread.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 主动关怀调试日志宏 */
#ifdef CONFIG_DEBUG_AI_CARE
#  define CARE_DEBUG(...) do { printf("[CARE] "); printf(__VA_ARGS__); \
                               printf("\n"); } while (0)
#else
#  define CARE_DEBUG(...) do { } while (0)
#endif

/* 关怀任务配置 */
#define CARE_MAX_TASKS          20      /* 最大关怀任务数 */
#define CARE_MAX_MESSAGE_LEN    256     /* 最大消息长度 */
#define CARE_MAX_NAME_LEN       32      /* 最大名称长度 */

/* 默认提醒时间 */
#define CARE_DEFAULT_HOUR       8       /* 默认提醒小时 */
#define CARE_DEFAULT_MINUTE     0       /* 默认提醒分钟 */

/* 一天的分钟数 */
#define CARE_MINUTES_PER_DAY    (24 * 60)

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* 关怀类型 */
typedef enum
{
  CARE_TYPE_GREETING = 0,       /* 问候关怀 */
  CARE_TYPE_HEALTH,             /* 健康提醒 */
  CARE_TYPE_LIFE,               /* 生活提醒 */
  CARE_TYPE_EMOTION,            /* 情绪陪伴 */
  CARE_TYPE_EXERCISE,           /* 运动提醒 */
  CARE_TYPE_MEDICINE,           /* 吃药提醒 */
  CARE_TYPE_MEAL,               /* 吃饭提醒 */
  CARE_TYPE_SLEEP,              /* 睡眠提醒 */
  CARE_TYPE_CUSTOM,             /* 自定义关怀 */
  CARE_TYPE_MAX                 /* 类型数量上限 */
} care_type_t;

/* 关怀任务状态 */
typedef enum
{
  CARE_TASK_DISABLED = 0,       /* 已禁用 */
  CARE_TASK_ENABLED,            /* 已启用 */
  CARE_TASK_RUNNING,            /* 执行中 */
  CARE_TASK_PAUSED              /* 暂停 */
} care_task_state_t;

/* 关怀触发条件 */
typedef enum
{
  CARE_TRIGGER_TIME = 0,        /* 定时触发 */
  CARE_TRIGGER_INTERVAL,        /* 间隔触发 */
  CARE_TRIGGER_EVENT,           /* 事件触发 */
  CARE_TRIGGER_SMART            /* 智能触发(根据习惯) */
} care_trigger_t;

/* 关怀回调 */
typedef void (*care_callback_t)(care_type_t type,
                                const char *message,
                                void *user_data);

/* 关怀任务结构 */
typedef struct
{
  int                 id;               /* 任务ID */
  char                name[CARE_MAX_NAME_LEN]; /* 任务名称 */
  care_type_t         type;             /* 关怀类型 */
  care_task_state_t   state;            /* 任务状态 */
  care_trigger_t      trigger;          /* 触发条件 */

  /* 时间配置 */
  uint8_t             hour;             /* 小时 (0-23) */
  uint8_t             minute;           /* 分钟 (0-59) */
  uint32_t            interval_minutes; /* 间隔(分钟) */
  bool                repeat_daily;     /* 每天重复 */
  bool                weekdays_only;    /* 仅工作日 */

  /* 消息配置 */
  char                message[CARE_MAX_MESSAGE_LEN]; /* 关怀消息 */
  char                voice_file[128];  /* 语音文件路径 */

  /* 统计信息 */
  uint32_t            trigger_count;    /* 触发次数 */
  uint32_t            last_trigger_time; /* 上次触发时间 */
  uint32_t            next_trigger_time; /* 下次触发时间 */

  /* 回调 */
  care_callback_t     callback;         /* 关怀回调 */
  void               *user_data;        /* 用户数据 */
} care_task_t;

/* 用户习惯配置 */
typedef struct
{
  uint8_t             wake_up_hour;     /* 起床时间-小时 */
  uint8_t             wake_up_minute;   /* 起床时间-分钟 */
  uint8_t             sleep_hour;       /* 睡觉时间-小时 */
  uint8_t             sleep_minute;     /* 睡觉时间-分钟 */
  uint8_t             breakfast_hour;   /* 早餐时间-小时 */
  uint8_t             lunch_hour;       /* 午餐时间-小时 */
  uint8_t             dinner_hour;      /* 晚餐时间-小时 */
  uint32_t            exercise_minutes; /* 运动时长(分钟) */
  bool                has_medicine;     /* 是否需要吃药 */
  char                medicine_times[8][5]; /* 吃药时间列表 */
} care_user_habit_t;

/* 关怀配置 */
typedef struct
{
  bool                enable_greeting;  /* 启用问候关怀 */
  bool                enable_health;    /* 启用健康提醒 */
  bool                enable_life;      /* 启用生活提醒 */
  bool                enable_emotion;   /* 启用情绪陪伴 */
  bool                enable_exercise;  /* 启用运动提醒 */
  bool                enable_medicine;  /* 启用吃药提醒 */
  bool                enable_meal;      /* 启用吃饭提醒 */
  bool                enable_sleep;     /* 启用睡眠提醒 */
  bool                smart_mode;       /* 智能模式 */
  care_user_habit_t   user_habit;       /* 用户习惯 */
  care_callback_t     callback;         /* 全局回调 */
  void               *user_data;        /* 用户数据 */
} care_config_t;

/* 关怀统计信息 */
typedef struct
{
  uint32_t     total_triggers;          /* 总触发次数 */
  uint32_t     greeting_count;          /* 问候次数 */
  uint32_t     health_count;            /* 健康提醒次数 */
  uint32_t     life_count;              /* 生活提醒次数 */
  uint32_t     emotion_count;           /* 情绪陪伴次数 */
  uint32_t     last_care_time;          /* 上次关怀时间 */
  care_type_t  last_care_type;          /* 上次关怀类型 */
} care_stats_t;

/* 关怀模块上下文 */
typedef struct
{
  care_config_t       config;           /* 关怀配置 */
  care_task_t         tasks[CARE_MAX_TASKS]; /* 关怀任务 */
  int                 task_count;       /* 任务数量 */
  care_stats_t        stats;            /* 统计信息 */
  bool                initialized;      /* 是否已初始化 */
  bool                running;          /* 是否运行中 */

  /* 定时器相关 */
  pthread_t           care_thread;      /* 关怀线程 */
  volatile bool       care_stop;        /* 停止标志 */
  bool                care_thread_valid;
  uint32_t            check_interval_ms; /* 检查间隔(毫秒) */

  /* 用户数据 */
  void               *user_data;        /* 用户自定义数据 */
} care_context_t;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/**
 * @brief  初始化主动关怀模块
 * @param  ctx: 关怀上下文指针
 * @param  config: 关怀配置, NULL使用默认配置
 * @return 0成功, 负值失败
 */

int care_init(care_context_t *ctx, const care_config_t *config);

/**
 * @brief  反初始化主动关怀模块
 * @param  ctx: 关怀上下文指针
 */

void care_deinit(care_context_t *ctx);

/**
 * @brief  启动主动关怀
 * @param  ctx: 关怀上下文指针
 * @return 0成功, 负值失败
 */

int care_start(care_context_t *ctx);

/**
 * @brief  停止主动关怀
 * @param  ctx: 关怀上下文指针
 */

void care_stop(care_context_t *ctx);

/**
 * @brief  添加关怀任务
 * @param  ctx: 关怀上下文指针
 * @param  task: 关怀任务
 * @return 任务ID, 负值失败
 */

int care_add_task(care_context_t *ctx, const care_task_t *task);

/**
 * @brief  删除关怀任务
 * @param  ctx: 关怀上下文指针
 * @param  task_id: 任务ID
 * @return 0成功, 负值失败
 */

int care_remove_task(care_context_t *ctx, int task_id);

/**
 * @brief  启用关怀任务
 * @param  ctx: 关怀上下文指针
 * @param  task_id: 任务ID
 * @return 0成功, 负值失败
 */

int care_enable_task(care_context_t *ctx, int task_id);

/**
 * @brief  禁用关怀任务
 * @param  ctx: 关怀上下文指针
 * @param  task_id: 任务ID
 * @return 0成功, 负值失败
 */

int care_disable_task(care_context_t *ctx, int task_id);

/**
 * @brief  更新关怀任务
 * @param  ctx: 关怀上下文指针
 * @param  task_id: 任务ID
 * @param  task: 新的任务配置
 * @return 0成功, 负值失败
 */

int care_update_task(care_context_t *ctx, int task_id,
                     const care_task_t *task);

/**
 * @brief  获取关怀任务
 * @param  ctx: 关怀上下文指针
 * @param  task_id: 任务ID
 * @return 任务指针, NULL表示不存在
 */

const care_task_t *care_get_task(care_context_t *ctx, int task_id);

/**
 * @brief  获取所有任务列表
 * @param  ctx: 关怀上下文指针
 * @param  tasks: 输出任务数组
 * @param  max_count: 最大数量
 * @return 实际任务数量
 */

int care_get_task_list(care_context_t *ctx,
                       care_task_t *tasks, int max_count);

/**
 * @brief  设置用户习惯
 * @param  ctx: 关怀上下文指针
 * @param  habit: 用户习惯配置
 */

void care_set_user_habit(care_context_t *ctx,
                         const care_user_habit_t *habit);

/**
 * @brief  触发关怀任务
 * @param  ctx: 关怀上下文指针
 * @param  type: 关怀类型
 * @return 0成功, 负值失败
 */

int care_trigger(care_context_t *ctx, care_type_t type);

/**
 * @brief  手动触发指定任务
 * @param  ctx: 关怀上下文指针
 * @param  task_id: 任务ID
 * @return 0成功, 负值失败
 */

int care_trigger_task(care_context_t *ctx, int task_id);

/**
 * @brief  获取关怀统计信息
 * @param  ctx: 关怀上下文指针
 * @return 统计信息指针
 */

const care_stats_t *care_get_stats(care_context_t *ctx);

/**
 * @brief  重置统计信息
 * @param  ctx: 关怀上下文指针
 */

void care_reset_stats(care_context_t *ctx);

/**
 * @brief  获取关怀类型名称字符串
 * @param  type: 关怀类型
 * @return 类型名称字符串
 */

const char *care_get_type_name(care_type_t type);

/**
 * @brief  获取任务状态名称字符串
 * @param  state: 任务状态
 * @return 状态名称字符串
 */

const char *care_get_task_state_name(care_task_state_t state);

/**
 * @brief  创建问候任务
 * @param  hour: 小时
 * @param  minute: 分钟
 * @param  message: 问候消息
 * @return 任务结构体指针
 */

care_task_t *care_create_greeting_task(uint8_t hour, uint8_t minute,
                                       const char *message);

/**
 * @brief  创建健康提醒任务
 * @param  hour: 小时
 * @param  minute: 分钟
 * @param  message: 提醒消息
 * @return 任务结构体指针
 */

care_task_t *care_create_health_task(uint8_t hour, uint8_t minute,
                                     const char *message);

/**
 * @brief  创建吃药提醒任务
 * @param  hour: 小时
 * @param  minute: 分钟
 * @param  medicine_name: 药品名称
 * @return 任务结构体指针
 */

care_task_t *care_create_medicine_task(uint8_t hour, uint8_t minute,
                                       const char *medicine_name);

/**
 * @brief  创建运动提醒任务
 * @param  hour: 小时
 * @param  minute: 分钟
 * @param  exercise_type: 运动类型
 * @return 任务结构体指针
 */

care_task_t *care_create_exercise_task(uint8_t hour, uint8_t minute,
                                       const char *exercise_type);

/**
 * @brief  创建吃饭提醒任务
 * @param  hour: 小时
 * @param  minute: 分钟
 * @param  meal_type: 餐次(早餐/午餐/晚餐)
 * @return 任务结构体指针
 */

care_task_t *care_create_meal_task(uint8_t hour, uint8_t minute,
                                   const char *meal_type);

/**
 * @brief  创建睡眠提醒任务
 * @param  hour: 小时
 * @param  minute: 分钟
 * @return 任务结构体指针
 */

care_task_t *care_create_sleep_task(uint8_t hour, uint8_t minute);

/**
 * @brief  初始化默认关怀任务
 * @param  ctx: 关怀上下文指针
 */

void care_init_default_tasks(care_context_t *ctx);

#endif /* __AI_CARE_H */
