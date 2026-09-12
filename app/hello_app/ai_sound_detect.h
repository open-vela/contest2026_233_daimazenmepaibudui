/****************************************************************************
 * AI Sound Detection Module Header
 * 智爱陪伴 - AI老人陪伴守护终端
 * 异常声音检测 - 检测呼救、跌倒等异常声音
 ****************************************************************************/

#ifndef __AI_SOUND_DETECT_H
#define __AI_SOUND_DETECT_H

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

/* 异常声音检测调试日志宏 */
#ifdef CONFIG_DEBUG_AI_SOUND_DETECT
#  define SOUND_DEBUG(...) do { printf("[SOUND] "); printf(__VA_ARGS__); \
                                printf("\n"); } while (0)
#else
#  define SOUND_DEBUG(...) do { } while (0)
#endif

/* 检测配置默认值 */
#define SOUND_DETECT_SAMPLE_RATE    16000   /* 16kHz采样率 */
#define SOUND_DETECT_FRAME_MS       30      /* 30ms每帧 */
#define SOUND_DETECT_WINDOW_MS      1000    /* 1秒检测窗口 */
#define SOUND_DETECT_CHANNELS       1       /* 单声道 */

/* 检测阈值 */
#define SOUND_DETECT_THRESHOLD_DEFAULT  0.7f   /* 默认置信度阈值 */
#define SOUND_DETECT_THRESHOLD_HIGH     0.85f  /* 高置信度阈值 */
#define SOUND_DETECT_THRESHOLD_LOW      0.5f   /* 低置信度阈值 */

/* 检测窗口帧数 */
#define SOUND_DETECT_FRAMES_PER_WINDOW \
    (SOUND_DETECT_SAMPLE_RATE * SOUND_DETECT_WINDOW_MS / 1000)

/* 模型配置 */
#define SOUND_DETECT_MAX_CLASSES      10      /* 最大分类数 */
#define SOUND_DETECT_MODEL_INPUT_SIZE 16000   /* 模型输入大小(采样点) */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* 异常声音类型 */
typedef enum
{
  SOUND_TYPE_NONE = 0,          /* 无异常 */
  SOUND_TYPE_HELP,              /* 呼救声 */
  SOUND_TYPE_SCREAM,            /* 异常喊叫 */
  SOUND_TYPE_FALL,              /* 跌倒撞击声 */
  SOUND_TYPE_KNOCK,             /* 敲击求救声 */
  SOUND_TYPE_GLASS_BREAK,       /* 玻璃破碎声 */
  SOUND_TYPE_DOOR_BANG,         /* 门撞击声 */
  SOUND_TYPE_CUSTOM_1,          /* 自定义类型1 */
  SOUND_TYPE_CUSTOM_2,          /* 自定义类型2 */
  SOUND_TYPE_CUSTOM_3,          /* 自定义类型3 */
  SOUND_TYPE_MAX                /* 类型数量上限 */
} sound_type_t;

/* 检测器状态 */
typedef enum
{
  DETECT_STATE_UNINIT = 0,      /* 未初始化 */
  DETECT_STATE_IDLE,            /* 空闲状态 */
  DETECT_STATE_COLLECTING,      /* 采集中 */
  DETECT_STATE_PROCESSING,      /* 处理中 */
  DETECT_STATE_DETECTED,        /* 检测到异常 */
  DETECT_STATE_ERROR            /* 错误状态 */
} detect_state_t;

/* 检测模式 */
typedef enum
{
  DETECT_MODE_REALTIME = 0,     /* 实时检测 */
  DETECT_MODE_BATCH,            /* 批量检测 */
  DETECT_MODE_TRIGGER           /* 触发检测 */
} detect_mode_t;

/* 检测结果回调 */
typedef void (*sound_detect_cb_t)(sound_type_t type,
                                  float confidence,
                                  void *user_data);

/* 检测配置 */
typedef struct
{
  detect_mode_t    mode;              /* 检测模式 */
  float            threshold;         /* 置信度阈值 */
  uint32_t         sample_rate;       /* 采样率 */
  uint8_t          frame_ms;          /* 帧长(毫秒) */
  bool             enable_vad;        /* 启用VAD预筛选 */
  bool             enable_feedback;   /* 启用检测反馈(蜂鸣器/LED) */
  sound_detect_cb_t callback;         /* 检测回调 */
  void            *user_data;         /* 用户数据 */
} sound_detect_config_t;

