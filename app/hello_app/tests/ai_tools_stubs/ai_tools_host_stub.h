/**
 * ai_tools_host_stub.h - host 测试用的「假框架 + 假依赖」状态
 *
 * ai_tools_provider.c 的 execute 逻辑离线可测：它一共只依赖三样外部东西 ——
 *   1) 框架的 tool_registry_register_provider() / _invalidate()（收下我们的
 *      两个回调，模型侧的清单和执行都由它转进来）；
 *   2) 发送出口 ai_network_send_device_command()（灯控）和
 *      ai_network_report_sound_alarm()（紧急事件上报）；
 *   3) 二次确认入口 ui_post_ask_alarm()（弹「是否报警？」询问框）。
 *
 * 这三样在 host 上全部由 stub_framework.c 替换掉，并把每次调用的参数记在这里，
 * 测试断言只看这张表 —— 不引 LVGL、不引 cJSON、不起线程。
 *
 * 真身在哪（签名/语义一律以它们为准，别照这份文件改）：
 *   - 框架那两个：openvela 侧 packages/ai_agent/src/tools/tool_registry.h
 *     （tool_registry_register_provider() / tool_registry_invalidate()，以及下面
 *     host_executor_fn 对应的那个 tool_executor_fn；真身注册时 name **只存指针、
 *     不复制**，所以 provider 传进去的必须是静态存储期的串 —— 这份 stub 照同样
 *     的语义存指针，A 组用例才敢断言名字没被拷成栈上的临时串）；
 *   - 网络那两个出口：**就是本仓库的真头** app/hello_app/ai_network.h。下面那句
 *     #include "ai_network.h" 由 -I$APP_DIR 解析到它，不是假头 —— 于是
 *     stub_framework.c 里那两个假实现的签名被真原型盯着（参数、0 / 负 errno 的
 *     返回口径写错一处就编不过）。返回 0 表示已经交给网络层；负值才是没进队列。
 *     ⚠️ 真头里 ai_network_send_alarm() 也存在，但**故意不做假实现**（见下）。
 *   - ui_post_ask_alarm()：声明在 app/robot_ui/robot_ui.h、实现在
 *     app/robot_ui/main.c（ai_tools_provider.c 里那份 weak 原型照它抄的）。
 *     这份 stub 只记「收没收到、reason 是什么」，不引 LVGL、不投递、不分线程。
 *
 * host 上为什么可以简化：这三样在板子上分别是 ai_agent 的注册表、MQTT 排队口和
 * 界面线程，host 上一个都不存在，也没有第二个调用者跟它抢状态。所以假实现只留
 * 「记下参数 + 返回 g_stub.xxx_ret」这一层 —— 决策分支（哪条路走到哪个出口）仍旧
 * 全部由被测源码决定，语义一点没动。
 *
 * 这份 -I 只作用于 app/hello_app/tests/run_host_tests_ai_tools.sh 的编译命令
 * （它把 -I tests/ai_tools_stubs 排在 -I$APP_DIR 前面）；真机固件的编译命令里
 * 没有这个目录，编的是 NuttX / 框架的真头，不受影响 —— 而且被测源码
 * ai_tools_provider.c 根本不 include 本文件（只有测试和 stub 用）。
 *
 * ⚠️ /sound_alarm 与 /alarm 的区别（stub 也照这个语义做）：
 *   我们调的是 report_sound_alarm（不推手机、不上报 /alarm）；如果哪天有人把
 *   紧急报告工具改成调 ai_network_send_alarm()，这里没有它的假实现，链接会直接
 *   失败（真头声明了、没人定义 -> undefined reference），等于把这条红线钉死在
 *   编译期。
 */

#ifndef AI_TOOLS_HOST_STUB_H
#define AI_TOOLS_HOST_STUB_H

#include <stddef.h>

#include "ai_network.h"

/* 和框架 tools/tool_registry.h 的 tool_executor_fn **同一个签名**（单列一个名字
 * 只是为了不让测试那份 TU 也把框架头拖进来）。stub_framework.c 把注册进来的回调
 * 原样存进 g_stub.execute，测试再通过它调工具 —— 两个类型必须兼容，否则那里就
 * 编不过。 */
typedef int (*host_executor_fn)(const char *name, const char *input_json,
                                char *output, size_t output_size);

typedef struct
{
  /* ── 框架侧：我们注册进去的东西 ────────────────────────── */
  const char   *provider_name;
  int           provider_count;   /* register_provider 被调了几次 */
  int           invalidate_calls; /* invalidate 被调了几次 */
  host_executor_fn execute;       /* provider 的 execute 回调 */

  /* ── 灯控出口 ─────────────────────────────────────────── */
  int           cmd_calls;
  char          last_device[64];
  char          last_command[16];
  int           cmd_ret;          /* 想让 send_device_command 返回什么 */

  /* ── 紧急事件上报出口 ─────────────────────────────────── */
  int           report_calls;
  char          last_sound_type[64];
  int           last_confidence;
  int           report_ret;       /* 想让 report_sound_alarm 返回什么 */

  /* ── 二次确认入口 ─────────────────────────────────────── */
  int           ask_calls;
  char          last_ask_reason[256];
} host_stub_t;

extern host_stub_t g_stub;

/* 每个用例前清掉**调用记录**（刚发了什么、返回值设成多少）。
 *
 * 刻意不清 provider_* / invalidate_calls / execute：ai_tools_provider_init() 是
 * 幂等的（第二次不会再注册），清了注册信息就等于把 execute 回调弄丢，后面所有
 * 用例都会崩在空指针上。注册那几项只在进程开头是干净值，A 组用例就看它。 */
void host_stub_reset_calls(void);

/* 取最近一次工具清单：内部会调 provider 的 get_tools 回调。
 * 返回 malloc 的串（和真框架一样，谁取谁 free）；没注册过返回 NULL。 */
char *host_stub_take_tools_json(void);

#endif /* AI_TOOLS_HOST_STUB_H */
