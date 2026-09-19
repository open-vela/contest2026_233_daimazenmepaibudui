/****************************************************************************
 * app/hello_app/mimo_voice.h
 *
 * 小米 MiMo 云端后端（ASR + TTS + 对话 chat）的对外接口。
 *
 * 实现见 mimo_voice.c：走普通 HTTPS POST（不是火山那套 WebSocket），
 * 凭据从 ai_agent 的配置库读（llm_host / api_key / model），本模块不含任何密钥。
 * mimo_chat() 是给别的 app（robot_ui）用的跨 app 对话接口，不依赖 ai_agent
 * 进程状态。
 *
 * 语音（voice_tts_speak 这条）对**调用方缓冲**的要求：一次能念多少字由
 * mimo_voice.c 的 MIMO_TTS_TEXT_MAX（300 字）和调用方的 PCM 缓冲一起决定，
 * 两块都要按"300 字 ≈ 60 秒 ≈ 2 MB"（16 kHz 单声道 s16le，32 KB/秒）准备：
 *   - robot_ui 的 VOICE_TTS_BUF_BYTES 建议 (16000 * 2 * 70) = 2240000 字节；
 *   - ai_audio 的 AUDIO_PLAY_BUFFER_MS 建议 70000（audio_play_start 会把
 *     PCM 整段拷进自己的播放缓冲，装不下直接返回 -ENOSPC，所以它也得这么大）。
 * 两块合起来约 4.3 MB，加上合成时那块 896 KB 的响应缓冲，一轮 TTS 的峰值
 * 约 5.2 MB（用户堆约 8 MB）。缓冲给得比这小也不会越界：mimo_tts_synthesize()
 * 按剩余空间自动少念几个字，并在日志里报"念到第几个字"。
 *
 ****************************************************************************/

#ifndef __APP_HELLO_APP_MIMO_VOICE_H
#define __APP_HELLO_APP_MIMO_VOICE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>

/* IP 定位（用户所在城市）的接口：mimo_chat() 自己会用它查一次并写进 system
 * prompt，跨 app 调 mimo_chat() 的人不用管；想自己预热缓存（比如联网之后先
 * 查一次）可以直接调 mimo_location_get()，声明和用法见 mimo_location.h。
 * 放在这里是为了"只 include mimo_voice.h 的调用方也能拿到这套接口"。 */

#include "mimo_location.h"

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: mimo_asr_register / mimo_tts_register
 *
 * Description:
 *   把 MiMo 的 ASR / TTS 后端挂进 voice_asr.c / voice_tts.c 的分发层，
 *   后端名字都是 "mimo"（用 voice_asr_set_backend("mimo") 选中）。
 *
 *   注意：register 只是登记；真正的凭据检查在 init / 每次调用时做，
 *   没配 llm_host + api_key 时注册会成功但调用返回 -ENOENT。
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int mimo_asr_register(void);
int mimo_tts_register(void);

/****************************************************************************
 * Name: mimo_voice_available
 *
 * Description:
 *   配置里同时有非空的 api_key 和 llm_host 就返回 1，否则返回 0。
 *   ai_companion 用它决定语音后端选 "mimo" 还是 "volcengine"。
 *
 ****************************************************************************/

int mimo_voice_available(void);