/* 单个分类信息 */
typedef struct
{
  sound_type_t     type;              /* 声音类型 */
  char             name[32];          /* 类型名称 */
  float            threshold;         /* 该类型阈值 */
  int              sample_count;      /* 训练样本数 */
  bool             enabled;           /* 是否启用 */
} sound_class_info_t;

/* 检测统计信息 */
typedef struct
{
  uint32_t     total_frames;          /* 总处理帧数 */
  uint32_t     detected_count;        /* 检测次数 */
  uint32_t     false_positive_count;  /* 误报次数 */
  uint32_t     last_detect_time;      /* 最后检测时间 */
  sound_type_t last_detect_type;      /* 最后检测类型 */
  float        last_confidence;       /* 最后置信度 */
} sound_detect_stats_t;

/* 模型元数据 */
typedef struct
{
  char         model_name[64];        /* 模型名称 */
  char         model_version[16];     /* 模型版本 */
  uint32_t     input_size;            /* 输入大小 */
  uint32_t     output_size;           /* 输出大小 */
  uint32_t     class_count;           /* 分类数量 */
  uint32_t     model_size;            /* 模型大小(字节) */
  uint32_t     created_time;          /* 创建时间戳 */
} sound_model_meta_t;

/* 检测器上下文 */
typedef struct
{
  detect_state_t      state;          /* 当前状态 */
  sound_detect_config_t config;       /* 检测配置 */
  bool                initialized;    /* 是否已初始化 */

  /* 模型相关 */
  void               *model_data;     /* 模型数据指针 */
  size_t              model_size;     /* 模型大小 */
  sound_model_meta_t  model_meta;     /* 模型元数据 */

  /* 分类信息 */
  sound_class_info_t  classes[SOUND_DETECT_MAX_CLASSES]; /* 分类信息 */
  int                 class_count;    /* 分类数量 */

  /* 音频缓冲 */
  int16_t            *audio_buffer;   /* 音频缓冲区 */
  size_t              buffer_size;    /* 缓冲区大小(采样点) */
  size_t              buffer_pos;     /* 当前缓冲位置 */

  /* 特征缓冲 */
  float              *feature_buffer; /* 特征缓冲区 */
  size_t              feature_size;   /* 特征大小 */

  /* 推理结果 */
  float               results[SOUND_DETECT_MAX_CLASSES]; /* 各类得分 */

  /* 统计信息 */
  sound_detect_stats_t stats;         /* 检测统计 */

  /* 线程相关 */
  pthread_t           detect_thread;  /* 检测线程 */
  volatile bool       detect_stop;    /* 停止检测标志 */
  bool                detect_thread_valid;
  pthread_mutex_t     buffer_lock;
  bool                buffer_lock_valid;

  /* 用户数据 */
  void               *user_data;      /* 用户自定义数据 */
} sound_detect_context_t;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/**
 * @brief  初始化声音检测模块
 * @param  ctx: 检测器上下文指针
 * @param  config: 检测配置, NULL使用默认配置
 * @return 0成功, 负值失败
 */

int sound_detect_init(sound_detect_context_t *ctx,
                      const sound_detect_config_t *config);

/**
 * @brief  反初始化声音检测模块
 * @param  ctx: 检测器上下文指针
 */

void sound_detect_deinit(sound_detect_context_t *ctx);

/**
 * @brief  加载检测模型
 * @param  ctx: 检测器上下文指针
 * @param  model_data: 模型数据
 * @param  model_size: 模型大小
 * @return 0成功, 负值失败
 */

int sound_detect_load_model(sound_detect_context_t *ctx,
                            const void *model_data, size_t model_size);

/**
 * @brief  从文件加载模型
 * @param  ctx: 检测器上下文指针
 * @param  model_path: 模型文件路径
 * @return 0成功, 负值失败
 */

int sound_detect_load_model_file(sound_detect_context_t *ctx,
                                 const char *model_path);

/**
 * @brief  卸载模型
 * @param  ctx: 检测器上下文指针
 */

void sound_detect_unload_model(sound_detect_context_t *ctx);

/**
 * @brief  开始实时检测
 * @param  ctx: 检测器上下文指针
 * @return 0成功, 负值失败
 */

