/****************************************************************************
 * app/robot_ui/ambient_listen.c
 *
 * 常态听音 + 突发大音量检测（工作流 C）
 *
 * 目标（用户原话）：常态录音 + 音量检测，检测到突发的大音量就把那一段录音
 * 交给大模型识别。
 *
 * 实现在这里，分四块：
 *
 *   ① 听音线程（ambient_thread）：常态占着麦克风，每 20 ms 读一帧（320 采样
 *      = 640 字节），算帧平均幅度，写进 5 秒环形缓冲；
 *
 *   ② 判突发（trigger 条件，两个条件同时成立，连续 2 帧）：
 *        帧平均幅度 >= AMBIENT_LEVEL_FLOOR            （绝对地板）
 *        帧平均幅度 >= 慢基线 * AMBIENT_RISE_RATIO     （相对"突然"）
 *      理由和误触发分析见下面常量那段的注释；
 *
 *   ③ 取 3 秒片段（触发前 2 秒从环形缓冲拿 + 触发后 1 秒直接从设备读），
 *      让出麦克风，voice_asr_recognize() 识别，识别完（默认）再
 *      mimo_chat() 问一句"这是什么声音/屋里有没有异常"；
 *
 *   ④ 结果用回调交给父 agent（父 agent 在回调里走 ui_post_xxx 投递到 LVGL
 *      线程）。本模块**不碰任何 LVGL 控件** —— 它整体跑在听音线程里。
 *
 * 半双工（本板麦克风和喇叭不能同时用）：
 *   - 常态占着麦克风会让语音聊天的 audio_record_start() 拿到 -EBUSY，
 *     所以给了 ambient_listen_pause() / ambient_listen_hold() 让父 agent
 *     在"要用麦克风/要放音"的路径上按停；
 *   - 自己放出来的声音不能被当成异常，靠三道保险：
 *       (a) 触发处理期间本模块主动把麦克风关掉（ASR/对话/父 agent 播放
 *           TTS 的整段时间里设备是空的）；
 *       (b) 父 agent 在放音前调 ambient_listen_hold()；
 *       (c) 可选注册 ambient_listen_set_busy_cb()（父 agent 传
 *           audio_is_playing() 之类的判断），true 期间连麦克风都不开；
 *     再加恢复后的 AMBIENT_RESUME_GUARD_MS 静默窗（喇叭余音/开机首帧
 *     的直流冲击/ALC 收敛都在这段时间里，期间不判突发）。
 *
 * 内存（开关打开后）：
 *   环形缓冲 AMBIENT_RING_BYTES   160000 字节（5 秒，156.25 KiB）
 *   事件片段 AMBIENT_CLIP_BYTES    96000 字节（3 秒， 93.75 KiB）
 *   单帧     AMBIENT_FRAME_BYTES     640 字节
 *   线程栈   AMBIENT_THREAD_STACK  32768 字节（NuttX 的 pthread 栈也在堆上）
 *   合计约 283 KiB 堆，全在堆上（静态数组会把内核 SRAM 顶满，本项目踩过）。
 *   开关**默认关**，关着的时候一个字节都不分配、一个线程都没有。
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "ambient_listen.h"

#include "sf32lb52_audio_in.h"   /* 板级录音封装 audio_in_start/read/stop */
#include "voice/voice_asr.h"     /* voice_asr_recognize()（ai_agent 分发层） */
#include "mimo_voice.h"          /* mimo_chat()（同步 HTTPS 对话） */
#include "infra/config_store.h"  /* claw_config_get()，读开关 */

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 录音格式：和板级封装、整条语音链路（ASR / TTS）一致，固定 16k/单声道/s16le */

#define AMBIENT_SAMPLE_RATE       AUDIO_IN_DEFAULT_RATE    /* 16000 */
#define AMBIENT_CHANNELS          1
#define AMBIENT_BITS              AUDIO_IN_DEFAULT_BITS    /* 16 */
#define AMBIENT_BYTES_PER_SAMPLE  2                        /* s16le */
#define AMBIENT_FRAME_MS          20

#define AMBIENT_SAMPLES_PER_FRAME \
  (AMBIENT_SAMPLE_RATE * AMBIENT_FRAME_MS / 1000)          /* 320 采样 */
#define AMBIENT_FRAME_BYTES \
  (AMBIENT_SAMPLES_PER_FRAME * AMBIENT_BYTES_PER_SAMPLE)   /* 640 字节 */

/* 环形缓冲保留最近 5 秒：
 *   16000 采样/秒 × 2 字节 × 5 秒 = 160000 字节 = 156.25 KiB。
 * 5 秒是父 agent 要的口径（比"触发前 2 秒"宽裕），留着将来想改成"人工回看
 * 最近 5 秒"或者把前置窗口调大都不用重新算大小。
 * 内存紧的话把 AMBIENT_RING_MS 降到 3000（96000 字节）也够用 ——
 * 前置窗口只要 2 秒。 */

#define AMBIENT_RING_MS      5000
#define AMBIENT_RING_BYTES \
  (AMBIENT_SAMPLE_RATE * AMBIENT_BYTES_PER_SAMPLE * AMBIENT_RING_MS / 1000)

