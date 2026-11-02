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

/* enter_critical_section()/leave_critical_section() —— 门控环形缓冲的互斥
 * 从 2026-10-30 起用关中断临界区实现，不用 pthread_mutex（理由见
 * sound_detect_pcm_tap 的文件内注释）。 */
#include <nuttx/config.h>
#include <nuttx/spinlock.h>

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

  /* 检查是否超过阈值
   *
   * 阈值**按类**取（2026-10-22）：原来这里一律比 ctx->config.threshold，
   * 每个类自己那条 classes[i].threshold 从头到尾没被查过 —— 是死配置。
   * 结果是"跌倒"这种一命中就报警 + 抢麦克风 + 打扰老人的紧急动作，和
   * "敲门"共用 0.7 的门槛：现场一声硬物响就以 0.73 的置信度报了跌倒
   * （2026-10-22 真机日志 `[门控] 本地小模型命中: 疑似跌倒撞击…置信度 0.73`），
   * 接着红屏 + 响铃 + 追问把正在进行的对话整条吃掉。
   *
   * 现在按类取：跌倒走 0.85 的高门槛（判错的代价是整机被打断 + 吓到老人），
   * 敲击/喊叫保持 0.7（求救信号宁可多问一句也不能漏）。 */
  float gate = (ctx->classes[max_idx].threshold > 0.0f)
               ? ctx->classes[max_idx].threshold
               : ctx->config.threshold;

  if (max_score >= gate && max_idx != SOUND_TYPE_NONE)
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

  /* 阈值同样按类取（和上面 sound_detect_run 里那处同一条规矩，见那边的说明） */
  if (max_score >= ((ctx->classes[max_idx].threshold > 0.0f)
                    ? ctx->classes[max_idx].threshold
                    : ctx->config.threshold) &&
      max_idx != SOUND_TYPE_NONE)
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
  ctx->classes[SOUND_TYPE_FALL].threshold = SOUND_DETECT_THRESHOLD_HIGH;
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
#define SOUND_GATE_ANOMALY_PEAK      0.150f  /* 喂本地模型的另一条路：峰值（-16dBFS）。
                                              * ★ 2026-10-31 加。摔倒/砸物是**短促冲击**，
                                              *   0.1~0.2 秒的峰值摊进 0.64 秒窗算 RMS
                                              *   会被稀释到 0.4~0.5 倍，本来刚过线的
                                              *   一声闷响就掉到线下、整个窗没喂进模型，
                                              *   3/4 投票永远凑不齐 —— 这是"摔倒没反应"
                                              *   最像的一条来路。峰值不稀释，拿它补一条
                                              *   路：**只多喂，不少喂**，喂不喂得准由
                                              *   本地小模型的 0.60/0.70 阈值 + 3/4 投票
                                              *   + 8 秒不应期把关。 */
#define SOUND_GATE_VOICED_ZCR_MAX    0.35f   /* 浊音帧过零率上限 */
#define SOUND_GATE_VOICED_AC_MIN     0.30f   /* 单帧"浊音样"的自相关峰下限 */
#define SOUND_GATE_VOICED_AC_STRONG  0.60f   /* 整窗强周期性判据 */
#define SOUND_GATE_VOICED_RATIO_MIN  0.25f   /* 浊音帧占比下限 */
#define SOUND_GATE_FRAME_SAMPLES     512     /* 32ms 分析帧（自相关用它）*/
#define SOUND_GATE_LAG_MIN           40      /* 16k/400Hz：人声基频上界 */
#define SOUND_GATE_LAG_MAX           200     /* 16k/80Hz：人声基频下界 */
#define SOUND_GATE_LAG_STEP          2
#define SOUND_GATE_FEED_LINGER       4      /* 喂模型的"松手拖尾"：能量门关掉之后
                                            * 再喂 4 跳（800ms）。一次撞击往往只有
                                            * 0.2~0.4 秒，只喂那一两跳的话小模型
                                            * 连第一个 0.64 秒窗都攒不满，一次推理
                                            * 都不会有，3/4 投票更是永远凑不齐。 */
#define SOUND_GATE_SENS_MIN          0.3f
#define SOUND_GATE_SENS_MAX          3.0f

/* 旁路环形缓冲与门控线程状态（单麦克风 → 单实例）*/
static int16_t        *g_gate_ring;
static size_t          g_gate_ring_head;
static size_t          g_gate_ring_count;
static bool            g_gate_ready;
static pthread_t       g_gate_thread;
static bool            g_gate_thread_valid;
static volatile bool   g_gate_stop;
static volatile bool   g_gate_enabled;

