/**
 * ai_companion_req.h - 请 hello_app 干一件事的跨 app 接口（非阻塞）
 *
 * ★ 2026-09-21：原来的 ai_companion_yield.h/.c 就长在这里，它装的第一个（也是
 *   最主要的一个）请求是「让路」—— robot_ui 播提醒/报警前先请 hello_app 交出
 *   **常开麦**，播完再 RECLAIM 收回。那条协议已经**整套删掉**了（文件和实现都删了，
 *   不是关掉），原因和决定写在这里，免得以后有人再把它捡回来：
 *
 *   用户拍板（2026-09-21 原话）：「关闭让路，聊天界面只保留提交。不需要做麦克风
 *   转让，因为我们的麦是常开的」。
 *
 *   真机上的形状（诊断快照原文）：
 *     ract:0  req:1  hold:0  want:1  wait:490645  rxr 冻住  sm:IDLE
 *   也就是**让路请求挂着、麦克风交出去了、收回请求丢了，麦克风再也不回来**；
 *   而"监听守护"原来的判据要求 ract==1（录音链路活着）才判，ract=0 时按设计
 *   当成"合法的让路中"故意不判 —— 两条守护都盖不到这个死角，只能重启板子。
 *
 *   "谁的设备谁动"这条原则本身没错（跨 task group 去停别人的 fd / join 别人的
 *   线程，正是 2026-09-14 那次 hello_app 整组死掉的成因），错的是**协议本身**：
 *   一个"整机一份、没有持有者概念"的方向电平，靠调用方自己守配对纪律
 *   （yield 之后到 reclaim 之间不许有任何 return），漏一次就是永久聋。
 *   而麦克风本来就不需要转让 —— 半双工是硬件事实，放音时驱动自己会停录音、
 *   放完 hello_app 自己会把录音重开（ai_audio.c 的 audio_prepare_output /
 *   audio_resume_record，见那边）。所以这个握手是纯负担，删掉。
 *
 *   删掉之后 hello_app 这边的收口（都写在 ai_companion_main.c 里）：
 *     - 麦克风**纯常开**：开机 start_audio_listening() 一次，录音线程的生命周期
 *       只由 hello_app 自己管；
 *     - 放音时停录、放完自动恢复：完全在 ai_audio.c 内部（audio_play_start →
 *       audio_prepare_output → audio_record_stop；播放线程收尾 → audio_resume_record），
 *       一个跨 app 的请求都不需要；
 *     - 监听守护的判据补上了原来盖不到的那个形状（g_listen_wanted 立着却长时间
 *       没在录音 = 异常，先停掉那半截会话再重开，见 LISTEN_SUPERVISE_WANT_MUTE_MS）。
 *
 * ★ 本文件留下来的两个请求和让路**没有关系**，只是当初和它共用了"只登记请求、
 *   真动作在 hello_app 自己线程里做"这一套写法：
 *
 *   1) 「提交」（ai_companion_voice_submit）—— 语音聊天**镜像面板**底部那个按钮，
 *      点一下 = "我说完了，立刻把当前这段录音送去识别"，不等 VAD 的静音超时（3 秒）。
 *      收尾走的是和"VAD 判静音超时"**同一段**代码（ai_companion_main.c 的
 *      speech_capture_complete()），所以"语音结束"的语义只有一份。
 *
 *   2) 「这条语音追问立刻收摊」（ai_companion_ask_abort）—— 屏幕上那条确认路
 *      （或者 MQTT 下行）已经报警了，报警声正在响，hello_app 的语音追问那一套
 *      就别再问第二轮了（两边都在问的时候，报警声和追问的 TTS 会抢同一台半双工
 *      音频设备，屏幕上还挂着两套确认）。
 *
 * 为什么单独一个文件：robot_ui 的 main.c / robot_ui.c 要 include 它，而 hello_app
 * 这边的音频状态（g_audio_ctx / g_listen_wanted / g_sm_ctx）全是 ai_companion_main.c
 * 的 static。那个 main.c 连同它的依赖（mimo_voice / ai_tools_provider …）都带不进
 * robot_ui 的编译单元，所以这个头文件必须**自给自足**：只依赖 <stdbool.h>，
 * 不引 LVGL、不引 robot_ui、不引 hello_app 的任何别的头文件（和
 * app/robot_ui/robot_ui_bridge.h 是同一套约束）。
 * 薄壳（只登记请求，一个设备都不碰）在本目录的 ai_companion_req.c，真动作在
 * ai_companion_main.c。
 *
 * ⚠️ 三条约束（调用方必须守）：
 *   1) **非阻塞，立刻返回**：这两条都只置一个标志位 / 读一次状态快照，不碰设备、
 *      不碰界面、不发网络请求，任何线程可调；
 *   2) **一次性的动作不能留在那里等以后有人认领**：登记请求之前调用方要先判
 *      "这一刻真的成立"，否则几秒前那一按会拦腰截断下一句话；
 *   3) **hello_app 没在跑时安全空转**：整机是单一大镜像（CONFIG_BUILD_FLAT），
 *      符号在最终链接时解析，robot_ui 完全可能比 ai_companion 先起来。
 */

