/****************************************************************************
 * app/hello_app/voice_emerg.c
 *
 * 语音急救报警的实现。设计取舍、为什么响铃不走 sf32lb52_alarm.c、
 * 为什么 Bark 要另起线程，全写在 voice_emerg.h 的文件头注释里 ——
 * **先看那个**，这里的注释只讲"这一段怎么做的"。
 *
 * 状态机：
 *
 *   IDLE ──trigger()──> CONFIRM（响-停-听循环）
 *                          ├─ 说「没事」      → CANCELLED，停铃，**不发** Bark
 *                          ├─ 10 秒没人吭声   → NOTIFIED，发 Bark，继续响
 *                          │                     └─ 每 30 秒再发一次
 *                          │                     └─ 期间说「没事」仍 → CANCELLED
 *                          └─ 60 秒到点        → EXPIRED，自动停
 *
 * 「响铃不能中断」怎么保证：铃是这条线程自己一轮一轮重新起播的，
 * 别人把播放掐了（audio_play_stop / 换代）只会让这一轮提前结束，下一轮
 * 立刻又起。唯一的例外是 g_cancel —— 那只有「没事」和到点会置。
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <nuttx/clock.h>

#include "ai_audio.h"
#include "network_comm.h"
#include "sf32lb52_alarm.h"
#include "voice_emerg.h"

/****************************************************************************
 * 铃声
 ****************************************************************************/

#define VE_SAMPLE_RATE    16000
#define VE_TONE_AMP       0.25        /* 约 -12dBFS，和 sf32lb52_alarm.c 一致 */
#define VE_TONE_MS        200         /* 单声时长 */
#define VE_HIGH_FREQ      1200        /* 沿用 EMERGENCY 那对高低音，老人听熟 */
#define VE_LOW_FREQ       700

#define VE_RING_SAMPLES   (VE_SAMPLE_RATE * VOICE_EMERG_RING_MS / 1000)

/* ★ 必须是**文件级 static**：大局部数组会被编译进函数序言，直接 hardfault
 *   （本项目踩过）。 */

static int16_t g_ring_pcm[VE_RING_SAMPLES];

/* 确认窗口内临时把 VAD 的两道门都压到 200 ms。
 *
 * ① 最短语音门（AUDIO_VAD_MIN_SPEECH_MS 现在 500 ms）：偷听窗只有 600 ms，
 *    「没事」两个字根本攒不满 500 ms 的能量帧，VAD 一次都起不了判，那就永远
 *    收不到。降到 200 ms 才有机会。
 *
 * ② 静音判停门（AUDIO_VAD_SILENCE_TIMEOUT_MS 现在 1000 ms）—— **2026-10-31
 *    补的这条才是真正的杀手**，之前只压了①，判据⑥因此 0 次都没过：
 *
 *    「没事」说完之后 VAD 还要再攒 1000 ms **实际收到的**静音才判"这一段说完了"，
 *    而这 1000 ms 里麦克风有三分之二的时间是聋的（半双工，铃响 1.2 s 里收不到
 *    一帧，静音计数器冻结不走）。攒满 1000 ms 实际静音 ≈ 要跨 3 个偷听窗 ≈
 *    3.8 秒，再加一段 ASR 往返，**早超过 10 秒确认窗**了 —— Bark 先发出去了，
 *    「没事」才姗姗来迟。所以这道门也得压。
 *
 * 代价是更早判"说完"，可能把老人的句中停顿切开 —— 但这里只等三个字面
 * （「没事」「我没事」「不用了」），本身就短，且**只在报警的 60 秒里生效**，
 * 出去就还回去，不影响正常对话。
 *
 * ⚠ 验收判据写死：**10 次里 ≥ 8 次能被「没事」停掉**，达不到不算做完。 */

#define VE_VAD_MIN_MS     200
#define VE_VAD_SIL_MS     200

/****************************************************************************
 * 私有状态
 ****************************************************************************/

static audio_context_t *g_ctx;
static volatile int     g_phase  = VOICE_EMERG_IDLE;
static volatile bool    g_cancel = false;

static sem_t            g_kick;          /* 触发 → 工作线程 */
static sem_t            g_play_done;     /* 一轮铃播完 */
static pthread_mutex_t  g_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_t        g_worker_tid;
static bool             g_inited = false;

static volatile int     g_push_inflight = 0;

/* 触发原因/原话，给 Bark 的正文用 */
static char             g_reason[32];
static char             g_text[96];

/* 辅助模式：触发时板级报警（sf32lb52_alarm.c）**已经**在响了。
 *
 * 面板那两个按钮（大圆紧急呼叫 / 菜单报警）今天已经走的是"报警页 + 响铃 +
 * MQTT + Bark"一整套（robot_ui 的 alarm_escalate / report_alarm）。这时候再
 * 让本模块起一套铃、再发一次 Bark，就是**双响双推**：两套铃抢同一台半双工
 * 设备，家人手机收两条一样的报警。
 *
 * 所以辅助模式下本模块**不响铃、不发 Bark**（那两件事板级那条路已经在做），
 * 只做一件它能补上的事：**盯「没事」**。老人误触了那个大红按钮，说一声
 * 「没事」就能把铃掐掉 —— 这在今天是做不到的，铃会一直响到 60 秒自停。
 *
 * 「响铃不能中断」在这里依然成立：只有「没事」和到点能停。 */

static bool             g_assist = false;

/* 一轮循环的三个时刻（在工作线程里写，在 ve_on_tick 里读） */
static uint32_t         g_confirm_end;
static uint32_t         g_stop_at;
static uint32_t         g_renotify_at;
static bool             g_notified;