/* 一次事件取「触发前 2 秒 + 触发后 1 秒」= 3 秒 = 96000 字节 = 93.75 KiB。
 * 为什么是这个比例：突发音本身只有几百毫秒，"这是什么"要靠前后文 ——
 * 前面 2 秒是触发前的环境（模型/ASR 有时能从上下文里认出人声），
 * 后面 1 秒是撞击/碎裂的尾巴。再长只是白花一次云端的字节量。 */

#define AMBIENT_PRE_MS       2000
#define AMBIENT_POST_MS      1000
#define AMBIENT_PRE_BYTES \
  (AMBIENT_SAMPLE_RATE * AMBIENT_BYTES_PER_SAMPLE * AMBIENT_PRE_MS / 1000)
#define AMBIENT_POST_BYTES \
  (AMBIENT_SAMPLE_RATE * AMBIENT_BYTES_PER_SAMPLE * AMBIENT_POST_MS / 1000)
#define AMBIENT_CLIP_BYTES   (AMBIENT_PRE_BYTES + AMBIENT_POST_BYTES)

/* 太短的片段不值得发一次网络请求（和 main.c 的 VOICE_PCM_MIN_BYTES 同口径） */

#define AMBIENT_MIN_CLIP_BYTES (AMBIENT_SAMPLE_RATE * AMBIENT_BYTES_PER_SAMPLE / 2)

/* ---------- 检测参数：为什么这么选 ----------
 *
 * 用什么量：**帧平均幅度**（20 ms / 320 个采样的 |x| 平均值），0..32767。
 *   没选 RMS 开方、也没选峰值当主判据，理由：
 *     - 平均幅度和响度成正比、和 ASR/TTS 链路的能量口径一致，比峰值抗毛刺
 *       （单个采样点的直流冲击/毛刺能把峰值顶到满量程，却几乎不动平均值）；
 *     - 一帧只要 320 次比较+加法，裸机上几十微秒，20 ms 周期里可以忽略；
 *     - 峰值另外算一份，只用于日志和给父 agent 看（不参与判定）。
 *   代价：短于 20 ms 的单个脉冲（比如很轻的一下玻璃碎裂首波）可能落在
 *   一帧里被平均掉 —— 已知限制，见 .h 里的说明。
 *
 * AMBIENT_LEVEL_FLOOR = 2500（约 -21 dBFS，平均幅度口径）
 *   绝对地板。安静房间的本底一般在几十到一两百之间，正常说话几百到一千五，
 *   喊叫/撞击/摔倒/thud 这类稳定超过 2500。
 *   没有这个地板，"本底 30 + 一声 300 的吱呀" 也会满足 10 倍比 —— 数值上
 *   像突发，实际根本听不见。
 *
 * AMBIENT_RISE_RATIO = 4（约 +12 dB）
 *   相对条件：比最近约 1.3 秒的平均响度大 4 倍才算"突然"。
 *   没有这个条件，一个本来就吵的屋子（电视常开、本底 2000）会被绝对地板
 *   一直命中 —— 吵不是异常，"突然变吵"才是。
 *
 * AMBIENT_TRIGGER_FRAMES = 2（连续 40 ms）
 *   连续两帧都满足才算。单独一帧满足的，实测基本是开关灯的电流声、
 *   敲桌子、设备自身抖动这类不该上报的东西。
 *
 * 误触发风险（诚实说）：
 *   - 关门、敲桌子、东西掉地上，都会满足条件 —— 这是这一类判据的固有代价，
 *     所以后面接了大模型那一步：让模型看到"响声 + 识别出来的（多半没意义
 *     的）文本"再判断要不要提醒。**本模块不直接报警**，报警仍然走
 *     ai_sound_detect 那条路（那里才是队友训的模型）。
 *   - 喇叭自己放出来的声音：靠 .h 里的三道保险（处理期间关麦克风、
 *     父 agent hold、busy 回调）+ 恢复后的 300 ms 静默窗压住。
 *   - 现场标定：串口日志里每帧会打印 level 和慢基线，改上面两个常量即可，
 *     不用改逻辑。
 */

#define AMBIENT_LEVEL_FLOOR       2500
#define AMBIENT_RISE_RATIO        4
#define AMBIENT_TRIGGER_FRAMES    2

/* 慢基线 EMA：α = 1/64，每帧 20 ms → 时间常数 ≈ 64 × 20 ms = 1.28 秒。
 * 选它是因为：要比"最近 1 秒多"的响度，太慢跟不上环境变化（有人开了电视
 * 之后要几十秒才适应，这段时间一直误触发），太快则会把这次事件自己吸进
 * 基线里、越触发越不灵敏。 */

#define AMBIENT_BASELINE_DIV      64

/* 触发后 10 秒内不再触发：一次突发音通常伴随好几声（摔倒 = 撞击 + 后续
 * 碰倒的东西），防抖保证只处理一次；也顺带限制了云端请求的频率。 */

#define AMBIENT_DEBOUNCE_MS       10000

