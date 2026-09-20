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

/* 单次播放的时长上限（audio_play_start / audio_play_file 超过它就返回 -ENOSPC，不截断）。
 *
 * 30 秒 @16k 单声道 16bit = 16000 × 1 × 2 × 30 = 960000 字节 ≈ 938 KiB **堆**分配，
 * 而且是在 audio_init() 里一次性分配、**常驻**（原来是 8 秒 / 256 KB）。
 * 真机 heap_info 约 8.65 MB 堆、空闲 7.27 MB（见 docs/ai_agent_usage.md 第 7 节），
 * 多这 700 KB 没问题。
 *
 * 为什么是 30 秒（这里有个取舍）：
 *   - 提醒语音最长 6 秒、要念两遍 = 12 秒，8 秒的旧上限会让第二遍直接 -ENOSPC
 *     （上层表现是"一声不响"）；
 *   - 云端 TTS 是**分块合成**的（见 mimo_voice.c），300 字的长回复 ≈ 66 秒，
 *     全放下要 2.1 MB；play_buf 是常驻的，再往上抬等于白占内存。
 *     所以取 30 秒（≈130 字）覆盖绝大多数回复，更长的在 TTS 那层按"装不下多少
 *     就念多少"截断并打日志（不会失败，也不会静默）。
 *   想更长：这里的 AUDIO_PLAY_BUFFER_MS 和 app/robot_ui/main.c 的
 *   VOICE_TTS_BUF_BYTES **必须一起改**（后者要 ≥ 16000 × 2 × 秒数），
 *   否则只是把 -ENOSPC 从播放层挪到 TTS 缓冲那层。 */

#define AUDIO_PLAY_BUFFER_MS         30000

/* VAD (Voice Activity Detection) 参数 */
#define AUDIO_VAD_ENERGY_THRESHOLD   500     /* 能量阈值 */
#define AUDIO_VAD_SILENCE_TIMEOUT_MS 3000    /* 静音超时3秒 */
#define AUDIO_VAD_MIN_SPEECH_MS      300     /* 最小语音长度300ms */

