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
#include <sched.h>
#include <time.h>
#include <errno.h>
#include <syslog.h>

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
/* 连续命中几个检测窗口才上报（见 sound_detect_run_model 里的确认逻辑）*/
#define SOUND_DETECT_CONFIRM_WINDOWS  2

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
          /* 同一个类别连续 SOUND_DETECT_CONFIRM_WINDOWS 个窗口都超阈值才上报。
           * 单窗口就报的话，一声脆响（点击声、喇叭自己的提示音、关门声）都会被
           * 判成"跌倒"——现场就是这个现象。真跌倒的声音是持续的，多要一个窗口
           * 代价是确认慢约 1 秒（一个窗口 1 秒）。 */
          if (ctx->confirm_type == max_idx)
            {
              ctx->confirm_count++;
            }
          else
            {
              ctx->confirm_type = max_idx;
              ctx->confirm_count = 1;
            }

          if (ctx->confirm_count >= SOUND_DETECT_CONFIRM_WINDOWS)
            {
              ctx->confirm_type = SOUND_TYPE_NONE;
              ctx->confirm_count = 0;
              sound_detect_add_result(ctx, max_idx, max_score);
            }

          return OK;
        }
    }

  /* 这一窗没有超阈值的类别：清掉待确认计数，免得"隔几秒响一下"被拼成连续 */
  ctx->confirm_type = SOUND_TYPE_NONE;
  ctx->confirm_count = 0;

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

#if !SOUND_DETECT_HEURISTIC_ENABLE
  /* 内置的启发式检测已被关掉：不起线程，也不进入 COLLECTING 状态，
   * 只是把回调留着 —— 等模型（或别的来源）调 sound_detect_report_anomaly()。 */
  ctx->state = DETECT_STATE_IDLE;
  syslog(LOG_INFO,
         "[sound_detect] 内置启发式检测已关闭（SOUND_DETECT_HEURISTIC_ENABLE=0），"
         "仅保留 sound_detect_report_anomaly() 上报入口\n");
  return OK;
#endif

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

#if !SOUND_DETECT_HEURISTIC_ENABLE
  /* 启发式检测关着：音频直接丢掉，不占缓冲也不算错误。
   * 上层照旧喂（录音回调里那句），一行都不用改。 */
  (void)ctx;
  (void)data;
  (void)frames;
  return 0;
#else
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
#endif /* SOUND_DETECT_HEURISTIC_ENABLE */

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

int sound_detect_report_anomaly(sound_detect_context_t *ctx,
                                sound_type_t type, float confidence)
{
  if (ctx == NULL || !ctx->initialized ||
      type <= SOUND_TYPE_NONE || type >= SOUND_TYPE_MAX)
    {
      return -EINVAL;
    }

  if (confidence < 0.0f)
    {
      confidence = 0.0f;
    }
  else if (confidence > 1.0f)
    {
      confidence = 1.0f;
    }

  /* 与内置检测器判出结果时走同一条路：统计 + 回调（报警页 / 响铃 / 推送） */
  sound_detect_add_result(ctx, type, confidence);
  return OK;
}

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

/****************************************************************************
 * 任务四：PCM 旁路 + 人声/非人声门控（实现）
 *
 * 详细设计见 ai_sound_detect.h 里那一段。这里只强调三条硬约束：
 *   1) tap（录音线程调用）只做一次 memcpy，锁用 trylock，**绝不等待**；
 *   2) 门控线程独立、优先级低于 app，慢了只丢窗；
 *   3) 播放态（板子自己喇叭在响）整窗跳过 —— 自己的 TTS 不是异常声。
 ****************************************************************************/

