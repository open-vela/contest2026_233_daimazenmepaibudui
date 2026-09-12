/****************************************************************************
 * AI Audio Module Header
 * 智爱陪伴 - AI老人陪伴守护终端
 * 音频模块 - 麦克风录音与音频播放
 ****************************************************************************/

#ifndef __AI_AUDIO_H
#define __AI_AUDIO_H

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

/* 音频调试日志宏 */
#ifdef CONFIG_DEBUG_AI_AUDIO
#  define AUDIO_DEBUG(...) do { printf("[AUDIO] "); printf(__VA_ARGS__); \
                                printf("\n"); } while (0)
#else
#  define AUDIO_DEBUG(...) do { } while (0)
#endif

/* 默认音频参数 */
#define AUDIO_DEFAULT_SAMPLE_RATE    16000   /* 16kHz采样率 */
#define AUDIO_DEFAULT_CHANNELS       1       /* 单声道 */
#define AUDIO_DEFAULT_BITS_PER_SAMPLE 16     /* 16位采样 */
#define AUDIO_DEFAULT_FRAME_MS       20      /* 20ms每帧 */

/* 音频缓冲区大小 */
/* 注意: 16000Hz * 20ms = 320 帧, 缓冲必须能容纳一个帧周期的数据 */
#define AUDIO_RECORD_BUF_FRAMES      640     /* 录音缓冲帧数 (640=40ms@16kHz, 留余量) */
#define AUDIO_PLAY_BUF_FRAMES        80      /* 播放缓冲帧数 (80*20ms=1.6秒) */
#define AUDIO_PLAY_BUFFER_MS         5000    /* 最大缓存5秒 */

/* VAD (Voice Activity Detection) 参数 */
#define AUDIO_VAD_ENERGY_THRESHOLD   500     /* 能量阈值 */
#define AUDIO_VAD_SILENCE_TIMEOUT_MS 3000    /* 静音超时3秒 */
#define AUDIO_VAD_MIN_SPEECH_MS      300     /* 最小语音长度300ms */

/* 音量范围 */
#define AUDIO_VOLUME_MIN             0
#define AUDIO_VOLUME_MAX             100
#define AUDIO_VOLUME_DEFAULT         70

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* 音频模块状态 */
typedef enum
{
  AUDIO_STATE_UNINIT = 0,     /* 未初始化 */
  AUDIO_STATE_IDLE,           /* 空闲状态 */
  AUDIO_STATE_RECORDING,      /* 正在录音 */
  AUDIO_STATE_PLAYING,        /* 正在播放 */
  AUDIO_STATE_BOTH            /* 同时录音和播放（全双工） */
} audio_state_t;

/* 音频采样率 */
typedef enum
{
  AUDIO_RATE_8K = 8000,       /* 8kHz */
  AUDIO_RATE_16K = 16000,     /* 16kHz (推荐) */
  AUDIO_RATE_44K = 44100,     /* 44.1kHz */
  AUDIO_RATE_48K = 48000      /* 48kHz */
} audio_sample_rate_t;

/* 音频通道数 */
typedef enum
{
  AUDIO_CH_MONO = 1,          /* 单声道 */
  AUDIO_CH_STEREO = 2         /* 双声道 */
} audio_channels_t;

/* 音频数据格式 */
typedef enum
{
  AUDIO_FORMAT_S16_LE = 0,    /* 有符号16位小端 (推荐) */
  AUDIO_FORMAT_S16_BE,        /* 有符号16位大端 */
  AUDIO_FORMAT_U8,            /* 无符号8位 */
  AUDIO_FORMAT_FLOAT32        /* 32位浮点 */
} audio_format_t;

/* 录音数据回调 */
typedef void (*audio_record_cb_t)(const int16_t *data, size_t frames,
                                  void *user_data);

/* 播放完成回调 */
typedef void (*audio_play_complete_cb_t)(void *user_data);

/* VAD (Voice Activity Detection) 回调 */
typedef void (*audio_vad_cb_t)(bool speech_detected, void *user_data);

/* 音频配置结构 */
typedef struct
{
  audio_sample_rate_t sample_rate;    /* 采样率 */
  audio_channels_t    channels;       /* 通道数 */
  audio_format_t      format;         /* 数据格式 */
  uint8_t             frame_ms;       /* 每帧毫秒数 */
  uint8_t             volume;         /* 音量 0-100 */
} audio_config_t;

/* 录音配置 */
typedef struct
{
  bool                enable_vad;     /* 启用VAD检测 */
  uint32_t            silence_timeout_ms; /* 静音超时 */
  uint32_t            min_speech_ms;  /* 最小语音长度 */
  audio_record_cb_t   data_callback;  /* 数据回调 */
  void               *user_data;      /* 用户数据 */
} audio_record_config_t;