/* 进确认窗口前的 VAD 两道门，退出时要还回去 */
static uint32_t         g_saved_vad_min;
static uint32_t         g_saved_vad_sil;
static bool             g_vad_saved;

/* 本轮是第几个 响/听 循环（诊断日志用） */
static volatile uint32_t g_cycle;

/* ★ 统一报警路径（2026-10-31）：屏幕要跟着状态走，但界面在 LVGL 线程、
 * 本模块在工作线程，不能直接碰控件 —— 只回调一个函数，投递由调用方做。 */

static voice_emerg_hook_t g_hook;
static void              *g_hook_arg;

/* 触发时刻，日志里报"距触发多少毫秒"，好和串口时间轴对上 */
static uint32_t         g_started_at;

/****************************************************************************
 * 前向声明（下面的定义顺序是按"读起来顺"排的，不是按依赖排的）
 ****************************************************************************/

static void ve_play_done_cb(void *user_data);
static int  ve_on_tick(uint32_t now);
static void ve_vad_lower(void);
static void ve_vad_restore(void);

/****************************************************************************
 * 小工具
 ****************************************************************************/

static uint32_t ve_now_ms(void)
{
  return (uint32_t)TICK2MSEC(clock_systime_ticks());
}

/* 把信号量里积着的计数清干净（上一轮可能播完 post 了还没人取） */

static void ve_drain(sem_t *s)
{
  while (sem_trywait(s) == 0)
    {
    }
}

/* 状态变了，叫屏幕跟着改。**在工作线程里跑**，所以回调那边只许投递。
 * 回调可能是 NULL（没接界面，比如 host 测试）。 */

static void ve_fire_hook(enum voice_emerg_phase_e phase)
{
  voice_emerg_hook_t cb = g_hook;

  if (cb != NULL)
    {
      cb(phase, g_reason, g_hook_arg);
    }
}

/****************************************************************************
 * 铃声生成
 *
 * 相位**连续累加**，不在换声那一刻重置 —— 否则高低声交界处会有"咔"。
 * 和 sf32lb52_alarm.c 的 alarm_tone_freq() 同一个道理。
 ****************************************************************************/

static void ve_build_tone(void)
{
  const int half = VE_SAMPLE_RATE * VE_TONE_MS / 1000;
  double    ph   = 0.0;
  int       i;

  for (i = 0; i < VE_RING_SAMPLES; i++)
    {
      int seg = i / half;
      int f   = (seg & 1) ? VE_LOW_FREQ : VE_HIGH_FREQ;

      ph += 2.0 * M_PI * (double)f / (double)VE_SAMPLE_RATE;
      if (ph > 2.0 * M_PI)
        {
          ph -= 2.0 * M_PI;
        }

      g_ring_pcm[i] = (int16_t)(32767.0 * VE_TONE_AMP * sin(ph));
    }
}

/****************************************************************************
 * Bark 推送（单独线程，别把铃卡住）
 *
 * push_send_alarm() 是一次 HTTPS 往返，本板实测约 5 秒。同步调的话这 5 秒
 * 铃就不响了 —— 直接违反"响铃不能中断"。所以丢一个 detached 线程去发。
 *
 * 同一时刻只许一个在飞（g_push_inflight）：30 秒才发一次，正常不会叠；
 * 真叠上了就跳过这一发 —— 家人少收一条比堆一串超时任务强。
 ****************************************************************************/

struct ve_push_arg
{
  char body[160];
};

static void *ve_push_task(void *arg)
{
  struct ve_push_arg *a = (struct ve_push_arg *)arg;

  printf("[急救] Bark 通知家人…\n");

  if (push_send_alarm("老人紧急呼救", a->body) < 0)
    {
      /* 失败原因常见两种：push_key.txt 不在 / 网络不通。
       * 都不影响铃 —— 铃照响，30 秒后还会再试一次。 */

      printf("[急救] Bark 没发出去（key 缺失或网络不通）—— 铃照响，30 秒后重试\n");
    }
  else
    {
      printf("[急救] Bark 已发\n");
    }

  free(a);
  g_push_inflight--;
  return NULL;
}

static void ve_push_async(void)
{
  struct ve_push_arg *a;
  pthread_attr_t      attr;
  pthread_t           tid;

  if (g_push_inflight > 0)
    {
      printf("[急救] 上一条还没发完，这一条跳过\n");
      return;
    }

  a = (struct ve_push_arg *)calloc(1, sizeof(*a));
  if (a == NULL)
    {
      return;
    }

  if (g_text[0] != '\0')
    {
      snprintf(a->body, sizeof(a->body), "老人说：%s（请立即查看）", g_text);
    }
  else
    {
      snprintf(a->body, sizeof(a->body), "老人%s（请立即查看）", g_reason);
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 8192);   /* 要走 TLS，栈给足 */
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  g_push_inflight++;

  if (pthread_create(&tid, &attr, ve_push_task, a) != 0)
    {
      g_push_inflight--;
      free(a);
      printf("[急救] 推送线程起不来\n");
    }

  pthread_attr_destroy(&attr);
}

/****************************************************************************
 * 阶段推进
 *
 * 每 20ms 被 ve_listen_gap() 调一次。放在偷听窗里检查才准 ——
 * 放在轮末会差出一整轮（1.8 秒），Bark 就变成 11.8 秒才发。
 *
 * 返回 0 = 继续响；非 0 = 到点收摊，外层该退出循环。
 ****************************************************************************/