/* 门限：全部按 sensitivity 缩放（sens 越大越敏感 = 门限越低）*/
#define SOUND_GATE_SILENCE_RMS       0.004f  /* 静音底噪（-48dBFS 量级）*/
#define SOUND_GATE_ANOMALY_RMS       0.020f  /* 喂本地模型的最小能量（-34dBFS）*/
#define SOUND_GATE_VOICED_ZCR_MAX    0.35f   /* 浊音帧过零率上限 */
#define SOUND_GATE_VOICED_AC_MIN     0.30f   /* 单帧"浊音样"的自相关峰下限 */
#define SOUND_GATE_VOICED_AC_STRONG  0.60f   /* 整窗强周期性判据 */
#define SOUND_GATE_VOICED_RATIO_MIN  0.25f   /* 浊音帧占比下限 */
#define SOUND_GATE_FRAME_SAMPLES     512     /* 32ms 分析帧（自相关用它）*/
#define SOUND_GATE_LAG_MIN           40      /* 16k/400Hz：人声基频上界 */
#define SOUND_GATE_LAG_MAX           200     /* 16k/80Hz：人声基频下界 */
#define SOUND_GATE_LAG_STEP          2
#define SOUND_GATE_SENS_MIN          0.3f
#define SOUND_GATE_SENS_MAX          3.0f

/* 旁路环形缓冲与门控线程状态（单麦克风 → 单实例）*/
static int16_t        *g_gate_ring;
static size_t          g_gate_ring_head;
static size_t          g_gate_ring_count;
static pthread_mutex_t g_gate_lock;
static bool            g_gate_lock_valid;
static pthread_t       g_gate_thread;
static bool            g_gate_thread_valid;
static volatile bool   g_gate_stop;
static volatile bool   g_gate_enabled;
static volatile float  g_gate_sensitivity = 1.0f;
static sound_detect_busy_cb_t  g_gate_busy_cb;
static void                   *g_gate_busy_arg;
static sound_detect_voice_cb_t g_gate_voice_cb;
static void                   *g_gate_voice_arg;
static sound_gate_stats_t      g_gate_stats;

/* 任务三 / robot_ui 的弱实现兜底：对方落地后强符号覆盖（见 ai_sound_detect.h）。
 *
 * ⚠️ sound_event 这两个弱桩必须**只在对方根本没进构建**时存在。原因不是
 * 「强符号覆盖弱符号」不够灵，而是静态库的懒加载：libapps_ai_companion.a 里
 * ai_sound_detect.c.o 排在 sound_event.c.o 前面，链接器找 sound_event_init
 * 时先命中的就是这里的弱定义，于是 sound_event.c.o **整个成员都不会被拉进
 * 镜像**，强符号从来没机会参与竞争 —— 结果是编过、链过、跑起来模型却是死的。
 * 所以按构建开关分家：开了 CONFIG_HELLO_APP_SOUND_EVENT 就把强符号留给
 * sound_event.c 独占，只有没开时才拿这里的弱桩顶住链接。
 *
 * （ui_post_ask_alarm 不存在这个问题：它的强符号在 robot_ui/main.c，那个
 * 成员因为别的符号必然被拉进镜像，弱定义能被正常覆盖。） */

#ifndef CONFIG_HELLO_APP_SOUND_EVENT

__attribute__((weak))
int sound_event_init(void)
{
  return -ENOSYS;
}

__attribute__((weak))
int sound_event_feed(const int16_t *pcm, size_t nsamples)
{
  (void)pcm;
  (void)nsamples;
  return -ENOSYS;
}

#endif /* !CONFIG_HELLO_APP_SOUND_EVENT */

__attribute__((weak))
void ui_post_ask_alarm(const char *reason)
{
  printf("[门控] ui_post_ask_alarm() 还没落地，询问框请求只能打串口: %s\n",
         reason != NULL ? reason : "(无)");
}

__attribute__((weak))
void ui_post_ask_standdown(const char *src)
{
  /* 询问页本来就不在这个镜像里，没有页面可撤 —— 这不是错，只是无从说起。 */
  (void)src;
}

__attribute__((weak))
bool robot_ui_alarm_claim(const char *src)
{
  /* robot_ui 没进镜像（或者那个模块还没落地）：屏幕上根本没有第二条确认入口，
   * 没有谁能和本路重复报警，所以一律放行 —— 去重闸宁可多报一次也不能漏报。 */
  (void)src;
  return true;
}

