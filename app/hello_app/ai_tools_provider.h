/****************************************************************************
 * AI Tools Provider Header
 * 智爱陪伴 - 把「我们自己的工具」注册进 openvela 框架的工具表
 *
 * 这一层解决的问题：我们的工具（灯控等）想被**框架里的那个大模型**看见并调用，
 * 而框架的工具表（tool_registry）长在 ai_agent 这个 app 里。本文件把
 * tool_registry_register_provider() 这条**包外注册**的公开接口包一层，
 * 用法收敛成「设一次网络上下文 + 初始化一次」。
 ****************************************************************************/

#ifndef AI_TOOLS_PROVIDER_H
#define AI_TOOLS_PROVIDER_H

#include <stdbool.h>

#include "ai_network.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 我们注册进框架的 provider 名字（只给人看，日志里会出现）。
 * tool_registry 只存这个指针、不复制字符串，所以必须是静态存储期 ——
 * 别传栈上的缓冲。 */

#define AI_TOOLS_PROVIDER_NAME   "contest"

/* 我们对模型暴露的工具名 */

#define AI_TOOL_SET_LIGHT        "set_light"

/* 紧急报告工具：把「呼救→云端→报警」这条路的后半截接上。
 *
 * 模型判断出"老人正在求救/可能受伤"时调用它。板子**不直接报警**，而是先弹
 * 「是否报警？」询问框让用户点头（见下面「二次确认」一节），确认之后才由界面
 * 那一侧走真正的报警（/alarm 上报 + 手机推送 + 报警页）。
 *
 * ⚠️ 这个名字是模型调用时的唯一凭据：清单 JSON（ai_tools_provider.c 的
 * AI_TOOLS_REPORT_EMERGENCY_JSON）和执行回调的分支都认它，改名等于把这条链
 * 从模型手里摘掉。 */

#define AI_TOOL_REPORT_EMERGENCY "report_emergency"

/* device_id 缺省值。**必须和 ai_companion_main.c 里的 LIGHT_DEVICE_ID 是
 * 同一个值**：队友的灯模拟器只认这一台，模型没提哪个灯时我们替它补上。
 * 那个宏在 ai_companion_main.c 里是文件私有的，这里只能各写一份，改一处
 * 记得改另一处。 */

#define AI_TOOLS_LIGHT_DEVICE_ID "living_room_light"

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/**
 * @brief  把我们的工具挂进框架的工具表
 *
 * 做了两件事：调 tool_registry_register_provider() 注册 provider（get_tools +
 * execute 两个回调），再调 tool_registry_invalidate() 让工具清单缓存失效，
 * 下一次 ai_agent 收消息时会重建清单、把 set_light 一起下发给模型。
 *
 * 幂等：同一个进程里重复调是空操作（provider 表一共只有 4 个名额，重复注册
 * 会把清单里塞进两份同名工具，有些服务端会因为工具名重复直接 400）。
 *
 * ⚠️ 前提：**ai_agent 那个 app 必须先跑起来**（本固件没有开机自启，
 * rcS 是空的、CONFIG_INIT_ENTRYPOINT="nsh_main"，得在 nsh 里手工敲
 * `ai_agent`）。原因见下面的「运行时机」一节。ai_agent 没跑时调这个函数
 * 本身不会崩（只是往一个没人读的数组里写一项），但模型这会儿也看不见工具。
 *
 * @return 0 成功；-ENOSYS 本构建没开 CONFIG_HELLO_APP_LLM_AI_AGENT
 *         （那时头文件路径都没有，整份实现被条件编译掉了）
 */

int ai_tools_provider_init(void);

/**
 * @brief  告诉 provider「往哪张网络上下文上发设备命令」
 *
 * 工具的 execute 回调跑在 **ai_agent 的 agent_loop 任务里**，拿不到
 * ai_companion_main.c 里那个 static 的 g_net_ctx，所以要在初始化阶段把它
 * 递进来。工具执行时调的是 ai_network_send_device_command(ctx, ...)，
 * ctx 为空时工具会回一句「灯控还没接上界面」给模型，而不是崩。
 *
 * 只需设一次，之后不要再改：这个指针会被另一个任务读，中途换会读到半新半旧
 * 的状态。传 NULL 表示「撤销」（反初始化时用）。
 *
 * @param  ctx  由 ai_network_start_shared() 建出来（或复用）的那份网络上下文，
 *              或 NULL。**不要**用已删除的 ai_network_init() 另建一份：
 *              network_comm 在一个固件里只有一份实例，另建会把界面正在用的
 *              那条 MQTT 连接顶掉。
 */

void ai_tools_provider_set_network_ctx(ai_network_context_t *ctx);

