/**
 * ai_companion_diag.h - hello_app 语音链路的只读快照（robot_ui 的诊断通道）
 *
 * 背景（2026-09-14，串口线丢了之后）：
 *   MQTT 是唯一还活着的观测通道，可 hello_app **自己**的上报被 g_net_started
 *   钉死 —— 语音链路哑掉的时候它一个字都发不出来，从外面完全看不出它死没死。
 *   robot_ui 的 network_task 是活的（心跳每 30 秒一条，已经证明可靠），
 *   所以由它替 hello_app 说话：心跳里放一个 ha 位（本接口在不在），
 *   {"action":"diag"} 里把整份快照原样内嵌。
 *
 * 契约（app/robot_ui/network_comm.c 按这个用，改这边要连着那边一起想）：
 *   - 返回 >= 0：写进 buf 的字节数（**不含**结尾 '\0'，但结尾一定写了 '\0'）。
 *     内容是一行**合法的 JSON 对象**，调用方 cJSON_Parse 之后原样内嵌；
 *   - 返回 < 0：取不到快照。就两种情况，别再多编：
 *       (1) hello_app 没在跑 —— 音频模块没初始化过（它还没起来 / 已经在退场。
 *           注意 main() 里 audio_init() 失败就直接返回了，所以"音频没初始化"
 *           和"它不在"是同一件事，不会出现"在跑但音频没起来"）；
 *       (2) buf 或 len 根本没法写（NULL / 小于 8 字节，连一个空对象都放不下）。
 *     调用方把负值原样报成 "unavailable" —— 那正是现场最缺的那条证据；
 *   - 缓冲小就**截断**：返回实际写进去的长度、内容仍然是合法 JSON（少几个
 *     字段、并在尾巴上附一个 "trunc":1 说明这份不完整；小到连那句说明都放
 *     不下时只剩一个空对象 "{}"），绝不因为缓冲小就报负值 —— 心跳那个探针
 *     只给 128 字节、只看返回值的正负，把"它还在跑"误报成"接口取不到"比
 *     不报还糟（见 network_comm.c 的 hello_diag_available 里那段说明）。
 *
 * 三条纪律（和 ai_companion_req.h 是同一套）：
 *   1) **任何线程可调，纯只读**：不碰音频设备、不加锁、不等任何人、不发网络
 *      请求。调用方是 robot_ui 的 network_task（MQTT 收包线程），它一阻塞，
 *      心跳 / 重连 / 收包就一起停摆 —— 那才叫"连唯一的观测通道也没了"；
 *   2) **hello_app 没在跑时安全**：整机是单一大镜像（CONFIG_BUILD_FLAT），
 *      robot_ui 完全可能比 ai_companion 先起来，那时这些 static 还是初值；
 *      本接口照常可调，只是按上面那条返回负值（一个设备都不碰）；
 *   3) **自给自足**：robot_ui 会 include 本文件（它拿不到 hello_app 的编译
 *      依赖），所以不许引 LVGL、不许引 robot_ui 或 hello_app 里别的头文件，
 *      只用 <stddef.h> 这类标准头。
 *
 * JSON 字段（短名字；值是**整数** 1 = 是 / 0 = 否 / -1 = 取不到，和
 * network_comm.h 里 local_state_t 那套口径一致；只有 sm 是字符串）：
 *   alive  hello_app 在跑。恒为 1 —— 它是"这份快照存在"的同义反复
 *          （不可用的时候返回的是负值、整块都不存在），留它是为了让
 *          network_comm.h 里 "hello":{"alive":true,...} 那个形状对得上
 *   audio  音频模块初始化过（g_audio_ctx.initialized）
 *   start  "要常听"的意图立着、而且麦已经开起来了（g_audio_started）
 *   rec    音频层认为在录音          ract  录音链路真的活着（录音线程还在跑）
 *   died   录音异常中断（不是谁让它停的）   exit  录音线程已经收尾
 *   idle   距最近一次读到音频数据多少毫秒（-1 = 从来没有参照点）
 *   lres   最近一次 read 的结果（0 = 拿到数据，负值 = 错误码，见 ai_audio.h）
 *   wait   此刻"等设备给数据"等了多久（-1 = 没人在等）
 *   empty  连续空读次数
 *   want   我要常听的意图（和 start 不是一回事：开机第一次开麦失败时 want 真
 *          而 start 假；★ 让路协议 2026-09-21 删除之后，want=1 配 ract=0 就是
 *          异常 —— 监听守护会把它当"会话已死"救回来，见 ai_companion_main.c
 *          的 LISTEN_SUPERVISE_WANT_MUTE_MS）
 *   net    MQTT 上报通道可用（g_net_started）
 *   nrt    网络回传通道补连尝试次数（0 = 开机就连上了，没有自愈动作）
 *         （2026-09-21 删掉的键：hold / busy / req / wd / wdead —— 全是"让路 /
 *           麦克风转让"那套握手的状态，协议本身已经整套删除，见
 *           ai_companion_req.h 头上。报文里少这五个键是**有意**的。）
 *   sm     状态机当前状态（字符串，见 ai_state_machine.h 的 sm_state_t）
 *   cap    正在累积一段语音            sp    这一段已经累积了多少帧
 *   kws    载入的唤醒词模板条数（0 = 唤醒词功能没开）
 *   vad    最近一次 VAD 的时间戳（现在恒为 -1：ai_companion_main.c 里没有记，
 *          到时候那边记了、这个字段自己就活了）
 *   asr    最近一次 ASR 的时间戳（同上）
 *   rxi    驱动里 RX 完成中断（RxCplt）次数
 *   rxh    驱动里 RX 半满中断（RxHalfCplt）次数
 *   rxr    驱动里 read() 进入次数
 *   rxt    驱动里 read() 等待超时次数（那次 5 秒预算用光一次算一次）
 *   rxe    驱动里 DMA 传输错误(TE) 中断次数（这一类 HAL 会自己拆通道，不自愈）
 *   rxl    恒为 0（驱动没有"环里来不及取、被 DMA 覆盖"这回事，字段留着不动上层）
 *          （2026-09-16 加这六个，全部来自 sf32lb52_audio_rx_stats()。它们和
 *           录音线程自己的 idle/wait 配起来看才分得出"卡在无界等待"和
 *           "DMA 交帧那一半丢了"：read/超时在涨而 rxi/rxh 不涨 = 请求线或 ADC
 *           死了；rxi/rxh 还在涨 = 数据在流，是等待/唤醒那一侧的问题；
 *           rxe 在涨 = TE；rxl 在涨 = 上层自己处理不过来。驱动还没初始化时
 *           这六个键根本不写进来，本文件按"取不到"输出 -1。
 *           ⚠️ 它们只是计数，**不触发任何恢复动作** —— 恢复是录音链路自己的事。）
 *
 * 实现拆两半（照 ai_companion_req.c / .h 的做法）：
 *   本目录的 ai_companion_diag.c —— 拼 JSON，不碰 hello_app 的任何 static；
 *   ai_companion_main.c 的 ai_companion_state_snapshot_impl() —— 唯一能读到
 *   那些 static 的地方，按下面那个内部格式把状态交出来。
 */

