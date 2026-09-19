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

/* 内置"启发式"异常声音检测的总开关。
 * 1 = 启用（sound_detect_start() 起检测线程，喂进来的音频会被实时打分）；
 * 0 = 关闭（不起线程、喂进来的音频直接丢弃，只在收到
 *     sound_detect_report_anomaly() 时才走回调）。
 *
 * 为什么默认关：没有真模型时走的是 sound_detect_run_fallback()（纯能量/过零率），
 * 一声脆响——包括板子自己喇叭放的提示音——就能拿到 0.9 的"跌倒"分，实测误报。
 * 队友训练好的模型接进来之后，把这里改成 1（或从 Kconfig 传）即可，
 * 上层（机器人的提醒/报警/推送）一行都不用动。
 * 接入点：CONFIG_HELLO_APP_EDGE_IMPULSE + edge_impulse_sound_classify()，
 * 或 sound_detect_load_model_file() 加载模型文件。 */
#ifndef SOUND_DETECT_HEURISTIC_ENABLE
#  define SOUND_DETECT_HEURISTIC_ENABLE  0
#endif

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

  /* 连续命中确认（抑制误报）：同一个类别要连续 N 个窗口都超阈值才上报。
   * fallback 分类器是纯启发式的，一声脆响就能拿到 0.9 的"跌倒"分。 */
  int                 confirm_type;   /* 正在确认的类别（SOUND_TYPE_NONE = 无） */
  int                 confirm_count;  /* 已连续命中几个窗口 */

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

/* 上报一次"异常声音"（这是给外部检测来源用的统一入口）。
 *
 * 谁可以调：
 *   - 队友训练好的模型（Edge Impulse / 自己撸的推理）判出结果后；
 *   - 将来别的传感器（IMU 撞击、按键长按等）；
 *   - 手动测试：想验证"报警页 + 手机推送"这条链，不必真的摔一跤。
 *
 * 它做的事和内置检测器判出结果时完全一样：累加统计 + 调 sound_detect_init()
 * 里注册的 callback（robot_ui / ai_companion 那边接到回调就弹报警页、响铃、推手机）。
 *
 * @param ctx          sound_detect_init() 过的上下文（回调从它里面取）
 * @param type         SOUND_TYPE_FALL / HELP / SCREAM / KNOCK ...（不能是 SOUND_TYPE_NONE）
 * @param confidence   0.0 ~ 1.0，用你自己的模型给的置信度
 * @return OK / -EINVAL
 */
int sound_detect_report_anomaly(sound_detect_context_t *ctx,
                                sound_type_t type, float confidence);

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

/****************************************************************************
 * 任务四：麦克风 PCM 旁路 + 「人声 / 非人声」门控
 *
 * 产品逻辑（用户拍板）：音频里**有人在说话** → 整段交给云端（现有
 * VAD → ASR → 大模型那条路，本模块只负责"认出人声并提示一声"）；
 * **没有人声但能量/形态异常** → 才喂给本地小模型（任务三的 sound_event）。
 * 本机小模型因此不用认识"救命"两个字，"呼救"按人声走云端。
 *
 * 数据流（旁路，绝不碰主录音链路）：
 *   ai_audio.c 录音线程 read() 到一帧
 *     → sound_detect_pcm_tap()（**非阻塞**，只往有界环形缓冲 memcpy）
 *     → 门控线程（独立、低优先级）每 200ms 取一个 0.64s 窗
 *         ├─ 播放态（板子自己喇叭在响）→ 整窗丢弃，不判
 *         ├─ 像人声 → voice_cb（接线方复用现有云端语音入口）
 *         └─ 非人声且能量超阈 → sound_event_feed()（本地小模型）
 *
 * 为什么要有界 + 丢最旧：门控线程慢一点没关系，**绝不能**让录音线程的
 * read() 等它 —— 麦克风是半双工、app 已常开独占，堵住录音就是"整机聋了"。
 ****************************************************************************/

/* 门控窗口口径（与任务三的 mel 前端契约一致）：
 * 一个窗口 = 64 帧 × 160 样本 = 0.64s；滑窗步长 20 帧 = 200ms。 */
#define SOUND_GATE_WINDOW_SAMPLES   10240
#define SOUND_GATE_HOP_SAMPLES      3200

/* 有界队列容量（样本数）：2 个窗 ≈ 40 KiB。满了丢最旧的样本，不阻塞写入方。 */
#define SOUND_GATE_RING_SAMPLES     (2 * SOUND_GATE_WINDOW_SAMPLES)

/* 门控线程优先级：数值比 CONFIG_HELLO_APP_PRIORITY(100) 大 = 更低优先级 */
#define SOUND_GATE_THREAD_PRIORITY  110
#define SOUND_GATE_THREAD_STACK     8192

/* 门控一次判断用的特征（全部归一化到 0..1，便于打印和门限解释）*/
typedef struct
{
  float energy;        /* 窗口平均能量（int16 幅度平方的均值，可 > 1） */
  float rms;           /* 归一化 RMS（0..1，1.0 = 满量程） */
  float zcr;           /* 过零率（0..1）：宽带冲击高，浊音低 */
  float ac_peak;       /* 自相关峰值比（0..1）：周期性强（浊音）才高 */
  float voiced_ratio;  /* "浊音样"分析帧占比（0..1） */
  int   active;        /* 1 = 能量高于静音底噪 */
  int   voice;         /* 1 = 判为像人声 */
} sound_gate_features_t;