/* 在途 tap 计数：sound_detect_pcm_tap（录音线程）从"检查通过"到"解锁返回"
 * 之间的那一小段。sound_detect_bypass_stop 先关 g_gate_enabled、再把这个计数
 * 等一小段，让收尾干净些。
 *
 * ⚠ 但**安全性现在不靠这个计数**（2026-10-25 起）：stop 已经不再销毁锁、
 * 也不再释放环形缓冲了，所以 tap 就算比 stop 慢一拍也掀不起风浪。历史上
 * 用这个计数去守"销毁锁前等在途归零"，结果 2026-10-20 和 2026-10-25 各崩
 * 一次（sem_post.c:63 持有者断言 → hello_app 崩 → 整条音频链卡死），根因是
 * 等在途只有 200 ms 上限 + set_enabled(true) 能中途把闸重新打开。现在改成
 * "永远不销毁"，这类窗口从根上不存在了，本计数退化为诊断用。
 * 单核 Cortex-M 上普通 int 自增自减即可，不需要原子操作。 */
static volatile int    g_gate_taps_inflight;
static volatile float  g_gate_sensitivity = 1.0f;
static sound_detect_busy_cb_t  g_gate_busy_cb;
static void                   *g_gate_busy_arg;
static sound_detect_voice_cb_t g_gate_voice_cb;
static void                   *g_gate_voice_arg;
static sound_gate_stats_t      g_gate_stats;

/* ★ 2026-11-01：人声"余温"截止时刻。
 *
 *   真机误报的 fall/knock 总是**紧跟在一串人声窗之后**：一声喊叫/说话的
 *   尾几跳 rms 还在门槛上，但"浊音"判据差一点没过，门控按非人声把它喂进
 *   模型，模型就判成 knock/fall（实测 fall 0.57、knock 0.71）。只在
 *   verdict>0 的那一跳做 scream-only 拦不住这些尾巴。
 *
 *   所以门控判成人声后 2.5 秒内的所有喂入都按人声处理（只允许 scream 投票）。
 *   真正的撞击/倒地声是宽带的、ac 低，不会紧贴着一段人声还过能量门槛 ——
 *   用 2.5 秒换掉"说句话就报警"是值得的。 */
static uint32_t                g_gate_voice_until_ms;

/* ★ 2026-11-01：连续"尖锐声窗"计数。
 *
 *   "声音还在持续（未回落）就不报 fall/knock"那一条原本是**逐窗**判的：
 *   敲桌子时当前 0.64 秒窗本来就还在响，于是每一发 hit 都被当成"持续声"
 *   挡掉 —— 真机 v20 测试 `★命中` 计数为 0，用户连敲完全没反应。
 *
 *   改成**连续多久**才算持续：> 6 秒（30 个 0.2 秒跳）才抑制。短敲
 *   （几窗）不受影响，风扇/摩擦这种连续几十秒的噪声才被挡。
 *   真正的单发误报由上层"两次确认"兜（单次 0.55~0.80 不报）。 */
static uint32_t                g_gate_loud_run;
#define SOUND_GATE_SUSTAINED_WINDOWS 30

/* ★ 门控逐窗可观测（2026-10-31）。
 *
 * 排查"摔倒异常声音检测一点反应都没有"时踩到的硬伤：门控线程**除了
 * verdict>0 之外一个字都不打**，统计又只在 bypass_stop 那一刻打印一次。
 * 于是一次测试跑完，串口里既看不到"喂了几个窗给模型"，也看不到"是不是
 * 全被判成人声送去云端了"，五个候选原因一个都排除不掉。
 *
 * 这里加的是**限速**日志（同一 250ms 桶只出一行，攒着的算在下一行里报），
 * 别把串口灌满；真正喂进模型的窗不受限速，必须每一扇都看得见。 */

static uint32_t g_gate_dbg_last_ms;
static uint32_t g_gate_dbg_suppressed;
static uint32_t g_gate_dbg_summary_ms;

