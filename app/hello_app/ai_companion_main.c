/****************************************************************************
 * AI Companion Main Entry
 * 智爱陪伴 - AI老人陪伴守护终端
 * 主程序入口 - 初始化并运行状态机
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/clock.h>     /* clock_systime_ticks / TICK2MSEC：开机起算的运行时长 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <semaphore.h>

#include "ai_state_machine.h"
#include "ai_audio.h"
#include "ai_llm.h"
#include "ai_sound_detect.h"
#include "sound_event.h"
#include "ai_care.h"
#include "ai_network.h"
#include "ai_network_config.h"
#include "kws_dtw.h"

/* 把"要显示的东西"直接推给界面（同一个地址空间里的 robot_ui）。
 *
 * 整机是单一大镜像（CONFIG_BUILD_FLAT）：界面和这里在同一个地址空间、符号在
 * 最终链接时解析，本来一次函数调用就够。原来界面反馈要绕公网 MQTT（publish 到
 * zhi_ai/<client_id>/command 再由 robot_ui 的 network_task 收回来），
 * 那条路上任何一环慢/丢/断，用户看到的就是"说话完全没反应"。
 * 这个头文件是自给自足的（不引 LVGL，hello_app 编不了 LVGL）。 */
#include "robot_ui_bridge.h"

/* 跨 app 的两个非阻塞请求（语音聊天的「提交」/ 报警后让语音追问收摊）：声明在
 * 那个自给自足的头文件里，真动作在本文件（ai_companion_voice_submit /
 * ask_flow_tick）。
 * ★ 原来的「让路 / 收回」握手（robot_ui 播出声前请我们交出常开麦）2026-09-21
 *   整套删掉了 —— 决定、真机故障形状和它为什么是纯负担写在 ai_companion_req.h
 *   头上；麦克风从此纯常开，放音时的停录/恢复全在 ai_audio.c 内部。 */
#include "ai_companion_req.h"

/* 只读诊断快照（robot_ui 的 network_task 每 30 秒的心跳、以及每次
 * {"action":"diag"} 都会来问一次）：调用契约写在那个头文件头上，这里只实现
 * 内部门面 ai_companion_state_snapshot_impl()（真话得从本文件的 static 里拿）。
 * 那个头文件同样是自给自足的 —— robot_ui 也要 include 它。 */
#include "ai_companion_diag.h"

/* 板级录音封装（ai_audio.c 已经在用同一个头）。这里只用它的**只读**观测入口
 * sf32lb52_audio_rx_stats()：断流时"卡在无界等待"和"DMA 交帧那一半丢了"
 * 这两种可能，上层看得见的 idle/wait 分不出来，得靠驱动里那几个计数器
 * （见下面 ai_companion_state_snapshot_impl 末尾和 rxstuck_watch_tick）。
 * 那个头是自给自足的（只引 nuttx 的 config/compiler + stdint），不引 LVGL。 */
#include "sf32lb52_audio_in.h"

#include "voice/voice_asr.h"
#include "voice/voice_tts.h"
#include "volc_asr.h"
#include "volc_tts.h"
#include "mimo_voice.h"
#include "ai_tools_provider.h"

/* 固定文案的 TTS 预缓存（PC 侧预合成 → ROMFS → 板端算个哈希直接播）。
 * 用的就是本目录的 tts_cache.c：文件名规则写在 tts_cache.h 头上，
 * 和 PC 侧脚本 _flash/gen_tts_cache.py 必须逐字节一致。 */
#include "tts_cache.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 主循环间隔(毫秒) */
#define MAIN_LOOP_INTERVAL_MS    100

/* ---- 语音状态回传 (S2) ----
 *
 * 语音入口搬到这里之后，界面对语音链路的进展一无所知（用户原话：
 * "我怎么知道他在听他什么时候回复"）。四个状态有**两条**出路：
 *
 *   1) 主路 —— 同进程直调界面（robot_ui_bridge.h 的
 *      robot_ui_bridge_voice_state()）。整机单一大镜像，界面就在同一个地址空间，
 *      本机自己的显示没有任何理由依赖网络；
 *   2) 旁路 —— MQTT publish 到 zhi_ai/<client_id>/command，给手机/PC 端
 *      （它们不在这个地址空间里）。动作名和状态串要和 robot_ui 的
 *      on_ai_command_received() 对上：
 *
 *   {"action":"voice_state","param":"listening"}  正在听用户说话
 *   {"action":"voice_state","param":"thinking"}   送 ASR / 大模型，等回复
 *   {"action":"voice_state","param":"speaking"}   TTS 正在出声
 *   {"action":"voice_state","param":"idle"}       常听 / 空闲，没有进行中的事
 *
 * 两条都发的原因：直调解决"本机界面没反应"，MQTT 解决"远端看不见"。
 * 原来只有 MQTT 这一条，开机时它常常连不上（RNDIS/DNS 还没就绪），
 * 于是本机界面从头到尾一动不动 —— 用户口中的"语音聊天卡死、说话没反应"。
 */

#define VOICE_STATE_ACTION       "voice_state"
#define VOICE_STATE_LISTENING    "listening"
#define VOICE_STATE_THINKING     "thinking"
#define VOICE_STATE_SPEAKING     "speaking"
#define VOICE_STATE_IDLE         "idle"

/* ---- 用户原话回传 (S2) ----
 *
 * 老人回头看不到"我刚说了什么"，对话看着像单向的（用户原话："又看不到回复又
 * 看不到自己说了什么"）。所以 ASR 出文本的那一刻把原文也发一份出去，
 * 同样是两条路：主路直调 robot_ui_bridge_voice_user_said()，旁路 MQTT
 * 发 {"action":"user_said","param":"<ASR 原文>"}（动作名和 robot_ui 的
 * on_ai_command_received() 对上，手机端也订着它）。
 *
 * 界面拿它和 ai_reply 摆进同一条对话时间线（写完这句的是用户，回的是它）。 */

#define AI_CMD_ACTION_USER_SAID  "user_said"

/* 回传给界面的用户原话上限（字节，不含结尾 '\0'）。
 * MQTT 那一路：ai_network_publish_command() 的参数缓冲是 AI_CMD_PARAM_MAX(512)，
 * ASR 的 text_buf 也是 512，理论上塞得下；但 JSON 转义会把 " 和 \ 涨成 2 字节，
 * 撑爆后 ai_network_json_escape() 会把参数**整段丢空**（界面收到一条空消息）。
 * 直调那一路：界面自己的显示缓冲也只有 256（robot_ui_bridge.h 的
 * ROBOT_UI_BRIDGE_TEXT_MAX），两边在这里取同一个上限，省得两路显示不一样。 */
#define USER_SAID_MAX            256

/* ---- 异常声追问流程的参数 (W1) ---- */

/* 同一阵响声的防抖时间：这段时间内再来一次检测直接丢掉 */
#define ASK_DEBOUNCE_MS          10000

/* 每轮追问最多听多久（毫秒） */
#define ASK_LISTEN_TIMEOUT_MS    6000

/* 迟迟收不到"播放完成"回调时的兜底（毫秒）。
 * audio_play_start() 的播放被 stop 打断时回调不会触发，不兜底就一直卡在 SPEAKING。 */
#define ASK_SPEAK_TIMEOUT_MS     8000

/* 追问总轮数上限（含第一次）：问满这么多轮还听不清就上报"未确认" */
#define ASK_MAX_ROUNDS           2

/* 追问用的 TTS 缓冲（"请问需要帮助吗？"这种短句约 2 秒 ≈ 64 KB） */
#define ASK_TTS_BUF_BYTES        (128 * 1024)

/* ASR 结果缓冲 */
#define ASK_ANSWER_MAX           256

/* 报警/推送里的详情串长度 */
#define ASK_DETAIL_MAX           192

/* 追问回答的判定结果 */
#define ASK_VERDICT_REASSURED    0    /* 不用帮忙 */
#define ASK_VERDICT_EMERGENCY    1    /* 需要帮忙 */
#define ASK_VERDICT_UNCLEAR      2    /* 听不清 / 不含关键词 */

/* ---- 本机灯控（"说一句话就把灯打开"）----
 *
 * 队友的灯模拟器订阅的是 zhi_ai/<client_id>/device_cmd，command 收的是
 * **裸字符串** "on"/"off"（见 robot_ui/network_comm.c 的 send_device_command()：
 * cJSON_AddStringToObject(root, "command", command)，它不会帮你拼 JSON，
 * 所以这里千万别把 command 写成 {"state":"on"} 之类的整串）。 */

#define LIGHT_DEVICE_ID          "living_room_light"
#define LIGHT_CMD_ON             "on"
#define LIGHT_CMD_OFF            "off"

/* 命中后本地回话（走和 AI 回复同一条 TTS 播报链路） */
#define LIGHT_REPLY_ON           "好，灯打开了"
#define LIGHT_REPLY_OFF          "好，灯关掉了"

/* 命令没发出去时的兜底：宁可明说"没搞定"，也不能不出声或者让界面一直转圈 */
#define LIGHT_FAIL_ON            "网络没连上，灯没打开"
#define LIGHT_FAIL_OFF           "网络没连上，灯没关掉"

/* 灯控回话的 TTS 缓冲。这句话 6 个字 ≈ 1 秒 ≈ 32 KB，给 4 秒余量，
 * 免得 TTS 返回的 PCM 被底层按 buffer full 静默截尾（volc_tts.c 的行为）。
 * 必须走堆：静态 128 KB 会把内核 SRAM 顶掉，堆里的大块会落到 PSRAM。 */
#define LIGHT_TTS_BUF_BYTES      (128 * 1024)

/* 灯控识别的文本上限：ASR 这次给的就是 512 的缓冲，一句灯控命令远不到这么长，
 * 超过就按"拿不准"处理，直接退回大模型。 */
#define LIGHT_TEXT_MAX           512

/* 本进程镜像里 main() 进来过几次（2026-09-20 加的诊断量）。
 *
 * 它回答的是"这次重初始化到底是新任务，还是整个镜像重新加载了一遍"：
 *   - 同一大镜像里被别人 task_create 再拉一次（比如看护线程）→ **.data 还在**，
 *     计数接着涨（第 2 次、第 3 次…），配合同一行里的 up=（系统运行秒数，
 *     来自 clock_systime_ticks，不受本进程影响）能看得很清楚；
 *   - 芯片真的复位了 → .data 被重新初始化，**计数回到 1**，而且 up= 也很小。
 * （CONFIG_BUILD_FLAT 单一大镜像，所以这条 static 就是"这份镜像里的计数"。） */
static int g_start_seq;

/* 程序退出标志 */
static volatile sig_atomic_t g_running = 1;
static bool g_audio_started;
static bool g_sound_started;
static bool g_care_started;
static bool g_net_started;

/* 上层"要常听"的意图（不是"已经听上了"）。
 * 开机第一次 start_audio_listening() 之前就置 true：那一次要是就失败
 * （比如设备忙），监听守护才会去重试。stop_audio_listening() 里清掉，
 * 免得退出流程刚关了麦又被守护拉起来。 */

static volatile bool g_listen_wanted;

/* 「本 app 已经在跑」标记。**file-static，单一大镜像（CONFIG_BUILD_FLAT）里
 * 只有一份**，所以开机自启那次置上之后，再从 NSH 敲一次 ai_companion 也看得见。
 *
 * 为什么必须有它：本 app 同时是 NSH 命令，而 main() 一进来就会把**整条**初始化
 * 链路再走一遍（sm_init / audio_init / kws_init / 声音检测 / 关怀 / 网络 /
 * 工具注册 / 开麦）。这些"初始化"写的都是共享 static（g_sm_ctx / g_audio_ctx /
 * g_sound_ctx ...），第二个实例头一步就把正在跑的那个实例的上下文 memset 掉；
 * 那个实例的录音线程随即判定自己"已被新一代接管"，走 ai_audio.c 的 stale 收尾
 * ——只退出，**不 stop 也不 close**；板级那份全局录音会话于是成了"持有线程已消失"
 * 的形态，第二个实例的 audio_in_start() 正好照"残留自愈"把它抢了过去。
 * 结果是同一个半双工设备上出现**两个 read 客户端**。驱动那边 priv 是单实例，
 * rx_sem / rx_busy / RX 的 DMA 武装都只有一份，而 DMA 只可能指向"最后一次 read
 * 交进来的那个 buffer" —— "一次 read 一次武装一次 post" 的配对就此散掉，另一条
 * read 只能靠它自己那 5 秒超时收场。真机就在这条路上崩了，断言是
 * sem_waitirq.c:137；驱动自己早就把这条断言记成这套等待方式的已知风险
 * （sf32lb52_audio.c:3471 "若将来还见到 sem_waitirq 那条断言…"）。
 *
 * 所以第二次启动一律不初始化，只把参数交给调试入口（--sounddetect）。 */

static volatile bool g_app_inited;

/* 主循环心跳（2026-09-20 加）。
 *
 * 它是"这个实例还在往前走"的**唯一**证据：main_loop_task 每一拍的最开头落一次
 * 时间戳（g_beat_ms），单位是"开机以来毫秒"（TICK2MSEC(clock_systime_ticks())，
 * 和串口里那个 up= / 驱动那几条指纹日志是同一个基准，两条日志能直接对表）。
 *
 * 为什么会需要它：原来的"这个 app 还活着吗"用的是两种间接判据，两边都错过 ——
 *   - board 侧看护用 nxsched_get_tcb(pid) 查主线程 TCB：NuttX 里主线程退出
 *     不等于任务组没了（sched/group/group_leave.c：HAVE_GROUP_MEMBERS 下只有
 *     tg_members 空了才 group_release），ps 里真出现过"主线程睡在 sleep(1)、
 *     而它查不到 TCB"的形状；
 *   - main() 里那道 g_app_inited 门：一置真就永久挡（见下面那段注释）。
 *   两个一起作用就是现场那个形状：每 5 秒拉起一个新实例、每个都被挡回，
 *   而真正在跑的那个早就聋了 —— 从外面看像"机器好好的、就是不理人"。
 *
 * 为什么不用 main_now_ms()（本文件其它定时器用的那个）：它走 CLOCK_MONOTONIC，
 * 而本机有 TimeSync 在跑（clock_settime 改系统钟）。心跳要的是一个只会涨的量，
 * 墙钟被改一次就可能让"多久没心跳"算出荒唐值。
 *
 * 只有 main_loop_task 写，别的线程只读；判据本身留了几十倍余量，读到慢半拍的
 * 值没有任何影响，所以不加锁（和本文件那套诊断量一个口径）。 */

static volatile uint32_t g_beat_ms;

/* "位子是什么时候被认领的"：main() 里把 g_app_inited 置真的那一刻。
 * 给心跳当兜底：从认领到主循环第一次 ping 之间有 1~3 秒的初始化窗口，这段
 * 窗口里 g_beat_ms 还是上一代留下的旧值 —— 拿这一格当"最后一次活着"的证据，
 * 那段时间算出来的年龄就是"刚认领"，而不是"上一代死多久了"。 */

static volatile uint32_t g_inited_at_ms;

/* ★ 2026-09-20 晚定：**心跳只当仪表，不做任何决策**（用户拍板："没用的东西不要拖累
 * 软件"）。原来的形状是"主循环心跳超过 60 秒没动 ⇒ 判定停摆 ⇒ 看护拉起新实例 +
 * 新实例接管"，那个阈值已经删掉了，理由写在下面这段，值得下一个人先读：
 *
 *   主循环里**同步**跑着一整轮对话（sm_run() → sm_ai_talking_enter() →
 *   process_ai_dialogue() → 云端 ASR → 大模型最多 3 轮工具调用（每轮 30 秒超时）
 *   → TTS 合成（读超时 120 秒））。也就是说**"这一拍很长"和"这一拍卡死"在时间上
 *   长得一模一样**，任何"多久没动就算死"的窗口都会在正常但被网络拖慢的一轮里误判 ——
 *   而误判的代价是把正在说话的一个实例当场拆掉（见 g_app_inited 那段里
 *   sem_waitirq 那条断言）。
 *   把窗口缩到看护的轮询间隔（"这一轮心跳还在不在走"）只会更糟：ASR 单次就几秒。
 *   所以：**不设阈值 = 不做决策**。要"卡住就自动换一个"，得先把那段阻塞从主循环里
 *   挪到线程里，否则只能用猜。
 *
 * 代价（明明白白写在这里）：主循环真卡死时不会自己恢复，得靠 nsh 里敲
 * `ai_companion --assume-dead`（手动把一个卡住的实例换掉）或者断电。
 * 而"它到底是卡死、还是就在慢慢跑"这件事，现在**看得见**：
 *   - 串口：`[看护] …（位子空着…）` 那类行、以及 MQTT 快照里的 `lbeat`；
 *   - 不开串口：`py -3.10 tools/pc_env/diag_query.py` 直接读 `lbeat`
 *     （正常几十~几百毫秒；涨到几万就是那一拍真的在慢慢跑或卡住了）。 */

/* 落一跳心跳。**只在 main_loop_task 里调**，而且是每一拍的第一句：
 * 写在所有 tick 之前是刻意的 —— 这一拍卡在哪个 tick 里，时间戳也已经落下去了，
 * 判读的是"上一拍什么时候开始的"，不会出现"卡住的那一拍不算数"的盲区。 */

static void app_beat_ping(void)
{
  g_beat_ms = (uint32_t)TICK2MSEC(clock_systime_ticks());
}

/* 心跳的"年龄"，毫秒；-1 = 这份镜像里没有活着的实例（还没认领位子 / 已经退场）。
 *
 * ★ 唯一的调用者/判据是**"位子空着"这一件事**（board 侧看护：-1 ⇒ 上一个实例已经
 *   退场、没有任何人在跑 ⇒ 把它重新拉起来）。这是**零窗口**的判据 —— 不带任何时间
 *   阈值，也就没有任何误判余地；"心跳多大算太久"那种判断一律不做（理由见上）。
 *
 * 任何线程可调：只读两个 volatile 量 + 一个 tick 计数，不阻塞、不碰设备。 */

int ai_companion_beat_age_ms(void)
{
  uint32_t last;

  if (!g_app_inited)
    {
      return -1;
    }

  last = g_beat_ms != 0 ? g_beat_ms : g_inited_at_ms;
  if (last == 0)
    {
      return -1;
    }

  return (int)((uint32_t)TICK2MSEC(clock_systime_ticks()) - last);
}

/* 「重置」请求位（2026-09-20 晚加）：界面那个按钮 → 语义与两步流程写在
 * ai_companion_req.h 那一段里，这里只存一个位。
 *
 * 为什么动手的不是本文件：要换掉一个**卡在设备调用里**的实例，只能再起一个实例
 * 去接（卡住的正是这个实例的主循环，它已经推不动任何东西了）。所以真正拉新实例的
 * 是 board 侧看护线程（sifli_ap.c 的 hello_app_watchdog）—— 下面两个函数就是给它
 * 的：requested() 是它每 5 秒看一眼的位，accept() 是它认领后清的。 */

static volatile bool g_takeover_req;

void ai_companion_request_takeover(void)
{
  g_takeover_req = true;
}

int ai_companion_takeover_requested(void)
{
  return g_takeover_req ? 1 : 0;
}

void ai_companion_takeover_accept(void)
{
  g_takeover_req = false;
}

/* 「接管后自检」的时刻（0 = 没有待做的自检）。
 *
 * 为什么需要它：接管（不管是手动 --assume-dead 还是界面「重置」）最要紧的问题是
 * **"设备到底起没起来"**。而接管那一刻看不出来 —— audio_record_start() 成功只说明
 * 板级封装把 START 发下去了，上层框架完全可能把 CONFIGURE/START 静默吞掉
 * （钉住的共享 status），于是要等第一次 read 才能看出真相。
 * 所以接管路径埋一个 3 秒后的检查点，由主循环那一拍打一行**证据**出来：
 * 录音 active / idle / lres + 驱动那四个计数（irq/half/armed/busy）。
 * 判读写在那一行里，不用回来翻代码。 */

#define APP_TAKEOVER_SELFCHECK_MS   3000

static volatile uint32_t g_takeover_check_at_ms;

/* 开机以来的毫秒数（和 g_beat_ms 同一个基准：只涨、不受改墙钟影响）。 */

static uint32_t app_now_ms(void)
{
  return (uint32_t)TICK2MSEC(clock_systime_ticks());
}

/* 主循环每拍调一次；没排自检时它只是一次判断。函数体放在主循环那一段
 * （它要用 g_audio_ctx，而那个 static 在这下面才声明）。 */

static void takeover_selfcheck_tick(void);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 全局状态机上下文 */
static sm_context_t g_sm_ctx;

/* 全局音频模块上下文 */
static audio_context_t g_audio_ctx;

/* 全局LLM模块上下文 */
static llm_context_t g_llm_ctx;

/* 全局声音检测上下文 */
static sound_detect_context_t g_sound_ctx;

/* 全局主动关怀上下文 */
static care_context_t g_care_ctx;

/* 全局网络上下文（AI 回复/表情/报警要回传给界面，见 ai_network.h） */
static ai_network_context_t g_net_ctx;

/* 网络回传通道的**补连**状态（主循环每拍跑 net_retry_tick，见那一段的说明）：
 *   g_net_client_id    —— 补连时复用 main() 收到的 --client-id（可能是 NULL，
 *                         ai_network_set_client_id() 自己会兜默认值）
 *   g_net_retry_at     —— 下次补连的时间点（0 = 还没排，先等 NET_RETRY_INTERVAL_MS）
 *   g_net_retry_count  —— 补连尝试次数（日志节流 + 诊断快照的 nrt 字段） */

static const char *g_net_client_id;
static uint32_t    g_net_retry_at;
static uint32_t    g_net_retry_count;

/* 语音段累积缓冲区 (用于 ASR) */

#define SPEECH_BUF_MAX_FRAMES  (16000 * 10)  /* 最长10秒 @16kHz */

/* 一段语音至少要有多长才值得送去识别：1600 帧 = 100ms @16kHz。
 * 两个地方共用它，口径必须一致：
 *   - process_ai_dialogue()：不够长就"语音数据不足，跳过"（不白跑一次识别）；
 *   - ai_companion_voice_submit()：「提交」按同一个门槛判"有没有听到人说话"
 *     —— 不够长就什么都不登记，界面提示"没听到你说什么"。 */

#define SPEECH_MIN_FRAMES_FOR_ASR  1600

static int16_t *g_speech_buf = NULL;
static size_t   g_speech_frames = 0;
static bool     g_speech_capturing = false;

/* 上一次成功发给界面的语音状态串（空串 = 还没发过），去重就靠它 */
static char g_voice_state_sent[16];

/* 上一次判去重时"界面就绪了没有"（robot_ui_bridge_is_ready() 的结果）。
 * 状态串没变、但界面从"没就绪"变成"就绪"时要补推一次 —— 那一轮之前的推送
 * 都被桥接那层丢掉了（界面还在 lv_init），不补的话界面上就永远停在开机默认值。
 * 界面一直没起来（robot_ui 不在跑）时它恒为 false，去重照常生效，不会刷屏。 */
static bool g_voice_state_ui_ready;

/* 本轮对话是否已经出过声：两个播放完成回调置位，VAD 又听到人开始时清掉。
 *
 * 播放线程的收尾顺序是"先关播放/恢复录音，再回调上层"，中间有一小段
 * 已经不在放音、状态机却还没从 AI_TALKING 切走的时间。没有这个标志的话，
 * 界面会从"正在说话…"闪一下"正在想…"再回到空闲。 */
static volatile bool g_voice_spoke;

/****************************************************************************
 * 唤醒词「你好，openvela」/「Hello，openvela」的命令词识别 (KWS)
 *
 * 识别算法全在 kws_dtw.c 里（纯本地纯计算，不开麦、不起线程）。这里只做
 * 接线，两件事：
 *   - 录音线程的 audio_data_callback() 里把每块 PCM 喂给 kws_feed()，
 *     命中时**只置一个标志**（回调每 20ms 跑一次，DTW 那一下是毫秒级突发，
 *     不能让录音线程去做流程：不在这里发网络、不碰 LVGL、不刷串口）；
 *   - 主循环 kws_wake_tick() 取这个标志，走和"VAD 检测到有人说话"同一条路。
 *
 * 两个标志都是"一个线程写、另一个线程读"的单个 bool，在 ARM32 上对齐读写
 * 不会读到半截值，所以不加锁。但两个都加了 volatile：不加的话编译器完全
 * 可以把"读一次就再也不变"当成前提，把循环里每次都该重读的判断变成常量
 *（kws_dtw 自己也要求它的接口只被一个线程调 —— 那个线程就是录音线程，
 * kws_init() 在它起来之前调一次）。
 *
 * g_kws_ready 是开机时定下来的模板条数，只用来打日志。
 * g_kws_enabled = 有模板才算"功能开了"：没有模板时 kws_feed 永远返回 0，
 *   喂不喂结果一样，跳过只是为了省掉 MFCC 那 2~3% 的 CPU。也正因为跳过，
 *   现场没录模板时，整条链路的行为和接这个模块之前**完全一样**（只有 VAD 触发）。
 *   ⚠ 它**不是**开机时定死的：每次喂帧前 kws_update_enabled() 会重问一遍
 *   （模板可能是这次开机之后才录进 /data/kws 的），只置真、不清假。
 ****************************************************************************/

static int           g_kws_ready;      /* 开机时从 /data/kws 载入的模板条数 */
static volatile bool g_kws_enabled;    /* 有模板才喂帧、才处理命中（运行中可变） */
static volatile bool g_kws_hit;        /* 录音线程置：刚命中唤醒词 */

/****************************************************************************
 * 异常声 -> 追问流程的共享状态 (W1)
 *
 * 相位（g_ask_phase）只有一个写者：main_loop_task 里的 ask_flow_tick()。
 * 别的线程（播放线程 / 录音线程 / 检测线程）只置事件标志，不改相位，
 * 免得几个人同时推流程。
 ****************************************************************************/

typedef enum
{
  ASK_PHASE_IDLE = 0,     /* 空闲，等着检测到异常声 */
  ASK_PHASE_PENDING,      /* 检测到了，等 tick 开始问 */
  ASK_PHASE_SPEAKING,     /* 正在说「请问需要帮助吗？」 */
  ASK_PHASE_LISTENING     /* 限时听回答：等 ASR 结果或超时 */
} ask_phase_t;