#ifndef __AI_COMPANION_REQ_H
#define __AI_COMPANION_REQ_H

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * 「提交」：请 ai_companion 立刻收尾当前这一段录音（非阻塞）
 *
 * 界面上的出处：语音聊天的**镜像面板**底部那个大按钮。语义是"我说完了，立刻把
 * 这段录音送去识别"，不等 VAD 的静音超时（AUDIO_VAD_SILENCE_TIMEOUT_MS = 3 秒）
 * 自己收尾 —— 老人说完话不用再干等 3 秒。
 *
 * 关闭仍然在面板右上角的「×」那一条路上，和这个请求无关。
 ****************************************************************************/

/* 提交请求的受理结果（ai_companion_voice_submit() 的返回值） */
typedef enum
{
  AI_COMPANION_SUBMIT_NONE = 0,   /* 没在累积语音：什么都没登记，调用方可以提示"没听到" */
  AI_COMPANION_SUBMIT_ACCEPTED,   /* 请求已登记：hello_app 下一帧就会把这一段送去识别 */
  AI_COMPANION_SUBMIT_BUSY        /* 它正忙（送识别 / 等大模型 / 出声 / 追问流程）：
                                   * 这一下不该插一脚，调用方也**不要**提示"没听到" ——
                                   * 界面上本来就有"正在想…/正在说话…" */
} ai_companion_submit_result_t;

/**
 * @brief  请 ai_companion 立刻收尾当前这一段录音。**非阻塞、任何线程可调。**
 *
 * @return 见 ai_companion_submit_result_t
 *
 * 它只做两件事：**读一次** hello_app 的语音状态（快照，只读，一个设备都不碰），
 * 有可提交的语音时**登记一个请求标志**（一次性：置一次、被认领一次就清）。
 * 真正"把这一段结束掉"的动作在 hello_app 自己那条**录音线程**上做 ——
 * 见 ai_companion_main.c 的 audio_data_callback() 里那段（为什么必须是那条线程：
 * VAD 判"说完了"本来就在那条线程里，用同一条线程走同一段收尾代码，才不会出现
 * "两条路同时收尾"；别的线程去触发会把整段 ASR 拽到那条线程上阻塞几十秒）。
 *
 * 三条纪律：
 *   1) 非阻塞，登记完立刻返回；下一次录音帧（≤ 20ms）就会被处理掉；
 *   2) hello_app 没在跑时安全空转：音频没初始化 / 没在常听一律报 NONE，
 *      **并且不登记请求** —— 一次性动作不能留在那里等"以后有人认领"，那会变成
 *      "老人几秒前那一按把后面的句子拦腰截断"；
 *   3) 调用方拿到 ACCEPTED 只是"请求登记上了"，别把它当成"识别结果马上就来"：
 *      结果照旧由 voice_state / user_said 那条链路推（robot_ui_bridge）。
 */
int ai_companion_voice_submit(void);

/****************************************************************************
 * 这条语音追问立刻收摊：报警一旦真的走起来，另一条确认路就该停下（非阻塞）
 *
 * 登记方：robot_ui（robot_ui_show_alarm() 的入口处，任何报警来源都覆盖）。
 * 认领方：ai_companion_main.c 的 ask_flow_tick()（主循环，每 100ms 一拍）。
 *
 * 和「提交」一样的纪律：只置标志，一个设备都不碰、一页界面都不碰；真正的收摊由
 * hello_app 自己的线程做（相位只有一个写者）。
 * 一次性语义：认领即清，一次报警只会让这一次追问收摊，不会波及下一次。
 ****************************************************************************/

void ai_companion_ask_abort(void);

/****************************************************************************
 * 内部实现（薄壳在 ai_companion_req.c，真动作在 ai_companion_main.c）
 *
 * 为什么在 main.c 里留一个薄门面，而不是把这些 static 放开可见性：
 *   g_speech_capturing / g_speech_frames / KWS 流式状态**什么时候清才安全**、
 *   以及该不该动状态机 —— 这些约束的解释都写在 main.c 那几段注释里。放开可见性
 *   等于把"谁有权改、什么时候能改"散到两个文件，外面乱调就没人拦得住。
 * （app/robot_ui/robot_ui_bridge.h 的"内部桥接原语"是同一套做法。）
 ****************************************************************************/

/**
 * @brief  登记一次「提交」请求（一次性：置一次、被认领一次就清）
 *
 * 只给 ai_companion_main.c 的 ai_companion_voice_submit() 用（它先读状态快照，
 * 确认"真的在累积一段语音"才登记）。
 *
 * 为什么是"一次性"：提交是**按钮动作**（点一下发生一次），不是持续有效的方向。
 * 请求一直挂着的话，下一次录音帧（可能是几十秒后另一句话）会被当成"这一下要
 * 立刻收尾"，把新句子拦腰截断。
 */
void ai_companion_voice_submit_request(void);

/**
 * @brief  认领一次「提交」请求（读走就清，返回"有没有人请求过"）
 *
 * 只给 ai_companion_main.c 的 audio_data_callback() 用（录音线程，每帧一次）。
 * 认领即清，所以一次按钮动作最多被处理一次。
 */
bool ai_companion_voice_submit_take(void);

/**
 * @brief  认领一次追问收摊请求（读走就清）。只给 ask_flow_tick() 用。
 */
bool ai_companion_ask_abort_take(void);

#endif /* __AI_COMPANION_REQ_H */