/* 音量范围（本模块对外口径 0..100）
 * 注意：nuttx/audio/audio.h 里也有个同名宏 AUDIO_VOLUME_MAX，那是驱动侧的
 * 0..1000。同一个文件里同时 include 两个头文件时要 #undef 让位，
 * 写法见 ai_audio.c 的 Included Files。 */
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
  int                 record_fd;      /* 保留字段：录音 fd 由板级封装 audio_in_* 持有 */
  int16_t            *record_buf;     /* 录音缓冲区 */
  size_t              record_buf_size; /* 缓冲区大小(字节) */
  volatile bool       record_stop;    /* 停止录音标志 */
  bool                record_thread_valid; /* 录音线程需要回收 */
  volatile bool       record_exited;  /* 录音线程已跑完（收尾用，见 audio_record_stop） */
  volatile bool       record_died;    /* 录音异常中断（不是谁让它停的；
                                       * 偶发单次读超时不算，见 ai_audio.c 的容忍上限）。
                                       * 只在线程**收尾时**才置位（recording 已经置
                                       * false 之后、record_exited 之前）：诊断出死因
                                       * 不等于立刻对外可见，理由见 ai_audio.c 录音
                                       * 线程末尾 */

  /* 数据流活跃度观测：把"线程还活着、设备却已经不出数据了"这种假健康
   * 变成看得见的东西。
   *
   * 为什么需要：上面那四个录音标志（recording / record_thread_valid /
   * record_stop / record_exited）全都只是"**软件**还在不在录"的证据，没有
   * 一个能证明"设备还在给数据"。它们答不上来的那种形状（DMA 不再产生完成
   * 中断、audio_in_read() 永远返回不了）只有这几个数看得见，所以它们是
   * **观测**，不是判据：diag 快照（ai_companion_main.c 的 diag 那一段）把
   * idle / wait 两栏打出去，"设备还在不在给数据"一眼可见。
   * 判"该不该重开麦"只看会话标志（录音不活跃就按退避重开），不拿这几个数
   * 当门 —— 2026-09-15 试过那种"数据流像死了"的判据，它自己会把恢复永久
   * 挡死（现场刷屏 196 秒、谁也开不了麦），理由见 listen_supervise_tick 的
   * 说明。
   *
   * 下面这几个字段记的就是"这一滴真数据 / 这次等待"，只由录音线程写
   * （record_last_data_ms 另在 audio_record_start() 里种一次）。
   * 读的人请走 audio_record_* 那几个访问器，别直接看字段：完整的语义
   * （以及各自的坑）写在头文件下方原型那里的注释里。 */

  volatile uint32_t   record_last_data_ms;  /* 最近一次"数据流有动静"的时刻（uptime 毫秒）；
                                             * 0 = 从没启动过录音，见 audio_record_idle_ms() */
  volatile uint32_t   record_read_start_ms; /* 当前这次 audio_in_read() 开始等待的时刻；
                                             * 0 = 此刻没在等，见 audio_record_wait_ms() */
  volatile uint32_t   record_empty_reads;   /* 连续"读了却没拿到字节"的次数（读超时会累加，
                                             * 到上限才算会话死），读到数据就清零，
                                             * 见 audio_record_empty_reads() */
  volatile int        record_last_result;   /* 最近一次 read 的结果；0 = 拿到了数据，
                                             * 见 audio_record_last_result() */

  /* 播放相关 */
  bool                playing;        /* 是否正在播放 */
  audio_play_complete_cb_t play_cb;   /* 播放完成回调 */
  void               *play_user_data; /* 播放回调用户数据 */
  int                 play_fd;        /* 保留字段：播放 fd 由播放线程自己 open/close */
  int16_t            *play_buf;       /* 播放缓冲区 */
  size_t              play_buf_size;  /* 缓冲区大小(字节) */
  volatile bool       play_stop;      /* 停止播放标志 */
  size_t              play_frames;    /* 当前播放帧数 */
  bool                play_thread_valid; /* 播放线程需要回收 */
  volatile bool       play_exited;    /* 播放线程已跑完（回收用，见 audio_reap_play_thread）
                                       * 语义和 record_exited 完全对称：线程最后一步才置位。
                                       * 为什么要它：NuttX 的 pthread_join 没有超时参数，
                                       * 裸 join 一旦线程卡在设备里就是永久等待；而这条 join
                                       * 永久等待会把整个 hello_app 主循环一起钉住（起播和
                                       * 监听守护的停开麦都会走到这条回收）。所以回收
                                       * 改成"有界轮询这个位 + 放弃 + 日志"。 */
  volatile int        play_last_result; /* 上一次播放的结果：0=整段放完，负值=没出声的原因
                                         * （-EINPROGRESS=这次还没结束）。只由播放线程写，
                                         * 读的人请用 audio_play_last_result()，语义见那里。 */
  bool                record_resume_on_play_end; /* 播放是为它停的录音，播完恢复 */

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
 * @param  config: 录音配置, NULL 表示不启用 VAD/回调
 * @return 0成功, 负值失败
 *
 * 走板级封装 sf32lb52_audio_in（设备 /dev/audio/audio0，16k/单声道/s16le）。
 * 本板是半双工：调用时如果正在播放，会先 audio_play_stop() 停掉播放；
 * 参数非法/设备忙时返回负 errno（-EINVAL / -EBUSY 等）。
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
 * @brief  录音这条链路是不是真的还活着（录音线程仍在跑）
 * @param  ctx: 音频上下文指针
 * @return true设备已 START 且录音线程仍在, false线程已退出/正在停/从未启动
 *
 * 给上层的"监听守护"用：audio_is_recording() 只说明 process 认为在录，
 * 录音线程因为驱动 AUDIOIOC_STOP 或连续读超时到上限（audio_in_read 返回 0 /
 * 一直拿不到数据）自己跳出循环之后，它同样会（并且只来得及）把
 * recording 置 false，但线程退出与 record_stop 的中间状态只有本函数能分清。
 * 注意它看不穿驱动：驱动把通路 STOP 掉、而线程还阻塞在 read 里的那一段
 * （最长下层那 5 秒）仍算"活着"；**偶发的单次读超时也不再算死**（线程会跳过
 * 那一帧接着读，见 ai_audio.c 的 AUDIO_RECORD_TIMEOUT_TOLERANCE）。
 *
 * 配套的 ctx->record_died 回答的是另一个问题："线程为什么退出的"：
 *   - false：线程是被 audio_record_stop() 停的（正常收尾，含放音前的半双工
 *     让路），或者根本没死 —— 上层按退避重开就行；
 *   - true ：没人要求停，read 自己返回了 0（EOF：驱动 STOP 了通路 / 设备被别的
 *     会话抢走）或返回了非超时的负值；也包括"连续读超时超过
 *     AUDIO_RECORD_TIMEOUT_TOLERANCE 次"（偶发的单次超时会被跳过，不置这个位）。
 *     这一代录音会话**不会再回来**了，上层该立刻重开，而不是等状态机 /
 *     守护自己的超时。
 *     ⚠️ 这一支的收尾按**死因**分岔（死因在发现死亡那一刻就定下，见 ai_audio.c）：
 *       · read 返回 0/EOF —— 是**别人**把这一代停掉的（设备已被别的会话接管，
 *         半双工下对方的设备级 STOP 会同时停掉录放两条通路）→ 收尾只关本层
 *         fd（abandon），**不发设备级 AUDIOIOC_STOP**：再发一次就会把刚接管
 *         设备的那个会话（现场多为正在播报的 TTS）一起打死（现场老串口里
 *         "录音异常中断…会话已死"后面紧跟播报半路哑掉，就是这条路径；那行日志
 *         现在按死因拆成了"录音被外部停掉"和"录音异常中断"两行）。关到"这个设备上
 *         最后一个 fd"时 NuttX 上层才会去动驱动（hw_shutdown，见
 *         nuttx/audio/audio.c:263-275），所以没人接管时设备照样被放回去，不会漏。
 *       · 连续读超时超过容忍次数（没有任何人发过 STOP，这一代很可能就是我们
 *         自己在用）→ 照旧 audio_in_stop()：它是"最后使用者"该做的收尾，
 *         只有它会在安全上下文里把驱动停干净（清 running、停 DMA、关功放），
 *         而随后的 close 在我们确实是这个设备最后一个 fd 时还会释放上层那份
 *         status —— STOP 留下的 DRAINING 正是在那一刻被带走的（不主动 STOP
 *         就没人来收这一代：abandon 只 close 自己的 fd，而且还唤醒不了卡在
 *         read 里的线程）。
 * 它是"一次性事件"标志：由 ai_audio 置位、由**上层认领后自己清掉**
 * （audio_record_start() 起新一代线程时也会清）。
 */

