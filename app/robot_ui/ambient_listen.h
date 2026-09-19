/****************************************************************************
 * app/robot_ui/ambient_listen.h
 *
 * 常态听音 + 突发大音量检测（工作流 C）
 *
 * 用户原话：「加上一个常态录音，音量检测，检测到突发的大音量就把该段录音
 * 发给大模型，让模型来识别。」
 *
 * 这个模块干的事：
 *   ① 用一个后台线程**常态**占着麦克风，按 20 ms 一帧算能量；
 *   ② 环形缓冲保留最近 5 秒 PCM（16k/单声道/s16le = 160000 字节）；
 *   ③ 判到"突发的大音量"时，取「触发前 2 秒 + 触发后 1 秒」= 3 秒，
 *      走 voice_asr_recognize() 识别，识别完（可选）再 mimo_chat() 问一句
 *      "这是什么声音/屋里有没有异常"；
 *   ④ 结果通过回调交给父 agent（父 agent 在回调里走 ui_post_xxx 投递，
 *      本模块**不碰任何 LVGL 控件** —— 它跑在非 LVGL 线程里）。
 *
 * ⚠️ 三条必须知道的约束：
 *
 * 1) **本板半双工**：麦克风是独占的（AUDIOIOC_STOP 会把录/放两条通路一起停），
 *    所以本模块常态占着麦克风时，语音聊天的 audio_record_start() 会拿到
 *    -EBUSY。父 agent 必须在「要用麦克风/要放音」的路径上先把它按停：
 *      语音聊天开始录音前   -> ambient_listen_hold(AMBIENT_HOLD_CHAT, true)
 *      这一轮（含 TTS 播完）-> ambient_listen_hold(AMBIENT_HOLD_CHAT, false)
 *      提醒铃声/TTS 单独放音 -> ambient_listen_hold(AMBIENT_HOLD_PLAYBACK, ...)
 *    两个入口都是**幂等**的（传同一个值两次等于一次），不会因为少调一次就
 *    把麦克风永久扣住；忘了调也只是会掉一次触发，不会死锁。
 *
 * 2) **回调在听音线程里跑**（不是 LVGL 线程）：里面不能碰控件，要刷界面
 *    必须用 main.c 里已有的 ui_post() / ui_post_reminder() / ui_post_alarm() /
 *    ui_post_error() 投递。回调是同步的，msg->text / msg->reply 指向本模块的
 *    栈上缓冲，**回调返回后立刻失效**，要留下自己拷一份。
 *
 * 3) **默认关**：要长期占麦克风 + 常驻约 283 KiB 堆（250 KiB 缓冲 + 32 KiB
 *    线程栈），必须用户明确打开
 *    （配置键 enable_ambient_listen，或 ambient_listen_enable(true)）。
 ****************************************************************************/

#ifndef __APP_ROBOT_UI_AMBIENT_LISTEN_H
#define __APP_ROBOT_UI_AMBIENT_LISTEN_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* 一次触发的事件类型 */
typedef enum
{
  AMBIENT_EVENT_TRIGGER = 0,  /* 刚检到突发大音量（片段已取好，麦克风已让出） */
  AMBIENT_EVENT_RESULT,       /* 一轮处理结束：text / reply 有效（可能为 NULL） */
  AMBIENT_EVENT_ERROR         /* 一轮处理失败：err 是负 errno，text 可能仍有值 */
} ambient_event_t;

/* 交给父 agent 的事件。
 *
 * ⚠️ 回调是**同步**的：text / reply 只在回调返回前有效，要留下自己拷一份。 */
typedef struct
{
  ambient_event_t type;
  const char     *text;   /* ASR 文本，没有则 NULL */
  const char     *reply;  /* 大模型回复，没有则 NULL */
  int             err;    /* 负 errno；成功为 0 */
  int             level;  /* 触发那一帧的平均幅度 0..32767（判定用的量） */
  int             peak;   /* 触发那一帧的峰值幅度 0..32767（只看不判） */
  uint32_t        seq;    /* 事件序号，从 1 开始递增 */
} ambient_listen_msg_t;

typedef void (*ambient_listen_cb_t)(const ambient_listen_msg_t *msg,
                                    void *user_data);

/* 喇叭是不是正在响（由父 agent 提供，例如返回 audio_is_playing(音频上下文)）。
 * 返回 true 期间本模块既不占麦克风、也不判突发 —— 这是"别把喇叭自己放出来的
 * 声音当异常"的第二道保险（第一道是上面的 hold）。可以不注册（NULL = 不查）。 */
typedef bool (*ambient_busy_cb_t)(void *user_data);

/* 让出麦克风的几路来源。用**位掩码**而不是计数器：每个来源各自置位/清位，
 * 重复调用不会累加，配错也只会多按一次或少按一次，不会把麦克风永久扣住。 */
typedef enum
{
  AMBIENT_HOLD_MANUAL = 0,    /* ambient_listen_pause() / _resume() 控制的那一格 */
  AMBIENT_HOLD_CHAT,          /* 语音聊天整轮：录音 + 识别 + 对话 + TTS 播放 */
  AMBIENT_HOLD_PLAYBACK,      /* 单独放音：提醒铃声 / 别的 TTS / 报警音 */
  AMBIENT_HOLD_COUNT
} ambient_hold_t;

