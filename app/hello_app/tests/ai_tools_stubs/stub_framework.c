/**
 * stub_framework.c - host 测试用的假框架 / 假网络出口 / 假二次确认入口
 *
 * 给谁用：app/hello_app/tests/run_host_tests_ai_tools.sh（那条 cc 命令编的是
 * test_ai_tools_provider.c + 本文件 + ../ai_tools_provider.c）。本文件顶掉
 * ai_tools_provider.c 的五个外部依赖，让 get_tools / execute 两个回调在 PC 上
 * 离线可测：
 *   1) 框架工具表 tool_registry_register_provider() / tool_registry_invalidate()
 *   2) 网络出口 ai_network_send_device_command() / ai_network_report_sound_alarm()
 *   3) 二次确认入口 ui_post_ask_alarm()
 *   4) 提醒落地入口 mimo_reminder_apply()（add_reminder 工具的最终落点）
 *   5) 工具开关 tool_guard_set_enabled()（设备工具裁剪）
 *
 * 真身在哪（这三样在板子上的实现，下面每个函数头上都注明了对应位置）：
 *   框架工具表：openvela/packages/ai_agent/src/tools/tool_registry.c
 *               （原型在 openvela/packages/ai_agent/src/tools/tool_registry.h；
 *                 假头是 tests/ai_tools_stubs/tools/tool_registry.h）
 *   网络出口：app/hello_app/ai_network.c
 *             （原型在 app/hello_app/ai_network.h）
 *   二次确认：app/robot_ui/main.c
 *             （原型在 app/robot_ui/robot_ui.h）
 *   提醒落地：app/hello_app/mimo_voice.c
 *             （原型在 app/hello_app/mimo_voice.h —— host 上**用真头**，
 *               所以签名写错一处就编不过）
 *   工具开关：openvela/packages/ai_agent/src/tools/tool_guard.c
 *             （原型在同目录 tool_guard.h —— 那个头自给自足，同样用真头）
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
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* 假的 <nuttx/config.h>：只为 OK / ERROR 两个名字（tool_guard_set_enabled()
 * 的真身用 ERROR 表示「名字拼不进 64 字节 key 缓冲」，这里照抄同一个码）。 */
#include <nuttx/config.h>

#include "ai_tools_host_stub.h"
#include "mimo_voice.h"
#include "tools/tool_guard.h"
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

  g_stub.reminder_calls = 0;
  g_stub.last_reminder_time[0] = '\0';
  g_stub.last_reminder_title[0] = '\0';
  g_stub.reminder_ret = 0;

  g_stub.settings_calls = 0;
  g_stub.last_setting_item = -1;
  g_stub.last_setting_action = -1;
  g_stub.last_setting_value = 0;
  g_stub.settings_ret = 0;

  g_stub.guard_calls = 0;
  g_stub.guard_off_calls = 0;
  g_stub.last_guard_tool[0] = '\0';
  g_stub.guard_ret = 0;
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

/* 按 UTF-8 **字符**边界截到 max_chars 个字（真身用的是 mimo_voice.c 里的
 * tts_truncate()，口径一样：整字截，绝不在三字节汉字中间断开）。
 * 多字节序列在串尾被截断时整条丢掉 —— 宁可少一个字，不产出坏 UTF-8，
 * 否则这段标题会被 snprintf 原样写进给模型看的话术里。 */
static void stub_utf8_trunc(const char *src, char *dst, size_t dst_cap,
                            unsigned int max_chars)
{
  unsigned int n = 0;
  size_t out = 0;

  if (dst_cap == 0)
    {
      return;
    }
  dst[0] = '\0';

  while (*src != '\0' && n < max_chars)
    {
      unsigned char c = (unsigned char)*src;
      size_t adv;
      size_t k;

      if ((c & 0x80u) == 0x00u)
        {
          adv = 1;
        }
      else if ((c & 0xE0u) == 0xC0u)
        {
          adv = 2;
        }
      else if ((c & 0xF0u) == 0xE0u)
        {
          adv = 3;
        }
      else if ((c & 0xF8u) == 0xF0u)
        {
          adv = 4;
        }
      else
        {
          src++; /* 不是合法的首字节，跳过，别把它算成一个字符 */
          continue;
        }

      for (k = 1; k < adv; k++)
        {
          if (src[k] == '\0')
            {
              adv = 0;
              break;
            }
        }
      if (adv == 0)
        {
          break; /* 串尾半个字符 */
        }

      if (out + adv >= dst_cap)
        {
          break; /* 放不下整字就不放，给 '\0' 留位置 */
        }

      memcpy(dst + out, src, adv);
      out += adv;
      src += adv;
      n++;
    }

  dst[out] = '\0';
}