/* ★ 喂模型必须是**连续、不重复**的音频流（2026-10-31 修）。
 *
 * sound_event_feed 是流式前端（sound_event.h:42-45 写着"允许任意长度、
 * 任意切分地连续调用"），它自己攒 0.64 秒窗、每 200ms 出一次推理。
 * 门控这边的 win 是**滑动窗**：每步只前进 HOP=3200 样本、重叠 7040 样本。
 * 原来把整窗 10240 一股脑喂进去 = 同一段音频被喂 3.2 遍，而且是
 * "前进 3200 / 倒退 7040"的锯齿 —— 模型听到的是世界上根本不存在的声音，
 * mel 历史整个是花的，3/4 投票永远凑不齐。
 *
 *   （旁证：模型每 20 帧出一次推理，20 帧 = 20*160 = 3200 样本 = 正好一跳。
 *    也就是说"一跳喂一推理"才是设计好的节奏，整窗喂是 3 次推理还都带重复。）
 *
 * 所以改成：只喂**新**的那一跳（win + skip，长度 W - skip）；能量门刚打开
 * 时（primed=false）补喂整窗好让首窗攒得够；断流之后重新补。 */

static bool     g_gate_primed;      /* 小模型手里已经有连续音频了吗 */
static size_t   g_gate_feed_skip;   /* win 开头有多少样本上一跳已经喂过 */
static int      g_gate_feed_linger; /* 松手拖尾还剩几跳 */

#define SOUND_GATE_DBG_INTERVAL_MS   250   /* 逐窗日志的限速桶 */
#define SOUND_GATE_DBG_SUMMARY_MS   15000  /* 周期性汇总 */

/* 逐窗一行。who 说清这一窗走了哪条路。 */
static void gate_dbg_window(uint32_t now, const char *who,
                            const sound_gate_features_t *f, float sens)
{
  if ((now - g_gate_dbg_last_ms) >= SOUND_GATE_DBG_INTERVAL_MS)
    {
      printf("[门控] %s rms=%.4f peak=%.4f zcr=%.2f ac=%.2f 浊音=%.2f"
             "（喂模型门槛 rms>=%.4f 或 peak>=%.4f，静音门 %.4f）"
             "%s\n",
             who, f->rms, f->peak, f->zcr, f->ac_peak, f->voiced_ratio,
             SOUND_GATE_ANOMALY_RMS / sens,
             SOUND_GATE_ANOMALY_PEAK / sens,
             SOUND_GATE_SILENCE_RMS / sens,
             g_gate_dbg_suppressed > 0 ? "（前面攒了几窗没打，省串口）" : "");
      g_gate_dbg_suppressed = 0;
      g_gate_dbg_last_ms    = now;
    }
  else
    {
      g_gate_dbg_suppressed++;
    }
}

/* 每 15 秒一行汇总。**哪怕一扇窗都没有也打** —— "0 窗"和"有窗但没喂"
 * 是两个完全不同的病因（录音链死了 vs 判据把窗挡了），必须分得开。 */