static int ve_on_tick(uint32_t now)
{
  if (!g_notified && now >= g_confirm_end)
    {
      g_notified = true;
      g_phase    = VOICE_EMERG_NOTIFIED;

      if (g_assist)
        {
          /* 辅助模式：板级那条路在按下的那一刻就已经推过了，
           * 这里**不再推** —— 否则家人手机收两条一样的报警。 */
          printf("[急救] %d 秒确认期过（辅助模式，不重复推送）\n",
                 VOICE_EMERG_CONFIRM_MS / 1000);
        }
      else
        {
          printf("[急救] %d 秒没人说没事 -> 通知家人，铃继续响\n",
                 VOICE_EMERG_CONFIRM_MS / 1000);
          ve_push_async();
        }

      g_renotify_at = now + VOICE_EMERG_RENOTIFY_MS;

      /* 屏幕跟着改口：「10 秒内说没事」->「已通知家人，30 秒后重发」 */
      ve_fire_hook(VOICE_EMERG_NOTIFIED);
    }
  else if (g_notified && now >= g_renotify_at)
    {
      if (g_assist)
        {
          /* 同上：不重复推 */
        }
      else
        {
          printf("[急救] 再发一次通知\n");
          ve_push_async();
        }

      g_renotify_at = now + VOICE_EMERG_RENOTIFY_MS;
    }

  if (now >= g_stop_at)
    {
      g_phase = VOICE_EMERG_EXPIRED;
      printf("[急救] %d 秒到点，自动收摊\n", VOICE_EMERG_AUTOSTOP_MS / 1000);
      return 1;
    }

  return 0;
}

/****************************************************************************
 * 等待小工具
 ****************************************************************************/

/* 等一轮铃播完。每 20ms 看一眼 g_cancel，好让「没事」能立刻停。
 * 返回 true = 真播完了，false = 被取消 / 超时。 */

static bool ve_wait_play_done(uint32_t timeout_ms)
{
  uint32_t start = ve_now_ms();

  while ((ve_now_ms() - start) < timeout_ms)
    {
      if (sem_trywait(&g_play_done) == 0)
        {
          return true;
        }
      if (g_cancel)
        {
          return false;
        }
      usleep(20000);
    }

  return false;
}

/* 把音频设备从「AI 聊天」手里抢回来给报警铃 + 给耳朵。
 *
 * ★ 2026-10-31 真机定案（这一批两个 P0 是**同一个根因**）：
 *   报警触发的那一刻，AI 那一轮的 TTS / 提示音**正在播**。本板半双工只有一台
 *   /dev/audio/audio0，于是两件事同时发生：
 *
 *     ① audio_play_start() 一看 ctx->playing 为真就 `return -EBUSY`，
 *        根本走不到 audio_play_begin()，更没碰 play_last_result ——
 *        而那是**上一段 TTS** 留下的 -EINPROGRESS(-115)。
 *        于是串口打的是「铃声起播失败（-115）」，看着像铃自己坏了，
 *        真相是被抢了。这一响 1.2 秒是静音的。
 *     ② AI 整轮播报举着 ctx->record_hold，audio_resume_record() 见它就早退
 *        （"整轮播报未结束，暂不还麦"），麦克风整个 10 秒确认窗是聋的 ——
 *        老人说「没事」没人听见。
 *
 *   实测三次起播失败，三次都撞在 AI 播放上：TTS「现在是晚上七点三十八分」、
 *   提示音「嗯，我在」、live TTS「我这边有点问题」。
 *
 * ★★ 但**只能用 audio_play_stop() 这一条路**（2026-10-31 当天就栽在别的路上）：
 *   一开始这里还调了 audio_record_hold_end()，想顺手把让路锁一起解了 ——
 *   结果**报警一响整机直接卡死**。机理是它内部那句
 *
 *       if (!ctx->playing && ctx->play_fd_running) audio_drop_play_device(ctx);
 *
 *   判据是 `!playing` 而不是 `play_exited`：播放线程「还没写完但 playing 已经
 *   清了」（尤其卡在 STOP/close 那一段，真机日志里出现过 `卡在【STOP/close】
 *   阶段 3`）时，这里就**跨线程 close 它正占着的 fd** —— 踩 fd。
 *   ai_audio.c 里对这颗雷的原话是「本项目被这条咬过三次」，而报警是每 1.8 秒
 *   一个循环、每轮都踩一次，把日志里那个"半楔死"直接推成"彻底卡死"：
 *
 *       audio: STOP 没停到硬件（status=5 nstate=5）
 *       audio:   会话[0] #81 priv->state=5  ★就是它在挡
 *
 *   audio_play_stop() 是**唯一**安全的收法：它自己按 play_exited 判完才动 fd，
 *   而且开头就把 ctx->record_hold 清了 —— 我们要的两件事它一件不落。
 *   麦克风由**铃那条播放线程自己的尾巴**（audio_resume_record）还回来，
 *   那是半双工契约里本来就在的路，不用在这里代劳。
 *
 * 不持 g_lock 调（audio_play_stop 里有有界 join，可能睡几十毫秒）。 */

static void ve_seize_audio(const char *why)
{
  if (g_ctx == NULL)
    {
      return;
    }

  /* ★ 辅助模式下一个指头都不动（沿用上面 ve_ring_once 的「不插一脚」不变量）：
   *   那时喇叭是板级报警（sf32lb52_alarm.c）用**它自己的 fd** 在响，而驱动那边
   *   priv 是单实例 —— 这里发 AUDIOIOC_STOP 一下就把人家的铃掐了。
   *   辅助模式只负责盯「没事」，设备归板级报警。 */

  if (g_assist)
    {
      return;
    }

  if (audio_is_playing(g_ctx))
    {
      printf("[急救] 抢回音频设备（%s）：掐掉正在播的那一段，铃不许被抢\n", why);
    }

  /* 没在播的时候照样调：它开头那句 ctx->record_hold = false 正是把 AI 那一轮
   * 的让路锁冲掉（"谁要开麦谁就把它冲掉"的第二条兜底），而 fd 那几下只在
   * play_exited 时才动，安全。 */

  audio_play_stop(g_ctx);
}

