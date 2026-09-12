/****************************************************************************
 * AI Sound Detection Module Implementation
 * 智爱陪伴 - AI老人陪伴守护终端
 * 异常声音检测 - 检测呼救、跌倒等异常声音
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "ai_sound_detect.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 特征提取参数 */
#define MFCC_NUM_FILTERS     26       /* Mel滤波器组数量 */
#define MFCC_NUM_COEFFS      13       /* MFCC系数数量 */
#define FFT_SIZE             512      /* FFT大小 */
#define HOP_LENGTH           160      /* 帧移 (10ms at 16kHz) */

/* 检测参数 */
#define DETECT_MIN_ENERGY    1000     /* 最小能量阈值 */
#define DETECT_COOLDOWN_MS   2000     /* 检测冷却时间 */

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void *sound_detect_thread(void *arg);
static int sound_detect_process_window(sound_detect_context_t *ctx);
static uint32_t sound_detect_get_tick_ms(void);
static float sound_detect_calc_energy(const int16_t *data, size_t frames);
static int sound_detect_extract_mfcc(const int16_t *data, size_t frames,
                                     float *mfcc, int num_coeffs);
static void sound_detect_add_result(sound_detect_context_t *ctx,
                                   sound_type_t type, float confidence);
static int sound_detect_run_model(sound_detect_context_t *ctx,
                                  const int16_t *data, size_t frames);
static int sound_detect_run_fallback(sound_detect_context_t *ctx,
                                     const int16_t *data, size_t frames);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 状态名称表 */
static const char *g_state_names[] =
{
  [DETECT_STATE_UNINIT]     = "UNINIT",
  [DETECT_STATE_IDLE]       = "IDLE",
  [DETECT_STATE_COLLECTING] = "COLLECTING",
  [DETECT_STATE_PROCESSING] = "PROCESSING",
  [DETECT_STATE_DETECTED]   = "DETECTED",
  [DETECT_STATE_ERROR]      = "ERROR"
};

/* 类型名称表 */
static const char *g_type_names[] =
{
  [SOUND_TYPE_NONE]         = "NONE",
  [SOUND_TYPE_HELP]         = "HELP",
  [SOUND_TYPE_SCREAM]       = "SCREAM",
  [SOUND_TYPE_FALL]         = "FALL",
  [SOUND_TYPE_KNOCK]        = "KNOCK",
  [SOUND_TYPE_GLASS_BREAK]  = "GLASS_BREAK",
  [SOUND_TYPE_DOOR_BANG]    = "DOOR_BANG",
  [SOUND_TYPE_CUSTOM_1]     = "CUSTOM_1",
  [SOUND_TYPE_CUSTOM_2]     = "CUSTOM_2",
  [SOUND_TYPE_CUSTOM_3]     = "CUSTOM_3"
};

/* Edge Impulse C++部署库可提供同名强符号。弱实现保证未导出模型时仍可构建。 */