static volatile int      g_ask_phase = ASK_PHASE_IDLE;
static volatile int      g_ask_pending_type = SOUND_TYPE_NONE;
static volatile float    g_ask_pending_conf;
static volatile int      g_ask_round;          /* 已经问过几轮 */
static volatile bool     g_ask_play_done;      /* 播放线程置：这轮的问话说完了 */
static volatile bool     g_ask_answer_ready;   /* 录音线程置：ASR 有结果了（空串=没听清） */
static volatile uint32_t g_ask_phase_ms;       /* 进入当前相位的时间 */
static volatile uint32_t g_ask_deadline_ms;    /* ASK_PHASE_LISTENING 的截止时间 */
static volatile uint32_t g_last_abnormal_ms;   /* 上次触发时间（防抖用） */
static char              g_ask_answer[ASK_ANSWER_MAX];

static void print_usage(const char *program)
{
  printf("用法: %s [选项]\n", program);
  printf("  --ask <文本>          启动后向 MiMo/ai_agent 发送文本\n");
  printf("  --sound-self-test     注入测试冲击声，验证检测链路\n");
  printf("  --ask-self-test       注入一条假的异常声，跑一遍追问流程\n");
  printf("  --client-id <ID>      MQTT 客户端 ID（默认 %s）\n",
         AI_DEFAULT_MQTT_CLIENT_ID);
  printf("  --sounddetect <子命令> [n]\n"
         "                        任务四门控调试：status（打印统计）/\n"
         "                        on / off / sensitivity <n>（默认 1.0，越大越敏感）\n");
  printf("  --assume-dead         手动接管：把当前跑着的实例拆掉、按全新启动重来\n"
         "                        （没有自动接管了；这是不用断电把卡住的实例换掉的入口）\n");
  printf("  --help                显示帮助\n");
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * @brief  取单调时钟的毫秒数（追问流程算超时用）
 */

static uint32_t main_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/****************************************************************************
 * 语音状态回传 (S2)
 *
 * 界面（robot_ui）只能从 MQTT 知道语音链路走到哪一步了，这里把
 * 「在听 / 在想 / 在说 / 空闲」推过去。没有新的状态机，四个态全部从
 * 现成的量算出来（唯一多记的是"这轮出过声没有"，见 g_voice_spoke）：
 *   - audio_is_playing()   板级播放标志（TTS 出声期间为真）
 *   - g_ask_phase          异常声追问流程的相位
 *   - g_speech_capturing   VAD 报过"开始说话"、还没报"说完了"
 *   - sm_get_state()       IDLE / LISTENING / AI_TALKING / ...
 ****************************************************************************/

/**
 * @brief  算出当前该显示哪个语音状态
 */

static const char *voice_state_probe(sm_context_t *ctx)
{
  /* 扬声器在响 = 我在说话。这条要排最前面：TTS 出声时状态机还停在
   * AI_TALKING，不先判播放，界面就会一直显示"正在想…"。 */

  if (audio_is_playing(&g_audio_ctx) || g_ask_phase == ASK_PHASE_SPEAKING)
    {
      return VOICE_STATE_SPEAKING;
    }

  /* 正听到人说话，或追问流程在限时等回答 = 界面上的「检测到声音」那一档。
   * 追问那条链路在等回答期间状态机可能还停在上一轮的 AI_TALKING
   * （要等 ask_flow_finish() 才补 AI_RESPONSE），所以这里要排在状态机前面。 */

  if (g_speech_capturing || g_ask_phase == ASK_PHASE_LISTENING)
    {
      return VOICE_STATE_LISTENING;
    }

  /* 追问流程刚被异常声触发、问句还在 TTS 合成里（这个相位从"排入"一直
   * 持续到 ask_flow_begin() 里开播）= 跟等大模型回复一样，算"正在想" */

  if (g_ask_phase == ASK_PHASE_PENDING)
    {
      return VOICE_STATE_THINKING;
    }

  /* AI_TALKING 覆盖 ASR + 等大模型 + TTS 合成这一整段。
   * 已经出过声的那一小段（见 g_voice_spoke）不算"正在想"。 */

  if (sm_get_state(ctx) == SM_STATE_AI_TALKING && !g_voice_spoke)
    {
      return VOICE_STATE_THINKING;
    }

  /* 剩下的都当空闲：IDLE / LISTENING（VAD 常开但没人说话）/ 报警态。 */

  return VOICE_STATE_IDLE;
}

/**
 * @brief  把上面的状态串翻成"直调界面"那个接口的档位
 *
 * 状态串是 MQTT 那一路的线协议（要和 robot_ui 的 on_ai_command_received()
 * 对上，也要和手机端对上），档位枚举是直调那一路的接口（在 robot_ui_bridge.h
 * 里自己定了一套，因为那个头文件不能引 LVGL）。两套值就在这里对上，
 * 别的地方一律用串，免得出现第三种表示。
 *
 * 认不出来的串一律按"空闲"处理：界面宁可少刷一次，也不要停在一个错的字上。
 */

static int voice_state_to_bridge(const char *state)
{
  if (state == NULL)
    {
      return ROBOT_UI_BRIDGE_VOICE_IDLE;
    }

  if (strcmp(state, VOICE_STATE_LISTENING) == 0)
    {
      return ROBOT_UI_BRIDGE_VOICE_LISTENING;
    }

  if (strcmp(state, VOICE_STATE_THINKING) == 0)
    {
      return ROBOT_UI_BRIDGE_VOICE_THINKING;
    }

  if (strcmp(state, VOICE_STATE_SPEAKING) == 0)
    {
      return ROBOT_UI_BRIDGE_VOICE_SPEAKING;
    }

  return ROBOT_UI_BRIDGE_VOICE_IDLE;
}

/**
 * @brief  语音状态有变化就发给界面（主循环每 100ms 调一次）
 *
 * 只在真的变化时发：状态没变就一个字节都不发（否则一秒十条，MQTT 和界面
 * 日志都别看了）。
 *
 * ★ 两条路一起走，不是二选一：
 *   1) **同进程直调**（主路）：robot_ui_bridge_voice_state()，本地立刻刷屏。
 *      整机是单一大镜像，界面就在同一个地址空间里，本机自己的界面反馈完全
 *      不该依赖公网一个来回 —— 真机上就是因为这条 MQTT 路断着（开机时
 *      DNS 还没就绪 -> MQTT connect 失败 -> g_net_started 恒为 false），
 *      老人说话时界面从头到尾一动不动，看着就是"卡死/没反应"。
 *   2) **MQTT 照发**（保留）：手机/PC 端还订阅着 zhi_ai/<client_id>/command，
 *      它们不在这个地址空间里，只能靠 MQTT。断了它们就瞎了。
 *      发失败只打一行日志：这只是个提示，不能反过来影响语音链路。
 */

static void voice_state_tick(sm_context_t *ctx)
{
  const char *want;
  bool ui_ready;

  want = voice_state_probe(ctx);
  ui_ready = robot_ui_bridge_is_ready();

  /* 去重：状态没变、而且界面的就绪状态也没变，就一个字节都不发。
   *
   * 为什么要带上 ui_ready 一起判：界面比我们先跑起来的情况是存在的
   * （robot_ui 还在 lv_init / 建控件时我们这一轮就已经在推状态了，那种推送会被
   * 桥接那层丢掉）。只看状态串的话，那一次"被丢掉的 idle"会被记成"已发"，
   * 之后再也不会补 —— 界面上就一直停在开机默认显示。带上这一位之后，
   * 界面就绪那一刻会**补推一次当前状态**，之后照常去重。
   * 也不会因此刷屏：界面一直没起来时这一位恒为 false，去重照旧生效。 */

  if (strcmp(want, g_voice_state_sent) == 0 && ui_ready == g_voice_state_ui_ready)
    {
      return;
    }

  /* 先记下来再发：发不出去也不重试（网络不好时这里会一直刷失败日志），
   * 界面看到的就还是上一次成功收到的状态。 */

  snprintf(g_voice_state_sent, sizeof(g_voice_state_sent), "%s", want);
  g_voice_state_ui_ready = ui_ready;

  /* 主路：同进程直调界面（无网络依赖，任何时刻都能刷） */

  robot_ui_bridge_voice_state(voice_state_to_bridge(want));

  /* 旁路：公网 MQTT，给手机/PC 端。没连上就只记一条日志 —— 界面那边已经由
   * 上面那条直调刷过了，这里失败不再等于"用户看不到"。 */

  if (!g_net_started)
    {
      /* 回传通道没起来（开机时 RNDIS/DNS 还没就绪的常见情况）：本地语音链路
       * 和界面直调都照常，只是手机端收不到状态了。 */
      return;
    }

  if (ai_network_publish_command(&g_net_ctx, VOICE_STATE_ACTION, want) < 0)
    {
      printf("[语音] 状态 MQTT 回传失败: %s（界面已由直调刷过，手机端看不到）\n",
             want);
    }
}

/* 这里原来有一个 signal_handler()，只做一件事：收到 SIGINT/SIGTERM 就
 * `g_running = 0`。**2026-09-19 已连同 signal() 注册一并删除** ——
 * 它让这个开机常驻的 app 能被外部信号整台关掉、而且关掉之后没人拉起来。
 * 完整理由见 ai_companion_main() 里那段"不注册 SIGINT / SIGTERM"的说明。 */

/**
 * @brief  开始累积一句话（VAD 报"人开始说话" 和 唤醒词命中 共用这一段）
 *
 * 抽出来的目的就是让"唤醒词命中"和"检测到有人说话"走**同一段代码**：
 * 两条路要的后续完全一样（清累积长度 → 置 capturing → 状态机收 WAKEUP →
 * 等这一次说话结束时报 VOICE_COMPLETE，由 AI_TALKING 进去送 ASR）。
 * 合成一处以后改一边不会漏另一边。
 */

static void speech_capture_begin(sm_context_t *ctx)
{
  g_speech_frames = 0;              /* 从这一块开始重新累积 */
  g_speech_capturing = true;
  g_voice_spoke = false;            /* 新一轮说话开始：这轮还没出过声 */
  sm_handle_event(ctx, SM_EVENT_WAKEUP);
}

/**
 * @brief  这一段语音说完了：走 ASR（VAD 静音超时 和 「提交」共用这一段）
 *
 * 只有一份是刻意的：「提交」只是把静音超时**提前**，语义必须和自动断句完全
 * 一致（清 capturing → 状态机 VOICE_COMPLETE → AI_TALKING → process_ai_dialogue
 * 送 ASR）。两条路的差别只有"谁在什么时候调它"：
 *   - 自动断句：ai_audio.c 的录音线程判到静音超时 → vad_callback(false) → 这里；
 *   - 「提交」：录音线程的数据回调（audio_data_callback）认领到请求 → 这里。
 * 两条都在**同一条录音线程**上，所以不存在"两个线程同时收尾"的时序问题。
 */

static void speech_capture_complete(sm_context_t *ctx)
{
  printf("[VAD] 语音结束 (累积 %zu 帧)\n", g_speech_frames);
  g_speech_capturing = false;
  sm_handle_event(ctx, SM_EVENT_VOICE_COMPLETE);
}

/**
 * @brief  VAD回调 - 语音活动检测
 */

static void vad_callback(bool speech_detected, void *user_data)
{
  sm_context_t *ctx = (sm_context_t *)user_data;

  if (speech_detected)
    {
      printf("[VAD] 检测到语音开始\n");
      speech_capture_begin(ctx);
    }
  else
    {
      speech_capture_complete(ctx);
    }
}

/**
 * @brief  重新评估「唤醒词功能开了没」（只在录音线程里调）
 *
 * g_kws_enabled 原来是开机时算一次就定死的，而模板**可以在运行中出现**：
 * NSH 里 `hw_test kws enroll <slot>` 现录一条，或者别的路径往 /data/kws 拷了
 * 文件再调 kws_template_load_all()。只在开机判一次，这些模板就要等到下次重启
 * 才生效 —— 而重启又把 /data 清空，等于永远用不上。所以每次喂帧前重问一遍。
 *
 * 代价可以忽略：功能已开时只是一次提前返回；没开时是一次 4 个槽的整数比较
 * （kws_ready_count 就是数 g_tpl_len[s] > 0 的个数），远小于一次 kws_feed。
 *
 * 只置真、不清假：写 g_kws_enabled 的只有这里（录音线程）和开机初始化那一步，
 * 两处都只会置真。注意这跟 kws_wake_tick() 里那句 `if (!g_kws_enabled) return;`
 * **没有关系** —— 能置上 g_kws_hit 就说明这一轮确实喂过帧（当时 enabled 必为真），
 * 命中不会因为 enabled 之后取什么值而失效。那句是纯防御：万一以后真加了
 * "运行中关掉唤醒词"的路径，那里不用再改一遍。现在不动它的行为，也不删它。
 */

static void kws_update_enabled(void)
{
  int ready;

  if (g_kws_enabled)
    {
      return;
    }

  ready = kws_ready_count();
  if (ready <= 0)
    {
      return;
    }

  g_kws_enabled = true;
  printf("[KWS] 运行中出现 %d 条模板 → 唤醒词功能启用"
         "（词：你好，openvela / Hello，openvela）\n", ready);
}

static void audio_data_callback(const int16_t *data, size_t frames,
                                void *user_data)
{
  sm_context_t *ctx = (sm_context_t *)user_data;

  /* 累积语音数据到缓冲区，供 ASR 使用 */

  if (g_speech_capturing && g_speech_buf != NULL)
    {
      size_t space = SPEECH_BUF_MAX_FRAMES - g_speech_frames;
      size_t copy = frames < space ? frames : space;
      if (copy > 0)
        {
          memcpy(&g_speech_buf[g_speech_frames], data,
                 copy * sizeof(int16_t));
          g_speech_frames += copy;
        }
    }

  /* 「提交」（语音聊天镜像面板底部那个按钮）：外部请我们"立刻收尾这一段"，
   * 就在这里做掉。
   *
   * ★ 为什么偏偏在这条线程、这个位置：
   *   - 这里是**录音线程**（ai_audio.c 的 audio_record_thread 每帧回调一次），
   *     而 VAD 判"说完了"本来也在这条线程上，就在本回调前面几行。用同一条线程、
   *     同一段收尾代码（speech_capture_complete），就不存在"两条路同时收尾"的
   *     时序问题；
   *   - 换别的线程做（主循环）会把整段 ASR（阻塞 HTTPS）拽到那条线程上阻塞
   *     几秒甚至几十秒 —— 主循环被拽住会拖住监听守护和追问流程。这个坑本文件
   *     踩过（见监听守护那一节的说明），所以「提交」不往那条线程上加东西。
   *
   * 顺序：先认领（一次性，读走就清），再判有没有真的在累积 —— 没有就安静地丢掉
   * （界面那边已经据此提示过"没听到你说什么"了，这里只留一行日志）。
   * ⚠️ 必须同时把 VAD 的相位收掉（audio_vad_end_speech）：不然 3 秒后静音超时
   * 还会就**同一段音频**再报一次"语音结束"，同一句话会被送去识别两次。 */

  if (ai_companion_voice_submit_take())
    {
      if (g_speech_capturing && g_speech_buf != NULL && g_speech_frames > 0)
        {
          printf("[语音] 「提交」：不等静音超时，立刻收尾这一段(累积 %zu 帧)\n",
                 g_speech_frames);
          audio_vad_end_speech(&g_audio_ctx);
          speech_capture_complete(ctx);
        }
      else
        {
          /* 竞态的正常一面：人刚好在这前后自己停了（VAD 抢先收尾）/ 按下按钮
           * 时还没真正开始说。什么都不做，别把空的一轮塞给状态机。 */
          printf("[语音] 「提交」：当前没有在累积的语音，忽略这一次\n");
        }
    }

  /* 送入唤醒词识别（命令词，纯本地计算）。
   *
   * 只在录音线程里调（kws_dtw 的线程约束就是"接口只允许一个线程调"），
   * 这个回调正是那个线程。帧口径：frames 是**每通道采样点数**，
   * 本板单声道 → 就是采样点数，正好是 kws_feed() 要的口径；
   * 窗边界不用我们管，它内部自己攒 20ms 窗（10ms 跳，50% 重叠）。
   *
   * 命中时只置标志：流程交给主循环的 kws_wake_tick()。这里绝不能做
   * 耗时/阻塞的事 —— 上一次命中时它会连着跑 1~4 次 DTW（合计毫秒级突发），
   * 再叠上 TTS/网络/LVGL 就会把这个 20ms 一次的录音回调拖垮。
   *
   * 先重问一次"现在有没有模板"：模板可能是这次开机之后才录进 /data/kws 的
   * （见 kws_update_enabled），只在开机判一次的话那些模板永远用不上。 */

  kws_update_enabled();

  if (g_kws_enabled && kws_feed(data, frames) == 1)
    {
      g_kws_hit = true;
    }

  /* 送入声音检测器 */

  if (g_sound_started)
    {
      int ret = sound_detect_feed(&g_sound_ctx, data, frames);

      /* -EINVAL 表示"检测器当前不在收数据状态"：检测线程正把上一窗口拿去处理，
       * state 短暂变成 PROCESSING，这时喂数据是正常被拒，不是故障。
       * 原来每帧打一行，串口被刷屏，真日志全淹没了。 */
      if (ret < 0 && ret != -ENOSPC && ret != -EINVAL)
        {
          printf("[安全] 音频送入检测器失败: %d\n", ret);
        }
    }
}

/**
 * @brief  唤醒词命中后的处理（只在主循环里跑，每 100ms 一次）
 *
 * 录音线程只置 g_kws_hit，流程在这里推。为什么不在录音回调里直接
 * sm_handle_event(SM_EVENT_WAKEUP)：命中那一下要跑 DTW（毫秒级突发），
 * 而且我们这个应用里"喂帧"和"推流程"本来就各在一个线程，分开更清楚。
 *
 * 三路：
 *   1) 没模板（g_kws_enabled == false）：回调根本没喂过帧，走不到这里；
 *   2) 正忙（播报 / 送 ASR / 等大模型 / 追问流程在跑）：丢弃这次命中，
 *      不打断正在进行的事 —— 老人说话时喇叭在响（或刚响完）是常态，
 *      这时候插一脚会把当前那轮对话搅乱；
 *   3) 常听（没在送 ASR/等大模型/播报）：醒。
 *
 * 醒法就是 speech_capture_begin()，和"VAD 检测到有人说话"逐字相同的一段。
 *
 * ★ 唤醒词本身算不算要送给 ASR 的内容：**不算**，所以这里要把已经累积的
 *   帧清零（speech_capture_begin 第一行）。理由是 kws_feed 的命中时机：
 *   整句 DTW 必须等人说完，它是**尾静音 300ms 之后**才报命中的，那时候
 *   "你好，openvela"这句话已经一块块经过回调、躺在 g_speech_buf 里了。
 *   不清零就等于把唤醒词跟着后面的命令一起送去 ASR，大模型会以为用户
 *   说了一句叫它名字的话（今天没接线时就是这个行为：喊一句唤醒词就会
 *   触发一次"识别 + 回复"）。清零之后送 ASR 的只有命中之后的音频，
 *   也就是用户真正要说的话。
 *
 * 和录音线程的写竞争：g_speech_frames 这里（主循环）也在写，理论上可能和
 * 录音线程那一次 memcpy/累加交错，最坏结果只是"这次清零没完全生效"
 * （唤醒词残留一点尾巴）或"清零后又被加上一块"。不会越界：录音线程的
 * 写入量是按快照算过上限的（copy ≤ 上限 − 快照值）。为这点概率加锁会把
 * 每 20ms 一次的录音回调一起锁进去，不值。
 */

static void kws_wake_tick(sm_context_t *ctx)
{
  if (!g_kws_hit)
    {
      return;
    }

  g_kws_hit = false;

  if (!g_kws_enabled)
    {
      return;
    }

  /* 正忙就丢弃：判据和 listen_supervise_tick() 的"正忙"保持一致 */

  if (audio_is_playing(&g_audio_ctx) ||
      g_ask_phase != ASK_PHASE_IDLE ||
      sm_get_state(ctx) == SM_STATE_AI_TALKING)
    {
      printf("[KWS] 命中唤醒词，但当前正忙（AITalking/播报/追问），丢弃\n");
      return;
    }

  printf("[KWS] 唤醒词命中：从这一刻起收用户的话"
         "（丢掉已累积的 %zu 帧 = 唤醒词本身）\n", g_speech_frames);

  speech_capture_begin(ctx);
}

/****************************************************************************
 * 任务四：麦克风 PCM 旁路 → 人声/非人声门控（接线）
 *
 * 门控线程（ai_sound_detect.c 里，独立低优先级）每 200ms 判一个 0.64s 窗：
 *   人声 → 这里只置一个标志，主循环 100ms 心跳收走，复用**现有**的云端
 *          语音入口（speech_capture_begin：VAD/唤醒词那条路，后面就是
 *          ASR → mimo_chat 云端大模型），不另造一条；
 *   非人声异常 → 门控线程直接喂 sound_event_feed()（本地小模型，任务三）；
 *   播放中 → 门控自己整窗跳过（判据是本文件给的 gate_busy_pred）。
 *
 * ★ 为什么"人声"这一步要绕一圈标志，而不是在门控线程里直接
 *   speech_capture_begin()：那个函数会写 g_speech_capturing / 推状态机，
 *   而它们的主人是录音线程和主循环（见 vad_callback 那一节的并发说明）。
 *   从第三条线程伸手进去改，等于把"谁在收尾/谁在清零"那套时序打散 ——
 *   本文件已经为这个坑付过代价（追问流程和唤醒词都只在主循环里推）。
 ****************************************************************************/

static volatile bool  g_gate_voice_hint;
static volatile float g_gate_voice_ac;
static volatile float g_gate_voice_zcr;

/**
 * @brief  门控线程里的人声回调（**跑在门控线程，只许置标志**）
 */

static void gate_voice_cb(const sound_gate_features_t *f, void *arg)
{
  (void)arg;

  if (f == NULL)
    {
      return;
    }

  g_gate_voice_ac  = f->ac_peak;
  g_gate_voice_zcr = f->zcr;
  g_gate_voice_hint = true;
}

/**
 * @brief  播放/忙判据（门控线程里调，必须极快）
 *
 * 板子自己喇叭出声时返回 true → 门控整窗不判。半双工下播放期间录音本来就停，
 * 这里兜的是"停之前已经灌进环形缓冲的那几帧"（历史误报：自己的 TTS 被当成异常声）。
 */

static bool gate_busy_pred(void *arg)
{
  (void)arg;
  return audio_is_playing(&g_audio_ctx);
}

/**
 * @brief  人声提示的心跳（只在主循环里跑，每 100ms 一次）
 */

static void gate_voice_tick(sm_context_t *ctx)
{
  if (!g_gate_voice_hint)
    {
      return;
    }

  g_gate_voice_hint = false;

  /* 正忙就丢弃：和 kws_wake_tick / listen_supervise_tick 的判据一致
   * （喇叭在响 / 送 ASR / 追问中都不打断，而且这时候 VAD 正在收同一段音频） */

  if (audio_is_playing(&g_audio_ctx) ||
      g_ask_phase != ASK_PHASE_IDLE ||
      sm_get_state(ctx) == SM_STATE_AI_TALKING)
    {
      printf("[门控] 判到人声，但当前正忙（AITalking/播报/追问），丢弃\n");
      return;
    }

  if (g_speech_capturing)
    {
      return;    /* VAD 已经在收这段话了，不用再喊一次 */
    }

  printf("[门控] 判到人声 (ac=%.2f zcr=%.2f) → 交给云端语音链路\n",
         (double)g_gate_voice_ac, (double)g_gate_voice_zcr);

  /* 复用现有入口：和 VAD「检测到有人开始说话」逐字相同的下一步 */

  speech_capture_begin(ctx);
}

/**
 * @brief  初始化音频并启动VAD监听
 */

static int start_audio_listening(sm_context_t *ctx)
{
  int ret;

  /* 启用VAD检测 */

  audio_vad_enable(&g_audio_ctx, vad_callback, ctx);

  /* 开始录音 */

  audio_record_config_t record_cfg =
  {
    .enable_vad = true,
    .silence_timeout_ms = AUDIO_VAD_SILENCE_TIMEOUT_MS,
    .min_speech_ms = AUDIO_VAD_MIN_SPEECH_MS,
    .data_callback = audio_data_callback,
    .user_data = ctx
  };

  ret = audio_record_start(&g_audio_ctx, &record_cfg);
  if (ret < 0)
    {
      printf("[错误] 启动录音失败: %d\n", ret);
      return ret;
    }

  printf("[音频] 开始语音监听\n");
  g_audio_started = true;
  return OK;
}

/**
 * @brief  停止音频监听
 */

static void stop_audio_listening(void)
{
  /* 先关"要常听"的意图：主循环里的监听守护一直在看这个标志，
   * 不收掉的话退出流程刚把设备还回去，守护就把麦又打开了。 */

  g_listen_wanted = false;

  if (!g_audio_started)
    {
      return;
    }

  audio_record_stop(&g_audio_ctx);
  audio_vad_disable(&g_audio_ctx);
  g_audio_started = false;
  printf("[音频] 停止语音监听\n");
}

/* ★ 网络回传通道的补连间隔（毫秒）：开机那一次 ai_network_start_shared() 失败
 * （RNDIS / DNS 还没就绪的常见情况）之后，主循环每隔这么久再试一次，成功即停。
 *
 * 为什么必须有它：g_net_started 只在开机成功时置真一次，而 voice_state /
 * user_said / ai_reply / 报警 这一整排上报全在 `if (!g_net_started) return;`
 * 之后 —— 开机没连上就是**整场一条都不发**，从外面（MQTT 是唯一的观测通道）
 * 完全看不出 hello_app 是死是活。
 *
 * 为什么取 10 秒：单次尝试本身最长要等 AI_MQTT_SHARED_WAIT_MS = 3000ms
 * （先等界面的 network_task 把连接建起来，见 ai_network.c），而它跑在**主循环
 * 这条线程**上 —— 试得太密会把监听守护这一拍一起拖慢。10 秒 = 最多 3 秒
 * 等待 + 7 秒正常干活。失败日志按下面的节流打，不刷屏。 */

#define NET_RETRY_INTERVAL_MS    10000
#define NET_RETRY_LOG_FIRST      3     /* 前 3 次每次打（开头最需要看见） */
#define NET_RETRY_LOG_EVERY      6     /* 之后每 6 次（约 1 分钟）一条 */

/****************************************************************************
 * ai_companion_diag_snapshot() 的内部门面（薄壳在 ai_companion_diag.c）
 *
 * robot_ui 的 network_task 每 30 秒（心跳里那个 ha 位）和每次
 * {"action":"diag"} 都会来这里读一次。本函数是整个模块里**唯一**能读到这一堆
 * static 的地方，它只读、不写、不碰设备、不加锁 —— 那三条纪律的来由写在
 * ai_companion_diag.h 头上（调用方是 MQTT 收包线程，它一阻塞，唯一的观测通道
 * 就一起没了）。不能碰设备这一条尤其重要：本文件里所有会动音频设备的动作都排在
 * 主循环那一条线程上（见下面"常听监听守护"那一节），这里多伸一只手就是那次
 * 事故的形状。
 *
 * 为什么交出去的是一行 "key=value" 文本、而不是直接拼好 JSON：
 *   键名和字段口径（哪些写数字、拿不到写什么、缓冲小了怎么截断）归
 *   ai_companion_diag.c；这里只负责"值"。而 robot_ui 也会 include 那个头文件，
 *   用文本就省得把 hello_app 的内部类型（甚至一个专用结构体）暴露到 app 外面。
 *
 * 为什么在这里读状态是安全的（可能读到慢一拍的值，但不会读到"半截"）：
 *   一个线程写、其他线程读的这几个量全是 32 位对齐的 bool / int / uint32
 *   （Cortex-M 上的读写本身就是原子的），把它们加锁反而等于把调用方挂到
 *   hello_app 的线程上；诊断要的是"现在大概什么形状"，慢一拍正是那点代价。
 *   唯一要留神的是录音链路正被停/重开的那一小段：那时录音的几个标志本来就是
 *   半程状态，读出来的组合可能自相矛盾（比如 ract=0 而 recording=1）。
 *   这不是 bug：报文里 rec / ract / died / exit 四个字段合起来看就分得清。
 *
 * 两个时间戳（vad / asr）这里**不写**：本文件里 VAD 回调、ASR 那两条路上
 * 没有记时刻的地方，硬编一个 -1 出来只会让人以为"记了但是没发生"。
 * 按 diag.c 的规矩，"没写进来的键 = 取不到"，那边会输出 -1；以后真在这里
 * 记了时刻，只要按同样的写法加进这一行，报文那侧一个字节都不用改。
 ****************************************************************************/

void ai_companion_state_snapshot_impl(char *buf, size_t len)
{
  struct sf32lb52_audio_rx_stats_s rxst;
  int    n;

  if (buf == NULL || len == 0)
    {
      return;
    }

  buf[0] = '\0';

  /* 值全部现读，顺序随便（diag.c 是按 key 找的）。run / audio 放最前面是因为
   * 它们决定"hello_app 在不在"—— 万一将来字段多到快照被截断，这两个也必须
   * 还在（后面的字段被截掉只会让那几个字段报 -1）。
   *
   * 每个量的出处：
   *   idle / lres / wait / empty —— ai_audio.c 的四个录音观测（那边只由录音
   *     线程写）；idle 是"距最近一次读到数据多久"，wait 是"此刻等设备等了多久"，
   *     两个一起看才分得清"没在等"（-1，正常）和"一直在等"（数据流可能死了）；
   *   want —— "我要常听"的意图（和 start 不是一回事：开机第一次开麦失败时
   *     want 真而 start 假；★ 让路删掉之后，want=1 配 ract=0 就是异常，
   *     监听守护会把它当"会话已死"救回来）；
   *   nrt —— 网络回传通道补连尝试过多少次（0 = 开机就连上了，没有自愈动作；
   *     配合 net 一起看：net=0 而 nrt 在涨 = 一直在补但从没连上）；
   *   cap / sp —— 语音段累积（"到底有没有听到人说话"的直接证据）；
   *   kws —— 唤醒词模板条数（0 = 没装模板，唤醒词功能等于没开）；
   *   rxi / rxh / rxr / rxt / rxe / rxl —— 2026-09-16 加的驱动侧录音分诊计数
   *     （sf32lb52_audio_rx_stats()，口径见 sf32lb52_audio_in.h 那个结构体）：
   *     完成中断 / 半满中断 / read 进入次数 / 等待超时次数 / DMA 传输错误(TE)
   *     次数 / 被 DMA 覆盖掉的帧数。加它们的原因就是上面 idle/wait 那一句的
   *     反面：录音线程"卡在 read 里、录音标志却全健康"时，上层看得见的
   *     idle/wait 分不出是"卡在无界等待"还是"DMA 交帧那一半丢了"，这六个数
   *     才是那条分界线。它们**只报数、不触发任何恢复动作**。 */

  n = snprintf(buf, len,
               "run=%d audio=%d start=%d rec=%d ract=%d died=%d exit=%d "
               "idle=%d lres=%d wait=%d empty=%u want=%d "
               "net=%d nrt=%u cap=%d sp=%u kws=%d sm=%s lbeat=%d",
               g_running ? 1 : 0,
               g_audio_ctx.initialized ? 1 : 0,
               g_audio_started ? 1 : 0,
               g_audio_ctx.recording ? 1 : 0,
               audio_record_is_active(&g_audio_ctx) ? 1 : 0,
               g_audio_ctx.record_died ? 1 : 0,
               g_audio_ctx.record_exited ? 1 : 0,
               audio_record_idle_ms(&g_audio_ctx),
               audio_record_last_result(&g_audio_ctx),
               audio_record_wait_ms(&g_audio_ctx),
               (unsigned)audio_record_empty_reads(&g_audio_ctx),
               g_listen_wanted ? 1 : 0,
               g_net_started ? 1 : 0,
               (unsigned)g_net_retry_count,
               g_speech_capturing ? 1 : 0,
               (unsigned)g_speech_frames,
               g_kws_ready,
               sm_get_state_name(sm_get_state(&g_sm_ctx)),
               ai_companion_beat_age_ms());

  /* 驱动侧那六个分诊计数接在最后（见上面字段表里 rxi..rxl 那一条）。
   *
   * ★ 为什么是"接在后面"而不是插在中间：diag.c 交上来的缓冲是 384 字节的
   *   DIAG_SNAP_BUF，最坏情况（每个数字都到 int/uint 的上限、状态名取最长的
   *   CARE_REMIND）这一行大概 320 字节，装得下；但**万一**将来字段再多到装
   *   不下，被切掉的必须是这几个锦上添花的诊断计数，前面那些回答"它在不在、
   *   在不在听"的字段（run/audio/ract/idle/wait…）一个都不能少。
   *
   * ★ 为什么取不到时**一个键都不写**：驱动还没初始化时 rx_stats 返回 -ENODEV。
   *   这里不写，diag.c 就按它那条"没写进来的键 = 取不到"的规矩输出 -1 ——
   *   这比给 %u 编一个哨兵值强得多（-1 在 %u 下是 4294967295，看报表的人只会
   *   当成真计数）。
   *
   * ★ 这一条也是本函数唯一一处**直接问驱动**的读数。它是只读的（驱动那边
   *   不加锁、不碰设备、不 post 信号量），所以前面那段"任何线程可调、不阻塞"
   *   的纪律仍然成立。 */

  if (n > 0 && (size_t)n < len && sf32lb52_audio_rx_stats(&rxst) == OK)
    {
      (void)snprintf(buf + n, len - (size_t)n,
                     " rxi=%u rxh=%u rxr=%u rxt=%u rxe=%u rxl=%u",
                     (unsigned)rxst.irq, (unsigned)rxst.half,
                     (unsigned)rxst.read, (unsigned)rxst.timeout,
                     (unsigned)rxst.dma_err, (unsigned)rxst.lost);

      /* snprintf 一定收尾，所以到这里 buf 仍是一行完整的 C 字符串；
       * 长度重新量一次就够（真装不下时它自己停在 len-1 那一格）。 */

      n = (int)strlen(buf);
    }

  if (n < 0 || (size_t)n >= len)
    {
      /* 截断了（字段变多到那一行装不下 384 字节才会碰上）。snprintf 已经把
       * 放得下的部分收好尾了，这里只是再保证一次结尾有 '\0'；调用方按
       * "后半截的键没写进来"处理，也就是那几个字段报 -1。 */

      buf[len - 1] = '\0';
    }
}

/****************************************************************************
 * 跨 app「提交」请求（薄壳在 ai_companion_req.c）
 *
 * robot_ui 语音聊天**镜像面板**底部那个按钮（用户原话：「下面是关闭按钮，我希望
 * 换成提交按钮」）点一下 = "我说完了，立刻把这一段送去识别"，不等 VAD 那 3 秒
 * 静音超时。接口的契约、纪律、为什么非阻塞都写在 ai_companion_req.h 的声明处；
 * 这里只补实现上的取舍：
 *
 * ★ 为什么本函数可以在这里**读** hello_app 的状态就下结论：
 *   它只做"看一眼 + 登记"两件事，一个设备都不碰，所以任何线程（现在是 robot_ui
 *   的 LVGL 线程）都可以调。真正的收尾动作交给录音线程（见 audio_data_callback），
 *   这里读到的快照**只用来**决定"要不要登记请求"和"怎么回界面" —— 即使快照过了
 *   一两拍就不准了，收尾那边也有自己的判据（g_speech_capturing）兜着，
 *   最坏就是"这次提交没提交（人刚好自己停了）"，绝不会把同一段音频送两次。
 *
 * ★ 为什么"没有可提交的语音"要分 NONE 和 BUSY 两种回答：
 *   界面拿它决定要不要说"没听到你说什么"。NONE（真的还没听到人说话）才该提示；
 *   BUSY（我正把上一句送去识别 / 正在回答）时提示就是误导 —— 面板上本来就写着
 *   "正在想…/正在说话…"，再插一句"没听到"只会让人以为它坏了。
 ****************************************************************************/

int ai_companion_voice_submit(void)
{
  /* 它没在跑 / 没在常听（还没起来，或者音频初始化 / 第一次开麦就失败了）：
   * 一律报"没听到"，**并且不登记请求** —— 一次性请求不能留在那里等以后有人
   * 认领（下面 audio_data_callback 的说明写了那会变成"拦腰截断下一句话"）。 */

  if (!g_running || !g_audio_ctx.initialized || !g_audio_started)
    {
      printf("[语音] 「提交」：现在没在常听（没起来 / 开机第一次开麦没成），"
             "不登记请求\n");
      return AI_COMPANION_SUBMIT_NONE;
    }

  /* 正在累积一段语音，而且够长（和 process_ai_dialogue 同一个门槛）：
   * 登记请求，剩下的交给录音线程下一帧做。 */

  if (g_speech_capturing && g_speech_buf != NULL &&
      g_speech_frames >= SPEECH_MIN_FRAMES_FOR_ASR)
    {
      printf("[语音] 「提交」：已登记，立刻收尾这一段(累积 %zu 帧)\n",
             g_speech_frames);
      ai_companion_voice_submit_request();
      return AI_COMPANION_SUBMIT_ACCEPTED;
    }

  /* 正忙：送 ASR / 等大模型 / 出声 / 追问流程在跑（它有自己的听说节奏）。
   * 这一刻插一脚没有意义，界面也不该提示"没听到"。
   * 注意这条必须排在"正在累积"后面：追问流程限时听回答期间也可能正在累积，
   * 那种情况要按"可以提交"处理（老人答完点一下「提交」正好提前收尾）。 */

  if (sm_get_state(&g_sm_ctx) == SM_STATE_AI_TALKING ||
      audio_is_playing(&g_audio_ctx) ||
      g_ask_phase != ASK_PHASE_IDLE)
    {
      printf("[语音] 「提交」：正忙（AI_TALKING / 出声 / 追问），暂不处理\n");
      return AI_COMPANION_SUBMIT_BUSY;
    }

  /* 剩下这一种就是"没听到人说话"：没在累积、也没在处理任何一轮。
   * 界面据此提示一句，这里一个请求都不登记。 */

  printf("[语音] 「提交」：还没听到人说话，不登记请求\n");
  return AI_COMPANION_SUBMIT_NONE;
}

/****************************************************************************
 * 常听监听守护（自愈）
 *
 * start_audio_listening() 原来只在开机时调一次，录音线程一旦因为驱动
 * AUDIOIOC_STOP 或下层 5 秒 DMA 超时（audio_in_read 返回 0 / -110）自己
 * 跳出循环，应用就永久变聋：还在跑、界面还在，但再也听不到声音，只能重启板子。
 * 下面这段在主循环里每 100ms 看一眼"录音是不是还活着"，不活就按退避重启。
 ****************************************************************************/

/* 第一次重试前的等待：给"线程自己收尾 / 驱动把设备还回来"留时间。
 * 立刻重开的话 audio_record_start() 会撞上还没退出的旧线程（返回 -EBUSY）。 */

#define LISTEN_SUPERVISE_RETRY_MS   1800

/* 连续失败时的退避上限：设备真坏了就别 1.8 秒一次地去敲它 */

#define LISTEN_SUPERVISE_MAX_MS    10000

/* "忙"的兜底上限（毫秒）：录音已经断了这么久、而那几个"忙"标志还挂着，
 * 就不再认"忙"，强行把麦克风抢回来（见 listen_supervise_tick 里那一段）。
 *
 * 取 45 秒，两个方向都要交代：
 *   - 不能比"正常的忙"短：正常流程里唯一会让录音停下来的事是 TTS 出声
 *     （半双工让路，见 ai_audio.c 的 audio_prepare_output），而单次出声的上限
 *     就是播放缓冲 AUDIO_PLAY_BUFFER_MS = 30 秒；45 秒 = 30 秒 + 15 秒余量，
 *     正常出声绝不会被它掐掉。状态机 AI_TALKING 自己的超时也是 30 秒，
 *     45 秒是它的 1.5 倍，够一次正常回复先跑完。
 *   - 又不能太长：真正会挂住的是**标志本身卡死**（播放线程写不动、追问相位
 *     没收尾、大模型回复永远不来），它们没有任何上限，全靠这个数兜底。
 *     45 秒是"老人还能忍"和"别误伤正常流程"之间的取舍：喊了半天没反应之后，
 *     45 秒内一定能把耳朵抢回来。
 * 注意它计的是"**失去麦克风**的时长"，不是"推理时长"：上面那句
 * audio_record_is_active() 一命中就清零，正常推理期间根本不会累加 ——
 * 这也正是它敢取这么温和的值的原因。 */

#define LISTEN_SUPERVISE_FORCE_MS  45000

/* 【2026-09-19 新加】"会话活着、但录音线程已经卡在设备调用里"的判据（毫秒）。
 *
 * 为什么需要它：真机上"录音跑一阵就永久停摆"的那种形状，**录音标志全健康**
 *   （rec=1 / ract=1 / died=0 / exit=0），下面那条 audio_record_is_active() 早退
 *   永远命中，守护一眼都看不到它。唯一看得见它的是录音线程自己写下的那个量：
 *   diag 里的 wait（= 这一次 audio_in_read() 已经等了多久）一路涨到 117986 ms，
 *   而驱动那一次等满本来只有 5 秒硬上界（SF32LB52_AUDIO_RX_READ_TIMEOUT_MS）、
 *   串口 dump 的栈也证实它就卡在 sf32lb52_audio_read() 里的那次等待上
 *   （`nxsem_clockwait_slow ← sf32lb52_audio_read+0x587`，
 *   原文见 _flash/gate_status.txt 的 sched_dumpstack [28]）：
 *   内核那记定时等待的超时被 DMA 中断抢掉之后就**再也不会回来**，
 *   而且 rxt（等待超时次数）恒 0 —— 驱动里那套按超时触发的分级恢复一次都没跑。
 *
 * 取值 8 秒 = 那个硬上界 × 1.6，两边都交代：
 *   - 正常一帧 20 ms；驱动最坏等满一次 5 秒。8 秒在正常路径上**不可能**出现
 *     （连"连续 5 次超时"那种粘住形态也不会：那种情况下驱动会正常返回 -110、
 *     wait 每次都在 5 秒内归零，而 empty/lres 会如实报出来）；
 *   - 又不能取太接近 5 秒：5 秒到点那一下、加上读回来的收尾和调度抖动，
 *     要留足余量，免得把"刚好赶在超时边上"的正常帧误判成卡死。
 *
 * ★ 判据只用这一个量，**不带任何由别的模块维护的豁免条件**（这是 2026-09-15
 *   那次教训的直接产物：当时那条"数据流看着像死了"的判据被自己的豁免项
 *   ——状态机不是 AI_TALKING——永久关掉了，反而造出它本来要防的"永久聋"）。
 *   合法地"不在读"的形状（放音时的半双工停录 / 手动停麦）这里天然安全：
 *   那时会话已经被停掉，audio_record_is_active() 是假，判据根本不成立。 */

#define LISTEN_SUPERVISE_RX_STALL_MS  8000

/* ★【2026-09-21 新加】"要常听"（want）立着却一直没在录音（ract=0），超过它
 * 就判定"会话已死"，**先 stop 掉那半截会话再按正常路径重开**。
 *
 * 为什么需要它（这就是那两条守护都盖不到的死角）：真机快照原文
 *     ract:0  want:1  wait:490645  rxr 冻住  sm:IDLE
 * 录音链路根本没在录，而且一挂就是几百秒。让路协议还在的时候，这种形状被当成
 * "合法的让路中"（设备借给 robot_ui 了，g_mic_hold_active 挡着本函数整个不跑），
 * 而"卡在设备读里"那条判据又要求 audio_record_is_active() 为真（ract=1），
 * 两条合起来正好漏掉 ract=0 —— 只能重启板子。
 * 让路删掉之后 `want==1 && ract==0` 就**不可能**再合法：只有 hello_app 自己会让
 * 录音停下来，而"我们自己在出声"那一种有上面那条 audio_is_playing 单独挡着。
 *
 * 为什么是 stop + 重开（不是只重开）：这种形状十有八九是"旧会话的录音线程没退
 * 干净、还占着设备"，直接 audio_record_start() 只会一直 -EBUSY（现场日志：
 * `AUDIO_IN: 录音设备仍被线程 N 占用` + `启动录音失败: -16`），退避重试永远失败。
 * 那一步以前只在 stalled 那条路上做，而 stalled 要求 ract=1，恰恰盖不到 ract=0。
 *
 * 为什么取 5 秒：
 *   - 正常一帧 20 ms，5 秒是它的 250 倍；"想听却没在录"能持续 5 秒以上的只有
 *     三种来源，都该被救：① 旧会话没退干净 / audio_record_start 被 -EBUSY 挡着
 *     （这种**没有任何自愈**）；② 录音线程被驱动停掉后异常退出；③ 刚开机第一次
 *     开麦失败。
 *   - 下限不能更短（1~2 秒）：开机第一次开麦失败时 want 立刻为真，而守护自己的
 *     退避（LISTEN_SUPERVISE_RETRY_MS = 1.8 秒起）正要开始试，判太早会把那条
 *     正常路径也一起当"卡死"，白多做一次没意义的重开、白多打一行日志。
 *   - 上限不用更长（>10 秒）：退避路径自己的间隔上限就是 10 秒，取到那个量级
 *     等于落回退避本身，覆盖不到"重开一直失败"的死结。5 秒比它短，正好用来
 *     给这道死结下"必须先 stop"的硬判据。
 *   - 误伤的代价可控：这一拍只多一次 audio_record_stop()（对已经不在录的会话是
 *     近似空操作）+ 一次 start_audio_listening()，本来就要做。 */

#define LISTEN_SUPERVISE_WANT_MUTE_MS  5000

/* 失败日志节流：前 5 次每次都打（开头几次最需要看见），之后每 10 次一条 */

#define LISTEN_SUPERVISE_LOG_FIRST      5
#define LISTEN_SUPERVISE_LOG_EVERY     10

static bool     g_listen_retry_armed;      /* 已经排好一次重试，等着到点 */
static uint32_t g_listen_retry_at;         /* 下次重试的时间点 (main_now_ms) */
static uint32_t g_listen_fail_count;       /* 连续失败次数（退避 + 日志节流） */
static uint32_t g_listen_backoff_ms = LISTEN_SUPERVISE_RETRY_MS;
static uint32_t g_listen_dead_since;       /* 从哪一刻起听不见了，0 = 还没断 */
static uint32_t g_listen_mute_since;       /* 从哪一刻起"要常听却没在录"，0 = 没这情况 */
static bool     g_listen_mute_logged;      /* 这一次断开已经报过一行了（别 100ms 刷一行） */
static uint32_t g_listen_last_try_ms;      /* 上一次真去重开录音的那一拍（失败也算；
                                            * 它是防抖与防刷屏的锚点，见下面那段） */

/**
 * @brief  监听守护：录音不活跃就按退避把它重新拉起来
 *
 * 只在"应该常听"的时候动手。本板录放是半双工（AUDIOIOC_STOP 会把两条通路
 * 一起停），下面任何一条成立时去开麦都是跟正在跑的那条流程抢设备：
 *   - 正在收尾：g_running / g_listen_wanted（stop_audio_listening 刚关了麦）；
 *   - 扬声器在响：TTS 出声期间 audio_play_start() 特意先把录音停掉让路，
 *     播完由播放线程自己按原配置恢复，这里插一脚必然互相打断；
 *   - 状态机在 AI_TALKING：送 ASR / 等大模型 / TTS 合成这一整段；
 *   - 追问流程在跑（说 / 限时听）：它有自己的听说节奏，只听它自己的窗口。
 *
 * 注意 g_speech_capturing 不能当"忙"来跳过重试：录音线程完全可能在 VAD
 * 报过"人开始说话"之后才死，这个标志就永远挂在 true 上，拿它挡重试等于
 * 把"永久变聋"原样搬回来。所以这里改成重开时顺手清掉它和累积长度。
 *
 * ★ 本函数存在的意义就在"忙也得有上限"这一条（LISTEN_SUPERVISE_FORCE_MS）：
 *   上面那几个"忙"判据都是**别人维护的标志**，任何一个卡住不回（播放线程写不动、
 *   追问相位没收尾、大模型回复永远不来），录音断了也没人来救 —— "忙 + 会话已死"
 *   叠加起来就是用户口中的"卡死了、说话没反应"。所以录音断了够久之后，
 *   连"忙"都不认，强行重开。另外还有一条更快的路：录音线程自己报的
 *   ctx->record_died（异常中断）不等任何时间 —— 只要是"下一拍"就能重开
 *   （只留一道"两次重开至少隔 1.8 秒"的防抖，防止设备一起来就死时高频开关设备）。
 *
 * 判据原来只有两条（活着 / 死了），不做任何"数据流看着像死了"的推断：录音线程
 * 卡在 read 里、标志却全健康的那种形状，这里是**看不见**的 —— 那正是不该在
 * 应用层拿一堆时间戳去猜的事（2026-09-15 试过，判据本身会把恢复永久挡死）。
 *
 * ★ 2026-09-19 补上第三条：**"活着但卡死在设备读里"**（真机上就是这么聋的 ——
 *   ract=1 / died=0，而 diag 的 wait 涨到 117986 ms、驱动 read 计数一动不动、
 *   等待超时计数恒 0）。判据只用录音线程自己写下的那一个量（这一次 read 等了
 *   多久 ≥ LISTEN_SUPERVISE_RX_STALL_MS），**不带任何由别的模块维护的豁免条件**
 *   —— 2026-09-15 那次之所以"判据把恢复永久挡死"，正是因为它的豁免项
 *   （状态机不是 AI_TALKING）是个无上界的布尔量。这里的判据没有任何豁免项，
 *   合法地"不在读"的形状（放音时的半双工停录 / 手动停麦）天然不满足
 *   audio_record_is_active()，所以不会被它挡住，也不会误伤。
 *   命中之后走的是既有的紧急重开（同一套 1.8 秒防抖 + 退避），只多一步"先停
 *   掉那一次卡住的会话"。 */

static void listen_supervise_tick(sm_context_t *ctx)
{
  uint32_t now;
  bool     died;
  bool     stale;
  bool     urgent;
  bool     stalled;
  bool     want_mute;
  int      rx_wait;
  int ret;

  if (!g_running || !g_listen_wanted)
    {
      /* 正在收尾 / 本来就没想常听：不动作。
       * ★ 这里原来还挡着两个让路标志（g_mic_hold_active / g_mic_device_busy）——
       *   让路协议 2026-09-21 整套删除（见 ai_companion_req.h）。删掉它们**不是**
       *   放松判据，恰恰相反：原来"want 真、录音却不在"的形状正好被那两个标志挡在
       *   门外，那就是现场那个"永久聋"的死角。 */

      g_listen_dead_since = 0;
      g_listen_mute_since = 0;
      g_listen_mute_logged = false;
      return;
    }

  /* ---- 【2026-09-19 新加】"会话活着、设备读却再也不回来"那种永久聋 -------
   *
   * 真机形状（本轮定案）：录音标志全健康，但录音线程卡在 sf32lb52_audio_read()
   * 里那次等待上再也出不来（内核定时等待的超时被 DMA 中断抢掉，之后这一次等待
   * 彻底没有上界）。唯一看得见它的量是录音线程自己写下的 record_read_start_ms
   * 的年龄 —— 就是 diag 里那个 wait。判据和理由（为什么 8 秒、为什么不带任何
   * 别人维护的豁免项）写在 LISTEN_SUPERVISE_RX_STALL_MS 头顶那一段。
   *
   * 会话真被停掉/正在停（record_stop 置位）时 read_start_ms 也会被下面那条
   * 早退挡掉：audio_record_is_active() 为假，判据不成立。 */

  rx_wait = audio_record_wait_ms(&g_audio_ctx);
  stalled = audio_record_is_active(&g_audio_ctx) &&
            rx_wait >= (int)LISTEN_SUPERVISE_RX_STALL_MS;

  if (!stalled && audio_record_is_active(&g_audio_ctx))
    {
      /* 录音还活着（设备已 START 且线程没退出）：把重试状态复位 */

      g_listen_retry_armed = false;
      g_listen_fail_count = 0;
      g_listen_backoff_ms = LISTEN_SUPERVISE_RETRY_MS;
      g_listen_dead_since = 0;
      g_listen_mute_since = 0;
      g_listen_mute_logged = false;      /* 录音回来了，下一次断开可以重新报一行 */
      g_audio_ctx.record_died = false;   /* 活着就说明上一代的死亡标记没意义了 */
      return;
    }

  now = main_now_ms();

  /* 【2026-09-21 新加】"want 立着却一直没在录"的计时（判据和取值理由见
   * LISTEN_SUPERVISE_WANT_MUTE_MS 头顶那一段）。能走到这里就说明
   * audio_record_is_active() 是假（真活跃且没卡住的话上面那条早退已经返回了），
   * 所以这个计时器量的正好是"要常听、录音却不在"持续了多久。
   * 录音一回来就在上面那条早退里清零，所以它不会累加两次独立的断开。 */

  if (g_listen_mute_since == 0)
    {
      g_listen_mute_since = (now != 0) ? now : 1;
    }

  want_mute = ((int32_t)(now - g_listen_mute_since) >=
               (int32_t)LISTEN_SUPERVISE_WANT_MUTE_MS);

  /* 记下"从哪一刻起听不见了"：只在第一次发现不活跃时置位。
   * 0 是保留值（表示还没断），真拿到 0 就退一格用 1，代价是那一轮少算 1ms。 */

  if (g_listen_dead_since == 0)
    {
      g_listen_dead_since = (now != 0) ? now : 1;
    }

  /* 断了多久了（用差值比较，避开 49 天一次的 uint32 回绕）。 */

  stale = ((int32_t)(now - g_listen_dead_since) >=
           (int32_t)LISTEN_SUPERVISE_FORCE_MS);

  /* 扬声器在响：这次先不抢设备。半双工上把麦抢回来就会掐掉正在放的那一段，
   * 而播放本身有上限（≤ AUDIO_PLAY_BUFFER_MS = 30 秒），播完播放线程还会按原
   * 配置把录音恢复起来 —— 也就是说这一条通常自愈。
   * 只有"长期不活跃"的兜底（stale）才越过它：那种情况下"还在放"本身就说明
   * 播放标志也卡住了（正常出声到不了 45 秒），不抢就永远聋着。 */

  if (audio_is_playing(&g_audio_ctx) && !stale)
    {
      g_listen_retry_armed = false;
      return;
    }

  /* 录音线程自己报的"异常中断"（read 返回 0 / 负值，但不是 audio_record_stop()
   * 让的 —— 这两种收尾在 ai_audio.c 录音线程末尾分开记）。认领一次就清掉，
   * 因为这一代录音会话不会再回来了：不用等状态机那 10 / 30 秒超时，也不用等
   * 守护的 1.8 秒排程（只剩下面 urgent 那道防抖下限）。
   * （它放行的"忙"只有 AI_TALKING / 追问这两种：那两件事本来就不占麦克风，
   *   现在把麦重开不会打断它们。） */

  died = g_audio_ctx.record_died;
  if (died)
    {
      g_audio_ctx.record_died = false;
      printf("[监听守护] 录音线程异常中断（不是我们要停的那种），会话已死，"
             "不等状态机那 10/30 秒超时\n");
    }

  /* 【2026-09-19 新加】卡在设备读里的会话，对守护来说同样是"会话已死"（它再也
   * 不会给数据），但**不能**直接复用下面那条"直接开麦"的路：那一次会话还占着
   * 设备（recording 还是 true），audio_record_start() 见到它会立刻 -EBUSY
   * （现场日志就是 `AUDIO_IN: 录音设备仍被线程 N 占用` + `启动录音失败: -16`，
   * 于是退避重试永远失败、麦克风永远回不来）。所以这里只置 died（拿到"不等
   * 状态机超时"的紧急重开资格），真正停设备那一下放在重开的动作之前。 */

  if (stalled)
    {
      printf("[监听守护] 录音线程卡在设备读里 %d ms（驱动一次等满只有 5 秒上界，"
             "说明那次等待的超时丢了）：判定这一次会话已死，停掉再重开\n",
             rx_wait);
      died = true;
    }

  /* 【2026-09-21 新加】"要常听"立着却一直没在录：同样是"会话已死"。给它 died
   * 的资格（拿到"不等状态机那 10/30 秒超时"的紧急重开），而下面重开之前会先把
   * 那半截会话 stop 掉 —— 那一步才是把 -EBUSY 死结解开的地方。 */

  if (want_mute)
    {
      /* 日志只打一次：这个条件一旦成立就会一直成立到录音回来，每 100ms 刷一行
       * 会把串口淹掉（本文件在别处已经为刷屏吃过亏）。重开失败那一路有它自己的
       * 失败日志节流，不靠这一行。 */
      if (!g_listen_mute_logged)
        {
          g_listen_mute_logged = true;
          printf("[监听守护] \"要常听\"立着 %u ms 却一直没在录（旧会话没退干净 / "
                 "audio_record_start 一直被 -EBUSY 挡着）：判定会话已死，"
                 "先停掉那半截会话再重开\n",
                 (unsigned)(now - g_listen_mute_since));
        }

      died = true;
    }

  /* 正忙：不排重试，也不动退避（退避要按"设备到底能不能起来"算）。
   * 等流程走完，下一轮从"发现不活跃"重新计时，那个 1.8 秒的余量还在。 */

  if (!died && !stale &&
      (g_ask_phase != ASK_PHASE_IDLE ||
       sm_get_state(ctx) == SM_STATE_AI_TALKING))
    {
      g_listen_retry_armed = false;
      return;
    }

  /* 立即重开的条件：会话已死（异常中断 / 长期不活跃），**而且**距上一次重开尝试
   * 已经过了正常的重试间隔。后半句是防抖：设备真坏的时候录音线程会"一起来就死"，
   * 不压这一下就成了每秒十次开关设备 + 十行日志（退避那套本来就是拦这个的）。 */

  urgent = (died || stale) &&
           ((int32_t)(now - g_listen_last_try_ms) >=
            (int32_t)LISTEN_SUPERVISE_RETRY_MS);

  /* 排一次重试再等到点 */

  if (!urgent)
    {
      if (!g_listen_retry_armed)
        {
          g_listen_retry_armed = true;
          g_listen_retry_at = now + g_listen_backoff_ms;
          printf("[监听守护] 录音不活跃，%u ms 后尝试重启语音监听\n",
                 (unsigned)g_listen_backoff_ms);
          return;
        }

      if ((int32_t)(now - g_listen_retry_at) < 0)
        {
          return;
        }

      /* 到点了：动手之前先把下一拍的排程推后一个退避。不推的话，下面那些
       * "这一拍先不重开"的 return 会让每一拍都落到这里（排程点已经过、又没被
       * 消费），串口就被同一条日志以 100ms 的节奏刷屏 —— 现场被刷过 196 秒。
       * 推后之后，无论这一拍成没成、有没有被守卫挡回来，日志最多每个退避间隔
       * 一条。 */

      g_listen_retry_at = now + g_listen_backoff_ms;
    }

  /* 从这里往下就是真的重开了。日志放在这里而不是判定处：判定是每一拍都跑的，
   * 放前面会刷屏。 */

  /* 时间戳在动设备**之前**落下：它表示"这一拍已经试过了"，下面每一条 return
   * （抢不到设备锁、守卫说现在不能开麦）都算这一拍试过了。它是本函数唯一的
   * 防抖 + 防刷屏锚点，任何一条 return 之前它都必须已经写过 —— 漏写一次就是
   * 那条日志以 100ms 的节奏刷屏。 */

  g_listen_last_try_ms = now;

  if (stale)
    {
      printf("[监听守护] 录音已断 %u ms 却一直\"忙\"（播放/追问/AITalking 卡住），"
             "判定会话已死，强制重开语音监听\n",
             (unsigned)(now - g_listen_dead_since));
    }

  /* 能走到这里，说明"录音断了、扬声器没响、状态机却在 AI_TALKING"——
   * 这就是"会话已死"最典型的样子（正常 AI_TALKING 期间录音是活的，唯一会停它的
   * 是 TTS 出声，而那种情况上面播放那条判据已经挡掉了）。所以顺手把状态机推回
   * 可听状态：AI_TALKING 对 VAD 报的"语音开始 / 结束"（SM_EVENT_WAKEUP /
   * SM_EVENT_VOICE_COMPLETE）是**全部忽略**的（见 ai_state_machine.c 的
   * sm_ai_talking_transition 默认分支），只重开麦、不推状态机的话，用户下一句话
   * 照样进不来，要一直等到 AI_TALKING 自己那 30 秒超时。
   * 推的目标态（IDLE）就是它自己超时那条路的目标态，只是不等那 30 秒。
   * 别的状态一律不动：IDLE / LISTENING / CARE_REMIND / ALARM 都会正常收 VAD 事件，
   * 尤其 CARE_REMIND / ALARM 有自己的收尾节奏，不能被我们拽走。
   * 迟到的回复不会因此丢：播放和界面反馈都在 llm_complete_callback 里，
   * 不依赖状态机停在哪个状态。 */

  if (sm_get_state(ctx) == SM_STATE_AI_TALKING)
    {
      printf("[监听守护] 状态机还卡在 AI_TALKING，推回 IDLE"
             "（否则新听到的语音会被它忽略）\n");
      sm_force_state(ctx, SM_STATE_IDLE);
    }

  /* 真要动设备了，先把"这一刻还该不该开麦"复查一遍：上面那段之前的状态
   * 可能已经变过（函数里几次早退都在同一拍里跑完）。
   *
   * ★ 这里原来还有抢"设备动作权"那把 g_mic_device_lock（和让路线程互斥用）——
   *   让路协议 2026-09-21 删除之后，hello_app 里还会动录音设备的只剩**主循环
   *   这一条线程**（本函数、起播、录音链路重开都在它上面），本来就不存在并发。
   *
   * stalled / want_mute 的那一次会话：它当然可能"活着"（audio_record_is_active
   * 为真），但我们这一拍就是要把它收掉的，所以不拿它当"设备被别人占着"看。 */

  if (!g_listen_wanted ||
      (audio_record_is_active(&g_audio_ctx) && !stalled && !want_mute))
    {
      return;
    }

  /* "长期不活跃"的判定到此用掉，清掉计时：下面万一重开失败，走的是正常退避
   * （1.8s 起翻倍），不会每一拍都来敲一次设备。 */

  g_listen_dead_since = 0;

  /* 上面判定的"要收掉那一次会话"的两条路（stalled 卡在设备读里 / want_mute
   * "要常听"却一直没在录）：那一次会话很可能还占着设备，直接开麦必然 -EBUSY
   *（现场日志的证据：`AUDIO_IN: 录音设备仍被线程 N 占用`），所以先把它停掉。
   *
   * 这一次 stop 为什么是确定会返回的：audio_in_stop() 里的 AUDIOIOC_STOP 会走到
   * hw_stop()，而 hw_stop() **无条件** post 一次 rx_sem 并作废会话代号；那一次
   * read 自己还有 5 秒硬上界（本轮把它做成私有看门狗了，见驱动的 rx_wait_wdog），
   * 所以随后的 join 有界 —— 卡住的那条线程一定出得来。
   * 只调 audio_record_stop()（不是 stop_audio_listening()）：后者会把
   * g_listen_wanted 收掉，而本函数就是在"要常听"的前提下跑的，收掉它这一拍
   * 后面所有守卫都会把自己挡回去（也让下次守护重开前要等更久）。
   * 对"本来就不在录"的会话（want_mute 那条）这一次调用近似空操作，代价可忽略。 */

  if (stalled || want_mute)
    {
      printf("[监听守护] 先停掉那一次没退干净的录音会话（wait=%d ms）\n", rx_wait);

      audio_record_stop(&g_audio_ctx);
      audio_vad_disable(&g_audio_ctx);
      g_audio_started = false;
    }

  /* 重开之前清场：上一段会话可能死在"人正说话"中间，g_speech_capturing
   * 还挂着 true、g_speech_buf 里有半截累积语音，不清掉的话下一次进 ASR 的
   * 会是跨会话的脏数据。此刻录音线程已经不在（上面刚判过不活跃），不会有回调
   * 往里写。 */

  g_speech_capturing = false;
  g_speech_frames = 0;

  /* 唤醒词模块的流式状态同理：它可能正好停在"这句话说到一半"（预滚环、
   * 待判句子、自适应本底都是上一段音频的）。麦克风断了这么久，这些状态
   * 已经对不上了，重新开始听之前清一次（kws_dtw.h 的建议）。
   * 这里清是安全的：录音线程已经死了，没有第二个线程在调这个模块 ——
   * kws_feed 只允许一个线程调，那个线程不在，就不存在并发。
   * 代价是清完要 300ms（KWS_SETTLE_FRAMES）重新量本底，这期间不会起端点。
   * 只在功能开了的时候清：没模板时这条路径和以前一个字节都不差。 */

  if (g_kws_enabled)
    {
      kws_reset();
    }

  ret = start_audio_listening(ctx);

  if (ret == OK)
    {
      printf("[监听守护] 语音监听已恢复（之前连续失败 %u 次）\n",
             (unsigned)g_listen_fail_count);
      g_listen_retry_armed = false;
      g_listen_fail_count = 0;
      g_listen_backoff_ms = LISTEN_SUPERVISE_RETRY_MS;
      return;
    }

  /* 失败就退避：1.8s -> 3.6s -> 7.2s -> 10s（封顶）。
   * 日志按上面的节流打，别让串口被一条刷屏的日志淹掉。 */

  g_listen_fail_count++;
  g_listen_backoff_ms *= 2;
  if (g_listen_backoff_ms > LISTEN_SUPERVISE_MAX_MS)
    {
      g_listen_backoff_ms = LISTEN_SUPERVISE_MAX_MS;
    }

  g_listen_retry_at = main_now_ms() + g_listen_backoff_ms;

  if (g_listen_fail_count <= LISTEN_SUPERVISE_LOG_FIRST ||
      g_listen_fail_count % LISTEN_SUPERVISE_LOG_EVERY == 0)
    {
      printf("[监听守护] 重启语音监听失败 %u 次 (ret=%d)，%u ms 后再试\n",
             (unsigned)g_listen_fail_count, ret,
             (unsigned)g_listen_backoff_ms);
    }
}

/* ---- 串口侧的"录音卡死"观测（2026-09-16） ----
 *
 * 背景：录音线程会**永久**卡在 read() 里 —— 板内快照上 ract=1 / died=0 /
 * want=1 全是健康的，只有 idle / wait 一路涨到几十万毫秒。那种形状
 * listen_supervise_tick 是**看不见**的（录音"活着"，守护只看"活不活"），
 * 而 MQTT 那条 diag 通道要有人去问才有回执、串口线又经常不在手上。
 * 所以这里补一条**纯观测**的串口输出：不用去问谁，卡住的时候自己往外说。
 *
 * 判据（两条同时成立才打）：录音标志说"在录"（audio_record_is_active，
 * 也就是 ract=1 那一套），而 audio_record_idle_ms() 已经超过 10 秒 ——
 * 10 秒是正常情况的 500 倍（20ms 一帧），照理说不可能，
 * 所以"超了"本身就是异常，不需要再猜。
 *
 * 打什么：idle/wait 这两个上层量 + 驱动那六个分诊计数 + armed/busy。
 * 这一组正是分诊要的全部：
 *   read/timeout 在涨而 irq/half 不涨 → 请求线或 ADC 侧死了（DMA 起了没数据）；
 *   irq/half 还在涨                   → 数据在流，是等待/唤醒那一侧的问题；
 *   dma_err 在涨                      → TE，那一类 HAL 自己拆通道、不会自愈；
 *   lost 在涨                         → 上层自己来不及取；
 *   armed=1 且 busy=1 配上 irq/half 冻结 → "通路武装得好好的、就是不来数据"。
 *
 * ★ 本函数**只打印**：不唤醒、不停设备、不重开录音、不改任何标志。
 *   恢复动作是录音链路自己的事（见 listen_supervise_tick 那段"判据只有活着/
 *   死了"的说明：在应用层拿一堆计数去猜着恢复，2026-09-15 试过，判据本身会把
 *   恢复永久挡死）。这一步只负责让下次断流时串口上有一锤定音的证据。
 *
 * ★ 限频是硬要求：主循环 100ms 一拍，不限频就是每秒 10 行把串口刷满、
 *   把别的线索冲掉（本文件在别处已经为刷屏吃过亏）。所以**每 10 秒最多一行**，
 *   用下面那个"下次允许打的时间点"挡；条件不再成立时把它清 0，下次真卡住
 *   立刻就能看到第一行。 */

#define RXSTUCK_IDLE_MS        10000   /* idle 超过它就算卡住（正常 20ms 一帧） */
#define RXSTUCK_LOG_PERIOD_MS  10000   /* 卡住期间：每 10 秒最多一行 */

static uint32_t g_rxstuck_log_at;      /* 下一次允许打日志的时间点 (main_now_ms) */

/**
 * @brief  录音卡死的串口观测（纯打印，主循环每 100ms 调一次）
 */

static void rxstuck_watch_tick(void)
{
  struct sf32lb52_audio_rx_stats_s rxst;
  uint32_t now;
  int      idle;
  int      wait;
  int      ret;

  if (!g_running || !audio_record_is_active(&g_audio_ctx))
    {
      /* 没在录音（或正在收尾）：不是这条观测管的形状，把限频锚点清掉，
       * 下次真卡住时第一行能立刻出来。 */

      g_rxstuck_log_at = 0;
      return;
    }

  idle = audio_record_idle_ms(&g_audio_ctx);
  if (idle < RXSTUCK_IDLE_MS)
    {
      g_rxstuck_log_at = 0;
      return;
    }

  now = main_now_ms();
  if (g_rxstuck_log_at != 0 && (int32_t)(now - g_rxstuck_log_at) < 0)
    {
      return;
    }

  g_rxstuck_log_at = now + RXSTUCK_LOG_PERIOD_MS;

  wait = audio_record_wait_ms(&g_audio_ctx);

  /* 驱动没起来（-ENODEV）时那六个数是真的取不到：如实把这行收成"取不到"，
   * 绝不打一串 0 冒充满快照 —— 看日志的人会把 0 当成"计数就是 0"，
   * 那是另一个结论（而且是最容易把人带偏的那个）。 */

  ret = sf32lb52_audio_rx_stats(&rxst);

  if (ret == OK)
    {
      printf("[rxstuck] 录音标志说在录、但已 %d ms 没读到数据"
             "（wait=%d）："
             "irq=%u half=%u read=%u timeout=%u dma_err=%u lost=%u "
             "armed=%d busy=%d up=%us\n",
             idle, wait,
             (unsigned)rxst.irq, (unsigned)rxst.half,
             (unsigned)rxst.read, (unsigned)rxst.timeout,
             (unsigned)rxst.dma_err, (unsigned)rxst.lost,
             rxst.armed, rxst.busy,
             (unsigned)(TICK2MSEC(clock_systime_ticks()) / 1000u));
    }
  else
    {
      printf("[rxstuck] 录音标志说在录、但已 %d ms 没读到数据"
             "（wait=%d）：驱动计数取不到 (ret=%d)\n",
             idle, wait, ret);
    }
}

/**
 * @brief  异常声音检测回调
 *
 * 检测到异常声之后不再直接跳报警：先问一句「请问需要帮助吗？」，
 * 听老人的回答再决定要不要真报警（流程见下面的"异常声 -> 追问流程"一节）。
 *
 * ⚠️ 本函数跑在**检测线程**里，而且检测线程正拿着 ctx->buffer_lock
 * （见 ai_sound_detect.c 的 sound_detect_thread：sound_detect_process_window()
 * 是在锁里调的）。所以这里绝不能做 TTS/ASR 这种秒级的阻塞动作 ——
 * 那会把喂数据的录音线程一起卡住。这里只做三件事：过滤、防抖、记事件。
 */

static void sound_detect_callback(sound_type_t type, float confidence,
                                  void *user_data)
{
  uint32_t now = main_now_ms();

  (void)user_data;

  printf("[安全] 检测到异常声音: %s (置信度: %.2f)\n",
         sound_detect_get_type_name(type), confidence);

  /* 1. 别把喇叭自己放出来的声音当异常：
   *    放音期间 audio_play_start() 已经把麦克风停了（录音线程退出，没人再喂
   *    检测器），这里的判断是兜住"停之前已经灌进去的那几帧"。 */

  if (audio_is_playing(&g_audio_ctx))
    {
      printf("[安全] 正在放音，忽略这次检测\n");
      return;
    }

  /* 2. 已经有一轮追问在跑：不叠加，也不排队 */

  if (g_ask_phase != ASK_PHASE_IDLE)
    {
      printf("[安全] 追问流程进行中，忽略这次检测\n");
      return;
    }

  /* 3. 防抖：同一阵响声（一秒的检测窗口会连着报好几次）只触发一次 */

  if (g_last_abnormal_ms != 0 &&
      now - g_last_abnormal_ms < ASK_DEBOUNCE_MS)
    {
      printf("[安全] 距上次触发 %u ms，防抖忽略\n",
             (unsigned)(now - g_last_abnormal_ms));
      return;
    }

  g_last_abnormal_ms = now;
  g_ask_pending_type = (int)type;
  g_ask_pending_conf = confidence;
  g_ask_round = 0;
  g_ask_answer_ready = false;
  g_ask_play_done = false;
  g_ask_answer[0] = '\0';
  g_ask_phase = ASK_PHASE_PENDING;

  printf("[追问] 已排入追问流程 (类型=%s, 置信度=%.2f)\n",
         sound_detect_get_type_name(type), confidence);

  /* 4. 弹「是否报警？」询问框（任务四）：
   *    回调里**不直接报警**，先请界面问一句。这个入口由 robot_ui 提供
   *    (ui_post_ask_alarm)，任何线程可调，它自己投到 LVGL 线程；还没落地时
   *    ai_sound_detect.c 里的弱实现只打一行串口（见 ai_sound_detect.h 末尾）。
   *    原来的「追问流程」保留：它问的是同一件事的另一半（语音确认），
   *    两者一个走界面、一个走喇叭，谁先落地都不影响另一条。 */

  {
    static char gate_reason[64];

    snprintf(gate_reason, sizeof(gate_reason), "异常声音: %s (%.2f)",
             sound_detect_get_type_name(type), (double)confidence);
    ui_post_ask_alarm(gate_reason);
  }
}

#ifdef CONFIG_HELLO_APP_SOUND_EVENT

/* 本地小模型（sound_event）命中回调的兜底阈值。
 * 模型自己已经做了 3/4 投票 + 每类阈值 + 同类 8s 不应期（sound_event.c 的
 * se_postprocess，阈值 0.60/0.70/0.70），这里只加一道很低的保底，
 * 防的是将来有人把模型阈值调松之后误报直接冒到界面上。 */
#define SOUND_EVENT_MIN_CONF 0.50f

/**
 * @brief  本地小模型（摔倒 / 敲击 / 尖叫）命中回调
 *
 * ⚠️ 本函数在 ai_sound_detect.c 的**门控线程**里同步执行（门控线程 →
 * sound_event_feed() → se_postprocess() → 这里），低优先级、还拿着门控那把锁。
 * 所以这里只做「过滤 + 防抖 + 投递」，**绝不碰 LVGL 控件**：要问用户一句
 * 一律走 ui_post_ask_alarm()，它自己投到 LVGL 线程（见 robot_ui/main.c）。
 *
 * 和 sound_detect_callback() 同一套口径：过滤 → 防抖 → 排入既有追问流程
 * （g_ask_phase），最后用 ui_post_ask_alarm() 弹「是否报警？」询问框。
 * 这里**不报警**：用户二次确认之后才真报警。
 */

static void sound_event_cb(int cls, float conf, void *arg)
{
  static uint32_t last_ms[SOUND_EVENT_SCREAM + 1];
  sound_type_t type;
  const char *what;
  char reason[64];
  uint32_t now;

  (void)arg;

  /* other 一律不上报；越界的类号也丢掉 */

  if (cls <= SOUND_EVENT_OTHER || cls > SOUND_EVENT_SCREAM)
    {
      return;
    }

  if (conf < SOUND_EVENT_MIN_CONF)
    {
      printf("[门控] 本地小模型 %s 置信度 %.2f 低于保底 %.2f，丢弃\n",
             sound_event_class_name(cls), (double)conf,
             (double)SOUND_EVENT_MIN_CONF);
      return;
    }

  /* 别把喇叭自己放出来的声音当异常：放音期间麦克风已经停了（见
   * audio_play_start），这里兜住"停之前已经灌进门控环形缓冲的那几帧"。 */

  if (audio_is_playing(&g_audio_ctx))
    {
      printf("[门控] 正在放音，忽略本地小模型这次命中（%s %.2f）\n",
             sound_event_class_name(cls), (double)conf);
      return;
    }

  /* 已经有一轮追问在跑：不叠加，也不排队 */

  if (g_ask_phase != ASK_PHASE_IDLE)
    {
      printf("[门控] 追问流程进行中，忽略本地小模型这次命中（%s %.2f）\n",
             sound_event_class_name(cls), (double)conf);
      return;
    }

  /* 防抖：同一类 10 秒内只上报一次。模型那 8s 不应期是按"类"记窗口号的，
   * 同一个声响本来就会连着出好几个窗，这里再压一道，顺便挡住"同一类里
   * 换了个声响"（例如连着敲两下）。 */

  now = main_now_ms();
  if (last_ms[cls] != 0 && now - last_ms[cls] < ASK_DEBOUNCE_MS)
    {
      printf("[门控] 同类（%s）距上次上报 %u ms，防抖忽略\n",
             sound_event_class_name(cls), (unsigned)(now - last_ms[cls]));
      return;
    }

  last_ms[cls] = now;

  switch (cls)
    {
      case SOUND_EVENT_FALL:
        what = "疑似跌倒撞击";
        type = SOUND_TYPE_FALL;
        break;

      case SOUND_EVENT_KNOCK:
        what = "疑似敲击求救";
        type = SOUND_TYPE_KNOCK;
        break;

      default:
        what = "疑似异常喊叫";
        type = SOUND_TYPE_SCREAM;
        break;
    }

  snprintf(reason, sizeof(reason), "%s %d%%", what,
           (int)(conf * 100.0f + 0.5f));

  printf("[门控] 本地小模型命中: %s（%s → %s, 置信度 %.2f）→ 先响铃\n",
         what, sound_event_class_name(cls),
         sound_detect_get_type_name(type), (double)conf);

  /* ★ 2026-09-20 晚（用户拍板："命中先响铃，用户点了不用了再停"）：
   * **先起铃** —— 板级报警模块的 EMERGENCY 级（高低交替警报音、每 5 秒重复一次），
   * 本地、立刻、**一个网络字节都不需要**（断网也照样响）。
   *
   * 停铃**不在这里**：由 robot_ui 的 ask_finish(confirmed == 0) 在
   * "用户点「不用了」/ 20 秒无人应答"那一刻做（见 robot_ui/main.c）。
   * 完整时序：命中起铃（这里）→ 红屏询问页弹出来 → 
   *   点「不用了」 ⇒ 停铃、不报警（这一下就是用户要的"再停"）；
   *   点「是的，报警」 ⇒ 走既有报警页那条收口，铃不停、连着一路响。
   * 起铃失败也不拦流程：询问页照旧会弹，只是这一次不响（那一行日志会说明原因）。 */

  (void)robot_ui_alarm_ring(reason);

  /* 复用既有的「异常声 → 追问流程」：和 sound_detect_callback() 一样只置标志，
   * 相位由 main_loop_task 的 ask_flow_tick() 推（那边是唯一的相位推进者）。
   * g_last_abnormal_ms 一起置，让检测器那条老路和这条共用同一个防抖基准。 */

  g_last_abnormal_ms = now;
  g_ask_pending_type = (int)type;
  g_ask_pending_conf = conf;
  g_ask_round = 0;
  g_ask_answer_ready = false;
  g_ask_play_done = false;
  g_ask_answer[0] = '\0';
  g_ask_phase = ASK_PHASE_PENDING;

  /* 「是否报警？」询问框（红屏）：任何线程可调，内部自己投到 LVGL 线程 */

  ui_post_ask_alarm(reason);
}

#endif /* CONFIG_HELLO_APP_SOUND_EVENT */

/**
 * @brief  初始化并启动声音检测
 */

static int start_sound_detection(sm_context_t *ctx)
{
  int ret;

  /* 配置声音检测 */

  sound_detect_config_t detect_cfg =
  {
    .mode = DETECT_MODE_REALTIME,
    .threshold = SOUND_DETECT_THRESHOLD_DEFAULT,
    .sample_rate = SOUND_DETECT_SAMPLE_RATE,
    .frame_ms = SOUND_DETECT_FRAME_MS,
    .enable_vad = true,
    .enable_feedback = true,
    .callback = sound_detect_callback,
    .user_data = ctx
  };

  ret = sound_detect_init(&g_sound_ctx, &detect_cfg);
  if (ret < 0)
    {
      printf("[错误] 声音检测初始化失败: %d\n", ret);
      return ret;
    }

  /* TODO: 加载训练好的Edge Impulse模型 */
  /* ret = sound_detect_load_model_file(&g_sound_ctx, "/data/model.bin"); */

  /* 启动实时检测 */

  ret = sound_detect_start(&g_sound_ctx);
  if (ret < 0)
    {
      printf("[错误] 启动声音检测失败: %d\n", ret);
      sound_detect_deinit(&g_sound_ctx);
      return ret;
    }

  printf("[安全] 声音检测已启动\n");
  g_sound_started = true;

  /* 任务四：接通「录音旁路 → 人声/非人声门控」。
   *
   * 顺序：先装回调（忙判据 / 人声回调），再启动旁路线程，最后初始化本地小模型。
   * 门控线程一起来录音线程就会往它的环形缓冲里灌数据（tap 在 ai_audio.c 里），
   * 所以回调必须先装好，否则前面的窗口判出来没人接。 */

  sound_detect_gate_set_busy_cb(gate_busy_pred, NULL);
  sound_detect_gate_set_voice_cb(gate_voice_cb, NULL);

  ret = sound_detect_bypass_start();
  if (ret < 0)
    {
      printf("[警告] 旁路门控没起来: %d（人声门控不可用，其他链路照旧）\n", ret);
    }

  /* 本地小模型（任务三 sound_event）。还没落地时是个返回 -ENOSYS 的弱桩，
   * 门控那边喂进去的异常窗会记账但不出结果 —— 不拦启动。 */

  if (sound_event_init() != 0)
    {
      printf("[门控] 本地小模型未就绪（sound_event_init 未落地），"
             "非人声异常暂时只进统计\n");
    }
  else
    {
      printf("[门控] 本地小模型已就绪\n");

#ifdef CONFIG_HELLO_APP_SOUND_EVENT
      /* 挂上命中回调：没这一步模型判出来了也没人接（结果直接丢）。
       * 门控线程此刻已经在灌数据了，所以紧跟着 init 装，别再往后放。 */

      sound_event_set_callback(sound_event_cb, NULL);
      printf("[门控] 本地小模型命中回调已挂上（摔倒/敲击/尖叫 → 追问+询问框）\n");
#endif
    }

  return OK;
}

/**
 * @brief  停止声音检测
 */

static void stop_sound_detection(void)
{
  if (!g_sound_started)
    {
      return;
    }

  /* 先停旁路（它只是拿一份拷贝，和录音链路无关）：必须在声音检测器之前，
   * 它会调 sound_event_feed，那个入口属于检测器那一侧。 */

  sound_detect_bypass_stop();

  sound_detect_stop(&g_sound_ctx);
  sound_detect_deinit(&g_sound_ctx);
  g_sound_started = false;
  printf("[安全] 声音检测已停止\n");
}

/**
 * @brief  任务四门控调试入口（`ai_companion --sounddetect ...`）
 *
 * 为什么不做成 `hw_test sounddetect ...`：hw_test 是**另一个 app**
 * （app/hw_test/main.c 的 main 里认自己的子命令），改它就越界了
 * （别的 agent 也在那附近干活）。所以挂在 ai_companion 自己的命令行上，
 * 子命令语义与任务书一致：status / on / off / sensitivity <n>。
 */

static void sound_detect_debug_cmd(const char *sub, float sens)
{
  if (sub == NULL)
    {
      return;
    }

  if (strcmp(sub, "off") == 0)
    {
      sound_detect_gate_set_enabled(false);
      return;
    }

  if (strcmp(sub, "on") == 0)
    {
      sound_detect_gate_set_enabled(true);
      return;
    }

  if (strcmp(sub, "sensitivity") == 0)
    {
      sound_detect_gate_set_sensitivity(sens > 0.0f ? sens : 1.0f);
      return;
    }

  if (strcmp(sub, "status") != 0)
    {
      printf("[门控] 未知子命令 '%s'（status / on / off / sensitivity <n>）\n",
             sub);
      return;
    }

  {
    const sound_gate_stats_t *st = sound_detect_gate_get_stats();

    if (st == NULL)
      {
        return;
      }

    printf("[门控] 状态: %s, 灵敏度=%.2f, 旁路线程=%s\n",
           sound_detect_gate_enabled() ? "on" : "off",
           (double)sound_detect_gate_sensitivity(),
           st->windows + st->mute_windows > 0 ? "在跑" : "还没收到窗");
    printf("[门控] 统计: 判过 %u 窗 / 人声 %u / 播放跳过 %u / 异常喂模型 %u\n",
           (unsigned)st->windows, (unsigned)st->voice_windows,
           (unsigned)st->mute_windows, (unsigned)st->anomaly_windows);
    printf("[门控] 最近一窗: rms=%.4f zcr=%.2f ac=%.2f 浊音帧=%.2f "
           "active=%d voice=%d\n",
           (double)st->last.rms, (double)st->last.zcr,
           (double)st->last.ac_peak, (double)st->last.voiced_ratio,
           st->last.active, st->last.voice);
  }
}

/****************************************************************************
 * 异常声 -> 追问流程 (W1)
 *
 * 用户点名的需求：检测到异常声别直接报警，先问一句「请问需要帮助吗？」，
 * 听老人的回答再决定。
 *
 * 状态机（全部在 main_loop_task 的 100ms 心跳里推进）：
 *
 *   IDLE ──sound_detect_callback() 过滤+防抖后置 PENDING──> PENDING
 *   PENDING ──tick: 播 TTS「请问需要帮助吗？」──> SPEAKING
 *   SPEAKING ──播放完成回调置 g_ask_play_done──> LISTENING(deadline = now + 6s)
 *   LISTENING ──录音线程做 ASR 后置 g_ask_answer_ready──> 三路判定：
 *        紧急（需要/救命/帮忙/疼/摔倒…） -> 上报报警 + publish start_alarm -> IDLE
 *        没事（不用/没事/还好…）        -> TTS 安抚一句                     -> IDLE
 *        没听清（空串/没说出来）         -> 还有轮数就再问一次；问满 2 轮就
 *                                          上报"未确认"                     -> IDLE
 *   LISTENING ──tick 发现超过 deadline──> 同上"没听清"
 *
 * 音频链路全部复用现成的：
 *   - 说话：voice_tts_speak() + audio_play_start()（播放内部会停录音、播完自动恢复录音）
 *   - 听  ：常开的 start_audio_listening()（VAD + 累积缓冲）+ process_ai_dialogue() 的 ASR
 ****************************************************************************/

/**
 * @brief  「请问需要帮助吗？」说完的回调（**在播放线程里**）
 *
 * 只置标志，相位交给 ask_flow_tick() 推。
 */

static void ask_play_done_cb(void *user_data)
{
  (void)user_data;
  g_voice_spoke = true;
  g_ask_play_done = true;
}

/**
 * @brief  用现成的 TTS + 播放链路说一句话（**预缓存优先**）
 * @param  text  要说的文本
 * @return 0 已开始播放, 负值失败
 *
 * ★ 预缓存优先（2026-09-19 加）：追问这三句问句（「请问需要帮助吗？」等）是**写死
 *   的固定文案**，PC 上早就合成好、随固件打进 ROMFS 了（app/hello_app/tts_cache.c
 *   的查表规则）。命中就 open + read 几十 KB + 起播，**一个字节的网络都不走** ——
 *   这很关键：本函数跑在**主循环**里（问句是 ask_flow_tick() 推的，见那边"TTS
 *   合成是阻塞调用"那段），而 live TTS 是一次阻塞 HTTPS：网络半通（RNDIS 起来了、
 *   DNS/网关还没就绪那种）时 getaddrinfo/connect 根本没有上界，主循环会被按死，
 *   连带状态机超时、监听守护全都一起停摆。预缓存把这条最常见的路
 *   从"要走网络"变成"读 ROMFS"，风险面直接小一圈。
 *   回落仍然保留：现场临时来的文案（没预生成的）照旧走 live TTS。
 */
static int ask_speak(const char *text)
{
  static unsigned char *tts_buf = NULL;
  static size_t tts_buf_size = 0;
  unsigned char *cached = NULL;
  size_t cached_len = 0;
  size_t tts_len = 0;
  int ret;

  /* ---- ① 先查预缓存（不阻塞、不出网） ---- */

  if (tts_cache_load(text, &cached, &cached_len) == 0)
    {
      ret = audio_play_start(&g_audio_ctx, (const int16_t *)cached,
                                    cached_len / sizeof(int16_t),
                                    ask_play_done_cb, NULL);
      printf("[追问] 出声（预缓存 %zu 字节 ≈ %zu ms，不出网）: %s\n",
             cached_len, cached_len * 1000 / (16000 * 2), text);

      /* 起播已经把数据 memcpy 进播放缓冲了，这里可以立刻还掉，
       * 不用等它念完（理由同 audio_play_start 的说明）。 */
      free(cached);

      if (ret < 0)
        {
          printf("[追问] 播放启动失败: %d\n", ret);
          return ret;
        }

      return OK;
    }

  /* ---- ② 没预生成：回落原来的 live TTS ---- */

  if (tts_buf == NULL)
    {
      tts_buf_size = ASK_TTS_BUF_BYTES;
      tts_buf = malloc(tts_buf_size);
      if (tts_buf == NULL)
        {
          /* 短句 1 秒 ≈ 32 KB，够用就不至于整条流程哑掉 */
          tts_buf_size = 32 * 1024;
          tts_buf = malloc(tts_buf_size);
        }

      if (tts_buf == NULL)
        {
          printf("[追问] TTS 缓冲分配失败\n");
          return -ENOMEM;
        }
    }

  printf("[追问] 没预缓存，回落 live TTS（这次要走网络）: %s\n", text);

  ret = voice_tts_speak(text, tts_buf, tts_buf_size, &tts_len);
  if (ret < 0 || tts_len == 0)
    {
      printf("[追问] 语音合成失败: %d\n", ret);
      return ret < 0 ? ret : -EIO;
    }

  /* 半双工不用自己管：audio_play_start() 会先停录音，播完按原配置把录音恢复起来
   * （ai_audio.c 的 audio_prepare_output / audio_resume_record），
   * 恢复之后麦克风才重新有人喂检测器 —— 顺带也挡住了"喇叭的声音被判成异常"。 */

  ret = audio_play_start(&g_audio_ctx, (const int16_t *)tts_buf,
                                tts_len / sizeof(int16_t), ask_play_done_cb, NULL);
  if (ret < 0)
    {
      printf("[追问] 播放启动失败: %d\n", ret);
      return ret;
    }

  printf("[追问] 出声: %s\n", text);
  return OK;
}

/**
 * @brief  录音线程送来这一轮的识别结果（空串 = 没听清）
 */

static void ask_flow_mark_answer(const char *text)
{
  g_ask_answer[0] = '\0';

  if (text != NULL)
    {
      strncpy(g_ask_answer, text, sizeof(g_ask_answer) - 1);
      g_ask_answer[sizeof(g_ask_answer) - 1] = '\0';
    }

  g_ask_answer_ready = true;
}

/* 回答的关键词表。
 * 判定顺序**必须**先否定后紧急：「不需要」「不用」这类否定回答里本身就含
 * 「需要」「用」这些字，先匹配紧急关键词会把"不需要，我没事"判成求救。
 * 表里都是整词/短句而不是单字，宁可判成"没听清"（会再问一次），也不要误报警。 */

static const char *const g_ask_neg_keywords[] =
{
  "不用", "不需要", "没事", "没有", "还好", "没关", "别管", "不痛", "不疼",
  "没摔", "没跌", "不难受", "挺好", "没啥", "不碍事"
};

static const char *const g_ask_sos_keywords[] =
{
  "需要", "救命", "救", "帮", "疼", "痛", "摔倒", "跌", "不舒服", "难受",
  "起不来", "骨折", "流血", "头晕", "恶心", "动不了"
};

/**
 * @brief  按关键词判定一句识别结果
 * @return ASK_VERDICT_*
 */

static int ask_classify(const char *text)
{
  size_t i;

  if (text == NULL || text[0] == '\0')
    {
      return ASK_VERDICT_UNCLEAR;
    }

  for (i = 0; i < sizeof(g_ask_neg_keywords) / sizeof(g_ask_neg_keywords[0]);
       i++)
    {
      if (strstr(text, g_ask_neg_keywords[i]) != NULL)
        {
          return ASK_VERDICT_REASSURED;
        }
    }

  for (i = 0; i < sizeof(g_ask_sos_keywords) / sizeof(g_ask_sos_keywords[0]);
       i++)
    {
      if (strstr(text, g_ask_sos_keywords[i]) != NULL)
        {
          return ASK_VERDICT_EMERGENCY;
        }
    }

  return ASK_VERDICT_UNCLEAR;
}

/**
 * @brief  收尾：清干净标志，并把状态机从 AI_TALKING 放回监听态
 */

static void ask_flow_finish(sm_context_t *ctx)
{
  g_ask_phase = ASK_PHASE_IDLE;
  g_ask_round = 0;
  g_ask_play_done = false;
  g_ask_answer_ready = false;
  g_ask_answer[0] = '\0';

  /* 这一轮的 ASR 是被 VAD 推进 AI_TALKING 之后在 sm_ai_talking_enter() 里做的，
   * 我们没给它发 AI_RESPONSE，收尾时补一下，否则它会一直停在 AI_TALKING
   * 等 30 秒超时。 */
  if (sm_get_state(ctx) == SM_STATE_AI_TALKING)
    {
      sm_handle_event(ctx, SM_EVENT_AI_RESPONSE);
    }

  /* 对称的另一半：这一路收尾了（判定完 / 两轮都没听清 / 已经报警），屏幕上那页
   * 「是否报警？」就不该继续挂着 —— 挂着的后果是用户十几秒后再去点它，那一下
   * 会变成**对一件已经被处理过的事**的又一次报警（两条确认路同时活着时最坏的
   * 那个结果）。robot_ui 那边按「作废」处理：撤页面，既不算确认（不报警）也不算
   * 否认（不记 60 秒静默期），见 main.c 的 ui_post_ask_standdown()。
   * 没有 pending 询问时那边是空操作（连日志都不打）。 */
  ui_post_ask_standdown("语音追问收尾");

  printf("[追问] 流程结束\n");
}

/**
 * @brief  判定紧急：上报报警 + 让界面弹报警页
 */

static void ask_flow_escalate(sm_context_t *ctx, const char *answer)
{
  char detail[ASK_DETAIL_MAX];

  snprintf(detail, sizeof(detail), "检测到%s（置信度 %.2f），老人回答「%s」",
           sound_detect_get_type_name((sound_type_t)g_ask_pending_type),
           (double)g_ask_pending_conf, answer);

  printf("[追问] 判定: 紧急 -> %s\n", detail);

  if (g_net_started)
    {
      /* ★ 报警去重闸：屏幕按钮 / MQTT confirm_alarm 那条确认路（robot_ui 的
       * ask_finish）可能已经把这次异常报警执行完了。一次异常只报一次警 ——
       * 没领到票就整块放弃（不上报、不请界面弹报警页、不换表情），因为赢家
       * 那一路已经把页面 + 铃声 + 上报都做过了，再来一遍家人手机上就是两条报警。
       * 弱符号（robot_ui 没进镜像时）恒为 true，不会把这条路堵死。 */
      if (!robot_ui_alarm_claim("语音追问"))
        {
          printf("[追问] 这次异常已经由另一条确认入口报过警了，本路放弃重复上报\n");
        }
      else
        {
          /* ai_network_send_alarm() 内部就是 report_alarm()：publish 到
           * zhi_ai/<client_id>/alarm + 手机推送，所以不用再单独调一次 report_alarm()。 */
          ai_network_send_alarm(&g_net_ctx, "sound_emergency", detail);

          /* 让界面（robot_ui）弹报警页 */
          ai_network_send_start_alarm(&g_net_ctx, detail);
          ai_network_send_face(&g_net_ctx, AI_CMD_FACE_WORRIED);
        }
    }

  /* 确认紧急才进报警状态（原来是"一检测到异常声就进"，误报会直接把设备按在
   * 报警态 60 秒，期间唤醒/对话全被吃掉）。 */
  sm_handle_event(ctx, SM_EVENT_ALARM_DETECTED);

  ask_flow_finish(ctx);
}

/**
 * @brief  判定"没事"：安抚一句就结束
 */

static void ask_flow_soothe(sm_context_t *ctx, const char *answer)
{
  printf("[追问] 判定: 不需要帮忙（回答「%s」）\n", answer);

  /* 这句话也要上屏：主路同进程直调，旁路照旧 MQTT（给手机/PC 端） */
  robot_ui_bridge_voice_reply("那就好，有事再叫我。");

  if (g_net_started)
    {
      ai_network_send_ai_reply(&g_net_ctx, "那就好，有事再叫我。");
      ai_network_send_face(&g_net_ctx, AI_CMD_FACE_HAPPY);
    }

  /* 这句播完不用再听，所以直接收尾（播放完成回调进来时相位已经是 IDLE，
   * tick 不会理它）。 */
  ask_speak("那就好，有事再叫我。");
  ask_flow_finish(ctx);
}

/**
 * @brief  没听清：还有轮数就再问一次，问满了就上报"未确认"
 */

static void ask_flow_unclear(sm_context_t *ctx)
{
  if (g_ask_round < ASK_MAX_ROUNDS)
    {
      printf("[追问] 没听清，再问一次（第 %d 轮）\n", g_ask_round + 1);
      g_ask_phase = ASK_PHASE_PENDING;
      return;
    }

  {
    char type_name[64];

    /* 报"未确认"走 /sound_alarm 这条通道（不上报 /alarm、不推手机）：
     * 两次都没听清就按真报警推给家人，误报比漏报更伤这个场景。 */
    snprintf(type_name, sizeof(type_name), "unconfirmed_%s",
             sound_detect_get_type_name((sound_type_t)g_ask_pending_type));

    printf("[追问] 两轮都没听清，上报未确认: %s\n", type_name);

    if (g_net_started)
      {
        ai_network_report_sound_alarm(&g_net_ctx, type_name,
                                      (int)(g_ask_pending_conf * 100.0f));
        ai_network_send_face(&g_net_ctx, AI_CMD_FACE_WORRIED);
      }
  }

  ask_flow_finish(ctx);
}

/**
 * @brief  按识别结果分三路
 */

static void ask_flow_decide(sm_context_t *ctx, const char *answer)
{
  int verdict = ask_classify(answer);

  printf("[追问] 回答「%s」-> %s\n", answer,
         verdict == ASK_VERDICT_EMERGENCY ? "需要帮助" :
         verdict == ASK_VERDICT_REASSURED ? "不需要帮助" : "没听清");

  if (verdict == ASK_VERDICT_EMERGENCY)
    {
      ask_flow_escalate(ctx, answer);
    }
  else if (verdict == ASK_VERDICT_REASSURED)
    {
      ask_flow_soothe(ctx, answer);
    }
  else
    {
      ask_flow_unclear(ctx);
    }
}

/**
 * @brief  进入 ASK_PHASE_LISTENING：开一个限时窗口
 */

static void ask_flow_listen_start(void)
{
  g_ask_answer_ready = false;
  g_ask_answer[0] = '\0';
  g_ask_deadline_ms = main_now_ms() + ASK_LISTEN_TIMEOUT_MS;
  g_ask_phase = ASK_PHASE_LISTENING;

  printf("[追问] 开始限时听回答（%u 秒）\n", ASK_LISTEN_TIMEOUT_MS / 1000);
}

/**
 * @brief  问一句（第 1 轮 / 第 2 轮用不同的话）
 */

static void ask_flow_begin(sm_context_t *ctx)
{
  const char *question;
  int ret;

  g_ask_round++;

  question = (g_ask_round <= 1) ? "请问需要帮助吗？"
                                : "您还好吗？需要我帮忙吗？";

  printf("[追问] 第 %d 轮: %s\n", g_ask_round, question);

  /* 屏幕上也要把这句话显示出来：主路同进程直调，旁路照旧 MQTT */
  robot_ui_bridge_voice_reply(question);

  if (g_net_started)
    {
      ai_network_send_ai_reply(&g_net_ctx, question);
      ai_network_send_face(&g_net_ctx, AI_CMD_FACE_THINKING);
    }

  /* 清掉上一轮的残留（ASR 累积缓冲 / 结果 / 播放完成标志） */
  g_ask_answer_ready = false;
  g_ask_play_done = false;
  g_ask_answer[0] = '\0';
  g_speech_frames = 0;

  ret = ask_speak(question);
  if (ret < 0)
    {
      /* 说不出来就别干等：按"没听清"走（还有轮数会再试，问满了就上报未确认） */
      printf("[追问] 这一轮说不出来，按没听清处理\n");
      ask_flow_unclear(ctx);
      return;
    }

  g_ask_phase = ASK_PHASE_SPEAKING;
  g_ask_phase_ms = main_now_ms();
}

/**
 * @brief  追问流程心跳（在 main_loop_task 里每 100ms 调一次）
 *
 * 只有这个线程改 g_ask_phase；TTS 合成是阻塞调用（云端 HTTPS），
 * 在这里阻塞几秒是可以接受的 —— 主循环这一轮只做状态机超时检查和一些空壳轮询。
 */

static void ask_flow_tick(sm_context_t *ctx)
{
  uint32_t now = main_now_ms();

  /* 退出中就别再开新动作了：收尾顺序里 stop_audio_listening() 在前、
   * pthread_join(loop_thread) 在后，这里要是还在播/听，会把已经关掉的
   * 录音又拉起来。 */
  if (!g_running)
    {
      return;
    }

  /* 最优先：屏幕上那条确认路（或者 MQTT 下行）已经报警了，报警声正在响 ——
   * 这条语音追问立刻收摊，别再问第二轮（登记处见 ai_companion_req.c；
   * robot_ui_show_alarm() 是唯一登记点，所以任何报警来源都覆盖）。
   * 收摊只做状态收尾：设备归 robot_ui 的报警那条链，界面归 robot_ui
   * （它自己会把询问页撤下）。 */
  if (ai_companion_ask_abort_take() && g_ask_phase != ASK_PHASE_IDLE)
    {
      printf("[追问] 报警已由另一条确认路执行，这条语音追问收摊\n");
      ask_flow_finish(ctx);
      return;
    }

  switch (g_ask_phase)
    {
      case ASK_PHASE_PENDING:
        if (g_ask_round >= ASK_MAX_ROUNDS)
          {
            /* 兜底：正常路径到不了这里（ask_flow_unclear() 会先收尾） */
            ask_flow_finish(ctx);
            break;
          }

        ask_flow_begin(ctx);
        break;

      case ASK_PHASE_SPEAKING:
        if (g_ask_play_done ||
            now - g_ask_phase_ms >= ASK_SPEAK_TIMEOUT_MS)
          {
            ask_flow_listen_start();
          }
        break;

      case ASK_PHASE_LISTENING:
        if (g_ask_answer_ready)
          {
            char answer[ASK_ANSWER_MAX];

            memcpy(answer, g_ask_answer, sizeof(answer));
            answer[sizeof(answer) - 1] = '\0';
            ask_flow_decide(ctx, answer);
          }
        else if (now >= g_ask_deadline_ms)
          {
            printf("[追问] 这一轮没听到回答（超时）\n");
            ask_flow_unclear(ctx);
          }
        break;

      default:
        break;
    }
}

/****************************************************************************
 * 关怀播报（W2）：预缓存优先 + 有界回落
 *
 * 用户原话："关怀该播报的时候没出声"、"这个提醒可以云端跑了下栽进去，不用每次运行"。
 * 原来这个回调里只有一行 TODO（"将消息转换为语音播放"）—— 也就是说**关怀文案
 * 从来没出过声**，只有一行串口日志。
 *
 * 现在把播报补上，但三条纪律一条都不能破：
 *   1) **不在 care 线程里说话**：care_thread_func 每 30 秒拍一轮、所有任务串行调
 *      回调，在这里阻塞一次（云端 TTS 几百 ms 到上百秒）后面的关怀任务全被推后；
 *   2) **不在主循环里说话**：状态机 CARE_REMIND 的收尾是主循环 sm_run() 那 5 秒
 *      超时干的（ai_state_machine.c 的 g_default_timeouts[SM_STATE_CARE_REMIND]），
 *      主循环一被卡住，它就一直停在 CARE_REMIND —— 正是这次要修的那个形状。所以
 *      播报走**独立的一次性 detached 线程**，起完就撒手，状态机怎么走与它无关；
 *   3) **从"进入播报"到"状态机退出 CARE_REMIND"之间不许有无上界的阻塞**：
 *      缓存命中 = 读 ROMFS（亚毫秒）+ 起播（memcpy + pthread_create），本来就没有
 *      上界问题；回落那一次 live TTS 是阻塞 HTTPS、中途拦不住，所以给它两条
 *      **事前**闸 + 一条**事后**判定（见 CARE_ANNOUNCE_BUDGET_MS）：
 *        - 网络根本没通（g_net_started 假）/ 预算已经用完 → 直接跳过，一行日志收工；
 *        - 合成回来时已经超过预算 → 按"没合成出来"收尾、**不播**（迟到的关怀没有
 *          意义，那一刻状态机早出了 CARE_REMIND）。
 *      于是最坏情况从"无上界"变成"一次有界的 HTTPS + 最长 CARE_ANNOUNCE_BUDGET_MS"，
 *      而且**永远**不会变成"状态机出不来"。
 *
 * 设备那头再挡一道：我们自己**正在出声**时就别抢设备，只留一行日志（半双工上
 * 再起一段播放就是自己掐自己）。
 * ★ 让路协议 2026-09-21 已整套删除（见 ai_companion_req.h）：这里不再需要看
 *   "别人有没有拿着设备"—— robot_ui 出声时驱动会停我们的录音，它那边放完由
 *   播放线程收尾、我们这边由监听守护把录音拉回来。
 ****************************************************************************/

/* 从"进入播报"到"该收尾了"的总预算（毫秒）。取 12 秒的理由：
 *   - 状态机 CARE_REMIND 自己 5 秒就超时收尾，本预算是**播报这一侧**的上限，
 *     不是状态机那一侧的上限（两者独立、互不等待）；
 *   - 正常一条关怀文案 2~4.5 秒就念完了；12 秒对"云端 TTS 一次往返"也够
 *     （正常几百 ms）。真慢到这个份上，跳过比干等强。 */
#define CARE_ANNOUNCE_BUDGET_MS   12000

/* live 回落那次合成的输出缓冲：最长一句关怀约 5 秒，15 秒留足余量 */
#define CARE_TTS_BUF_BYTES        (16000 * 2 * 15)

/* 同一时刻只允许一次关怀播报（两条线程同时抢半双工设备没有意义） */
static volatile bool g_care_announce_busy;

/**
 * @brief  关怀播报线程（detached，一次性）
 *
 * arg 是堆上那句文案，本函数负责 free。全程不碰状态机。
 */

static void *care_announce_task(void *arg)
{
  char *text = (char *)arg;
  unsigned char *pcm = NULL;
  size_t len = 0;
  uint32_t start;
  uint32_t spent;
  int ret;

  if (text == NULL)
    {
      return NULL;
    }

  start = main_now_ms();

  printf("[关怀] 播报开始: %s\n", text);

  /* ---- ① 预缓存（写死的关怀文案全在 ROMFS 里，走这条不出网） ---- */

  if (tts_cache_load(text, &pcm, &len) == 0)
    {
      printf("[关怀] 命中预缓存: %zu 字节（约 %zu ms），不走网络\n",
             len, len * 1000 / (16000 * 2));
    }
  else
    {
      /* ---- ② 回落 live TTS（两条事前闸，挡住"没上界的等待"） ---- */

      if (!g_net_started)
        {
          printf("[关怀] 这句没预缓存、网络又还没通 —— 按\"没合成出来\"收尾，"
                 "跳过播报（不拿一次没有上界的 DNS/connect 去堵线程）\n");
          free(text);
          g_care_announce_busy = false;
          return NULL;
        }

      if ((int32_t)(main_now_ms() - start) >=
          (int32_t)CARE_ANNOUNCE_BUDGET_MS)
        {
          printf("[关怀] 这句没预缓存、预算已经用完，跳过播报\n");
          free(text);
          g_care_announce_busy = false;
          return NULL;
        }

      pcm = (unsigned char *)malloc(CARE_TTS_BUF_BYTES);
      if (pcm == NULL)
        {
          printf("[关怀] 合成缓冲分配失败，这条不播报\n");
          free(text);
          g_care_announce_busy = false;
          return NULL;
        }

      printf("[关怀] 没预缓存，回落 live TTS（这次要走网络）\n");

      ret = voice_tts_speak(text, pcm, CARE_TTS_BUF_BYTES, &len);
      if (ret < 0 || len == 0)
        {
          printf("[关怀] 语音合成失败 %d —— 按\"没合成出来\"收尾，只留提示不播报\n",
                 ret);
          free(pcm);
          free(text);
          g_care_announce_busy = false;
          return NULL;
        }

      /* ---- ③ 事后判定：合成回来得太晚就别播了 ---- */

      spent = main_now_ms() - start;
      if ((int32_t)spent >= (int32_t)CARE_ANNOUNCE_BUDGET_MS)
        {
          printf("[关怀] 合成花了 %u ms（超过预算 %u ms）—— 按\"没合成出来\"收尾，"
                 "这句不播了\n",
                 (unsigned)spent, (unsigned)CARE_ANNOUNCE_BUDGET_MS);
          free(pcm);
          free(text);
          g_care_announce_busy = false;
          return NULL;
        }
    }

  /* ---- ④ 出声（本进程自己的 g_audio_ctx，半双工由 audio_play_start 内部处理） ---- */

  if (audio_is_playing(&g_audio_ctx))
    {
      printf("[关怀] 扬声器正被别的路径用着（正在出声），这次不抢设备、只留提示\n");
    }
  else
    {
      ret = audio_play_start(&g_audio_ctx, (const int16_t *)pcm,
                             len / sizeof(int16_t), NULL, NULL);
      if (ret < 0)
        {
          printf("[关怀] 播放启动失败: %d（这句没出声）\n", ret);
        }
      else
        {
          printf("[关怀] 播报已起播（%zu 字节 ≈ %zu ms，从进入到起播 %u ms）\n",
                 len, len * 1000 / (16000 * 2),
                 (unsigned)(main_now_ms() - start));
        }
    }

  free(pcm);
  free(text);
  g_care_announce_busy = false;
  return NULL;
}

/**
 * @brief  关怀回调函数（**跑在 care 线程里**）
 *
 * 只做两件不阻塞的事：推状态机事件 + 起一条播报线程。前者原来就有，后者见上面
 * 那一大段的纪律说明 —— 这个回调里**不许**出现任何"等网络 / 等设备"的动作。
 */

static void care_callback(care_type_t type, const char *message,
                          void *user_data)
{
  sm_context_t *ctx = (sm_context_t *)user_data;
  pthread_attr_t attr;
  pthread_t tid;
  char *copy;
  size_t n;

  printf("[关怀] 触发关怀: %s\n", care_get_type_name(type));
  printf("[关怀] 消息: %s\n", message);

  /* 触发关怀事件 */

  sm_handle_event(ctx, SM_EVENT_CARE_TIMER);

  if (message == NULL || message[0] == '\0')
    {
      printf("[关怀] 这条没有文案，不播报\n");
      return;
    }

  if (g_care_announce_busy)
    {
      printf("[关怀] 上一次关怀播报还没收工，这条只留提示不播报\n");
      return;
    }

  /* 文案拷一份给播报线程：message 指向 care_task_t 里的缓冲，生命周期不归我们管 */
  n = strlen(message);
  copy = (char *)malloc(n + 1);
  if (copy == NULL)
    {
      printf("[关怀] 文案缓冲分配失败，这条不播报\n");
      return;
    }

  memcpy(copy, message, n + 1);

  /* 独立的一次性线程 + 32 KB 栈：live 回落那一次要跑完整 TLS 握手（getaddrinfo /
   * x509 都在这一层），栈给足 —— 理由与 robot_ui 的 reminder_start_announce 相同。
   * 起不起来都不影响关怀流程本身（起不来就是这次没出声，日志会说明）。 */

  g_care_announce_busy = true;

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&attr, 32768);

  if (pthread_create(&tid, &attr, care_announce_task, copy) != 0)
    {
      printf("[关怀] 播报线程起不来，这条只留提示\n");
      g_care_announce_busy = false;
      free(copy);
    }

  pthread_attr_destroy(&attr);

  printf("[关怀] 关怀完成\n");
}

