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
 * 调用方缓冲按"300 字 ≈ 60 秒 ≈ 2 MB"（16 kHz 单声道 s16le，32 KB/秒）准备
 * （hello_app 的 g_tts_buf / robot_ui 的 s_tts_buf，都是开机预分配 + 阶梯降级）。
 *
 * ⚠ 2026-10-19 更新：播放层**不再需要**同尺寸的第二份缓冲 —— TTS 出声走
 * audio_play_start_ref()（零拷贝，直读调用方缓冲），ai_audio 的
 * AUDIO_PLAY_BUFFER_MS 已从 70000 砍回 16000（play_buf 只服务短提示音）。
 * 一轮 TTS 的常驻峰值就是调用方那 2 MB + 合成时 896 KB 响应缓冲 ≈ 3 MB；
 * 旧口径"两块合起来 4.3 MB"曾把开机水位挤爆（hello_app 崩溃重启死亡螺旋，
 * 取证见 ai_audio.h 的 AUDIO_PLAY_BUFFER_MS 注释）。缓冲给得比 2 MB 小也
 * 不会越界：mimo_tts_synthesize() 按剩余空间自动少念几个字，并在日志里报
 * "念到第几个字"。
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

/* ★ 2026-11-03：直调 MiMo ASR（不走 voice_asr 的后端切换）。
 *   专用于报警听窗兜底：火山空结果时再识别一次同一段 PCM。
 *   返回 OK / 负 errno；text_out 成功时为 UTF-8 文本。 */
int mimo_asr_recognize_direct(const unsigned char *pcm_data, size_t pcm_len,
                              char *text_out, size_t text_cap);

/* ★ 2026-11-01：把火山「语音合成模型2.0」设为流式播报的首选后端。
 *   on != 0：mimo_tts_speak_head 先走 volcengine（seed-tts-2.0 + 2.0 音色），
 *            这一块失败再直连 MiMo 兜底；
 *   on == 0：恢复 MiMo 优先。
 *   调用方（ai_companion）在 volc_api_key 已配置时打开它。 */
void mimo_tts_prefer_volc(int on);

/****************************************************************************
 * Name: mimo_voice_available
 *
 * Description:
 *   配置里同时有非空的 api_key 和 llm_host 就返回 1，否则返回 0。
 *   ai_companion 用它决定语音后端选 "mimo" 还是 "volcengine"。
 *
 ****************************************************************************/

int mimo_voice_available(void);

/* ★ 2026-11-01：天气快速通道（绕开 velaclaw/LLM，直接 open-meteo）。
 * city 为空时用默认城市（济南）。返回 OK 时 out 是一句可以直接念的中文，
 * 也可能包含"没找到/不可用"的说明；调用方遇到否定结果就回落到大模型路径。 */
int mimo_weather_lookup(const char *city, char *out, size_t out_cap);

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
 *   三个工具（请求体多一段 "tools"，模型先回 finish_reason="tool_calls"，
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
 *     adjust_setting(item,action,value) —— **默认开**：配置键
 *       enable_setting_tool（"1"/"true"/"on" 算开，配成 "0"/"false"/"off"
 *       才关），同样必须先用 mimo_set_settings_hook() 注册落地实现。用户说
 *       「声音大一点」「把亮度调到最亮」这类话时调用，真正改的是设置页里那
 *       两个值（不是只回一句话）。声明见下面 adjust_setting 工具那节。
 *   工具查不到 / 执行失败都只管把原因喂回模型让它解释，只有网络层失败
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
 * Name: mimo_reminder_apply
 *
 * Description:
 *   「替用户建一条提醒」的**唯一落地入口**。两条对话链路的 add_reminder 工具
 *   都走这里（mimo_chat 那条 + ai_tools_provider 那条），标题截断、时间归一化、
 *   hook 缺失兜底只写一份，不会漂移。
 *
 *   落地是 robot_ui 启动时注册进来的 voice_add_reminder()：内部
 *   reminder_sched_add + 刷新 + 通知界面，所以**提醒在设定当时就进提醒页**，
 *   到点由 reminder_sched 自己响（含"正在录音就只弹窗不响铃"的克制）。
 *
 * Input Parameters:
 *   hhmm           - 24 小时制 "H:MM" / "HH:MM"（"8:00" 和 "08:00" 都收）
 *   title          - 提醒做什么，按整字截到 20 字
 *   used_title     - 可选输出：**实际用上的**标题（已截断）。调用方要把标题
 *                    写进给模型看的话术时必须用这个，别用原文 —— 原文可能有
 *                    256 字节，会把 384 字节的工具结果缓冲撑爆，截断了的错话
 *                    会被模型原样念给老人听。不关心就传 NULL。
 *   used_title_cap - used_title 的大小（至少 84，容纳 20 个汉字的 UTF-8）
 *
 * Returned Value:
 *   OK 成功；
 *   -EINVAL   hhmm 不是 24 小时制 HH:MM，或 title 为空；
 *   -ENOSYS   本机没注册提醒落地实现；
 *   其它负值  hook 的失败码（例如提醒条数已满），**原样透传**给调用方说清楚。
 *
 ****************************************************************************/