bool audio_record_is_active(const audio_context_t *ctx);

/**
 * @brief  录音这条链路"安静"了多久：距上一滴数据流动静（或这一代录音开始）多少毫秒
 * @param  ctx: 音频上下文指针
 * @return >=0 毫秒；-1 = 从来没启动过录音（没有参照点，谈不上"安静了多久"）
 *
 * 两个参照点都写在本模块里：
 *   - audio_record_start() 起这一代会话的时刻（先种一次）；
 *   - 录音线程每**成功读到一段数据**的时刻。
 * 所以它不只是"距上次读到数据"，而是"距这一代录音最后一次有进展"：
 * "设备 START 好了、线程也起了、却一个字都没读到"同样会随时间增长。
 *
 * 用途是**观测**（diag 的 idle 一栏），不是判据：它答不了"该不该重开麦"。
 * VAD 报"说完了"之后那一整段 ASR + 大模型 + TTS 是在**录音线程的 VAD 回调里**
 * 同步跑的（ai_state_machine.c 的 sm_ai_talking_enter 直接调
 * process_ai_dialogue），那期间一次 read 都不会发生，正常对话就能让它涨到
 * 几十秒 —— 拿它当"死没死"的门，正常对话每轮都会白重开一次麦。
 *
 * 写入者是录音线程（外加 audio_record_start 种的那一次），读的是别的线程：
 * 不用加锁，靠 volatile + 有符号差值算间隔（uint32 绕一圈约 49 天也不会算反）。
 */

int audio_record_idle_ms(const audio_context_t *ctx);

/**
 * @brief  录音线程此刻"等设备给数据"等了多久（观测用）
 * @param  ctx: 音频上下文指针
 * @return >=0 = 线程**正阻塞在 audio_in_read() 里**、已经等了这么多毫秒；
 *         -1 = 此刻没有人在等（没启动 / 线程正在数据回调里 / 正在收尾）
 *
 * 它和 audio_record_idle_ms() 的分工：那个是"距上一滴数据多久"（ASR 那几十秒
 * 里照样涨），这个是"此刻到底在不在等设备"（那几十秒里恒为 -1）。两个一起看
 * 才分得清"没在等"（-1，正常）和"一直在等"（数据流可能死了）。
 *
 * ⚠️ -1 **不代表健康**，只代表"现在没人在等"；反过来"一直在等"也不足以判死 ——
 *    线程刚被别人喊停的那一瞬（AUDIOIOC_STOP 已经发下去、read 还没返回）同样
 *    算"在等"。
 *
 * 所以它在 hello_app 里只进 diag 快照（idle / wait 两栏），**不做门**：
 * 2026-09-15 拿它当"数据流已死"的判据用过一轮，结果是那道判据自己把重开麦
 * 永久挡死（现场刷屏 196 秒、谁也开不了麦），理由见 listen_supervise_tick 的
 * 说明。真要重开麦只按会话标志判（录音不活跃就重开）。
 *
 * 顺带说一句"真要重开"的正确顺序（将来谁要用）：先 audio_record_stop()、
 * 再 start —— 线程还在的时候 ctx->recording 仍是 true，直接
 * audio_record_start() 只会拿到 -EBUSY（本模块不会替调用者停一个"看起来还在
 * 录"的会话），而 stop 顺带把设备 STOP 掉、唤醒那次卡住的 read。
 */

