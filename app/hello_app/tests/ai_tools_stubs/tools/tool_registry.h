/**
 * 给 host 测试用的假 tools/tool_registry.h
 *
 * 真身：openvela/packages/ai_agent/src/tools/tool_registry.h
 *       （绝对路径 /home/youdian/openvela/packages/ai_agent/src/tools/；
 *         openvela/apps/packages 是指向 openvela/packages 的软链接）
 *
 * 为什么 host 上需要它：ai_tools_provider.c 在 CONFIG_HELLO_APP_LLM_AI_AGENT 下会
 * `#include "tools/tool_registry.h"`，而真头第一件事就是 include "agent_compat.h"
 * （pthread.h / syslog.h / sys/time.h / sys/types.h 一大串），后面还有内置工具表的
 * agent_tool_t + TOOL_SCHEMA_* / REGISTER_TOOL 宏，以及 init / get_tools_json /
 * execute / cleanup 等原型。run_host_tests_ai_tools.sh 那条 cc 的 -I 里没有这些头，
 * 而我们真正用到的只有两条函数：注册 provider、打脏清单缓存。
 *
 * ⚠️ 这份文件**只在 host 测试的 -I 里**（见 run_host_tests_ai_tools.sh 的 -I$STUBS）；
 *    真机固件编的是 ai_agent 包里那份真头，两者互不影响（真头一份 -I 都没动）。
 *    以后 ai_tools_provider.c 新读了真头里的别的符号，编不过时再往这里补一条。
 */

#ifndef AI_TOOLS_HOST_STUB_TOOL_REGISTRY_H
#define AI_TOOLS_HOST_STUB_TOOL_REGISTRY_H

#include <stddef.h>

/* 下面四条的类型和签名逐个对着真头核过。签名写歪了 host 上照样编得过（没人对账），
 * 真机链接/运行时才炸，所以这里不许"简化"成看着像的东西。
 * 返回值口径是 NuttX 的 OK / ERROR（0 / -1），不是 POSIX 的 -1 + errno。 */

/* 取工具清单的回调。谁用：假框架 stub_framework.c 存下它（转发给
 * host_stub_take_tools_json()）；真身是 ai_tools_provider.c 的
 * ai_tools_provider_get_tools()。返回 malloc 出来的 JSON 数组串（取的人负责 free），
 * 没有工具可以回 NULL —— host 上只是一个函数指针类型，不碰真头里那套 cJSON。 */

typedef char *(*tool_provider_fn)(void);

/* 执行工具调用的回调。谁用：假框架 tool_registry_register_provider() 把它存进
 * g_stub，测试再通过 g_stub.execute 直接调；真身是 ai_tools_provider_execute()。
 * 口径抄真头原话 "Returns OK if tool was found and executed, ERROR otherwise"
 * （真实现也只判 ret == OK）：OK = 名字是我们的、已经处理（失败原因写进 output），
 * ERROR = 不是我们的工具，框架会接着问下一个 provider。
 * host 上 OK / ERROR 由假 <nuttx/config.h> 提供，值跟 NuttX <sys/types.h> 一致。 */

typedef int (*tool_executor_fn)(const char *name, const char *input_json,
                                char *output, size_t output_size);

/* 注册一个包外 provider（真头注明最多 4 个，真实现是 MAX_PROVIDERS）。
 * 谁用：ai_tools_provider.c 的 ai_tools_provider_init()（只在开了
 * CONFIG_HELLO_APP_LLM_AI_AGENT 时编进去）。host 上假实现只把 name 和两个回调
 * 记进 g_stub，没有真表；真实现只存 name 指针、不复制字符串（所以调用方给的
 * 名字必须是静态存储期，这条约束的假实现也照做 —— 它同样只存指针）。 */

void tool_registry_register_provider(const char *name,
                                     tool_provider_fn get_tools,
                                     tool_executor_fn execute);

/* 把工具清单缓存打脏，下次取清单时重建。谁用：ai_tools_provider_init() 里注册完
 * 紧跟一句。host 上没有真缓存可脏，假实现只记调用次数（测试据此断言"打脏过"）。 */

void tool_registry_invalidate(void);

#endif /* AI_TOOLS_HOST_STUB_TOOL_REGISTRY_H */