int mimo_reminder_apply(const char *hhmm, const char *title, char *used_title,
                        size_t used_title_cap);

/****************************************************************************
 * Name: adjust_setting 工具（语音改音量/亮度）的接口
 *
 * Description:
 *   用户说「声音小一点」「把亮度调到最亮」这类话时，模型会调 adjust_setting
 *   工具，板子把 (item, action, value) 交给 app 注册进来的 mimo_settings_hook_t
 *   真正改设置 —— 和在设置页拖滑块走**同一条路**（改 user_settings + 真下发
 *   硬件 + 存盘），所以语音调完再打开设置页，滑块就停在新值上。
 *
 *   与 add_reminder 同构：两条对话链路（mimo_chat 那条 + ai_tools_provider
 *   那条）共用 mimo_settings_apply() 这一个落地入口，不会漂移。
 *
 *   ★ 这些常量是**跨 app 的协议**，实现方（app/robot_ui/touch_ui.c 的
 *     touch_ui_settings_apply）也用它们，改值要两边一起改。
 *
 ****************************************************************************/

#define MIMO_SET_ITEM_VOLUME     0  /* 音量 */
#define MIMO_SET_ITEM_BRIGHTNESS 1  /* 亮度 */

#define MIMO_SET_ACT_UP    0  /* 调高一档 */
#define MIMO_SET_ACT_DOWN  1  /* 调低一档 */
#define MIMO_SET_ACT_SET   2  /* 设成 value（0..100）*/

/****************************************************************************
 * Name: mimo_set_settings_hook
 *
 * Description:
 *   注册「替用户改音量/亮度」的实现。fn 传 NULL = 注销。
 *   **没注册时 adjust_setting 工具根本不出现在 tools 表里** —— 模型看不到它，
 *   也就不会调一个没人实现的工具（宁可让它老实说"改不了"）。
 *
 *   fn 的约定：
 *     item   - MIMO_SET_ITEM_VOLUME / MIMO_SET_ITEM_BRIGHTNESS；
 *     action - MIMO_SET_ACT_UP / _DOWN / _SET；
 *     value  - 只在 action==MIMO_SET_ACT_SET 时有意义，0..100。
 *   返回 >=0 = **已经生效的新值**（0..100，到顶/到底也如实回当前值）；
 *   负 errno 会被当作工具结果喂回模型，让模型自己跟用户解释。
 *
 *   注意：fn 在**语音工作线程**里执行（不是 LVGL 线程），里面不能碰 LVGL
 *   控件；要刷界面请自己用 lv_async_call() 投过去。
 *
 ****************************************************************************/

typedef int (*mimo_settings_hook_t)(int item, int action, int value);

void mimo_set_settings_hook(mimo_settings_hook_t fn);

/****************************************************************************
 * Name: mimo_settings_apply
 *
 * Description:
 *   「语音改音量/亮度」的**唯一落地入口**。两条对话链路的 adjust_setting
 *   工具都走这里，参数解析 / 落地 / hook 缺失兜底只写一份。
 *
 * Input Parameters:
 *   item   - MIMO_SET_ITEM_VOLUME / MIMO_SET_ITEM_BRIGHTNESS；
 *   action - MIMO_SET_ACT_UP / _DOWN / _SET；
 *   value  - 只在 action==MIMO_SET_ACT_SET 时有意义，会被夹到 0..100。
 *
 * Returned Value:
 *   >=0    已生效的新值（0..100）；
 *   -EINVAL 参数不对（item / action 越界）；
 *   -ENOSYS 本机没注册落地实现；
 *   其它负值 hook 的失败码（例如背光驱动不认），**原样透传**。
 *
 ****************************************************************************/

int mimo_settings_apply(int item, int action, int value);

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

/* ===== 切块上限：调用方按"首块 / 后续块"分别传进 mimo_tts_speak_head() =====
 *
 * 两个值定义在这里（不是 mimo_voice.c）是因为它们是**调用方要选的旋钮**：
 * 首块要小（早出声），后续块要大（块间空档少）。2026-10-25 之前后续块上限
 * 写死在 mimo_voice.c 里、走的是首块那个小值，改宏是空操作 —— 详见
 * mimo_voice.c 里"切块上限的设计史"那一大段真机实测。
 *
 * 两个值都是**上限**：tts_split_len() 优先切在句子边界，实际块一般更小。
 * min_chars = max/2，所以首块最短 10 字、后续块最短 32 字，不会切出碎块。 */

