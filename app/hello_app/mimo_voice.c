/****************************************************************************
 * app/hello_app/mimo_voice.c
 *
 * 小米 MiMo 云端后端（ASR + TTS + 对话 chat），普通 HTTPS POST 实现。
 *
 * 为什么另写一个后端：
 *   packages/ai_agent/src/voice/ 下的 volc_asr.c / volc_tts.c 是上游 openvela
 *   的实现（ASR 自搓 WebSocket 二进制协议，TTS 走 V3 单向流），凭据要求火山
 *   引擎的 appkey / token。本工程的凭据是 token-plan 的 MiMo key，所以这里按
 *   voice_asr_ops_t / voice_tts_ops_t 的接口另写一份，用 mimo_asr_register() /
 *   mimo_tts_register() 挂进同一个分发层（voice_asr.c / voice_tts.c），
 *   上游那些文件一行都不用改。
 *
 * 接口（和 LLM 同一个 host + path，只有 model 不同；PC 上实测 200）：
 *   ASR: POST <path>
 *        {"model":"mimo-v2.5-asr","messages":[{"role":"user","content":[
 *           {"type":"input_audio","input_audio":
 *              {"data":"<base64(整个 WAV，含 44 字节头)>","format":"wav"}}]}]}
 *        -> choices[0].message.content
 *   TTS: POST <path>
 *        {"model":"mimo-v2.5-tts","messages":[{"role":"assistant",
 *           "content":"<要合成的文本>"}]}
 *        （**必须 assistant 角色**；写成 user 服务端会报
 *          "messages must contain an assistant role for TTS model"）
 *        -> choices[0].message.audio.data = base64(WAV)
 *           这个 WAV 是 24 kHz / 单声道 / 16 bit，而本工程整条链是
 *           16 kHz / 单声道 / s16le，所以这里线性插值重采样到 16k 再交出去。
 *   对话: mimo_chat()（给别的 app 用的同步接口，不经过 ai_agent 进程）
 *        请求体在运行时拼，带 3 轮上限的工具循环；两个工具各自独立开关
 *        （add_reminder 默认开、get_weather 默认关，见 MIMO_CFG_KEY_*）：
 *        两个都关（默认单轮）
 *        {"model":"<配置里的 model>",
 *         "messages":[{"role":"system","content":"<当前时间 + 用户所在城市
 *                     + 说话要求>"},
 *                     {"role":"user","content":"<转义文本>"}],
 *         "max_tokens":1024}
 *        "用户所在城市"是板子自己用 IP 定位查的（mimo_location.c，结果缓存
 *        6 小时）；查不到那一段就不写，模型也就不会编一个城市。问天气时
 *        这个城市优先于配置里的 default_city（提示里让它用的城市和板子真去
 *        查的城市必须是同一个）。
 *        至少开一个时多一段 tools（数组里的成员按开关拼，见 MIMO_TOOL_*_JSON）：
 *        "tools":[{"type":"function","function":{"name":"add_reminder",...}},
 *                 {"type":"function","function":{"name":"get_weather",...}}]
 *        -> finish_reason="tool_calls" 时，取 message.tool_calls[0] 的
 *           function.name / .arguments 按名字分派：
 *             add_reminder(time,title) -> 交给 app 注册的
 *               mimo_set_reminder_hook() 实现落地（robot_ui 那边写进提醒列表
 *               并重挂 RTC 闹钟），工具结果是"已建好：每天 08:00 提醒用户吃药"；
 *             get_weather(city) -> 板子替它打 open-meteo 查
 *               （geocode + forecast 两个免 key 的 HTTPS GET），工具结果是
 *               中文实况；
 *           把 {"role":"assistant","content":null,"tool_calls":[...]} 和
 *           {"role":"tool","tool_call_id":"<id>","content":"<工具结果>"}
 *           两条消息追加进 messages 再发一轮，让模型用口语复述给用户；
 *        -> 模型不问工具时第 1 轮就出正文。
 *           choices[0].message.content（**只取 content**，reasoning_content
 *           是推理模型的思维链）
 *        为什么要有这个循环：模型不知道实时天气、也没有改板子状态的手，
 *        不把工具结果喂回去，"今天天气怎么样"就只能瞎编，"八点提醒我吃药"
 *        就只能回一句"我记住了"（实际什么都没发生）。
 *        天气默认关掉是因为 open-meteo 的实况和用户所在地经常对不上（还不如
 *        不说），而且多一轮请求更慢；"今天几号 / 现在几点"靠 system prompt
 *        里的时间就能答，不需要工具。
 *
 * 凭据：
 *   从 claw_config_get() 读 llm_host / api_key / model（开机由板级代码把
 *   /etc/assets/agent_config.json 拷进 /data/ai_agent/config/config.json）。
 *   本文件没有硬编码 key；日志只报长度 / 状态 / host / model，绝不打
 *   Authorization 头或 base64 音频（出错时才打响应体前 200 字节）。
 *
 * 内存：
 *   请求体、响应体、编解码缓冲全部走堆（大块分配会落到 PSRAM），**用完立刻 free**。
 *   每次 malloc/calloc/free 都会打一条带字节数的日志（mimo_alloc_logged 等），
 *   上板串口日志里可以直接对着看每步的块大小和峰值。
 *   三处的峰值（一轮对话里依次发生，不是同时）：
 *     ASR   : pcm(调用方) + wav(44+n) + body(约 1.34*n) + resp(16 KB)
 *     chat  : msgs(约 10 KB，system+user+每轮追加的 tool_calls/tool 两条)
 *             + body(约 msgs + 1 KB) + resp(64 KB)；问天气时中间还会过两个
 *             8 KB 的天气响应（用完立刻还，不叠加）；IP 定位最多一次 4 KB
 *             的响应，而且只在缓存过期 / 退避到期时才打（见 mimo_location.c）
 *     TTS   : body(不到 2 KB) + resp(MIMO_TTS_RESP_CAP，**一块**的量，就地
 *             base64 解码 → 重采样直接追加写进调用方的 PCM 缓冲，不再额外
 *             要 decoded / pcm16 两块)
 *   TTS 是**分块**合成的（一次送 MIMO_TTS_CHUNK_CHARS 个字），所以响应缓冲
 *   只要装得下一块就行；整段能念多少字由 MIMO_TTS_TEXT_MAX 和调用方的 PCM
 *   缓冲一起决定，见宏注释和 mimo_voice.h 里给调用方的缓冲建议。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "mimo_voice.h"

#include "agent_compat.h"
#include "agent_config.h"
#include "infra/config_store.h"
#include "infra/vela_tls.h"
#include "voice/voice_asr.h"
#include "voice/voice_tts.h"

#include "mbedtls/base64.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 后端名字（voice_asr_set_backend("mimo") / voice_tts_set_backend("mimo")） */

#define MIMO_BACKEND_NAME "mimo"

#define MIMO_ASR_MODEL    "mimo-v2.5-asr"
#define MIMO_TTS_MODEL    "mimo-v2.5-tts"

/* host / path 都从配置读（和 LLM 共用）；这两个只是兜底默认值，
 * 配置里没有 llm_path / llm_port 时用得上。host 没有默认值：
 * 读不到 llm_host 直接返回错误，绝不硬编码任何 host/key。 */

#define MIMO_DEFAULT_PATH "/v1/chat/completions"
#define MIMO_DEFAULT_PORT "443"

/* 整条语音链路的格式：16 kHz / 单声道 / 16 bit */

#define MIMO_PCM_RATE     16000
#define MIMO_PCM_CHANNELS 1
#define MIMO_PCM_BITS     16
#define MIMO_WAV_HDR_LEN  44

/* ASR 响应很小（就是一段文字）；TTS 响应是一条 base64 音频。
 * 实测（PC 上真打这个接口）：13 个汉字的 MiMo TTS 回包 143907 字节
 * （base64 143420 字符 -> WAV 107564 字节 = 24 kHz 的 2.24 秒），
 * 也就是每字约 11 KB base64。
 *
 * **一次不送整段**：两三百字的回复，整段响应是 2~3 MB，光这一块就把 8 MB
 * 用户堆吃掉三分之一，再叠上调用方的 PCM 和 ai_audio 的播放缓冲（见下面），
 * 必炸。所以按 MIMO_TTS_CHUNK_CHARS 个字一块、一块一块地合成，base64 就地
 * 解码、重采样后**追加**写进调用方的 PCM 缓冲（各块顺序拼起来 = 整段音频）。
 * 响应缓冲只要装得下**一块**：
 *
 *   MIMO_TTS_RESP_CAP = 896 KB，一块最多 24 字 × 约 11 KB/字 ≈ 264 KB，
 *   余量很足（896 KB 是按当年 64 字/块定的，块切小后就没再缩 —— 留着当
 *   免疫：每个字的实际音频长度跟语速有关，留够免得响应被 cap 截掉）。
 *
 * 块尽量切在一句话的末尾（tts_split_len 优先在句末标点处切），所以拼接处的
 * 停顿听起来是自然的。代价是整段要分几次 HTTPS（每次一次握手 + 一次合成），
 * 比一次送完慢几秒 —— 换来的是"整段都念得完"。
 *
 * 峰值：resp(896 KB) + body(不到 2 KB) + 调用方 PCM 缓冲（几 MB 的那块在
 * 调用方手里，见 mimo_voice.h 的缓冲建议）。
 * 以前是 resp(cap) + decoded(cap*3/4) + pcm16(完整时长) 三层叠在一起，
 * 现在 base64 就地解码（省掉 decoded）、重采样直接写进调用方的 PCM 缓冲
 * （省掉 pcm16）。 */
#define MIMO_ASR_RESP_CAP (16 * 1024)
#define MIMO_TTS_RESP_CAP (896 * 1024)

/* 一次最多念多少个字（UTF-8 码点，不是字节）。超过就只念前这么多，剩下的
 * 字数和原因打到串口日志上（见 mimo_tts_synthesize），对话区显示的回复
 * 不受影响（那边是另一份完整文本）。
 *
 * 300 字 ≈ 60 秒音频 ≈ 2 MB PCM：调用方的 TTS 缓冲和 ai_audio 的播放缓冲
 * 都要有这么大才装得下（数字见 mimo_voice.h 里给 robot_ui 的建议）。
 * 用户原来报的"回复没读完就停了"，主因是调用方只有 8 秒缓冲（约 35 字），
 * 这个上限保证常规回复（几十到两三百字）能整段合出来。 */
#define MIMO_TTS_TEXT_MAX 300

/* ===== 一块送多少个字 —— 切块上限的设计史 =====
 *
 * 两个上限宏 MIMO_TTS_CHUNK_CHARS（后续块）/ MIMO_TTS_HEAD_CHUNK_CHARS（首块）
 * **定义在 mimo_voice.h**，因为 2026-10-25 查明它们必须由调用方按"首块 / 后续块"
 * 分别传进 mimo_tts_speak_head() 的 max_chars 参数。
 *
 * ★★ 2026-10-25 修的坑：MIMO_TTS_CHUNK_CHARS 原来写死在本文件，注释还写着
 * "后续块在 mimo_tts_synthesize 的循环里用"。**那句话是错的** —— 流水线每一块
 * （含后续块）都调 mimo_tts_speak_head()，一块都没走过 mimo_tts_synthesize()。
 * 结果是改那个宏纯属空操作：真机块大小一动不动，还是卡在 20 字。日志为证
 * （/tmp/c64.log，宏改成 64 之后）：最大块 60 字节 = 20 个汉字，正好撞死在
 * MIMO_TTS_HEAD_CHUNK_CHARS 上。注释说的和代码做的不是一回事，以代码为准。
 *
 * 这些是**上限**：切块优先切在一句话的末尾（见 tts_split_len），所以实际块
 * 一般比这个小。
 *
 * ★ 2026-10-25 两次改，第二次推翻了第一次，依据是真机实测：
 *
 * 【第一次 64 → 24】看的是「首块大小 → 首句合成耗时」，那组数是对的：
 *     70 字节 →  6047 ms      152 字节 → 13656 ms
 *    100 字节 →  9428 ms      174 字节 → 13820 ms
 *    134 字节 → 11787 ms      188 字节 → 15282 ms
 *   （约 78 ms/字节 + ~800 ms TLS 握手）。首块切小确实让出声变快，这一步没白做
 *   —— 它由 MIMO_TTS_HEAD_CHUNK_CHARS 单独保住，本宏不再背这个锅。
 *
 * 【第二次 24 → 64】当初的警告「首块小必须配套把后续块也切小」是**错的**，
 *   错在只看了合成耗时、没算上真实的静音。2026-10-25 晚实测（同一份日志里
 *   拿音频设备 hw_stop→hw_start 量，不是拿"块间空档"那行量）：
 *
 *       块 1→2  日志报 1360 ms，真实静音 3410 ms
 *       块 2→3  日志报  770 ms，真实静音 2200 ms
 *       块 3→4  日志报  710 ms，真实静音 2200 ms
 *
 *   日志那行只量"队列等了多久"，没算上每块一次的设备关/开（约 600 ms）
 *   与排队。**耳朵听到的是真实静音那一列** —— 说话和静音差不多各占一半。
 *
 *   按「合成 = 800 ms 固定 + 235 ms/字，音频 = 196 ms/字」反推静音占比：
 *       12 字 → 54%    24 字 → 37%    64 字 → 26%    128 字 → 23%
 *   64 之后收益递减，而且回合被打断时浪费的音频更多，所以停在 64。
 *
 *   ★ 硬约束先说清，别指望调这个调到 0：云端合成比实时慢约 20%（235 vs
 *   196 ms/字），光靠加块只能把静音压到 ~20%。
 *
 *   ★★ 2026-10-30 更正上面两段里两处**已经过时/本来就不准**的说法，
 *   以真机日志为准（/home/xiaoyingyu/board_boot.log，21 块逐块算过）：
 *
 *   ① 「复用被 vela_tls.c 的排空判定挡死、需要单独授权」—— **不成立了**。
 *      根因早已在 vela_tls.c 里修掉（排空只认 WANT_READ/WANT_WRITE 为活着，
 *      其余一律 pool_reconnect），本文件 944 行那段把补丁撤掉了，轮内复用
 *      **确实在生效**：日志里能看到 `[vela_tls] Reusing pooled connection`，
 *      且复用块的固定开销确实低于清池块。上面这句留着会把后来人引到错的方向。
 *
 *   ② 「每块固定开销 ≈ 800 ms，全是 TLS 握手」—— **归因错了**。逐块实测
 *      「合成墙钟 − 本块音频时长」：
 *          清池后（每轮首块）9 块，平均 2071 ms
 *          轮内复用      12 块，平均 1347 ms
 *      两者之差只有 ~720 ms —— **握手只占固定开销的三分之一强**，剩下那
 *      1350 ms 是请求往返 + 服务端每请求开销 + 本地 base64 解码/重采样。
 *      所以「砍握手」只能把每轮首声提前 ~0.7 秒，砍不到 2 秒。真想再快
 *      要么走 token 流式（需对 vela_tls.c 扩权），要么砍本地/服务端那部分
 *      —— 2026-10-30 加了拆解计时（POST 往返 vs 本地）就是为了把这 1350 ms
 *      归到实处，别再猜。
 *
 * 想再快出声就调小 MIMO_TTS_HEAD_CHUNK_CHARS；嫌停顿多就调大
 * MIMO_TTS_CHUNK_CHARS。两个宏的值在 mimo_voice.h。 */

/* 估算一个字念出来占多少字节 PCM（16k 单声道 16bit）：实测 13 字 = 107564
 * 字节的 24 kHz WAV，转成 16 kHz 约 71700 字节 ≈ 5.5 KB/字（那一段语速偏快，
 * 约 5.8 字/秒）；按常见语速 4.6 字/秒算是 7 KB/字。这里取 7 KB 做**保守**
 * 估算，只用来在合成前算"调用方的缓冲还放得下几个字"，宁可少念一个字，
 * 也绝不写到缓冲外面。 */
#define MIMO_TTS_PCM_PER_CHAR 7000

/* 应答里最多往串口打多少字节（只在出错时打） */

#define MIMO_ERR_DUMP_LEN 200

/* ASR 请求的 JSON 骨架：base64 的 WAV 直接填在中间的空档里（不经过 cJSON，
 * 省掉一整份字符串拷贝 —— hello_app 的编译参数里也没有 cJSON 的 include 路径）。
 *
 *   {"model":"<asr_model>","language":"<asr_language>",
 *    "messages":[{"role":"user","content":[{"type":"input_audio",
 *                "input_audio":{"data":"<base64 整个 WAV>"}}]}]}
 *
 * model / language 都改成从配置读（2026-10-28，计划 S4.4）：
 *   asr_model     缺省 MIMO_ASR_MODEL
 *   asr_language  缺省 "zh"；配成字面 "none" 则**整个 language 键不发**
 * 为什么值得改：换模型 / 换网关只要改配置，不用重烧（改配置走 ROMFS 那条
 * agent_config.json 流程，见 sifli_ap.c 的 sf32lb52_install_agent_config）。
 *
 * ── 关于 language 字段：两次 PC 直连实测（2026-10-28）────────────────
 *
 *   ① **这字段是摆设**。同一段真中文语音（ROMFS 里的 TTS 缓存，16 kHz 裸
 *      s16le），language 取 zh / zh-CN / zh-Hans / yue / yue-CN / wuu / nan /
 *      sd / auto / 空串 / **zz-ZZ 故意乱写** / 整个键不发 —— 转写结果**逐字
 *      相同**。再把信号压到 45% 并叠强噪声重测（怕"语音太清楚显不出偏置"），
 *      zh / yue / zz-ZZ / 不发 四个取值依然逐字相同，重复两次也无随机性。
 *      → **想靠它开方言，实测没用**，别再试了。
 *
 *   ② **网关也只有这一个 ASR 模型**。GET /v1/models 共 6 个：
 *      mimo-v2.5-asr / mimo-v2.5-tts / mimo-v2.5-tts-voiceclone /
 *      mimo-v2.5-tts-voicedesign / mimo-v2.6-flash / mimo-v2.6-pro。
 *      试过拿 mimo-v2.6-pro 当 ASR 用（网关是"注入转写提示"的通用 chat 端点，
 *      语法上能通）：**是描述不是转写**（"清晨的太阳形状像一朵花，被金色的
 *      光晕环绕…"），而且中位墙钟 5470~15281 ms（2.5-asr 只要 685~834 ms，
 *      最慢一次 54.7 秒）。**不能当 ASR 用**，别被某一次"转写对了"骗过去。
 *
 *   ③ 顺带排掉一个坑：换 model 时 TTS 那三个会 400 "Param Incorrect"，
 *      flash 会收音频但答非所问 —— 只有 mimo-v2.5-asr 是真 ASR。
 *
 * ── 历史（2026-10-14 加 language:"zh"，动机仍然成立，别拿掉）────────
 *   不钉语言时网关自带的转写提示不做语种约束，老人中文口音/环境噪声下偶尔
 *   整句被识别成英文（用户实测反馈）。所以**缺省仍然钉 "zh"**。
 *   想塞 text 提示段这条路是死的 —— PC 实测网关 400 拒绝："ASR request must
 *   not include text parts; text prompt is injected by the gateway"。 */

/* 前缀运行时拼（model / language 要从配置读）。长度是算出来的：
 *   固定部分 109 字节 + model 63 + 语言段(引号冒号逗号 14 + 31) + NUL = 238，
 *   取 256。写小了会被 -Wformat-truncation 抓住（这个库的该告警都是真截断）。 */
#define MIMO_ASR_JSON_PFX_CAP  256

/* asr_language 配成这个字面值 = 不发 language 键。
 * 为什么用哨兵：claw_config_get() 对「键不存在」和「空串」都返回 ERROR
 * （config_store.c:117 卡 valuestring[0] != '\0'），两种情况区分不开。 */
#define MIMO_ASRLANG_NONE      "none"

#define MIMO_ASR_JSON_SUFFIX "\",\"format\":\"wav\"}}]}]}"

#define MIMO_TTS_JSON_PREFIX                                             \
  "{\"model\":\"" MIMO_TTS_MODEL "\",\"messages\":[{\"role\":"          \
  "\"assistant\",\"content\":\""

#define MIMO_TTS_JSON_SUFFIX "\"}]}"

/* 对话（chat/completions）：和 ASR/TTS 共用 host / path / api_key，只有
 * model 不同。请求体在运行时拼（model 从配置的 "model" 键读，不写死）：
 *   {"model":"<model>",
 *    "messages":[{"role":"system","content":"<时间 [+ 用户所在城市 + 默认城市]
 *                + 说话要求>"},
 *                {"role":"user","content":"<转义文本>"},
 *                ... 开了工具才有后续轮次：assistant(tool_calls) / tool ...],
 *    "tools":[{...get_weather...}],        <- enable_weather_tool 才带这段
 *    "max_tokens":1024}
 * "用户所在城市"是 mimo_location.c 的 IP 定位结果（查不到就不写这一段），
 * 问天气时它优先于配置里的 default_city。
 * 默认不带 tools（用户反馈查出来的天气不准），messages 就 system + user 两条，
 * 一轮就完事；要开就在配置里把 enable_weather_tool 设成 1。
 * max_tokens 给 1024 —— 这个模型是推理模型，思维链会吃掉一部分 token，
 * 给太小正文（content）就会是空的。
 * 开了工具时，模型要查天气会先回 finish_reason="tool_calls"（正文为空），
 * 板子替它查完再把结果作为 role:"tool" 的消息追加进去问一轮；最多
 * MIMO_CHAT_MAX_ROUNDS 轮。
 * 应答只取 choices[0].message.content：reasoning_content 是思维链，不要。
 * 响应是纯文本 JSON（含思维链），几十 KB 足够；缓冲走堆。 */

/* 请求体的三段：前缀（含 model）+ messages 数组 + 尾巴（tools 表 + max_tokens）。
 * messages 由 mimo_chat() 拼好放在中间，见那里的 sjoin_add*。
 * 尾巴分两份：带 tools 的 / 不带的 —— 不能用一份再删，否则不是多一个逗号
 * 就是留下空的 "tools":[]，服务端 json 解析直接报错。 */

#define MIMO_CHAT_JSON_FMT "{\"model\":\"%s\",\"messages\":["

/* tools 表：两个工具。这里每个宏只是**一个函数对象**（不带数组方括号），
 * 四种开关组合在 mimo_chat 里各选一段字面量：`"[" A "," B "]"` 之类的相邻
 * 字符串常量由**编译器**拼成一整条（见 mimo_chat 里的 tools_json）。不再运行
 * 时拼装 —— 那个写法留过一次缓冲溢出的前科（见下面「tools 数组**没有缓冲**了」
 * 那段注释）。"拼了再删"的老写法同样不行：删不干净就是尾逗号，服务端直接报错。
 * description 是写给模型看的，里面的括号 / 逗号 / 引号一律用中文全角 ——
 * 这是手写的 JSON 字面量，一个 ASCII 引号或反斜杠就能把它拼坏。 */

#define MIMO_TOOL_WEATHER_JSON                                           \
  "{\"type\":\"function\",\"function\":{\"name\":\"get_weather\","      \
  "\"description\":\"查询中国某个城市的实时天气：天气现象、气温、体感温度、"                 \
  "湿度、风速。用户问天气、气温、冷不冷、热不热、要不要带伞时调用。\","                      \
  "\"parameters\":{\"type\":\"object\",\"properties\":{\"city\":"         \
  "{\"type\":\"string\",\"description\":\"中文城市名，例如 北京/上海/深圳；"             \
  "用户没说城市时用默认城市\"}},\"required\":[\"city\"]}}}"