#ifndef __AI_COMPANION_DIAG_H
#define __AI_COMPANION_DIAG_H

#include <stddef.h>

/**
 * @brief  把 hello_app 的语音链路关键内部状态写成一行紧凑 JSON。
 * @param  buf 输出缓冲（调用方给）
 * @param  len buf 的字节数（含结尾 '\0' 占的那一格）
 * @return >= 0 写入长度（不含 '\0'）；< 0 取不到（见本文件头上那两条）
 *
 * 任何线程可调；调用方（robot_ui 的 network_task）只读，本函数不阻塞。
 */
int ai_companion_diag_snapshot(char *buf, size_t len);

/****************************************************************************
 * 内部实现（薄壳在 ai_companion_diag.c，真读取在 ai_companion_main.c）
 *
 * 为什么不把那些 static 放开可见性：g_listen_wanted 的语义（"想常听"而不是
 * "已经听上了"）、录音那几个标志之间的先后、以及录音链路正被停/重开时读到的那
 * 半程状态该怎么解释，约束全都写在 ai_companion_main.c 的注释里。加一层门面
 * 代价最小，也不用把 hello_app 的内部类型暴露给 robot_ui（它也 include 本文件）。
 ****************************************************************************/

/**
 * @brief  只读快照：把 hello_app 的内部状态写进调用方给的缓冲
 * @param  buf 调用方缓冲（@param len 是它的字节数，含结尾 '\0'）
 *
 * 内容是一行、空格分隔的 "key=value" 文本，键就是本文件头上那张字段表里的
 * 短名字（布尔写 1/0）；**没写进来的键 = 取不到**，diag.c 按 -1 输出 ——
 * 这样"某个字段暂时还没人记录"（vad / asr 就是这种）不需要在两边各写一遍特判。
 * 装不下就截断（可能停在半截键值对上，diag.c 会忽略读不完整的尾巴）。
 *
 * 只给 ai_companion_diag.c 调。它只读 static、不碰设备、不加锁，立刻返回 ——
 * 读到的可能是慢一拍的旧值（那些标志本来就由 hello_app 自己的几条线程在写），
 * 诊断要的是"现在大概什么形状"，为此加锁等于把调用方挂到 hello_app 的线程上，
 * 那正是这套诊断最不该引入的东西。
 */
void ai_companion_state_snapshot_impl(char *buf, size_t len);

#endif /* __AI_COMPANION_DIAG_H */
