/**
 * ai_companion_req.c - 跨 app 请求的薄壳：只登记请求，一个设备都不碰
 *
 * 装的是两个异步请求（都从这里受理、真动作都归 hello_app 自己的线程）：
 *   - 「提交」（g_voice_submit_req，**一次性**语义：镜像面板点一下 = "我说完了，
 *     立刻把这一段送去识别"，录音线程每帧认领一次）。收尾动作本身在
 *     ai_companion_main.c 的 audio_data_callback() 里，走的是和静音超时同一段代码；
 *   - 「这条语音追问立刻收摊」（g_ask_abort_req，**一次性**语义：报警真的走起来了，
 *     另一条确认路就该停下），认领方是 ai_companion_main.c 的 ask_flow_tick()。
 *
 * ★ 这里原来还装着第三、第四个请求 —— 请 hello_app 交出常开麦的「让路 /
 *   收回」（g_mic_req 电平 + g_mic_req_seq 序号）。**那一套 2026-09-21 整套删掉了**，
 *   连同它的登记接口（原来的 ai_companion_yield.c/.h 整文件删除，换成本文件）。
 *   决定、真机故障形状和它为什么是纯负担，写在 ai_companion_req.h 头上。
 *
 * ★ 这个文件里**不许**出现任何 audio_* / audio_in_* / ioctl / close /
 *   pthread_join —— 一条都没有。理由就是当年那次真机事故：跨 app（另一个 task
 *   group）替 hello_app 停设备，一旦不按预期收敛（AUDIOIOC_STOP 报 ENOTTY、
 *   录音线程 300ms 没退出、join 被放弃），留下的就是"录音线程还阻塞在 read() 里、
 *   fd 状态不明"的残局，紧接着在同一份驱动状态上起播放 hw_start，整个 hello_app
 *   组就死了。所以这里只置标志位，真动作全在 hello_app 自己的线程里。
 *
 * 为什么不需要锁：这两个量各是一个布尔位，写者写、认领者读走就清，32 位对齐的
 * 读写在 Cortex-M 上是原子的（和以前那个 g_mic_req 一个路子）。为它们引入一把
 * （可能还是跨 app 的）锁，只会把"设备动作权"又散到调用方那边去。
 */

#include <nuttx/config.h>

#include "ai_companion_req.h"

/* 「提交」请求标志（一次性：置一次、被认领一次就清）。
 * 语义差别写在头文件里：提交是**按钮动作**（点一下一次），不是持续有效的方向。
 *   - 写者：任何线程（现在只有 robot_ui 的 LVGL 线程：镜像面板点「提交」，
 *     经 main.c 转发 → ai_companion_voice_submit() → 本文件的写函数）；
 *   - 认领者：hello_app 那条录音线程（每帧一次，见 main.c 的数据回调）。 */
static volatile bool g_voice_submit_req;

void ai_companion_voice_submit_request(void)
{
  /* 只置位，立刻返回：不判有没有在录音、不等任何人、不碰设备。
   * 该不该登记由调用方（ai_companion_voice_submit）先判过状态快照 —— 那里
   * 才看得到 hello_app 的 g_speech_capturing / g_speech_frames。 */

  g_voice_submit_req = true;
}

bool ai_companion_voice_submit_take(void)
{
  /* 认领即清：一次按钮动作最多被处理一次（理由见头文件里那个入口的说明）。
   *
   * 为什么"读走就清"是安全的：唯一认领者是录音线程那一帧里的一段顺序代码，
   * 而登记请求的前置条件（ai_companion_voice_submit() 里判）是"此刻真的在常听、
   * 而且正在累积够长的一段语音"。所以这个位最多活到**下一帧**（≤ 20ms）就被
   * 认领掉；万一这一小段里麦克风正好被停了（放音时的半双工停录），请求会留到
   * 录音恢复后的第一帧 —— 那一刻上一次的累积已经被清场清掉了（录音链路重开时都会
   * 清 g_speech_capturing / g_speech_frames），而一句新的话要先攒够 300ms 语音
   * 才会被 VAD 认成"开始说话"，所以那一帧认领到的请求只会被丢掉（见
   * audio_data_callback 里的判据），不会拦腰截断老人下一句话。 */

  bool req = g_voice_submit_req;

  g_voice_submit_req = false;
  return req;
}

/* ---------------- 这条语音追问立刻收摊请求 ----------------
 *
 * 谁登记：robot_ui（LVGL 线程，报警真的走起来的那一刻）。
 * 谁认领：hello_app 的主循环（ai_companion_main.c 的 ask_flow_tick()）。
 * 语义是**一次性**的（和上面的 submit 一样：置一次、认领一次就清），因为它
 * 是对这一次异常别再问了这一个动作，不是一个持续的方向。
 *
 * 为什么要它（现场形状）：屏幕上的「是否报警？」询问页和 hello_app 的语音追问
 * 是同一件事的两条确认路，两边都以为自己是唯一那个在问的人。用户在屏幕上
 * 点了「是的，报警」、报警真的响起来之后，语音追问那一套还在按自己的节奏问
 * 第二轮（大字「检测到声音」+ 聊天气泡），而且它下一轮还会再问 —— 屏幕上于是
 * 同时挂着两套确认，报警声和追问的 TTS 还抢同一台半双工音频设备。报警一旦
 * 执行，另一条确认路就该停下：这就是这条请求存在的全部理由。
 *
 * 这里照旧**不碰任何设备、不碰任何界面**（只置一个标志位），真正的收摊在
 * ai_companion_main.c 的 ask_flow_tick() 里：相位只有一个写者那条纪律不变，
 * 别人只登记，主循环认领。
 */

static volatile bool g_ask_abort_req;

void ai_companion_ask_abort(void)
{
  g_ask_abort_req = true;
}

bool ai_companion_ask_abort_take(void)
{
  bool req = g_ask_abort_req;

  g_ask_abort_req = false;
  return req;
}
