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

/* 播放缓冲的时长上限（audio_play_start 拷贝路径 / audio_play_file 整文件
 * 读入超过它就返回 -ENOSPC，不截断）。
 *
 * ⚠ 2026-10-19 从 70 秒砍回 16 秒 —— 这不是把截断挪个地方，而是**整个耦合
 * 被拆掉了**：TTS 全段播放现在走 audio_play_start_ref()（直读调用方缓冲、
 * 零拷贝），play_buf 根本不参与 TTS，只服务三类短音频：
 *   - 追问/灯控的预缓存提示（几十 KB）与 live 回落（ASK/LIGHT_TTS_BUF=128 KB）；
 *   - 关怀播报 CARE_TTS_BUF=480 KB（= 15 秒整）；
 *   - 提醒铃 audio_play_file 整文件读入（提醒语音最长 6 秒 × 2 遍分两段，
 *     单段 ≤ 6 秒 = 192 KB）。
 * 16 秒 = 512 KB，给 480 KB 的关怀播报留 32 KB 余量。
 *
 * 为什么必须砍（2026-10-19 真机取证）：70 秒 play_buf = 2.24 MB **常驻**，
 * 而且 hello_app 和 robot_ui **各有一份**（两个 audio_context_t）＝ 4.48 MB；
 * 叠上 TTS 预分配 2 MB、语音累积缓冲 480 KB、系统+LVGL+镜像 ~3.7 MB，
 * 开机水位只剩 606 KB —— 主循环任务 16 KB 的栈都建不起来（pthread_create
 * 返回 ENOMEM）→ main 退出 → 看护重拉 → 每一代重演 → hello_app 崩溃重启
 * 死亡螺旋（串口实测 100+ 代、堆钉死在 141 KB 空闲、录音/门控/关怀全部
 * -12、连 USB gadget 都被饿死）。砍掉这两份 2.24 MB 后开机空闲回到 ~4 MB，
 * 死亡螺旋的土壤不存在了。
 *
 * 2026-10-17 把它从 30 秒抬到 70 秒的原始动机（"101 字回复只念了 51 字"）
 * 其实是 TTS 懒分配 2 MB 失败落到 32 KB 兜底造成的，与 play_buf 无关 ——
 * 当时诊断错了对象；真正的修复是 tts_buf_ensure() 开机预分配。 */

#define AUDIO_PLAY_BUFFER_MS         16000

/* VAD (Voice Activity Detection) 参数
 *
 * ⚠⚠ 2026-10-25 真机推翻了一条根因，先记在最前面 ⚠⚠
 *
 *   **2026-10-24 把能量阈值 500 → 300 的那一刻，静音判停就变成死代码了。**
 *   实测这台机器的房间底噪：峰值 296、RMS 27 ⇒ 帧能量 = RMS² ≈ **729**。
 *   而本文件的判「有声」条件是 `帧能量 > 300`（⇒ RMS ≥ 17.3）——
 *   **底噪 729 已经是门槛 300 的 2.4 倍**，于是每一帧都算「有声」，
 *   `vad_silence_frames` 每帧被清零，永远走不到静音帧数（当时 8000ms ⇒ 400 帧）。
 *   真机证据（2026-10-25 串口）：一段**纯房间噪声**（峰值 296、RMS 27）累积满了
 *   15 秒缓冲才被镜像面板的「提交」掐断，8 秒静音超时一次都没触发；
 *   另两段「语音结束」查下来也都是「提交」按出来的，不是判出来的。
 *
 *   也就是说：**在那之后，自动断句从来没工作过**，一直是靠面板「提交」兜着。
 *   「说完必须点提交」不是玄学，就是这一条。
 *
 * 修法（2026-10-25，用户确认）：门槛不再写死，改成 **自适应噪声门**，
 * 见下面 VAD_NOISE_* 与 ai_audio.c 的判据。静音超时同时 8000 → 1000 ——
 * 顺序不能反：门槛不先修好，调短超时是白调（计数器根本走不到）。
 *
 * ── 历史（2026-10-24 三项放宽，动机仍然成立，只是实现踩了上面的坑）──
 *
 *   能量阈值 500 → 300
 *     老人/方言说话中气弱、音量低，写死的高门槛会把句首句尾削掉。
 *     **动机是对的**，错在把它写成了一个绝对数。现在门槛跟着底噪走，
 *     只要人声比底噪强就进得来 —— 实测人声 RMS 419 / 底噪 RMS 27，差 240 倍，
 *     自适应门不会误杀小声说话，反倒把「安静屋子」和「吵屋子」都照顾到了。
 *
 *   静音超时 4000 → 8000 → **1000 ms**
 *     1000 ms 是「说完多久开始处理」的体感大头。用户明确说过
 *     **不需要为老人句中停顿留余量**（句中停顿被切断是本轮接受的取舍）。
 *     嫌快往大调（800~1500ms 试），嫌慢往小调，就这一个宏。
 *     配套：ai_companion_main.c 的 ASK_LISTEN_TIMEOUT_MS 必须 >= 本值。
 *
 *   最小语音 300 → 500 ms
 *     把 300~500ms 的短噪声/咳嗽/碰麦挡在门外，不至于误触发一轮对话。
 */