/* 后续块上限：大一点，块与块之间的空档少、断句落在句号上而不是逗号上。
 * 嫌停顿多就调大；代价是被打断时浪费的音频更多。
 *
 * ★ 2026-11-01：**64 → 32**。64 字那一档真机跑出过 140 字节（46 个汉字）的块，
 *   MiMo 返回的 base64 音频超过 MIMO_TTS_RESP_CAP（896 KB）被截断，代码判成
 *   硬错 → 粘性降级到火山 TTS → 火山 403（这个 key 没开 TTS）→ **整机彻底无声**。
 *   896 KB 的响应缓冲是按"一块 24 字"设计的；32 字这一档实测响应约 450~600 KB，
 *   留足余量。想再调大必须同时核算 MIMO_TTS_RESP_CAP：base64 膨胀 4/3，
 *   按最坏 ~14 KB/字算，上限 ≈ (896K × 3/4) / 14K ≈ 48 字。 */
#define MIMO_TTS_CHUNK_CHARS 32

/* 首块上限：比后续块小，只为让第一声早点出来（"回复快"优先级高于"顺滑"）。 */
/* ★ 2026-11-02：20 → **10** 字。
 *   TTS 首块是"回复到手到出声"的最后一段阻塞。火山 TTS 合成 20 字约 5 秒，
 *   10 字约 2~3 秒 —— 首声提前 2~3 秒，总延迟从 ~24s 降到 ~21s。 */
#define MIMO_TTS_HEAD_CHUNK_CHARS 10

/****************************************************************************
 * Name: mimo_tts_speak_head
 *
 * Description:
 *   只合成回复的**一块**（按句子边界切，字数上限由 max_chars 给），给
 *   "边播边合成"的流水线用（2026-10-19 用户反馈"回复太慢"：原来要等整条
 *   回复合成完才出声，两三秒的等待被感知成七八秒）。
 *
 *   ⚠ 名字叫 speak_head，但流水线**每一块都调它**（首块 + 后续块）。所以
 *   必须区分传值：
 *     - 第 1 块：max_chars = MIMO_TTS_HEAD_CHUNK_CHARS（小，早出声）
 *     - 后续块：max_chars = MIMO_TTS_CHUNK_CHARS（大，少空档）
 *
 *   典型用法（见 ai_companion_main.c 的 llm_complete_callback）：
 *     1. mimo_tts_speak_head(全文, buf, cap, HEAD, &len, &consumed);
 *     2. audio_play_start_ref(buf, len) 立刻开始出声；
 *     3. 播放的同时，生产者循环对 text+consumed 继续调本函数（max_chars 传
 *        CHUNK），把后续块写进**另外两个缓冲槽**（三槽轮转，队列深度 2）；
 *     4. 每块播放完成回调里取下一块接着播，队列空了才算整轮收尾。
 *
 *   ⚠ 顺滑的天花板：云端合成比实时慢约 20%（实测 235 ms/字 合成 vs 196
 *   ms/字 播放），所以块间必有静音，调块大小只能把它从"每 4 秒断一次"
 *   变成"每 12 秒断一次"，压不到 0。剩下每块 ~800 ms 的 TLS 握手要靠连接
 *   复用才能省，而复用被 vela_tls.c 的排空判定挡死，需要单独授权才能动。
 *
 * Input Parameters:
 *   text          - 待合成文本（UTF-8，从句首开始）；
 *   pcm_out       - 输出缓冲（调用方的 TTS 常驻缓冲某一槽）；
 *   pcm_cap       - 缓冲大小；
 *   max_chars     - 本块最多几个字（传 MIMO_TTS_HEAD_CHUNK_CHARS 或
 *                   MIMO_TTS_CHUNK_CHARS）；
 *   pcm_len       - 输出：本块 PCM 字节数；
 *   consumed_bytes - 输出：本块消耗的 text 字节数（调用方据此推进游标）。
 *
 * Returned Value:
 *   OK on success; a negated errno (-EINVAL 参数 / -ENOENT 未配置 /
 *   -ENOSPC 槽一个字都装不下 / -EIO 网络失败)。
 *
 ****************************************************************************/

int mimo_tts_speak_head(const char *text, unsigned char *pcm_out,
                        size_t pcm_cap, size_t max_chars, size_t *pcm_len,
                        size_t *consumed_bytes);

/****************************************************************************
 * Name: tts_text_sanitize
 *
 * Description:
 *   出声前的文本清洗，原地去掉 TTS 会照着念出来的格式记号：
 *     - Markdown（**粗体**、*斜体*、行首 `#`、行首 `- `）
 *     - JSON 整块（工具结果 `{"ok":false,"error":{...}}` 被模型抄进回复时，
 *       不再一个字符一个字符念出来）
 *
 *   **进 TTS 流水线之前必须过一遍**。两条对话链路共用这一份（mimo_voice.c
 *   本来就一份），别在别处再写一份清洗逻辑。
 *
 *   只删不加，结果长度 <= 原长度。整个文本都是机器格式时会清成空串 ——
 *   「空回复」的兜底由调用方自己管，本函数不替它编一句话。
 *
 * Input Parameters:
 *   s - NUL 结尾字符串，原地改写；NULL 直接返回
 *
 ****************************************************************************/

void tts_text_sanitize(char *s);

#endif /* __APP_HELLO_APP_MIMO_VOICE_H */