__attribute__((weak))
int edge_impulse_sound_classify(const int16_t *data, size_t frames,
                                float *results, size_t result_count)
{
  (void)data;
  (void)frames;
  (void)results;
  (void)result_count;
  return -ENOSYS;
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * @brief  获取当前时间戳(毫秒)
 */

static uint32_t sound_detect_get_tick_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/**
 * @brief  计算音频能量
 */

static float sound_detect_calc_energy(const int16_t *data, size_t frames)
{
  if (data == NULL || frames == 0)
    {
      return 0;
    }

  uint64_t sum = 0;
  for (size_t i = 0; i < frames; i++)
    {
      sum += (int64_t)data[i] * data[i];
    }

  return (float)sum / frames;
}

/**
 * @brief  提取MFCC特征 (简化版本)
 * @note   实际实现需要FFT和Mel滤波器组
 */

static int sound_detect_extract_mfcc(const int16_t *data, size_t frames,
                                     float *mfcc, int num_coeffs)
{
  if (data == NULL || mfcc == NULL || num_coeffs <= 0)
    {
      return -EINVAL;
    }

  /* TODO: 实现完整的MFCC特征提取 */
  /*
   * 步骤:
   * 1. 预加重
   * 2. 分帧加窗
   * 3. FFT变换
   * 4. Mel滤波器组
   * 5. 对数能量
   * 6. DCT变换得到MFCC
   */

  /* 模拟特征提取 - 使用简单的统计特征 */

  float energy = sound_detect_calc_energy(data, frames);
  float mean = 0;
  float variance = 0;

  /* 计算均值 */

  for (size_t i = 0; i < frames; i++)
    {
      mean += data[i];
    }
  mean /= frames;

  /* 计算方差 */

  for (size_t i = 0; i < frames; i++)
    {
      float diff = data[i] - mean;
      variance += diff * diff;
    }
  variance /= frames;

  /* 生成模拟MFCC系数 */

  mfcc[0] = log(energy + 1);
  mfcc[1] = mean / 32768.0f;
  mfcc[2] = sqrt(variance) / 32768.0f;

  for (int i = 3; i < num_coeffs; i++)
    {
      mfcc[i] = mfcc[i - 1] * 0.9f + mfcc[i - 2] * 0.1f;
    }

  return OK;
}

/**
 * @brief  添加检测结果
 */

static void sound_detect_add_result(sound_detect_context_t *ctx,
                                   sound_type_t type, float confidence)
{
  if (ctx == NULL || type >= SOUND_TYPE_MAX)
    {
      return;
    }

  /* 更新统计 */

  ctx->stats.detected_count++;
  ctx->stats.last_detect_time = sound_detect_get_tick_ms();
  ctx->stats.last_detect_type = type;
  ctx->stats.last_confidence = confidence;

  SOUND_DEBUG("检测到异常: type=%s, confidence=%.2f",
              g_type_names[type], confidence);

  /* 调用回调 */

  if (ctx->config.callback != NULL)
    {
      ctx->config.callback(type, confidence, ctx->config.user_data);
    }
}

/**
 * @brief  执行模型推理 (模拟实现)
 */

static int sound_detect_run_fallback(sound_detect_context_t *ctx,
                                     const int16_t *data, size_t frames)
{
  float energy;
  float rms;
  float peak = 0.0f;
  size_t crossings = 0;

  if (ctx == NULL || data == NULL || frames == 0)
    {
      return -EINVAL;
    }

  memset(ctx->results, 0, sizeof(ctx->results));
  energy = sound_detect_calc_energy(data, frames);
  rms = sqrtf(energy) / 32768.0f;

  for (size_t i = 0; i < frames; i++)
    {
      float sample = fabsf((float)data[i]) / 32768.0f;
      if (sample > peak)
        {
          peak = sample;
        }

      if (i > 0 && ((data[i - 1] < 0 && data[i] >= 0) ||
                    (data[i - 1] >= 0 && data[i] < 0)))
        {
          crossings++;
        }
    }

  float zcr = (float)crossings / frames;
  ctx->results[SOUND_TYPE_NONE] = 0.95f;

  /* 这是无模型演示后备：只用于联调，不代替训练模型或医疗判断。 */

  if (peak > 0.75f && rms < 0.20f)
    {
      ctx->results[SOUND_TYPE_FALL] = 0.90f;
      ctx->results[SOUND_TYPE_NONE] = 0.10f;
    }
  else if (rms > 0.30f && zcr > 0.08f)
    {
      ctx->results[SOUND_TYPE_SCREAM] = 0.86f;
      ctx->results[SOUND_TYPE_NONE] = 0.14f;
    }
  else if (peak > 0.55f && rms < 0.30f)
    {
      ctx->results[SOUND_TYPE_KNOCK] = 0.78f;
      ctx->results[SOUND_TYPE_NONE] = 0.22f;
    }

  return OK;
}

static int sound_detect_run_model(sound_detect_context_t *ctx,
                                  const int16_t *data, size_t frames)
{
  int ret;

  if (ctx == NULL || data == NULL || frames == 0)
    {
      return -EINVAL;
    }

#ifdef CONFIG_HELLO_APP_EDGE_IMPULSE
  ret = edge_impulse_sound_classify(data, frames,
                                    ctx->results, SOUND_TYPE_MAX);
  if (ret < 0 && ret != -ENOSYS)
    {
      return ret;
    }
#else
  ret = -ENOSYS;
#endif

  if (ret == -ENOSYS)
    {
      ret = sound_detect_run_fallback(ctx, data, frames);
      if (ret < 0)
        {
          return ret;
        }
    }

  float max_score = 0;
  int max_idx = 0;

  for (int i = 0; i < SOUND_TYPE_MAX; i++)
    {
      if (ctx->results[i] > max_score)
        {
          max_score = ctx->results[i];
          max_idx = i;
        }
    }

  /* 检查是否超过阈值 */

  if (max_score >= ctx->config.threshold && max_idx != SOUND_TYPE_NONE)
    {
      SOUND_DEBUG("模型检测: type=%s, score=%.2f",
                  g_type_names[max_idx], max_score);

      /* 检查该类型是否启用 */

      if (ctx->classes[max_idx].enabled)
        {
          sound_detect_add_result(ctx, max_idx, max_score);
          return OK;
        }
    }

  return -ENODATA;
}

/**
 * @brief  处理检测窗口
 */

static int sound_detect_process_window(sound_detect_context_t *ctx)
{
  if (ctx == NULL)
    {
      return -EINVAL;
    }

  int ret;

  SOUND_DEBUG("处理检测窗口: %zu 采样点", ctx->buffer_pos);

  /* 检查能量是否足够 */

  float energy = sound_detect_calc_energy(ctx->audio_buffer, ctx->buffer_pos);
  if (energy < DETECT_MIN_ENERGY)
    {
      SOUND_DEBUG("能量过低 (%.0f), 跳过检测", energy);
      return -ENODATA;
    }

  /* 提取特征 */

  ret = sound_detect_extract_mfcc(ctx->audio_buffer, ctx->buffer_pos,
                                  ctx->feature_buffer, MFCC_NUM_COEFFS);
  if (ret < 0)
    {
      SOUND_DEBUG("特征提取失败: %d", ret);
      return ret;
    }

  /* 运行模型 */

  ret = sound_detect_run_model(ctx, ctx->audio_buffer, ctx->buffer_pos);
  if (ret < 0 && ret != -ENODATA)
    {
      SOUND_DEBUG("模型推理失败: %d", ret);
      return ret;
    }

  return OK;
}

/**
 * @brief  检测线程
 */

static void *sound_detect_thread(void *arg)
{
  sound_detect_context_t *ctx = (sound_detect_context_t *)arg;

  SOUND_DEBUG("检测线程启动");

  while (!ctx->detect_stop)
    {
      /* 检查是否有足够的数据 */

      pthread_mutex_lock(&ctx->buffer_lock);
      if (ctx->buffer_pos >= SOUND_DETECT_FRAMES_PER_WINDOW)
        {
          ctx->state = DETECT_STATE_PROCESSING;

          /* 处理检测窗口 */

          sound_detect_process_window(ctx);

          /* 移动缓冲区 */

          size_t remaining = ctx->buffer_pos - SOUND_DETECT_FRAMES_PER_WINDOW;
          if (remaining > 0)
            {
              memmove(ctx->audio_buffer,
                      ctx->audio_buffer + SOUND_DETECT_FRAMES_PER_WINDOW,
                      remaining * sizeof(int16_t));
            }

          ctx->buffer_pos = remaining;
          ctx->state = DETECT_STATE_COLLECTING;
        }
      pthread_mutex_unlock(&ctx->buffer_lock);

      /* 休眠等待 */

      usleep(10000); /* 10ms */
    }

  SOUND_DEBUG("检测线程退出");
  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/**
 * @brief  初始化声音检测模块
 */

int sound_detect_init(sound_detect_context_t *ctx,
                      const sound_detect_config_t *config)
{
  if (ctx == NULL)
    {
      return -EINVAL;
    }

  SOUND_DEBUG("初始化声音检测模块");

  /* 清零上下文 */

  memset(ctx, 0, sizeof(sound_detect_context_t));

  /* 设置默认配置 */

  if (config != NULL)
    {
      memcpy(&ctx->config, config, sizeof(sound_detect_config_t));
    }
  else
    {
      ctx->config.mode = DETECT_MODE_REALTIME;
      ctx->config.threshold = SOUND_DETECT_THRESHOLD_DEFAULT;
      ctx->config.sample_rate = SOUND_DETECT_SAMPLE_RATE;
      ctx->config.frame_ms = SOUND_DETECT_FRAME_MS;
      ctx->config.enable_vad = true;
      ctx->config.enable_feedback = true;
    }

  /* 分配音频缓冲区 */

  ctx->buffer_size = SOUND_DETECT_FRAMES_PER_WINDOW * 2; /* 2秒缓冲 */
  ctx->audio_buffer = (int16_t *)malloc(ctx->buffer_size * sizeof(int16_t));
  if (ctx->audio_buffer == NULL)
    {
      SOUND_DEBUG("分配音频缓冲区失败");
      return -ENOMEM;
    }

  /* 分配特征缓冲区 */

  ctx->feature_size = MFCC_NUM_COEFFS;
  ctx->feature_buffer = (float *)malloc(ctx->feature_size * sizeof(float));
  if (ctx->feature_buffer == NULL)
    {
      SOUND_DEBUG("分配特征缓冲区失败");
      free(ctx->audio_buffer);
      return -ENOMEM;
    }

  int ret = pthread_mutex_init(&ctx->buffer_lock, NULL);
  if (ret != 0)
    {
      free(ctx->feature_buffer);
      free(ctx->audio_buffer);
      return -ret;
    }

  ctx->buffer_lock_valid = true;

  /* 初始化默认分类 */

  sound_detect_init_default_classes(ctx);

  ctx->state = DETECT_STATE_IDLE;
  ctx->initialized = true;

  SOUND_DEBUG("声音检测模块初始化完成");
  SOUND_DEBUG("  采样率: %d Hz", ctx->config.sample_rate);
  SOUND_DEBUG("  检测窗口: %d ms", SOUND_DETECT_WINDOW_MS);
  SOUND_DEBUG("  阈值: %.2f", ctx->config.threshold);

  return OK;
}

/**
 * @brief  反初始化声音检测模块
 */

void sound_detect_deinit(sound_detect_context_t *ctx)
{
  if (ctx == NULL || !ctx->initialized)
    {
      return;
    }

  SOUND_DEBUG("反初始化声音检测模块");

  /* 停止检测 */

  sound_detect_stop(ctx);

  /* 卸载模型 */

  sound_detect_unload_model(ctx);

  if (ctx->buffer_lock_valid)
    {
      pthread_mutex_destroy(&ctx->buffer_lock);
      ctx->buffer_lock_valid = false;
    }

  /* 释放缓冲区 */

  if (ctx->audio_buffer != NULL)
    {
      free(ctx->audio_buffer);
      ctx->audio_buffer = NULL;
    }

  if (ctx->feature_buffer != NULL)
    {
      free(ctx->feature_buffer);
      ctx->feature_buffer = NULL;
    }

  ctx->initialized = false;
  ctx->state = DETECT_STATE_UNINIT;

  SOUND_DEBUG("声音检测模块已反初始化");
}

/**
 * @brief  加载检测模型
 */

int sound_detect_load_model(sound_detect_context_t *ctx,
                            const void *model_data, size_t model_size)
{
  if (ctx == NULL || model_data == NULL || model_size == 0)
    {
      return -EINVAL;
    }

  SOUND_DEBUG("加载检测模型: %zu 字节", model_size);

  /* 释放旧模型 */

  sound_detect_unload_model(ctx);

  /* 复制模型数据 */

  ctx->model_data = malloc(model_size);
  if (ctx->model_data == NULL)
    {
      SOUND_DEBUG("分配模型内存失败");
      return -ENOMEM;
    }

  memcpy(ctx->model_data, model_data, model_size);
  ctx->model_size = model_size;

  /* TODO: 解析模型元数据 */

  /* 模拟模型元数据 */

  strncpy(ctx->model_meta.model_name, "sound_detect_v1",
          sizeof(ctx->model_meta.model_name) - 1);
  strncpy(ctx->model_meta.model_version, "1.0",
          sizeof(ctx->model_meta.model_version) - 1);
  ctx->model_meta.input_size = SOUND_DETECT_MODEL_INPUT_SIZE;
  ctx->model_meta.output_size = SOUND_TYPE_MAX;
  ctx->model_meta.class_count = SOUND_TYPE_MAX;
  ctx->model_meta.model_size = model_size;

  SOUND_DEBUG("模型加载成功: %s v%s",
              ctx->model_meta.model_name,
              ctx->model_meta.model_version);

  return OK;
}

/**
 * @brief  从文件加载模型
 */

int sound_detect_load_model_file(sound_detect_context_t *ctx,
                                 const char *model_path)
{
  if (ctx == NULL || model_path == NULL)
    {
      return -EINVAL;
    }

  SOUND_DEBUG("从文件加载模型: %s", model_path);

  FILE *file = fopen(model_path, "rb");
  if (file == NULL)
    {
      return -errno;
    }

  if (fseek(file, 0, SEEK_END) != 0)
    {
      int error = errno;
      fclose(file);
      return -error;
    }

  long length = ftell(file);
  if (length <= 0 || fseek(file, 0, SEEK_SET) != 0)
    {
      fclose(file);
      return -EINVAL;
    }

  void *data = malloc((size_t)length);
  if (data == NULL)
    {
      fclose(file);
      return -ENOMEM;
    }

  size_t read_size = fread(data, 1, (size_t)length, file);
  fclose(file);
  if (read_size != (size_t)length)
    {
      free(data);
      return -EIO;
    }

  int ret = sound_detect_load_model(ctx, data, read_size);
  free(data);
  return ret;
}

/**
 * @brief  卸载模型
 */

void sound_detect_unload_model(sound_detect_context_t *ctx)
{
  if (ctx == NULL)
    {
      return;
    }

  if (ctx->model_data != NULL)
    {
      SOUND_DEBUG("卸载检测模型");
      free(ctx->model_data);
      ctx->model_data = NULL;
      ctx->model_size = 0;
    }
}

/**
 * @brief  开始实时检测
 */

int sound_detect_start(sound_detect_context_t *ctx)
{
  if (ctx == NULL || !ctx->initialized)
    {
      return -EINVAL;
    }

  if (ctx->state == DETECT_STATE_COLLECTING)
    {
      SOUND_DEBUG("已在检测中");
      return -EBUSY;
    }

  SOUND_DEBUG("开始声音检测");

  /* 清空缓冲区 */

  ctx->buffer_pos = 0;
  ctx->detect_stop = false;
  memset(&ctx->stats, 0, sizeof(ctx->stats));

  /* 启动检测线程 */

  int ret = pthread_create(&ctx->detect_thread, NULL,
                           sound_detect_thread, ctx);
  if (ret != 0)
    {
      SOUND_DEBUG("创建检测线程失败: %d", ret);
      ctx->state = DETECT_STATE_ERROR;
      return -ret;
    }

  ctx->state = DETECT_STATE_COLLECTING;
  ctx->detect_thread_valid = true;

  return OK;
}

/**
 * @brief  停止检测
 */

void sound_detect_stop(sound_detect_context_t *ctx)
{
  if (ctx == NULL || !ctx->detect_thread_valid)
    {
      return;
    }

  SOUND_DEBUG("停止声音检测");

  /* 设置停止标志 */

  ctx->detect_stop = true;

  /* 等待线程退出 */

  pthread_join(ctx->detect_thread, NULL);
  ctx->detect_thread_valid = false;

  ctx->state = DETECT_STATE_IDLE;
}

/**
 * @brief  输入音频数据进行检测
 */

int sound_detect_feed(sound_detect_context_t *ctx,
                      const int16_t *data, size_t frames)
{
  if (ctx == NULL || data == NULL || frames == 0)
    {
      return -EINVAL;
    }

  if (ctx->state != DETECT_STATE_COLLECTING)
    {
      return -EINVAL;
    }

  if (frames > ctx->buffer_size)
    {
      data += frames - ctx->buffer_size;
      frames = ctx->buffer_size;
    }

  pthread_mutex_lock(&ctx->buffer_lock);

  /* 检查缓冲区空间 */

  size_t free_space = ctx->buffer_size - ctx->buffer_pos;
  if (frames > free_space)
    {
      /* 缓冲区满，丢弃最旧的数据 */

      size_t discard = frames - free_space;
      memmove(ctx->audio_buffer,
              ctx->audio_buffer + discard,
              (ctx->buffer_pos - discard) * sizeof(int16_t));
      ctx->buffer_pos -= discard;
    }

  /* 复制数据到缓冲区 */

  memcpy(ctx->audio_buffer + ctx->buffer_pos, data,
         frames * sizeof(int16_t));
  ctx->buffer_pos += frames;

  ctx->stats.total_frames += frames;

  pthread_mutex_unlock(&ctx->buffer_lock);

  return OK;
}

/**
 * @brief  执行单次检测
 */

int sound_detect_once(sound_detect_context_t *ctx,
                      const int16_t *data, size_t frames,
                      sound_type_t *type, float *confidence)
{
  if (ctx == NULL || data == NULL || type == NULL || confidence == NULL)
    {
      return -EINVAL;
    }

  *type = SOUND_TYPE_NONE;
  *confidence = 0;

  /* 检查能量 */

  float energy = sound_detect_calc_energy(data, frames);
  if (energy < DETECT_MIN_ENERGY)
    {
      return -ENODATA;
    }

  /* 提取特征 */

  float features[MFCC_NUM_COEFFS];
  int ret = sound_detect_extract_mfcc(data, frames,
                                      features, MFCC_NUM_COEFFS);
  if (ret < 0)
    {
      return ret;
    }

  /* 运行模型 */

  ret = sound_detect_run_model(ctx, data, frames);
  if (ret < 0 && ret != -ENODATA)
    {
      return ret;
    }

  /* 找到最高分 */

  float max_score = 0;
  int max_idx = 0;

  for (int i = 0; i < SOUND_TYPE_MAX; i++)
    {
      if (ctx->results[i] > max_score)
        {
          max_score = ctx->results[i];
          max_idx = i;
        }
    }

  if (max_score >= ctx->config.threshold && max_idx != SOUND_TYPE_NONE)
    {
      *type = max_idx;
      *confidence = max_score;
      return OK;
    }

  return -ENODATA;
}

/**
 * @brief  获取检测结果
 */

int sound_detect_get_results(sound_detect_context_t *ctx,
                             float *results, int count)
{
  if (ctx == NULL || results == NULL)
    {
      return -EINVAL;
    }

  int copy_count = count < SOUND_TYPE_MAX ? count : SOUND_TYPE_MAX;
  memcpy(results, ctx->results, copy_count * sizeof(float));

  return copy_count;
}

/**
 * @brief  设置检测阈值
 */

void sound_detect_set_threshold(sound_detect_context_t *ctx,
                                float threshold)
{
  if (ctx == NULL)
    {
      return;
    }

  SOUND_DEBUG("设置检测阈值: %.2f", threshold);

  ctx->config.threshold = threshold;

  /* 更新各分类阈值 */

  for (int i = 0; i < SOUND_TYPE_MAX; i++)
    {
      ctx->classes[i].threshold = threshold;
    }
}

/**
 * @brief  启用/禁用特定类型检测
 */

void sound_detect_enable_type(sound_detect_context_t *ctx,
                              sound_type_t type, bool enable)
{
  if (ctx == NULL || type >= SOUND_TYPE_MAX)
    {
      return;
    }

  SOUND_DEBUG("设置类型 %s: %s",
              g_type_names[type], enable ? "启用" : "禁用");

  ctx->classes[type].enabled = enable;
}

/**
 * @brief  获取检测统计信息
 */

const sound_detect_stats_t *sound_detect_get_stats(
    sound_detect_context_t *ctx)
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

void sound_detect_reset_stats(sound_detect_context_t *ctx)
{
  if (ctx == NULL)
    {
      return;
    }

  SOUND_DEBUG("重置统计信息");
  memset(&ctx->stats, 0, sizeof(ctx->stats));
}

/**
 * @brief  获取分类信息
 */

const sound_class_info_t *sound_detect_get_class_info(
    sound_detect_context_t *ctx, sound_type_t type)
{
  if (ctx == NULL || type >= SOUND_TYPE_MAX)
    {
      return NULL;
    }

  return &ctx->classes[type];
}

/**
 * @brief  获取检测器状态
 */

detect_state_t sound_detect_get_state(sound_detect_context_t *ctx)
{
  if (ctx == NULL)
    {
      return DETECT_STATE_UNINIT;
    }

  return ctx->state;
}

/**
 * @brief  获取状态名称字符串
 */

const char *sound_detect_get_state_name(detect_state_t state)
{
  if (state >= DETECT_STATE_UNINIT && state <= DETECT_STATE_ERROR)
    {
      return g_state_names[state];
    }

  return "UNKNOWN";
}

/**
 * @brief  获取声音类型名称字符串
 */

const char *sound_detect_get_type_name(sound_type_t type)
{
  if (type >= SOUND_TYPE_NONE && type < SOUND_TYPE_MAX)
    {
      return g_type_names[type];
    }

  return "UNKNOWN";
}

/**
 * @brief  提取音频特征
 */

int sound_detect_extract_features(const int16_t *data, size_t frames,
                                  float *features, size_t feature_size)
{
  return sound_detect_extract_mfcc(data, frames, features, feature_size);
}

/**
 * @brief  执行模型推理
 */

int sound_detect_inference(sound_detect_context_t *ctx,
                           const float *features, size_t feature_size,
                           float *results)
{
  if (ctx == NULL || features == NULL || results == NULL)
    {
      return -EINVAL;
    }

  (void)feature_size;

  /* 当前适配器接收原始PCM；保留旧API但不伪造推理结果。 */

  return -ENOTSUP;
}

/**
 * @brief  初始化默认分类信息
 */

void sound_detect_init_default_classes(sound_detect_context_t *ctx)
{
  if (ctx == NULL)
    {
      return;
    }

  /* 呼救声 */

  ctx->classes[SOUND_TYPE_HELP].type = SOUND_TYPE_HELP;
  strncpy(ctx->classes[SOUND_TYPE_HELP].name, "呼救声",
          sizeof(ctx->classes[SOUND_TYPE_HELP].name) - 1);
  ctx->classes[SOUND_TYPE_HELP].threshold = SOUND_DETECT_THRESHOLD_HIGH;
  ctx->classes[SOUND_TYPE_HELP].enabled = true;

  /* 异常喊叫 */

  ctx->classes[SOUND_TYPE_SCREAM].type = SOUND_TYPE_SCREAM;
  strncpy(ctx->classes[SOUND_TYPE_SCREAM].name, "异常喊叫",
          sizeof(ctx->classes[SOUND_TYPE_SCREAM].name) - 1);
  ctx->classes[SOUND_TYPE_SCREAM].threshold = SOUND_DETECT_THRESHOLD_DEFAULT;
  ctx->classes[SOUND_TYPE_SCREAM].enabled = true;

  /* 跌倒撞击 */

  ctx->classes[SOUND_TYPE_FALL].type = SOUND_TYPE_FALL;
  strncpy(ctx->classes[SOUND_TYPE_FALL].name, "跌倒撞击",
          sizeof(ctx->classes[SOUND_TYPE_FALL].name) - 1);
  ctx->classes[SOUND_TYPE_FALL].threshold = SOUND_DETECT_THRESHOLD_DEFAULT;
  ctx->classes[SOUND_TYPE_FALL].enabled = true;

  /* 敲击求救 */

  ctx->classes[SOUND_TYPE_KNOCK].type = SOUND_TYPE_KNOCK;
  strncpy(ctx->classes[SOUND_TYPE_KNOCK].name, "敲击求救",
          sizeof(ctx->classes[SOUND_TYPE_KNOCK].name) - 1);
  ctx->classes[SOUND_TYPE_KNOCK].threshold = SOUND_DETECT_THRESHOLD_DEFAULT;
  ctx->classes[SOUND_TYPE_KNOCK].enabled = true;

  /* 玻璃破碎 */

  ctx->classes[SOUND_TYPE_GLASS_BREAK].type = SOUND_TYPE_GLASS_BREAK;
  strncpy(ctx->classes[SOUND_TYPE_GLASS_BREAK].name, "玻璃破碎",
          sizeof(ctx->classes[SOUND_TYPE_GLASS_BREAK].name) - 1);
  ctx->classes[SOUND_TYPE_GLASS_BREAK].threshold =
    SOUND_DETECT_THRESHOLD_DEFAULT;
  ctx->classes[SOUND_TYPE_GLASS_BREAK].enabled = true;

  /* 门撞击 */

  ctx->classes[SOUND_TYPE_DOOR_BANG].type = SOUND_TYPE_DOOR_BANG;
  strncpy(ctx->classes[SOUND_TYPE_DOOR_BANG].name, "门撞击",
          sizeof(ctx->classes[SOUND_TYPE_DOOR_BANG].name) - 1);
  ctx->classes[SOUND_TYPE_DOOR_BANG].threshold =
    SOUND_DETECT_THRESHOLD_DEFAULT;
  ctx->classes[SOUND_TYPE_DOOR_BANG].enabled = true;

  ctx->class_count = SOUND_TYPE_MAX;

  SOUND_DEBUG("初始化默认分类: %d 个", ctx->class_count);
}