/* 门槛下限：自适应算出来的门槛不会低于它。保留 300 是为了**极安静的环境**
 * （深夜、消音室）里门槛不至于塌到 0 而把底噪当人声。 */
#define AUDIO_VAD_ENERGY_THRESHOLD   300

/* 静音超时：连续这么久的「无声」就判这一段说完了。 */
#define AUDIO_VAD_SILENCE_TIMEOUT_MS 1000

/* 最短语音：从「开始有声」到判「真的在说话」要攒够这么久。 */
#define AUDIO_VAD_MIN_SPEECH_MS      500

/* 自适应噪声门（2026-10-25）
 *
 *   有效门槛 = max(AUDIO_VAD_ENERGY_THRESHOLD, 底噪能量 × VAD_NOISE_FLOOR_MULT)
 *
 * 倍数取 4：实测人声能量 ≈ 175561、底噪 ≈ 729，差 240 倍，4 倍在中间靠下，
 * 给「老人小声说话」留了足量余量（再小声也只要超过底噪的 2 倍幅度）。
 * 嫌不灵敏往下调到 3；环境噪声忽大忽小导致误触发就往上调到 6。
 *
 * 帧能量超过 VAD_NOISE_LEARN_BLOCK 的**绝不拿去学底噪** —— 那些帧几乎必然
 * 是人声或突发噪声，混进去会把门槛抬到人声量级，之后小声说话全被自己挡掉
 * （自适应门最常见的自杀方式）。这个切点取 AUDIO_VAD_ENERGY_THRESHOLD 的 20 倍，
 * 落在实测「底噪 729 / 人声 175561」之间偏底噪一侧。 */
#define VAD_NOISE_FLOOR_MULT         4
#define VAD_NOISE_LEARN_BLOCK        (AUDIO_VAD_ENERGY_THRESHOLD * 20)

/* 播放线程走到哪一步（写进 audio_context_t::play_stage，**纯诊断**）。
 * 判读方法：audio_reap_play_thread() 放弃回收时会连着阶段一起打出来，
 * "卡在哪一步"一眼可见。0 = 还没开工，4 = 已收尾（正常情况不该被读到）。 */