int audio_record_wait_ms(const audio_context_t *ctx);

/**
 * @brief  最近一次录音 read 的结果
 * @param  ctx: 音频上下文指针
 * @return 0 = 最近一次 read 真的**拿到了数据**（通路还在给东西）；
 *         -EINPROGRESS = 这一代录音起来了、但还没有任何一次 read 有结果；
 *         -ETIMEDOUT = 最近一次 read **分片等待超时**（下层 5 秒没等到 DMA 完成，
 *                      驱动用这个负值把它区分出来）。注意它**不代表**会话结束 ——
 *                      录音线程会跳过这一帧接着读，只有连续超时超过
 *                      ai_audio.c 的 AUDIO_RECORD_TIMEOUT_TOLERANCE(5) 次才收摊；
 *         -ECANCELED = 最近一次 read 返回 0（EOF：被 AUDIOIOC_STOP 打断 /
 *                      会话换代 —— "别人把设备拿走了"），这一代会话到此为止，
 *                      收尾只 close、**不发 STOP**；
 *         -ENODEV    = 最近一次 read 报"设备没在跑"（驱动侧确认，**不是**被抢走）。
 *                      处置和上面一条正好相反：收尾要 STOP + close 把设备收干净，
 *                      否则框架那份共享 status 一直被钉着，后面每个会话都起不来；
 *         其他负值 = read 原样返回的负值（未 start 的 -EINVAL、fd 失效等）。
 *
 * 用途：回答"为什么断了"。diag 快照（ai_companion_main.c）把它和
 * audio_record_idle_ms() 一起打出来，一眼能分出是设备超时、fd 没了，还是
 * "压根没等到 read 返回"（那种情况这里还停在 -EINPROGRESS，说明线程连第一次
 * read 都没出来）。
 *
 * 它只是**事后**的错误码：只说明上一次 read 的结局，不说明现在数据流还在不在。
 */

int audio_record_last_result(const audio_context_t *ctx);

/**
 * @brief  连续"读了却没拿到一个字节"的次数
 * @param  ctx: 音频上下文指针
 * @return 次数（读到一段数据就清零）
 *
 * 这个数现在真的会累加：录音线程对"读超时"是容忍的（跳过这一帧接着读），
 * 只有**连续**超过 AUDIO_RECORD_TIMEOUT_TOLERANCE(5) 次才按"会话死了"收摊，
 * 而每读到一次真数据就清零（清零时还会打一行"读已恢复"）。所以它等于
 * "当前这一段连续超时有多长"，日志里用它看这次抖动连了几拍。
 * read 返回 0（EOF）和别的负值仍然立刻跳出循环，那两条路上它最多到 1。
 */

uint32_t audio_record_empty_reads(const audio_context_t *ctx);

/**
 * @brief  开始播放音频数据
 * @param  ctx: 音频上下文指针
 * @param  data: 音频数据（16k/单声道/s16le）
 * @param  frames: 帧数
 * @param  callback: 播放完成回调（在播放线程里调用；被 stop 打断时不调用）
 * @param  user_data: 回调用户数据
 * @return 0成功, 负值失败
 *
 * 半双工：调用时如果正在录音，会先 audio_record_stop() 停掉录音，
 * **播完（或写失败退出）再按原配置自动恢复录音** —— 因为 ai_companion 的录音是
 * 常开监听、只在开机时启动一次（ai_companion_main.c 的 start_audio_listening），
 * 不自动恢复的话第一次 TTS 之后就没有麦克风了。要让上层自己管录音，
 * 删掉 ai_audio.c 播放线程末尾那段"恢复录音"即可。
 * data 超过播放缓冲（AUDIO_PLAY_BUFFER_MS）返回 -ENOSPC，不截断。
 * 实际出声由播放线程完成（本线程自己 open/write/close 设备）。
 *
 * ⚠️ 本函数返回 0 只代表"数据拷进去了、播放线程起来了"，**不代表会出声**
 *    （设备/START 都要到播放线程里才动）。而完成回调的语义是"播放线程收尾了"，
 *    设备打不开、START 被 -EBUSY 拒掉、写失败时同样会回调 —— 回调被调用**不等于**
 *    有声音。要知道"这次到底出声没有"，读 audio_play_last_result()。
 */