/* 偷听窗。同时也是 deadline 的检查点。
 * 返回 0 = 时间到，继续；非 0 = 到点收摊，外层必须退出循环。 */

static int ve_listen_gap(uint32_t ms)
{
  uint32_t start = ve_now_ms();
  uint32_t waited = 0;
  int      alive;
  int      idle;

  /* ★ 听窗一进来就先把设备和耳朵要回来（2026-10-31）：
   *   上一代铃声让路停掉的录音，是「起线程 → 认领」这条链回来的，中间隔几十毫秒；
   *   而 AI 那一轮如果还在排队出块，它会在这中间把设备再抢走一次。
   *   所以这里无条件收一次，把 1.6 秒（确认期）整个听窗让给耳朵。 */

  /* ★ 2026-11-01：只在"真有人占着设备/还举着让路锁"时才抢。
   *
   *   上一版每个听窗都无条件 audio_play_stop()。铃自然播完时，播放线程的
   *   尾巴已经把麦克风还回来了（audio_resume_record），这一下等于把刚起跑的
   *   录音线程再停一次 —— 真机日志里紧接着就是
   *       [录音] 录音异常中断（read 返回 -1，errno=19）
   *   然后「没事」整段没被录到。音频来回拆装是"说没事没反应"的一半原因
   *   （另一半是门控把它当"忙"丢掉），这里先把这个多余动作去掉。 */

  if (audio_is_playing(g_ctx) || g_ctx->record_hold)
    {
      ve_seize_audio("听窗开始");
    }

  /* 先给录音最多 300 ms 起跑再报死活 —— 否则这一行永远报「死的」，
   * 分不出"刚让完路还没认领"和"真起不来"（上一版就是这么被误读的：
   * 实测有的窗口日志上一行写着「录音线程起跑并认领」，这里却报死的）。 */

  while (g_ctx != NULL && !audio_record_is_active(g_ctx) && waited < 300)
    {
      if (g_cancel || ve_on_tick(ve_now_ms()) != 0)
        {
          return 1;
        }

      usleep(20000);
      waited += 20;
    }

  alive = (g_ctx != NULL && audio_record_is_active(g_ctx));

  printf("[急救] #%lu 听 %lu ms 起（距触发 %lu ms，录音链路 %s，"
         "等它起来 %lu ms）\n",
         (unsigned long)g_cycle, (unsigned long)ms,
         (unsigned long)(ve_now_ms() - g_started_at),
         alive ? "活着" : "死的", (unsigned long)waited);

  while ((ve_now_ms() - start) < ms)
    {
      if (g_cancel)
        {
          return 0;
        }

      if (ve_on_tick(ve_now_ms()) != 0)
        {
          return 1;
        }

      usleep(20000);
    }

  /* ★ 听窗收尾报告（2026-10-31）：这一窗**到底有没有听到东西**。
   *   光看「链路活着」不够 —— 活着但一滴数据都没流过一样是聋的。
   *   audio_record_idle_ms() = 距上一滴音频多久：小于窗长 = 这一窗有数据流过；
   *   负值 = 这一代压根没有参照点（从未启动过录音 / ctx 空）。 */

  idle = (g_ctx != NULL) ? audio_record_idle_ms(g_ctx) : -1;

  printf("[急救] #%lu 听 %lu ms 收（距上一滴音频 %d ms%s）\n",
         (unsigned long)g_cycle, (unsigned long)ms, idle,
         idle < 0 ? "，从来没收到过音频"
                  : (idle < (int)ms ? "，这一窗有音频流过"
                                    : "，这一窗没收到音频"));

  return 0;
}

/****************************************************************************
 * 响一轮
 *
 * 走 ai_audio 的 audio_play_start()：那是"半双工让路 → 播完自动还麦"那条路，
 * 麦是**立刻**回来的。sf32lb52_alarm.c 的裸 open/STOP 那条路没有还麦，
 * 偷听窗会被吞掉（理由见 voice_emerg.h）。
 ****************************************************************************/

static void ve_play_done_cb(void *user_data)
{
  (void)user_data;
  sem_post(&g_play_done);
}