/****************************************************************************
 * Name: mimo_chat
 *
 * Description:
 *   同步跑完一次对话，把回复正文写进 reply_out。请求体（POST 到配置里的
 *   llm_host + llm_path）：
 *     {"model":"<配置里的 model>",
 *      "messages":[{"role":"system","content":"<当前北京时间 + 星期 +
 *                  [用户所在城市] + 说话要求>"},
 *                  {"role":"user","content":"<转义后的文本>"}],
 *      "max_tokens":1024}
 *   当前时间是从系统时钟自己 +8 小时算的，放在 system prompt 里 —— 所以
 *   "今天几号 / 现在几点 / 今天星期几"不用联网，直接照时间答。
 *   用户所在城市来自 mimo_location_get()（板子自己打免 key 的 HTTPS 接口问
 *   IP 定位，结果缓存 6 小时）：**查到才写进提示**，查不到那一段一个字都没有。
 *   本函数在开始对话前会**同步**调它一次（第一次要一次 TLS 请求，之后走缓存），
 *   所以别在 LVGL 线程里调 mimo_chat()（本来也不行，见下面"会阻塞"那段）。
 *   Bearer = 配置里的 api_key。解析只取 choices[0].message.content，
 *   不取 reasoning_content（这个模型是推理模型，思维链在 reasoning_content，
 *   正文才在 content）。
 *
 *   两个工具（请求体多一段 "tools"，模型先回 finish_reason="tool_calls"，
 *   板子执行完把 assistant/tool 两条消息追加进 messages 再问一轮，最多 3 轮）：
 *     add_reminder(time,title) —— **默认开**：配置键 enable_reminder_tool
 *       （"1"/"true"/"on" 算开，配成 "0"/"false"/"off" 才关），另外必须已经用
 *       mimo_set_reminder_hook() 注册了落地实现，否则工具不下发。执行时把
 *       规范化好的 "HH:MM" 和标题交给那个回调（robot_ui 会写进提醒列表并
 *       重挂 RTC 闹钟），工具结果是"已建好：每天 08:00 提醒用户吃药"，模型据此
 *       复述确认。模型给的时间不合法就打回去让它自己换算重试（不猜）；
 *       落地失败（列表满等）也当成工具结果讲给用户听，不返回负值。
 *     get_weather(city) —— **默认关**：配置键 enable_weather_tool 开才带，
 *       开着时系统提示里会加默认城市（配置键 default_city；IP 定位查到了就
 *       用定位的城市，查不到才退回 default_city）。模型要查天气时本函数自己
 *       去打 open-meteo（geocode + forecast 两个免 key 的 HTTPS GET）。
 *       默认不开是因为查出来的实况和用户所在地常对不上。
 *   两个工具查不到 / 执行失败都只管把原因喂回模型让它解释，只有网络层失败
 *   （TLS/DNS/连接 / HTTP 非 200）才返回负值。
 *
 *   这是给**别的 app**（如 robot_ui）用的跨 app 接口：自带凭据、只走 HTTPS，
 *   不依赖 ai_agent 进程 / 消息总线。ai_agent 的 llm_send_text() /
 *   velaclaw_* 只能在 ai_agent 自己进程里用，别的 app 调会撞
 *   "消息队列锁未初始化"的 NXSEM_IS_MUTEX 断言，把整个 app 打死。
 *
 *   会阻塞（TLS 握手 + 云端推理，可能几十秒到一两分钟），只能在非 LVGL
 *   线程里调用。
 *
 * Input Parameters:
 *   text      - 用户输入文本（UTF-8）；
 *   reply_out - 输出缓冲（调用方提供）；
 *   reply_cap - 输出缓冲大小（含结尾 '\0'）。
 *
 * Returned Value:
 *   回复正文长度（字节）；<0 为负 errno（-ENOENT 未配置 / -EIO HTTP 或网络
 *   失败 / -EPROTO 响应里没有正文或解析不出工具调用）。
 *
 ****************************************************************************/

int mimo_chat(const char *text, char *reply_out, size_t reply_cap);

/****************************************************************************
 * Name: mimo_set_reminder_hook
 *
 * Description:
 *   注册"替用户建一条提醒"的实现，让 mimo_chat() 的 add_reminder 工具能落地。
 *
 *   为什么要回调而不是直接调 reminder_sched_*：提醒列表（reminder_sched.c）
 *   属于 robot_ui 这个 app，本文件属于 hello_app。hello_app 单独编的时候
 *   那边不存在，直接引用会缺符号；用回调把两个 app 解耦，谁提供谁注册。
 *
 *   fn 传 NULL = 注销。**没注册时 add_reminder 工具根本不出现在 tools 表里**
 *   （模型看不到它，也就不会调一个没人实现的工具）。
 *
 *   fn 的约定：
 *     hhmm  - 24 小时制 "HH:MM"，已由 mimo_voice.c 校验并规范化成两位数；
 *     title - 提醒内容（UTF-8，已按整字截到 20 字以内）。
 *   返回 0 = 建好了；负 errno（如 -ENOSPC 列表满）会作为工具结果喂回模型，
 *   让模型自己跟用户解释。
 *
 *   注意：fn 在**语音工作线程**里执行（不是 LVGL 线程），里面不能碰 LVGL
 *   控件；要刷界面请自己用 lv_async_call() 投过去。
 *
 ****************************************************************************/

typedef int (*mimo_reminder_hook_t)(const char *hhmm, const char *title);

void mimo_set_reminder_hook(mimo_reminder_hook_t fn);

/****************************************************************************
 * Name: mimo_wav_load_16k
 *
 * Description:
 *   读一个 WAV 文件，转成整条语音链路用的 16 kHz / 单声道 / s16le。
 *
 *   支持：16bit PCM（fmt=1）；多声道只取第 0 声道；采样率不是 16k 时
 *   用线性插值重采样到 16k（MiMo TTS 回的 24k 就是这么处理的）。
 *   不支持 8bit / 浮点 / ADPCM（返回 -ENOSYS）。
 *
 *   给 `hw_test asr <文件>` 用：板子上的录音链路是 16k 单声道，
 *   随手放的测试 WAV 可能是 24k/44.1k，这里统一转换。
 *
 * Input Parameters:
 *   path    - WAV 文件路径（任意路径，不要求 ROMFS）；
 *   pcm_out - 输出：堆上分配的 PCM 缓冲（调用方 free）；
 *   pcm_len - 输出：PCM 字节数。
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int mimo_wav_load_16k(const char *path, unsigned char **pcm_out,
                      size_t *pcm_len);

#endif /* __APP_HELLO_APP_MIMO_VOICE_H */