/* 重新拿到麦克风后的静默窗：喇叭余音、开机首帧的直流冲击、ALC 收敛
 * 都在这一段里，期间不判突发（基线照常喂，好让本底尽快收敛）。 */

#define AMBIENT_RESUME_GUARD_MS   300

/* 文本缓冲 */
#define AMBIENT_ASR_TEXT_MAX      512
#define AMBIENT_CHAT_TEXT_MAX     1024
#define AMBIENT_PROMPT_MAX        1200

/* 听音线程栈：ASR / 对话要跑 TLS + HTTPS，和 main.c 的语音工作线程一样给
 * 32 KB（默认的 16 KB 偏紧）。 */

#define AMBIENT_THREAD_STACK      32768

/* 让出麦克风期间的轮询间隔。不参与检测时线程只在睡，50 ms 一次几乎不耗 CPU，
 * 代价是 hold 之后最迟 50 ms 才真的放掉设备 —— 所以要"放下就能立刻录音"的
 * 路径用同步的 ambient_listen_pause()。 */

#define AMBIENT_IDLE_POLL_US      (50 * 1000)

/* 设备这一轮用不成时的退避：open 失败（-EBUSY = 别人正占着），或者刚开起来
 * 就读不出一整帧。两种情况都别刷日志、等一等再来；读失败那条如果不等，
 * 下一轮马上又 open + read，read 立刻报错时就是忙循环。 */

#define AMBIENT_RETRY_POLL_US     (200 * 1000)

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* 听音线程的生命周期 */

typedef enum
{
  AMB_STATE_IDLE = 0,         /* 没有听音线程，缓冲已释放 */
  AMB_STATE_RUNNING           /* 听音线程活着（可能正在让出麦克风） */
} amb_state_t;

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 一把锁保护下面这些跨线程状态。锁的临界区都很短，**只有**两处会稍微久一点：
 * 听音线程的 audio_in_start() 和 pause() 里的 audio_in_stop() —— 这样
 * "把设备开起来"和"把设备关掉"相对彼此是原子的，pause() 返回时设备一定已经
 * 关掉了（否则紧接着的 audio_record_start() 会拿到 -EBUSY）。 */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static amb_state_t     g_state = AMB_STATE_IDLE;
static bool            g_enabled;        /* 用户开关 */
static bool            g_quit;           /* 让听音线程退出 */
static uint32_t        g_hold_mask;      /* 让出麦克风的几路来源（位掩码） */
static bool            g_mic_open;       /* 设备已 START；只有听音线程会置 true */
static volatile bool   g_processing;     /* 正在跑一次触发的 ASR/对话 */

/* 堆缓冲：只在听音线程活着的时候存在，由听音线程自己在退出时释放 */
static uint8_t  *g_ring;                 /* 5 秒环形缓冲 */
static uint8_t  *g_clip;                 /* 3 秒事件片段 */
static int16_t  *g_frame;                /* 一帧 20 ms */

/* 回调（init 时登记，之后只读） */
static ambient_listen_cb_t g_cb;
static void               *g_cb_arg;
static ambient_busy_cb_t   g_busy_cb;
static void               *g_busy_arg;

/* 运行参数 / 统计 / 诊断量 */
static bool            g_chat_enabled = true;
static uint32_t        g_seq;
static uint32_t        g_last_trigger_ms;
static volatile int32_t g_noise_floor;   /* 当前慢基线（诊断用） */
static ambient_listen_stats_t g_stats;   /* 只由听音线程写，纯诊断 */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ambient_tick_ms
 *
 * Description:
 *   单调时钟毫秒（和项目里 ai_sound_detect.c / net_test.c 同一写法）。
 *   CLOCK_MONOTONIC 不会因为网络对时而跳变，适合做防抖/静默窗。
 ****************************************************************************/

static uint32_t ambient_tick_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/****************************************************************************
 * Name: ambient_frame_level
 *
 * Description:
 *   一帧的平均幅度（|x| 平均），0..32767。见文件头"检测参数"那段。
 ****************************************************************************/

static int ambient_frame_level(const int16_t *pcm, size_t samples)
{
  int32_t sum = 0;
  size_t  i;

  if (pcm == NULL || samples == 0)
    {
      return 0;
    }

  for (i = 0; i < samples; i++)
    {
      int32_t v = pcm[i];

      sum += (v < 0) ? -v : v;
    }

  return sum / (int32_t)samples;
}

/****************************************************************************
 * Name: ambient_frame_peak
 *
 * Description:
 *   一帧的峰值幅度。只用于日志和上报给父 agent 看，不参与判定。
 ****************************************************************************/

static int ambient_frame_peak(const int16_t *pcm, size_t samples)
{
  int32_t peak = 0;
  size_t  i;

  if (pcm == NULL || samples == 0)
    {
      return 0;
    }

  for (i = 0; i < samples; i++)
    {
      int32_t v = pcm[i];

      if (v < 0)
        {
          v = -v;
        }
      if (v > peak)
        {
          peak = v;
        }
    }

  return (int)peak;
}