int sound_detect_start(sound_detect_context_t *ctx);

/**
 * @brief  停止检测
 * @param  ctx: 检测器上下文指针
 */

void sound_detect_stop(sound_detect_context_t *ctx);

/**
 * @brief  输入音频数据进行检测
 * @param  ctx: 检测器上下文指针
 * @param  data: 音频数据(16bit PCM)
 * @param  frames: 帧数
 * @return 0成功, 负值失败
 */

int sound_detect_feed(sound_detect_context_t *ctx,
                      const int16_t *data, size_t frames);

/**
 * @brief  执行单次检测
 * @param  ctx: 检测器上下文指针
 * @param  data: 音频数据
 * @param  frames: 帧数
 * @param  type: 输出检测类型
 * @param  confidence: 输出置信度
 * @return 0成功, 负值失败
 */

int sound_detect_once(sound_detect_context_t *ctx,
                      const int16_t *data, size_t frames,
                      sound_type_t *type, float *confidence);

/**
 * @brief  获取检测结果
 * @param  ctx: 检测器上下文指针
 * @param  results: 输出各类得分数组
 * @param  count: 分类数量
 * @return 0成功, 负值失败
 */

int sound_detect_get_results(sound_detect_context_t *ctx,
                             float *results, int count);

/**
 * @brief  设置检测阈值
 * @param  ctx: 检测器上下文指针
 * @param  threshold: 置信度阈值
 */

void sound_detect_set_threshold(sound_detect_context_t *ctx,
                                float threshold);

/**
 * @brief  启用/禁用特定类型检测
 * @param  ctx: 检测器上下文指针
 * @param  type: 声音类型
 * @param  enable: 启用/禁用
 */

void sound_detect_enable_type(sound_detect_context_t *ctx,
                              sound_type_t type, bool enable);

/**
 * @brief  获取检测统计信息
 * @param  ctx: 检测器上下文指针
 * @return 统计信息指针
 */

const sound_detect_stats_t *sound_detect_get_stats(
    sound_detect_context_t *ctx);

/**
 * @brief  重置统计信息
 * @param  ctx: 检测器上下文指针
 */

void sound_detect_reset_stats(sound_detect_context_t *ctx);

/**
 * @brief  获取分类信息
 * @param  ctx: 检测器上下文指针
 * @param  type: 声音类型
 * @return 分类信息指针, NULL表示不存在
 */

const sound_class_info_t *sound_detect_get_class_info(
    sound_detect_context_t *ctx, sound_type_t type);

/**
 * @brief  获取检测器状态
 * @param  ctx: 检测器上下文指针
 * @return 状态枚举
 */

detect_state_t sound_detect_get_state(sound_detect_context_t *ctx);

/**
 * @brief  获取状态名称字符串
 * @param  state: 状态枚举
 * @return 状态名称字符串
 */

const char *sound_detect_get_state_name(detect_state_t state);

/**
 * @brief  获取声音类型名称字符串
 * @param  type: 声音类型
 * @return 类型名称字符串
 */

const char *sound_detect_get_type_name(sound_type_t type);

/**
 * @brief  提取音频特征(MFCC等)
 * @param  data: 音频数据
 * @param  frames: 帧数
 * @param  features: 输出特征数组
 * @param  feature_size: 特征大小
 * @return 0成功, 负值失败
 */

int sound_detect_extract_features(const int16_t *data, size_t frames,
                                  float *features, size_t feature_size);

/**
 * @brief  执行模型推理
 * @param  ctx: 检测器上下文指针
 * @param  features: 特征数据
 * @param  feature_size: 特征大小
 * @param  results: 输出各类得分
 * @return 0成功, 负值失败
 */

int sound_detect_inference(sound_detect_context_t *ctx,
                           const float *features, size_t feature_size,
                           float *results);

/**
 * @brief  初始化默认分类信息
 * @param  ctx: 检测器上下文指针
 */

void sound_detect_init_default_classes(sound_detect_context_t *ctx);

/**
 * @brief Edge Impulse适配入口。链接生成模型时提供同名强符号即可覆盖弱实现。
 */

int edge_impulse_sound_classify(const int16_t *data, size_t frames,
                                float *results, size_t result_count);

#endif /* __AI_SOUND_DETECT_H */
