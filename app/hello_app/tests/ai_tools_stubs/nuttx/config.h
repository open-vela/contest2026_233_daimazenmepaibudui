/**
 * 给 host 测试用的假 <nuttx/config.h>
 *
 * 谁在用：tests/run_host_tests_ai_tools.sh 编的 TU 里，ai_tools_provider.c 和
 * tests/test_ai_tools_provider.c 都 `#include <nuttx/config.h>`；PC 上没有真机那
 * 一份，所以这里补一份最小的。（stub_framework.c 只读 tools/tool_registry.h，
 * 不经过这里。）
 *
 * 真身在哪：
 *   - 开关：构建时生成的 nuttx/config.h。本板卡那份在 openvela/cmake_out/
 *     contest2026_233_board_sf32lb52_ai/include/nuttx/config.h，源头是
 *     board/contest_board/configs/sf32lb52_ai/defconfig。
 *   - OK / ERROR：其实**不在** config.h 里 —— 真身是 nuttx/include/sys/types.h 的
 *     `enum { ERROR = -1, OK = 0 }`；真机上这两个名字是 <stdio.h> 顺着
 *     <sys/types.h> 带进来的。
 *
 * ⚠️ 这份文件只在 host 测试脚本的 -I 里（`-I$STUBS` 排在 -I$APP_DIR 前面）；
 *    真机固件编的是上面那份生成的 config.h，两边互不影响。
 *    以后哪个 TU 新读了别的开关、host 编不过，照上面两条线索往这里补一行。
 *
 * 只放真正被读到的这几项：
 *   OK / ERROR —— 谁用：tester 拿它判 `exec_tool(...) == OK / == ERROR`，
 *                 provider 自己 `return OK / ERROR`。host 上 <sys/types.h> 是 glibc
 *                 的、没有这两个名字，所以照真身取值补成 #define：成功 0、
 *                 失败 -1（NuttX 口径是 0 / 负 errno，不是 POSIX 的 -1+errno）。
 *                 NuttX 自家的 host 假头 apps/system/zmodem/host/nuttx/config.h
 *                 也是 `#define OK 0` / `#define ERROR -1` 这么补的。
 *                 唯一语义差：真身是枚举、这里是宏，`#ifdef OK` 那种写法两边结果
 *                 不同 —— 被测源码和它们带进来的头里没有这种写法（grep 过），
 *                 所以这个简化不改语义。
 *   CONFIG_HELLO_APP_LLM_AI_AGENT —— 谁用：provider 用它圈住整段注册代码
 *                 （ai_tools_provider.c 的 3 处 #ifdef，以及 init 里那句
 *                 "本构建没开"）。真身来自 Kconfig 的 choice：EXAMPLES_AI_AGENT_VELA=y
 *                 时默认选 HELLO_APP_LLM_AI_AGENT，生成出来就是
 *                 `#define CONFIG_HELLO_APP_LLM_AI_AGENT 1`，这里连"值 = 1"一起照抄
 *                 （别写成 `#define ... y`）。
 */

#ifndef AI_TOOLS_HOST_STUB_NUTTX_CONFIG_H
#define AI_TOOLS_HOST_STUB_NUTTX_CONFIG_H

#define OK                        0
#define ERROR                     (-1)

#define CONFIG_HELLO_APP_LLM_AI_AGENT 1

#endif /* AI_TOOLS_HOST_STUB_NUTTX_CONFIG_H */