/* 门控运行统计（调试入口 / 汇报用）*/
typedef struct
{
  uint32_t windows;         /* 真正判过的窗口数（不含播放态跳过的）*/
  uint32_t voice_windows;   /* 判为人声的窗口数 */
  uint32_t mute_windows;    /* 因播放态被整窗跳过的数量 */
  uint32_t anomaly_windows; /* 判为非人声异常、已喂本地模型的窗口数 */
  uint32_t last_voice_ms;   /* 最近一次人声的单调毫秒 */
  uint32_t last_anomaly_ms; /* 最近一次异常的单调毫秒 */
  sound_gate_features_t last;  /* 最近一个判过的窗口的特征 */
} sound_gate_stats_t;

/**
 * @brief  人声门控判据（纯函数，无副作用，可在主机上单测）
 *
 * 三个特征：整窗 RMS（能量）、过零率 ZCR、逐 32ms 帧自相关峰值比。
 * 经验判据（灵敏度 sens 越大越容易判成人声）：
 *   静音     : rms < 0.004/sens                  → 不活跃
 *   像人声   : 活跃 且 （浊音帧占比 ≥ 0.25/sens
 *                       或 自相关峰 ≥ 0.60/sens 且 zcr ≤ 0.35）
 *   其余     : 非人声（宽带冲击/噪声/静音）
 *
 * @param  pcm   16kHz 单声道 s16le 样本
 * @param  n     样本数（建议 ≥ SOUND_GATE_WINDOW_SAMPLES）
 * @return 1 = 像人声, 0 = 不像, 负值 = 参数非法
 */

int sound_detect_voice_gate(const int16_t *pcm, size_t n);

/**
 * @brief  同上，但把中间特征也带出来（调试 / 打印 / 单测用）
 * @param  out   非 NULL 时填入特征与判决
 */

int sound_detect_voice_gate_ex(const int16_t *pcm, size_t n,
                               sound_gate_features_t *out);

/**
 * @brief  旁路喂入 PCM（**录音线程里调，绝不阻塞**）
 * @return 0成功（可能因队列非空/满而丢样本，不算错）
 */

int sound_detect_pcm_tap(const int16_t *pcm, size_t n);

/**
 * @brief  启动/停止门控线程（幂等）。旁路在 stop 之后仍然安全可调（直接丢）
 */

int sound_detect_bypass_start(void);
void sound_detect_bypass_stop(void);

void        sound_detect_gate_set_enabled(bool on);
bool        sound_detect_gate_enabled(void);
void        sound_detect_gate_set_sensitivity(float sens);   /* 0.3 ~ 3.0 */
float       sound_detect_gate_sensitivity(void);

/**
 * @brief  注册"板子自己正在出声"的判据（在门控线程里调，必须快、不加锁）
 *
 * 播放态返回 true → 整个窗口直接丢弃（不判、不喂本地模型）。这是历史误报的
 * 直接来源：自己的 TTS/提示音被纯能量启发式判成"异常声"。
 */

typedef bool (*sound_detect_busy_cb_t)(void *arg);
void sound_detect_gate_set_busy_cb(sound_detect_busy_cb_t cb, void *arg);

/**
 * @brief  注册"判到人声"的回调（在门控线程里调）
 *
 * ⚠️ 回调里**只许置标志**，不要在里面推状态机 / 做网络 / 播报 —— 它跑在
 * 门控线程上，拖住它就等于丢门控窗口。接线方（ai_companion_main.c）只置一个
 * 标志，真正的"送去云端"放到主循环 100ms 心跳里做。
 */

typedef void (*sound_detect_voice_cb_t)(const sound_gate_features_t *f,
                                        void *arg);
void sound_detect_gate_set_voice_cb(sound_detect_voice_cb_t cb, void *arg);

const sound_gate_stats_t *sound_detect_gate_get_stats(void);
void sound_detect_gate_reset_stats(void);

/****************************************************************************
 * 任务四：跨模块弱依赖声明
 *
 * 下面三个符号分别由**别的 agent** 负责落地（任务三的 sound_event.h /
 * robot_ui 的询问框）。本文件先弱声明 + 在 ai_sound_detect.c 里给弱实现兜底：
 * 对方还没落地时能编、能链、能跑（只是本地模型不生效 / 询问框打在串口里），
 * 对方落地后强符号天然覆盖，这里一个字都不用改。
 ****************************************************************************/

/* 任务三：本地小模型入口（sound_event.h） */
int sound_event_init(void);
int sound_event_feed(const int16_t *pcm, size_t nsamples);

/* robot_ui：弹「是否报警？」询问框。任何线程都可调，内部自己投到 LVGL 线程。
 * （本该声明在 robot_ui/robot_ui.h，但那个头 include 了 LVGL，hello_app 编不了，
 * 所以在这里弱声明。） */
void ui_post_ask_alarm(const char *reason);

/* robot_ui：语音追问那一路对这次异常已经有结论了 —— 屏幕上那页询问框让位。
 * 语义是「作废」：撤页面，既不报警、也不记「用户否认」的静默期（它不代表用户
 * 回答过）。声明在这里的理由同上（robot_ui.h 带 LVGL，hello_app 编不了）。
 * 弱实现见 ai_sound_detect.c：robot_ui 没进镜像时什么都不做。 */
void ui_post_ask_standdown(const char *src);

/* robot_ui：报警去重闸（同一次异常的两条确认入口只许报一次警）。
 * true = 这次报警由本路执行；false = 另一条确认入口（屏幕按钮 / MQTT）已经报过了，
 * 本路放弃。声明在这里的理由同上。弱实现在 ai_sound_detect.c：robot_ui 那一侧
 * 没进镜像时返回 true（没有第二条路可去重，照常报警）。 */
bool robot_ui_alarm_claim(const char *src);

#endif /* __AI_SOUND_DETECT_H */
