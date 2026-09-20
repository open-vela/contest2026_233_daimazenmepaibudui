/**
 * stub_framework.c - host 测试用的假框架 / 假网络出口 / 假二次确认入口
 *
 * 给谁用：app/hello_app/tests/run_host_tests_ai_tools.sh（那条 cc 命令编的是
 * test_ai_tools_provider.c + 本文件 + ../ai_tools_provider.c）。本文件顶掉
 * ai_tools_provider.c 的三个外部依赖，让 get_tools / execute 两个回调在 PC 上
 * 离线可测：
 *   1) 框架工具表 tool_registry_register_provider() / tool_registry_invalidate()
 *   2) 网络出口 ai_network_send_device_command() / ai_network_report_sound_alarm()
 *   3) 二次确认入口 ui_post_ask_alarm()
 *
 * 真身在哪（这三样在板子上的实现，下面每个函数头上都注明了对应位置）：
 *   框架工具表：openvela/packages/ai_agent/src/tools/tool_registry.c
 *               （原型在 openvela/packages/ai_agent/src/tools/tool_registry.h；
 *                 假头是 tests/ai_tools_stubs/tools/tool_registry.h）
 *   网络出口：app/hello_app/ai_network.c
 *             （原型在 app/hello_app/ai_network.h）
 *   二次确认：app/robot_ui/main.c
 *             （原型在 app/robot_ui/robot_ui.h）
 *
 * 为什么 host 上需要它：那三个真身一个要 ai_agent 的 pthread 互斥 + cJSON 工具清单
 * 缓存，一个要连着 MQTT 的 network_comm 排队口，一个要 LVGL 线程（ui_post_ask_alarm
 * 内部 lv_async_call 投递到界面线程）。PC 上这三样都没有，也不该为了一段「取参数
 * → 判合法 → 调出口 → 回文本」的纯逻辑把它们搬过来。本文件只做一件事：把调用参数
 * 记到 g_stub 里，成功/失败由测试用 g_stub.cmd_ret / g_stub.report_ret 摆。
 *
 * 返回值口径照真身：0 = 成功，负值 = 失败（真身给的是 -EINVAL / -ENOTCONN 这类
 * 负 errno，**不是** POSIX 的 -1 + errno），所以 <errno.h> 也得引。
 *
 * 真机固件不受影响：本文件（以及整个 tests/ 目录）只出现在 host 测试脚本那条 cc
 * 命令里，-Itests/ai_tools_stubs 也只加在那条命令上；固件的源文件列表
 * （app/hello_app/Makefile 的 MAINSRC/CSRCS、CMakeLists.txt 里那一串）没有 tests/，
 * 板子上链的还是上面那三个真身。
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "ai_tools_host_stub.h"
#include "tools/tool_registry.h"

host_stub_t g_stub;

/* provider 的 get_tools 回调（真框架每次重建清单都会调一次）。
 *
 * 真身把它和 name / execute 一起存进 MAX_PROVIDERS(=4) 格的 s_providers[]，满格就
 * 丢掉并打一条日志。host 上只留最近注册的那一份：A 组只注册一个 provider，那 4 格
 * 上限和「满格丢日志」没有任何断言点，在假实现里复现只会多一张没人看的表。 */
static tool_provider_fn g_get_tools;

void host_stub_reset_calls(void)
{
  g_stub.cmd_calls = 0;
  g_stub.last_device[0] = '\0';
  g_stub.last_command[0] = '\0';
  g_stub.cmd_ret = 0;

  g_stub.report_calls = 0;
  g_stub.last_sound_type[0] = '\0';
  g_stub.last_confidence = 0;
  g_stub.report_ret = 0;

  g_stub.ask_calls = 0;
  g_stub.last_ask_reason[0] = '\0';
}

char *host_stub_take_tools_json(void)
{
  if (g_get_tools == NULL)
    {
      return NULL;
    }

  return g_get_tools();
}

/* 真身：openvela/packages/ai_agent/src/tools/tool_registry.c
 *
 * 一字不差地照抄真头的原型（tools/tool_registry.h），免得 host 编过了、真机链接时
 * 签名不匹配。真身这里只管存进 s_providers[]、**不打脏** —— 标脏是调用方的事
 * （ai_tools_provider_init() 注册完自己调一次 invalidate()，理由见那个函数）。
 * host 上也照这个分工：只记注册信息，不碰 invalidate_calls。 */