/****************************************************************************
 * Name: ambient_ring_peek_tail
 *
 * Description:
 *   把环形缓冲里"最近 len 字节"（len <= 环形缓冲大小）拷到 dst，处理回绕。
 *   len 由调用方保证有效。只有一个写者（听音线程自己），所以不用加锁。
 ****************************************************************************/

static void ambient_ring_peek_tail(uint32_t wr, uint8_t *dst, size_t len)
{
  size_t start = (size_t)((wr + AMBIENT_RING_BYTES - len) % AMBIENT_RING_BYTES);
  size_t first = AMBIENT_RING_BYTES - start;

  if (first > len)
    {
      first = len;
    }

  memcpy(dst, g_ring + start, first);
  if (len > first)
    {
      memcpy(dst + first, g_ring, len - first);
    }
}

/****************************************************************************
 * Name: ambient_deliver
 *
 * Description:
 *   把一次事件交给父 agent 的回调。
 *   **绝不在持 g_lock 的时候调回调**：父 agent 的回调里很可能要调
 *   ambient_listen_hold() / pause()，持着锁叫回调就是自己锁死自己。
 *   text / reply 指向调用方的缓冲，回调返回后失效。
 ****************************************************************************/

static void ambient_deliver(ambient_event_t type, const char *text,
                            const char *reply, int err,
                            int level, int peak)
{
  ambient_listen_msg_t msg;
  ambient_listen_cb_t  cb;
  void                *arg;

  pthread_mutex_lock(&g_lock);
  cb = g_cb;
  arg = g_cb_arg;
  g_seq++;
  msg.seq = g_seq;
  pthread_mutex_unlock(&g_lock);

  if (cb == NULL)
    {
      return;
    }

  msg.type  = type;
  msg.text  = text;
  msg.reply = reply;
  msg.err   = err;
  msg.level = level;
  msg.peak  = peak;

  cb(&msg, arg);
}

/****************************************************************************
 * Name: ambient_process
 *
 * Description:
 *   一次触发的后半程：ASR ->（可选）对话 -> 回调。
 *   调到这里时麦克风**已经让出去了**（见 ambient_thread 里 trigger 那段），
 *   所以父 agent 在回调里想放 TTS、或者用户这时点开语音聊天都不冲突。
 *
 *   只从听音线程调。
 ****************************************************************************/

static void ambient_process(size_t clip_len, int level, int peak, int32_t base)
{
  char text[AMBIENT_ASR_TEXT_MAX]   = { 0 };
  char reply[AMBIENT_CHAT_TEXT_MAX] = { 0 };
  char prompt[AMBIENT_PROMPT_MAX];
  int  ratio;
  int  ret;
  bool chat;

  /* 先告诉父 agent "有动静了"：界面可以在这里先动一下（比如显示"我听到
   * 一声响，正在看看是什么"）。后面的云端往返要好几秒到几十秒。 */

  ambient_deliver(AMBIENT_EVENT_TRIGGER, NULL, NULL, 0, level, peak);

  printf("[Ambient] 送识别 %zu 字节 (~%u 秒)\n",
         clip_len, (unsigned int)(clip_len / (AMBIENT_SAMPLE_RATE * 2)));

  ret = voice_asr_recognize(g_clip, clip_len, text, sizeof(text));
  if (ret < 0)
    {
      printf("[Ambient] 识别失败: %d\n", ret);
      g_stats.errors++;
      ambient_deliver(AMBIENT_EVENT_ERROR, NULL, NULL, ret, level, peak);
      return;
    }

  printf("[Ambient] 识别结果: %s\n", (text[0] != '\0') ? text : "(空)");
  if (text[0] != '\0')
    {
      g_stats.asr_ok++;
    }

  pthread_mutex_lock(&g_lock);
  chat = g_chat_enabled;
  pthread_mutex_unlock(&g_lock);

  if (!chat)
    {
      ambient_deliver(AMBIENT_EVENT_RESULT, text, NULL, 0, level, peak);
      return;
    }

  /* 送进来的只是"识别出来的文字"，不是音频 —— 一声脆响 ASR 多半给出空串
   * 或者"嗯。"这类没意义的结果。所以提示词里把客观量（响度、比平时大多少）
   * 一起给模型，并且明确要它别编造、判断不出来就说判断不出来。 */

  ratio = (base > 0) ? (int)(level / base) : 0;
  if (ratio > 999)
    {
      ratio = 999;
    }

  snprintf(prompt, sizeof(prompt),
           "屋里这台设备一直开着听音。刚才听到一声突发的响声："
           "响度大概是它平时听着的 %d 倍（这一帧平均幅度 %d/32767，"
           "峰值 %d/32767），像是很短促的一下。"
           "我把这一小段做了语音识别，结果（这一小段很可能听不出人话，"
           "非常不可靠）是：「%s」。"
           "请判断这可能是什么声音、屋里有没有需要留意的情况，"
           "用一两句普通话讲给老人听。不要编造没听到的内容；"
           "判断不出来就直说只听到一声响、建议家人留意。",
           ratio, level, peak,
           (text[0] != '\0') ? text : "（没识别出人话）");

  printf("[Ambient] 问大模型…\n");
  ret = mimo_chat(prompt, reply, sizeof(reply));

  if (ret < 0 || reply[0] == '\0')
    {
      /* 识别文本已经拿到了，别把它一起丢掉：RESULT 里带上 text，
       * err 告诉父 agent 是哪一步没成（-ENOENT 没配密钥 / -EIO 网络）。 */
      printf("[Ambient] 大模型判断失败: %d\n", ret);
      g_stats.errors++;
      ambient_deliver(AMBIENT_EVENT_RESULT, text, NULL,
                      (ret < 0) ? ret : -EPROTO, level, peak);
      return;
    }

  g_stats.chat_ok++;
  ambient_deliver(AMBIENT_EVENT_RESULT, text, reply, 0, level, peak);
}