/* add_reminder：口语时间由模型负责折算成 24 小时制 —— system prompt 里带着
 * 当前日期时间，所以"明天早上八点"它能算对（见 chat_build_system_prompt）。
 *
 * ★ 2026-10-30 补防谎报硬话，和 adjust_setting（下面那个）同一套写法：
 *   先说「没调用就说是骗他」，再说「调用成功才是真建好了」。真机抓到过它
 *   一次工具都不调（`tool_use=0, calls=0`）就答「提醒已经设好了」，原来那
 *   句"调用成功就真的建好了，可以直接告诉用户已经设好"等于给了它许可。
 *   面板语音这条链路（mimo_chat）和常开麦那条（ai_tools_provider.c 的
 *   AI_TOOLS_ADD_REMINDER_JSON）是**两份独立的工具清单**，改一份漏一份就是
 *   「面板能防、常开麦照样谎」，所以两份都改了。
 *   说到底这仍是赌模型老实；不依赖模型的那条是 ai_companion_main.c 的
 *   reminder_intent_try() 本地快路（本文件的 mimo_reminder_apply 是它的落点）。 */

#define MIMO_TOOL_REMINDER_JSON                                          \
  "{\"type\":\"function\",\"function\":{\"name\":\"add_reminder\","      \
  "\"description\":\"给用户建一条每天重复的提醒。用户说让我几点几分提醒他做"           \
  "某件事时调用，例如明天早上八点提醒我吃药、下午三点提醒我量血压。"                    \
  "时间必须你自己换算成 24 小时制的 HH:MM 再传：早上八点是 08:00，"                  \
  "下午三点是 15:00，晚上八点是 20:00。「晚上十二点」是 00:00 不是 12:00，" \
  "「夜里两点」是 02:00 不是 14:00。"                                       \
  "**没调用本工具就对用户说「已经设好了」「到点会响」是骗他** —— 他到点没"   \
  "听见响，只会以为这东西坏了，事实上一条提醒都没有。必须先调用本工具，拿到"   \
  "执行结果里那句「add_reminder 成功」，再照着它给的时刻和事项告诉用户。"     \
  "反过来：**调用成功就是真建好了**（进板子提醒页、到点响铃弹窗），可以放心"   \
  "复述时刻和事项。\","                                  \
  "\"parameters\":{\"type\":\"object\",\"properties\":{"                  \
  "\"time\":{\"type\":\"string\",\"description\":\"24 小时制时刻，格式 "     \
  "HH:MM，例如 08:00 / 15:30。必须换算好，不要传明天早上这种词。\"},"             \
  "\"title\":{\"type\":\"string\",\"description\":\"提醒做什么，简短中文，"    \
  "例如 吃药 / 量血压 / 喝水 / 给儿子打电话\"}},"                             \
  "\"required\":[\"time\",\"title\"]}}}"

/* adjust_setting：语音改音量/亮度。用户说「声音小一点」「把亮度调到最亮」
 * 这类话时调用，**真的改设置**（和在设置页拖滑块同一条路，改完存盘），
 * 不是只回一句话。落地见 mimo_settings_apply()。 */

#define MIMO_TOOL_SETTING_JSON                                           \
  "{\"type\":\"function\",\"function\":{\"name\":\"adjust_setting\","    \
  "\"description\":\"调整本机的音量或者屏幕亮度。用户说声音太小、声音太大、"              \
  "调高音量、调低音量、把屏幕调亮、把屏幕调暗、看不清屏幕、太刺眼这类话时"                 \
  "调用。调用成功就是真的改好了，而且会存下来、断电重启也还在，"                          \
  "你可以放心把改完的数值告诉用户。item 传 volume 表示音量、brightness 表示"              \
  "屏幕亮度；action 传 up 表示调高一档、down 表示调低一档、set 表示直接设成"              \
  "value 那个数。用户说调到最亮、最大声就用 set 加 100，说关掉声音就用 set 加 0。"        \
  "**没调用本工具就对用户说「已经调好了」「现在是多少」是骗他** —— 他回头"                  \
  "一看根本没变。必须先调用本工具，拿到执行结果里返回的数值，再照着那个数说。\","            \
  "\"parameters\":{\"type\":\"object\",\"properties\":{"                  \
  "\"item\":{\"type\":\"string\",\"description\":\"改哪一项\","           \
  "\"enum\":[\"volume\",\"brightness\"]},"                               \
  "\"action\":{\"type\":\"string\",\"description\":\"怎么改\","          \
  "\"enum\":[\"up\",\"down\",\"set\"]},"                                 \
  "\"value\":{\"type\":\"integer\",\"description\":\"只在 action 是 set " \
  "的时候传，0 到 100 的整数。\"}},"                                       \
  "\"required\":[\"item\",\"action\"]}}}"

/* 默认（工具关）：messages 收尾 + max_tokens，没有 tools 字段 */

#define MIMO_CHAT_JSON_TAIL_NO_TOOLS "],\"max_tokens\":1024}"

/* 带工具时尾巴分三段拼：HEAD + 运行时拼好的 tools 数组 + END */

#define MIMO_CHAT_JSON_TAIL_TOOLS_HEAD "],\"tools\":"
#define MIMO_CHAT_JSON_TAIL_TOOLS_END  ",\"max_tokens\":1024}"

/* tools 数组**没有缓冲**了。
 *
 * ⚠️ 前科（真机 2026-10-28 抓到）：这里原来是
 *      #define MIMO_CHAT_TOOLS_BUF_MAX 1024
 *      char tools_json[MIMO_CHAT_TOOLS_BUF_MAX];
 *    运行时 snprintf 拼进去。注释还写着「两个工具对象的 JSON 大约 600 字节，
 *    1 KB 给足」—— 但 add_reminder 的 description 后来涨到 787 字节，
 *    实测 MIMO_TOOL_WEATHER_JSON 475 + MIMO_TOOL_REMINDER_JSON 787
 *    + [ , ] \0 = **1266 > 1024**，snprintf 判溢出直接 return -ENOMEM(-12)。
 *    后果是 velaclaw 一超时(-110)、兜底那条 mimo_chat() 也一起死，
 *    轮里只剩一句「我这边有点问题」—— 就是「听到了不回复」。
 *
 * 根子不是「缓冲开小了」，是**手写容量注释迟早过期**。所以改成一张写死的
 * 组合表（相邻字符串常量由编译器自动拼接），不再运行时拼装 —— 「装不下」
 * 这一类故障在结构上不存在了，也不用再维护那条注释。
 *
 * 各工具 JSON 的真实大小（★ 2026-10-29 **用编译器量的**，不是手数的：把这
 * 三个宏原样从本文件抽出来编译 sizeof/strlen，免得又手抄一份跟真身对不上）：
 *   MIMO_TOOL_WEATHER_JSON    439 B   （+NUL = 440）
 *   MIMO_TOOL_REMINDER_JSON   739 B   （+NUL = 740）
 *   MIMO_TOOL_SETTING_JSON    992 B   （+NUL = 993）
 *   组合表最长那条（111，三个都开）  2174 B （+NUL = 2175）
 *
 *   ⚠ 这几个数到 2026-10-30 又过期了（MIMO_TOOL_REMINDER_JSON 加了防谎报
 *   硬话，涨了约 200 字节）—— **故意没有跟着改**：改了就得再量一次，而
 *   "下一个人以为这些数是新的"比"这些数明显是旧的"更危险。真正要紧的是
 *   上面那句结论：结构上没有缓冲可卡，数只当量级看。
 *
 * ★ 这几行以前写的是 475 / 787 / 663 / 1930，**四个数全是过期的** ——
 *   连标着「实测」的那两个也是（字面量后来改过，数没跟着改）。这恰好就是
 *   上面那段说的「手写容量注释迟早过期」的现场证据。所以别再拿这些数去卡
 *   什么缓冲：现在结构上就没有缓冲可卡（写死的字符串常量，编译期定死），
 *   这些数只给读代码的人一个量级概念。
 */

#define MIMO_CHAT_RESP_CAP (64 * 1024)

/* 工具循环最多几轮：开了 get_weather 时正常问一次天气是 2 轮（第 1 轮要工具、
 * 第 2 轮给正文），多出来的 1 轮留给模型追问；到上限还在要工具就按 mimo_chat
 * 里的兜底走。工具关着时用不到这个上限 —— 模型手里没有工具，第 1 轮就给正文。 */

#define MIMO_CHAT_MAX_ROUNDS 3

/* get_weather 工具：open-meteo 的两个免 key 接口（都是 HTTPS GET）。
 * geocode 拿经纬度，forecast 拿实况；weather_code 是 WMO 码，由 wmo_text()
 * 在板子上映射成中文短句再喂给模型（模型不认识数字码）。
 * 注意 forecast 那个路径要过 snprintf：URL 里的 % 必须写成 %%，否则
 * "Asia%2FShanghai" 会被 printf 当成 %2F 转换符（编译器也会告警）。 */

#define MIMO_GEO_HOST     "geocoding-api.open-meteo.com"
#define MIMO_GEO_PATH_FMT "/v1/search?name=%s&count=1&language=zh"

#define MIMO_WEATHER_HOST "api.open-meteo.com"
#define MIMO_WEATHER_PATH_FMT                                            \
  "/v1/forecast?latitude=%s&longitude=%s&current="                       \
  "temperature_2m,apparent_temperature,relative_humidity_2m,weather_code," \
  "wind_speed_10m&timezone=Asia%%2FShanghai"

/* 天气响应就几百字节，8 KB 足够（截断了会解析失败，宁大勿小）；
 * 城市名先进 URL：中文一个字 percent-encode 成 9 个字符，256 字节能装 28 个汉字。 */

#define MIMO_WEATHER_RESP_CAP (8 * 1024)
#define MIMO_CITY_BUF_MAX     64
#define MIMO_CITY_URL_MAX     256

/* 用户没说城市时用哪个：先读配置库的 default_city，读不到用这个。
 * 只在 enable_weather_tool 开着（真的会用 get_weather）时才读。 */

#define MIMO_DEFAULT_CITY "济南"

/* 天气工具的开关（配置键，字符串 "1" / "true" / "on"，大小写不敏感；
 * 缺省、空串、别的值一律算关）。**默认关**：open-meteo 查出来的实况跟用户
 * 所在地常常对不上，用户宁可不要；关掉后请求体里没有 "tools" 字段，也就
 * 不会多跑工具循环。"今天几号 / 现在几点"不靠这个 —— 时间在 system prompt 里。 */

#define MIMO_CFG_KEY_WEATHER_TOOL "enable_weather_tool"

/* 建提醒工具的开关，键名 enable_reminder_tool。**默认开**（配成 "0" / "false" /
 * "off" 才关）：这是老人说"八点提醒我吃药"就要生效的功能，不该让用户先去改
 * 配置。真正决定它出不出现在 tools 表里的还有一条：robot_ui 有没有注册
 * mimo_set_reminder_hook()（没注册就没地方落地，工具干脆不给模型看）。 */

#define MIMO_CFG_KEY_REMINDER_TOOL "enable_reminder_tool"

/* 语音改音量/亮度工具的开关，键名 enable_setting_tool。**默认开**（配成
 * "0" / "false" / "off" 才关）。和 add_reminder 一样还有一条硬条件：必须有
 * app 注册了 mimo_set_settings_hook()，没注册就不出现在 tools 表里。 */

#define MIMO_CFG_KEY_SETTING_TOOL "enable_setting_tool"

/* add_reminder 的 title 最长留几个字（按 UTF-8 整字截，尾部补省略号）。
 * 提醒列表本身是 64 字节，20 个汉字 = 60 字节，正好放得下。 */

#define MIMO_REMINDER_TITLE_CHARS 20

/* 单次读进来的 WAV 文件上限（16k 单声道 16bit 的 32 秒） */