static void gate_dbg_summary(uint32_t now)
{
  if ((now - g_gate_dbg_summary_ms) < SOUND_GATE_DBG_SUMMARY_MS)
    {
      return;
    }

  g_gate_dbg_summary_ms = now;

  printf("[门控] 汇总：判过 %lu 窗 = 人声 %lu + 非人声喂模型 %lu"
         "（其中靠峰值 %lu）；播放态跳过 %lu；"
         "⚠判成人声但峰值很高 %lu；有声没喂 %lu；"
         "最近一窗 rms=%.4f peak=%.4f 浊音=%.2f\n",
         (unsigned long)g_gate_stats.windows,
         (unsigned long)g_gate_stats.voice_windows,
         (unsigned long)g_gate_stats.anomaly_windows,
         (unsigned long)g_gate_stats.peak_feed_windows,
         (unsigned long)g_gate_stats.mute_windows,
         (unsigned long)g_gate_stats.voice_peak_windows,
         (unsigned long)g_gate_stats.nearmiss_windows,
         g_gate_stats.last.rms, g_gate_stats.last.peak,
         g_gate_stats.last.voiced_ratio);
}

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
int robot_ui_alarm_ring(const char *reason)
{
  /* robot_ui 没进镜像（或者那个模块还没落地）：板级警音归它那条路，这里起不了铃。
   * 这一行是给 host / 别的小镜像兜底的，真机上永远走不到 —— 真机里 robot_ui 一定
   * 在镜像里，强符号覆盖这里。 */
  printf("[门控] robot_ui_alarm_ring() 还没落地，这一记铃起不了: %s\n",
         reason != NULL ? reason : "(无)");
  return -ENOSYS;
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
  int32_t peak = 0;
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

      if (s < 0)
        {
          s = -s;      /* s 从 int16_t 提上来的，-32768 也取得到 */
        }

      if (s > peak)
        {
          peak = s;
        }

      if (i > 0 && ((pcm[i - 1] < 0 && s >= 0) ||
                    (pcm[i - 1] >= 0 && s < 0)))
        {
          crossings++;
        }
    }

  mean_sq = (float)((double)acc / (double)n);
  f.energy = mean_sq;
  f.rms = sqrtf(mean_sq) / 32768.0f;
  f.peak = (float)peak / 32768.0f;
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

  if (!g_gate_enabled || g_gate_ring == NULL || !g_gate_ready)
    {
      return 0;    /* 门控没开：静默丢掉，不影响主链路 */
    }

  /* 先登记在途，再**重新**检查一遍三件套。顺序不能反：stop 的协议是
   * "关 enabled → 等在途归零 → 销毁锁/释放环"，重查这一步保证
   * "检查通过"和"在途计数可见"对 stop 而言是原子的 —— stop 要么在
   * 增量之前看到计数为 0（此后本函数重查 enabled 一定失败、直接退出），
   * 要么在增量之后看到计数为 1（等本函数走完才继续）。 */

  g_gate_taps_inflight++;

  if (!g_gate_enabled || g_gate_ring == NULL || !g_gate_ready)
    {
      g_gate_taps_inflight--;
      return 0;
    }

  /* ★ 2026-10-30：这里（以及门控线程、start 里）的互斥换成**关中断临界区**。
   *
   * 2026-10-25 那次"不再销毁锁"的修复**没能止住崩溃** —— 10-30 真机又崩在
   * 同一个位置（见 bypass_stop 注释里的 backtrace）。而且源码里三处加/解锁
   * 全是**线性的**（拿一次放一次，没有第二条路径去放），所以"重复解锁"从源
   * 码上讲不出来。那就只剩两种可能：
   *
   *   (a) pthread_mutex 的**持有者跟踪**本身出错（NuttX 的 sem_post.c:63
   *       断言正是拿持有者做判据的，held 标记丢了/被别的线程抢改了都会打）；
   *   (b) g_gate_lock 这 28 字节被别处写坏了（这个断言对锁内容极其敏感）。
   *
   * 两种都**没法从源码证明**是哪一种，但两种都被下面这个改法一次性消掉：
   * 关中断临界区没有"持有者"可言，`pthread_mutex_give` → `sem_post` 这条路
   * 就不存在了，断言无从触发。同时 (b) 也落空 —— 没有锁对象可被写坏。
   *
   * 顺带把"抢不到锁就丢掉这一帧"一起去掉了：单核临界区不可能抢不到，
   * 送进环的数据只会更多不会更少（摔倒检测那边只会更灵敏）。
   *
   * 代价是临界区里有一次 memcpy：tap 这边一个录音块（1 KB 上下），
   * 门控线程那边一个窗（20 KB）。SF32LB52 上最多几十 µs，而音频 DMA 一档
   * 20 ms、RNDIS 也没这么紧 —— 余量两个数量级，不会顶到实时性。
   *
   * ⚠ 摔倒/异常声音检测的**算法、判定、上报一行没改**，只换了锁的实现。 */

  {
    irqstate_t flags = enter_critical_section();
    gate_ring_push(pcm, n);
    leave_critical_section(flags);
  }

  g_gate_taps_inflight--;
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

      /* 临界区只包住"看一眼够不够一窗 + 取一窗 + 丢一跳"这几步索引运算和
       * memcpy（理由见 sound_detect_pcm_tap 里的长注释：2026-10-30 起不用
       * pthread_mutex，改关中断临界区，让 sem_post.c:63 那类断言无从触发）。 */

      if (g_gate_ready)
        {
          irqstate_t flags = enter_critical_section();

          if (g_gate_ring_count >= SOUND_GATE_WINDOW_SAMPLES)
            {
              gate_ring_peek(win, SOUND_GATE_WINDOW_SAMPLES);
              gate_ring_drop(SOUND_GATE_HOP_SAMPLES);
              have = true;
            }

          leave_critical_section(flags);
        }

      if (!have)
        {
          /* 环里不够一整窗 = 断流（录音停了 / 起步没攒够）。
           * 小模型那边的连续性到此为止，下次有窗要重新补满一整窗；
           * 同时也不能让它在断流时一声不吭 —— "一扇窗都没有"和"有窗但没喂"
           * 是两个完全不同的病因。 */

          g_gate_primed    = false;
          g_gate_feed_skip = 0;
          g_gate_feed_linger = 0;

          gate_dbg_summary(sound_detect_get_tick_ms());

          usleep(20000);    /* 20ms：攒够一个窗口再判 */
          continue;
        }

      /* 播放态：板子自己喇叭在响 → 整窗不判。
       * 半双工下录音本来就停了，这里是兜住"停之前灌进来的那几帧"。 */

      if (g_gate_busy_cb != NULL && g_gate_busy_cb(g_gate_busy_arg))
        {
          g_gate_stats.mute_windows++;
          g_gate_primed    = false;   /* 自己在放音：模型那边断流，回来重新补 */
          g_gate_feed_skip = 0;
          g_gate_feed_linger = 0;
          gate_dbg_summary(sound_detect_get_tick_ms());
          continue;
        }

      verdict = sound_detect_voice_gate_ex(win, SOUND_GATE_WINDOW_SAMPLES, &f);
      if (verdict < 0)
        {
          continue;
        }

      now = sound_detect_get_tick_ms();
      if (verdict > 0)
        {
          g_gate_voice_until_ms = now + 2500;
        }

      g_gate_stats.windows++;
      g_gate_stats.last = f;
      sens = sound_detect_gate_sensitivity();

      /* 更新"连续尖锐窗"计数：这一窗还过能量门槛就 ++，否则归零。
       * 只在 > SOUND_GATE_SUSTAINED_WINDOWS 时才把 loud_hint 传给模型。 */

      {
        bool loud_win = f.active &&
            (f.rms >= SOUND_GATE_ANOMALY_RMS / sens ||
             f.peak >= SOUND_GATE_ANOMALY_PEAK / sens);

        if (loud_win)
          {
            if (g_gate_loud_run < 1000000u)
              {
                g_gate_loud_run++;
              }
          }
        else
          {
            g_gate_loud_run = 0;
          }
      }

      /* ★★ 2026-10-31：喂不喂本地小模型只看**能量**，与"是不是人声"正交。
       *
       *   原来人声窗直接 `continue`、一个样本都不喂小模型，于是尖叫（SCREAM 类）
       *   **结构上永远不可能**触发 —— 尖叫是浊音、必然被判成人声走云端，而人声
       *   分支压根不喂模型。真机 `top1=scream` 全程 0、用户大声叫喊被判成 knock、
       *   「大声叫喊板子没报警」就是这么来的。一声脆响（撞击）被"浊音"判据吃掉
       *   也一样，只多记一个计数就过去了。
       *
       *   本地小模型的职责是"这是什么声"（OTHER/FALL/KNOCK/SCREAM）；
       *   门控的职责是"要不要顺手送云端做语音"。这两件事原来被做成互斥的，
       *   现在分开：能量够就喂模型，判成人声的**另外**再送云端。
       *   CPU 代价：真机 315 秒 / 1061 窗里多喂约 137 窗 ≈ 0.4 次/秒，可忽略。
       *
       * 两条能量路（取**或**，只多喂不少喂，判得准不准由小模型那边的
       * 0.60/0.70 阈值 + 3/4 投票 + 8 秒不应期把关）：
       *   ① 整窗 RMS 过门槛（持续性噪声/长响）；
       *   ② 峰值过门槛（**短促撞击**：0.64 秒窗会把 0.15 秒的响摊薄到
       *      0.48 倍 RMS，本来刚过线的一声闷响就掉到线下，整窗白扔）。 */

      if (verdict > 0)
        {
          /* 人声：顺手送云端语音链路（回调里只许置标志）。
           * ★ 不再 continue —— 下面喂小模型那一步照样要做。 */

          g_gate_stats.voice_windows++;
          g_gate_stats.last_voice_ms = now;

          /* ★ 人声窗里峰值还很高 = 一声脆响/尖叫被"浊音"判据吃掉了。
           *   这个计数一涨就说明"撞击/喊叫被当成说话送去云端"，是门控判据
           *   的问题，不是小模型的问题。 */

          if (f.peak >= SOUND_GATE_ANOMALY_PEAK / sens)
            {
              g_gate_stats.voice_peak_windows++;
              gate_dbg_window(now, "判成人声且峰值高 -> 送云端 + 也喂小模型"
                                   "（尖叫/撞击最可能落在这里）",
                              &f, sens);
            }
          else
            {
              gate_dbg_window(now, "判成人声 -> 云端语音链路", &f, sens);
            }

          if (g_gate_voice_cb != NULL)
            {
              g_gate_voice_cb(&f, g_gate_voice_arg);
            }
        }

      {
        bool feed_now = false;

        if (f.active)
          {
            if (f.rms >= SOUND_GATE_ANOMALY_RMS / sens)
              {
                feed_now = true;
              }
            else if (f.peak >= SOUND_GATE_ANOMALY_PEAK / sens)
              {
                g_gate_stats.peak_feed_windows++;
                feed_now = true;
              }
            else
              {
                g_gate_stats.nearmiss_windows++;
              }
          }

        if (feed_now)
          {
            g_gate_stats.anomaly_windows++;
            g_gate_stats.last_anomaly_ms = now;
            g_gate_feed_linger = SOUND_GATE_FEED_LINGER;
          }
        else if (g_gate_feed_linger > 0)
          {
            /* 松手再拖几跳，好让小模型凑得满一个窗（见 SOUND_GATE_FEED_LINGER）。 */

            g_gate_feed_linger--;
            feed_now = true;
          }

        if (feed_now)
          {
            const int16_t *feed_p;
            size_t         feed_n;
            int            wins;

            if (!g_gate_primed)
              {
                /* 断流之后头一喂：整窗补上，首窗（10480 样本）才攒得够。 */

                feed_p = win;
                feed_n = SOUND_GATE_WINDOW_SAMPLES;
              }
            else
              {
                /* 只喂上一跳没喂过的那一截 —— 见上面那段长注释。 */

                feed_p = win + g_gate_feed_skip;
                feed_n = SOUND_GATE_WINDOW_SAMPLES - g_gate_feed_skip;
              }

            /* 人声或"人声余温"（见 g_gate_voice_until_ms）：只让 SCREAM
             * 类投票（见 sound_event_feed_ex 的注释）。fall/knock 只从
             * 真正非人声的窗触发，这样"说话/喊叫"（包括它的尾音）不会再
             * 被当成敲击/跌倒去报警。 */
            {
              int voice_ctx = (verdict > 0) ||
                  ((int32_t)(now - g_gate_voice_until_ms) < 0);

              /* 连续 >6 秒都过能量门槛才算"持续声"，才抑制 fall/knock。
               * 短敲（几窗）不再被误伤 —— v20 真机就是逐窗判导致真敲
               * 一发 `★命中` 都没有（全被"声音还在持续"挡住）。 */
              int loud_now = (g_gate_loud_run > SOUND_GATE_SUSTAINED_WINDOWS);

              wins = sound_event_feed_ex(feed_p, feed_n, voice_ctx, loud_now);
            }

            g_gate_primed    = true;
            g_gate_feed_skip = SOUND_GATE_WINDOW_SAMPLES -
                               SOUND_GATE_HOP_SAMPLES;

            if (wins < 0 && g_gate_stats.anomaly_windows == 1)
              {
                printf("[门控] 本地小模型还没落地（sound_event_feed 返回 -ENOSYS），"
                       "异常窗暂时只记账\n");
              }
            else if (wins > 0)
              {
                printf("[门控] 喂模型 %lu 样本 -> %d 次推理\n",
                       (unsigned long)feed_n, wins);
              }

            gate_dbg_window(now, (feed_n == SOUND_GATE_WINDOW_SAMPLES)
                                 ? "喂本地小模型（补喂整窗）"
                                 : "喂本地小模型（只喂新的一跳）",
                            &f, sens);
          }
        else
          {
            /* 这一跳不喂：小模型那边就断流了，下次得重新补满一整窗。 */

            g_gate_primed    = false;
            g_gate_feed_skip = 0;

            gate_dbg_window(now, (verdict > 0)
                                 ? "人声但没喂模型（能量/峰值都不够）"
                                 : "非人声但没喂（能量/峰值都不够）", &f, sens);
          }
      }

      gate_dbg_summary(now);
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

  /* 环形缓冲/统计区一旦用上就永远算就绪 —— 它们永不销毁（理由见
   * bypass_stop）。2026-10-30 起互斥是关中断临界区，不再有"锁初始化"这回事，
   * 也就没有 init 失败这条路（原来那条路会 free 环、把 g_gate_lock_valid
   * 留在 false，是另一种"半初始化"状态，现在不存在了）。 */

  g_gate_ready = true;

  {
    irqstate_t flags = enter_critical_section();
    g_gate_ring_head = 0;
    g_gate_ring_count = 0;
    memset(&g_gate_stats, 0, sizeof(g_gate_stats));
    leave_critical_section(flags);
  }

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

  /* ★ "关中断临界区"这几个字是**新旧固件的分水岭**：串口开机看到它 =
   *   2026-10-30 那版（sem_post 崩溃已修）落地了；看不到就是还跑着旧的。
   *   前面连续两次烧录失败都是靠行为反推出来的，以后直接看这一行。 */

  printf("[门控] 旁路门控已启动（关中断临界区）: 窗口 %.2fs / 步长 %.2fs / "
         "环形 %d 样本 / 优先级 %d\n",
         (double)SOUND_GATE_WINDOW_SAMPLES / 16000.0,
         (double)SOUND_GATE_HOP_SAMPLES / 16000.0,
         (int)SOUND_GATE_RING_SAMPLES,
         SOUND_GATE_THREAD_PRIORITY);

  return OK;
}