/**
 * @brief  初始化并启动主动关怀
 */

static int start_care(sm_context_t *ctx)
{
  int ret;

  /* 配置主动关怀 */

  care_config_t care_cfg =
  {
    .enable_greeting = true,
    .enable_health = true,
    .enable_life = true,
    .enable_emotion = true,
    .enable_exercise = true,
    .enable_medicine = true,
    .enable_meal = true,
    .enable_sleep = true,
    .smart_mode = true,
    .callback = care_callback,
    .user_data = ctx
  };

  ret = care_init(&g_care_ctx, &care_cfg);
  if (ret < 0)
    {
      printf("[错误] 主动关怀初始化失败: %d\n", ret);
      return ret;
    }

  /* 启动关怀 */

  ret = care_start(&g_care_ctx);
  if (ret < 0)
    {
      printf("[错误] 启动主动关怀失败: %d\n", ret);
      care_deinit(&g_care_ctx);
      return ret;
    }

  printf("[关怀] 主动关怀已启动\n");
  g_care_started = true;

  /* 打印任务列表 */
  /* 注意: care_task_t 含 256B message 等, 20 个约 9KB, 放栈上会溢出
   * hello_app 任务栈(8KB), 因此用 static 一次性缓冲 */
  static care_task_t tasks[CARE_MAX_TASKS];
  int count = care_get_task_list(&g_care_ctx, tasks, CARE_MAX_TASKS);
  printf("[关怀] 当前关怀任务: %d 个\n", count);

  return OK;
}