/* ---- 环形缓冲（调者必须持锁）---- */

static void gate_ring_drop(size_t n)
{
  if (n > g_gate_ring_count)
    {
      n = g_gate_ring_count;
    }

  g_gate_ring_head = (g_gate_ring_head + n) % SOUND_GATE_RING_SAMPLES;
  g_gate_ring_count -= n;
}

static void gate_ring_push(const int16_t *pcm, size_t n)
{
  while (n > 0)
    {
      size_t space = SOUND_GATE_RING_SAMPLES - g_gate_ring_count;
      size_t tail;
      size_t first;

      if (space == 0)
        {
          /* 队列满：丢最旧的样本（一次最多丢一个跳长，保住窗对齐）*/
          gate_ring_drop(n < SOUND_GATE_HOP_SAMPLES ? n
                                                     : SOUND_GATE_HOP_SAMPLES);
          continue;
        }

      if (space > n)
        {
          space = n;
        }

      tail = (g_gate_ring_head + g_gate_ring_count) % SOUND_GATE_RING_SAMPLES;
      first = SOUND_GATE_RING_SAMPLES - tail;
      if (first > space)
        {
          first = space;
        }

      memcpy(&g_gate_ring[tail], pcm, first * sizeof(int16_t));
      if (space > first)
        {
          memcpy(&g_gate_ring[0], pcm + first,
                 (space - first) * sizeof(int16_t));
        }

      g_gate_ring_count += space;
      pcm += space;
      n -= space;
    }
}

static void gate_ring_peek(int16_t *dst, size_t n)
{
  size_t first = SOUND_GATE_RING_SAMPLES - g_gate_ring_head;

  if (first > n)
    {
      first = n;
    }

  memcpy(dst, &g_gate_ring[g_gate_ring_head], first * sizeof(int16_t));
  if (n > first)
    {
      memcpy(dst + first, &g_gate_ring[0],
             (n - first) * sizeof(int16_t));
    }
}

/* ---- 门控判据（纯函数）---- */