#define PLAY_STAGE_NONE    0   /* 播放线程还没跑到 */
#define PLAY_STAGE_OPEN    1   /* 卡在 open / CONFIGURE / START */
#define PLAY_STAGE_WRITE   2   /* 卡在 write()（数据还没写完） */
#define PLAY_STAGE_CLOSE   3   /* 卡在 AUDIOIOC_STOP / close（收尾挂住） */
#define PLAY_STAGE_DONE    4   /* 走完了 */

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

  /* 录音线程的**代际号 + 认领握手**（2026-10-25 加，真机「测试几轮之后又不回答了」
   * 的根治点）。
   *
   * 现场形状（25 秒串口 + MQTT diag 双证，2026-10-25）：设备每 7.23 秒一圈
   * "判定僵尸会话 → 停掉再重开"，麦一次都没真开起来过：
   *     armed=0 busy=0（驱动里没人 read、DMA 没武装）
   *     wait=-1  idle=5168ms  empty=0  lres=-115(-EINPROGRESS)
   *     exit=0  rec=1  ract=1
   * `lres` 停在 audio_record_start() 种下的 -EINPROGRESS，说明新一代录音线程
   * **一次 audio_in_read() 都没返回过**；`wait=-1` 说明也不在 read 里；
   * `exit=0` 说明它走了「上一代录音线程退出…不碰设备，也不动任何标志」那条
   * 早退 —— 那条路径**故意不置 record_exited**（怕踩新一代的账本），于是
   * audio_record_is_active() 的 `!(record_stop || record_exited)` 永远为真，
   * 监听守护每 5 秒重开一次、永不升级，一次抖动就被放大成永久性"不能对话"。
   *
   * 这两个字段把那个洞堵死：
   *   record_gen     —— 每次 audio_record_start() 自增。线程把自己那一代的号
   *                     抄进**局部变量**（从线程自己的参数里拿，不读共享字段），
   *                     之后判"我还是不是当前这一代"只比这个号。比拿
   *                     pthread_t 比对可靠：ctx->record_thread 是个会被下一次
   *                     pthread_create 覆盖的共享字段，代际号不会。
   *   record_claimed —— 线程起跑后、进读循环之前自己置位，答的是"这一代到底
   *                     有没有真跑起来"。没认领就退的那一代，audio_record_is_active()
   *                     报假，守护立刻按"要常听却没在录"重开 —— **自愈**，
   *                     而不是把假健康一直挂着。
   *
   * 认领握手的时序余量：audio_record_start() 返回到线程真正跑起来是微秒级，
   * 而守护判"没在录"要等 LISTEN_SUPERVISE_WANT_MUTE_MS（秒级），中间这段
   * 窗口不会误触发。 */

  volatile uint32_t   record_gen;      /* 录音会话代际号，每次 start 自增 */
  volatile bool       record_claimed;  /* 这一代的线程已经跑起来并认领了 */

  /* 播放相关 */
  bool                playing;        /* 是否正在播放 */
  audio_play_complete_cb_t play_cb;   /* 播放完成回调 */
  void               *play_user_data; /* 播放回调用户数据 */
  int                 play_fd;        /* 保留字段：播放 fd 由播放线程自己 open/close */
  int16_t            *play_buf;       /* 播放缓冲区 */
  size_t              play_buf_size;  /* 缓冲区大小(字节) */
  const int16_t      *play_src;       /* 播放线程实际读的数据源：
                                       *   - 拷贝路径（audio_play_start /
                                       *     audio_play_file）＝ ctx->play_buf；
                                       *   - 零拷贝路径（audio_play_start_ref）
                                       *     ＝ 调用方自己的缓冲（如 g_tts_buf），
                                       *       生命周期契约见 audio_play_start_ref。
                                       * 播放线程只认这个指针，不再关心数据在
                                       * 哪里 —— 2026-10-19 加，为的是把 TTS
                                       * 全段播放从"必须拷进 2.24 MB play_buf"
                                       * 里解耦出来（见 AUDIO_PLAY_BUFFER_MS
                                       * 头上的取证说明）。 */
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
  volatile uint32_t   play_gen;       /* ★ 播放代数（2026-10-25 加）。每起播一代 +1，
                                       * 在跑的播放线程开工时抄一份去，在写块间隙和
                                       * 完成回调前核对：不等即弃。
                                       *
                                       * 为什么必须有：audio_reap_play_thread() 等不到
                                       * 线程退出时会**解闩放行**（否则整机永久哑巴，
                                       * 见那里的说明），被甩下的那条线程可能哪天从设备
                                       * 调用里醒过来。没有代数的话，它醒过来会接着往
                                       * 设备里灌数据、还会调 play_cb 把状态机再推一遍
                                       * —— 表现是"没人说话它自己响"。有了代数，它一
                                       * 核对就自己收尾退出。
                                       *
                                       * 就是小智那套 playback_generation_（计划 §B）：
                                       * 打断/切代只需 ++代数，在途结果自动作废。 */
  volatile uint8_t    play_stage;     /* ★ 播放线程走到哪一步（2026-10-25 加）。
                                       * **纯诊断，不改任何行为**：只由播放线程自己写，
                                       * audio_reap_play_thread() 放弃回收时把它打出来。
                                       *
                                       * 为什么必须有：2026-10-25 真机抓到一次播放线程
                                       * 卡死（解闩放行那条日志出现了），但只知道"卡在
                                       * open/write/STOP/close 里的某一处"，四选一没法
                                       * 立案。有了阶段标记，下次那行日志直接说卡在哪：
                                       *   open  → open/CONFIGURE/START 里（多半是设备被
                                       *           另一方向占着，START 等不到）
                                       *   write → write() 阻塞（DMA 不搬了 / 设备半死）
                                       *   close → AUDIOIOC_STOP/close 阻塞（驱动收尾挂住）
                                       * 取值见 PLAY_STAGE_*。 */
  volatile uint32_t   play_progress_ms; /* ★ 播放推进打卡（2026-10-25 加）。
                                       * 播放线程**每写完一块**盖一次章，起播时先盖一次。
                                       * audio_play_watchdog_tick() 看的就是"离上次盖章
                                       * 多久了"，太久 = 这条线程卡死了。
                                       *
                                       * 为什么必须有（2026-10-25 真机定案）：一次播报
                                       * 卡在 write 里 80 秒，期间 playing 一直为真 →
                                       * 「提交」全被判"正忙"、新一轮也起不来，整机
                                       * 80 秒不能对话。而自愈的那条「解闩放行」只在
                                       * **别人调 audio_play_stop()** 时才触发 —— 那 80
                                       * 秒里没人调（轮次看门狗在 ROUND_PHASE_IDLE 早退，
                                       * 追问/报警提示音压根不在"轮"里），于是只能等
                                       * 监听守护 80 秒后兜底。
                                       *
                                       * 用"推进"而不是"起了多久"当判据：合法的长回复
                                       * 可以连着念 60 秒以上（300 字上限、2 MB 缓冲），
                                       * 按总时长一刀切会把好好的长播报掐断。块是
                                       * AUDIO_PLAY_CHUNK_MS=250ms，驱动单次 write 上界
                                       * 是"一块+500ms"，所以正常情况下章是每 250ms 左右
                                       * 一个 —— 连着 5 秒没盖章，只剩"卡死了"一种解释。 */
  bool                record_resume_on_play_end; /* 播放是为它停的录音，播完恢复 */
  bool                record_hold;    /* ★ 整轮播报的"让路锁"（2026-10-24 加）。
                                       * 置真期间 audio_resume_record() 只欠着不还麦。
                                       * 为什么需要：流水线 TTS 是"首句 + 尾段"两段播的，
                                       * 中间有空档（尾段还在每 64 字一次 HTTPS 地合成）。
                                       * 按半双工契约首句一播完就把常开监听恢复起来，
                                       * 那几秒麦是开的 —— 旁边一有声音 VAD 就把状态机
                                       * 从"正在说"切到"正在听"，用户听到的就是
                                       * "AI 说话断掉、转去听、过一会自己接着说"。
                                       * 上层在整轮播报期间置真，轮子收尾调
                                       * audio_record_hold_end() 解锁并补上还麦。
                                       * audio_record_start() / audio_play_stop() 都会
                                       * 主动清它（有人明确要动设备，锁就该让路），
                                       * 加上收尾那条一共三条路，锁不会永久挂住。 */

  /* VAD相关 */
  bool                vad_enabled;    /* VAD是否启用 */
  audio_vad_cb_t      vad_callback;   /* VAD回调 */
  void               *vad_user_data;  /* VAD用户数据 */
  uint32_t            vad_energy_threshold; /* 能量阈值（自适应后的当前值） */
  uint32_t            vad_noise_floor_e;    /* 帧能量底噪估计（自适应噪声门） */
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
 * VAD 报"说完了"之后，**ASR 那一段还在录音线程的 VAD 回调里同步跑**
 * （ai_state_machine.c 的 sm_ai_talking_enter 直接调 process_ai_dialogue →
 * voice_asr_recognize），那期间一次 read 都不会发生，正常对话就能让它涨到
 * 几十秒 —— 拿它当"死没死"的门，正常对话每轮都会白重开一次麦。
 * （大模型和 TTS 已经挪到 llm_request_thread 了，不在这条同步段里；ASR 这段
 * 是目前仅剩的阻塞，也是"说话期间没法干别的"的直接原因。）
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
 * @brief  开始播放音频数据（**零拷贝**：直读调用方缓冲，不占 play_buf）
 * @param  ctx: 音频上下文指针
 * @param  data: 音频数据（16k/单声道/s16le）。**调用方必须保证它在整个播放
 *               期间持续有效**：从本函数返回 0 起，到完成回调被调用（或
 *               audio_play_stop() 返回）为止，不许 free / 不许覆写。
 *               典型用法：进程级常驻的 TTS 缓冲（hello_app 的 g_tts_buf、
 *               robot_ui 的 s_tts_buf）—— 下一次合成必然排在这一轮播放
 *               完成之后（状态机串行），天然满足契约。
 * @param  frames: 帧数（不受 AUDIO_PLAY_BUFFER_MS 限制 —— 数据不在 play_buf 里）
 * @param  callback: 播放完成回调（语义与 audio_play_start 相同：失败也回调）
 * @param  user_data: 回调用户数据
 * @return 0成功, 负值失败
 *
 * 和 audio_play_start 的区别只有"数据怎么进播放线程"：
 *   - audio_play_start：memcpy 进 play_buf 再播，data 随即可释放，但塞不下
 *     （> AUDIO_PLAY_BUFFER_MS）就 -ENOSPC；
 *   - 本函数：不拷贝、不淡入淡出（淡入淡出会改数据，调用方缓冲不许碰）、
 *     不受 play_buf 容量限制 —— 换来的义务是上面那条生命周期契约。
 *
 * 半双工、完成回调、play_last_result 等语义与 audio_play_start 完全一致。
 * 2026-10-19 加：TTS 全段（最长 2 MB ≈ 64 秒）走这条路，play_buf 才得以
 * 从 2.24 MB 砍到 512 KB（来龙去脉见 AUDIO_PLAY_BUFFER_MS 头上的说明）。
 */