/****************************************************************************
 * Name: ambient_thread
 *
 * Description:
 *   听音线程。常态监听 + 判突发 + 取片段 + 处理，全在这一个线程里串行跑
 *   （同一时刻只可能有一次触发，天然不会重入；处理期间麦克风是关的，
 *   也不会再触发）。
 *
 *   跑在非 LVGL 线程里：任何界面动作都由父 agent 在回调里投递，本文件
 *   一行 LVGL 都不碰。
 ****************************************************************************/

static void *ambient_thread(void *arg)
{
  uint32_t wr = 0;             /* 环形缓冲写指针（字节） */
  size_t   filled = 0;         /* 环形缓冲里已有多少字节（≤ AMBIENT_RING_BYTES） */
  int32_t  base = 0;           /* 帧平均幅度的慢基线 */
  int      hit = 0;            /* 连续命中帧数 */
  uint32_t guard_until = 0;    /* 这个时刻之前不判突发（刚拿到麦克风的静默窗） */
  uint32_t diag_tick = 0;

  (void)arg;

  for (;;)
    {
      bool busy;
      uint32_t now;

      /* ---------- ① 该不该占着麦克风 ---------- */

      /* busy 判断放到锁外调（父 agent 传进来的实现可能要看它自己的状态，
       * 不该在我们的锁里跑） */
      busy = (g_busy_cb != NULL) ? g_busy_cb(g_busy_arg) : false;

      pthread_mutex_lock(&g_lock);

      if (g_quit)
        {
          pthread_mutex_unlock(&g_lock);
          break;                     /* 收尾在循环后面统一做 */
        }

      if (!g_enabled || g_hold_mask != 0 || busy)
        {
          if (g_mic_open)
            {
              /* 自己关设备（不是在阻塞的 read 里，直接关是安全的）。
               * 别人（pause）也可能刚替我们关过，audio_in_stop() 幂等。 */
              audio_in_stop();
              g_mic_open = false;
            }
          pthread_mutex_unlock(&g_lock);
          usleep(AMBIENT_IDLE_POLL_US);
          continue;
        }

      if (!g_mic_open)
        {
          /* 在锁里做 open/configure/START：这样 pause() 只要拿到锁，
           * 就一定是在"已经开起来"的状态之后去关它，不会漏关。 */
          int ret = audio_in_start(AMBIENT_SAMPLE_RATE, AMBIENT_CHANNELS,
                                   AMBIENT_BITS);

          if (ret != 0)
            {
              pthread_mutex_unlock(&g_lock);
              /* -EBUSY = 别人（语音聊天 / 别的 app）正占着麦克风。
               * 别的错误也不刷日志，退避重试即可。 */
              usleep(AMBIENT_RETRY_POLL_US);
              continue;
            }

          g_mic_open = true;

          /* 一段新的连续音频：环形缓冲、基线、命中计数全部重来 */
          wr = 0;
          filled = 0;
          base = 0;
          hit = 0;
          guard_until = ambient_tick_ms() + AMBIENT_RESUME_GUARD_MS;
        }

      pthread_mutex_unlock(&g_lock);

      /* ---------- ② 读一帧（20 ms） ---------- */

      {
        ssize_t nbytes = audio_in_read(g_frame, AMBIENT_FRAME_BYTES);

        /* 必须恰好一帧：0 = 被 AUDIOIOC_STOP 打断（pause / 关机路径）
         * 或下层 5 秒超时；负值 = 设备已经被收掉；短读理论上不会出现。
         * 三种都当"这一段音频不连续"，跳出去重新开始 ——
         * 绝不能当成"这次没数据、再读一次就有"（那样是死循环）。 */
        if (nbytes != (ssize_t)AMBIENT_FRAME_BYTES)
          {
            bool quit;

            pthread_mutex_lock(&g_lock);
            if (g_mic_open)
              {
                audio_in_stop();
                g_mic_open = false;
              }
            quit = g_quit;
            pthread_mutex_unlock(&g_lock);

            /* 刚 open 出来就读不出一整帧，多半是设备/下层有毛病（不是被
             * 让路那种正常情况）。重开之前先退避：read 如果是一直"立刻
             * 报错"，不等就是每轮白跑一次 open/stop 的忙循环。
             * 只有真出过异常才走这里，正常一帧一帧读的时候一次都不睡。
             * 已经收到停止请求就别再等这 200 ms 了，直接回顶部退出。 */
            if (!quit)
              {
                usleep(AMBIENT_RETRY_POLL_US);
              }
            continue;
          }
      }

      /* ---------- ③ 一帧的指标 + 落环形缓冲 ---------- */

      now = ambient_tick_ms();

      {
        int level = ambient_frame_level(g_frame, AMBIENT_SAMPLES_PER_FRAME);
        int peak  = ambient_frame_peak(g_frame, AMBIENT_SAMPLES_PER_FRAME);
        int lvl   = (base > 0) ? (int)base : AMBIENT_LEVEL_FLOOR;
        bool guard;
        bool debounce;
        bool loud;

        g_stats.frames++;

        /* 先把这一帧放进环形缓冲，触发时"触发前 2 秒"才包含这一帧 */
        memcpy(g_ring + wr, g_frame, AMBIENT_FRAME_BYTES);
        wr = (uint32_t)((wr + AMBIENT_FRAME_BYTES) % AMBIENT_RING_BYTES);
        if (filled < AMBIENT_RING_BYTES)
          {
            filled += AMBIENT_FRAME_BYTES;
          }

        /* 判定用的是**更新基线之前**的基线 —— 否则这一帧自己就把基线抬起来，
         * 越是突发越判不出来。 */
        guard    = (now < guard_until);
        debounce = ((uint32_t)(now - g_last_trigger_ms) < AMBIENT_DEBOUNCE_MS);
        loud     = (level >= AMBIENT_LEVEL_FLOOR &&
                    level >= lvl * AMBIENT_RISE_RATIO);

        if (guard || debounce || !loud)
          {
            hit = 0;
          }
        else if (++hit >= AMBIENT_TRIGGER_FRAMES)
          {
            hit = 0;
            g_last_trigger_ms = now;
            g_stats.triggers++;

            printf("[Ambient] 触发：level=%d peak=%d 基线=%d 倍率=%d\n",
                   level, peak, (int)base,
                   (base > 0) ? (int)(level / base) : 0);

            /* ---------- ④ 取 3 秒片段 ---------- */
            /* (a) 触发前 2 秒：环形缓冲里最近 2 秒。刚开机环形缓冲还没攒满
             *     时前面补 0，保证片段和触发点的时间对齐（不是左对齐）。 */

            {
              size_t have = (filled < AMBIENT_PRE_BYTES) ? filled
                                                         : AMBIENT_PRE_BYTES;
              size_t got = 0;

              memset(g_clip, 0, AMBIENT_PRE_BYTES);
              ambient_ring_peek_tail(wr, g_clip + (AMBIENT_PRE_BYTES - have),
                                     have);

              /* (b) 触发后 1 秒：直接从设备读，边读边追加（不进环形缓冲，
               *     免得这段尾巴把环形缓冲里还有用的历史挤掉）。 */
              while (got < AMBIENT_POST_BYTES)
                {
                  ssize_t n = audio_in_read(g_clip + AMBIENT_PRE_BYTES + got,
                                            AMBIENT_FRAME_BYTES);

                  if (n != (ssize_t)AMBIENT_FRAME_BYTES)
                    {
                      break;    /* 被 pause 抢了设备：用已经拿到的部分 */
                    }
                  got += AMBIENT_FRAME_BYTES;
                }

              /* ---------- ⑤ 让出麦克风 ---------- */
              /* 后面是云端 ASR + 对话（几十秒到一分钟），没有理由占着它；
               * 也必须让出来，父 agent 才可能在这期间放 TTS / 开语音聊天。 */

              pthread_mutex_lock(&g_lock);
              if (g_mic_open)
                {
                  audio_in_stop();
                  g_mic_open = false;
                }
              g_processing = true;
              pthread_mutex_unlock(&g_lock);

              if (AMBIENT_PRE_BYTES + got >= AMBIENT_MIN_CLIP_BYTES)
                {
                  ambient_process(AMBIENT_PRE_BYTES + got, level, peak, base);
                }
              else
                {
                  printf("[Ambient] 片段太短 (%zu 字节)，这一轮丢掉\n",
                         AMBIENT_PRE_BYTES + got);
                }

              g_processing = false;
            }

            /* 这一小段时间里麦克风是关的，音频不连续：
             * 环形缓冲/基线/命中计数全部重来（下一轮循环会重新开麦克风）。 */
            wr = 0;
            filled = 0;
            base = 0;
            hit = 0;
            continue;
          }

        /* 基线更新（放在判定之后）。只在 guard 之外、且不处于防抖窗口时
         * 才允许基线"慢慢向上爬"，避免把这次事件自己吸进基线。 */
        if (base == 0)
          {
            base = level;        /* 开机第一帧就锚定，别从 0 慢慢爬 */
          }
        else
          {
            base += (level - base) / AMBIENT_BASELINE_DIV;
          }

        g_noise_floor = base;

        /* 每 ~10 秒打一行诊断日志：现场调门槛、也方便事后判断是"本底太高"
         * 还是"门槛太低"。这一行是整条常态通路上唯一的周期性输出，
         * 嫌吵把它删掉就行（判定逻辑一行都不用动）。 */
        if (++diag_tick >= (10 * 1000 / AMBIENT_FRAME_MS))
          {
            diag_tick = 0;
            printf("[Ambient] level=%d peak=%d 基线=%d 帧=%lu\n",
                   level, peak, (int)base,
                   (unsigned long)g_stats.frames);
          }
      }
    }

  /* 收尾：还设备 + 释放缓冲 + 让 enable() 可以再次开起来。
   * 缓冲是听音线程自己分配、自己释放的，所以不存在"一边 free 一边还在用"。 */

  pthread_mutex_lock(&g_lock);
  if (g_mic_open)
    {
      audio_in_stop();
      g_mic_open = false;
    }
  free(g_ring);   g_ring  = NULL;
  free(g_clip);   g_clip  = NULL;
  free(g_frame);  g_frame = NULL;
  g_processing = false;
  g_state = AMB_STATE_IDLE;
  pthread_mutex_unlock(&g_lock);

  printf("[Ambient] 听音线程已退出，缓冲已释放\n");
  return NULL;
}