/**
 * @brief  停止主动关怀
 */

static void stop_care(void)
{
  if (!g_care_started)
    {
      return;
    }

  care_stop(&g_care_ctx);
  care_deinit(&g_care_ctx);
  g_care_started = false;
  printf("[关怀] 主动关怀已停止\n");
}

/**
 * @brief  模拟语音唤醒检测
 * @note   实际实现由成员一提供音频驱动接口
 */

static void simulate_wakeup_detection(sm_context_t *ctx)
{
  /* TODO: 替换为真实的语音唤醒检测 */
  /* wakeup_listen_start(); */
  /* if (wakeup_word_detected()) { */
  /*     sm_handle_event(ctx, SM_EVENT_WAKEUP); */
  /* } */
}

/**
 * @brief  模拟异常声音检测
 * @note   实际实现由成员二集成声音分类模型
 */

static void simulate_alarm_detection(sm_context_t *ctx)
{
  /* TODO: 替换为真实的声音检测 */
  /* if (sound_detect_alarm()) { */
  /*     sm_handle_event(ctx, SM_EVENT_ALARM_DETECTED); */
  /* } */
}

/**
 * @brief  模拟主动关怀定时器
 * @note   实际实现由成员二设计关怀逻辑
 */

static void simulate_care_timer(sm_context_t *ctx)
{
  /* TODO: 实现定时关怀逻辑 */
  /* static uint32_t last_care_time = 0; */
  /* uint32_t now = get_tick_ms(); */
  /* if (now - last_care_time > CARE_INTERVAL_MS) { */
  /*     sm_handle_event(ctx, SM_EVENT_CARE_TIMER); */
  /*     last_care_time = now; */
  /* } */
}