int audio_play_start(audio_context_t *ctx,
                     const int16_t *data, size_t frames,
                     audio_play_complete_cb_t callback,
                     void *user_data);

/**
 * @brief  从文件播放音频
 * @param  ctx: 音频上下文指针
 * @param  filepath: 音频文件路径（裸 PCM：16k / 单声道 / s16le）
 * @param  callback: 播放完成回调
 * @param  user_data: 回调用户数据
 * @return 0成功, 负值失败
 *
 * 不解析 WAV/MP3 等容器格式（本板没有解码器），文件要和
 * `audio_test record <ms> <file>` 存出来的裸 PCM 一致。
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
 * @brief  上一次播放到底出没出声
 * @param  ctx: 音频上下文指针
 * @return 0 = 整段都写进了设备、正常放完（真出声）；
 *         -EINPROGRESS = 这一次播放还没结束（刚开始，或者正在放、还没写满）；
 *         其他负值 = 没出声的原因，就是播放层记下的错误码
 *                    （-EBUSY: START 被驱动拒掉；-EIO: 写设备失败；
 *                     -errno: 设备打不开；-ECANCELED: 中途被 stop。按 0 以外皆为"不可信"处理即可）。
 *
 * 为什么需要它（播放完成回调回答不了这个问题）：
 *   audio_play_start() 只是 memcpy + pthread_create 就返回，真正
 *   open/CONFIGURE/START/write 全在播放线程里；而**完成回调的语义是"播放线程收尾了"，
 *   不是"声音出来了"** —— START 被驱动以 -EBUSY 拒掉（半双工，另一方向的会话正占着
 *   通路）、write() 返回 <= 0 时，播放线程照样走到末尾并且照样回调，为的是让上层状态机
 *   （hello_app 的 TTS 播报）不会永远等一个不会来的完成事件。所以"回调被调了"和
 *   "有声音"是两件事，只有本接口能区分。
 *   回调语义保持原样（有人靠它推状态机），本接口是**加**出来的旁路信息。
 *
 * 谁写：ai_audio.c 的播放线程（唯一写入者），写在完成回调**之前** ——
 *       所以回调里/回调返回后读到的都是最终值。
 * 谁读：robot_ui/main.c 的 reminder_play_exclusive()（判"这声铃到底响没响"）。
 * 为什么不直接把 ctx->play_last_result 字段暴露给上层：那是写入方的私有物，
 *       读取方只该认这个语义（以后加错误码、改判据只动一处），而且本函数对 NULL 安全。
 */

int audio_play_last_result(const audio_context_t *ctx);

/**
 * @brief  设置音量
 * @param  ctx: 音频上下文指针
 * @param  volume: 音量 0-100
 * @return 0成功, 负值失败
 *
 * 会即时下发到驱动（AUDIOIOC_CONFIGURE + AUDIO_TYPE_FEATURE +
 * AUDIO_FU_VOLUME，换算成驱动的 0..1000）。设备打不开时返回负 errno，
 * 但软件音量已经记下（audio_get_volume 返回新值）。
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
 * @brief  把 VAD 里"正在说一句话"的相位收掉（外部提前收尾时用）
 * @param  ctx: 音频上下文指针
 *
 * 只清 VAD 的内部相位（vad_speech_active / 静音与语音帧计数），**不回调上层**：
 * 上层的"这一段说完了"由调用方自己走（ai_companion_main.c 的
 * speech_capture_complete()）。这样"语音结束"这件事在两条路上只有一份语义 ——
 * 静音超时那条（vad_callback(false)）和外部请求提前收尾这条（镜像面板底部
 * 「提交」）。
 *
 * 为什么外部收尾**必须**调它：相位不复位的话，外面提前收尾之后静音帧计数还在
 * 累加，3 秒后静音超时会把**同一段音频**再报一次"语音结束"，同一句话会被送去
 * 识别两次。
 *
 * ⚠️ 只能在录音线程里调（现在唯一调用点是录音线程的数据回调，就排在 VAD 判定
 * 后面几行）：它读改的就是 VAD 那几个字段，别的线程同时调进来就是并发写。
 */

void audio_vad_end_speech(audio_context_t *ctx);

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