/* 真身：app/hello_app/mimo_voice.c 的 mimo_reminder_apply()（原型在
 * app/hello_app/mimo_voice.h，由上面那个 include 钉住签名）。
 *
 * 它是 add_reminder 工具的最终落点，真身依次做四件事：
 *   1) 判参（NULL / hhmm 不是 24 小时制 H:MM|HH:MM / title 为空 -> -EINVAL）；
 *   2) 把 hhmm 归一化成 "%02d:%02d"；
 *   3) 标题按整字截到 20 个字（mimo_voice.c 的 MIMO_REMINDER_TITLE_CHARS，
 *      那个宏是它私有的、头里没有 —— 下面照抄 20，改真身时记得一起改）；
 *   4) 调 robot_ui 注册进来的 hook（板子上是 reminder_sched_add + reload +
 *      界面通知），hook 没注册 -> -ENOSYS，hook 出错 -> **原样透传**它的错误码
 *      （压成笼统的 -EIO 会让工具话术说不清"提醒记不下了"是满额）。
 *
 * host 上没有提醒表、没有界面、也没有 hook，所以 1)~3) 照真身做
 * （used_title 必须写：调用方要把它拼进给模型看的话术，留着没初始化的缓冲
 * 就是让模型念垃圾），4) 换成返回 g_stub.reminder_ret —— 默认 0 = 设好了，
 * 这跟板子上的常态一致（hook 是注册着的）；想走「提醒模块没起来」那条分支就
 * 把它设成 -ENOSYS，想走「满额记不下」就设成 hook 那边的负错误码。 */
int mimo_reminder_apply(const char *hhmm, const char *title, char *used_title,
                        size_t used_title_cap)
{
  char normalized[8];
  char trimmed[84]; /* 20 个汉字的 UTF-8 上界 + 余量，同真身 */
  int hh;
  int mm;

  if (hhmm == NULL || title == NULL)
    {
      return -EINVAL;
    }

  if (sscanf(hhmm, "%d:%d", &hh, &mm) != 2 || hh < 0 || hh > 23 || mm < 0
      || mm > 59)
    {
      return -EINVAL;
    }

  snprintf(normalized, sizeof(normalized), "%02d:%02d", hh, mm);

  if (title[0] == '\0')
    {
      return -EINVAL;
    }

  stub_utf8_trunc(title, trimmed, sizeof(trimmed), 20);

  if (used_title != NULL && used_title_cap > 0)
    {
      snprintf(used_title, used_title_cap, "%s", trimmed);
    }

  g_stub.reminder_calls++;
  snprintf(g_stub.last_reminder_time, sizeof(g_stub.last_reminder_time), "%s",
           normalized);
  snprintf(g_stub.last_reminder_title, sizeof(g_stub.last_reminder_title),
           "%s", trimmed);
  return g_stub.reminder_ret;
}

/* 真身：app/hello_app/voice_emerg.c 的 voice_emerg_trigger()（原型在
 * app/hello_app/voice_emerg.h），host 上这条不需要重现状态机，只需要让链接
 * 成功；真正的报警/铃声逻辑在板子上的 C 文件里做。 */
void voice_emerg_trigger(const char *reason, const char *text)
{
  (void)reason;
  (void)text;
}

/* 真身：app/hello_app/mimo_voice.c 的 mimo_settings_apply()（原型在
 * app/hello_app/mimo_voice.h），host 上只复现参数校验 + 记录返回值；调
 * 真机上的设置页/硬件逻辑不在这里。 */
int mimo_settings_apply(int item, int action, int value)
{
  if (item != MIMO_SET_ITEM_VOLUME && item != MIMO_SET_ITEM_BRIGHTNESS)
    {
      return -EINVAL;
    }

  if (action != MIMO_SET_ACT_UP && action != MIMO_SET_ACT_DOWN
      && action != MIMO_SET_ACT_SET)
    {
      return -EINVAL;
    }

  if (action == MIMO_SET_ACT_SET && (value < 0 || value > 100))
    {
      return -EINVAL;
    }

  g_stub.settings_calls++;
  g_stub.last_setting_item = item;
  g_stub.last_setting_action = action;
  g_stub.last_setting_value = value;
  return g_stub.settings_ret;
}

/* 真身：openvela/packages/ai_agent/src/tools/tool_guard.c 的
 * tool_guard_set_enabled()（原型在同目录 tool_guard.h，host 上用真头）。
 * 真身只做 `claw_config_set("tool_enabled_<name>", enabled ? "1" : "0")`，
 * 名字拼进 64 字节缓冲放不下就返回 ERROR(-1)，其余把 claw_config_set 的
 * 返回值原样透传（0 / 负 errno）。host 上没有配置库，照抄那个 64 字节上限
 * 判一次（超长 -> ERROR，和真身同码），其余由 g_stub.guard_ret 摆。
 *
 * 谁在用它判数：ai_tools_apply_device_policy() 数「返回 OK 的次数」打
 * 「关掉 %d 个用不上的工具」，所以 guard_off_calls 记的就是 enabled=false
 * 的次数 —— 测试要断言「裁剪真的关掉了 N 个」看它。 */
int tool_guard_set_enabled(const char *tool_name, bool enabled)
{
  char key[64]; /* 真身 GUARD_KEY_BUF */
  int n;

  if (tool_name == NULL)
    {
      return ERROR;
    }

  /* 真身这一句是 snprintf(key, sizeof(key), "%s%s", GUARD_KEY_PREFIX,
   * tool_name)，前缀 "tool_enabled_" 共 13 字节；这里照样拼一次只为复现
   * 「拼不下就 ERROR」那条判据，拼出来的 key 本身没人用。 */
  n = snprintf(key, sizeof(key), "%s%s", "tool_enabled_", tool_name);
  if (n < 0 || (size_t)n >= sizeof(key))
    {
      return ERROR;
    }

  g_stub.guard_calls++;
  if (!enabled)
    {
      g_stub.guard_off_calls++;
    }
  snprintf(g_stub.last_guard_tool, sizeof(g_stub.last_guard_tool), "%s",
           tool_name);
  return g_stub.guard_ret;
}