/**
 * @brief  播放完成回调
 */

static void play_complete_callback(void *user_data)
{
  sm_context_t *ctx = (sm_context_t *)user_data;
  printf("[音频] 语音播放完成\n");

  /* 这轮已经出过声了：语音状态回传据此不在收尾的那一小段里报"正在想" */
  g_voice_spoke = true;

  /* 播放完成后触发AI响应完成事件 */

  sm_handle_event(ctx, SM_EVENT_AI_RESPONSE);
}

/**
 * @brief  AI回复完成回调
 */

static void llm_complete_callback(const char *response, int error,
                                  void *user_data)
{
  sm_context_t *ctx = (sm_context_t *)user_data;

  if (error != 0)
    {
      printf("[LLM] AI请求失败: %d\n", error);
      sm_handle_event(ctx, SM_EVENT_AI_ERROR);
      return;
    }

  printf("[LLM] AI回复: %s\n", response);

  /* 回传界面：AI 的每次回复正文都要上屏。
   * 主路是同进程直调（robot_ui_bridge_voice_reply：清洗 + 主屏对话区 + 镜像
   * 面板对话区，内部全是 lv_async_call，本函数在录音线程里跑也安全）；
   * 旁路是原来的 MQTT ai_reply/set_face，给手机/PC 端 —— 那种"本机界面反馈
   * 必须绕公网跑一个来回"的写法，网络一抖用户看到的就是完全没反应。 */

  robot_ui_bridge_voice_reply(response);

  if (g_net_started)
    {
      ai_network_send_ai_reply(&g_net_ctx, response);
      ai_network_send_face(&g_net_ctx, AI_CMD_FACE_HAPPY);
    }

  /* TTS: 文字转语音 */

  /* TTS 缓冲 256 KB ≈ 16 kHz/单声道/16 bit 的 8 秒；原来 32 KB 只有 1 秒，
   * 超出部分会被 volc_tts.c 静默截断（PCM buffer full）。
   * 必须走堆：静态 256 KB 会把内核 SRAM 顶到 96%（构建报告可查），运行时就没余量了；
   * 堆里有第二块 PSRAM（8 MB），大块分配会落到那边。 */
  static unsigned char *tts_buf = NULL;
  static size_t tts_buf_size = 0;
  size_t tts_len = 0;
  int ret;

  if (tts_buf == NULL)
    {
      tts_buf_size = 256 * 1024;
      tts_buf = malloc(tts_buf_size);
      if (tts_buf == NULL)
        {
          tts_buf_size = 32 * 1024;
          tts_buf = malloc(tts_buf_size);
        }

      if (tts_buf == NULL)
        {
          printf("[TTS] 缓冲分配失败，跳过合成\n");
          sm_handle_event(ctx, SM_EVENT_AI_RESPONSE);
          return;
        }

      printf("[TTS] 缓冲 %zu 字节已分配\n", tts_buf_size);
    }

  ret = voice_tts_speak(response, tts_buf, tts_buf_size, &tts_len);
  if (ret < 0 || tts_len == 0)
    {
      printf("[TTS] 语音合成失败: %d\n", ret);
      sm_handle_event(ctx, SM_EVENT_AI_RESPONSE);
      return;
    }

  printf("[TTS] 合成完成, %zu 字节\n", tts_len);

  /* 播放合成的语音（起播内部会先停录音，见 ai_audio.c 的 audio_prepare_output） */

  size_t frames = tts_len / sizeof(int16_t);
  ret = audio_play_start(&g_audio_ctx,
                                (const int16_t *)tts_buf, frames,
                                play_complete_callback, ctx);
  if (ret < 0)
    {
      printf("[TTS] 播放失败: %d\n", ret);
      sm_handle_event(ctx, SM_EVENT_AI_RESPONSE);
      return;
    }

  printf("[TTS] 开始播放语音\n");
}