int sound_detect_voice_gate_ex(const int16_t *pcm, size_t n,
                               sound_gate_features_t *out)
{
  sound_gate_features_t f;
  uint64_t acc = 0;
  size_t crossings = 0;
  int frames = 0;
  int voiced_frames = 0;
  float ac_peak = 0.0f;
  float sens;
  float mean_sq;

  if (pcm == NULL || n == 0)
    {
      return -EINVAL;
    }

  memset(&f, 0, sizeof(f));

  /* 整窗能量 + 过零率 */

  for (size_t i = 0; i < n; i++)
    {
      int32_t s = pcm[i];
      acc += (uint64_t)((int64_t)s * s);

      if (i > 0 && ((pcm[i - 1] < 0 && s >= 0) ||
                    (pcm[i - 1] >= 0 && s < 0)))
        {
          crossings++;
        }
    }

  mean_sq = (float)((double)acc / (double)n);
  f.energy = mean_sq;
  f.rms = sqrtf(mean_sq) / 32768.0f;
  f.zcr = (n > 1) ? (float)crossings / (float)(n - 1) : 0.0f;

  /* 逐 32ms 帧算自相关峰值比（浊音性）和帧过零率。
   * 自相关对**周期性**敏感：人声浊音段高、宽带冲击/噪声低 —— 这正是
   * "一声脆响"和"有人在说话"最好分的地方。 */

  for (size_t off = 0; off + SOUND_GATE_FRAME_SAMPLES <= n;
       off += SOUND_GATE_FRAME_SAMPLES)
    {
      float x[SOUND_GATE_FRAME_SAMPLES];
      float r0 = 0.0f;
      size_t fcross = 0;
      float best = 0.0f;
      float fzcr;

      for (int i = 0; i < SOUND_GATE_FRAME_SAMPLES; i++)
        {
          x[i] = (float)pcm[off + i] / 32768.0f;
          r0 += x[i] * x[i];

          if (i > 0 && ((x[i - 1] < 0 && x[i] >= 0) ||
                        (x[i - 1] >= 0 && x[i] < 0)))
            {
              fcross++;
            }
        }

      frames++;

      if (r0 < 1e-9f)
        {
          continue;    /* 静音帧：没有周期可谈，也不算浊音 */
        }

      for (int lag = SOUND_GATE_LAG_MIN; lag <= SOUND_GATE_LAG_MAX;
           lag += SOUND_GATE_LAG_STEP)
        {
          float r = 0.0f;
          float norm;

          for (int i = 0; i + lag < SOUND_GATE_FRAME_SAMPLES; i++)
            {
              r += x[i] * x[i + lag];
            }

          norm = r / r0;
          if (norm > best)
            {
              best = norm;
            }
        }

      if (best > ac_peak)
        {
          ac_peak = best;
        }

      fzcr = (float)fcross / (float)(SOUND_GATE_FRAME_SAMPLES - 1);
      if (best >= SOUND_GATE_VOICED_AC_MIN &&
          fzcr <= SOUND_GATE_VOICED_ZCR_MAX)
        {
          voiced_frames++;
        }
    }

  f.ac_peak = ac_peak;
  f.voiced_ratio = (frames > 0) ? (float)voiced_frames / (float)frames : 0.0f;

  sens = g_gate_sensitivity;
  if (sens < SOUND_GATE_SENS_MIN)
    {
      sens = SOUND_GATE_SENS_MIN;
    }
  else if (sens > SOUND_GATE_SENS_MAX)
    {
      sens = SOUND_GATE_SENS_MAX;
    }

  f.active = (f.rms >= SOUND_GATE_SILENCE_RMS / sens) ? 1 : 0;
  f.voice = 0;

  if (f.active)
    {
      /* 像人声的两条路：
       *   ① 有一定比例的"浊音样"帧（正常说话）；
       *   ② 整窗强周期性且过零率不高（长元音 / 拖长音的呼救）。 */
      if (f.voiced_ratio >= SOUND_GATE_VOICED_RATIO_MIN / sens ||
          (f.ac_peak >= SOUND_GATE_VOICED_AC_STRONG / sens &&
           f.zcr <= SOUND_GATE_VOICED_ZCR_MAX))
        {
          f.voice = 1;
        }
    }

  if (out != NULL)
    {
      *out = f;
    }

  return f.voice;
}

int sound_detect_voice_gate(const int16_t *pcm, size_t n)
{
  return sound_detect_voice_gate_ex(pcm, n, NULL);
}

/* ---- 旁路入口（录音线程里调）---- */

int sound_detect_pcm_tap(const int16_t *pcm, size_t n)
{
  if (pcm == NULL || n == 0)
    {
      return -EINVAL;
    }

  if (!g_gate_enabled || g_gate_ring == NULL || !g_gate_lock_valid)
    {
      return 0;    /* 门控没开：静默丢掉，不影响主链路 */
    }

  /* 只**试**锁：录音线程在这里等门控线程 = 丢麦克风数据，绝不允许。
   * 抢不到就当下这一小块不要了（下一帧 20ms 后还有）。 */

  if (pthread_mutex_trylock(&g_gate_lock) != 0)
    {
      return 0;
    }

  gate_ring_push(pcm, n);
  pthread_mutex_unlock(&g_gate_lock);
  return 0;
}

/* ---- 门控线程 ---- */