static void ve_ring_once(void)
{
  int ret = -1;
  int try;

  g_cycle++;

  printf("[急救] #%lu 响 %d ms 起（距触发 %lu ms）\n",
         (unsigned long)g_cycle, VOICE_EMERG_RING_MS,
         (unsigned long)(ve_now_ms() - g_started_at));

  /* 辅助模式：铃是板级报警（sf32lb52_alarm.c）在响，本模块**不插一脚** ——
   * 两套铃抢同一台半双工设备只会互相打死。这里只是按一轮时间走过去，
   * 好让 ve_listen_gap 里的 deadline 检查照常跑。 */

  if (g_assist)
    {
      usleep(VOICE_EMERG_RING_MS * 1000);
      return;
    }

  if (g_ctx == NULL)
    {
      /* 没接音频（比如纯逻辑测试）：按一轮时间睡过去 */

      usleep(VOICE_EMERG_RING_MS * 1000);
      return;
    }

  ve_drain(&g_play_done);

  /* ★ 2026-10-31：先把设备抢到手再起播，被拒不弃响（连试 3 次）。
   *
   *   上一版是「一次被拒就整个 1.2 秒静音等过去」，铃有洞；而且打的是
   *   audio_play_last_result() —— 那是**上一段**播放的结果，本段压根没起播，
   *   它还停在 -EINPROGRESS(-115)，于是串口里全是「失败（-115）」，
   *   看着像铃自己坏了，其实每一次都是被 AI 那轮的 TTS / 提示音抢了设备。
   *   这里改成打 audio_play_start() 的**真实返回值**。
   *
   *   -EBUSY 的两种来路都会被 ve_seize_audio 解掉：
   *     · ctx->playing 为真（AI 那一轮正在播）—— audio_play_stop() 掐掉它；
   *     · 上一代播放线程没收尾（reap 放弃后闩着）—— audio_play_stop() 走的
   *       正是「换代 + 解闩放行」那条自愈路。 */

  ve_seize_audio("起播前");

  for (try = 1; try <= 3; try++)
    {
      ret = audio_play_start(g_ctx, g_ring_pcm, VE_RING_SAMPLES,
                             ve_play_done_cb, NULL);
      if (ret >= 0)
        {
          if (try > 1)
            {
              printf("[急救] #%lu 铃声第 %d 次才起来（前面被抢过设备）\n",
                     (unsigned long)g_cycle, try);
            }

          break;
        }

      printf("[急救] #%lu 铃声起播第 %d 次没起来（真实返回 %d%s），"
             "抢回设备再试\n",
             (unsigned long)g_cycle, try, ret,
             ret == -EBUSY ? " = -EBUSY：有人正占着播放，或上一代没收尾" : "");

      ve_seize_audio("铃声起播被拒");
      usleep(150000);
    }

  if (ret < 0)
    {
      printf("[急救] #%lu 铃声起播连试 3 次都没起来（真实返回 %d），"
             "这一轮按时间等过去\n", (unsigned long)g_cycle, ret);
      usleep(VOICE_EMERG_RING_MS * 1000);
      return;
    }

  /* 铃响着的这 1.2 秒里麦克风是聋的（半双工）—— 这就是"响"，
   * 后面那 0.6 秒才是"听"。 */
  (void)ve_wait_play_done(VOICE_EMERG_RING_MS + 3000);
}

/****************************************************************************
 * VAD 最短语音门：进确认窗口调低，出窗口还回去
 ****************************************************************************/

static void ve_vad_lower(void)
{
  if (g_ctx == NULL || g_vad_saved)
    {
      return;
    }

  g_saved_vad_min = g_ctx->record_cfg.min_speech_ms;
  g_saved_vad_sil = g_ctx->record_cfg.silence_timeout_ms;
  g_vad_saved     = true;

  g_ctx->record_cfg.min_speech_ms    = VE_VAD_MIN_MS;
  g_ctx->record_cfg.silence_timeout_ms = VE_VAD_SIL_MS;

  printf("[急救] 确认窗口内 VAD 两道门都压到 %d ms"
         "（最短语音 %lu -> %d，静音判停 %lu -> %d）\n",
         VE_VAD_MIN_MS,
         (unsigned long)g_saved_vad_min, VE_VAD_MIN_MS,
         (unsigned long)g_saved_vad_sil, VE_VAD_SIL_MS);
}

static void ve_vad_restore(void)
{
  if (g_ctx == NULL || !g_vad_saved)
    {
      return;
    }

  g_ctx->record_cfg.min_speech_ms      = g_saved_vad_min;
  g_ctx->record_cfg.silence_timeout_ms = g_saved_vad_sil;
  g_vad_saved = false;

  printf("[急救] VAD 两道门还回（最短语音 %lu ms / 静音判停 %lu ms）\n",
         (unsigned long)g_saved_vad_min, (unsigned long)g_saved_vad_sil);
}

/****************************************************************************
 * 工作线程
 ****************************************************************************/