void sound_detect_bypass_stop(void)
{
  int i;

  if (!g_gate_thread_valid)
    {
      return;
    }

  /* 顺序：先关闸（新 tap 从此在第一道检查就被拦下），再等在途的 tap
   * 归零，然后 join 门控线程。
   *
   * ★★★ 这里的崩溃史（三步，别再倒回去）：
   *
   *   1. 早先：stop 会 pthread_mutex_destroy + free(环)，tap 在"检查通过
   *      之后、unlock 之前"撞上销毁，unlock 打在已销毁的互斥量上 ——
   *      sem_post.c:63 的持有者断言，hello_app 当场崩（2026-10-20）。
   *
   *   2. 2026-10-25 修复：不销毁锁、不释放环，只留 g_gate_enabled=false
   *      + 等在途。**没止住** —— 2026-10-30 真机又崩在同一个 backtrace：
   *        Assertion failed at /sched/semaphore/sem_post.c:63 task: hello_app
   *        backtrace: nxsem_post_slow ← pthread_mutex_give
   *                   ← sound_detect_pcm_tap ← audio_record_thread
   *      而源码里三处加/解锁全是线性的、不存在重复解锁，所以第 1 条那类
   *      "use-after-destroy"解释不了这次 —— 只能是 pthread 的持有者跟踪出错，
   *      或者锁内存被别处写坏（两种都无法从源码证明是哪一种）。
   *
   *   3. 2026-10-30 现在：**这个模块不再持有任何 pthread_mutex**。
   *      g_gate_ring 的互斥改成 enter_critical_section()（关中断 + 关抢占，
   *      详细理由见 sound_detect_pcm_tap）。没有锁对象就没有持有者，
   *      `pthread_mutex_give → sem_post` 这条路根本不存在，断言无从触发；
   *      同时"锁内存被写坏"也落空 —— 没有锁可写。
   *      ⇒ 第 1、2 条那两类崩溃同时消失，而且**与在途计数的时序无关**。
   *
   * 环形缓冲仍然**固定大小一次性分配、永不释放**（复用），这条不变。
   * g_gate_enabled=false + 等在途也保留（收尾更干净、日志更好读），
   * 但它早已不是安全性的承重墙。 */

  g_gate_enabled = false;
  for (i = 0; i < 200 && g_gate_taps_inflight > 0; i++)
    {
      usleep(1000);
    }

  if (g_gate_taps_inflight > 0)
    {
      /* 等超时也不再是危险信号（下面不销毁任何东西），但值得记一笔：
       * 说明有 tap 线程被拖住了，是别的问题的线索。 */
      printf("[门控] ★ 停止时仍有 %d 个 tap 在途（已不再销毁锁，无害；"
             "但说明录音线程被拖住过）\n",
             g_gate_taps_inflight);
    }

  g_gate_stop = true;
  pthread_join(g_gate_thread, NULL);
  g_gate_thread_valid = false;

  /* ⚠ 不 free(g_gate_ring)、不把 g_gate_ready 置回 false —— 理由见上。
   * （这里已经没有 pthread_mutex 可销毁了。）下次 start 会复用它们。 */

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