static void *sound_detect_gate_thread(void *arg)
{
  int16_t *win;

  (void)arg;

  win = (int16_t *)malloc(SOUND_GATE_WINDOW_SAMPLES * sizeof(int16_t));
  if (win == NULL)
    {
      printf("[门控] 分配门控窗口失败，门控线程退出\n");
      return NULL;
    }

  printf("[门控] 门控线程启动\n");

  while (!g_gate_stop)
    {
      bool have = false;
      uint32_t now;
      sound_gate_features_t f;
      int verdict;
      float sens;

      if (g_gate_lock_valid && pthread_mutex_trylock(&g_gate_lock) == 0)
        {
          if (g_gate_ring_count >= SOUND_GATE_WINDOW_SAMPLES)
            {
              gate_ring_peek(win, SOUND_GATE_WINDOW_SAMPLES);
              gate_ring_drop(SOUND_GATE_HOP_SAMPLES);
              have = true;
            }

          pthread_mutex_unlock(&g_gate_lock);
        }

      if (!have)
        {
          usleep(20000);    /* 20ms：攒够一个窗口再判 */
          continue;
        }

      /* 播放态：板子自己喇叭在响 → 整窗不判。
       * 半双工下录音本来就停了，这里是兜住"停之前灌进来的那几帧"。 */

      if (g_gate_busy_cb != NULL && g_gate_busy_cb(g_gate_busy_arg))
        {
          g_gate_stats.mute_windows++;
          continue;
        }

      verdict = sound_detect_voice_gate_ex(win, SOUND_GATE_WINDOW_SAMPLES, &f);
      if (verdict < 0)
        {
          continue;
        }

      now = sound_detect_get_tick_ms();
      g_gate_stats.windows++;
      g_gate_stats.last = f;

      if (verdict > 0)
        {
          /* 人声：交给云端语音链路（回调里只许置标志）*/

          g_gate_stats.voice_windows++;
          g_gate_stats.last_voice_ms = now;

          if (g_gate_voice_cb != NULL)
            {
              g_gate_voice_cb(&f, g_gate_voice_arg);
            }

          continue;
        }

      /* 非人声 + 能量超阈：喂本地小模型（任务三的 sound_event）。
       * 每 200ms 一个窗地喂（重叠窗），投票/不应期由那边做。 */

      sens = sound_detect_gate_sensitivity();
      if (f.active && f.rms >= SOUND_GATE_ANOMALY_RMS / sens)
        {
          g_gate_stats.anomaly_windows++;
          g_gate_stats.last_anomaly_ms = now;

          if (sound_event_feed(win, SOUND_GATE_WINDOW_SAMPLES) < 0 &&
              g_gate_stats.anomaly_windows == 1)
            {
              printf("[门控] 本地小模型还没落地（sound_event_feed 返回 -ENOSYS），"
                     "异常窗暂时只记账\n");
            }
        }
    }

  free(win);
  printf("[门控] 门控线程退出\n");
  return NULL;
}

/* ---- 启停 / 配置 ---- */

int sound_detect_bypass_start(void)
{
  struct sched_param param;
  pthread_attr_t attr;
  int ret;

  if (g_gate_thread_valid)
    {
      return OK;
    }

  if (g_gate_ring == NULL)
    {
      g_gate_ring = (int16_t *)malloc(SOUND_GATE_RING_SAMPLES *
                                      sizeof(int16_t));
      if (g_gate_ring == NULL)
        {
          printf("[门控] 分配环形缓冲失败\n");
          return -ENOMEM;
        }
    }

  if (!g_gate_lock_valid)
    {
      ret = pthread_mutex_init(&g_gate_lock, NULL);
      if (ret != 0)
        {
          free(g_gate_ring);
          g_gate_ring = NULL;
          return -ret;
        }

      g_gate_lock_valid = true;
    }

  pthread_mutex_lock(&g_gate_lock);
  g_gate_ring_head = 0;
  g_gate_ring_count = 0;
  memset(&g_gate_stats, 0, sizeof(g_gate_stats));
  pthread_mutex_unlock(&g_gate_lock);

  g_gate_stop = false;

  /* 低优先级线程：比 app 低一档，抢不到 CPU 无所谓，丢窗不丢音频 */

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, SOUND_GATE_THREAD_STACK);
  pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  memset(&param, 0, sizeof(param));
  param.sched_priority = SOUND_GATE_THREAD_PRIORITY;

  if (pthread_attr_setschedparam(&attr, &param) != 0)
    {
      /* 有些系统（主机侧跑单测时的 Linux SCHED_OTHER）不接受非 0 优先级：
       * 那就退回继承创建者的优先级，线程照样起来，别为了"低优先级"失败。 */
      pthread_attr_setinheritsched(&attr, PTHREAD_INHERIT_SCHED);
    }

  ret = pthread_create(&g_gate_thread, &attr, sound_detect_gate_thread, NULL);
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      printf("[门控] 创建门控线程失败: %d\n", ret);
      return -ret;
    }

  g_gate_thread_valid = true;
  g_gate_enabled = true;

  printf("[门控] 旁路门控已启动: 窗口 %.2fs / 步长 %.2fs / 环形 %d 样本 / "
         "优先级 %d\n",
         (double)SOUND_GATE_WINDOW_SAMPLES / 16000.0,
         (double)SOUND_GATE_HOP_SAMPLES / 16000.0,
         (int)SOUND_GATE_RING_SAMPLES,
         SOUND_GATE_THREAD_PRIORITY);

  return OK;
}