/****************************************************************************
 * 本机灯控意图（"说一句话就把灯打开"）
 *
 * 位置：ASR 出了文本之后、送大模型之前。命中就本机发命令 + 本地回话，
 * 这一轮不再往大模型绕 —— 省一次往返，也免得模型一本正经地说
 * "已经帮您把灯打开了"而实际上什么都没发出去。
 *
 * 判定原则是**宁可漏、不可猜**：只要沾一点否定或歧义就返回 NONE，
 * 让这一轮照原路走大模型（大模型那边不会去动灯，最多多说一句话）。
 * 灯控说错的代价（当着老人把灯开/关掉）比多等一次往返大得多。
 ****************************************************************************/

typedef enum
{
  LIGHT_INTENT_NONE = 0,   /* 不是灯控 -> 交给大模型 */
  LIGHT_INTENT_ON,         /* 开灯 */
  LIGHT_INTENT_OFF,        /* 关灯 */
  LIGHT_INTENT_BAIL        /* 是灯控说法但含否定/歧义 -> 整句交给大模型 */
} light_intent_t;

/* 开灯的说法。只收整词/短句：单字"开""灯"太容易在闲聊里撞上
 * （"开心""灯挺亮的"），误开灯比多说一句"没听懂"严重得多。 */

static const char *const g_light_on_keywords[] =
{
  "开灯", "把灯开", "打开灯", "灯打开", "开一下灯", "灯开一下", "开个灯",
  "点灯", "灯亮起", "亮一点", "亮一些", "亮点儿", "调亮",
  NULL
};

/* 关灯的说法。同理只收整词：不收"暗一点"这种（"暗"也可能是让屏幕暗），
 * 拿不准就让大模型去接。 */

static const char *const g_light_off_keywords[] =
{
  "关灯", "把灯关", "关掉灯", "灯关掉", "关一下灯", "灯关一下", "关个灯",
  "灯关上", "灯关了", "熄灯", "灭灯",
  NULL
};

/* 否定词。命中动作词之后再回头看：**同一个小句里、动作词之前**出现过
 * 这些词就整句不执行（"别开灯""不用开灯""先别关灯"绝对不能真去动灯）。
 * 表里连"没""不能"这种也收进来：多退回一次大模型只是慢一点，
 * 猜错一次是当着老人把灯开了/关了。 */

static const char *const g_light_neg_keywords[] =
{
  "别", "不用", "不要", "先别", "无需", "勿", "不能", "不准", "不许",
  "没有", "没",
  NULL
};

/* 小句分隔符（半角 + 全角标点 + 空白）。按小句判定是为了让
 * "没什么事，把灯打开"里前一句的"没"管不到后一句的"打开灯"。
 *
 * 这里必须是**整串**的表，不能用 strcspn()/strspn() 那种"字节集合"：
 * 全角标点的 UTF-8 里带的字节会和汉字撞上（例："开" = E5 BC 80，
 * "。" = E3 80 82，两者都有 0x80），字节集合匹配会把"开灯"从中间切成
 * "开"和"灯"，灯控就永远命中不了。 */

static const char *const g_light_clause_seps[] =
{
  ",", ".", "!", "?", ";", ":", " ", "\t", "\r", "\n",
  "，", "。", "！", "？", "；", "：", "、",
  NULL
};

/**
 * @brief  p 开头是不是分隔符；是就返回它的字节数，不是返回 0
 */

static size_t light_sep_len(const char *p)
{
  size_t i;

  for (i = 0; g_light_clause_seps[i] != NULL; i++)
    {
      size_t n = strlen(g_light_clause_seps[i]);

      if (strncmp(p, g_light_clause_seps[i], n) == 0)
        {
          return n;
        }
    }

  return 0;
}

/**
 * @brief  在 hay 里找表内最早出现的关键词
 * @param  table  关键词表（以 NULL 结尾）
 * @param  pos    输出：命中位置相对 hay 的字节下标（可为 NULL）
 * @return 命中的关键词（**指向表内的字符串，不是 hay 里的位置**），
 *         没命中返回 NULL。要位置就读 pos，别拿返回值和 hay 相减。
 */

static const char *light_kw_find(const char *const *table, const char *hay,
                                 size_t *pos)
{
  const char *best = NULL;
  size_t best_pos = 0;
  size_t i;

  for (i = 0; table[i] != NULL; i++)
    {
      const char *p = strstr(hay, table[i]);

      if (p != NULL && (best == NULL || (size_t)(p - hay) < best_pos))
        {
          best = table[i];
          best_pos = (size_t)(p - hay);
        }
    }

  if (best != NULL && pos != NULL)
    {
      *pos = best_pos;
    }

  return best;
}

/**
 * @brief  判定一个小句的灯控意图
 * @param  clause  已经用 '\0' 截好的小句
 * @return LIGHT_INTENT_ON / _OFF / _NONE / _BAIL（否定或歧义）
 */

static light_intent_t light_clause_intent(const char *clause)
{
  const char *on_kw;
  const char *off_kw;
  size_t on_pos = 0;
  size_t off_pos = 0;
  size_t kw_pos;
  size_t i;

  on_kw = light_kw_find(g_light_on_keywords, clause, &on_pos);
  off_kw = light_kw_find(g_light_off_keywords, clause, &off_pos);

  /* 一句里又开又关（"把灯关掉再打开"）= 拿不准，退回大模型 */

  if (on_kw != NULL && off_kw != NULL)
    {
      printf("[意图] 灯控语句同时含开/关，拿不准，交给大模型: %s\n", clause);
      return LIGHT_INTENT_BAIL;
    }

  if (on_kw == NULL && off_kw == NULL)
    {
      return LIGHT_INTENT_NONE;
    }

  kw_pos = (on_kw != NULL) ? on_pos : off_pos;

  /* 反向动作字：这句已经命中灯控词之后再看一眼有没有反方向的字
   * （"把灯关掉再打开"的"开"、"开一下灯再关掉"的"关"）。
   * 只查单字（"开"/"关"），所以"把窗帘打开"这种没有灯字的话根本走不到这里，
   * 不会被误判成开灯。 */

  if (strstr(clause, (on_kw != NULL) ? "关" : "开") != NULL)
    {
      printf("[意图] 灯控语句含反方向动作字，拿不准，交给大模型: %s\n", clause);
      return LIGHT_INTENT_BAIL;
    }

  /* 否定词落在动作词**之前**才算否定这句话：动作词后面的否定词管的是
   * 下一个动作（"把灯打开…别关"），不归这一句。 */

  for (i = 0; g_light_neg_keywords[i] != NULL; i++)
    {
      const char *p = strstr(clause, g_light_neg_keywords[i]);

      if (p != NULL && (size_t)(p - clause) < kw_pos)
        {
          printf("[意图] 灯控语句含否定词「%s」，不执行，交给大模型: %s\n",
                 g_light_neg_keywords[i], clause);
          return LIGHT_INTENT_BAIL;
        }
    }

  return (on_kw != NULL) ? LIGHT_INTENT_ON : LIGHT_INTENT_OFF;
}

/**
 * @brief  判定整句 ASR 结果的灯控意图（按标点切成小句逐句看）
 * @return LIGHT_INTENT_ON / _OFF / _NONE（含拿不准的情况）
 */