/* 音频模块上下文 */
typedef struct
{
  audio_state_t       state;          /* 当前状态 */
  audio_config_t      config;         /* 音频配置 */
  bool                initialized;    /* 是否已初始化 */

  /* 录音相关 */
  bool                recording;      /* 是否正在录音 */
  audio_record_config_t record_cfg;   /* 录音配置 */
  int                 record_fd;      /* 录音设备文件描述符 */
  int16_t            *record_buf;     /* 录音缓冲区 */
  size_t              record_buf_size; /* 缓冲区大小(字节) */
  volatile bool       record_stop;    /* 停止录音标志 */
  bool                record_thread_valid; /* 录音线程需要回收 */

  /* 播放相关 */
  bool                playing;        /* 是否正在播放 */
  audio_play_complete_cb_t play_cb;   /* 播放完成回调 */
  void               *play_user_data; /* 播放回调用户数据 */
  int                 play_fd;        /* 播放设备文件描述符 */
  int16_t            *play_buf;       /* 播放缓冲区 */
  size_t              play_buf_size;  /* 缓冲区大小(字节) */
  volatile bool       play_stop;      /* 停止播放标志 */
  size_t              play_frames;    /* 当前播放帧数 */
  bool                play_thread_valid; /* 播放线程需要回收 */

  /* VAD相关 */
  bool                vad_enabled;    /* VAD是否启用 */
  audio_vad_cb_t      vad_callback;   /* VAD回调 */
  void               *vad_user_data;  /* VAD用户数据 */
  uint32_t            vad_energy_threshold; /* 能量阈值 */
  uint32_t            vad_silence_frames;   /* 静音帧计数 */
  uint32_t            vad_speech_frames;    /* 语音帧计数 */
  bool                vad_speech_active;    /* 是否检测到语音 */

  /* 线程相关 */
  pthread_t           record_thread;  /* 录音线程 */
  pthread_t           play_thread;    /* 播放线程 */
} audio_context_t;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/**
 * @brief  初始化音频模块
 * @param  ctx: 音频上下文指针
 * @param  config: 音频配置, NULL使用默认配置
 * @return 0成功, 负值失败
 */

int audio_init(audio_context_t *ctx, const audio_config_t *config);

/**
 * @brief  反初始化音频模块
 * @param  ctx: 音频上下文指针
 */

void audio_deinit(audio_context_t *ctx);

/**
 * @brief  开始录音
 * @param  ctx: 音频上下文指针
 * @param  config: 录音配置
 * @return 0成功, 负值失败
 */

int audio_record_start(audio_context_t *ctx,
                       const audio_record_config_t *config);

/**
 * @brief  停止录音
 * @param  ctx: 音频上下文指针
 */

void audio_record_stop(audio_context_t *ctx);

/**
 * @brief  是否正在录音
 * @param  ctx: 音频上下文指针
 * @return true正在录音, false未录音
 */

bool audio_is_recording(audio_context_t *ctx);

/**
 * @brief  开始播放音频数据
 * @param  ctx: 音频上下文指针
 * @param  data: 音频数据
 * @param  frames: 帧数
 * @param  callback: 播放完成回调
 * @param  user_data: 回调用户数据
 * @return 0成功, 负值失败
 */

int audio_play_start(audio_context_t *ctx,
                     const int16_t *data, size_t frames,
                     audio_play_complete_cb_t callback,
                     void *user_data);

/**
 * @brief  从文件播放音频
 * @param  ctx: 音频上下文指针
 * @param  filepath: 音频文件路径
 * @param  callback: 播放完成回调
 * @param  user_data: 回调用户数据
 * @return 0成功, 负值失败
 */

int audio_play_file(audio_context_t *ctx,
                    const char *filepath,
                    audio_play_complete_cb_t callback,
                    void *user_data);

/**
 * @brief  停止播放
 * @param  ctx: 音频上下文指针
 */

void audio_play_stop(audio_context_t *ctx);

/**
 * @brief  是否正在播放
 * @param  ctx: 音频上下文指针
 * @return true正在播放, false未播放
 */

bool audio_is_playing(audio_context_t *ctx);

/**
 * @brief  设置音量
 * @param  ctx: 音频上下文指针
 * @param  volume: 音量 0-100
 * @return 0成功, 负值失败
 */

int audio_set_volume(audio_context_t *ctx, uint8_t volume);

/**
 * @brief  获取音量
 * @param  ctx: 音频上下文指针
 * @return 当前音量 0-100
 */

uint8_t audio_get_volume(audio_context_t *ctx);

/**
 * @brief  启用VAD检测
 * @param  ctx: 音频上下文指针
 * @param  callback: VAD回调
 * @param  user_data: 用户数据
 */

void audio_vad_enable(audio_context_t *ctx,
                      audio_vad_cb_t callback,
                      void *user_data);

/**
 * @brief  禁用VAD检测
 * @param  ctx: 音频上下文指针
 */

void audio_vad_disable(audio_context_t *ctx);

/**
 * @brief  设置VAD能量阈值
 * @param  ctx: 音频上下文指针
 * @param  threshold: 能量阈值
 */

void audio_vad_set_threshold(audio_context_t *ctx, uint32_t threshold);

/**
 * @brief  计算音频帧能量
 * @param  data: 音频数据
 * @param  frames: 帧数
 * @return 平均能量值
 */

uint32_t audio_calc_energy(const int16_t *data, size_t frames);

/**
 * @brief  获取音频模块状态
 * @param  ctx: 音频上下文指针
 * @return 状态枚举
 */

audio_state_t audio_get_state(audio_context_t *ctx);

/**
 * @brief  获取状态名称字符串
 * @param  state: 状态枚举
 * @return 状态名称字符串
 */

const char *audio_get_state_name(audio_state_t state);

#endif /* __AI_AUDIO_H */