void sound_detect_bypass_stop(void)
{
  if (!g_gate_thread_valid)
    {
      return;
    }

  g_gate_stop = true;
  pthread_join(g_gate_thread, NULL);
  g_gate_thread_valid = false;
  g_gate_enabled = false;

  if (g_gate_ring != NULL)
    {
      free(g_gate_ring);
      g_gate_ring = NULL;
    }

  if (g_gate_lock_valid)
    {
      pthread_mutex_destroy(&g_gate_lock);
      g_gate_lock_valid = false;
    }

  g_gate_ring_head = 0;
  g_gate_ring_count = 0;

  printf("[门控] 旁路门控已停止（判过 %u 窗 / 人声 %u / 播放跳过 %u / "
         "异常 %u）\n",
         (unsigned)g_gate_stats.windows,
         (unsigned)g_gate_stats.voice_windows,
         (unsigned)g_gate_stats.mute_windows,
         (unsigned)g_gate_stats.anomaly_windows);
}

void sound_detect_gate_set_enabled(bool on)
{
  g_gate_enabled = on;
  printf("[门控] 已%s（%s）\n", on ? "打开" : "关闭",
         on ? "非人声异常交给本地模型" : "只出人声提示，不喂本地模型");
}

bool sound_detect_gate_enabled(void)
{
  return g_gate_enabled;
}

void sound_detect_gate_set_sensitivity(float sens)
{
  if (sens < SOUND_GATE_SENS_MIN)
    {
      sens = SOUND_GATE_SENS_MIN;
    }
  else if (sens > SOUND_GATE_SENS_MAX)
    {
      sens = SOUND_GATE_SENS_MAX;
    }

  g_gate_sensitivity = sens;
  printf("[门控] 灵敏度 = %.2f（越大越容易判成人声/异常）\n", (double)sens);
}

float sound_detect_gate_sensitivity(void)
{
  float sens = g_gate_sensitivity;

  if (sens < SOUND_GATE_SENS_MIN)
    {
      sens = SOUND_GATE_SENS_MIN;
    }
  else if (sens > SOUND_GATE_SENS_MAX)
    {
      sens = SOUND_GATE_SENS_MAX;
    }

  return sens;
}

void sound_detect_gate_set_busy_cb(sound_detect_busy_cb_t cb, void *arg)
{
  g_gate_busy_cb = cb;
  g_gate_busy_arg = arg;
}

void sound_detect_gate_set_voice_cb(sound_detect_voice_cb_t cb, void *arg)
{
  g_gate_voice_cb = cb;
  g_gate_voice_arg = arg;
}

const sound_gate_stats_t *sound_detect_gate_get_stats(void)
{
  return &g_gate_stats;
}

void sound_detect_gate_reset_stats(void)
{
  memset(&g_gate_stats, 0, sizeof(g_gate_stats));
}