/* 运行统计（只读快照，调试和回报用） */
typedef struct
{
  uint32_t frames;      /* 处理过的 20 ms 帧数 */
  uint32_t triggers;    /* 触发次数 */
  uint32_t asr_ok;      /* 识别出非空文本的次数 */
  uint32_t chat_ok;     /* 大模型给出回复的次数 */
  uint32_t errors;      /* 识别/对话失败的次数 */
  int32_t  noise_floor; /* 当前的慢基线（峰值口径，调试用） */
  uint32_t holds;       /* 当前的 hold 位掩码 */
} ambient_listen_stats_t;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: ambient_listen_init
 *
 * Description:
 *   登记回调、读配置开关。**默认关**：只有配置键 enable_ambient_listen
 *   是 1/true/on 时才真的起线程占麦克风。
 *
 *   配置键（和 llm_host 同一份 /data/ai_agent/config/config.json）：
 *     enable_ambient_listen —— 总开关，默认关。不配 = 关。
 *     enable_ambient_chat   —— 识别出内容后是否再问大模型，默认开。
 *
 *   必须在 mimo_asr_register() + voice_asr_set_backend("mimo") 之后调
 *   （不然第一次触发时 voice_asr_recognize() 没有后端）。
 *
 * Input Parameters:
 *   cb        - 事件回调，非 NULL；在**听音线程**里跑，不能碰 LVGL 控件；
 *   user_data - 回调用户数据。
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 ****************************************************************************/

int ambient_listen_init(ambient_listen_cb_t cb, void *user_data);

/****************************************************************************
 * Name: ambient_listen_enable
 *
 * Description:
 *   运行期开关。true = 起听音线程并占住麦克风；false = 让线程退出。
 *
 *   内存：true 时一次性分配
 *     环形缓冲 160000 字节 + 事件片段 96000 字节 + 单帧 640 字节
 *     ≈ 250 KiB 堆，外加线程栈 32 KiB（NuttX 的 pthread 栈也是堆上分配的）。
 *   CPU：常态就是每 20 ms 一次读 + 一次 320 点的峰值/能量统计（几十微秒级），
 *     基本等于"读设备"本身的开销；不触发时没有任何网络请求。
 *
 *   false 是**异步**的：线程如果正卡在一次云端 ASR/对话里（可能几十秒到
 *   一分钟），它会跑完那一次再退出并自己释放缓冲 —— 这里不等它，否则会在
 *   LVGL 线程里卡住界面。所以在它退出之前再 enable(true) 会返回 -EBUSY。
 *
 * Returned Value:
 *   OK on success; -EBUSY = 上一次还没退干净 / -ENOMEM = 内存不够。
 ****************************************************************************/

int ambient_listen_enable(bool enable);

/****************************************************************************
 * Name: ambient_listen_is_enabled / _is_paused / _is_busy
 *
 * Description:
 *   is_enabled - 线程是否已经起来（用户在设置里打开过）；
 *   is_paused  - 当前是否因为某一路 hold 或喇叭在响而让出了麦克风；
 *   is_busy    - 正在跑一次触发的处理（ASR/对话）。
 ****************************************************************************/

bool ambient_listen_is_enabled(void);
bool ambient_listen_is_paused(void);
bool ambient_listen_is_busy(void);

/****************************************************************************
 * Name: ambient_listen_hold
 *
 * Description:
 *   按停/放开某一路上来源（见 ambient_hold_t）。**幂等**，可以随便重复调。
 *
 *   hold 置位后最迟 50 ms 内（一次轮询周期）麦克风就会被真的关掉；
 *   要在关掉之后立刻自己开设备的路径（语音聊天开始录音）请用
 *   ambient_listen_pause()，那个是同步的。
 ****************************************************************************/

void ambient_listen_hold(ambient_hold_t who, bool on);

/****************************************************************************
 * Name: ambient_listen_pause / ambient_listen_resume
 *
 * Description:
 *   同步让出 / 交还麦克风，等价于 hold(AMBIENT_HOLD_MANUAL, ...)，
 *   但 pause() **返回时麦克风一定已经关了**（内部会等听音线程手上那次
 *   open/close 做完，一般几毫秒），所以紧接着可以放心
 *   audio_record_start()。两个都是幂等的。
 *
 *   典型用法（父 agent 侧）：
 *     语音聊天按钮按下 -> ambient_listen_pause()  -> audio_record_start()
 *     这一轮彻底结束   -> ambient_listen_resume()
 ****************************************************************************/

void ambient_listen_pause(void);
void ambient_listen_resume(void);

/****************************************************************************
 * Name: ambient_listen_set_busy_cb
 *
 * Description:
 *   注册"喇叭正在响"的判断（例如返回 audio_is_playing(音频上下文)）。
 *   注册后，它返回 true 期间本模块会主动关掉麦克风、也不判突发。
 *   传 NULL 注销。这只是保险；正常的做法仍然是上面的 hold。
 ****************************************************************************/

void ambient_listen_set_busy_cb(ambient_busy_cb_t fn, void *user_data);

/****************************************************************************
 * Name: ambient_listen_set_chat_enabled
 *
 * Description:
 *   运行期开关"识别完还要不要问大模型"。关掉就只有 ASR 文本，一次触发
 *   省掉一次会阻塞几十秒的 HTTPS 推理。
 ****************************************************************************/

void ambient_listen_set_chat_enabled(bool enable);
bool ambient_listen_chat_enabled(void);

/****************************************************************************
 * Name: ambient_listen_get_stats
 *
 * Description:
 *   取一份统计快照（纯读，随时可调）。stats 不能为 NULL。
 ****************************************************************************/

void ambient_listen_get_stats(ambient_listen_stats_t *stats);

#endif /* __APP_ROBOT_UI_AMBIENT_LISTEN_H */