int audio_play_start_ref(audio_context_t *ctx,
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
 * @brief  播放停滞看门狗：播放线程太久没往前走就强制收摊
 * @param  ctx: 音频上下文指针
 *
 * 上层（hello_app 的 main_loop_task）每 100ms 调一次，**与轮次无关** ——
 * 追问/报警提示音、灯控音这些单段播报压根不在"轮"里，轮次看门狗对它们
 * 是瞎的（2026-10-25 真机 80 秒不能对话就是这么来的）。
 *
 * 判据是"推进"不是"总时长"（见 audio_context_t::play_progress_ms）：
 * 正常播放每 250ms 左右盖一次章，连着 AUDIO_PLAY_STALL_MAX_MS 没盖就是卡死。
 *
 * 命中动作就是 audio_play_stop()：那条路已经把「解闩放行」做完了（换代 +
 * 置 play_stop + play_thread_valid 放回 false），下一轮起播能自己好，不用断电。
 *
 * 没在播 / 没卡住时是 no-op，可以随便调。
 */

void audio_play_watchdog_tick(audio_context_t *ctx);

/**
 * @brief  解除"整轮播报"的让路锁（ctx->record_hold），并把欠着的那次还麦补上
 * @param  ctx: 音频上下文指针
 *
 * 流水线 TTS 是"首句 + 尾段"两段播的，中间有个空档（尾段还在合成）。上层在
 * 整个播报轮期间把 ctx->record_hold 置真，让半双工契约里那句"播完还麦"一直
 * 欠着不动；轮子收尾时调本函数解锁，audio_resume_record() 随即把常开监听
 * 恢复起来。
 *
 * 幂等：没在 hold 时调它是 no-op。单段播放（追问 / 灯控 / 关怀）从来没置过
 * 锁，走的仍是 ai_audio.c 里播完自动还麦那条老路，本函数对它们不动作。
 */

void audio_record_hold_end(audio_context_t *ctx);

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