/****************************************************************************
 * Name: ambient_config_bool
 *
 * Description:
 *   读一个布尔配置键。没配 / 读不到就用 def。
 *   口径和 mimo_voice.c 一致：「0 / false / off / no」算关，别的值算开。
 ****************************************************************************/

static bool ambient_config_bool(const char *key, bool def)
{
  char val[16];

  if (claw_config_get(key, val, sizeof(val)) != OK || val[0] == '\0')
    {
      return def;
    }

  if (strcmp(val, "0") == 0 || strcmp(val, "false") == 0 ||
      strcmp(val, "off") == 0 || strcmp(val, "no") == 0)
    {
      return false;
    }

  return true;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ambient_listen_init(ambient_listen_cb_t cb, void *user_data)
{
  bool on;
  bool chat;

  if (cb == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_lock);
  g_cb     = cb;
  g_cb_arg = user_data;
  pthread_mutex_unlock(&g_lock);

  /* 开关默认**关**：这个功能要长期占着麦克风（半双工，语音聊天就没法用了），
   * 还要常驻约 250 KiB 堆 —— 必须由用户明确打开。
   * 配置键（和 llm_host 同一份 /data/ai_agent/config/config.json）：
   *   enable_ambient_listen —— 总开关，不配 = 关
   *   enable_ambient_chat   —— 识别出内容后是否再问大模型，不配 = 开 */

  on   = ambient_config_bool("enable_ambient_listen", false);
  chat = ambient_config_bool("enable_ambient_chat", true);

  g_chat_enabled = chat;
  printf("[Ambient] 常态听音: %s（大模型判断: %s）\n",
         on ? "开" : "关", chat ? "开" : "关");

  if (!on)
    {
      return OK;
    }

  return ambient_listen_enable(true);
}

int ambient_listen_enable(bool enable)
{
  pthread_attr_t attr;
  pthread_t      tid;
  int            ret;

  if (!enable)
    {
      /* 异步停止：不 join。听音线程如果正卡在一次云端 ASR/对话里，可能还要
       * 几十秒，在这里等会把调用者（很可能是 LVGL 线程）卡住。所以只喊停 +
       * 立刻把设备还回去，线程跑完手上这一次再自己退出、自己释放缓冲。
       * 在那之前再 enable(true) 会返回 -EBUSY。 */

      pthread_mutex_lock(&g_lock);
      if (!g_enabled && g_state == AMB_STATE_IDLE)
        {
          pthread_mutex_unlock(&g_lock);
          return OK;
        }
      g_enabled = false;
      g_quit    = true;
      if (g_mic_open)
        {
          audio_in_stop();          /* 会唤醒阻塞中的 audio_in_read() */
          g_mic_open = false;
        }
      pthread_mutex_unlock(&g_lock);

      printf("[Ambient] 已请求停止听音（线程会在手上那次网络请求结束后退出）\n");
      return OK;
    }

  pthread_mutex_lock(&g_lock);

  if (g_state != AMB_STATE_IDLE)
    {
      bool already = g_enabled;

      pthread_mutex_unlock(&g_lock);

      /* 已经开着就是幂等的成功；正在退出（上一次网络请求还没跑完）
       * 则明确告诉调用方稍后再来，不要偷偷起第二个线程抢麦克风。 */
      if (already)
        {
          return OK;
        }
      printf("[Ambient] 上一次还没退干净，稍后再开\n");
      return -EBUSY;
    }

  g_ring  = malloc(AMBIENT_RING_BYTES);
  g_clip  = malloc(AMBIENT_CLIP_BYTES);
  g_frame = malloc(AMBIENT_FRAME_BYTES);

  if (g_ring == NULL || g_clip == NULL || g_frame == NULL)
    {
      free(g_ring);   g_ring  = NULL;
      free(g_clip);   g_clip  = NULL;
      free(g_frame);  g_frame = NULL;
      pthread_mutex_unlock(&g_lock);
      printf("[Ambient] 缓冲分配失败（需要约 %d KiB）\n",
             (int)((AMBIENT_RING_BYTES + AMBIENT_CLIP_BYTES + AMBIENT_FRAME_BYTES)
                   / 1024));
      return -ENOMEM;
    }

  g_enabled   = true;
  g_quit      = false;
  g_hold_mask = 0;          /* 新开一轮：之前残留的 hold 全部清掉 */
  g_processing = false;
  g_seq       = 0;
  g_state     = AMB_STATE_RUNNING;

  pthread_mutex_unlock(&g_lock);

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, AMBIENT_THREAD_STACK);
  /* detach：用户随时可能把开关关掉，没人会去 join 它；线程退出时自己释放
   * 缓冲并把 g_state 置回 IDLE。 */
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  ret = pthread_create(&tid, &attr, ambient_thread, NULL);
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      pthread_mutex_lock(&g_lock);
      g_state   = AMB_STATE_IDLE;
      g_enabled = false;
      free(g_ring);   g_ring  = NULL;
      free(g_clip);   g_clip  = NULL;
      free(g_frame);  g_frame = NULL;
      pthread_mutex_unlock(&g_lock);
      printf("[Ambient] 听音线程创建失败: %d\n", ret);
      return -ret;
    }

  printf("[Ambient] 常态听音已开：环形缓冲 %d KiB(5 秒) + 片段 %d KiB(3 秒)"
         " + 线程栈 %d KiB\n",
         (int)(AMBIENT_RING_BYTES / 1024), (int)(AMBIENT_CLIP_BYTES / 1024),
         (int)(AMBIENT_THREAD_STACK / 1024));
  return OK;
}