static light_intent_t light_intent_classify(const char *text)
{
  light_intent_t found = LIGHT_INTENT_NONE;
  char buf[LIGHT_TEXT_MAX];
  char *p;
  size_t len;

  if (text == NULL || text[0] == '\0')
    {
      return LIGHT_INTENT_NONE;
    }

  /* 太长的句子不是灯控命令，也可能被截断到看不见关键词，不猜 */

  if (strlen(text) >= sizeof(buf))
    {
      printf("[意图] 语句过长，灯控不判定，交给大模型\n");
      return LIGHT_INTENT_NONE;
    }

  /* 逐小句要就地截断成 C 串，先拷一份（调用方还要用原文） */

  strncpy(buf, text, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  p = buf;
  while (*p != '\0')
    {
      light_intent_t one;
      const char *sep;
      size_t sep_pos = 0;
      size_t sep_len;
      char saved;

      /* 先跳过连着的标点/空白（上一轮只处理到小句末尾，分隔符留在原地） */

      sep_len = light_sep_len(p);
      if (sep_len > 0)
        {
          p += sep_len;
          continue;
        }

      /* 这个小句一直取到下一个分隔符为止。
       * light_kw_find() 返回的是**表里的字符串**，长度必须走 sep_pos，
       * 拿返回值和 p 相减会算出一个越界的"长度"。 */

      sep = light_kw_find(g_light_clause_seps, p, &sep_pos);
      len = (sep != NULL) ? sep_pos : strlen(p);

      saved = p[len];
      p[len] = '\0';                    /* 就地截出这一小句 */

      one = light_clause_intent(p);

      p[len] = saved;                   /* 还原分隔符，下一轮开头跳过它 */

      /* 有一个小句拿不准，整句就拿不准（"别开灯…把灯打开"这种别去猜） */

      if (one == LIGHT_INTENT_BAIL)
        {
          return LIGHT_INTENT_NONE;
        }

      if (one != LIGHT_INTENT_NONE)
        {
          /* 几个小句指了不同方向（"先关灯…再开灯"）也算拿不准 */

          if (found != LIGHT_INTENT_NONE && found != one)
            {
              printf("[意图] 灯控语句前后矛盾，拿不准，交给大模型: %s\n", text);
              return LIGHT_INTENT_NONE;
            }

          found = one;
        }

      p += len;
    }

  return found;
}

/**
 * @brief  说一句灯控回话（复用现成的 TTS + 播放链路，**预缓存优先**）
 * @return 0 已开始播放, 负值失败（调用方负责收尾状态机）
 *
 * ★ 预缓存优先（2026-09-19 加）：这四句回话全是写死的宏（LIGHT_REPLY_ON/OFF、
 *   LIGHT_FAIL_ON/OFF，见本文件 :173-178），PC 上早就合成好、随固件打进 ROMFS 了
 *   （查表规则见 tts_cache.h 头上）。命中就 open + read 几十 KB + 起播，
 *   **一个字节的网络都不走** —— 这条尤其值当：`语音开灯` 是我们最拿得出手的
 *   演示动作，可它原来必须先过一次阻塞 HTTPS 才说得出话，网络一慢，用户看到的是
 *   "灯亮了但机器人半天不吭声"。回落仍然保留：没预生成的文案照旧走 live TTS。
 */

static int light_speak(sm_context_t *ctx, const char *text)
{
  static unsigned char *tts_buf = NULL;
  static size_t tts_buf_size = 0;
  unsigned char *cached = NULL;
  size_t cached_len = 0;
  size_t tts_len = 0;
  int ret;

  /* ---- ① 先查预缓存（不阻塞、不出网） ---- */

  if (tts_cache_load(text, &cached, &cached_len) == 0)
    {
      ret = audio_play_start(&g_audio_ctx, (const int16_t *)cached,
                                    cached_len / sizeof(int16_t),
                                    play_complete_callback, ctx);
      printf("[意图] 灯控回话出声（预缓存 %zu 字节 ≈ %zu ms，不出网）: %s\n",
             cached_len, cached_len * 1000 / (16000 * 2), text);

      /* 起播已经把数据 memcpy 进播放缓冲了，这里可以立刻还掉（同 ask_speak）。 */
      free(cached);

      if (ret < 0)
        {
          printf("[意图] 灯控回话播放启动失败: %d\n", ret);
          return ret;
        }

      return OK;
    }

  /* ---- ② 没预生成：回落原来的 live TTS ---- */

  if (tts_buf == NULL)
    {
      tts_buf_size = LIGHT_TTS_BUF_BYTES;
      tts_buf = malloc(tts_buf_size);
      if (tts_buf == NULL)
        {
          /* 短句 1 秒 ≈ 32 KB，够用就不至于整个功能哑掉 */
          tts_buf_size = 32 * 1024;
          tts_buf = malloc(tts_buf_size);
        }

      if (tts_buf == NULL)
        {
          printf("[意图] TTS 缓冲分配失败，这句不播了\n");
          return -ENOMEM;
        }
    }

  printf("[意图] 灯控回话没预缓存，回落 live TTS（这次要走网络）: %s\n", text);

  ret = voice_tts_speak(text, tts_buf, tts_buf_size, &tts_len);
  if (ret < 0 || tts_len == 0)
    {
      printf("[意图] 灯控回话合成失败: %d\n", ret);
      return ret < 0 ? ret : -EIO;
    }

  /* 半双工不用自己管：audio_play_start() 会先停录音，播完按原配置把录音恢复起来
   * （和追问流程的 ask_speak() 同一条路）。
   * 播放完成回调直接用现成的 play_complete_callback()：它置"这轮出过声"
   * 并发 AI_RESPONSE，让状态机从 AI_TALKING 收尾回 LISTENING。 */

  ret = audio_play_start(&g_audio_ctx, (const int16_t *)tts_buf,
                                tts_len / sizeof(int16_t),
                                play_complete_callback, ctx);
  if (ret < 0)
    {
      printf("[意图] 灯控回话播放启动失败: %d\n", ret);
      return ret;
    }

  printf("[意图] 灯控回话出声: %s\n", text);
  return OK;
}

/**
 * @brief  执行灯控意图：发 device_cmd + 回一句话给界面 + 用 TTS 说出来
 *
 * 返回前必须让状态机离开 AI_TALKING（要么靠 playback 完成回调，要么靠这里的
 * 兜底），否则界面会一直停在"正在识别"，监听守护也不敢开麦（它把 AI_TALKING
 * 当忙）。
 */

static void handle_light_intent(sm_context_t *ctx, light_intent_t intent)
{
  const char *what = (intent == LIGHT_INTENT_ON) ? "开灯" : "关灯";
  const char *cmd = (intent == LIGHT_INTENT_ON) ? LIGHT_CMD_ON : LIGHT_CMD_OFF;
  const char *speech;
  int cmd_ret = -ENOTCONN;

  if (g_net_started)
    {
      cmd_ret = ai_network_send_device_command(&g_net_ctx, LIGHT_DEVICE_ID, cmd);
    }

  if (cmd_ret < 0)
    {
      /* 兜底话术：明确告诉老人灯没被动过，别静默失败 */

      speech = (intent == LIGHT_INTENT_ON) ? LIGHT_FAIL_ON : LIGHT_FAIL_OFF;

      printf("[意图] 命中灯控: %s，但 device_cmd 发送失败(%d)，"
             "兜底回话「%s」\n", what, cmd_ret, speech);
    }
  else
    {
      speech = (intent == LIGHT_INTENT_ON) ? LIGHT_REPLY_ON : LIGHT_REPLY_OFF;

      printf("[意图] 命中灯控: %s -> device_cmd %s=%s，回话「%s」\n",
             what, LIGHT_DEVICE_ID, cmd, speech);
    }

  /* 界面也要看到这句话：主路同进程直调，旁路照旧 MQTT（手机/PC 端） */

  robot_ui_bridge_voice_reply(speech);

  if (g_net_started)
    {
      ai_network_send_ai_reply(&g_net_ctx, speech);
      ai_network_send_face(&g_net_ctx,
                           (cmd_ret < 0) ? AI_CMD_FACE_WORRIED
                                         : AI_CMD_FACE_HAPPY);
    }

  /* 播报期间状态机还留在 AI_TALKING：voice_state 会由 audio_is_playing()
   * 算成"正在说话"，监听守护也正好因为 AI_TALKING 不去碰麦克风。 */

  if (light_speak(ctx, speech) < 0)
    {
      /* 说不出话也得收尾，不能让这一轮悬着 */

      printf("[意图] 灯控回话没能出声，直接收尾\n");
      sm_handle_event(ctx, SM_EVENT_AI_RESPONSE);
    }
}

/**
 * @brief  取一个 UTF-8 码点，并返回它占的字节数
 *
 * 只认规范的 1~4 字节序列；续字节单独出现、或序列尾巴被截断时，退回"单个字节
 * 算一个码点"（后面按"有内容"处理：宁可多发一次，也别把老人说的话吞掉）。
 *
 * @param  p   指向 UTF-8 串里的某个字节（调用方保证 p[0] != '\0'）
 * @param  cp  输出码点
 * @return 该码点占的字节数（>= 1；只有整段续字节都齐了才返回 > 1）
 */

static size_t utf8_next_cp(const unsigned char *p, unsigned int *cp)
{
  size_t n;
  size_t i;

  if (p[0] < 0x80)
    {
      *cp = p[0];
      return 1;
    }

  if ((p[0] & 0xe0) == 0xc0)
    {
      n = 2;
      *cp = p[0] & 0x1f;
    }
  else if ((p[0] & 0xf0) == 0xe0)
    {
      n = 3;
      *cp = p[0] & 0x0f;
    }
  else if ((p[0] & 0xf8) == 0xf0)
    {
      n = 4;
      *cp = p[0] & 0x07;
    }
  else
    {
      *cp = p[0];
      return 1;
    }

  /* 先按首字节定长度，再要求后面全是续字节（高两位 10）。
   * 有一节不满足就说明这句在中间断了，当作非法字节，只吃这一个。 */

  for (i = 1; i < n; i++)
    {
      if ((p[i] & 0xc0) != 0x80)
        {
          *cp = p[0];
          return 1;
        }

      *cp = (*cp << 6) | (p[i] & 0x3f);
    }

  return n;
}

/**
 * @brief  这个码点算不算"没内容的噪声"（空白或标点）
 *
 * 只用来判"ASR 是不是压根没听清"：老人说的话总归有汉字、数字或字母；
 * 整串都是空白和标点（"."、"。。。"、"?! "、全角空格）基本是一声异响被 VAD
 * 切出来的误触发，这种既不该回传界面，也不该拿去问大模型。
 *
 * 注意别把全角字母/数字（Ａ１）也算进来：那是有内容的输入。
 */

static bool utf8_cp_is_noise(unsigned int cp)
{
  /* 控制字符、半角空格、半角删除符、不间断空格、全角空格 */
  if (cp < 0x21 || cp == 0x7f || cp == 0xa0 || cp == 0x3000)
    {
      return true;
    }

  /* ASCII 可见标点：!"#$%&'()*+,-./  :;<=>?@  [\]^_`  {|}~ */

  if ((cp >= 0x21 && cp <= 0x2f) || (cp >= 0x3a && cp <= 0x40) ||
      (cp >= 0x5b && cp <= 0x60) || (cp >= 0x7b && cp <= 0x7e))
    {
      return true;
    }

  /* U+2010 ~ U+205E：破折号、引号、省略号、间隔号等一整段符号 */

  if (cp >= 0x2010 && cp <= 0x205e)
    {
      return true;
    }

  /* 中文标点：、。〃、「」『』（）【】〔〕等 */

  if ((cp >= 0x3001 && cp <= 0x3003) ||
      (cp >= 0x3008 && cp <= 0x3011) ||
      (cp >= 0x3014 && cp <= 0x301f) ||
      (cp >= 0x3030 && cp <= 0x303f) ||
      (cp >= 0xfe10 && cp <= 0xfe19) ||
      (cp >= 0xfe30 && cp <= 0xfe4f))
    {
      return true;
    }

  /* 全角标点（！？，。：；""（）等）与半角 CJK 标点（｡｢｣､･）。
   * 全角字母（Ａ-Ｚａ-ｚ）和全角数字（０-９）不在此列，它们算内容。 */

  if ((cp >= 0xff01 && cp <= 0xff0f) || (cp >= 0xff1a && cp <= 0xff20) ||
      (cp >= 0xff3b && cp <= 0xff40) || (cp >= 0xff5b && cp <= 0xff65))
    {
      return true;
    }

  return false;
}

/**
 * @brief  ASR 原文里有没有真内容（不是空串，也不是只有空白/标点）
 */

static bool user_said_has_content(const char *text)
{
  const unsigned char *p = (const unsigned char *)text;
  unsigned int cp;

  if (text == NULL)
    {
      return false;
    }

  while (*p != '\0')
    {
      p += utf8_next_cp(p, &cp);

      if (!utf8_cp_is_noise(cp))
        {
          return true;
        }
    }

  return false;
}

/**
 * @brief  把 ASR 原文裁到界面能收下的长度（超长按整字截断）
 *
 * 砍点落在某个 UTF-8 多字节序列中间时，把那个残缺的字符整个丢掉 ——
 * 界面是直接拿去显示的，半个字会变成方块。
 *
 * @param  dst       输出缓冲
 * @param  dst_size  输出缓冲大小（含结尾 '\0'）
 * @param  src       原文
 * @return 写进 dst 的字节数（不含结尾 '\0'）
 */

static size_t user_said_cut(char *dst, size_t dst_size, const char *src)
{
  size_t n;

  if (dst_size == 0)
    {
      return 0;
    }

  n = strlen(src);
  if (n > dst_size - 1)
    {
      n = dst_size - 1;
    }

  /* 续字节的高两位固定是 10：砍点正好落在续字节上，说明这个字还缺头，
   * 往前退到它的首字节（下标 n 处最多是结尾 '\0'，不会读过界）。 */

  while (n > 0 && ((unsigned char)src[n] & 0xc0) == 0x80)
    {
      n--;
    }

  memcpy(dst, src, n);
  dst[n] = '\0';
  return n;
}

/**
 * @brief  把用户原话回传界面（本文件唯一的 user_said 发送点）
 *
 * 只从 process_ai_dialogue() 里调一次，位置在拿到非空 ASR 文本之后、**所有分支
 * 之前**：下面的追问判定 / 灯控 / 大模型三条出路都被这一次覆盖，所以每句话只发
 * 一遍 —— handle_light_intent() 里绝不能再发，否则老人说"开灯"时界面会看到两遍。
 *
 * 两条路一起走（和 voice_state_tick 同一个理由）：
 *   1) 同进程直调 robot_ui_bridge_voice_user_said()：本机界面立刻刷屏，不依赖网络；
 *   2) MQTT 照发：手机/PC 端还订阅着这个主题（它们不在这个地址空间里）。
 * 任何一条失败都只打日志：界面少显示一句绝不能反过来影响 ASR -> 大模型这条链路。
 *
 * @param  text  非空的 ASR 原文
 */

static void publish_user_said(const char *text)
{
  char said[USER_SAID_MAX];
  size_t text_len;
  size_t said_len;
  int ret = -ENOTCONN;

  if (text == NULL || text[0] == '\0')
    {
      return;
    }

  text_len = strlen(text);
  said_len = user_said_cut(said, sizeof(said), text);

  if (said_len < text_len)
    {
      printf("[语音] 用户原话过长(%zu 字节)，回传界面按整字截到 %zu 字节\n",
             text_len, said_len);
    }

  /* 主路：同进程直调。清洗（emoji/markdown -> 字库能渲染的纯文本）在界面那边
   * 做（robot_ui_bridge.c 里过 main.c 的 sanitize_for_display），这里给原文。 */

  robot_ui_bridge_voice_user_said(said);

  /* 旁路：MQTT，给手机/PC 端。没连上就只记日志 —— 界面已经刷过了。 */

  if (g_net_started)
    {
      ret = ai_network_publish_command(&g_net_ctx, AI_CMD_ACTION_USER_SAID,
                                       said);
    }

  if (ret < 0)
    {
      printf("[语音] user_said MQTT 回传失败(%d)（界面已由直调刷过，手机端看不到）:"
             " %s\n", ret, said);
    }
  else
    {
      printf("[语音] user_said 已回传: %s\n", said);
    }
}

/**
 * @brief  处理AI对话的本体（在AI_TALKING状态调用，见 sm_ai_talking_enter）
 */

static void process_ai_dialogue(sm_context_t *ctx)
{
  char text_buf[512] = {0};
  light_intent_t light_intent;
  size_t frames;
  int ret;

  /* 异常声追问流程（W1）：这一轮的识别结果不送大模型，只用来判定
   * "要不要真报警"。判定和收尾都在 ask_flow_tick() 里做（那个线程独占相位），
   * 这里只把文本送过去。 */
  bool ask_mode = (g_ask_phase == ASK_PHASE_LISTENING);

  /* 累积帧数取一次快照，后面的判断和送 ASR 都用它 —— 清场那几段（监听守护里
   * 重开会话之前、以及放音收尾恢复录音之前）都在别的线程里把 g_speech_frames
   * 清零，读到一半被清就会出现"判断时有数据、送 ASR 时长度变 0"这种自相矛盾的
   * 一轮。快照只影响这一次识别（用的是旧值），不会越界：缓冲是常驻的，
   * 帧数只会变小。 */

  frames = g_speech_frames;

  printf("[AI] 开始AI对话处理 (累积 %zu 帧音频)\n", frames);

  /* 如果没有累积到足够的语音数据，跳过 ASR。
   * 门槛（100ms）和 ai_companion_voice_submit() 那边共用同一个宏：两边口径必须
   * 一致 —— 那边达标才会登记「提交」请求，这里不达标就白白跳过一次识别。 */

  if (g_speech_buf == NULL || frames < SPEECH_MIN_FRAMES_FOR_ASR)
    {
      printf("[AI] 语音数据不足，跳过\n");

      if (ask_mode)
        {
          ask_flow_mark_answer(NULL);
        }
      else
        {
          sm_handle_event(ctx, SM_EVENT_AI_ERROR);
        }

      return;
    }

  /* ASR: 语音转文字 */

  printf("[AI] 正在进行语音识别...\n");
  ret = voice_asr_recognize((const unsigned char *)g_speech_buf,
                            frames * sizeof(int16_t),
                            text_buf, sizeof(text_buf));
  if (ret < 0)
    {
      printf("[AI] 语音识别失败: %d\n", ret);

      if (ask_mode)
        {
          ask_flow_mark_answer(NULL);
        }
      else
        {
          sm_handle_event(ctx, SM_EVENT_AI_ERROR);
        }

      return;
    }

  if (!user_said_has_content(text_buf))
    {
      /* 空串、或者只有空白/标点（一声异响被 VAD 当成话切出来时很常见）：
       * 当没听见。既不回传界面、也不送大模型，照原来"识别为空"的收尾走 ——
       * 追问流程记一笔空回答，常听链路由 AI_ERROR 退回 IDLE，随后接着听。 */

      printf("[AI] 语音识别结果为空(或只有空白/标点)，当没听见\n");

      if (ask_mode)
        {
          ask_flow_mark_answer(NULL);
        }
      else
        {
          sm_handle_event(ctx, SM_EVENT_AI_ERROR);
        }

      return;
    }

  printf("[AI] 识别结果: %s\n", text_buf);

  /* 用户原话回传界面：全文件唯一的发送点。放在这里（所有分支之前）是因为
   * 下面三条出路 —— 追问判定 / 灯控 / 大模型 —— 都被这一次覆盖：
   *   - 灯控命中那一轮走的也是这一行，所以 handle_light_intent() 里不能再发，
   *     否则老人说"开灯"时界面会把这句显示两遍；
   *   - 追问确认那一轮里老人的回答同样是"用户说的话"，也该上屏，所以这里
   *     没有放在 ask_mode 分支之后。
   * 判定用的还是同一份 text_buf，ask_flow_mark_answer() 拿到的文本没有变化。 */

  publish_user_said(text_buf);

  if (ask_mode)
    {
      ask_flow_mark_answer(text_buf);
      return;
    }

  /* 本机灯控意图：命中就直接做完（device_cmd + 回话 + 播报），
   * 这一轮不再送大模型；拿不准的（否定/歧义）返回 NONE，照原路送大模型。 */

  light_intent = light_intent_classify(text_buf);
  if (light_intent == LIGHT_INTENT_ON || light_intent == LIGHT_INTENT_OFF)
    {
      handle_light_intent(ctx, light_intent);
      return;
    }

  printf("[意图] 灯控未命中，交给大模型\n");

  /* LLM: 发送文字到大模型 */

  if (g_net_started)
    {
      /* 界面上换成"思考中"的表情（回复正文到手时会再发 ai_reply） */
      ai_network_send_face(&g_net_ctx, AI_CMD_FACE_THINKING);
    }

  ret = llm_send_text(&g_llm_ctx, text_buf,
                      NULL, llm_complete_callback, ctx);
  if (ret < 0)
    {
      printf("[AI] 发送LLM请求失败: %d\n", ret);
      sm_handle_event(ctx, SM_EVENT_AI_ERROR);
    }
}

/****************************************************************************
 * 网络回传通道自愈（补连）
 *
 * g_net_started 只在开机 ai_network_start_shared() 成功那一刻置真一次；开机
 * 那一刻 RNDIS / DNS 常常还没就绪（真机日志：`MQTT DNS 解析失败` → `Error 101`），
 * 于是它一直是 false，而 voice_state / user_said / ai_reply / 报警**整排上报**都在
 * `if (!g_net_started) return;` 之后 —— **整场一条都不发**。MQTT 是这台设备唯一的
 * 外部观测通道（robot_ui 的心跳 + `{"action":"diag"}` 就在那条连接上），
 * 所以"发不出去"的后果是：从外面完全看不出 hello_app 是死是活。
 * 下面这段就是给这条路加的自愈：失败之后按 NET_RETRY_INTERVAL_MS 一直重试，
 * 成功即停，并把"只在变化时发"的状态补推一次。
 ****************************************************************************/

/**
 * @brief  网络回传通道补连（main_loop_task 每 100ms 调一次，10 秒才真动手）
 *
 * 重试用的还是 ai_network_start_shared()：它内部先看界面（robot_ui）的
 * network_task 有没有把 MQTT 连起来，连上了就直接复用（最常见的情况 ——
 * 界面自己会重连，我们只是补上"那时我还没起来/还没连上"的记账）；
 * 没人连的时候才自己兜底连一次。所以这条重试既不会开出第二条连接
 * （同 client_id 的两条连接会被 broker 互踢，见 ai_network.c 的说明），
 * 也不用等别人。
 *
 * 成功之后要做的**不只是置标志**：voice_state 那条路是"只在变化时发"的
 * （g_voice_state_sent 去重），开机那次失败之后它已经认为"当前状态发过了"，
 * 不重置的话之后就永远不会补 —— 外面看见的还是"整场没有一条语音状态"。
 * 所以这里把去重键清掉，让下一拍 voice_state_tick() 重推一次当前状态。
 *
 * ⚠️ 本函数跑在主循环线程里，而一次尝试最长要等 3 秒（见
 * AI_MQTT_SHARED_WAIT_MS），所以间隔取 10 秒、失败日志还要节流 ——
 * 这条线程同时管着监听守护，不能被网络试连拖住太久。
 */

static void net_retry_tick(void)
{
  uint32_t now;
  int      ret;

  if (g_net_started || !g_running)
    {
      return;
    }

  now = main_now_ms();

  if (g_net_retry_at == 0)
    {
      /* 开机那一次刚试过（main() 里），先等一个间隔再补。 */

      g_net_retry_at = now + NET_RETRY_INTERVAL_MS;
      return;
    }

  if ((int32_t)(now - g_net_retry_at) < 0)
    {
      return;
    }

  g_net_retry_at = now + NET_RETRY_INTERVAL_MS;
  g_net_retry_count++;

  ret = ai_network_start_shared(&g_net_ctx, g_net_client_id);
  if (ret < 0)
    {
      if (g_net_retry_count <= NET_RETRY_LOG_FIRST ||
          g_net_retry_count % NET_RETRY_LOG_EVERY == 0)
        {
          printf("[网络] 补连回传通道第 %u 次失败: %d（%u ms 后再试；"
                 "本机界面走同进程直调，不受影响）\n",
                 (unsigned)g_net_retry_count, ret,
                 (unsigned)NET_RETRY_INTERVAL_MS);
        }

      return;
    }

  g_net_started = true;

  /* 去重键清掉：让下一拍的 voice_state_tick() 把**当前**状态重推一次
   * （否则它认为"早就发过了"，这一整场都不会再补）。 */

  g_voice_state_sent[0] = '\0';

  printf("[网络] 回传通道补连成功（第 %u 次尝试）：主题 zhi_ai/%s/command，"
         "已补推一次当前语音状态\n",
         (unsigned)g_net_retry_count, ai_network_get_client_id(&g_net_ctx));
}

/**
 * @brief  接管后自检（主循环每拍一次判断，接管后 3 秒那一拍才真打日志）
 *
 * 见 g_takeover_check_at_ms 那段的说明：接管那一刻看不出"设备起没起来"，
 * 要等第一次 read 才知道真相，所以埋在 3 秒后打一行**证据**。
 */

static void takeover_selfcheck_tick(void)
{
  struct sf32lb52_audio_rx_stats_s rxst;
  uint32_t at = g_takeover_check_at_ms;

  if (at == 0 || (int32_t)(app_now_ms() - at) < 0)
    {
      return;
    }

  g_takeover_check_at_ms = 0;

  if (sf32lb52_audio_rx_stats(&rxst) != OK)
    {
      memset(&rxst, 0, sizeof(rxst));
    }

  printf("[启动] 接管后自检（+%u ms）：录音 active=%d idle=%dms lres=%d | "
         "RX irq=%u half=%u armed=%d busy=%d\n",
         (unsigned)APP_TAKEOVER_SELFCHECK_MS,
         audio_record_is_active(&g_audio_ctx) ? 1 : 0,
         audio_record_idle_ms(&g_audio_ctx),
         audio_record_last_result(&g_audio_ctx),
         (unsigned)rxst.irq, (unsigned)rxst.half, rxst.armed, rxst.busy);

  printf("[启动]   └─ 判读：active=1 且 irq/half 在涨 = 设备真起来了；"
         "active=0 / idle 一直涨 / lres=%d（-ENODEV = 设备没在跑）"
         "= 设备没起来 ⇒ 还有别的 fd 占着这台设备 / 共享 status 还钉着，"
         "该走 C2（放干净）或 C3（整机重启）。"
         "（本行打出来时可以再采一次，隔十几秒对比 irq/half 有没有涨。）\n",
         -ENODEV);
}

/**
 * @brief  主循环任务
 */

static void *main_loop_task(void *arg)
{
  sm_context_t *ctx = (sm_context_t *)arg;

  printf("主循环任务启动\n");

  while (g_running)
    {
      /* 0. 心跳（见 g_beat_ms 那段）：写在所有 tick 之前，这一拍就一定是"活过"的 */

      app_beat_ping();

      /* 1. 运行状态机（检查超时等） */

      sm_run(ctx);

      /* 2. 轮询检测各种事件 */

      simulate_wakeup_detection(ctx);
      simulate_alarm_detection(ctx);
      simulate_care_timer(ctx);

      /* 2.5 唤醒词命中处理：录音线程只在回调里置标志，流程在这里推
       *     （没有模板时这个函数每轮读一个 false 就返回） */

      kws_wake_tick(ctx);

      /* 2.6 门控判到人声（录音旁路那条线）：流程也在这里推，
       *     走的是和 VAD 同一条云端语音入口（任务四） */

      gate_voice_tick(ctx);

      /* 3. 异常声追问流程（限时听 / 重问 / 超时都在这里推进） */

      ask_flow_tick(ctx);

      /* 4. 语音状态有变化就告诉界面（检测到声音／录制中 / 正在想 / 正在说 / 空闲） */

      voice_state_tick(ctx);

      /* 5. 监听守护：录音线程死了（驱动 STOP / 5 秒 DMA 超时）就按退避重启，
       *    否则应用会一直跑着但永久听不到声音 */

      listen_supervise_tick(ctx);

      /* 5.2 录音卡死的串口观测（**只打印**，不做任何恢复动作）：
       *     上面那条守护看不见"录音线程卡在 read 里、标志却全健康"的形状
       *     （那种情况录音"活着"），所以另开一条纯观测的路，卡住时每 10 秒
       *     往串口吐一行 idle/wait + 驱动那六个计数（见 rxstuck_watch_tick）。
       *     放在守护之后：守护这一拍要不要动设备与它无关，它一个字节都不改。 */

      rxstuck_watch_tick();

      /* 5.5 网络回传通道自愈：开机没连上（RNDIS/DNS 还没就绪）就每 10 秒补一次，
       *     连上了主动补推一次当前状态 —— 否则整场 MQTT 上报一条都发不出去，
       *     从外面（那是唯一的观测通道）完全看不出 hello_app 是死是活 */

      net_retry_tick();

      /* 5.6 接管后自检（一次性，见 takeover_selfcheck_tick）：接管（--assume-dead
       *     或界面「重置」）之后 3 秒那一拍打一行"设备到底起没起来"的证据，
       *     平时它就是一次判断就返回。 */

      takeover_selfcheck_tick();

      /* 6. 休眠等待 */

      usleep(MAIN_LOOP_INTERVAL_MS * 1000);
    }

  printf("主循环任务退出\n");
  return NULL;
}

/**
 * @brief  状态机调试输出
 */

static void print_system_status(sm_context_t *ctx)
{
  printf("\n========================================\n");
  printf("  智爱陪伴 - AI老人陪伴守护终端\n");
  printf("========================================\n");
  printf("  当前状态: %s\n", sm_get_state_name(ctx->current_state));
  printf("  上一状态: %s\n", sm_get_state_name(ctx->prev_state));
  printf("  最后事件: %s\n", sm_get_event_name(ctx->last_event));
  printf("  状态机状态: %s\n",
         ctx->initialized ? "已初始化" : "未初始化");
  printf("========================================\n\n");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/**
 * @brief  主函数入口
 */

int main(int argc, char *argv[])
{
  int ret;
  pthread_t loop_thread;
  const char *startup_text = NULL;
  const char *mqtt_client_id = NULL;
  bool sound_self_test = false;
  bool ask_self_test = false;
  bool assume_dead = false;             /* --assume-dead：手动接管（拆掉当前实例重来） */
  const char *sounddetect_sub = NULL;   /* 任务四：门控调试子命令 */
  float sounddetect_sens = -1.0f;       /* sensitivity <n> 的可选数值 */

  /* 每次进 main() 先自报一行（2026-09-20 加的诊断，判读见 g_start_seq 的说明）。
   * 放在参数解析**之前**：调试子命令那条"第二次启动"的路也要留下痕迹 ——
   * 真机上要区分"看护线程又拉了一次"和"整机复位过"，就是看这一行的计数与 up=。
   * 纯日志，不改任何行为。 */

  {
    /* up 用 clock_systime_ticks()（开机以来的 tick 数），和 robot_ui 那条
     * `[ui] 慢统计 …, up=%u s` 是**同一个基准**，两条日志能直接对表。 */

    unsigned long up_ms = (unsigned long)TICK2MSEC(clock_systime_ticks());

    g_start_seq++;

    printf("[启动] hello_app 第 %d 次启动 (pid=%d, 系统已运行 %lu.%03lu s)\n",
           g_start_seq, (int)getpid(),
           up_ms / 1000UL, up_ms % 1000UL);
  }

  for (int i = 1; i < argc; i++)
    {
      if (strcmp(argv[i], "--help") == 0)
        {
          print_usage(argv[0]);
          return 0;
        }
      else if (strcmp(argv[i], "--ask") == 0 && i + 1 < argc)
        {
          startup_text = argv[++i];
        }
      else if (strcmp(argv[i], "--client-id") == 0 && i + 1 < argc)
        {
          mqtt_client_id = argv[++i];
        }
      else if (strcmp(argv[i], "--sound-self-test") == 0)
        {
          sound_self_test = true;
        }
      else if (strcmp(argv[i], "--ask-self-test") == 0)
        {
          ask_self_test = true;
        }
      else if (strcmp(argv[i], "--assume-dead") == 0)
        {
          /* 手动接管（2026-09-20 加，晚些时候从"自测入口"改成"唯一的手动恢复入口"）：
           * 把当前跑着的实例拆掉、按全新启动重来。自动接管已删除（见 g_beat_ms 那段），
           * 所以这是**不用断电**把卡住的实例换掉的唯一办法；顺带它也是验这条拆卸路径
           * 本身的手段（在活实例上把设备与会话收掉再重建，是唯一有风险的那段）。 */
          assume_dead = true;
        }
      else if (strcmp(argv[i], "--sounddetect") == 0 && i + 1 < argc)
        {
          sounddetect_sub = argv[++i];

          if (strcmp(sounddetect_sub, "sensitivity") == 0 && i + 1 < argc)
            {
              sounddetect_sens = (float)atof(argv[++i]);
            }
        }
      else
        {
          printf("未知或不完整的参数: %s\n", argv[i]);
          print_usage(argv[0]);
          return -EINVAL;
        }
    }

  /* 重复启动保护（理由见 g_app_inited 那段）。
   * 放在参数解析**之后**：--help 和未知参数在上面已经走完了，这里要拿到的正是
   * 解析出来的 sounddetect_sub。
   *
   * 第二次启动**一个字都不初始化**：不碰音频、网络、MQTT、工具注册、状态机
   * （它们写的都是共享 static，一碰就把正在跑的那个实例带坏），
   * 也不注册信号、不动 g_running —— 那个正在跑的实例靠 g_running 收场，
   * 被这里改成 1 它就退不出去了。
   *
   * 门控那几个量（g_gate_enabled / g_gate_sensitivity / g_gate_stats）都是
   * ai_sound_detect.c 的 file-static，单一大镜像里只有一份，所以这里读到、改到的
   * 就是**那个正在跑的实例**的实时门控状态 —— 这不是巧合，是这条调试入口能成立的
   * 前提（status 读到的是它的真数，on/off/sensitivity 改的也是它）。
   *
   * ★ 2026-09-20 晚：**这里原来那套"心跳停了就自动接管"已删除**（用户拍板：
   *   "没用的东西不要拖累软件"）。理由见上面 g_beat_ms 那段 —— 主循环里
   *   "这一拍很长"和"这一拍卡死"在时间上长得一模一样，任何时间阈值都会在正常但被
   *   网络拖慢的一轮里误判，而误判要把**正在说话的那个实例当场拆掉**。
   *   所以这道门恢复原来的语义：**占着就挡，只放行 --sounddetect**；要主动换掉一个
   *   卡住的实例，用 `ai_companion --assume-dead`（手动，见下面那一段）。
   *
   *   那 2026-09-20 现场那个"每 5 秒拉起一个新实例、每个都被挡回"怎么办？
   *   它的**源头不在这道门，在看护**：看护当时的判据（nxsched_get_tcb 查主线程）
   *   本身就错 —— NuttX 里主线程退出 ≠ 任务组没了。现在看护只看"位子空着"
   *   （见 sifli_ap.c 的 hello_app_watchdog），位子被占着时它一个字都不做，
   *   所以这里不再需要"自己猜那个实例是不是死了"。 */

  if (g_app_inited && !assume_dead)
    {
      printf("[启动] 已经有实例在跑（PID %d），本次只处理调试命令后退出\n",
             (int)getpid());

      if (sounddetect_sub != NULL)
        {
          sound_detect_debug_cmd(sounddetect_sub, sounddetect_sens);
        }
      else
        {
          printf("[启动] 只有 --sounddetect <子命令> 在第二次启动时有意义"
                 "（--ask / 自检 / --client-id 都属于第一次启动的那个实例）\n");
        }

      return 0;
    }

  if (g_app_inited)
    {
      /* 手动接管（`ai_companion --assume-dead`，没有自动接管了 —— 见上面那道门的
       * 注释）。这是"不用断电就能把一个卡住的实例换掉"的唯一入口，顺带也是验这条
       * 拆卸路径本身的手段。走到这里只可能是这个参数。
       *
       * 先把设备从它手里收回来，再按全新启动往下走一遍。
       *
       * **为什么必须先把设备收回来**：它那条半截录音会话还挂在设备上。不收就直接
       * 重开麦，等于同意一台半双工设备上有两个 read 客户端 —— 板级/驱动那两层的
       * 持有者与残留判据（见 sf32lb52_audio_in.h 头上那段）只认"持有线程查不到 TCB
       * 且它的任务组也查不到 TCB"，而这里的情况恰恰是"线程还在、任务组也还在"，
       * 所以谁也抢不过来；真抢过来就是 sem_waitirq.c:137 那条断言
       * （本文件 g_app_inited 那一段记着那次崩溃）。
       *
       * **只收设备，别的什么都不释放**：
       *   - stop_audio_listening() → audio_record_stop()：有界（1 秒上限回收录音
       *     线程）+ **无条件** AUDIOIOC_STOP（2026-09-20 已上板的那一刀），设备那
       *     一层一定被收干净，卡在 read 里的那条线程也会被 STOP 唤醒；
       *   - audio_play_stop()：有界（一块 + 500ms + 有界回收），同样只置标志、不动 fd；
       *   - **不调 audio_deinit / llm_deinit / sm_deinit，也不 free 任何缓冲**：那几个
       *     会 free（audio_deinit 真会 free record_buf/play_buf），而上一代那条卡在
       *     设备调用里的线程**可能还活着、手里还捏着那块缓冲**（DMA 可能正往里写）——
       *     那正是 ai_audio.c 的 audio_deinit 里写着的那条纪律："绝不能 free，线程会
       *     踩到已释放的缓冲，比漏一点内存严重得多"。代价是接管一次漏几百 KB 的堆，
       *     换的是"绝不会踩已释放内存"，这笔账在这个场景下是划算的。
       *     上下文随后由新实例的 sm_init / audio_init / llm_init 自己 memset + 重建
       *     （这三个 init 都是"清零 + 置 initialized"，没有"已经初始化过就报错"的守卫，
       *     所以不清也能接上），上一代那些线程随后会在自己的 loop 里看到被清零的
       *     ctx、按它们本来就有的 stale/收尾路径安静退出。
       *
       * 关怀 / 声音检测那两条线程**只立停止标志、不 join**：它们的 stop
       * （care_stop / sound_detect_stop）里是无上界的 pthread_join，而恢复路径上
       * 不能引入"可能永不返回"的等待（本文件已经栽过一次，见 ai_audio.c 的
       * audio_reap_record_thread 头上那段）。它们只发消息 / 只看数据、不持有设备，
       * 下一拍自查标志就收场。 */

      int age = ai_companion_beat_age_ms();
      int i;

      printf("[启动] 手动接管（--assume-dead）：先请上一个实例收摊，再按全新启动重来"
             "（它已经 %d ms 没有心跳；这个数只是告诉你它当时在干什么）\n", age);

      /* ★ 2026-09-20 晚（真机 ps + dumpstack 实锤 —— 这一步原来漏了）：
       * **先把上一个实例的主循环停下来。**
       *
       * 现场：`ps` 里 group 14 的 main_loop_task（线程 55）还活着，`dumpstack 55`
       * 显示它正在 usleep 里一拍一拍地正常转（不是卡住），而新实例（group 134）
       * 已经起来了。两个实例共享同一批 static（g_listen_wanted / g_audio_ctx /
       * g_sound_ctx …），于是两个"监听守护"同时抢同一台半双工设备：一个 start、
       * 另一个 stop，谁都拿不到一次完整会话 —— 串口里每秒好几轮的
       * `AUDIO_IN: started / stopped / read -ENODEV` 就是它；而旧实例一直握着板级
       * 那份录音会话的**持有者身份**（"会话属于 group 14 / 线程 55"），新实例每次
       * start 只会拿到 -EBUSY。
       *
       * g_running 是共享 static，置 0 = "请所有实例的主循环收摊"：旧 loop 一拍之内
       * 跳出 while，旧 main() 顺着它自己的退出路径把 audio/sm/llm 收干净（那正是
       * 我们要的：它会关掉设备 fd、收掉 care / 声音检测 / 录音线程），最后把
       * g_app_inited 置假 —— 我们就等这一个信号（有界：它那边每一步都是有界等待）。
       * 等到了 = 旧实例彻底退场、设备也放干净了，下面才开始我们自己的初始化。 */

      g_running = 0;

      for (i = 0; i < 250 && g_app_inited; i++)     /* 最多等 ~2.5 秒 */
        {
          usleep(10 * 1000);
        }

      printf("[启动] 上个实例的收尾等待：%d ms%s\n", i * 10,
             g_app_inited ? "（它没来收尾 —— 多半主线程早就不在了，下面自己兜底）"
                          : "（它已退场，设备与各模块都由它收干净了）");

      /* 兜底（幂等，和上面那条路做的是同一件事）：上一个实例的主线程早就没了、
       * 或者它收尾卡住了，那就由我们自己把这几个收掉。
       * "只收设备、什么都不释放"这条纪律不变，理由见下面那段长注释。 */

      g_care_ctx.care_stop = true;
      g_sound_ctx.detect_stop = true;

      stop_audio_listening();
      audio_play_stop(&g_audio_ctx);

      /* 埋一个 3 秒后的自检点（见 takeover_selfcheck_tick）：接管那一刻看不出
       * "设备起没起来"，要等第一次 read 才有真相 —— 那行日志就是给下一次定案用的。 */

      g_takeover_check_at_ms = app_now_ms() + APP_TAKEOVER_SELFCHECK_MS;

      g_app_inited = false;
    }

  g_app_inited = true;

  /* 心跳从"认领位子"这一刻起算：主循环起来之前那 1~3 秒的初始化窗口里，
   * 心跳的年龄就是"刚认领"，看护不会在这段窗口里把新实例当成死的拉第二遍。 */

  g_inited_at_ms = (uint32_t)TICK2MSEC(clock_systime_ticks());
  g_running = 1;

  printf("\n");
  printf("╔══════════════════════════════════════════╗\n");
  printf("║  智爱陪伴 - AI老人陪伴守护终端 v1.0     ║\n");
  printf("║  Powered by openvela                    ║\n");
  printf("╚══════════════════════════════════════════╝\n");
  printf("\n");

  /* **不注册 SIGINT / SIGTERM**（2026-09-19 真机定案，用户拍板）。
   *
   * 为什么去掉：这个 app 是**开机常驻服务**（由 board_late_initialize() 的
   * task_create 起，见 board/contest_board/src/sifli_ap.c，任务名 hello_app）。
   * 而 signal_handler() 里只有一句 `g_running = 0` —— 主循环
   * `while (g_running)` 一假就 `return NULL`，main() 随即 pthread_join 到它，
   * 把 audio / sm / llm 全 deinit 掉，最后 `return 0`，
   * **整个任务组就此消失，而且没有任何人负责把它拉起来**。
   *
   * 真机现象（当晚实测）：`ps` 里 hello_app 连同它的 5 条线程一起不见、
   * 串口里**一条断言都没有**（是优雅退出，不是崩溃）、界面那边只觉得"没反应"
   * —— app 都不在了，自然没人听麦克风，于是表现为"麦克风又坏了"。
   *
   * 全文件只有两处能清 g_running：
   *   1) 这里注册的 SIGINT / SIGTERM（运行中唯一能关掉它的东西）；
   *   2) pthread_create(main_loop_task) 失败（那条会打 [错误] 日志，不是本现象）。
   * 所以从源头断掉第 1 条：常驻服务不该被 Ctrl+C 之类的外部信号整台关掉。
   *
   * 现在要主动停它只有一条路：把 g_running 置 0（目前没有任何代码这么做，
   * 也就是说它设计上就是"一直跑"）。另有一层兜底：sifli_ap.c 里的看护线程
   * 会在它万一消失后把它重新 task_create 起来。 */

  /* 1. 初始化状态机 */

  printf("[初始化] 正在初始化状态机...\n");
  ret = sm_init(&g_sm_ctx);
  if (ret < 0)
    {
      printf("[错误] 状态机初始化失败: %d\n", ret);
      g_app_inited = false;           /* 什么都没跑起来，别占着这个位子 */
      return ret;
    }

  /* 2. 初始化音频模块 */

  printf("[初始化] 正在初始化音频模块...\n");
  ret = audio_init(&g_audio_ctx, NULL);
  if (ret < 0)
    {
      printf("[错误] 音频模块初始化失败: %d\n", ret);
      sm_deinit(&g_sm_ctx);
      g_app_inited = false;           /* 什么都没跑起来，别占着这个位子 */
      return ret;
    }

  /* 3. 初始化LLM模块 */

  printf("[初始化] 正在初始化AI对话模块...\n");
  ret = llm_init(&g_llm_ctx, NULL);
  if (ret < 0)
    {
      printf("[错误] AI对话模块初始化失败: %d\n", ret);
      audio_deinit(&g_audio_ctx);
      sm_deinit(&g_sm_ctx);
      g_app_inited = false;           /* 什么都没跑起来，别占着这个位子 */
      return ret;
    }

  /* 3.5 注册语音 ASR/TTS 后端
   *
   * 两套都注册，然后按配置选：
   *   - 配置里有非空的 api_key + llm_host（开机从 agent_config.json 拷进
   *     /data/ai_agent/config/config.json）-> 用小米 MiMo（mimo_voice.c）；
   *   - 否则回落到火山引擎（volc_asr/volc_tts，需要 volc_api_key 等凭据）。
   * 火山那条路保留不动：队友的凭据以后可能还会用。
   */

  printf("[初始化] 正在注册语音后端...\n");
  mimo_asr_register();
  mimo_tts_register();
  volc_asr_register();
  volc_tts_register();

  if (mimo_voice_available())
    {
      if (voice_asr_set_backend("mimo") == 0
          && voice_tts_set_backend("mimo") == 0)
        {
          printf("[初始化] 语音后端: MiMo (mimo)\n");
        }
      else
        {
          printf("[警告] MiMo 后端注册异常，回落到火山引擎\n");
          voice_asr_set_backend("volcengine");
          voice_tts_set_backend("volcengine");
        }
    }
  else
    {
      printf("[初始化] 语音后端: 火山引擎 (volcengine)"
             "（配置里没有 api_key + llm_host）\n");
      voice_asr_set_backend("volcengine");
      voice_tts_set_backend("volcengine");
    }

  /* 分配语音段累积缓冲区。
   *
   * **没有才分配**（2026-09-20）：接管路径上上一代这块还留着，而这里**不许 free**
   * —— 上一代可能还有线程（录音回调那条）在用同一块缓冲，free 掉就是让它踩已释放
   * 内存，那比漏一点内存严重得多（同一条纪律见 ai_audio.c 的 audio_deinit）。
   * 容量是常量、两个实例一样大，直接接着用就是对的。 */

  if (g_speech_buf == NULL)
    {
      g_speech_buf = malloc(SPEECH_BUF_MAX_FRAMES * sizeof(int16_t));
    }

  if (g_speech_buf == NULL)
    {
      printf("[警告] 语音缓冲区分配失败\n");
    }

  /* 3.6 初始化唤醒词模块（「你好，openvela」/「Hello，openvela」）
   *
   * 必须在 start_audio_listening() 之前：录音线程一起来就会开始喂帧，
   * 而 kws_dtw 要求它的接口只被一个线程调（喂帧的那个），所以建表、
   * 读模板都得在录音线程存在之前做完。
   *
   * kws_init() 自己会先去 ROMFS 的 /etc/assets/kws/ 把出厂模板补进 /data/kws，
   * 再把 /data/kws/slotN.tpl 读回来（文件名是模块里定死的 KWS_DATA_DIR +
   * "slot%d.tpl"，路径不用我们拼）；它是幂等的：建表只做一次，
   * 重复调用只是重新装/读一遍模板文件。
   *
   * ⚠ /data 在本板是 tmpfs，重启就空 —— 没有模板的情况下 kws_feed
   *   永远返回 0，我们把 g_kws_enabled 置 false 后**连喂都不喂**，
   *   整条链路就退回"只靠 VAD 触发"，和接线之前一模一样。
   *   模板怎么来：① 固件里带（把 slotN.tpl 放进 etc/assets/kws/ 一起打包，
   *   开机自动装，这是"重启后还有"的唯一途径）；② 现场录（kws_dtw.h 的
   *   kws_enroll()，只在本次开机有效，见 kws_update_enabled）。 */

  printf("[初始化] 正在初始化唤醒词模块...\n");
  ret = kws_init();
  if (ret < 0)
    {
      printf("[警告] 唤醒词模块初始化失败: %d（唤醒词不可用，本地语音链路照常）\n",
             ret);
    }

  g_kws_ready = kws_ready_count();
  g_kws_enabled = (g_kws_ready > 0);

  if (g_kws_enabled)
    {
      printf("[KWS] 唤醒词就绪：%d 条模板（词：你好，openvela / Hello，openvela）\n",
             g_kws_ready);
    }
  else
    {
      printf("[KWS] 没有模板（%s 是空的，/data 是 tmpfs 重启就丢）→ "
             "唤醒词功能未启用，只靠 VAD 触发，行为与以前一致"
             "（运行中录到模板会自动启用；想开机即用就把 slotN.tpl 放进固件里"
             "的 /etc/assets/kws/）\n",
             KWS_DATA_DIR);
    }

  /* 设置 AI 对话回调给状态机 (通过 user_data) */

  sm_set_user_data(&g_sm_ctx, (void *)process_ai_dialogue);

  /* 4. 初始化声音检测模块 */

  printf("[初始化] 正在初始化声音检测模块...\n");
  ret = start_sound_detection(&g_sm_ctx);
  if (ret < 0)
    {
      printf("[警告] 声音检测初始化失败: %d\n", ret);
      /* 声音检测失败不退出，继续运行 */
    }
  else if (sounddetect_sub != NULL)
    {
      /* 任务四：`--sounddetect status|on|off|sensitivity <n>` 在这里生效
       * （门控默认 on，所以 off/sensitivity 必须放在它起来之后）。
       * 门控线程是异步的，status 打印的是"这一刻"的统计，可能还是 0；
       * 想看有数的 status 就开机后等一会儿重跑一次（或看正常日志）。 */

      sound_detect_debug_cmd(sounddetect_sub, sounddetect_sens);
    }

  /* 5. 初始化主动关怀模块 */

  printf("[初始化] 正在初始化主动关怀模块...\n");
  ret = start_care(&g_sm_ctx);
  if (ret < 0)
    {
      printf("[警告] 主动关怀初始化失败: %d\n", ret);
      /* 关怀失败不退出，继续运行 */
    }

  /* 6. 初始化网络模块（**给手机/PC 端的旁路**：AI 回复正文 / 用户原话 /
   *    状态 / 表情 / 报警页从这里 publish 到 zhi_ai/<client_id>/command）
   *
   * 本机界面已经走同进程直调（robot_ui_bridge.h）了，所以这一步失败**不再**
   * 等于"界面没反应" —— 它只影响远端。原来界面反馈全押在这条路上，而开机时
   * RNDIS/DNS 常常还没就绪（真机日志：`MQTT DNS 解析失败` -> `Error 101` ->
   * 这里打"网络初始化失败"），于是 g_net_started 一直是 false、界面整场不动。
   *
   * 不走 ai_network_init()：那个入口已经删掉了（它会另起一份 network_comm 状态，
   * 还会把唯一的 MQTT 收包回调槽抢过来，界面从此收不到 ai_reply）。
   * network_comm.c 在一个固件里只有一份实例，界面（robot_ui）的 network_task
   * 已经拥有那条 MQTT socket，hello_app 只能借：ai_network_start_shared() 只
   * 复用那条连接（没人连的时候才兜底连一次），理由写在 ai_network.c 里。 */

  printf("[初始化] 正在初始化网络模块...\n");

  /* 记下 client_id 给主循环的补连用（net_retry_tick）：argv 里的字符串在整个
   * 进程生命周期内都有效，存指针就够。这里是唯一一处拿到它的地方。 */

  g_net_client_id = mqtt_client_id;

  ret = ai_network_start_shared(&g_net_ctx, mqtt_client_id);
  if (ret < 0)
    {
      printf("[警告] 网络初始化失败: %d（手机/PC 端收不到回传，本机界面走直调"
             "不受影响）\n", ret);
      printf("[网络] 回传通道没起来：主循环会每 %u ms 补连一次，连上自动补推状态\n",
             (unsigned)NET_RETRY_INTERVAL_MS);
    }
  else
    {
      g_net_started = true;
      printf("[网络] 回传主题: zhi_ai/%s/command\n",
             ai_network_get_client_id(&g_net_ctx));
    }

  /* 7. 把「我们自己的工具」挂进框架的工具表
   *
   * 走 tool_registry_register_provider() 这条**包外注册**的公开接口：
   * 框架的工具表长在 ai_agent 里，但它是 CONFIG_BUILD_FLAT 的单地址空间，
   * 我们可以在自己进程里把 provider 注册进去，不用改 ai_agent 源码、不用出补丁。
   *
   * ⚠️ 模型"看得见"这些工具的前提是 **ai_agent 那个 app 已经在跑**
   * （本固件没有开机自启，要手工敲 ai_agent）。这跟 llm_send_text() 能不能用
   * 是同一个前提，不是新增的依赖。注册动作本身不依赖它：tool_registry_init()
   * 不清 provider 数组，先注册后启动也照样带上。
   *
   * 传的是 start_shared 的 ctx：那条 MQTT 连接归界面（robot_ui）所有，
   * 工具发 device_cmd 就是借它发。 */

  ai_tools_provider_set_network_ctx(&g_net_ctx);

  ret = ai_tools_provider_init();
  if (ret < 0)
    {
      printf("[工具] 注册到框架工具表失败: %d（模型看不到我们的工具，"
             "本地语音链路不受影响）\n", ret);
    }
  else
    {
      printf("[工具] 已注册到框架工具表: %s\n", AI_TOOL_SET_LIGHT);
    }

  /* 打印初始状态 */

  print_system_status(&g_sm_ctx);

  /* 5. 启动语音监听 */

  printf("[启动] 正在启动语音监听...\n");

  /* 先把"要常听"的意图立起来再开麦：这一次就是失败（设备忙 / 驱动没起来），
   * 主循环里的监听守护也会照着重试，而不是让应用安静地聋到下次重启。 */

  g_listen_wanted = true;

  ret = start_audio_listening(&g_sm_ctx);
  if (ret < 0)
    {
      printf("[警告] 启动语音监听失败: %d（监听守护会重试）\n", ret);
    }

  /* 2. 启动主循环任务 */

  printf("[启动] 正在启动主循环任务...\n");
  ret = pthread_create(&loop_thread, NULL, main_loop_task, &g_sm_ctx);
  if (ret != 0)
    {
      printf("[错误] 主循环任务创建失败: %d\n", ret);

      g_running = 0;
      stop_audio_listening();
      stop_care();
      stop_sound_detection();
      llm_deinit(&g_llm_ctx);
      audio_deinit(&g_audio_ctx);
      sm_deinit(&g_sm_ctx);

      /* 这次启动已经彻底收场，把"已经在跑"标记放掉，
       * 之后 NSH 里还能重新起一个实例。 */

      g_app_inited = false;
      return -ret;
    }

  printf("[运行] 系统已启动, 按 Ctrl+C 退出\n\n");

  if (startup_text != NULL)
    {
      ret = llm_send_text(&g_llm_ctx, startup_text, NULL,
                          llm_complete_callback, &g_sm_ctx);
      if (ret < 0)
        {
          printf("[LLM] 启动请求失败: %d\n", ret);
        }
    }

  if (sound_self_test && g_sound_started)
    {
      int16_t *test_audio = calloc(SOUND_DETECT_FRAMES_PER_WINDOW,
                                   sizeof(*test_audio));
      if (test_audio == NULL)
        {
          printf("[自检] 内存不足，跳过测试\n");
        }
      else
        {
      for (size_t i = 0; i < SOUND_DETECT_FRAMES_PER_WINDOW; i += 80)
        {
          test_audio[i] = (i / 80) % 2 == 0 ? INT16_MAX : INT16_MIN;
        }

      ret = sound_detect_feed(&g_sound_ctx, test_audio,
                              SOUND_DETECT_FRAMES_PER_WINDOW);
      printf("[自检] 声音检测测试数据已注入: %s (%d)\n",
             ret == OK ? "成功" : "失败", ret);
          free(test_audio);
        }
    }

  if (ask_self_test && g_sound_started)
    {
      /* 真机上不摔一跤也能验证整条追问链路：
       *   NSH 里 `ai_companion --ask-self-test`
       * 灌一条假的"异常声"进去，追问流程会自己从主循环里跑起来
       * （问一句 → 限时听 6 秒 → 按回答判定/重问）。 */
      ret = sound_detect_report_anomaly(&g_sound_ctx, SOUND_TYPE_KNOCK, 0.9f);
      printf("[自检] 已注入异常声(KNOCK 0.9): %s (%d)\n",
             ret == OK ? "成功" : "失败", ret);
    }

  /* 3. 主线程等待退出 */

  while (g_running)
    {
      sleep(1);
    }

  /* 4. 清理退出 */

  printf("\n[退出] 正在停止系统...\n");

  /* 停止主动关怀 */

  stop_care();

  /* 先等待录音线程退出，确保不会再调用 sound_detect_feed。 */

  stop_audio_listening();

  /* 然后释放声音检测器。 */

  stop_sound_detection();

  /* 等待主循环任务退出 */

  pthread_join(loop_thread, NULL);

  /* 反初始化LLM模块 */

  llm_deinit(&g_llm_ctx);

  /* 释放语音缓冲区 */

  free(g_speech_buf);
  g_speech_buf = NULL;

  /* 反初始化音频模块 */

  audio_deinit(&g_audio_ctx);

  /* 反初始化状态机 */

  sm_deinit(&g_sm_ctx);

  /* 网络：只关掉自己这一侧的上下文。
   * 那条 MQTT 连接是界面（robot_ui）的 network_task 建的、由它重连/收包，
   * 这里不能去断它 —— 断了界面也跟着掉线。 */

  g_net_started = false;
  memset(&g_net_ctx, 0, sizeof(g_net_ctx));

  printf("[退出] 系统已安全退出\n\n");

  /* 全部收干净了才放掉这个标记：半路放掉就等于"收尾还没完就允许第二个实例
   * 进来初始化"，那正是这次要堵死的那条路。 */

  g_app_inited = false;

  return 0;
}