#define MIMO_WAV_MAX_FILE (1024 * 1024)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct wav_info_s
{
  unsigned int rate;
  unsigned int channels;
  unsigned int bits;
  size_t       data_off;
  size_t       data_len;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char *TAG = "mimo_voice";

/* 配置快照（每次调用前重新读一遍，配置改了不用重启） */

static char s_host[128];
static char s_key[128];
static char s_path[128];
static char s_port[8];
static char s_model[64];

/* ASR 的 model / language（S4.4）：从配置读，缺省就是原来的写死值，
 * 不配就和改动前逐字节一样。 */

static char s_asr_model[64];
static char s_asr_lang[32];

/* 建提醒的落地实现，由 robot_ui 在启动时用 mimo_set_reminder_hook() 注册。
 * 为 NULL 时 add_reminder 不出现在 tools 表里（见 chat_reminder_tool_enabled）。 */

static mimo_reminder_hook_t s_reminder_hook = NULL;

/* 语音改音量/亮度的落地实现，由 robot_ui 在启动时用 mimo_set_settings_hook()
 * 注册。为 NULL 时 adjust_setting 不出现在 tools 表里。 */

static mimo_settings_hook_t s_settings_hook = NULL;

/****************************************************************************
 * Name: mimo_set_reminder_hook
 *
 * Description:
 *   注册"替用户建一条提醒"的实现（声明与约定见 mimo_voice.h）。传 NULL 注销。
 *   只写一个函数指针，任何线程都能调；真正的回调发生在语音工作线程里。
 *
 ****************************************************************************/

void mimo_set_reminder_hook(mimo_reminder_hook_t fn)
{
  s_reminder_hook = fn;

  syslog(LOG_INFO, "[%s] 建提醒工具: %s\n", TAG,
         fn != NULL ? "已注册（add_reminder 可用）" : "未注册（工具不下发）");
}

/****************************************************************************
 * Name: mimo_set_settings_hook
 *
 * Description:
 *   注册"替用户改音量/亮度"的实现（声明与约定见 mimo_voice.h）。传 NULL 注销。
 *   只写一个函数指针，任何线程都能调；真正的回调发生在语音工作线程里。
 *
 ****************************************************************************/

void mimo_set_settings_hook(mimo_settings_hook_t fn)
{
  s_settings_hook = fn;

  syslog(LOG_INFO, "[%s] 改音量亮度工具: %s\n", TAG,
         fn != NULL ? "已注册（adjust_setting 可用）" : "未注册（工具不下发）");
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: mimo_alloc_logged / mimo_calloc_logged / mimo_free_logged
 *
 * Description:
 *   语音链路里的大块内存都从这里要、也从这里还，"每步要了多少、还了多少"
 *   直接打到串口上（上板复现内存峰值时对着看即可）。free 的字节数由调用方
 *   传 —— 这里不记账，只是把同一份数字打出来。
 *
 *   失败一定打 LOG_ERR（这是真正会把整轮对话搞挂的路径）。
 *
 ****************************************************************************/

static void *mimo_alloc_logged(const char *what, size_t bytes)
{
  void *p = malloc(bytes);

  if (p == NULL)
    {
      syslog(LOG_ERR, "[%s] 分配失败: %s %zu 字节\n", TAG, what, bytes);
    }
  else
    {
      syslog(LOG_INFO, "[%s] alloc  %-14s %zu 字节\n", TAG, what, bytes);
    }

  return p;
}

static void *mimo_calloc_logged(const char *what, size_t bytes)
{
  void *p = calloc(1, bytes);

  if (p == NULL)
    {
      syslog(LOG_ERR, "[%s] 分配失败: %s %zu 字节\n", TAG, what, bytes);
    }
  else
    {
      syslog(LOG_INFO, "[%s] alloc  %-14s %zu 字节\n", TAG, what, bytes);
    }

  return p;
}

static void mimo_free_logged(const char *what, void *p, size_t bytes)
{
  if (p == NULL)
    {
      return;
    }

  syslog(LOG_INFO, "[%s] free   %-14s %zu 字节\n", TAG, what, bytes);
  free(p);
}

/****************************************************************************
 * Name: utf8_char_len / utf8_count
 *
 * Description:
 *   utf8_char_len 取 src 开头那个 UTF-8 字符占几个字节（remain 是剩下的字节
 *   数）。非法首字节、或者多字节序列被截断在末尾，都按 1 个字节算 —— 吃掉
 *   一个脏字节，别把后面整串带偏。
 *   utf8_count 数整串有几个字（UTF-8 码点），两个版本：utf8_count_n 按给定
 *   长度数，utf8_count 按 '\0' 结尾数。日志里"截掉了多少字"就靠它。
 *
 ****************************************************************************/

static size_t utf8_char_len(const char *src, size_t remain)
{
  unsigned char c;
  size_t used = 1;

  if (src == NULL || remain == 0)
    {
      return 0;
    }

  c = (unsigned char)src[0];

  if (c < 0x80)
    {
      used = 1;
    }
  else if ((c & 0xe0) == 0xc0)
    {
      used = 2;
    }
  else if ((c & 0xf0) == 0xe0)
    {
      used = 3;
    }
  else if ((c & 0xf8) == 0xf0)
    {
      used = 4;
    }
  else
    {
      used = 1;                  /* 非法首字节：吃掉一个 */
    }

  if (used > remain)
    {
      used = 1;
    }

  return used;
}

static size_t utf8_count_n(const char *src, size_t len)
{
  size_t i = 0;
  size_t n = 0;

  if (src == NULL)
    {
      return 0;
    }

  while (i < len)
    {
      i += utf8_char_len(src + i, len - i);
      n++;
    }

  return n;
}

static size_t utf8_count(const char *src)
{
  return (src == NULL) ? 0 : utf8_count_n(src, strlen(src));
}

/****************************************************************************
 * Name: tts_punct_rank
 *
 * Description:
 *   给一个字符的字节串（p 是它的起点，n 是它的字节数）打分，决定这里适不适合
 *   切块：
 *     2 = 句末（。！？；!?; 和换行）—— 最自然的切点；
 *     1 = 句内（，、：,）—— 次选；
 *     0 = 不切。
 *   标点本身算在前一块里（念出来那一块的收尾才自然）。
 *
 ****************************************************************************/

static int tts_punct_rank(const char *p, size_t n)
{
  if (n == 1)
    {
      if (p[0] == '.' || p[0] == '!' || p[0] == '?' || p[0] == ';'
          || p[0] == '\n')
        {
          return 2;
        }

      if (p[0] == ',' || p[0] == ':')
        {
          return 1;
        }

      return 0;
    }

  if (n == 3)
    {
      if (memcmp(p, "\xe3\x80\x82", 3) == 0      /* 。 */
          || memcmp(p, "\xef\xbc\x81", 3) == 0   /* ！ */
          || memcmp(p, "\xef\xbc\x9f", 3) == 0   /* ？ */
          || memcmp(p, "\xef\xbc\x9b", 3) == 0)  /* ； */
        {
          return 2;
        }

      if (memcmp(p, "\xef\xbc\x8c", 3) == 0      /* ， */
          || memcmp(p, "\xe3\x80\x81", 3) == 0   /* 、 */
          || memcmp(p, "\xef\xbc\x9a", 3) == 0)  /* ： */
        {
          return 1;
        }

      return 0;
    }

  return 0;
}

/****************************************************************************
 * Name: tts_split_len
 *
 * Description:
 *   在 src 的前 max_chars 个字里找一个适合切块的位置，返回它的**字节**偏移
 *   （返回值 > 0）。优先切在句末标点后面，其次切在逗号 / 顿号后面；都没有
 *   就硬切在第 max_chars 个字处。
 *
 *   为什么要挑标点：分块合成是"一块一次请求"，切在一句话中间听起来就是
 *   半句话断掉；切在句末的停顿本来就有，拼起来跟一次合成差不多。
 *   切点还不能太靠前（否则一句话被切成一堆碎块、多打好几次网络）：句末 /
 *   句内标点都只在前半段之后才算数，最靠前的可接受切点是 max_chars / 2。
 *
 ****************************************************************************/

static size_t tts_split_len(const char *src, size_t len, size_t max_chars)
{
  size_t i = 0;
  size_t chars = 0;
  size_t soft_end = 0;
  size_t hard_end = 0;
  size_t min_chars = (max_chars > 4) ? (max_chars / 2) : 1;

  if (src == NULL || len == 0 || max_chars == 0)
    {
      return 0;
    }

  while (i < len && chars < max_chars)
    {
      size_t n = utf8_char_len(src + i, len - i);
      int rank;

      i += n;
      chars++;

      if (chars < min_chars)
        {
          continue;
        }

      rank = tts_punct_rank(src + i - n, n);

      if (rank == 2)
        {
          hard_end = i;
        }
      else if (rank == 1)
        {
          soft_end = i;
        }
    }

  if (hard_end != 0)
    {
      return hard_end;
    }

  if (soft_end != 0)
    {
      return soft_end;
    }

  return i;
}

/****************************************************************************
 * Name: tts_truncate
 *
 * Description:
 *   按"字"（UTF-8 码点）把要合成的文本截到 max_chars 个，超长时在末尾补一个
 *   省略号（U+2026，"…"）。返回写进 dst 的字节数，*truncated 记下有没有截。
 *
 *   为什么要截：MiMo TTS 回的是音频文件的 base64，每字约 11 KB，而板子的
 *   响应缓冲和调用方的 PCM 缓冲都是有上限的（见宏注释）。宁可少念后面一段
 *   （并在日志里报出截了多少字），也好过整段卡死一声不出。
 *
 *   ⚠️ 只截 TTS 这一路。对话区显示和 ASR 都不截（显示当然要完整）。
 *
 ****************************************************************************/

static size_t tts_truncate(const char *src, char *dst, size_t dst_cap,
                           size_t max_chars, bool *truncated)
{
  size_t i = 0;
  size_t o = 0;
  size_t chars = 0;
  size_t len;

  *truncated = false;

  if (src == NULL || dst == NULL || dst_cap == 0)
    {
      return 0;
    }

  len = strlen(src);

  while (i < len)
    {
      size_t used = utf8_char_len(src + i, len - i);

      if (used == 0 || chars >= max_chars || o + used + 1 > dst_cap)
        {
          *truncated = true;
          break;
        }

      memcpy(dst + o, src + i, used);
      o += used;
      i += used;
      chars++;
    }

  /* 省略号：U+2026 的 UTF-8 是 E2 80 A6，字库里有这个字形 */

  if (*truncated && o + 4 <= dst_cap)
    {
      memcpy(dst + o, "\xe2\x80\xa6", 3);
      o += 3;
    }

  dst[o] = '\0';
  return o;
}


/****************************************************************************
 * Name: mimo_load_config
 *
 * Description:
 *   把 llm_host / api_key / llm_path / llm_port / model 读进静态快照。
 *   host 和 key 缺一个都算不可用（不给默认值，避免用错凭据去打别人的服务）。
 *   model 只有对话（mimo_chat）用，缺了不算整体不可用，由 mimo_chat 自己报错。
 *
 ****************************************************************************/

static int mimo_load_config(void)
{
  s_host[0] = '\0';
  s_key[0]  = '\0';
  s_path[0] = '\0';
  s_port[0] = '\0';
  s_model[0] = '\0';
  s_asr_model[0] = '\0';
  s_asr_lang[0]  = '\0';

  if (claw_config_get(AGENT_CFG_KEY_LLM_HOST, s_host, sizeof(s_host)) != OK
      || s_host[0] == '\0')
    {
      return -ENOENT;
    }

  if (claw_config_get(AGENT_CFG_KEY_API_KEY, s_key, sizeof(s_key)) != OK
      || s_key[0] == '\0')
    {
      return -ENOENT;
    }

  if (claw_config_get(AGENT_CFG_KEY_LLM_PATH, s_path, sizeof(s_path)) != OK
      || s_path[0] == '\0')
    {
      strncpy(s_path, MIMO_DEFAULT_PATH, sizeof(s_path) - 1);
    }

  /* llm_port 没有独立的宏，llm_proxy.c 也是直接写 "llm_port" */

  if (claw_config_get("llm_port", s_port, sizeof(s_port)) != OK
      || s_port[0] == '\0')
    {
      strncpy(s_port, MIMO_DEFAULT_PORT, sizeof(s_port) - 1);
    }

  /* 对话模型：从配置的 model 键读（不写死） */

  claw_config_get(AGENT_CFG_KEY_MODEL, s_model, sizeof(s_model));

  /* ASR 的 model / language（S4.4）。读不到就用改动前的写死值，保证不配
   * 任何东西时请求体和以前逐字节相同 —— 换模型/换语言是"改配置"级别的事，
   * 不该逼着重烧。 */

  if (claw_config_get("asr_model", s_asr_model, sizeof(s_asr_model)) != OK
      || s_asr_model[0] == '\0')
    {
      strncpy(s_asr_model, MIMO_ASR_MODEL, sizeof(s_asr_model) - 1);
      s_asr_model[sizeof(s_asr_model) - 1] = '\0';
    }

  if (claw_config_get("asr_language", s_asr_lang, sizeof(s_asr_lang)) != OK
      || s_asr_lang[0] == '\0')
    {
      strncpy(s_asr_lang, "zh", sizeof(s_asr_lang) - 1);
      s_asr_lang[sizeof(s_asr_lang) - 1] = '\0';
    }

  return OK;
}

/* ── TLS 连接复用策略（2026-10-25） ────────────────────────────
 *
 * mimo_post() 以前**每次**都先 vela_tls_pool_cleanup() 强制全新 TLS 握手，
 * 那是为了躲 vela_tls.c 池子复用路径的一个 bug（排空残包时把错误态当"排空
 * 干净了"继续用，死连接要等满读超时才失败，界面停在"正在识别"）。
 *
 * 那个根因**后来在 vela_tls.c 里修掉了**：现在排空结果只有 WANT_READ /
 * WANT_WRITE 算"连接还活着"，其余一律 pool_reconnect 重建，注释里还写明
 * "mimo_post()/weather_http_get() 里那句 vela_tls_pool_cleanup() 是给这个
 * bug 打的补丁 —— 根因修在这里，补丁以后可以逐步撤掉"。现在就是撤补丁。
 *
 * 为什么要撤：实测每块 TTS 都要付一次握手税（约 800 ms），而块间空档中位
 * 870 ms 里绝大部分就是它（合成 78 ms/字 ≈ 播放 75 ms/字，两者基本抵消）。
 * 轮内复用后空档应降到 200 ms 量级。而且每块一次握手 = 每块一次可能失败的
 * DNS/TCP/TLS，块数越多越容易中途掉一次；复用反而是**更少的故障点**。
 *
 * 保守在哪（三条，都为了"稳定优先于流畅"）：
 *   ① 空闲超过 MIMO_TLS_FRESH_AFTER_MS 就当"新一轮"，起手先清池。
 *      一轮内相邻两次 POST 间隔只有 3~5 秒（播放一小块的时间），远小于这个
 *      阈值 → 轮内复用；两轮之间隔着"说话 + ASR + LLM"，十几秒到几十秒
 *      → 每轮首块前仍是全新连接，捡不到上一轮的陈货。
 *      这正好等于计划里写的"只在每轮首块前清一次"，只是判据放在本函数内，
 *      不用跨文件去数 g_tts_seq，也不会漏掉某个入口。
 *   ② 上一次 POST 失败或墙钟超过 MIMO_POST_SLOW_MS，置 s_tls_dirty，
 *      下一次起手必定清池。真踩上一次坏连接，最多慢一块，不会一路慢下去。
 *      （失败时 vela_tls 自己已经 pool_release(keep=false) 拆掉连接了，
 *      这条只是"网络可疑就别再复用"的额外保守。）
 *   ③ 每次清池都打一行带原因的日志，异常（失败/过慢）另打一行。一轮大概
 *      一两行，不吵，但烧录后能直接看出"到底有没有在复用"。
 *
 * 残余风险（说实话）：半开连接（对端悄悄没了、没发 FIN/RST）是池子复用的
 * 真空洞 —— 排空 10ms 超时读到 WANT_READ 会当成活着，写进去之后在
 * tls_read_response() 上等满 AGENT_LLM_SOCKET_TIMEOUT_SEC（**120 秒**）才
 * 失败。①就是冲着它去的：半开和"闲置多久"强相关，闲置短的连接基本不会半开。
 * 万一还是踩上，界面上有 ROUND_TTS_STALL_MAX_MS（25 秒无进展即作废整轮）
 * 兜底 —— 轮会掉、但设备会自愈，最坏是那条调用线程再挂到 120 秒读超时。
 *
 * 硬回退判据（**30 轮里出现一次「合成卡住 >20 秒」就照做**）：
 *   把 MIMO_TLS_REUSE_IN_ROUND 改成 0，一行回到旧的"每块清池"。
 *
 * weather_http_get() 里的清池**不动**：那是另一台机器（open-meteo）、
 * 平时用不上池子，保持每次全新连接。
 */

#ifndef MIMO_TLS_REUSE_IN_ROUND
#define MIMO_TLS_REUSE_IN_ROUND 1
#endif

/* 距上次 POST 超过这个毫秒数就当"新一轮"，起手先清池。
 *
 * ★ 2026-10-30 从 12000 改到 60000，理由是真机数据（board_boot.log）：
 *   旧值 12 秒把**每一轮的首块**都判成"闲置太久"而清池 —— 一轮里相邻两块
 *   只隔 3~5 秒（复用），两轮之间隔着"说话 + ASR + LLM"，12~19 秒（同一次
 *   连续追问）到 38~163 秒（歇一会儿再问）。于是十次清池里有五次是
 *   12880 / 14082 / 19296 / 38036 / 39684 ms —— 全都在 12~40 秒这个
 *   "用户就是连续在问"的区间里，白白付了一次握手。
 *
 *   60 秒正好盖住这批（下一个是 71155 ms，仍然会走全新连接）：
 *   "用户真的歇了半分钟以上才问下一句"仍按新一轮处理，宁可慢一点也不冒险。
 *   收益 = 每轮首声提前约 0.7 秒（握手实测 600~700 ms，见上面那段更正）。
 *
 * 保守起见留着的两条阀（都在下面 mimo_post 里）：
 *   · s_tls_dirty —— 上次 POST 失败或墙钟超 MIMO_POST_SLOW_MS，下次必定清池；
 *   · 每次清池都打一行带原因的日志，烧录后能直接看出到底有没有在复用。
 *
 * ★ 硬回退（**30 轮里出现一次「合成卡住 >20 秒」就照做**，一行回到旧行为）：
 *   把本宏改回 12000；再不行把 MIMO_TLS_REUSE_IN_ROUND 改成 0（每块清池）。
 *   残余风险说清楚：半开连接（对端悄悄没了、没发 FIN/RST）排空那 10ms
 *   读到 WANT_READ 会当成活着，写进去之后要等满 120 秒读超时才失败。
 *   60 秒闲置下这个概率比 12 秒高，但真踩上也有 ROUND_TTS_STALL_MAX_MS
 *   （25 秒无进展即作废整轮）兜底 —— 轮会掉、设备会自愈，不会从此不能对话。
 */
#define MIMO_TLS_FRESH_AFTER_MS 60000

/* 单次 POST 墙钟超过这个毫秒数 = 异常（不是"念得久"，是网络/服务端不对劲）。
 * 计划里定的 15 秒：正常合成一块（24 字上限 ≈ 50 字节）约 4~5 秒。 */
#define MIMO_POST_SLOW_MS 15000

static uint32_t s_post_last_ms;   /* 上一次 mimo_post 起手的时刻 */
static bool     s_tls_dirty;      /* 置位后下次 mimo_post 必须清池 */

static uint32_t mimo_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/****************************************************************************
 * Name: mimo_post
 *
 * Description:
 *   POST 一段 JSON 到 <llm_host>:<llm_port><llm_path>，复用 ai_agent 的
 *   vela_https_post_json()（它自己加 Content-Type: application/json）。
 *   这里只补 Authorization: Bearer <api_key>。
 *
 *   不加 Connection 头：ai_agent 的 vela_tls.c 在 tls_write_request() 里已经
 *   写死了 "Connection: keep-alive"，再传一个会出现重复头（volc_tts.c 就是这么
 *   传的，能通，但没必要冒这个险）。
 *
 ****************************************************************************/

static int mimo_post(const char *body, char *resp, size_t resp_cap)
{
  char auth[192];
  uint32_t now;
  uint32_t t0;
  uint32_t dt;
  int ret;
  bool fresh;

  /* 只在本函数里用，绝不打印 */

  snprintf(auth, sizeof(auth), "Bearer %s", s_key);

  vela_header_t hdrs[] = {
    { "Authorization", auth },
    { NULL, NULL }
  };

  now = mimo_now_ms();

  /* 该不该清池：开关关着 / 上次出过事 / 空闲太久 → 清；否则轮内复用。
   * 见上面 MIMO_TLS_REUSE_IN_ROUND 那整段说明。 */
  if (!MIMO_TLS_REUSE_IN_ROUND)
    {
      fresh = true;
    }
  else if (s_tls_dirty)
    {
      fresh = true;
    }
  else if (s_post_last_ms == 0 ||
           (uint32_t)(now - s_post_last_ms) >= MIMO_TLS_FRESH_AFTER_MS)
    {
      fresh = true;
    }
  else
    {
      fresh = false;
    }

  if (fresh)
    {
      const char *why;

      if (!MIMO_TLS_REUSE_IN_ROUND)
        {
          why = "复用已关（旧行为）";
        }
      else if (s_tls_dirty)
        {
          why = "上次 POST 出过事";
        }
      else if (s_post_last_ms == 0)
        {
          why = "本轮第一次";
        }
      else
        {
          why = "闲置太久";
        }

      /* 这行是给烧录后验收用的：一轮里应该只看到一次"清池"，
       * 后面十几块都不再打，说明确实没再重复握手。 */
      printf("[TLS] 清池强制全新连接（%s，距上次 %lu ms）\n",
             why,
             (unsigned long)(s_post_last_ms == 0
                             ? 0 : (uint32_t)(now - s_post_last_ms)));

      vela_tls_pool_cleanup();
      s_tls_dirty = false;
    }

  t0 = mimo_now_ms();
  s_post_last_ms = t0;

  ret = vela_https_post_json(s_host, s_port, s_path, hdrs, body,
                             resp, resp_cap);

  dt = (uint32_t)(mimo_now_ms() - t0);

  /* 失败或过慢 → 下次别再复用，并打一行异常日志（观测行，不改别的行为）。
   * 用 printf 而不是 syslog：这条就是要在串口上一眼看见。 */
  if (ret < 0)
    {
      s_tls_dirty = true;
      printf("[TLS] ★ POST 失败 ret=%d，耗时 %lu ms —— 下次强制全新连接\n",
             ret, (unsigned long)dt);
    }
  else if ((int32_t)dt >= (int32_t)MIMO_POST_SLOW_MS)
    {
      s_tls_dirty = true;
      printf("[TLS] ★ POST 耗时 %lu ms（≥ %d）—— 异常慢，下次强制全新连接"
             "（这就是「合成卡住」的样子，30 轮里出现一次就该回退）\n",
             (unsigned long)dt, MIMO_POST_SLOW_MS);
    }

  return ret;
}

/****************************************************************************
 * Name: mimo_log_http_error
 *
 * Description:
 *   出错时打状态码 + 响应体前 200 字节。响应体可能是服务端的错误 JSON，
 *   截断打印足够定位问题；成功路径（含 base64 音频）不打。
 *
 ****************************************************************************/

static void mimo_log_http_error(const char *what, int status,
                                const char *resp)
{
  if (status < 0)
    {
      syslog(LOG_ERR, "[%s] %s: HTTPS 失败 %d\n", TAG, what, status);
      return;
    }

  syslog(LOG_ERR, "[%s] %s: HTTP %d, 响应 %.*s\n", TAG, what, status,
         MIMO_ERR_DUMP_LEN,
         (resp != NULL && resp[0] != '\0') ? resp : "(空)");
}

/****************************************************************************
 * Name: hex4 / utf8_put / json_unescape / json_escape
 *
 * Description:
 *   极简 JSON 字符串编解码：不引 cJSON（hello_app 的编译参数里没有它的
 *   include 路径，而且大 base64 过一遍 cJSON 会多一整份拷贝，板子上不划算）。
 *
 *   json_find_string() 只做"找键 -> 拿引号内的原始字节"，不复制、不反转义
 *   （base64 直接原地解码）；文本类字段再用 json_unescape() 还原 UTF-8。
 *
 ****************************************************************************/

static int hex4(const char *p, unsigned int *out)
{
  unsigned int v = 0;
  int i;

  for (i = 0; i < 4; i++)
    {
      char c = p[i];

      v <<= 4;

      if (c >= '0' && c <= '9')
        {
          v |= (unsigned int)(c - '0');
        }
      else if (c >= 'a' && c <= 'f')
        {
          v |= (unsigned int)(c - 'a' + 10);
        }
      else if (c >= 'A' && c <= 'F')
        {
          v |= (unsigned int)(c - 'A' + 10);
        }
      else
        {
          return -1;
        }
    }

  *out = v;
  return 0;
}

static size_t utf8_put(unsigned int cp, char *dst)
{
  if (cp < 0x80)
    {
      dst[0] = (char)cp;
      return 1;
    }

  if (cp < 0x800)
    {
      dst[0] = (char)(0xc0 | (cp >> 6));
      dst[1] = (char)(0x80 | (cp & 0x3f));
      return 2;
    }

  if (cp < 0x10000)
    {
      dst[0] = (char)(0xe0 | (cp >> 12));
      dst[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
      dst[2] = (char)(0x80 | (cp & 0x3f));
      return 3;
    }

  dst[0] = (char)(0xf0 | (cp >> 18));
  dst[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
  dst[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
  dst[3] = (char)(0x80 | (cp & 0x3f));
  return 4;
}

static const char *json_find_string(const char *json, const char *key,
                                    size_t *out_len)
{
  char pat[64];
  const char *p;

  if (json == NULL || key == NULL || out_len == NULL)
    {
      return NULL;
    }

  if (strlen(key) + 3 > sizeof(pat))
    {
      return NULL;
    }

  snprintf(pat, sizeof(pat), "\"%s\"", key);

  p = strstr(json, pat);
  if (p == NULL)
    {
      return NULL;
    }

  p += strlen(pat);

  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    {
      p++;
    }

  if (*p != ':')
    {
      return NULL;
    }

  p++;

  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    {
      p++;
    }

  if (*p != '"')
    {
      return NULL;
    }

  p++;

  /* 扫到配对的结束引号（跳过 \"）。扫不到引号 = 响应被截断了。 */

  {
    const char *start = p;

    while (*p != '\0' && *p != '"')
      {
        if (*p == '\\' && p[1] != '\0')
          {
            p++;
          }

        p++;
      }

    if (*p != '"')
      {
        return NULL;
      }

    *out_len = (size_t)(p - start);
    return start;
  }
}

/****************************************************************************
 * Name: json_find_number
 *
 * Description:
 *   找 "key" 后面那个数字（JSON 里的数值字段，例如 adjust_setting 的 value）。
 *
 *   json_find_string() 只认**带引号的字符串**值，数值字段一个都取不到 ——
 *   模型按 schema 传的是裸数字 `"value":60`，用它去取必然落空。
 *
 *   这里两种都认：裸数字 `60`，以及有的模型会自作主张写成的 `"60"`。
 *
 * Returned Value:
 *   OK 成功（值在 *out）；-ENOENT 没这个键 / 值不是数字；-EINVAL 参数不对。
 *
 ****************************************************************************/

static int json_find_number(const char *json, const char *key, int *out)
{
  char pat[64];
  const char *p;
  char *end;
  long v;

  if (json == NULL || key == NULL || out == NULL)
    {
      return -EINVAL;
    }

  if (strlen(key) + 3 > sizeof(pat))
    {
      return -EINVAL;
    }

  snprintf(pat, sizeof(pat), "\"%s\"", key);

  p = strstr(json, pat);
  if (p == NULL)
    {
      return -ENOENT;
    }

  p += strlen(pat);

  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    {
      p++;
    }

  if (*p != ':')
    {
      return -ENOENT;
    }

  p++;

  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    {
      p++;
    }

  /* 有的模型会把数字写成字符串，两种都收 */

  if (*p == '"')
    {
      p++;
    }

  if (*p != '-' && (*p < '0' || *p > '9'))
    {
      return -ENOENT;
    }

  v = strtol(p, &end, 10);
  if (end == p)
    {
      return -ENOENT;
    }

  *out = (int)v;
  return OK;
}

static size_t json_unescape(const char *src, size_t len, char *dst,
                            size_t dst_cap)
{
  size_t i = 0;
  size_t o = 0;

  while (i < len && o < dst_cap)
    {
      char c = src[i++];

      if (c != '\\' || i >= len)
        {
          dst[o++] = c;
          continue;
        }

      c = src[i++];

      switch (c)
        {
          case '"':  dst[o++] = '"';  break;
          case '\\': dst[o++] = '\\'; break;
          case '/':  dst[o++] = '/';  break;
          case 'b':  dst[o++] = '\b'; break;
          case 'f':  dst[o++] = '\f'; break;
          case 'n':  dst[o++] = '\n'; break;
          case 'r':  dst[o++] = '\r'; break;
          case 't':  dst[o++] = '\t'; break;

          case 'u':
            {
              unsigned int cp;

              if (i + 4 > len || hex4(src + i, &cp) != 0)
                {
                  dst[o++] = '?';
                  break;
                }

              i += 4;

              /* UTF-16 代理对：高代理 + \uXXXX 低代理合成一个码点 */

              if (cp >= 0xd800 && cp <= 0xdbff && i + 6 <= len
                  && src[i] == '\\' && src[i + 1] == 'u')
                {
                  unsigned int lo;

                  if (hex4(src + i + 2, &lo) == 0
                      && lo >= 0xdc00 && lo <= 0xdfff)
                    {
                      cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                      i += 6;
                    }
                }

              if (o + 4 > dst_cap)
                {
                  break;
                }

              o += utf8_put(cp, dst + o);
            }
            break;

          default:
            dst[o++] = c;
            break;
        }
    }

  return o;
}

static size_t json_escape(const char *src, size_t len, char *dst,
                          size_t dst_cap)
{
  size_t i;
  size_t o = 0;

  for (i = 0; i < len; i++)
    {
      unsigned char c = (unsigned char)src[i];
      char tmp[8];
      size_t n;

      switch (c)
        {
          case '"':  tmp[0] = '\\'; tmp[1] = '"';  n = 2; break;
          case '\\': tmp[0] = '\\'; tmp[1] = '\\'; n = 2; break;
          case '\n': tmp[0] = '\\'; tmp[1] = 'n';  n = 2; break;
          case '\r': tmp[0] = '\\'; tmp[1] = 'r';  n = 2; break;
          case '\t': tmp[0] = '\\'; tmp[1] = 't';  n = 2; break;

          default:
            if (c < 0x20)
              {
                static const char hexd[] = "0123456789abcdef";

                tmp[0] = '\\';
                tmp[1] = 'u';
                tmp[2] = '0';
                tmp[3] = '0';
                tmp[4] = hexd[(c >> 4) & 0xf];
                tmp[5] = hexd[c & 0xf];
                n = 6;
              }
            else
              {
                tmp[0] = (char)c;
                n = 1;
              }
            break;
        }

      if (o + n > dst_cap)
        {
          break;
        }

      memcpy(dst + o, tmp, n);
      o += n;
    }

  return o;
}

/****************************************************************************
 * Name: trim_whitespace
 *
 * Description:
 *   去掉字符串首尾的空格 / 制表符 / 换行（就地）。模型回复常带一个尾换行。
 *
 ****************************************************************************/

static void trim_whitespace(char *s)
{
  size_t i = 0;
  size_t len;

  if (s == NULL)
    {
      return;
    }

  while (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')
    {
      i++;
    }

  if (i > 0)
    {
      memmove(s, s + i, strlen(s + i) + 1);
    }

  len = strlen(s);

  while (len > 0)
    {
      char c = s[len - 1];

      if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
        {
          break;
        }

      s[--len] = '\0';
    }
}

/****************************************************************************
 * Name: rd_le16 / rd_le32 / wr_le16 / wr_le32
 *
 * Description:
 *   小端读写。WAV 头按字节拼，避开"把任意偏移强转成 int16_t*"的对齐问题。
 *
 ****************************************************************************/

static unsigned int rd_le16(const unsigned char *p)
{
  return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static unsigned long rd_le32(const unsigned char *p)
{
  return (unsigned long)p[0] | ((unsigned long)p[1] << 8)
         | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static void wr_le16(unsigned char *p, unsigned int v)
{
  p[0] = (unsigned char)(v & 0xff);
  p[1] = (unsigned char)((v >> 8) & 0xff);
}

static void wr_le32(unsigned char *p, unsigned long v)
{
  p[0] = (unsigned char)(v & 0xff);
  p[1] = (unsigned char)((v >> 8) & 0xff);
  p[2] = (unsigned char)((v >> 16) & 0xff);
  p[3] = (unsigned char)((v >> 24) & 0xff);
}

/****************************************************************************
 * Name: wav_write_header
 *
 * Description:
 *   写 44 字节标准 PCM WAV 头（ASR 要把整段 WAV 传上去，服务端按 fmt 块解析）。
 *
 ****************************************************************************/

static void wav_write_header(unsigned char *hdr, size_t pcm_len)
{
  unsigned int block = MIMO_PCM_CHANNELS * MIMO_PCM_BITS / 8;

  memcpy(hdr, "RIFF", 4);
  wr_le32(hdr + 4, (unsigned long)(36 + pcm_len));
  memcpy(hdr + 8, "WAVEfmt ", 8);
  wr_le32(hdr + 16, 16);                            /* fmt 块长度 */
  wr_le16(hdr + 20, 1);                             /* 1 = 未压缩 PCM */
  wr_le16(hdr + 22, MIMO_PCM_CHANNELS);
  wr_le32(hdr + 24, MIMO_PCM_RATE);
  wr_le32(hdr + 28, (unsigned long)MIMO_PCM_RATE * block);
  wr_le16(hdr + 32, block);
  wr_le16(hdr + 34, MIMO_PCM_BITS);
  memcpy(hdr + 36, "data", 4);
  wr_le32(hdr + 40, (unsigned long)pcm_len);
}

/****************************************************************************
 * Name: wav_parse
 *
 * Description:
 *   扫 RIFF 块，找 fmt / data。只认未压缩 PCM（fmt=1）16bit。
 *
 ****************************************************************************/

static int wav_parse(const unsigned char *buf, size_t len,
                     struct wav_info_s *info)
{
  size_t pos = 12;

  if (len < MIMO_WAV_HDR_LEN)
    {
      return -EINVAL;
    }

  if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0)
    {
      return -EINVAL;
    }

  memset(info, 0, sizeof(*info));

  while (pos + 8 <= len)
    {
      const unsigned char *id = buf + pos;
      unsigned long sz = rd_le32(buf + pos + 4);
      size_t body = pos + 8;

      if (memcmp(id, "fmt ", 4) == 0)
        {
          unsigned int fmt_tag;

          if (sz < 16 || body + 16 > len)
            {
              return -EINVAL;
            }

          fmt_tag = rd_le16(buf + body);

          if (fmt_tag != 1)
            {
              /* WAVE_FORMAT_EXTENSIBLE(0xFFFE)：真正的格式在扩展块开头
               * 那 2 字节（PCM 就是 1），后面各字段偏移不变。
               * 扩展块布局：cbSize(16) / valid bits(18) / channel mask(20) /
               * SubFormat GUID(24)，所以 SubFormat 的 tag 在 body+24。 */

              if (fmt_tag != 0xfffe || sz < 26 || body + 26 > len
                  || rd_le16(buf + body + 24) != 1)
                {
                  return -ENOSYS;   /* 非 PCM（浮点/ADPCM）不支持 */
                }
            }

          info->channels = rd_le16(buf + body + 2);
          info->rate     = (unsigned int)rd_le32(buf + body + 4);
          info->bits     = rd_le16(buf + body + 14);

          /* data 块可能出现在 fmt 前面，反过来再找一遍 */

          if (info->data_len != 0)
            {
              return OK;
            }
        }
      else if (memcmp(id, "data", 4) == 0)
        {
          info->data_off = body;
          info->data_len = (size_t)sz;

          if (info->data_off + info->data_len > len)
            {
              info->data_len = len - info->data_off;   /* 文件被截短 */
            }

          if (info->rate != 0 && info->data_len != 0)
            {
              return OK;
            }
        }
      else if ((size_t)sz > len - body)
        {
          break;                 /* 块长度超出文件，别把 pos 算溢出 */
        }

      pos = body + (size_t)sz + (sz & 1);   /* 块按偶数字节对齐 */
    }

  if (info->rate == 0 || info->data_len == 0)
    {
      return -EINVAL;
    }

  return OK;
}

/****************************************************************************
 * Name: resample_to_16k
 *
 * Description:
 *   int16 线性插值重采样到 16 kHz（24k -> 16k 是 3:2），多声道只取第 0 声道。
 *   源缓冲按字节读（WAV 的 data 段偏移不保证 2 字节对齐），所以这里收
 *   unsigned char* + channels，而不是 int16_t*。
 *   采样率已经是 16k 且单声道时直接拷贝。返回写进 dst 的采样点数。
 *
 ****************************************************************************/

static size_t resample_to_16k(const unsigned char *src, size_t frames,
                              unsigned int rate, unsigned int channels,
                              int16_t *dst, size_t dst_frames)
{
  size_t stride = (size_t)channels * 2;
  uint64_t step;
  uint64_t pos = 0;
  size_t n = 0;

  if (src == NULL || dst == NULL || frames == 0 || dst_frames == 0
      || channels == 0)
    {
      return 0;
    }

  if (rate == MIMO_PCM_RATE)
    {
      if (frames > dst_frames)
        {
          frames = dst_frames;
        }

      if (channels == 1)
        {
          memcpy(dst, src, frames * sizeof(int16_t));
        }
      else
        {
          size_t i;

          for (i = 0; i < frames; i++)
            {
              dst[i] = (int16_t)rd_le16(src + i * stride);
            }
        }

      return frames;
    }

  if (rate == 0)
    {
      return 0;
    }

  /* 定点：pos 的单位是 1/65536 个输入采样，step = rate/16000 * 65536 */

  step = ((uint64_t)rate << 16) / MIMO_PCM_RATE;

  if (step == 0)
    {
      return 0;                            /* 输入采样率低于 1 Hz，防死循环 */
    }

  while (n < dst_frames)
    {
      size_t idx = (size_t)(pos >> 16);
      unsigned int frac;
      int32_t a;
      int32_t b;

      if (idx + 1 >= frames)
        {
          break;                           /* 最后一个采样没有右邻居，收尾 */
        }

      frac = (unsigned int)(pos & 0xffff);
      a = (int16_t)rd_le16(src + idx * stride);
      b = (int16_t)rd_le16(src + (idx + 1) * stride);
      dst[n++] = (int16_t)(a + ((int32_t)(((int64_t)(b - a) * frac) >> 16)));
      pos += step;
    }

  return n;
}

/****************************************************************************
 * Name: wav_extract_16k_into
 *
 * Description:
 *   从一段 WAV 内存里取出 PCM，转成 16 kHz / 单声道 / s16le，**直接写进
 *   调用方给的缓冲**（不再自己分配一份"完整时长"的 pcm16）。
 *   多声道只取第 0 声道（本工程的链路就是单声道）。
 *
 *   dst_frames 是目标缓冲能放多少个采样点：TTS 那条路上调用方的缓冲只有
 *   VOICE_TTS_BUF_BYTES（8 秒），音频更长时这里只填满前一段（语义和原来
 *   "装得下多少给多少"一致），*full_frames 回的是"完整放得下会有多少"，
 *   调用方据此判断有没有被截。
 *
 ****************************************************************************/

static int wav_extract_16k_into(const unsigned char *buf, size_t len,
                                int16_t *dst, size_t dst_frames,
                                size_t *out_frames, size_t *full_frames)
{
  struct wav_info_s info;
  size_t frames;
  size_t total;
  int ret;

  *out_frames = 0;

  if (full_frames != NULL)
    {
      *full_frames = 0;
    }

  if (dst == NULL || dst_frames == 0)
    {
      return -EINVAL;
    }

  ret = wav_parse(buf, len, &info);
  if (ret != OK)
    {
      return ret;
    }

  if (info.bits != 16)
    {
      return -ENOSYS;                      /* 只支持 16bit */
    }

  if (info.channels < 1 || info.channels > 2)
    {
      return -ENOSYS;
    }

  frames = info.data_len / ((size_t)info.channels * 2);
  if (frames == 0)
    {
      return -EINVAL;
    }

  if (info.rate == 0)
    {
      return -EINVAL;
    }

  total = (size_t)(((uint64_t)frames * MIMO_PCM_RATE) / info.rate);
  if (total == 0)
    {
      return -EINVAL;
    }

  if (full_frames != NULL)
    {
      *full_frames = total;
    }

  *out_frames = resample_to_16k(buf + info.data_off, frames, info.rate,
                                info.channels, dst, dst_frames);

  return (*out_frames == 0) ? -EINVAL : OK;
}

/****************************************************************************
 * Name: wav_extract_16k
 *
 * Description:
 *   同上，只是输出缓冲由本函数在堆上分配（mimo_wav_load_16k 用，
 *   调用方负责 free）。
 *
 ****************************************************************************/

static int wav_extract_16k(const unsigned char *buf, size_t len,
                           unsigned char **pcm_out, size_t *pcm_len)
{
  struct wav_info_s info;
  size_t frames;
  size_t out_frames;
  size_t total = 0;
  int16_t *out;
  size_t got = 0;
  int ret;

  *pcm_out = NULL;
  *pcm_len = 0;

  ret = wav_parse(buf, len, &info);
  if (ret != OK)
    {
      return ret;
    }

  if (info.bits != 16)
    {
      return -ENOSYS;                      /* 只支持 16bit */
    }

  if (info.channels < 1 || info.channels > 2)
    {
      return -ENOSYS;
    }

  frames = info.data_len / ((size_t)info.channels * 2);
  if (frames == 0)
    {
      return -EINVAL;
    }

  out_frames = (size_t)(((uint64_t)frames * MIMO_PCM_RATE) / info.rate);
  if (out_frames == 0)
    {
      return -EINVAL;
    }

  out = malloc((out_frames + 1) * sizeof(int16_t));
  if (out == NULL)
    {
      return -ENOMEM;
    }

  ret = wav_extract_16k_into(buf, len, out, out_frames, &got, &total);
  if (ret != OK)
    {
      free(out);
      return ret;
    }

  *pcm_out = (unsigned char *)out;
  *pcm_len = got * sizeof(int16_t);
  return OK;
}

/****************************************************************************
 * Name: b64_decode_inplace
 *
 * Description:
 *   base64 **就地**解码：dst 和 src 是同一块内存。
 *
 *   为什么能就地：mbedtls_base64_decode() 是顺序解码，每吃 4 个输入字节才吐
 *   3 个输出字节，写指针永远落后于读指针（mbedtls/library/base64.c 的第二遍
 *   循环就是这个形状），所以原地解码不会踩到还没读的数据。
 *
 *   为什么值得：TTS 响应是几十万到一两 MB 的 base64，原来要再额外 malloc 一份
 *   3/4 大小的解码缓冲（1.5 MB 的响应就是 ~1.1 MB），是这条链路上第二大的块。
 *
 * Returned Value:
 *   OK（*out 指向 buf 自己）；-EINVAL（base64 非法或解出来是空的）
 *
 ****************************************************************************/

static int b64_decode_inplace(char *buf, size_t b64_len,
                              unsigned char **out, size_t *out_len)
{
  size_t got = 0;
  int ret;

  *out = NULL;
  *out_len = 0;

  if (buf == NULL || b64_len == 0)
    {
      return -EINVAL;
    }

  ret = mbedtls_base64_decode((unsigned char *)buf, b64_len, &got,
                              (const unsigned char *)buf, b64_len);
  if (ret != 0 || got == 0)
    {
      return -EINVAL;
    }

  *out = (unsigned char *)buf;
  *out_len = got;
  return OK;
}

/****************************************************************************
 * Name: mimo_asr_recognize
 *
 * Description:
 *   voice_asr_ops_t.recognize：把 16k 单声道 s16le 的 PCM 包成 WAV、
 *   base64 后 POST 给 MiMo ASR，取 choices[0].message.content。
 *
 ****************************************************************************/

static int mimo_asr_recognize(const unsigned char *pcm_data, size_t pcm_len,
                              char *text_out, size_t text_cap)
{
  char prefix[MIMO_ASR_JSON_PFX_CAP];
  char lang_part[48];
  const char *suffix = MIMO_ASR_JSON_SUFFIX;
  size_t pfx_len;
  size_t sfx_len = strlen(suffix);
  size_t wav_len;
  size_t b64_need = 0;
  size_t b64_len = 0;
  unsigned char *wav;
  char *body;
  char *resp;
  const char *content;
  size_t content_len = 0;
  size_t out_len;
  int status;
  int ret;

  if (pcm_data == NULL || pcm_len == 0 || text_out == NULL
      || text_cap == 0)
    {
      return -EINVAL;
    }

  text_out[0] = '\0';

  if (mimo_load_config() != OK)
    {
      syslog(LOG_ERR, "[%s] ASR: 配置里缺 llm_host / api_key，MiMo 不可用\n",
             TAG);
      return -ENOENT;
    }

  /* 拼请求前缀。model / language 都来自配置（S4.4），language 配成 "none"
   * 就整段不发 —— 见上面 MIMO_ASRLANG_NONE 的注释。 */

  if (strcmp(s_asr_lang, MIMO_ASRLANG_NONE) == 0)
    {
      lang_part[0] = '\0';
    }
  else
    {
      snprintf(lang_part, sizeof(lang_part), "\"language\":\"%s\",",
               s_asr_lang);
    }

  pfx_len = (size_t)snprintf(prefix, sizeof(prefix),
                             "{\"model\":\"%s\",%s\"messages\":["
                             "{\"role\":\"user\",\"content\":["
                             "{\"type\":\"input_audio\","
                             "\"input_audio\":{\"data\":\"",
                             s_asr_model, lang_part);

  if (pfx_len >= sizeof(prefix))
    {
      syslog(LOG_ERR, "[%s] ASR: 请求前缀被截断（要 %zu 字节，缓冲 %d）—— "
             "asr_model / asr_language 配得太长\n", TAG, pfx_len,
             MIMO_ASR_JSON_PFX_CAP);
      return -E2BIG;
    }

  /* 1. PCM -> WAV（服务端要的就是"整个 WAV 文件"，含 44 字节头） */

  wav_len = MIMO_WAV_HDR_LEN + pcm_len;
  wav = mimo_alloc_logged("asr.wav", wav_len);
  if (wav == NULL)
    {
      return -ENOMEM;
    }

  wav_write_header(wav, pcm_len);
  memcpy(wav + MIMO_WAV_HDR_LEN, pcm_data, pcm_len);

  /* 2. 直接拼请求体：前缀 + base64 + 后缀，省掉中间那份 base64 字符串 */

  ret = mbedtls_base64_encode(NULL, 0, &b64_need, wav, wav_len);
  if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || b64_need == 0)
    {
      mimo_free_logged("asr.wav", wav, wav_len);
      return -EIO;
    }

  body = mimo_alloc_logged("asr.body", pfx_len + b64_need + sfx_len + 1);
  if (body == NULL)
    {
      mimo_free_logged("asr.wav", wav, wav_len);
      return -ENOMEM;
    }

  memcpy(body, prefix, pfx_len);

  ret = mbedtls_base64_encode((unsigned char *)body + pfx_len,
                              b64_need, &b64_len, wav, wav_len);
  mimo_free_logged("asr.wav", wav, wav_len);   /* 只活到编码完这一次 */

  if (ret != 0)
    {
      mimo_free_logged("asr.body", body, pfx_len + b64_need + sfx_len + 1);
      return -EIO;
    }

  memcpy(body + pfx_len + b64_len, suffix, sfx_len + 1);

  syslog(LOG_INFO, "[%s] ASR: PCM %zu 字节 -> WAV %zu 字节 -> base64 %zu 字节"
         "（model=%s language=%s）\n",
         TAG, pcm_len, wav_len, b64_len, s_asr_model,
         (lang_part[0] == '\0') ? "不发" : s_asr_lang);

  /* 3. 请求。只报长度，不报 body（里面是音频）。
   *    ASR 的响应就是一段识别文字，16 KB 足够（别按 TTS 那个量级开）。 */

  resp = mimo_calloc_logged("asr.resp", MIMO_ASR_RESP_CAP);
  if (resp == NULL)
    {
      mimo_free_logged("asr.body", body, pfx_len + b64_need + sfx_len + 1);
      return -ENOMEM;
    }

  /* ★ 2026-11-04：量一下 MiMo ASR 的**单次 HTTPS 请求**耗时。
   *   用户要把 MiMo 和火山 e2e 对比：火山日志有 infer(0.5s) 与 e2e(6~10s)，
   *   MiMo 是单个 POST，这个数就是它的端到端网络段。 */
  struct timespec asr_t0, asr_t1;
  clock_gettime(CLOCK_MONOTONIC, &asr_t0);
  status = mimo_post(body, resp, MIMO_ASR_RESP_CAP);
  clock_gettime(CLOCK_MONOTONIC, &asr_t1);
  syslog(LOG_INFO, "[%s] ASR: 请求耗时 %ld ms (HTTP %d)\n", TAG,
         (long)((asr_t1.tv_sec - asr_t0.tv_sec) * 1000 +
                (asr_t1.tv_nsec - asr_t0.tv_nsec) / 1000000),
         status);

  /* body 是 16k*2*最长 15 秒 PCM 的 base64（~640 KB，见
   * ai_companion_main.c 的 SPEECH_BUF_MAX_FRAMES），请求一回来就还给堆 ——
   * 后面解析响应、进 chat/TTS 都不再需要它。 */
  mimo_free_logged("asr.body", body, pfx_len + b64_need + sfx_len + 1);

  if (status != 200)
    {
      mimo_log_http_error("ASR", status, resp);
      mimo_free_logged("asr.resp", resp, MIMO_ASR_RESP_CAP);
      return -EIO;
    }

  content = json_find_string(resp, "content", &content_len);

  if (content == NULL)
    {
      content = json_find_string(resp, "text", &content_len);
    }

  if (content == NULL)
    {
      syslog(LOG_ERR, "[%s] ASR: 响应里没有 content/text: %.*s\n", TAG,
             MIMO_ERR_DUMP_LEN, resp[0] != '\0' ? resp : "(空)");
      mimo_free_logged("asr.resp", resp, MIMO_ASR_RESP_CAP);
      return -EPROTO;
    }

  out_len = json_unescape(content, content_len, text_out, text_cap - 1);
  text_out[out_len] = '\0';

  syslog(LOG_INFO, "[%s] ASR: 识别结果 %zu 字节\n", TAG, out_len);

  mimo_free_logged("asr.resp", resp, MIMO_ASR_RESP_CAP);
  return OK;
}

/****************************************************************************
 * Name: mimo_asr_recognize_direct
 *
 * Description:
 *   给 ai_companion 的「报警听窗兜底」用的直调入口：火山返回空结果时，
 *   用 MiMo 再识别一遍同一段 PCM。不经过 voice_asr 的后端切换，避免
 *   改全局 s_active（会把主链路临时切到 MiMo 再切回来）。
 *
 ****************************************************************************/

int mimo_asr_recognize_direct(const unsigned char *pcm_data, size_t pcm_len,
                              char *text_out, size_t text_cap)
{
  return mimo_asr_recognize(pcm_data, pcm_len, text_out, text_cap);
}

/****************************************************************************
 * Name: tts_synth_chunk
 *
 * Description:
 *   POST **一块**文本（一句话左右，不超过 MIMO_TTS_CHUNK_CHARS 个字）给 MiMo
 *   TTS，取 choices[0].message.audio.data（base64 WAV，24 kHz），解码 + 重采样
 *   成 16 kHz / 单声道 / s16le 写进 pcm_out。
 *
 *   内存上的三处收紧（见文件头）：
 *     - base64 就地解码，不再额外要一份 3/4 大小的解码缓冲；
 *     - 重采样直接写进 pcm_out，不再先建一份"完整时长"的 pcm16 再 memcpy；
 *     - 一块的文字量有限，所以响应缓冲（MIMO_TTS_RESP_CAP）只要装得下一块。
 *   每一步的块大小都打日志（mimo_alloc_logged / mimo_free_logged）。
 *
 * Input Parameters:
 *   text      - 这一块的文本（不需要 '\0' 结尾，按 text_len 算）；
 *   text_len  - 这一块的字节数；
 *   pcm_out   - 输出：这一块的 16k / 单声道 / s16le PCM（调用方给的剩余空间）；
 *   pcm_cap   - pcm_out 的字节数；
 *   pcm_len   - 输出：实际写进去的字节数；
 *   full_bytes- 输出：这块音频**完整**要多少字节（允许传 NULL）。比 *pcm_len
 *               大就说明 pcm_out 装不下这一块。
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure（网络 / 响应被 cap 截断 /
 *   WAV 解析失败）。
 *
 ****************************************************************************/

static int tts_synth_chunk(const char *text, size_t text_len,
                           unsigned char *pcm_out, size_t pcm_cap,
                           size_t *pcm_len, size_t *full_bytes)
{
  const char *prefix = MIMO_TTS_JSON_PREFIX;
  const char *suffix = MIMO_TTS_JSON_SUFFIX;
  size_t pfx_len = strlen(prefix);
  size_t sfx_len = strlen(suffix);
  size_t body_len;
  size_t esc_cap;
  size_t esc_len;
  char *body;
  char *resp;
  const char *audio;
  const char *b64;
  size_t b64_len = 0;
  unsigned char *decoded = NULL;
  size_t decoded_len = 0;
  size_t out_frames = 0;
  size_t full_frames = 0;
  uint32_t t_synth0 = mimo_now_ms();   /* 只给末尾那行观测日志用 */
  uint32_t t_post_dt = 0;              /* POST 往返那一段占了多少 */
  uint32_t t_all;                      /* 整块墙钟（末尾算一次，别调两次 now） */
  int status;
  int ret;

  *pcm_len = 0;

  if (full_bytes != NULL)
    {
      *full_bytes = 0;
    }

  if (text == NULL || text_len == 0 || pcm_out == NULL || pcm_cap == 0)
    {
      return -EINVAL;
    }

  /* 1. 文本 JSON 转义后拼请求体（role 必须是 assistant）。
   *    转义直接写进 body 里，不再先 malloc 一份 esc 再 copy 一遍。 */

  esc_cap = text_len * 6;
  body_len = pfx_len + esc_cap + sfx_len + 1;

  body = mimo_alloc_logged("tts.body", body_len);
  if (body == NULL)
    {
      return -ENOMEM;
    }

  memcpy(body, prefix, pfx_len);
  esc_len = json_escape(text, text_len, body + pfx_len, esc_cap);
  memcpy(body + pfx_len + esc_len, suffix, sfx_len + 1);

  /* 2. 请求（一块的响应也要几百 KB，缓冲走堆） */

  resp = mimo_calloc_logged("tts.resp", MIMO_TTS_RESP_CAP);
  if (resp == NULL)
    {
      mimo_free_logged("tts.body", body, body_len);
      return -ENOMEM;
    }

  status = mimo_post(body, resp, MIMO_TTS_RESP_CAP);
  t_post_dt = mimo_now_ms() - t_synth0;
  mimo_free_logged("tts.body", body, body_len);

  if (status != 200)
    {
      mimo_log_http_error("TTS", status, resp);
      mimo_free_logged("tts.resp", resp, MIMO_TTS_RESP_CAP);
      return -EIO;
    }

  /* 3. 取 message.audio.data。先在 "audio" 之后再找 "data"，
   *    免得命中响应里别的同名字段。
   *    json_find_string() 要求配对结束引号，找不到就是响应被 cap 截断了 ——
   *    这里必须**明确失败返回**，不能让调用方以为还在合成。 */

  audio = strstr(resp, "\"audio\"");
  if (audio != NULL)
    {
      b64 = json_find_string(audio, "data", &b64_len);
    }
  else
    {
      b64 = NULL;
    }

  if (b64 == NULL)
    {
      syslog(LOG_ERR,
             "[%s] TTS: 响应里没有完整的 audio.data（这一块 %zu 字节文本的响应"
             "超过 %d 字节被截断？）: %.*s\n",
             TAG, text_len, MIMO_TTS_RESP_CAP, MIMO_ERR_DUMP_LEN,
             resp[0] != '\0' ? resp : "(空)");
      mimo_free_logged("tts.resp", resp, MIMO_TTS_RESP_CAP);
      return -EPROTO;
    }

  /* 4. base64 **就地**解码（decoded 就在 resp 里），省掉一份 3/4 大小的
   *    解码缓冲。resp 必须活到重采样结束，之后才能 free。 */

  ret = b64_decode_inplace((char *)b64, b64_len, &decoded, &decoded_len);
  if (ret != OK)
    {
      syslog(LOG_ERR, "[%s] TTS: base64 解码失败 %d（响应被截断？）\n", TAG,
             ret);
      mimo_free_logged("tts.resp", resp, MIMO_TTS_RESP_CAP);
      return ret;
    }

  /* 5. WAV -> 16k 单声道 s16le（MiMo 回的是 24 kHz），直接写进调用方给的
   *    那块空间：装不下就填满到哪儿算哪儿（调用方拿 *pcm_len 和 *full_bytes
   *    一比就知道被截了）。 */

  ret = wav_extract_16k_into(decoded, decoded_len, (int16_t *)pcm_out,
                             pcm_cap / sizeof(int16_t),
                             &out_frames, &full_frames);
  mimo_free_logged("tts.resp", resp, MIMO_TTS_RESP_CAP);

  if (ret != OK || out_frames == 0)
    {
      syslog(LOG_ERR, "[%s] TTS: WAV 解析/重采样失败 %d\n", TAG, ret);
      return (ret != OK) ? ret : -EINVAL;
    }

  *pcm_len = out_frames * sizeof(int16_t);

  if (full_bytes != NULL)
    {
      *full_bytes = full_frames * sizeof(int16_t);
    }

  /* ★ 观测行（2026-10-26，2026-10-30 拆开）：把「合成一块的墙钟」和「这块有
   * 多少毫秒音频」并排打出来。块间空档 = max(0, 合成墙钟 − 音频时长)，之前
   * 只打得出发音频时长，判不出空档到底是握手贵还是云端合成贵，只能猜。
   * 乒乓的账是「播放 N 的同时合成 N+1」，所以这两个数一比就知道队列还顶不
   * 顶得住：合成墙钟 > 音频时长 = 迟早断流（队列再深也只是晚几块断）。
   *
   * 2026-10-30 再拆一层：固定开销里只有三分之一是 TLS 握手（见 MIMO_TLS_
   * FRESH_AFTER_MS 上面那段实测），剩下的 1350 ms 一直没归到实处。这一行
   * 现在把「POST 往返」和「本地（base64 解码 + 重采样）」分开打，下一轮
   * 就能照着数决定往哪儿砍，不用再猜。t_post_dt 里还含拼请求体那几毫秒，
   * 可忽略。不改任何行为，只改这一行。 */
  t_all = mimo_now_ms() - t_synth0;

  syslog(LOG_INFO,
         "[%s] TTS: 合成墙钟 %lu ms = POST %lu ms + 本地 %lu ms"
         "（本块音频约 %zu ms）\n",
         TAG, (unsigned long)t_all, (unsigned long)t_post_dt,
         (unsigned long)(t_all - t_post_dt),
         (size_t)(out_frames * 1000 / MIMO_PCM_RATE));

  return OK;
}

/* ===== TTS 自动降级到火山引擎（2026-10-31）==================================
 *
 * 真机实测（2026-10-31 15:51）：
 *   [mimo_voice] TTS: HTTP 429, 响应
 *       {"error":{"code":"429","message":"quota exhausted","type":"limitation"}}
 *   [提示音] live TTS 合成失败 -5，退回提示音
 * —— MiMo 侧配额一光，整轮对话**一个字都念不出来**，只剩一声 325 ms 提示音。
 * 当天同一份日志里 ASR 却是好的，因为它早就有"出错自动回落"（见
 * ai_companion_main.c 3.5 那节的长注释）。TTS 一直没有：后端在开机时选定
 * "mimo" 就再也不动了。本节补上对称的那半。
 *
 * 策略：**MiMo 优先不变** —— 那条"出声是老人最不能缺的一环"的判断没错，
 * 换后端的收益确实只是音色。但"优先"不等于"只此一家"：一旦撞上 HTTP 硬错
 * （配额耗尽 / 4xx / 5xx），就把后端**永久降到 volcengine**（本次开机内粘性），
 * 不再回头打一个注定 429 的接口。降级后每块直接交给当前后端合成。
 *
 * ⚠ 只救"打过去了、对方说不行"这一种失败。文本为空、缓冲放不下之类的
 *   -EINVAL / -ENOSPC **不降级** —— 那不是后端的锅，换了照样失败，
 *   白白把好后端也换掉。-ENOENT（配置里压根没有 llm_host/api_key）也不降：
 *   那说明整套凭据没配，火山那头未必配了，降级只会掩盖真问题。
 *
 * ⚠ 火山 TTS 用的是和火山 ASR 同一把 AGENT_CFG_KEY_VOLC_API_KEY
 *   （x-api-key 头），而 ASR 当天是通的 ⇒ 这把 key 是活的。
 */

#define TTS_FALLBACK_CHUNK_MAX  (MIMO_TTS_CHUNK_CHARS * 3 + 8)

static volatile bool s_tts_demoted;
static volatile bool s_volc_tts_bad;   /* 火山 TTS 试过且失败（不反复切） */
static volatile bool s_tts_volc_primary; /* 火山 2.0 作为首选 TTS 后端 */

static bool tts_error_is_backend_fault(int ret)
{
  /* ★ -EPROTO（响应被本地缓冲截断）**不是后端的锅**：2026-11-01 真机就是
   *   一个 140 字节的块触发了它，结果把整个 TTS 粘性降级到火山，而火山
   *   这个 key 没开通 TTS（403），整机彻底无声。缓冲装不下 = 我们自己切块
   *   切大了（已把 MIMO_TTS_CHUNK_CHARS 降到 32），不该连累后端选择。 */
  return ret != OK && ret != -EINVAL && ret != -ENOSPC && ret != -ENOENT &&
         ret != -EPROTO;
}

/* 把一块文本交给**当前**后端合成。text 不必 NUL 结尾（是回复里的一个切片）。 */

static int tts_chunk_via_active_backend(const char *text, size_t text_len,
                                        unsigned char *pcm_out,
                                        size_t pcm_cap, size_t *pcm_len)
{
  char buf[TTS_FALLBACK_CHUNK_MAX];

  if (text == NULL || pcm_out == NULL || pcm_len == NULL ||
      text_len == 0 || text_len >= sizeof(buf))
    {
      return -EINVAL;
    }

  memcpy(buf, text, text_len);
  buf[text_len] = '\0';

  int ret = voice_tts_speak(buf, pcm_out, pcm_cap, pcm_len);

  /* ★ 2026-11-01：火山 TTS 这个 key 只开通了 ASR，TTS 返回 403
   *   「requested resource not granted」。降级过去只会让整机无声 ——
   *   第一次失败就切回 MiMo，并且只切一次（s_volc_tts_bad），避免
   *   "MiMo 失败 → 火山 → 切回 → MiMo 失败" 来回振荡。 */

  if (ret < 0 && s_tts_demoted && !s_tts_volc_primary && !s_volc_tts_bad)
    {
      s_volc_tts_bad = true;
      printf("[TTS] 火山 TTS 合成失败(%d)，切回 MiMo（火山 TTS 资源未开通？）\n",
             ret);

      if (voice_tts_set_backend("mimo") == 0)
        {
          s_tts_demoted = false;
          ret = voice_tts_speak(buf, pcm_out, pcm_cap, pcm_len);
        }
    }

  return ret;
}

static void tts_demote(const char *why)
{
  if (s_tts_demoted)
    {
      return;
    }

  /* ★ 这一行是新旧固件的分水岭，也是"到底降没降级"的唯一硬判据：
   *   串口看到它 = 这一版落地了、而且真撞上了 MiMo 的硬错。 */

  printf("[TTS] MiMo 合成失败（%s），本次开机降级到火山引擎"
         "（不再回头打 MiMo）\n", why);

  if (voice_tts_set_backend("volcengine") != 0)
    {
      printf("[TTS] 降级失败：volcengine 不在后端注册表里，仍留在 MiMo\n");
      return;
    }

  s_tts_demoted = true;
}

/****************************************************************************
 * Name: mimo_tts_synthesize
 *
 * Description:
 *   voice_tts_ops_t.synthesize：把整段回复合成成 16 kHz / 单声道 / s16le，
 *   全部写进调用方的 pcm_out。
 *
 *   分四步：
 *     1. 按 MIMO_TTS_TEXT_MAX 截断（超过就在日志里报出"后面还有多少字没念"）；
 *     2. 按**句子边界**切块（一块最多 MIMO_TTS_CHUNK_CHARS 个字），一块一次
 *        HTTPS —— 一次送整段的话，两三百字的响应就有两三 MB，堆里放不下；
 *     3. 每一块的 PCM 直接追加在上一块后面（顺序拼接 = 整段音频），中间不再
 *        多一份"完整时长"的临时缓冲；
 *     4. 调用方的缓冲装不下时**停下来并报出念到第几个字**，绝不越界写。
 *
 *   失败处理：第一块就失败 → 返回负值（调用方会告诉用户合成失败）；后面的块
 *   失败 → 只保留已经合好的前面几段（能念多少念多少），返回 OK 并打警告。
 *   一路的块大小、字数、时长都在日志里。
 *
 ****************************************************************************/

static int mimo_tts_synthesize(const char *text, unsigned char *pcm_out,
                               size_t pcm_cap, size_t *pcm_len)
{
  char spoken[MIMO_TTS_TEXT_MAX * 4 + 8];   /* 一个字最多 4 字节 + 省略号 */
  size_t text_len;
  size_t spoken_len;
  size_t total_chars;
  size_t spoken_chars;
  size_t done_chars = 0;      /* 已经合出去的字数 */
  size_t offset = 0;          /* spoken 里当前这一块的起点（字节） */
  size_t used = 0;            /* 已经写进 pcm_out 的字节数 */
  size_t chunk_no = 0;
  bool truncated = false;
  int ret;

  if (text == NULL || pcm_out == NULL || pcm_len == NULL || pcm_cap == 0)
    {
      return -EINVAL;
    }

  *pcm_len = 0;

  if (mimo_load_config() != OK)
    {
      syslog(LOG_ERR, "[%s] TTS: 配置里缺 llm_host / api_key，MiMo 不可用\n",
             TAG);
      return -ENOENT;
    }

  text_len = strlen(text);
  if (text_len == 0)
    {
      return -EINVAL;
    }

  /* 1. 太长就只念前面一段，并**明确报出截掉了多少字**：调用方（robot_ui）
   *    拿不到这里的细节，用户只能从串口日志里看。 */

  total_chars = utf8_count(text);

  spoken_len = tts_truncate(text, spoken, sizeof(spoken), MIMO_TTS_TEXT_MAX,
                            &truncated);
  if (spoken_len == 0)
    {
      return -EINVAL;
    }

  if (truncated)
    {
      /* tts_truncate 末尾补的省略号不算"念出来的字"，按上限算更贴近实际 */
      spoken_chars = MIMO_TTS_TEXT_MAX;

      syslog(LOG_WARNING,
             "[%s] TTS: 回复 %zu 字超过一次能念的上限 %d 字，只念前 %d 字，"
             "后面 %zu 字不念（对话区显示的仍是完整回复）\n",
             TAG, total_chars, MIMO_TTS_TEXT_MAX, MIMO_TTS_TEXT_MAX,
             total_chars - MIMO_TTS_TEXT_MAX);
    }
  else
    {
      spoken_chars = total_chars;
    }

  syslog(LOG_INFO,
         "[%s] TTS: 待合成 %zu 字 / %zu 字节，输出缓冲 %zu 字节（约 %zu 秒，"
         "按 %d 字节每字估）\n",
         TAG, spoken_chars, spoken_len, pcm_cap,
         (pcm_cap / (MIMO_PCM_RATE * 2)), MIMO_TTS_PCM_PER_CHAR);

  /* 2. 一块一块合成：每块的长度由"调用方还剩多少空间"和 MIMO_TTS_CHUNK_CHARS
   *    一起定，再按句子边界收一收（见 tts_split_len）。 */

  while (offset < spoken_len)
    {
      size_t remain = pcm_cap - used;
      size_t fit_chars;
      size_t chunk_len;
      size_t chunk_chars;
      size_t got = 0;
      size_t full = 0;

      /* 按保守估算算"还放得下几个字"：放不下一整字就收工（宁可不念，
       * 也不写到缓冲外面） */
      fit_chars = remain / MIMO_TTS_PCM_PER_CHAR;
      if (fit_chars == 0)
        {
          syslog(LOG_WARNING,
                 "[%s] TTS: 输出缓冲只剩 %zu 字节，放不下下一个字，"
                 "只念了前 %zu 字（共 %zu 字）\n",
                 TAG, remain, done_chars, spoken_chars);
          break;
        }

      if (fit_chars > MIMO_TTS_CHUNK_CHARS)
        {
          fit_chars = MIMO_TTS_CHUNK_CHARS;
        }

      chunk_len = tts_split_len(spoken + offset, spoken_len - offset,
                                fit_chars);
      if (chunk_len == 0)
        {
          break;
        }

      chunk_chars = utf8_count_n(spoken + offset, chunk_len);
      chunk_no++;

      ret = tts_synth_chunk(spoken + offset, chunk_len, pcm_out + used,
                            remain, &got, &full);
      if (ret != OK)
        {
          if (used == 0)
            {
              /* 第一块就没合出来：整段没声，让调用方按失败处理 */
              return ret;
            }

          syslog(LOG_WARNING,
                 "[%s] TTS: 第 %zu 块（从第 %zu 字起）合成失败 %d，"
                 "只播前面已经合好的 %zu 字\n",
                 TAG, chunk_no, done_chars + 1, ret, done_chars);
          break;
        }

      used += got;
      offset += chunk_len;
      done_chars += chunk_chars;

      if (got < full)
        {
          /* 缓冲被这一块填满了（估算偏乐观，或者调用方缓冲本来就小）：
           * 写到哪儿算哪儿，后面不再合 */
          syslog(LOG_WARNING,
                 "[%s] TTS: 输出缓冲放不下第 %zu 块，只写进去 %zu/%zu 字节，"
                 "大约念到第 %zu 字（共 %zu 字）\n",
                 TAG, chunk_no, got, full, done_chars, spoken_chars);
          break;
        }

      syslog(LOG_INFO, "[%s] TTS: 第 %zu 块 %zu 字 -> %zu 字节 PCM（累计 %zu）\n",
             TAG, chunk_no, chunk_chars, got, used);
    }

  if (used == 0)
    {
      syslog(LOG_ERR, "[%s] TTS: 一段都没合成出来（文本 %zu 字）\n", TAG,
             spoken_chars);

      /* ★ 2026-10-31：一段都没合成出来，说明是后端那头的硬错（配额/HTTP）。
       *   降级到火山后**整段重来一次**，而不是把失败丢回给调用方 ——
       *   调用方（prompt_ack / 关怀 / 追问）拿到失败就只放一声提示音，
       *   老人听到的是"叮"而不是话。 */

      tts_demote("整段都没合成出来");

      if (s_tts_demoted)
        {
          return voice_tts_speak(text, pcm_out, pcm_cap, pcm_len);
        }

      return -EIO;
    }

  if (done_chars < spoken_chars)
    {
      syslog(LOG_WARNING,
             "[%s] TTS: 只念了 %zu/%zu 字（输出缓冲 %zu 字节不够）\n",
             TAG, done_chars, spoken_chars, pcm_cap);
    }

  *pcm_len = used;

  syslog(LOG_INFO, "[%s] TTS: 输出 16k PCM %zu 字节（约 %zu ms，%zu 块）\n",
         TAG, *pcm_len, *pcm_len * 1000 / (MIMO_PCM_RATE * 2), chunk_no);
  return OK;
}

/****************************************************************************
 * Name: tts_text_sanitize
 *
 * Description:
 *   出声前的文本清洗：把 TTS 会照着念出来的格式记号从文本里去掉。原地改写，
 *   结果长度一定 <= 原长度（只删不加），不会写越界。
 *
 *   要删两类东西：
 *
 *   1. **Markdown 记号**（`**粗体**`、`*斜体*`、行首 `#` 标题、行首 `- ` 列表）
 *      —— 汉字念得出来，星号井号念不出来，只能是噪音。
 *
 *   2. **JSON 整块** —— 工具结果是 `{"ok":false,"error":{...}}` 这种机器格式。
 *      模型有时会把它原样抄进回复（工具出错时尤其常见），TTS 就一个字符一个
 *      字符念出来，老人听到的是一串「引号 ok 冒号 false」，完全不知所云。
 *      这是「工具错误 JSON 被当回答念出来」那个 bug 的兜底：不管它从哪条路
 *      漏进来，出声前一律掐掉。
 *
 *   Markdown 那部分与 packages/ai_agent 的
 *   `voice/voice_channel.c:tts_strip_markdown` 同源。那边是给 ai_agent 自己的
 *   播报栈用的，**我们板子上那个栈的音频后端是空壳**（`media_player_open` 全
 *   构建只有 weak 空壳恒返回 NULL，见 audio_playback.c:57），出不了声，所以把
 *   清洗逻辑搬到我们自己的共享出声路径上，两条对话链路一起受益。
 *
 * Input Parameters:
 *   s - 待清洗的 NUL 结尾字符串，原地改写；NULL 时直接返回
 *
 * Returned Value:
 *   无。若整个文本都是机器格式（清完变空），调用方原有的「空回复」兜底照旧
 *   生效，不会替调用方编一句话出来。
 *
 ****************************************************************************/

void tts_text_sanitize(char *s)
{
  char *r = s;
  char *w = s;

  if (s == NULL)
    {
      return;
    }

  while (*r != '\0')
    {
      /* JSON 对象 / 数组整块丢掉。按括号配对找收尾，双引号里的括号不计数
       * （字符串里的 `{` `}` 不能把配对搞乱），反斜杠转义也认。 */

      if (*r == '{' || *r == '[')
        {
          char open = *r;
          char close = (open == '{') ? '}' : ']';
          int depth = 0;
          bool in_str = false;

          while (*r != '\0')
            {
              if (in_str)
                {
                  if (*r == '\\' && r[1] != '\0')
                    {
                      r += 2;
                      continue;
                    }
                  if (*r == '"')
                    {
                      in_str = false;
                    }
                  r++;
                  continue;
                }

              if (*r == '"')
                {
                  in_str = true;
                  r++;
                  continue;
                }

              if (*r == open)
                {
                  depth++;
                }
              else if (*r == close)
                {
                  depth--;
                  if (depth == 0)
                    {
                      r++;
                      break;
                    }
                }

              r++;
            }

          /* 块被抠掉后补一个空格，免得前后两个词粘成一个（"好的{"ok":1}行"
           * 不能变成"好行"）。已经在行首或已有空白就不补。 */

          if (w != s && w[-1] != ' ' && w[-1] != '\n')
            {
              *w++ = ' ';
            }
          continue;
        }

      /* Markdown：跳过 **（粗体标记） */

      if (r[0] == '*' && r[1] == '*')
        {
          r += 2;
          continue;
        }

      /* 跳过孤立的 *（斜体标记），但 "3*4" 这种前后都是数字的乘号要留住
       * （判据抄自 ai_agent 的 tts_strip_markdown）。 */

      if (r[0] == '*')
        {
          if ((r == s || !isdigit((unsigned char)r[-1]))
              || !isdigit((unsigned char)r[1]))
            {
              r++;
              continue;
            }
        }

      /* 跳过行首的 #（标题标记） */

      if (r[0] == '#' && (r == s || r[-1] == '\n'))
        {
          while (*r == '#' || *r == ' ')
            {
              r++;
            }
          continue;
        }

      /* 跳过行首的 "- "（列表符号） */

      if (r[0] == '-' && r[1] == ' ' && (r == s || r[-1] == '\n'))
        {
          r += 2;
          continue;
        }

      *w++ = *r++;
    }

  *w = '\0';

  if (w != r)
    {
      syslog(LOG_INFO, "[%s] TTS: 出声前清掉 %d 个字符的格式/JSON 记号\n",
             TAG, (int)(r - w));
    }
}

/****************************************************************************
 * Name: mimo_tts_speak_head
 *
 * Description:
 *   只合成**一块**（用法和设计动机见 mimo_voice.h 的声明处）。
 *   切句逻辑与 mimo_tts_synthesize 的块循环完全一致（tts_split_len 按
 *   句子边界收），所以"逐块拼起来"念的效果和整段一次合成完全一样。
 *
 *   ⚠ 名字叫 speak_head，但流水线**每一块都调它**（首块 + 后续块，见
 *   ai_companion_main.c 生产者循环 4476 与 robot_ui/main.c 1895）。所以上限
 *   必须由调用方传进来：首块传 MIMO_TTS_HEAD_CHUNK_CHARS（小，只为让第一声
 *   早点出来），后续块传 MIMO_TTS_CHUNK_CHARS（大，块间空档少）。
 *   2026-10-25 之前上限是写死的，于是后续块一直被卡在首块那个小值上。
 *
 ****************************************************************************/

int mimo_tts_speak_head(const char *text, unsigned char *pcm_out,
                        size_t pcm_cap, size_t max_chars, size_t *pcm_len,
                        size_t *consumed_bytes)
{
  size_t fit_chars;
  size_t chunk_len;
  size_t got = 0;
  size_t full = 0;
  int ret;

  if (text == NULL || pcm_out == NULL || pcm_len == NULL ||
      consumed_bytes == NULL || pcm_cap == 0 || max_chars == 0)
    {
      return -EINVAL;
    }

  *pcm_len = 0;
  *consumed_bytes = 0;

  if (mimo_load_config() != OK)
    {
      syslog(LOG_ERR, "[%s] TTS 首句: 配置里缺 llm_host / api_key，MiMo 不可用\n",
             TAG);
      return -ENOENT;
    }

  if (text[0] == '\0')
    {
      return -EINVAL;
    }

  fit_chars = pcm_cap / MIMO_TTS_PCM_PER_CHAR;
  if (fit_chars == 0)
    {
      return -ENOSPC;
    }

  /* 上限由调用方传（见函数头的 ⚠）：首块传 MIMO_TTS_HEAD_CHUNK_CHARS，
   * 后续块传 MIMO_TTS_CHUNK_CHARS。
   *
   * ★ 2026-10-25 修：这里原来写死 MIMO_TTS_HEAD_CHUNK_CHARS，而流水线每一块
   * 都调本函数 —— 于是"后续块用 MIMO_TTS_CHUNK_CHARS"那句注释从来没生效过，
   * 真机每块都卡在 20 字（日志里最大块 60 字节 = 20 个汉字）。改宏是空操作，
   * 只有把上限做成参数才真正生效。
   *
   * fit_chars 还会被上面那步「缓冲还放得下几个字」压得更小 —— 两个上限取
   * 小的那个。所以槽不够大时是**优雅降级**（块变小），不是写越界、也不会
   * 截断成半截音频。 */
  /* ★ 2026-11-01：火山和 MiMo **都用调用方传的上限（32 字）**。
   *
   *   上一版这里把火山放大到 64 字（192 字节），真机长回复时火山对
   *   140~192 字节的块返回 `-22`，每一块都回落到 MiMo；MiMo 每块 10~16 秒，
   *   流水线跟不上，25 秒 TTS 停滞判据把整轮作废 —— 用户听到的就是
   *   "话没说完就停了"。
   *   实测火山对 40~96 字节（32 字以内）的块全部成功，所以这里不再放大。
   *   块小一点只多几次 HTTPS，火山单块 2~4 秒，流水线跟得上。 */

  if (fit_chars > max_chars)
    {
      fit_chars = max_chars;
    }

  chunk_len = tts_split_len(text, strlen(text), fit_chars);
  if (chunk_len == 0)
    {
      return -EINVAL;
    }

  if (s_tts_volc_primary)
    {
      /* ★ 2026-11-01：**火山「语音合成模型2.0」优先**（用户已开通该产品）。
       *   本块交给当前后端 = volcengine（X-Api-Key + seed-tts-2.0 + 2.0 音色）。
       *   失败则**这一块**直连 MiMo 兜底 —— 火山抖一下不至于整句哑掉，
       *   反过来 MiMo 的配额/截断问题也不会再把 TTS 永久打死。 */

      ret = tts_chunk_via_active_backend(text, chunk_len, pcm_out, pcm_cap,
                                         &got);
      if (ret != OK)
        {
          printf("[TTS] 火山 2.0 合成失败(%d)，这一块改用 MiMo\n", ret);
          ret = tts_synth_chunk(text, chunk_len, pcm_out, pcm_cap, &got,
                                &full);
        }
    }
  else if (s_tts_demoted)
    {
      /* 已经降级过：本块直接交给当前后端（火山），MiMo 这条路一次都不走。 */

      ret = tts_chunk_via_active_backend(text, chunk_len, pcm_out, pcm_cap,
                                         &got);
    }
  else
    {
      ret = tts_synth_chunk(text, chunk_len, pcm_out, pcm_cap, &got, &full);

      if (ret != OK && tts_error_is_backend_fault(ret))
        {
          /* ★ 2026-10-31：MiMo 侧硬错（配额 429 之类）→ 降级火山后，
           *   **这一块**就地重试，别让"第一声"就此哑掉。 */

          tts_demote("TTS 首句合成报硬错");

          if (s_tts_demoted)
            {
              ret = tts_chunk_via_active_backend(text, chunk_len, pcm_out,
                                                 pcm_cap, &got);
            }
        }
    }

  if (ret != OK)
    {
      syslog(LOG_WARNING, "[%s] TTS 首句合成失败: %d\n", TAG, ret);
      return ret;
    }

  *pcm_len = got;
  *consumed_bytes = chunk_len;

  syslog(LOG_INFO,
         "[%s] TTS: 块 %zu 字节 -> %zu 字节 PCM（约 %zu ms）\n",
         TAG, chunk_len, got, got * 1000 / (MIMO_PCM_RATE * 2));
  return OK;
}

/****************************************************************************
 * Name: mimo_init
 *
 * Description:
 *   后端 init 回调：只做一次配置检查 + 打日志。失败不算注册失败
 *   （分发层忽略返回值），真正调用时还会再检查一遍。
 *
 ****************************************************************************/

static int mimo_init(void)
{
  int ret = mimo_load_config();

  if (ret != OK)
    {
      syslog(LOG_WARNING,
             "[%s] 未配置 llm_host / api_key，MiMo 语音后端调用会返回 -ENOENT\n",
             TAG);
      return ret;
    }

  /* 只打 host / path，不打 key */

  syslog(LOG_INFO, "[%s] MiMo 后端就绪: host=%s path=%s port=%s\n", TAG,
         s_host, s_path, s_port);
  return OK;
}

/****************************************************************************
 * Private Data（后端 ops）
 ****************************************************************************/

static const voice_asr_ops_t s_mimo_asr_ops = {
  .name      = MIMO_BACKEND_NAME,
  .init      = mimo_init,
  .recognize = mimo_asr_recognize,
  .deinit    = NULL,
};

static const voice_tts_ops_t s_mimo_tts_ops = {
  .name       = MIMO_BACKEND_NAME,
  .init       = mimo_init,
  .synthesize = mimo_tts_synthesize,
  .deinit     = NULL,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int mimo_asr_register(void)
{
  return voice_asr_register(&s_mimo_asr_ops);
}

int mimo_tts_register(void)
{
  return voice_tts_register(&s_mimo_tts_ops);
}

void mimo_tts_prefer_volc(int on)
{
  s_tts_volc_primary = on ? true : false;

  if (s_tts_volc_primary)
    {
      /* 用户已开通「语音合成模型2.0」：火山优先，MiMo 只做单块兜底。
       * 每次切换都重新给火山一次机会（不要被上一轮的历史状态钉死）。 */
      s_volc_tts_bad = false;
      s_tts_demoted  = false;
    }
}

int mimo_voice_available(void)
{
  char host[128];
  char key[128];

  host[0] = '\0';
  key[0] = '\0';

  if (claw_config_get(AGENT_CFG_KEY_LLM_HOST, host, sizeof(host)) != OK
      || host[0] == '\0')
    {
      return 0;
    }

  if (claw_config_get(AGENT_CFG_KEY_API_KEY, key, sizeof(key)) != OK
      || key[0] == '\0')
    {
      return 0;
    }

  return 1;
}

int mimo_wav_load_16k(const char *path, unsigned char **pcm_out,
                      size_t *pcm_len)
{
  FILE *fp;
  long size;
  unsigned char *raw;
  size_t got;
  int ret;

  if (path == NULL || pcm_out == NULL || pcm_len == NULL)
    {
      return -EINVAL;
    }

  *pcm_out = NULL;
  *pcm_len = 0;

  fp = fopen(path, "rb");
  if (fp == NULL)
    {
      syslog(LOG_ERR, "[%s] 打不开 WAV: %s (%d)\n", TAG, path, errno);
      return -ENOENT;
    }

  if (fseek(fp, 0, SEEK_END) != 0)
    {
      fclose(fp);
      return -EIO;
    }

  size = ftell(fp);
  if (size <= MIMO_WAV_HDR_LEN || size > MIMO_WAV_MAX_FILE)
    {
      syslog(LOG_ERR, "[%s] WAV 大小不合适: %ld 字节（上限 %d）\n", TAG,
             size, MIMO_WAV_MAX_FILE);
      fclose(fp);
      return -EINVAL;
    }

  rewind(fp);

  raw = malloc((size_t)size);
  if (raw == NULL)
    {
      fclose(fp);
      return -ENOMEM;
    }

  got = fread(raw, 1, (size_t)size, fp);
  fclose(fp);

  if (got != (size_t)size)
    {
      syslog(LOG_ERR, "[%s] 读 WAV 只拿到 %zu/%ld 字节\n", TAG, got, size);
      free(raw);
      return -EIO;
    }

  ret = wav_extract_16k(raw, got, pcm_out, pcm_len);
  free(raw);

  if (ret != OK)
    {
      syslog(LOG_ERR, "[%s] WAV 解析/重采样失败: %d\n", TAG, ret);
      return ret;
    }

  syslog(LOG_INFO, "[%s] %s -> %zu 字节 16k 单声道 PCM\n", TAG, path,
         *pcm_len);
  return OK;
}

/****************************************************************************
 * Name: sjoin_add / sjoin_addstr / sjoin_add_escaped / str_last
 *
 * Description:
 *   拼请求体用的小工具（请求体是手拼的，没引 cJSON）。缓冲区一旦装不下就
 *   置 ovf 并停止写入，调用方拼完检查一次 ovf 决定成败 —— 比每处手算剩余
 *   长度省心，也不会写出半个 JSON。
 *   每块的余量沿用文件里"最坏放大 6 倍"的老规矩（一个字节可能变成 \u00XX）。
 *
 ****************************************************************************/

struct sjoin_s
{
  char  *buf;    /* 输出缓冲 */
  size_t cap;    /* 含结尾 '\0' */
  size_t len;    /* 已写入字节数（不含结尾 '\0'） */
  bool   ovf;    /* true = 溢出，内容已不可信 */
};

static void sjoin_add(struct sjoin_s *sj, const char *src, size_t len)
{
  if (sj->ovf || len > sj->cap - 1 - sj->len)
    {
      sj->ovf = true;
      return;
    }

  memcpy(sj->buf + sj->len, src, len);
  sj->len += len;
  sj->buf[sj->len] = '\0';
}

static void sjoin_addstr(struct sjoin_s *sj, const char *src)
{
  sjoin_add(sj, src, strlen(src));
}

/* 把 src 当 JSON 字符串内容转义后追加（两边的引号由调用方拼） */

static void sjoin_add_escaped(struct sjoin_s *sj, const char *src, size_t len)
{
  size_t room;

  if (sj->ovf)
    {
      return;
    }

  room = sj->cap - 1 - sj->len;

  /* json_escape() 装不下时是静默停手（会拼出"看着合法但内容不全"的 JSON），
   * 所以先按最坏放大 6 倍确认装得下 */

  if (len > room / 6)
    {
      sj->ovf = true;
      return;
    }

  sj->len += json_escape(src, len, sj->buf + sj->len, room);
  sj->buf[sj->len] = '\0';
}

/* 最后一次出现的位置：模型的思维链里也可能出现要找的词（比如 tool_calls），
 * 而 JSON 里真正的那个键在 message 的最后 */

static const char *str_last(const char *hay, const char *needle)
{
  const char *hit = NULL;
  const char *p = hay;

  while ((p = strstr(p, needle)) != NULL)
    {
      hit = p;
      p++;
    }

  return hit;
}

/****************************************************************************
 * Name: json_find_number_text
 *
 * Description:
 *   取 <key> 后面的 JSON 数字**原文**（"21.5" / "39.9075"），不解析成 double：
 *   这版 NuttX 的 printf 不一定编进了 %f，而 open-meteo 给的就是一两位小数，
 *   原文直接拼进中文句子既好看又省事。
 *   键后面不是数字（字符串 / null / 对象）也算没找到 —— 比如 weather_code 的
 *   单位在 "current_units" 里是个字符串。
 *
 ****************************************************************************/

static int json_find_number_text(const char *json, const char *key,
                                 char *out, size_t out_cap)
{
  char pat[64];
  const char *p;
  size_t n = 0;

  if (json == NULL || key == NULL || out == NULL || out_cap < 2
      || strlen(key) + 3 > sizeof(pat))
    {
      return -EINVAL;
    }

  out[0] = '\0';
  snprintf(pat, sizeof(pat), "\"%s\"", key);

  p = strstr(json, pat);
  if (p == NULL)
    {
      return -ENOENT;
    }

  p += strlen(pat);

  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    {
      p++;
    }

  if (*p != ':')
    {
      return -ENOENT;
    }

  p++;

  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    {
      p++;
    }

  if (*p != '-' && *p != '+' && (*p < '0' || *p > '9'))
    {
      return -ENOENT;
    }

  while ((*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '+'
         || *p == 'e' || *p == 'E')
    {
      if (n + 1 >= out_cap)
        {
          break;
        }

      out[n++] = *p++;
    }

  out[n] = '\0';
  return n > 0 ? OK : -ENOENT;
}

/* "20.0" -> "20"，"21.50" -> "21.5"（纯文本剪尾巴，不做数值转换） */

static void num_trim_tail_zeros(char *s)
{
  size_t len = strlen(s);

  if (strchr(s, '.') == NULL)
    {
      return;
    }

  while (len > 0 && s[len - 1] == '0')
    {
      s[--len] = '\0';
    }

  if (len > 0 && s[len - 1] == '.')
    {
      s[--len] = '\0';
    }
}

/****************************************************************************
 * Name: url_encode
 *
 * Description:
 *   把城市名 percent-encode 进 URL query（中文一个字 3 字节 -> 9 个字符）。
 *   只放过 RFC 3986 的 unreserved（字母数字和 - _ . ~），其它一律 %XX：
 *   城市名里可能有空格、斜杠、&，不编码会把 query 拆坏。
 *
 ****************************************************************************/

static int url_encode(const char *src, char *dst, size_t dst_cap)
{
  static const char hexd[] = "0123456789ABCDEF";
  size_t o = 0;

  if (src == NULL || dst == NULL || dst_cap < 4)
    {
      return -EINVAL;
    }

  for (; *src != '\0'; src++)
    {
      unsigned char c = (unsigned char)*src;
      bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                   || (c >= '0' && c <= '9')
                   || c == '-' || c == '_' || c == '.' || c == '~';

      if (plain)
        {
          if (o + 1 >= dst_cap)
            {
              return -ENOSPC;
            }

          dst[o++] = (char)c;
        }
      else
        {
          if (o + 3 >= dst_cap)
            {
              return -ENOSPC;
            }

          dst[o++] = '%';
          dst[o++] = hexd[(c >> 4) & 0xf];
          dst[o++] = hexd[c & 0xf];
        }
    }

  dst[o] = '\0';
  return OK;
}

/****************************************************************************
 * Name: wmo_text
 *
 * Description:
 *   WMO weather code -> 中文短句。模型不认识数字码（问它 61 它不知道是雨），
 *   所以在板子上先翻译好再作为工具结果喂给它。
 *
 ****************************************************************************/

static const char *wmo_text(int code)
{
  if (code == 0)                return "晴";
  if (code == 1 || code == 2)   return "多云";
  if (code == 3)                return "阴";
  if (code == 45 || code == 48) return "有雾";
  if (code >= 51 && code <= 57) return "毛毛雨";
  if (code >= 61 && code <= 67) return "下雨";
  if (code >= 71 && code <= 77) return "下雪";
  if (code >= 80 && code <= 82) return "阵雨";
  if (code == 85 || code == 86) return "阵雪";
  if (code >= 95 && code <= 99) return "雷雨";
  return "天气状况未知";
}

/****************************************************************************
 * Name: weather_http_get
 *
 * Description:
 *   打一次 open-meteo 的 HTTPS GET（443）。请求前清一次连接池，理由和
 *   mimo_post() 里那段一样：池子复用死连接时排空只用 10ms 超时、不重连，
 *   真踩上要等满 120 秒的读超时。天气接口是另外两台机器、平时用不上池子，
 *   每次多花一次握手（几百毫秒）换掉这个风险更划算。
 *
 ****************************************************************************/

static int weather_http_get(const char *host, const char *path, char *resp,
                            size_t resp_cap)
{
  vela_tls_pool_cleanup();
  return vela_https_get(host, "443", path, resp, resp_cap);
}

/****************************************************************************
 * Name: weather_lookup
 *
 * Description:
 *   get_weather 工具的实现：geocode 拿经纬度 -> forecast 拿实况 -> 拼一句
 *   中文（例："北京：晴，气温 21.5℃，体感 20℃，湿度 25%，风速 7.3 km/h"）。
 *   两个接口都是 open-meteo 的免 key HTTPS GET，中文城市名直接进 query。
 *
 * Returned Value:
 *   OK  = out 里是给模型看的结果，**包含"没找到这个城市"这类失败原因**
 *         （让模型自己跟用户解释，不直接报错给用户）；
 *   <0  = 负 errno，只有网络层失败（DNS/TLS/连接）和内存不足才会走到这里。
 *
 ****************************************************************************/

static int weather_lookup(const char *city, char *out, size_t out_cap)
{
  char enc[MIMO_CITY_URL_MAX];
  char path[512];
  char lat[24];
  char lon[24];
  char name[MIMO_CITY_BUF_MAX];
  char temp[24];
  char feels[24];
  char humid[24];
  char wind[24];
  char code[24];
  const char *p;
  const char *val;
  char *resp;
  size_t n;
  int status;

  if (city == NULL || city[0] == '\0' || out == NULL || out_cap == 0)
    {
      return -EINVAL;
    }

  out[0]  = '\0';
  name[0] = '\0';

  if (url_encode(city, enc, sizeof(enc)) != OK)
    {
      snprintf(out, out_cap, "城市名太长，查不了天气");
      return OK;
    }

  /* 1. 地理编码：城市名 -> 经纬度 */

  snprintf(path, sizeof(path), MIMO_GEO_PATH_FMT, enc);

  resp = mimo_calloc_logged("chat.geo", MIMO_WEATHER_RESP_CAP);
  if (resp == NULL)
    {
      return -ENOMEM;
    }

  status = weather_http_get(MIMO_GEO_HOST, path, resp, MIMO_WEATHER_RESP_CAP);

  if (status < 0)
    {
      mimo_free_logged("chat.geo", resp, MIMO_WEATHER_RESP_CAP);
      return -EIO;
    }

  if (status != 200)
    {
      syslog(LOG_ERR, "[%s] 天气: 地理编码 HTTP %d\n", TAG, status);
      snprintf(out, out_cap, "天气服务暂时不可用（HTTP %d）", status);
      mimo_free_logged("chat.geo", resp, MIMO_WEATHER_RESP_CAP);
      return OK;
    }

  /* 只在 "results" 之后找：顶层还有 generationtime_ms 之类的数字键 */

  p = strstr(resp, "\"results\"");
  if (p == NULL
      || json_find_number_text(p, "latitude", lat, sizeof(lat)) != OK
      || json_find_number_text(p, "longitude", lon, sizeof(lon)) != OK)
    {
      syslog(LOG_INFO, "[%s] 天气: 没查到城市 %s\n", TAG, city);
      snprintf(out, out_cap, "没找到%s这个城市", city);
      mimo_free_logged("chat.geo", resp, MIMO_WEATHER_RESP_CAP);
      return OK;
    }

  val = json_find_string(p, "name", &n);
  if (val != NULL && n > 0)
    {
      n = json_unescape(val, n, name, sizeof(name) - 1);
      name[n] = '\0';
    }

  /* resp 在这里还，后面只用抄出来的经纬度和城市名 */

  mimo_free_logged("chat.geo", resp, MIMO_WEATHER_RESP_CAP);

  if (name[0] == '\0')
    {
      snprintf(name, sizeof(name), "%s", city);
    }

  /* 2. 实况：经纬度 -> 天气现象 / 气温 / 体感 / 湿度 / 风速 */

  snprintf(path, sizeof(path), MIMO_WEATHER_PATH_FMT, lat, lon);

  resp = mimo_calloc_logged("chat.wx", MIMO_WEATHER_RESP_CAP);
  if (resp == NULL)
    {
      return -ENOMEM;
    }

  status = weather_http_get(MIMO_WEATHER_HOST, path, resp,
                            MIMO_WEATHER_RESP_CAP);

  if (status < 0)
    {
      mimo_free_logged("chat.wx", resp, MIMO_WEATHER_RESP_CAP);
      return -EIO;
    }

  if (status != 200)
    {
      syslog(LOG_ERR, "[%s] 天气: 实况 HTTP %d\n", TAG, status);
      snprintf(out, out_cap, "天气服务暂时不可用（HTTP %d）", status);
      mimo_free_logged("chat.wx", resp, MIMO_WEATHER_RESP_CAP);
      return OK;
    }

  /* 必须在 "current" 之后找：前面 "current_units" 里同名的键是单位字符串
   * （"°C" / "%"），先找会拿到非数字 */

  p = strstr(resp, "\"current\":");
  if (p == NULL
      || json_find_number_text(p, "temperature_2m", temp, sizeof(temp)) != OK
      || json_find_number_text(p, "apparent_temperature", feels,
                               sizeof(feels)) != OK
      || json_find_number_text(p, "relative_humidity_2m", humid,
                               sizeof(humid)) != OK
      || json_find_number_text(p, "weather_code", code, sizeof(code)) != OK
      || json_find_number_text(p, "wind_speed_10m", wind, sizeof(wind)) != OK)
    {
      syslog(LOG_ERR, "[%s] 天气: 实况响应解析失败: %.*s\n", TAG,
             MIMO_ERR_DUMP_LEN, resp);
      snprintf(out, out_cap, "天气数据没拿到，请稍后再试");
      mimo_free_logged("chat.wx", resp, MIMO_WEATHER_RESP_CAP);
      return OK;
    }

  mimo_free_logged("chat.wx", resp, MIMO_WEATHER_RESP_CAP);

  num_trim_tail_zeros(temp);
  num_trim_tail_zeros(feels);
  num_trim_tail_zeros(humid);
  num_trim_tail_zeros(wind);

  snprintf(out, out_cap, "%s：%s，气温 %s℃，体感 %s℃，湿度 %s%%，风速 %s km/h",
           name, wmo_text(atoi(code)), temp, feels, humid, wind);

  syslog(LOG_INFO, "[%s] 天气: %s lat=%s lon=%s\n", TAG, name, lat, lon);
  return OK;
}

/****************************************************************************
 * Name: mimo_weather_lookup
 *
 * Description:
 *   ★ 2026-11-01：给 hello_app 的"天气快速通道"用的**公开**入口。
 *
 *   为什么需要它：问一句天气原来要走 velaclaw 的 ReAct（第一轮就可能 -110
 *   超时 45 秒）→ 直连兜底再跑一遍工具（又 45 秒），真机首字 **105 秒**。
 *   天气这种"一次 HTTP 就能答"的问题根本不该经过大模型决策链：
 *   ai_companion 识别到天气意图后直接调这里，查完把结果交给 TTS。
 *
 *   返回 OK = out 里是给老人念的一句话（也包含"没找到/不可用"这类失败说明）；
 *   <0 = 网络层失败。调用方看到失败/否定词就回落到原来的大模型路径。
 *
 ****************************************************************************/

int mimo_weather_lookup(const char *city, char *out, size_t out_cap)
{
  return weather_lookup(city, out, out_cap);
}

/****************************************************************************
 * Name: chat_config_flag_on
 *
 * Description:
 *   读配置里一个开关键：值是 "1" / "true" / "on"（大小写不敏感，前后空白先
 *   去掉）算开，别的值算关。键读不到时返回 missing_default —— 两个工具的
 *   默认值不一样（天气默认关、提醒默认开），差别就靠这个参数表达。
 *
 *   claw_config_get() 失败时不动缓冲区，所以先自己置空。
 *
 * Returned Value:
 *   true = 开；false = 关。
 *
 ****************************************************************************/

static bool chat_config_flag_on(const char *key, bool missing_default)
{
  char val[16];
  size_t i;

  val[0] = '\0';

  if (claw_config_get(key, val, sizeof(val)) != OK)
    {
      return missing_default;
    }

  trim_whitespace(val);

  /* 先折成小写再比：不引 strings.h，不动 strcasecmp，只处理 ASCII 字母
   * （这几个字面量里也没有非 ASCII） */

  for (i = 0; val[i] != '\0'; i++)
    {
      if (val[i] >= 'A' && val[i] <= 'Z')
        {
          val[i] = (char)(val[i] - 'A' + 'a');
        }
    }

  return strcmp(val, "1") == 0 || strcmp(val, "true") == 0
         || strcmp(val, "on") == 0;
}

/****************************************************************************
 * Name: chat_weather_tool_enabled
 *
 * Description:
 *   天气工具（get_weather）开不开：配置键 enable_weather_tool。
 *
 *   2026-09-14 改成**缺省开**：当初默认关是因为"open-meteo 查出来的实况跟用户
 *   所在地对不上"—— 那时用户只说"今天天气"时模型只能用写死的 default_city。
 *   现在有了 IP 定位（mimo_location.c），默认城市被真实定位到的城市顶替，
 *   这条前提没了，工具就该能用（用户也要求"把天气加进去"）。
 *   想关还是配 "0" / "false" / "off"。
 *
 * Returned Value:
 *   true = 带 get_weather 工具（默认）；false = 不带。
 *
 ****************************************************************************/

static bool chat_weather_tool_enabled(void)
{
  return chat_config_flag_on(MIMO_CFG_KEY_WEATHER_TOOL, true);
}

/****************************************************************************
 * Name: chat_reminder_tool_enabled
 *
 * Description:
 *   建提醒工具（add_reminder）开不开：配置键 enable_reminder_tool，
 *   **键缺省算开**（配成 "0" / "false" / "off" 才关）。
 *
 *   还有一条硬条件：必须有 app 注册了 mimo_set_reminder_hook()。没注册就
 *   没地方落地，这时**不能**把工具下发给模型 —— 模型调了也只能回它一句
 *   "建不了"，还不如让它直接回答"我记不住提醒"。
 *
 * Returned Value:
 *   true = 带 add_reminder 工具（默认，前提是 hook 已注册）。
 *
 ****************************************************************************/

static bool chat_reminder_tool_enabled(void)
{
  if (s_reminder_hook == NULL)
    {
      return false;
    }

  return chat_config_flag_on(MIMO_CFG_KEY_REMINDER_TOOL, true);
}

/****************************************************************************
 * Name: chat_setting_tool_enabled
 *
 * Description:
 *   语音改音量/亮度工具（adjust_setting）开不开：配置键 enable_setting_tool，
 *   **键缺省算开**（配成 "0" / "false" / "off" 才关）。
 *
 *   和 add_reminder 一样的硬条件：必须有 app 注册了 mimo_set_settings_hook()。
 *   没注册就没地方落地，这时**不能**把工具下发给模型 —— 模型调了也只能回它
 *   一句"改不了"，还不如让它直接回答"我改不了音量"。
 *
 * Returned Value:
 *   true = 带 adjust_setting 工具（默认，前提是 hook 已注册）。
 *
 ****************************************************************************/

static bool chat_setting_tool_enabled(void)
{
  if (s_settings_hook == NULL)
    {
      return false;
    }

  return chat_config_flag_on(MIMO_CFG_KEY_SETTING_TOOL, true);
}

/****************************************************************************
 * Name: chat_default_city / chat_build_system_prompt
 *
 * Description:
 *   系统提示是"让模型靠谱"的关键，由几段拼成（prompt_append 逐段追加）：
 *     1) 说话要求 —— 口语化中文、不要 emoji / markdown（屏幕字库没有 emoji，
 *        TTS 也念不出来）。跟工具开不开无关；
 *     2) 当前北京时间 —— 本构建里 localtime 就是 gmtime（romfs 没有 zoneinfo），
 *        所以自己 +8 小时再格式化（星期也一并给）。时间写在提示里而不是做成
 *        工具："今天几号 / 现在几点 / 今天星期几"照抄就行，不用再打一次网络；
 *     3) 用户所在城市（loc_city）—— 板子自己用 IP 定位查的（mimo_location.c），
 *        **查到了才写，查不到一个字都不提**（模型不知道就不会瞎编一个城市）；
 *     4) 提醒工具（use_reminder）—— 让模型把口语时间折算成 24 小时制再调
 *        add_reminder，并复述一遍确认。没给这个工具时这段必须拿掉；
 *     5) 默认城市 + 天气工具（use_weather）—— 只有开了 get_weather 才加配置库
 *        的 default_city（读不到用 MIMO_DEFAULT_CITY），并写明"实时天气必须查
 *        工具"；没开时改成"这类你答不了，别编"；
 *     6) 改音量/亮度工具（use_setting）—— 让模型知道"声音小一点"是**真能改**
 *        的，别答成"您可以在设置里调"。没给这个工具时这段必须拿掉。
 *
 ****************************************************************************/

static void chat_default_city(char *out, size_t out_cap)
{
  if (out == NULL || out_cap == 0)
    {
      return;
    }

  /* claw_config_get() 失败时不动缓冲区，所以先自己置空 */

  out[0] = '\0';

  if (claw_config_get("default_city", out, out_cap) != OK || out[0] == '\0')
    {
      snprintf(out, out_cap, MIMO_DEFAULT_CITY);
    }
}

/* 往提示串尾部追加一段（纯字符串，不做格式化 —— 要格式化先自己 snprintf 进
 * tmp 再传进来）。装不下就停下不再追加：提示短一点只是"说得不那么细"，
 * 不能因为截断把 out 写成非 '\0' 结尾。返回新的已用长度。 */

static size_t prompt_append(char *out, size_t out_cap, size_t used,
                            const char *src)
{
  size_t n = strlen(src);

  if (used + n >= out_cap)
    {
      return used;
    }

  memcpy(out + used, src, n);
  used += n;
  out[used] = '\0';
  return used;
}

static void chat_build_system_prompt(bool use_weather, bool use_reminder,
                                     bool use_setting, const char *city,
                                     const char *loc_city,
                                     char *out, size_t out_cap)
{
  static const char *WEEKDAY[] = { "日", "一", "二", "三", "四", "五", "六" };
  struct tm tm_bj;
  time_t bj = time(NULL) + 8 * 3600;
  char now[64];
  char tmp[512];
  size_t used = 0;

  if (out == NULL || out_cap == 0)
    {
      return;
    }

  gmtime_r(&bj, &tm_bj);
  snprintf(now, sizeof(now), "%04d-%02d-%02d %02d:%02d",
           tm_bj.tm_year + 1900, tm_bj.tm_mon + 1, tm_bj.tm_mday,
           tm_bj.tm_hour, tm_bj.tm_min);

  out[0] = '\0';

  /* 说话要求：口语化中文、不要 emoji / markdown（屏幕字库没有 emoji，
   * TTS 也念不出来）。这段跟工具开不开无关，每轮都在。
   * "一律简体中文"是硬性规定单列一段（2026-10-14 用户实测：中文问题偶尔
   * 得到英文回答）——光说"用口语化的中文说话"约束力不够，模型遇到被 ASR
   * 识别成英文的输入会顺坡下驴用英文回；写死"识别成什么都要用中文回"。 */

  used = prompt_append(out, out_cap, used,
                       "你是老人陪伴机器人里的语音助手，用口语化的中文说话，"
                       "简短直接，一般一两句话就够。不要用 emoji，也不要用"
                       "星号或者 markdown 列表，因为屏幕字库和语音都不支持"
                       "这些符号。");

  /* ★ 自我介绍措辞（2026-10-31 用户指定，**一字不改**）。
   *
   *   用户原话：「这是 ai 自我介绍措辞：我是智爱助手，可以和您聊天，答时间，
   *   查天气，设提醒，紧急响铃联系家人，上网搜索消息，还能修改音量和亮度，
   *   有什么需要帮忙的的和我说。」
   *
   *   原来模型是**自己现编**的（实测答过「和您聊天，答时间，查天气，设提醒，
   *   紧急响铃联系家人，上网搜索消息，还能修改音量和亮度。」），每次措辞都不
   *   一样，用户点名要固定下来。
   *
   *   ⚠ 末句「有什么需要帮忙的的和我说」里有个**叠字"的的"**，看着像笔误。
   *     但这是用户给的原话，先照抄 —— 想改成「有什么需要帮忙的和我说」，
   *     只动下面这一个串即可（和 voice_emerg.h 的开关一样，一处改全生效）。
   *
   *   放在提示词**开头**（而不是末尾）：缓冲是 2048 字节，万一段落超长被
   *   prompt_append 截断，先丢的得是后面那些工具说明，不能是这一句。 */

  used = prompt_append(out, out_cap, used,
                       "用户问「你是谁」「你能干什么」「你会什么」「你都会"
                       "什么」这类自我介绍的问题时，必须用下面这一句回答，"
                       "一字不改，也不要再补充别的："
                       "「我是智爱助手，可以和您聊天，答时间，查天气，设提醒，"
                       "紧急响铃联系家人，上网搜索消息，还能修改音量和亮度，"
                       "有什么需要帮忙的的和我说。」");

  /* 2026-10-19 加的硬上限：语音一次最多念 MIMO_TTS_TEXT_MAX=300 字
   * （2 MB PCM 缓冲 ÷ 16 kHz 的物理限制），模型不守"简短"约定时长回复
   * 会被拦腰截断 —— 用户实测"长答案只念一部分"。留 20 字余量写 280，
   * 让"屏幕显示多少 = 念到多少"成立。 */

  used = prompt_append(out, out_cap, used,
                       "回答最多 280 个字。内容多的时候挑最重要的先说完整，"
                       "宁可少说几条，也不要每条都说一半 —— 说一半的话"
                       "语音念不完，用户既听不到也看不到。");

  used = prompt_append(out, out_cap, used,
                       "任何时候都必须用简体中文回答，这是硬性规定。"
                       "即使用户的话被语音识别成了英文、拼音或者错字，"
                       "也必须用简体中文回复，绝对不要用英文回答。");

  /* 当前北京时间：放提示里而不是做成工具 —— "今天几号 / 现在几点 /
   * 今天星期几"照抄就行，不用再打一次网络 */

  snprintf(tmp, sizeof(tmp),
           "现在是 %s（北京时间，星期%s）。用户问今天几号、现在几点、"
           "今天星期几时，直接用上面给你的时间回答，不要说不知道。",
           now, WEEKDAY[tm_bj.tm_wday]);
  used = prompt_append(out, out_cap, used, tmp);

  /* 用户所在城市（板子用 IP 定位查的，见 mimo_location.c）：**只有查到才写
   * 这一段**，查不到一个字都不提 —— 不写它，模型就只会说不知道，不会编一个
   * 城市出来。接口给的城市名可能是英文或拼音（例如 Hangzhou），让模型自己
   * 换成中文再说，免得对着老人冒出个英文地名。
   * 天气那半句只在开着 get_weather 时写：工具关着时提示下面明说了"实时天气
   * 你答不了"，这里再提"以这个城市为准"就是让它编一个天气出来。 */

  if (loc_city != NULL && loc_city[0] != '\0')
    {
      if (use_weather)
        {
          snprintf(tmp, sizeof(tmp),
                   "用户所在城市：%s（IP 定位得到）。用户问“我在哪”这类问题时，"
                   "就用这个城市回答；问天气而没说城市时，也以这个城市为准。"
                   "这个城市名可能是英文或者拼音，回答时换成对应的中文城市名，"
                   "别直接念英文。", loc_city);
        }
      else
        {
          snprintf(tmp, sizeof(tmp),
                   "用户所在城市：%s（IP 定位得到）。用户问“我在哪”这类问题时，"
                   "就用这个城市回答。这个城市名可能是英文或者拼音，回答时换成"
                   "对应的中文城市名，别直接念英文。", loc_city);
        }

      used = prompt_append(out, out_cap, used, tmp);
    }

  /* 提醒工具：让模型自己把口语时间折算成 24 小时制（提示里刚给了当前日期
   * 时间，"明天早上八点"它能算）。不提工具时这段必须拿掉，否则模型会去调
   * 一个 tools 表里根本没有的函数。 */

  if (use_reminder)
    {
      used = prompt_append(out, out_cap, used,
                           "用户让你在某个时间提醒他做某件事时，必须调用 "
                           "add_reminder 工具把提醒建好：时间用 24 小时制，"
                           "例如“明天早上八点吃药”就是 08:00 加 吃药，"
                           "“下午三点量血压”就是 15:00 加 量血压。"
                           "建好之后在回复里简短复述一遍让用户确认，例如"
                           "“好，8 点提醒你吃药”。不要只说记住了而不调用"
                           "工具，也不要说自己做不到。");
    }

  /* 改音量/亮度工具：让模型知道这是**真能改**的，别再答"我改不了音量"。
   * 不提工具时这段必须拿掉，否则模型会去调一个 tools 表里根本没有的函数。 */

  if (use_setting)
    {
      used = prompt_append(out, out_cap, used,
                           "用户说声音太小、声音太大、把屏幕调亮调暗这类话时，"
                           "必须调用 adjust_setting 工具去真的改，不要只回一句"
                           "“您可以在设置里调”。"
                           "★说「已经调好了」「现在是 60」这类话之前，你必须已经"
                           "真的调用过 adjust_setting 工具。工具一次都没调用就报"
                           "成功、报数值，用户回头一看根本没变，那就是在骗他。"
                           "你手上没有任何数值，回复里出现的每一个具体数字都只能"
                           "来自工具执行结果里返回的那个数，不能自己编。"
                           "改完把工具返回的新数值简短说一声，例如"
                           "“已经帮您把音量调到 60 了”。");
    }

  /* 天气工具：开着才提默认城市和"必须查工具"；关着就明确告诉它这类问题
   * 答不了（否则会编一个数字给用户）。 */

  if (use_weather)
    {
      snprintf(tmp, sizeof(tmp),
               "用户问天气、气温、冷不冷、热不热、要不要带伞这类实时信息时，"
               "必须调用 get_weather 工具去查，再把它查回来的内容用自然口语"
               "讲给用户听，不要把 JSON、字段名或者数字天气码念出来。"
               "用户没说城市时就用默认城市%s。"
               "工具查不到或者查询失败时，就老实说没查到，不要编。", city);
      used = prompt_append(out, out_cap, used, tmp);
    }
  else
    {
      used = prompt_append(out, out_cap, used,
                          "用户问实时天气这类你答不了的信息时，就老实说你查不了，"
                          "不要编一个数字给他。");
    }
}

/****************************************************************************
 * Name: chat_tool_city
 *
 * Description:
 *   从 tool_calls 的 arguments 原文里取 city。arguments 是"JSON 里的字符串"，
 *   里面是转义过的 JSON 文本（形如 "{\"city\": \"北京\"}"），所以先 unescape
 *   再找键。取不到（模型没给 / 形状不对）返回 -ENOENT，调用方改用默认城市。
 *
 ****************************************************************************/

static int chat_tool_city(const char *args_escaped, char *out, size_t out_cap)
{
  char plain[256];
  const char *val;
  size_t n;

  if (args_escaped == NULL || args_escaped[0] == '\0' || out == NULL
      || out_cap == 0)
    {
      return -ENOENT;
    }

  n = json_unescape(args_escaped, strlen(args_escaped), plain,
                    sizeof(plain) - 1);
  plain[n] = '\0';

  val = json_find_string(plain, "city", &n);
  if (val == NULL || n == 0)
    {
      return -ENOENT;
    }

  n = json_unescape(val, n, out, out_cap - 1);
  out[n] = '\0';
  trim_whitespace(out);

  return out[0] != '\0' ? OK : -ENOENT;
}

/****************************************************************************
 * Name: mimo_reminder_apply
 *
 * Description:
 *   「替用户建一条提醒」的**唯一落地入口**。两条对话链路的工具都要走这里：
 *     - mimo_chat() 的 add_reminder（面板语音那条）
 *     - ai_tools_provider 的 add_reminder（常开麦走 velaclaw 那条）
 *
 *   为什么要抽成一份：标题截断、时间归一化、hook 缺失的兜底，本来各写一份
 *   就会漂移 —— 漏一处就是「面板能建、常开麦建出来是坏的」。判据也只有一条。
 *
 *   落地本身是 robot_ui 启动时用 mimo_set_reminder_hook() 注册的
 *   voice_add_reminder()：内部 reminder_sched_add + 刷新 + 通知界面，所以
 *   **提醒在设定当时就进提醒页**（不是到点才冒出来），到点由 reminder_sched
 *   自己响。半双工/录音中的克制也是那条既有路径在管，这里不碰音频。
 *
 * Input Parameters:
 *   hhmm           - 24 小时制 "H:MM" 或 "HH:MM"；"8:00" 和 "08:00" 都收
 *   title          - 提醒做什么，按整字截到 MIMO_REMINDER_TITLE_CHARS（提醒
 *                    列表槽只有 64 字节，截了比让 hook 拒收好）
 *   used_title     - 可选输出：实际用上的标题（已截断），调用方写话术用这个
 *   used_title_cap - used_title 的大小
 *
 * Returned Value:
 *   OK 成功；
 *   -EINVAL   hhmm 不是 24 小时制 HH:MM，或者 title 为空；
 *   -ENOSYS   本机没注册提醒落地实现（此时模型本不该看到这个工具）；
 *   其它负值  hook 的失败码（例如提醒条数已满），原样透传。
 *
 ****************************************************************************/

int mimo_reminder_apply(const char *hhmm, const char *title, char *used_title,
                        size_t used_title_cap)
{
  char normalized[8];
  char trimmed[MIMO_REMINDER_TITLE_CHARS * 4 + 4];
  bool truncated = false;
  int hh;
  int mm;
  int hook_ret;

  if (hhmm == NULL || title == NULL)
    {
      return -EINVAL;
    }

  /* 只认 24 小时制的 H:MM / HH:MM，别的一律打回（口语时间该由模型自己换算） */

  if (sscanf(hhmm, "%d:%d", &hh, &mm) != 2 || hh < 0 || hh > 23 || mm < 0
      || mm > 59)
    {
      return -EINVAL;
    }

  snprintf(normalized, sizeof(normalized), "%02d:%02d", hh, mm);

  if (title[0] == '\0')
    {
      return -EINVAL;
    }

  tts_truncate(title, trimmed, sizeof(trimmed), MIMO_REMINDER_TITLE_CHARS,
               &truncated);

  /* 把**实际用上的**标题回带给调用方：它要把标题写进给模型看的话术，用
   * 截断前的原文会让 snprintf 撑爆那个 384 字节的工具结果缓冲（截断了的
   * 错话会被模型原样念给老人听）。used_title 传 NULL 表示不关心。 */

  if (used_title != NULL && used_title_cap > 0)
    {
      snprintf(used_title, used_title_cap, "%s", trimmed);
    }

  if (s_reminder_hook == NULL)
    {
      /* 工具能下发就说明注册过（见 chat_reminder_tool_enabled），这里是兜底 */

      return -ENOSYS;
    }

  hook_ret = s_reminder_hook(normalized, trimmed);
  if (hook_ret < 0)
    {
      /* 透传 hook 的错误码，调用方要在话术里如实报给模型（例如"提醒记不下了"
       * 对应的满额码），压成一个笼统的 -EIO 就说不清了。 */

      syslog(LOG_WARNING, "[%s] add_reminder(%s, %s) 失败: %d\n", TAG,
             normalized, trimmed, hook_ret);
      return hook_ret;
    }

  syslog(LOG_INFO, "[%s] add_reminder 成功: %s %s%s\n", TAG, normalized,
         trimmed, truncated ? "（标题已截短）" : "");
  return OK;
}

/****************************************************************************
 * Name: mimo_settings_apply
 *
 * Description:
 *   「语音改音量/亮度」的**唯一落地入口**（声明见 mimo_voice.h）。两条对话
 *   链路的 adjust_setting 工具都走这里，落地是 robot_ui 启动时注册进来的
 *   touch_ui_settings_apply()：改 user_settings + 真下发硬件 + 存盘，
 *   **和设置页拖滑块同一条路** —— 所以语音调完再打开设置页，滑块就停在
 *   新值上。
 *
 *   到顶/到底不算失败：hook 回的是当前值（0 或 100），调用方据此告诉用户
 *   "已经是最大/最小了"。
 *
 * Returned Value:
 *   >=0 已生效的新值（0..100）；-EINVAL 参数不对；-ENOSYS 没注册落地实现；
 *   其它负值 hook 的失败码（原样透传）。
 *
 ****************************************************************************/

int mimo_settings_apply(int item, int action, int value)
{
  int ret;

  if (item != MIMO_SET_ITEM_VOLUME && item != MIMO_SET_ITEM_BRIGHTNESS)
    {
      return -EINVAL;
    }

  if (action != MIMO_SET_ACT_UP && action != MIMO_SET_ACT_DOWN
      && action != MIMO_SET_ACT_SET)
    {
      return -EINVAL;
    }

  if (s_settings_hook == NULL)
    {
      return -ENOSYS;
    }

  /* 值越界在这里夹一道，别把越界值交给实现方去各自处理 */

  if (value < 0)
    {
      value = 0;
    }

  if (value > 100)
    {
      value = 100;
    }

  ret = s_settings_hook(item, action, value);
  return ret;
}

/****************************************************************************
 * Name: chat_tool_add_reminder
 *
 * Description:
 *   执行 add_reminder 工具：从 arguments 里取出 time / title，校验并规范化
 *   时间，再交给 app 注册进来的 mimo_reminder_hook_t 落地（robot_ui 注册的
 *   实现会写进提醒列表并重挂 RTC 闹钟、刷新界面）。
 *
 *   **失败不返回负值**：把中文原因写进 out 当工具结果喂回模型，让模型自己跟
 *   用户解释（"提醒满了""时间没算对"这些让它说出来比界面弹窗自然，模型还能
 *   自己改正重试）。只有 out 本身不够长这种编程错误才回 -ENOMEM。
 *
 *   时间不合法时**绝不自己猜**：模型传 "8点" / "晚上八点" 就让它换算重试 ——
 *   猜错了就是到点不响，用户会以为提醒功能坏了。
 *
 * Returned Value:
 *   OK（out 里是工具结果文本）; -ENOMEM（out 装不下）。
 *
 ****************************************************************************/

static int chat_tool_add_reminder(const char *args_escaped, char *out,
                                  size_t out_cap)
{
  char plain[640];
  char time_raw[32];
  char title_raw[256];
  char title_used[MIMO_REMINDER_TITLE_CHARS * 4 + 4];
  char hhmm[8];
  const char *val;
  size_t n;
  int hh;
  int mm;
  int ret;

  if (out == NULL || out_cap == 0)
    {
      return -ENOMEM;
    }

  out[0] = '\0';

  /* arguments 是"JSON 里的 JSON 字符串"，先反转义出真正的 JSON 文本 */

  n = json_unescape(args_escaped, strlen(args_escaped), plain,
                    sizeof(plain) - 1);
  plain[n] = '\0';

  val = json_find_string(plain, "time", &n);
  if (val == NULL || n == 0)
    {
      snprintf(out, out_cap,
               "调用失败：缺少 time 参数。请用 24 小时制的 HH:MM（例如 08:00）"
               "重新调用一次。");
      return OK;
    }

  n = json_unescape(val, n, time_raw, sizeof(time_raw) - 1);
  time_raw[n] = '\0';
  trim_whitespace(time_raw);

  val = json_find_string(plain, "title", &n);
  if (val == NULL || n == 0)
    {
      snprintf(out, out_cap,
               "调用失败：缺少 title 参数。请用简短中文写清楚提醒做什么"
               "（例如 吃药）重新调用一次。");
      return OK;
    }

  n = json_unescape(val, n, title_raw, sizeof(title_raw) - 1);
  title_raw[n] = '\0';
  trim_whitespace(title_raw);

  /* 只认 24 小时制的 H:MM / HH:MM —— "8:00" 和 "08:00" 都收，别的一律打回 */

  if (sscanf(time_raw, "%d:%d", &hh, &mm) != 2 || hh < 0 || hh > 23
      || mm < 0 || mm > 59)
    {
      snprintf(out, out_cap,
               "调用失败：time 必须是 24 小时制的 HH:MM，收到的是“%.40s”。"
               "请把口语时间自己换算好再调用一次，例如下午三点写成 15:00。",
               time_raw);
      return OK;
    }

  /* 时间归一化、标题截断、hook 落地，统一走 mimo_reminder_apply —— 两条对话
   * 链路的 add_reminder 共用一份（见那里的注释），别在这里再抄一遍。 */

  snprintf(hhmm, sizeof(hhmm), "%02d:%02d", hh, mm);

  ret = mimo_reminder_apply(hhmm, title_raw, title_used, sizeof(title_used));

  if (ret == -ENOSYS)
    {
      /* 工具能下发就说明注册过（见 chat_reminder_tool_enabled），这里是兜底 */

      snprintf(out, out_cap, "调用失败：本机没有可用的提醒功能。");
      return OK;
    }

  if (ret < 0)
    {
      snprintf(out, out_cap,
               "调用失败：%s 提醒%s 没能建起来（错误码 %d）。请跟用户说一声"
               "这次没记住，稍后再试。", hhmm, title_used, ret);
      return OK;
    }

  snprintf(out, out_cap,
           "已建好：每天 %s 提醒用户%s。请在回复里简短复述一遍确认。",
           hhmm, title_used);

  return OK;
}

/****************************************************************************
 * Name: chat_tool_adjust_setting
 *
 * Description:
 *   执行 adjust_setting 工具：从 arguments 里取出 item / action / value，
 *   交给 mimo_settings_apply() 落地（那是两条链路共用的唯一入口，见那边）。
 *
 *   与 add_reminder 同纪律：**失败不返回负值**，把中文原因写进 out 当工具结果
 *   喂回模型，让模型自己跟用户解释并改正重试。参数不认就是不认，绝不猜 ——
 *   猜错了就是"说调高结果把亮度调没了"，比不调更糟。
 *
 * Returned Value:
 *   OK（out 里是工具结果文本）; -ENOMEM（out 装不下）。
 *
 ****************************************************************************/

static int chat_tool_adjust_setting(const char *args_escaped, char *out,
                                    size_t out_cap)
{
  char plain[320];
  char item_raw[32];
  char action_raw[32];
  const char *val;
  size_t n;
  int item;
  int action;
  int value = 0;
  int ret;

  if (out == NULL || out_cap == 0)
    {
      return -ENOMEM;
    }

  out[0] = '\0';

  /* arguments 是"JSON 里的 JSON 字符串"，先反转义出真正的 JSON 文本 */

  n = json_unescape(args_escaped, strlen(args_escaped), plain,
                    sizeof(plain) - 1);
  plain[n] = '\0';

  val = json_find_string(plain, "item", &n);
  if (val == NULL || n == 0)
    {
      snprintf(out, out_cap,
               "调用失败：缺少 item 参数。volume 是音量、brightness 是屏幕"
               "亮度，请重新调用一次。");
      return OK;
    }

  n = json_unescape(val, n, item_raw, sizeof(item_raw) - 1);
  item_raw[n] = '\0';
  trim_whitespace(item_raw);

  if (strcmp(item_raw, "volume") == 0)
    {
      item = MIMO_SET_ITEM_VOLUME;
    }
  else if (strcmp(item_raw, "brightness") == 0)
    {
      item = MIMO_SET_ITEM_BRIGHTNESS;
    }
  else
    {
      snprintf(out, out_cap,
               "调用失败：item 只认 volume（音量）和 brightness（屏幕亮度），"
               "收到的是“%.40s”。请重新调用一次。", item_raw);
      return OK;
    }

  val = json_find_string(plain, "action", &n);
  if (val == NULL || n == 0)
    {
      snprintf(out, out_cap,
               "调用失败：缺少 action 参数。up 是调高一档、down 是调低一档、"
               "set 是直接设成某个值，请重新调用一次。");
      return OK;
    }

  n = json_unescape(val, n, action_raw, sizeof(action_raw) - 1);
  action_raw[n] = '\0';
  trim_whitespace(action_raw);

  if (strcmp(action_raw, "up") == 0)
    {
      action = MIMO_SET_ACT_UP;
    }
  else if (strcmp(action_raw, "down") == 0)
    {
      action = MIMO_SET_ACT_DOWN;
    }
  else if (strcmp(action_raw, "set") == 0)
    {
      action = MIMO_SET_ACT_SET;

      /* set 必须给 value —— 不给就打回去让它重传，绝不"猜一个数" */

      if (json_find_number(plain, "value", &value) != OK)
        {
          snprintf(out, out_cap,
                   "调用失败：action 是 set 的时候必须给 value（0 到 100 的"
                   "整数）。请带上 value 重新调用一次。");
          return OK;
        }
    }
  else
    {
      snprintf(out, out_cap,
               "调用失败：action 只认 up、down、set 三个值，收到的是"
               "“%.40s”。请重新调用一次。", action_raw);
      return OK;
    }

  ret = mimo_settings_apply(item, action, value);

  if (ret == -ENOSYS)
    {
      /* 工具能下发就说明注册过（见 chat_setting_tool_enabled），这里是兜底 */

      snprintf(out, out_cap, "调用失败：本机没有可以改这个设置的入口。");
      return OK;
    }

  if (ret < 0)
    {
      snprintf(out, out_cap,
               "调用失败：%s没能改成（错误码 %d）。请跟用户说一声这次没改上，"
               "可以说慢一点再说一遍。", item == MIMO_SET_ITEM_VOLUME ?
               "音量" : "亮度", ret);
      return OK;
    }

  snprintf(out, out_cap,
           "已经改好了：%s现在是 %d（0 到 100）。请简短告诉用户，"
           "顺便说清是调高了还是调低了。",
           item == MIMO_SET_ITEM_VOLUME ? "音量" : "亮度", ret);

  return OK;
}

/* 一轮应答里解析出来的工具调用（都是响应里的原文） */

struct chat_toolcall_s
{
  char id[64];     /* tool_call id：回填下一轮 role:"tool" 的 tool_call_id */
  char name[32];   /* function.name */
  char args[512];  /* function.arguments 原文：已是 JSON 转义过的文本 */
};

/****************************************************************************
 * Name: chat_round
 *
 * Description:
 *   发一轮请求并解析应答：
 *     finish_reason="tool_calls" -> 返回 1，把 tool_calls[0] 的 id / name /
 *       arguments 抄进 tc（响应缓冲随后就还了，所以必须抄出来）；
 *     其它 -> 返回 0，正文已 unescape 进 reply_out；
 *     失败 -> 负 errno（-EIO HTTP 失败 / -EPROTO 没有可用的正文或解析不出
 *       工具调用 / -ENOMEM）。
 *
 *   "tool_calls" 用 str_last() 取最后一处：模型的 reasoning_content 里也常
 *   出现这个词，而 JSON 里真正的那个键在 message 的最后。
 *
 ****************************************************************************/

static int chat_round(int round, const char *body, char *reply_out,
                      size_t reply_cap, struct chat_toolcall_s *tc)
{
  char extra[64];
  bool is_tool;
  const char *reason;
  const char *val;
  const char *tc_json;
  char *resp;
  size_t len = 0;
  size_t n;
  size_t out_len;
  int status;

  memset(tc, 0, sizeof(*tc));

  resp = mimo_calloc_logged("chat.resp", MIMO_CHAT_RESP_CAP);
  if (resp == NULL)
    {
      return -ENOMEM;
    }

  status = mimo_post(body, resp, MIMO_CHAT_RESP_CAP);

  if (status != 200)
    {
      mimo_log_http_error("chat", status, resp);
      mimo_free_logged("chat.resp", resp, MIMO_CHAT_RESP_CAP);
      return -EIO;
    }

  /* finish_reason 决定这一轮是"要调工具"还是"给正文" */

  reason = json_find_string(resp, "finish_reason", &len);
  if (reason == NULL)
    {
      syslog(LOG_ERR, "[%s] chat: 第 %d 轮响应里没有 finish_reason: %.*s\n",
             TAG, round, MIMO_ERR_DUMP_LEN, resp);
      mimo_free_logged("chat.resp", resp, MIMO_CHAT_RESP_CAP);
      return -EPROTO;
    }

  extra[0] = '\0';
  is_tool = (len == 10 && memcmp(reason, "tool_calls", 10) == 0);

  if (is_tool)
    {
      tc_json = str_last(resp, "\"tool_calls\"");
      if (tc_json != NULL)
        {
          val = json_find_string(tc_json, "id", &n);
          if (val != NULL && n < sizeof(tc->id))
            {
              memcpy(tc->id, val, n);
              tc->id[n] = '\0';
            }

          val = json_find_string(tc_json, "name", &n);
          if (val != NULL && n < sizeof(tc->name))
            {
              memcpy(tc->name, val, n);
              tc->name[n] = '\0';
            }

          /* arguments 是"JSON 里的 JSON 字符串"：这里拿到的是**已经转义好**
           * 的原文，下一轮直接抄进请求体就行（不用先反转义再转义回来） */

          val = json_find_string(tc_json, "arguments", &n);
          if (val != NULL && n < sizeof(tc->args))
            {
              memcpy(tc->args, val, n);
              tc->args[n] = '\0';
            }
        }

      snprintf(extra, sizeof(extra), ", 工具=%s",
               tc->name[0] != '\0' ? tc->name : "(没解析出来)");
    }

  syslog(LOG_INFO, "[%s] chat: 第 %d 轮, finish=%.*s%s\n", TAG, round,
         (int)len, reason, extra);

  if (is_tool)
    {
      if (tc->name[0] == '\0')
        {
          /* 要工具但连函数名都没解析出来，拼不出下一轮。模型偶尔会既写正文
           * 又要工具，有正文就用正文，没有就当这一轮失败。 */

          val = json_find_string(resp, "content", &n);
          if (val != NULL && n > 0)
            {
              out_len = json_unescape(val, n, reply_out, reply_cap - 1);
              reply_out[out_len] = '\0';
              tts_text_sanitize(reply_out);
              trim_whitespace(reply_out);
              mimo_free_logged("chat.resp", resp, MIMO_CHAT_RESP_CAP);
              return reply_out[0] != '\0' ? 0 : -EPROTO;
            }

          syslog(LOG_ERR, "[%s] chat: tool_calls 解析不出函数名\n", TAG);
          mimo_free_logged("chat.resp", resp, MIMO_CHAT_RESP_CAP);
          return -EPROTO;
        }

      /* tool_call_id 理论上一定有；真没有就自己造一个，别让这一轮白费 */

      if (tc->id[0] == '\0')
        {
          syslog(LOG_WARNING, "[%s] chat: tool_calls 里没有 id，用 call_0 顶上\n",
                 TAG);
          strcpy(tc->id, "call_0");
        }

      mimo_free_logged("chat.resp", resp, MIMO_CHAT_RESP_CAP);
      return 1;
    }

  /* 只取 choices[0].message.content。json_find_string() 找的是字面量
   * "content"，不会命中 "reasoning_content"（那里 content 前面是下划线，
   * 不是引号）。 */

  val = json_find_string(resp, "content", &n);
  if (val == NULL || n == 0)
    {
      syslog(LOG_ERR,
             "[%s] chat: 响应里没有 message.content（思维链把 token 占满了？）"
             ": %.*s\n",
             TAG, MIMO_ERR_DUMP_LEN, resp[0] != '\0' ? resp : "(空)");
      mimo_free_logged("chat.resp", resp, MIMO_CHAT_RESP_CAP);
      return -EPROTO;
    }

  out_len = json_unescape(val, n, reply_out, reply_cap - 1);
  reply_out[out_len] = '\0';
  mimo_free_logged("chat.resp", resp, MIMO_CHAT_RESP_CAP);

  /* 出声前清洗：Markdown 记号 + 工具结果 JSON 整块都不能念出来（见
   * tts_text_sanitize 的注释）。放在 trim 之前，清洗在删掉 JSON 块的地方
   * 补的空格由 trim 收干净。 */

  tts_text_sanitize(reply_out);

  trim_whitespace(reply_out);

  if (reply_out[0] == '\0')
    {
      syslog(LOG_ERR, "[%s] chat: 回复正文为空（max_tokens 被思维链吃光？）\n",
             TAG);
      return -EPROTO;
    }

  return 0;
}

/****************************************************************************
 * Name: mimo_chat
 *
 * Description:
 *   同步跑完一次对话（POST <llm_host>:<llm_port><llm_path>），把回复正文
 *   写进 reply_out。和 ASR/TTS 共用 host / path / api_key，只有 model 从
 *   配置的 "model" 键读（不写死）。
 *
 *   默认是**单轮对话**：请求体里不带 "tools"，messages 就 system + user 两条，
 *   模型没有工具可要，第 1 轮直接出正文。system prompt 里带着当前北京时间
 *   （自己 +8 小时算，本构建没有 zoneinfo），所以"今天几号 / 现在几点 /
 *   今天星期几"这类问题不用再打一次网络。
 *
 *   两个工具，各自独立开关，请求体里的 tools 表按开关从四段字面量里选（见
 *   mimo_chat 里的 tools_json）：
 *     add_reminder —— **默认开**（enable_reminder_tool 配成 0/false/off 才关），
 *       前提是 app 已经注册了 mimo_set_reminder_hook()。模型要调它时，板子把
 *       time/title 交给那个回调落地（robot_ui 会写进提醒列表 + 重挂 RTC 闹钟），
 *       结果作为 role:"tool" 追加进去再问一轮，让模型复述确认；
 *     get_weather —— **默认关**（enable_weather_tool 设成 1 才开）：模型要调它时
 *       板子替它打 open-meteo 查，同样追加两条消息再问一轮。
 *   messages 会长，最多 MIMO_CHAT_MAX_ROUNDS 轮。开天气工具时系统提示里才提
 *   默认城市。
 *   工具查不到 / 服务端 HTTP 出错不改语义：把"没查到"喂回给模型，让它自己跟
 *   用户解释；只有网络层失败（TLS/DNS/连接）才回负值。
 *
 *   这是给**别的 app**（robot_ui）用的跨 app 接口：凭据自己读、请求自己发，
 *   不依赖 ai_agent 进程的任何全局状态（消息总线队列等）。相比之下 ai_agent
 *   的 llm_send_text() / velaclaw_* 只能在本进程里用 —— 别的 app 调会把消息
 *   投进一个从没初始化过的队列，pthread_mutex_lock 直接撞
 *   NXSEM_IS_MUTEX 断言，把整个 app 打死。
 *
 *   注意：内部要跑 TLS 握手 + 云端推理，会阻塞几十秒，只能在非 LVGL 线程里调。
 *
 * Input Parameters:
 *   text      - 用户输入文本（UTF-8，内部做 JSON 转义）；
 *   reply_out - 输出缓冲（调用方提供，栈或堆都行）；
 *   reply_cap - 输出缓冲大小（含结尾 '\0'）。
 *
 * Returned Value:
 *   回复正文长度（字节，去掉首尾空白后）；<0 为负 errno
 *   （-ENOENT 未配置 / -EIO HTTP 或网络失败 / -EPROTO 响应里没有正文或
 *   解析不出工具调用）。
 *
 ****************************************************************************/

int mimo_chat(const char *text, char *reply_out, size_t reply_cap)
{
  char sys[2048];
  char city[MIMO_CITY_BUF_MAX];
  char loc_city[MIMO_CITY_BUF_MAX];
  char city_hint[MIMO_CITY_BUF_MAX];
  char tool_result[384];
  char prefix[128];
  const char *tools_json;
  struct chat_toolcall_s tc;
  struct sjoin_s msj;
  char *msgs;
  size_t msgs_cap;
  size_t sys_len;
  size_t text_len;
  size_t body_cap;
  int pfx_len;
  int round;
  int ret;
  bool use_weather;
  bool use_reminder;
  bool use_setting;
  bool use_tools;

  if (text == NULL || reply_out == NULL || reply_cap == 0)
    {
      return -EINVAL;
    }

  reply_out[0] = '\0';

  if (mimo_load_config() != OK)
    {
      syslog(LOG_ERR, "[%s] chat: 配置里缺 llm_host / api_key，MiMo 不可用\n",
             TAG);
      return -ENOENT;
    }

  if (s_model[0] == '\0')
    {
      syslog(LOG_ERR, "[%s] chat: 配置里缺 model，无法发起对话\n", TAG);
      return -ENOENT;
    }

  text_len = strlen(text);
  if (text_len == 0)
    {
      return -EINVAL;
    }

  pfx_len = snprintf(prefix, sizeof(prefix), MIMO_CHAT_JSON_FMT, s_model);
  if (pfx_len < 0 || (size_t)pfx_len >= sizeof(prefix))
    {
      return -EINVAL;
    }

  /* 三个工具各自独立开关：
   *   天气默认关（用户反馈查得不准），要开在配置里设 enable_weather_tool=1；
   *   提醒默认开（"八点提醒我吃药"就要生效），前提是 app 注册了建提醒回调；
   *   改音量/亮度默认开（"声音小一点"就要生效），同样前提是注册了回调。
   * 关掉的工具不进 tools 表，系统提示里也不提，模型就看不到它。 */

  use_weather  = chat_weather_tool_enabled();
  use_reminder = chat_reminder_tool_enabled();
  use_setting  = chat_setting_tool_enabled();
  use_tools    = use_weather || use_reminder || use_setting;

  /* 用户在哪：板子自己查一次 IP 定位（mimo_location.c，结果在里面缓存，
   * 6 小时内不会重复联网）。查不到就留空 —— 系统提示里那段"用户所在城市"
   * 一个字都不写，模型问不出来也编不出来。这一步只在非 LVGL 线程里做，
   * mimo_chat() 本来就是这么用的。 */

  loc_city[0] = '\0';

  if (mimo_location_get(loc_city, sizeof(loc_city), NULL, NULL) != OK)
    {
      loc_city[0] = '\0';       /* 失败：当没有这回事（原因在定位模块里打过了） */
    }

  if (use_weather)
    {
      chat_default_city(city, sizeof(city));

      /* 开了天气工具时，配置里的 default_city 只是"IP 定位没查到时"的兜底：
       * 定位查到了就用它 —— 提示里让模型用的城市，必须和板子真去查的城市
       * 是同一个（get_weather 的兜底名额就是这里的 city）。 */

      if (loc_city[0] != '\0')
        {
          snprintf(city, sizeof(city), "%s", loc_city);
          syslog(LOG_INFO, "[%s] chat: 天气默认城市用 IP 定位的 %s\n", TAG,
                 city);
        }
    }
  else
    {
      city[0] = '\0';
    }

  chat_build_system_prompt(use_weather, use_reminder, use_setting, city,
                           loc_city, sys, sizeof(sys));
  sys_len = strlen(sys);
  syslog(LOG_INFO, "[%s] chat: 用户所在城市=%s\n", TAG,
         (loc_city[0] != '\0') ? loc_city : "(没查到，提示里不提)");

  /* tools 表：单个工具是"一个函数对象"（见 MIMO_TOOL_*_JSON），三种开关的
   * 八种组合各是一段**写死的**字面量，直接指过去（相邻字符串常量编译器自动
   * 拼接）。不再运行时拼装，也就没有"缓冲装不下"这一类故障 —— 见本文件前面
   * 「tools 数组**没有缓冲**了」那段注释里记的前科。
   * 关掉的那个不参与拼接，所以八种组合都是合法 JSON —— 不能用"先拼全再删"
   * 那套，删不干净就是尾逗号，服务端 json 解析直接报错。
   *
   * 下标 = (天气<<2) | (提醒<<1) | 改设置，八行一目了然。加第四个工具要再翻
   * 一倍写全 —— 宁可写全，也不要在运行时拼 JSON（那条路留过缓冲溢出前科）。 */

  {
    const char *const tools_by_switch[8] = {
      "[]",                                                          /* 000 */
      "[" MIMO_TOOL_SETTING_JSON "]",                                /* 001 */
      "[" MIMO_TOOL_REMINDER_JSON "]",                               /* 010 */
      "[" MIMO_TOOL_REMINDER_JSON "," MIMO_TOOL_SETTING_JSON "]",    /* 011 */
      "[" MIMO_TOOL_WEATHER_JSON "]",                                /* 100 */
      "[" MIMO_TOOL_WEATHER_JSON "," MIMO_TOOL_SETTING_JSON "]",     /* 101 */
      "[" MIMO_TOOL_WEATHER_JSON "," MIMO_TOOL_REMINDER_JSON "]",    /* 110 */
      "[" MIMO_TOOL_WEATHER_JSON "," MIMO_TOOL_REMINDER_JSON ","
        MIMO_TOOL_SETTING_JSON "]",                                  /* 111 */
    };

    tools_json = tools_by_switch[(use_weather ? 4 : 0)
                                 | (use_reminder ? 2 : 0)
                                 | (use_setting ? 1 : 0)];
  }

  /* 1. 先拼 messages 数组：system（当前时间 + 用户所在城市 + [默认城市]
   *    + 说话要求）+ user。开了工具时之后每轮最多再追加
   *    assistant(tool_calls) + tool 两条（1 KB 量级），余量按最多 3 轮 +
   *    最坏 6 倍放大留，装不下由 sjoin 的 ovf 兜住；工具关着就固定两条消息。 */

  msgs_cap = (sys_len + text_len) * 6 + 4096;
  msgs = mimo_alloc_logged("chat.msgs", msgs_cap);
  if (msgs == NULL)
    {
      return -ENOMEM;
    }

  msgs[0] = '\0';
  msj = (struct sjoin_s) { msgs, msgs_cap, 0, false };

  sjoin_addstr(&msj, "{\"role\":\"system\",\"content\":\"");
  sjoin_add_escaped(&msj, sys, sys_len);
  sjoin_addstr(&msj, "\"},{\"role\":\"user\",\"content\":\"");
  sjoin_add_escaped(&msj, text, text_len);
  sjoin_addstr(&msj, "\"}");

  if (msj.ovf)
    {
      syslog(LOG_ERR, "[%s] chat: messages 缓冲 %zu 字节不够（文本太长？）\n",
             TAG, msgs_cap);
      mimo_free_logged("chat.msgs", msgs, msgs_cap);
      return -ENOMEM;
    }

  /* + model / tools 表 / max_tokens 骨架。
   * ⚠️ 这里必须 strlen：tools_json 现在是 const char *（见上面），sizeof(指针)
   * 只有 4，拿它当容量会把 body 分小、后面对不上字节数。 */
  body_cap = msgs_cap + 1024 + strlen(tools_json);

  /* 日志里只说这一轮带哪些工具，key 一个字节都不打 */

  syslog(LOG_INFO,
         "[%s] chat: 工具=[提醒=%s 天气=%s 改设置=%s], 请求 %zu 字节文本, "
         "model=%s%s%s\n",
         TAG, use_reminder ? "开" : "关", use_weather ? "开" : "关",
         use_setting ? "开" : "关",
         text_len, s_model,
         use_weather ? ", 默认城市=" : "", use_weather ? city : "");

  ret = -EPROTO;

  /* 2. 工具循环：每轮把当前的 messages 拼成完整请求体发出去。
   *    模型要工具 -> 板子执行一次（建提醒 / 查天气）-> 追加两条消息 -> 再来
   *    一轮；模型直接给正文 -> 那就是回复（工具都关着时必然走这条，一轮结束）。 */

  for (round = 1; round <= MIMO_CHAT_MAX_ROUNDS; round++)
    {
      struct sjoin_s bj;
      char *body;

      body = mimo_alloc_logged("chat.body", body_cap);
      if (body == NULL)
        {
          ret = -ENOMEM;
          break;
        }

      body[0] = '\0';
      bj = (struct sjoin_s) { body, body_cap, 0, false };
      sjoin_add(&bj, prefix, (size_t)pfx_len);
      sjoin_add(&bj, msgs, msj.len);

      /* 尾巴两份，二选一：不带 tools 的整段，或者" ] , tools : [...] , "
       * 三段拼（tools 数组是上面按开关拼好的）—— 关掉的工具不进这个数组 */

      if (use_tools)
        {
          sjoin_addstr(&bj, MIMO_CHAT_JSON_TAIL_TOOLS_HEAD);
          sjoin_addstr(&bj, tools_json);
          sjoin_addstr(&bj, MIMO_CHAT_JSON_TAIL_TOOLS_END);
        }
      else
        {
          sjoin_addstr(&bj, MIMO_CHAT_JSON_TAIL_NO_TOOLS);
        }

      if (bj.ovf)
        {
          mimo_free_logged("chat.body", body, body_cap);
          ret = -ENOMEM;
          break;
        }

      ret = chat_round(round, body, reply_out, reply_cap, &tc);
      mimo_free_logged("chat.body", body, body_cap);

      if (ret < 0)
        {
          break;                     /* 网络 / 解析失败 */
        }

      if (ret == 0)
        {
          break;                     /* 拿到正文，已经在 reply_out 里 */
        }

      /* ret == 1：模型要调工具。按**函数名**分派 —— tools 表里有三个工具，
       * 名字对不上（模型幻觉出来的）也不能把整轮对话打死：回一句"没这个工具"
       * 喂回去，让它下一轮改用正文回答用户。 */

      if (strcmp(tc.name, "add_reminder") == 0 && use_reminder)
        {
          /* 建提醒：交给 app 注册进来的回调落地（robot_ui 注册的实现会写进
           * 提醒列表、重挂 RTC 闹钟、刷新界面）。失败原因由它写成中文工具
           * 结果，不回负值 —— 让模型自己跟用户解释。 */

          ret = chat_tool_add_reminder(tc.args, tool_result,
                                       sizeof(tool_result));
          if (ret < 0)
            {
              break;
            }
        }
      else if (strcmp(tc.name, "get_weather") == 0 && use_weather)
        {
          if (chat_tool_city(tc.args, city_hint, sizeof(city_hint)) != OK)
            {
              /* 模型没给城市（或者 arguments 不是我们预期的形状）：用默认城市 */

              snprintf(city_hint, sizeof(city_hint), "%s", city);
            }

          ret = weather_lookup(city_hint, tool_result, sizeof(tool_result));

          if (ret < 0)
            {
              syslog(LOG_ERR, "[%s] chat: get_weather(%s) 网络失败 %d\n", TAG,
                     city_hint, ret);
              break;
            }

          syslog(LOG_INFO, "[%s] chat: get_weather(%s) 结果 %zu 字节\n", TAG,
                 city_hint, strlen(tool_result));
        }
      else if (strcmp(tc.name, "adjust_setting") == 0 && use_setting)
        {
          /* 改音量/亮度：落地走 mimo_settings_apply()（两条链路共用的唯一
           * 入口，和设置页拖滑块同一条路）。失败原因由它写成中文工具结果，
           * 不回负值 —— 让模型自己跟用户解释。 */

          ret = chat_tool_adjust_setting(tc.args, tool_result,
                                         sizeof(tool_result));
          if (ret < 0)
            {
              break;
            }
        }
      else
        {
          /* 名字不认识，或者这个工具这一轮根本不在 tools 表里（模型不该
           * 知道它）。当成工具结果喂回去，别用负值收场。 */

          syslog(LOG_WARNING,
                 "[%s] chat: 收到不可用的工具 %s（提醒=%s 天气=%s 改设置=%s）\n",
                 TAG, tc.name[0] != '\0' ? tc.name : "(空名)",
                 use_reminder ? "开" : "关", use_weather ? "开" : "关",
                 use_setting ? "开" : "关");

          snprintf(tool_result, sizeof(tool_result),
                   "本机没有名为 %s 的工具，请直接用中文回答用户。", tc.name);
          ret = OK;
        }

      if (round == MIMO_CHAT_MAX_ROUNDS)
        {
          /* 轮次用完模型还在要工具：不再发请求，把工具结果直接当回复给用户
           * （总比让界面报"AI 请求失败"强） */

          syslog(LOG_WARNING,
                 "[%s] chat: %d 轮上限仍在要工具，直接用工具结果兜底\n", TAG,
                 MIMO_CHAT_MAX_ROUNDS);
          snprintf(reply_out, reply_cap, "%s", tool_result);
          ret = (int)strlen(reply_out);
          break;
        }

      /* 把这一轮的 assistant(tool_calls) 和 tool(结果) 两条消息追加进
       * messages，下一轮再问一次。arguments 直接抄响应里的原文（它本来就是
       * 转义好的 JSON 文本）。 */

      sjoin_addstr(&msj,
                   ",{\"role\":\"assistant\",\"content\":null,\"tool_calls\":"
                   "[{\"id\":\"");
      sjoin_add_escaped(&msj, tc.id, strlen(tc.id));
      sjoin_addstr(&msj, "\",\"type\":\"function\",\"function\":{\"name\":\"");
      sjoin_add_escaped(&msj, tc.name, strlen(tc.name));
      sjoin_addstr(&msj, "\",\"arguments\":\"");
      sjoin_add(&msj, tc.args, strlen(tc.args));
      sjoin_addstr(&msj, "\"}}]},{\"role\":\"tool\",\"tool_call_id\":\"");
      sjoin_add_escaped(&msj, tc.id, strlen(tc.id));
      sjoin_addstr(&msj, "\",\"content\":\"");
      sjoin_add_escaped(&msj, tool_result, strlen(tool_result));
      sjoin_addstr(&msj, "\"}");

      if (msj.ovf)
        {
          syslog(LOG_ERR, "[%s] chat: messages 追加不下（缓冲 %zu 字节）\n",
                 TAG, msgs_cap);
          ret = -ENOMEM;
          break;
        }
    }

  mimo_free_logged("chat.msgs", msgs, msgs_cap);

  if (ret < 0)
    {
      return ret;
    }

  tts_text_sanitize(reply_out);

  trim_whitespace(reply_out);

  if (reply_out[0] == '\0')
    {
      syslog(LOG_ERR, "[%s] chat: 回复正文为空（max_tokens 被思维链吃光？）\n",
             TAG);
      return -EPROTO;
    }

  ret = (int)strlen(reply_out);
  syslog(LOG_INFO, "[%s] chat: 回复 %d 字节\n", TAG, ret);
  return ret;
}
