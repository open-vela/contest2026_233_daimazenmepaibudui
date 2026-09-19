/**
 * 给 host 测试用的假 tools/tool_registry.h
 *
 * 真头文件在 openvela/apps/packages/ai_agent/src/tools/tool_registry.h，
 * 里面还带着 cJSON / syslog / 内置工具表那一大套。host 上只需要我们真正用到的
 * 两样：注册 provider 和打脏清单缓存。函数原型一字不差地抄自真头文件，
 * 免得 host 测试通过、真机链接签名不匹配。
 */

#ifndef AI_TOOLS_HOST_STUB_TOOL_REGISTRY_H
#define AI_TOOLS_HOST_STUB_TOOL_REGISTRY_H

#include <stddef.h>

typedef char *(*tool_provider_fn)(void);
typedef int (*tool_executor_fn)(const char *name, const char *input_json,
                                char *output, size_t output_size);

void tool_registry_register_provider(const char *name,
                                     tool_provider_fn get_tools,
                                     tool_executor_fn execute);

void tool_registry_invalidate(void);

#endif /* AI_TOOLS_HOST_STUB_TOOL_REGISTRY_H */