static void *ve_worker(void *arg)
{
  enum voice_emerg_phase_e final_phase;

  (void)arg;

  for (;;)
    {
      while (sem_wait(&g_kick) != 0 && errno == EINTR)
        {
        }

      if (g_phase != VOICE_EMERG_CONFIRM)
        {
          continue;             /* 被取消了 / 空唤醒 */
        }

      pthread_mutex_lock(&g_lock);
      g_cancel      = false;
      g_notified    = false;
      g_started_at  = ve_now_ms();
      g_confirm_end = g_started_at + VOICE_EMERG_CONFIRM_MS;
      g_stop_at     = g_started_at + VOICE_EMERG_AUTOSTOP_MS;
      g_renotify_at = 0;
      g_cycle       = 0;
      pthread_mutex_unlock(&g_lock);

      printf("[急救] 响铃开始：%d 秒内说「没事」就停，否则通知家人\n",
             VOICE_EMERG_CONFIRM_MS / 1000);

      /* ★ 报警一到点就接管音频（2026-10-31）：AI 那一轮的 TTS/提示音此刻多半
       *   正占着半双工的喇叭和麦克风，不抢的话整个 10 秒确认窗既响不了铃、
       *   也听不见「没事」。抢一次，后面的每一轮铃/听窗还会再抢。 */

      ve_seize_audio("报警接管");

      /* ★ 循环出口有两个：g_cancel（说「没事」）和 ve_listen_gap() 返回非 0
       *   （到点收摊）。后者曾经漏了 —— 只判 g_cancel 的话 60 秒自停失效，
       *   铃会一直响到关机。 */

      while (!g_cancel)
        {
          ve_ring_once();
          if (g_cancel)
            {
              break;
            }

          /* 确认期听长一点、发完 Bark 收回到 0.6 秒（理由见 voice_emerg.h）。 */

          if (ve_listen_gap(g_notified ? VOICE_EMERG_LISTEN_MS
                                       : VOICE_EMERG_LISTEN_CONFIRM_MS) != 0)
            {
              break;            /* 到点，阶段已置 EXPIRED */
            }
        }

      /* 收尾：把铃掐了（别人可能还在放），阶段定下来。
       *
       * ★ 这里**不要**补调 audio_record_hold_end()（2026-10-31 栽过）：
       *   它内部会 audio_drop_play_device()，判据是 `!playing` 而不是
       *   `play_exited`，播放线程还活着时就是跨线程踩 fd —— 报警一响整机卡死
       *   就是这么来的。让路锁由上面的 audio_play_stop() 开头那句冲掉了，
       *   麦克风由铃那条播放线程的尾巴还回来。 */

      if (g_ctx != NULL)
        {
          audio_play_stop(g_ctx);
        }

      /* ★ 2026-10-31：报警这一路最容易把音频上半部留在"半楔死"里收不了摊 ——
       *   真机日志（60 秒自停那一刻）的指纹就是
       *       audio: STOP 没停到硬件（status=5 nstate=5，本 fd 记账 0）
       *       audio:   会话[0] #81 priv->state=5  ★就是它在挡
       *   有个 fd 的 priv->state 钉在 RUNNING，把 audio_stop() 的放行判据恒判为假，
       *   之后每一次 CONFIGURE/START 都被静默吞掉却照样返回 OK —— 报警一结束
       *   整机就没声也没耳朵了，看着就是"彻底卡了"。
       *   audio_reclaim_stale() 是对症的药（一次性 fd 打一次 CONFIGURE(OUTPUT)，
       *   踩响内核那条 OUTPUT 抢 INPUT 的自愈，顺手 audio_forget_sessions()）。
       *   它开的是自己的一次性 fd，**不碰**本 ctx 的 fd，没有踩 fd 的风险；
       *   幂等，设备好好的时候白花几毫秒（ai_audio.c 原话："每轮自愈都调，
       *   不用先判定"）。放收尾而不放接管：接管那一刻我们只要干净地接过设备
       *   （audio_play_stop 已经够了），收摊这一刻才需要把钉死的记账抹掉，
       *   免得把烂摊子留给后面的对话/常听麦。 */

      (void)audio_reclaim_stale("急救报警收尾");

      pthread_mutex_lock(&g_lock);

      if (g_cancel && (g_phase == VOICE_EMERG_CONFIRM ||
                       g_phase == VOICE_EMERG_NOTIFIED))
        {
          /* 走到这里只有"说「没事」"一种来路：到点那条路已经把阶段置成
           * EXPIRED 了，不会再进这个 if。 */

          g_phase = VOICE_EMERG_CANCELLED;
          printf("[急救] 老人说没事 -> 停铃，**不通知家人**\n");
        }

      ve_vad_restore();
      final_phase = (enum voice_emerg_phase_e)g_phase;
      pthread_mutex_unlock(&g_lock);

      printf("[急救] 本轮结束，阶段=%d\n", g_phase);

      /* 收摊这一刻叫屏幕把红页撤掉（取消/到点都要）。放在解锁之后 ——
       * 回调那边要投递到 LVGL 线程，可能短暂取别的锁。 */
      ve_fire_hook(final_phase);
    }

  return NULL;
}

/****************************************************************************
 * Public
 ****************************************************************************/