bool ambient_listen_is_enabled(void)
{
  bool on;

  pthread_mutex_lock(&g_lock);
  on = g_enabled;
  pthread_mutex_unlock(&g_lock);

  return on;
}

bool ambient_listen_is_paused(void)
{
  bool paused;

  pthread_mutex_lock(&g_lock);
  paused = (g_hold_mask != 0);
  pthread_mutex_unlock(&g_lock);

  return paused;
}

bool ambient_listen_is_busy(void)
{
  return g_processing;
}

void ambient_listen_hold(ambient_hold_t who, bool on)
{
  uint32_t bit;

  if ((int)who < 0 || (int)who >= AMBIENT_HOLD_COUNT)
    {
      return;
    }

  bit = (uint32_t)1 << (int)who;

  pthread_mutex_lock(&g_lock);
  if (on)
    {
      g_hold_mask |= bit;
    }
  else
    {
      g_hold_mask &= ~bit;
    }
  pthread_mutex_unlock(&g_lock);
}

void ambient_listen_pause(void)
{
  bool need_stop;

  /* 同步让路：在锁里就把设备关掉再返回，这样紧接着的 audio_record_start()
   * 一定不会拿到 -EBUSY。
   *
   * 为什么不会漏关：听音线程的 audio_in_start() 也在同一把锁里做，所以
   *   - 线程先拿到锁：它开完设备、g_mic_open = true 才放锁，这里随后看到
   *     g_mic_open 是 true，关掉；
   *   - 这里先拿到锁：先置 hold，线程之后拿到锁时看到 hold 非 0，根本不会开。
   * 两种顺序都安全。 */
  pthread_mutex_lock(&g_lock);
  g_hold_mask |= (uint32_t)1 << AMBIENT_HOLD_MANUAL;
  need_stop = g_mic_open;
  if (need_stop)
    {
      /* 会唤醒可能正阻塞在 audio_in_read() 里的听音线程（read 返回 0） */
      audio_in_stop();
      g_mic_open = false;
    }
  pthread_mutex_unlock(&g_lock);
}

