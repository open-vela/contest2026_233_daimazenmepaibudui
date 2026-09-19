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
 * ⚠️ /sound_alarm 与 /alarm 的区别（stub 也照这个语义做）：
 *   我们调的是 report_sound_alarm（不推手机、不上报 /alarm）；如果哪天有人把
 *   紧急报告工具改成调 ai_network_send_alarm()，这里就会多一次会推手机的假调用
 *   —— 测试里没有这个假实现，链接会直接失败，等于把这条红线钉死在编译期。
 */

#ifndef AI_TOOLS_HOST_STUB_H
#define AI_TOOLS_HOST_STUB_H

#include <stddef.h>

#include "ai_network.h"

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