int voice_emerg_init(audio_context_t *ctx)
{
  pthread_attr_t attr;

  if (g_inited)
    {
      return 0;
    }

  g_ctx = ctx;
  ve_build_tone();

  if (sem_init(&g_kick, 0, 0) != 0 || sem_init(&g_play_done, 0, 0) != 0)
    {
      printf("[急救] 信号量初始化失败\n");
      return -1;
    }

  pthread_attr_init(&attr);

  /* ★ 2026-10-31：4096 → 8192。这条线程是报警主循环，链路比想象的深：
   *   ve_ring_once → audio_play_start → audio_play_begin（开设备、ioctl、
   *   再起一条播放线程）+ 一串长格式串 printf，收尾还要跑 audio_reclaim_stale
   *   （open + 两次 ioctl + close）。4096 一直悬着，栈溢出的症状和"整机卡死"
   *   长得一模一样，出了事分不清。多 4 KB 换掉一个说不清的变量。
   *   参考：ve_push_async 那条要走 TLS，8192。 */

  pthread_attr_setstacksize(&attr, 8192);

  if (pthread_create(&g_worker_tid, &attr, ve_worker, NULL) != 0)
    {
      printf("[急救] 工作线程起不来\n");
      pthread_attr_destroy(&attr);
      return -1;
    }

  pthread_attr_destroy(&attr);

  g_inited = true;

  /* ★ 2026-10-31 这一版的落地标记（判据⑥）：确认窗内静音判停门也压到 200 ms
   *   + 逐轮日志。串口开机看到它 = 这一版落地了；看不到就是还跑着旧固件。 */

  printf("[急救] 就绪：响 %d ms / 听 %d ms（确认期）· %d ms（发完 Bark 后），"
         "%d 秒内说「没事」可取消"
         "（2026-10-31 确认窗内 VAD 两道门都压到 %d ms，逐轮有日志）\n",
         VOICE_EMERG_RING_MS, VOICE_EMERG_LISTEN_CONFIRM_MS,
         VOICE_EMERG_LISTEN_MS, VOICE_EMERG_CONFIRM_MS / 1000, VE_VAD_MIN_MS);

  /* ★ 统一报警路径（2026-10-31）落地标记：AI 语音报警、本地摔倒/异常声报警
   *   从这一版起走**同一条**路，行为一律是
   *   「响铃不许断 → 10 秒内说「没事」/点屏幕按钮取消 → 否则 Bark 通知家人
   *     → 持续响 + 30 秒重发 → 60 秒自停」。
   *   串口开机看到这一行 = 新固件落地；看不到就是还跑着旧的 20 秒追问那套。 */

  printf("[版本] 统一报警路径：AI 报警和摔倒/异常声报警同一条（10 秒没事取消 / "
         "超时 Bark / 30 秒重发 / 3 分钟自停）（2026-11-01）\n");

  /* ★ 2026-11-01 v13 落地标记：报警时长 3 分钟 + 听窗更宽 + 收摊撤页。
   *   用户拍板：10 秒内不说「没事」/不点按钮就发 Bark，之后每 30 秒重发，
   *   总时长 3 分钟。听窗从 1.2/0.6 放宽到 1.2/2.0（确认期）与 1.2/1.2
   *   （通知后），并且在听窗开始不再无条件 audio_play_stop —— 那一下会把
   *   刚起跑的录音线程再停一次，紧跟着就是 read ENODEV，「没事」被漏掉。 */

  printf("[版本] 报警 3 分钟 + 听窗放宽 + 收摊撤页停铃：10 秒 Bark、30 秒重发（2026-11-01）\n");

  /* ★ 本批（2026-10-31 第三批）的落地标记：修「一报警响铃整机就卡死」。
   *   上一版为了抢回被 AI TTS 占着的半双工设备，在报警线程里多调了一次
   *   audio_record_hold_end() —— 它内部 audio_drop_play_device() 判据是
   *   `!playing` 不是 `play_exited`，播放线程还活着就是跨线程踩 fd，
   *   每 1.8 秒踩一次，把"半楔死"推成"彻底卡死"。
   *   这一版抢设备**只走 audio_play_stop()**（它自己按 play_exited 判完才动
   *   fd），收尾再补一次 audio_reclaim_stale() 把钉死的会话记账抹掉。
   *   串口开机看到这一行 = 这一版落地；看不到就是还跑着会卡死的那版。 */

  printf("[版本] 报警抢音频设备：只走 audio_play_stop 不踩 fd"
         "（一响铃就整机卡死修复）+ 收尾强制回收陈旧会话（2026-10-31）\n");
  return 0;
}

void voice_emerg_trigger(const char *reason, const char *text)
{
  if (!g_inited)
    {
      printf("[急救] 没初始化就触发，忽略\n");
      return;
    }

  pthread_mutex_lock(&g_lock);

  if (g_phase == VOICE_EMERG_CONFIRM || g_phase == VOICE_EMERG_NOTIFIED)
    {
      /* ★ 已经在报警中：**不重新计时**。
       *   否则来一句假的「救命」就能把 10 秒确认窗口无限往后推，
       *   Bark 永远发不出去 —— 而那正是这个功能存在的意义。 */

      printf("[急救] 已在报警中，忽略这次触发（%s）\n",
             reason != NULL ? reason : "?");
      pthread_mutex_unlock(&g_lock);
      return;
    }

  snprintf(g_reason, sizeof(g_reason), "%s",
           reason != NULL ? reason : "voice_sos");
  snprintf(g_text, sizeof(g_text), "%s", text != NULL ? text : "");

  /* 板级报警是不是已经在响了？是的话转辅助模式（不重复响铃/推送）。
   * 判据用 alarm_get_status()，那是无锁安全读、不用先 init 也能调。 */

  {
    struct alarm_status_s st;

    g_assist = (alarm_get_status(&st) == OK && st.active);
    if (g_assist)
      {
        printf("[急救] 板级报警已在响 -> 辅助模式"
               "（不重复响铃/推送，只盯「没事」好把它掐掉）\n");
      }
  }

  g_phase  = VOICE_EMERG_CONFIRM;
  g_cancel = false;

  ve_vad_lower();

  pthread_mutex_unlock(&g_lock);

  printf("[急救] 触发：%s —— %s\n", g_reason,
         g_text[0] != '\0' ? g_text : "(无原话)");

  sem_post(&g_kick);
}

bool voice_emerg_is_active(void)
{
  return g_phase == VOICE_EMERG_CONFIRM || g_phase == VOICE_EMERG_NOTIFIED;
}

enum voice_emerg_phase_e voice_emerg_phase(void)
{
  return (enum voice_emerg_phase_e)g_phase;
}

unsigned voice_emerg_elapsed_ms(void)
{
  if (!voice_emerg_is_active() || g_started_at == 0)
    {
      return 0;
    }

  return (unsigned)(ve_now_ms() - g_started_at);
}