void tool_registry_register_provider(const char *name,
                                     tool_provider_fn get_tools,
                                     tool_executor_fn execute)
{
  g_stub.provider_name = name;
  g_stub.provider_count++;
  g_stub.execute = execute;
  g_get_tools = get_tools;
}

/* 真身：tool_registry.c，只把 s_tools_dirty 置真（加了把互斥锁）。
 * host 上没有工具清单缓存要标脏，就数一下调用次数 —— A 组那条「注册完打了脏
 * （不然模型看不到新工具）」的断言靠它。 */
void tool_registry_invalidate(void)
{
  g_stub.invalidate_calls++;
}

/* 真身：app/hello_app/ai_network.c 的 ai_network_send_device_command()（原型在
 * app/hello_app/ai_network.h）。真身开局就是
 *   if (!ctx || !device_id || !command) { return -EINVAL; }
 * host 上照同一口径先判一次：NULL 进来是「一包都不发」的错误调用，不能记成一次
 * 成功下发 —— 记成成功的话 provider 会回模型一句真机上不可能出现的「已交给网络层」。
 * 再往下真身要查 MQTT 连接、走 network_comm 的排队口（负值 = 没进发送队列），
 * host 上不连网，返回什么由 g_stub.cmd_ret 摆（测试用 -1 代表那条路）。 */
int ai_network_send_device_command(ai_network_context_t *ctx,
                                   const char *device_id, const char *command)
{
  if (ctx == NULL || device_id == NULL || command == NULL)
    {
      return -EINVAL;
    }

  g_stub.cmd_calls++;
  snprintf(g_stub.last_device, sizeof(g_stub.last_device), "%s", device_id);
  snprintf(g_stub.last_command, sizeof(g_stub.last_command), "%s", command);
  return g_stub.cmd_ret;
}

/* 真身：app/hello_app/ai_network.c 的 ai_network_report_sound_alarm()，判参同样是
 * `if (!ctx || !sound_type) return -EINVAL;`，之后走 report_abnormal_sound_queued()
 * 的排队口 —— 那条通道发的是 /sound_alarm，不发 /alarm、不推手机（这条红线见
 * ai_tools_host_stub.h 的文件头）。host 上只记参数 + 返回 g_stub.report_ret。 */
int ai_network_report_sound_alarm(ai_network_context_t *ctx,
                                  const char *sound_type, int confidence)
{
  if (ctx == NULL || sound_type == NULL)
    {
      return -EINVAL;
    }

  g_stub.report_calls++;
  snprintf(g_stub.last_sound_type, sizeof(g_stub.last_sound_type), "%s",
           sound_type);
  g_stub.last_confidence = confidence;
  return g_stub.report_ret;
}

/* 二次确认入口：真身是 app/robot_ui/main.c 的 ui_post_ask_alarm()（原型在
 * app/robot_ui/robot_ui.h，任何线程可调，内部把询问页 lv_async_call 投到 LVGL
 * 线程），这里给一个强符号顶上。host 上不起界面线程，只把 reason 记下来 —— 断言
 * 的是「我们有没有按约定调它、参数是不是 reason 原文」；真弹框、用户点确认之后走
 * /alarm 是另一个模块的活（见 test_ai_tools_provider.c 的「没覆盖的」那一段）。
 *
 * 真身拿到 NULL / 空串时会把原因换成兜底文案「异常声响」（那句话最后显示在询问页
 * 上），这里跟着做同一个替换，g_stub.last_ask_reason 才和真机上用户看到的对得上。
 * 真身另外那些规矩（中断上下文里不发起询问、同一 reason 的 60 秒静默期、同一时间
 * 只留一条 pending）在 provider 这条路上没有断言点，host 上不复现。
 *
 * 用 -DNO_ASK_STUB 编译时**不提供**它 —— 那条路走的是"对面还没落地"的分支
 * （ai_tools_provider.c 里那个 weak 声明判空）。 */
#ifndef NO_ASK_STUB
void ui_post_ask_alarm(const char *reason)
{
  if (reason == NULL || reason[0] == '\0')
    {
      reason = "异常声响";
    }

  g_stub.ask_calls++;
  snprintf(g_stub.last_ask_reason, sizeof(g_stub.last_ask_reason), "%s",
           reason);
}
#endif