void ambient_listen_resume(void)
{
  pthread_mutex_lock(&g_lock);
  g_hold_mask &= ~((uint32_t)1 << AMBIENT_HOLD_MANUAL);
  pthread_mutex_unlock(&g_lock);
}

void ambient_listen_set_busy_cb(ambient_busy_cb_t fn, void *user_data)
{
  pthread_mutex_lock(&g_lock);
  g_busy_cb  = fn;
  g_busy_arg = user_data;
  pthread_mutex_unlock(&g_lock);
}

void ambient_listen_set_chat_enabled(bool enable)
{
  pthread_mutex_lock(&g_lock);
  g_chat_enabled = enable;
  pthread_mutex_unlock(&g_lock);
}

bool ambient_listen_chat_enabled(void)
{
  bool on;

  pthread_mutex_lock(&g_lock);
  on = g_chat_enabled;
  pthread_mutex_unlock(&g_lock);

  return on;
}

void ambient_listen_get_stats(ambient_listen_stats_t *stats)
{
  if (stats == NULL)
    {
      return;
    }

  pthread_mutex_lock(&g_lock);
  *stats          = g_stats;
  stats->noise_floor = g_noise_floor;
  stats->holds       = g_hold_mask;
  pthread_mutex_unlock(&g_lock);
}
