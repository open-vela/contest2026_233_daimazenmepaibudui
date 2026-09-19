/**
 * stub_framework.c - host 测试用的假框架 / 假网络出口 / 假二次确认入口
 *
 * 只做一件事：把调用记到 g_stub 里。行为（成功/失败）由测试通过
 * g_stub.cmd_ret / g_stub.report_ret 控制。
 */

#include <stdio.h>
#include <string.h>

#include "ai_tools_host_stub.h"
#include "tools/tool_registry.h"

host_stub_t g_stub;

/* provider 的 get_tools 回调（真框架每次重建清单都会调一次） */
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

void tool_registry_register_provider(const char *name,
                                     tool_provider_fn get_tools,
                                     tool_executor_fn execute)
{
  g_stub.provider_name = name;
  g_stub.provider_count++;
  g_stub.execute = execute;
  g_get_tools = get_tools;
}

void tool_registry_invalidate(void)
{
  g_stub.invalidate_calls++;
}

int ai_network_send_device_command(ai_network_context_t *ctx,
                                   const char *device_id, const char *command)
{
  (void)ctx;

  g_stub.cmd_calls++;
  snprintf(g_stub.last_device, sizeof(g_stub.last_device), "%s",
           device_id != NULL ? device_id : "");
  snprintf(g_stub.last_command, sizeof(g_stub.last_command), "%s",
           command != NULL ? command : "");
  return g_stub.cmd_ret;
}

int ai_network_report_sound_alarm(ai_network_context_t *ctx,
                                  const char *sound_type, int confidence)
{
  (void)ctx;

  g_stub.report_calls++;
  snprintf(g_stub.last_sound_type, sizeof(g_stub.last_sound_type), "%s",
           sound_type != NULL ? sound_type : "");
  g_stub.last_confidence = confidence;
  return g_stub.report_ret;
}

/* 二次确认入口：由另一个模块（app/robot_ui）落地，这里给一个强符号顶上。
 * 用 -DNO_ASK_STUB 编译时**不提供**它 —— 那条路走的是"对面还没落地"的分支
 * （ai_tools_provider.c 里那个 weak 声明判空）。 */
#ifndef NO_ASK_STUB
void ui_post_ask_alarm(const char *reason)
{
  g_stub.ask_calls++;
  snprintf(g_stub.last_ask_reason, sizeof(g_stub.last_ask_reason), "%s",
           reason != NULL ? reason : "");
}
#endif