/****************************************************************************
 * 二次确认：紧急报告工具依赖的那一个外部符号
 *
 *   模型调 report_emergency 之后**不会直接报警** —— 板子先请用户确认。确认的
 *   入口就是界面（robot_ui）那一侧的一个动作：
 *
 *       void ui_post_ask_alarm(const char *reason);
 *
 *   它弹「是否报警？」询问框（reason 是询问框上那句话），任何任务线程可调
 *   （内部只做投递，不碰 LVGL，和 ui_post_* 那一组同一个做法）。用户点了
 *   「确认」之后，真正报警那一步由界面那一侧负责：/alarm 上报 + 手机推送 +
 *   报警页（都走现成部件，不在我们这一层）。
 *
 *   ⚠️ 这个符号由**另一个模块（app/robot_ui）落地**，现在已经落地了：
 *      声明在 app/robot_ui/robot_ui.h（`void ui_post_ask_alarm(const char *reason);`），
 *      实现在 app/robot_ui/main.c。这一侧：
 *     - 不 include robot_ui.h：那个头文件要 <lvgl.h>，而 hello_app 的编译命令
 *       里没有（也不该有）LVGL 的头文件路径，理由和 robot_ui_bridge.h /
 *       fall_alarm.h 那两个"自给自足头文件"完全一样；
 *     - 在 ai_tools_provider.c 里自己声明一份原型（签名和对面一字不差），
 *       并且加 __attribute__((weak))：一句话——**对面的符号先合、我们的代码
 *       先合，两种顺序都不能把整机镜像链断**。对面没在时函数地址是 NULL，
 *       这里判空后如实回一句「询问框没弹出来」给模型，而不是假装弹过；对面
 *       在时（就是现在）自动绑到那个强符号上（host 测试里验过这条绑定）。
 *
 *   这一层不做"用户确认了没有"的判断：那是界面那条链的状态，模型那边只能
 *   拿到"已经请用户确认"这一句，见 ai_tools_provider.c 里回给模型的文本。
 ****************************************************************************/

/****************************************************************************
 * 运行时机：这两个函数该在哪调（具体接线在 ai_companion_main.c 的 main() 里）
 *
 * 一、谁在跑框架的 agent loop
 *
 *   运行时只有 **ai_agent 这个 app** 在跑 agent loop（agent_loop.c 里的
 *   agent_loop_task），它由 ai_agent 自己的 main() 里 agent_loop_start() 拉起来。
 *   ai_companion 不起 agent loop：它的 llm_send_text() 走 velaclaw 客户端，
 *   而 velaclaw_ask() 只是把文本 message_bus_push_inbound() 丢进 ai_agent
 *   进程里的那条**进程内**队列（CONFIG_BUILD_FLAT=y，同一个地址空间），
 *   真正组请求、下发工具表、执行工具、把回复推回来的是 ai_agent 那边。
 *
 *   所以「模型能不能看见我们的工具」= 「ai_agent 在不在跑」。它跟
 *   llm_send_text() 能不能用是同一个前提，不是两件事。
 *
 * 二、调用顺序（都在 ai_companion 的 main() 里，建议紧挨着各自的依赖）
 *
 *   1. ai_network_start_shared(&g_net_ctx, <client_id>) 成功之后
 *        -> ai_tools_provider_set_network_ctx(&g_net_ctx);
 *      理由：工具执行时要有 ctx 才能发 device_cmd。放太早会拿到没初始化的
 *      ctx（ai_network_is_mqtt_connected() 会直接判成没连上）。
 *      这里只能借界面（robot_ui）那条连接，**不要**改回 ai_network_init() ——
 *      那个入口连同 ai_network_send_voice() / ai_network_send_text() 一起删掉了
 *      （零调用者），剩下的那条路会复位 network_comm 的全局状态、并把 MQTT
 *      收包回调槽抢走。
 *
 *   2. 紧接着（同一处）调 ai_tools_provider_init();
 *      理由：注册本身跟网络无关，只要在「第一次向模型提问」之前完成就行。
 *      注意它**不需要** ai_agent 已经在跑：tool_registry_init() 只清空内置
 *      工具计数、不清 provider 数组，dirty 标志也是我们这边置的，所以
 *      「先 ai_companion 注册、后 ai_agent 起来」这条路一样生效
 *      （ai_agent 启动时的第一次 build 就会带上我们的工具）。
 *
 *   3. 反初始化：ai_network_deinit() 之前调
 *        ai_tools_provider_set_network_ctx(NULL);
 *      理由：ai_agent 可能还活着并在处理某条消息，别让它拿着已经析构的 ctx
 *      去发命令。注册本身不用撤销（provider 数组没有反注册接口，
 *      get_tools 也永远返回同一份清单）。
 *
 *   这三点都接线在 ai_companion_main.c 的 main() 里（已经接好了），这里只把
 *   约定写清楚。
 *
 *   另外：**MQTT 收包回调不在这里注册**。全固件只有一个槽，归 robot_ui 的
 *   network_task —— app/robot_ui/main.c 里 network_set_mqtt_callback() 那一处，
 *   只注册一次。hello_app 侧只 publish（走排队口 mqtt_publish_queued()）、
 *   不订阅、不抢槽；抢了界面就再也收不到 ai_reply。
 ****************************************************************************/

#ifdef __cplusplus
}
#endif

#endif /* AI_TOOLS_PROVIDER_H */