void voice_emerg_on_text(const char *text)
{
  if (!voice_emerg_is_active() || text == NULL)
    {
      return;
    }

  /* ★ 判据⑥ 的另一半可观测：ASR 文本到底有没有回到这一层。
   *   铃响期间每来一句都记一笔（不管是不是「没事」）—— 一句都没有 = 整条
   *   VAD→ASR 链没跑通；有别的句子、没「没事」 = 链是通的，是判定没认出来。 */

  /* ★ 2026-11-02：**短句一律当「没事」**。
   *
   *   真机 ASR 把「没事」听成过「好,是又调。」「冤枉」「收到几个呀?」，
   *   关键词匹配永远追不上误听变体。而报警确认期用户的**唯一合理回答**
   *   就是「没事」—— 短句（≤ 15 字）在报警期当「没事」处理，
   *   长句（电视声/噪音）照旧忽略。
   *
   *   安全性：emerg_classify 在本函数**之前**已把「救命/着火」等
   *   急救词拦走，不会误停真正的求救。 */

  {
    /* ★ 2026-11-02：**按字符数，不按字节数**。原来 strlen() 对 UTF-8 中文
     *   是字节数：一句「我没事，取消报警」= 8 个字 = 24 字节，被判成
     *   ">15 字长句" —— 用户按提示说完整句，反而被当噪音，铃照响。
     *   真机 20:16：「你是石小姐？」（6 个字/18 字节）也被判成长句。 */
    size_t tlen = 0;
    const char *p;

    for (p = text; *p != '\0'; p++)
      {
        if (((unsigned char)*p & 0xc0) != 0x80)   /* 只数 UTF-8 引导字节 */
          {
            tlen++;
          }
      }

    if (tlen > 15)
      {
        printf("[急救] 报警中收到长句（>15 字，判为噪音，铃照响，"
               "距触发 %lu ms）：%s\n",
               (unsigned long)(ve_now_ms() - g_started_at), text);
        return;
      }

    /* 短句 → 一律当「没事」 */
    printf("[急救] 报警中收到短句（≤15 字，判为「没事」）：%s\n", text);
  }

  /* 走到这里就一定是「没事」/「不用了」（上面已经把别的都挡掉了） */

  voice_emerg_cancel("语音「没事」");
}

/****************************************************************************
 * Name: voice_emerg_cancel
 *
 * Description:
 *   取消报警。语音那句「没事」和屏幕上那个「我没事，取消」按钮**走这同一条**
 *   —— 用户拍板过两者必须逐字一致，所以只许有这一份实现。
 *
 ****************************************************************************/

/* ★ 2026-11-02：detached 线程里调 audio_play_stop，不阻塞 UI。
 *   audio_play_stop 里有 join（最多 1 秒），在 LVGL 线程里调会卡住按钮。 */
static void *ve_cancel_stop_audio(void *arg)
{
  audio_context_t *ctx = (audio_context_t *)arg;

  if (ctx != NULL)
    {
      audio_play_stop(ctx);
    }

  return NULL;
}

void voice_emerg_cancel(const char *src)
{
  if (!voice_emerg_is_active())
    {
      printf("[急救] 取消请求（%s）：本来就没在报警，忽略\n",
             src != NULL ? src : "?");
      return;
    }

  if (src != NULL && strstr(src, "兜底") != NULL)
    {
      /* 主循环的 3 分钟兜底：不是有人说了「没事」，别让日志误导排查。 */
      printf("[急救] %s：强制收摊（距触发 %lu ms）\n", src,
             (unsigned long)(ve_now_ms() - g_started_at));
    }
  else
    {
      printf("[急救] 听到「没事」-> 取消报警（距触发 %lu ms，来源=%s）\n",
             (unsigned long)(ve_now_ms() - g_started_at),
             src != NULL ? src : "?");
    }

  g_cancel = true;

  /* ★ 2026-11-02：**起一条 detached 线程立刻掐铃**。
   *
   *   v34 去掉 audio_play_stop 后，铃的停止要等 ve_worker 工作线程
   *   自己检测 g_cancel（最多 1.2 秒后），用户看到"按钮关了铃还响"。
   *   但也不能在 LVGL 线程直接调 audio_play_stop（有 join，会卡 UI）。
   *   折中：detached 线程调 audio_play_stop —— 立刻掐铃、不卡 UI。 */
  {
    pthread_attr_t a;
    pthread_t      t;

    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 4096);

    if (pthread_create(&t, &a, ve_cancel_stop_audio, g_ctx) != 0)
      {
        /* 线程起不来就直接调（最多卡 UI 1 秒，好过铃不停） */
        if (g_ctx != NULL) audio_play_stop(g_ctx);
      }

    pthread_attr_destroy(&a);
  }

  /* 辅助模式下铃是板级报警在响，得叫它停 —— 这正是本模块给面板按钮
   * 补上的那件事：老人误触大红按钮，说一声「没事」就能掐掉。 */

  if (g_assist)
    {
      printf("[急救] 辅助模式：叫板级报警一起停\n");
      alarm_clear();
    }

  sem_post(&g_play_done);      /* 叫醒可能在等播完的工作线程 */
}

/****************************************************************************
 * Name: voice_emerg_confirm_left_ms
 *
 * Description:
 *   10 秒确认窗还剩多少毫秒（给屏幕上的倒计时用）。不在确认期返回 0。
 *
 ****************************************************************************/

unsigned voice_emerg_confirm_left_ms(void)
{
  uint32_t now;
  uint32_t left;

  if (g_phase != VOICE_EMERG_CONFIRM)
    {
      return 0;
    }

  now = ve_now_ms();
  if (now >= g_confirm_end)
    {
      return 0;
    }

  left = g_confirm_end - now;
  return (unsigned)left;
}

/****************************************************************************
 * Name: voice_emerg_set_hook
 *
 * Description:
 *   登记阶段回调（界面跟着状态改口用）。传 NULL 卸掉。
 *
 ****************************************************************************/

void voice_emerg_set_hook(voice_emerg_hook_t cb, void *arg)
{
  pthread_mutex_lock(&g_lock);
  g_hook     = cb;
  g_hook_arg = arg;
  pthread_mutex_unlock(&g_lock);
}
