/**
 * 给 host 测试用的假 <nuttx/config.h>
 *
 * 只放 ai_tools_provider.c 真正读到的那几个开关/常量：
 *   OK / ERROR          —— 框架 execute 回调的返回值
 *   CONFIG_HELLO_APP_LLM_AI_AGENT —— 打开之后 provider 注册那一整段（含对
 *                          框架 tool_registry 的调用）才会编进来，
 *                          也就是真机固件现在用的那份配置。
 */

#ifndef AI_TOOLS_HOST_STUB_NUTTX_CONFIG_H
#define AI_TOOLS_HOST_STUB_NUTTX_CONFIG_H

#define OK                        0
#define ERROR                     (-1)

#define CONFIG_HELLO_APP_LLM_AI_AGENT 1

#endif /* AI_TOOLS_HOST_STUB_NUTTX_CONFIG_H */
