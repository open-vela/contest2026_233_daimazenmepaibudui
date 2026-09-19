/**
 * test_ai_tools_provider.c - ai_tools_provider.c 的 host 测试（不上板）
 *
 * 为什么能离线测：execute 回调是一段纯逻辑 —— 从 input_json 里取参数、判合法性、
 * 调几个外部出口、把一句话写进 output。三个外部出口（框架 tool_registry、网络、
 * 二次确认入口）在 ai_tools_stubs/ 里全有假实现，调用参数都记在 g_stub 里。
 *
 * 覆盖到的（每一条都能在输出的 [ok] 行里对上）：
 *   A. 注册与幂等：provider 名字、两个回调、invalidate 打脏、重复 init 不重复注册
 *   B. 工具清单 JSON：两个工具、name/description/input_schema 三键齐全、
 *      手拼 JSON 形状正确（引号成对 + 括号配平 + 串里无裸控制字符）
 *   C. 灯控 set_light 的回归：缺参/非法值打回、默认设备、正常下发、ctx 为空
 *   D. report_emergency：reason 必填打回、level 三态（high / normal / 认不出来）、
 *      detail 透传、上报成功/失败、ctx 为空、超长 reason 截断（不留半个 UTF-8）
 *   E. 非我们的工具名一律 ERROR（后面注册的 provider 才能轮到）
 *
 * 没覆盖的（要上板或要别的模块）：
 *   - ui_post_ask_alarm 那一侧真正弹框、用户点确认、确认后走 /alarm：
 *     另一个模块的活；这里只验"我们有没有按约定调它、参数是不是 reason"。
 *   - get_tools 回调返回的串被框架 cJSON 解析 → 下发模型这条链：
 *     框架侧（llm_parse.c 的 build_openai_tools_array()）的活。
 *   - 真板子上的弱符号绑定：weak 那条路（-DNO_ASK_STUB 那份）在 host 上等价于
 *     "对面没落地"，只能证明判空分支不崩；真机是否绑到对面那个强符号要靠整机链接。
 *
 * ⚠️ 红线：stub 里**只有** ai_network_report_sound_alarm()，没有
 * ai_network_send_alarm()。紧急报告工具一旦改成调后者（会推手机、弹报警页，
 * 等于绕过二次确认），这份测试会直接链接失败 —— 把"没确认前不许真报警"钉在编译期。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 假的 <nuttx/config.h>（在 ai_tools_stubs/ 里）—— 提供 OK / ERROR，
 * 以及真机固件在用的 CONFIG_HELLO_APP_LLM_AI_AGENT */
#include <nuttx/config.h>

#include "ai_tools_host_stub.h"
#include "ai_tools_provider.h"

static int g_checks;

/* 只当"非空指针"用：stub 里的出口不解引用 ctx */
static ai_network_context_t g_ctx;

static void check(int ok, const char *what)
{
  g_checks++;

  if (!ok)
    {
      printf("  [FAIL] %s\n", what);
      fflush(stdout);
      exit(1);
    }

  printf("  [ok] %s\n", what);
}

static int asks_available(void)
{
#ifdef NO_ASK_STUB
  return 0;
#else
  return 1;
#endif
}

/* 手拼 JSON 的极小形状检查（不引 cJSON，理由见 ai_tools_provider.c 文件头）：
 * 引号成对、转义只在串内、{} [] 配平、串里没有裸控制字符。 */
static int json_shape_ok(const char *s)
{
  int in_str = 0;
  int esc = 0;
  int depth = 0;
  const char *p;

  for (p = s; *p != '\0'; p++)
    {
      unsigned char c = (unsigned char)*p;

      if (in_str)
        {
          if (esc)
            {
              esc = 0;
            }
          else if (c == '\\')
            {
              esc = 1;
            }
          else if (c == '"')
            {
              in_str = 0;
            }
          else if (c < 0x20)
            {
              return 0;
            }

          continue;
        }

      if (c == '"')
        {
          in_str = 1;
        }
      else if (c == '{' || c == '[')
        {
          depth++;
        }
      else if (c == '}' || c == ']')
        {
          depth--;
          if (depth < 0)
            {
              return 0;
            }
        }
    }

  return (in_str == 0 && esc == 0 && depth == 0) ? 1 : 0;
}

static int utf8_valid(const char *s)
{
  const unsigned char *p = (const unsigned char *)s;

  while (*p != '\0')
    {
      unsigned char c = *p++;
      int need;

      if (c < 0x80)
        {
          continue;
        }
      else if (c >= 0xc2 && c <= 0xdf)
        {
          need = 1;
        }
      else if (c >= 0xe0 && c <= 0xef)
        {
          need = 2;
        }
      else if (c >= 0xf0 && c <= 0xf4)
        {
          need = 3;
        }
      else
        {
          return 0;
        }

      while (need-- > 0)
        {
          if ((*p & 0xc0) != 0x80)
            {
              return 0;
            }

          p++;
        }
    }

  return 1;
}

static int count_occurrences(const char *hay, const char *needle)
{
  int n = 0;
  size_t nl = strlen(needle);
  const char *p = hay;

  while ((p = strstr(p, needle)) != NULL)
    {
      n++;
      p += nl;
    }

  return n;
}

static int exec_tool(const char *name, const char *input, char *out,
                     size_t cap)
{
  if (out != NULL && cap > 0)
    {
      out[0] = '\0';
    }

  return g_stub.execute(name, input, out, cap);
}

/* ── A. 注册与幂等 ───────────────────────────────────────── */

static void test_register(void)
{
  printf("[A] 注册与幂等\n");

  check(ai_tools_provider_init() == 0, "init 返回 0");
  check(strcmp(g_stub.provider_name, AI_TOOLS_PROVIDER_NAME) == 0,
        "provider 名字是 \"contest\"（静态存储期，不是栈上的）");
  check(g_stub.provider_count == 1, "register_provider 调了 1 次");
  check(g_stub.execute != NULL, "execute 回调交上去了");
  check(g_stub.invalidate_calls == 1, "注册完打了脏（不然模型看不到新工具）");

  check(ai_tools_provider_init() == 0, "重复 init 返回 0");
  check(g_stub.provider_count == 1, "重复 init 不重复注册（provider 表只有 4 格）");
  check(g_stub.invalidate_calls == 1, "重复 init 不再打脏");
}

/* ── B. 工具清单 JSON ────────────────────────────────────── */

static void test_tools_json(void)
{
  char *json;

  printf("[B] 工具清单 JSON\n");

  json = host_stub_take_tools_json();
  check(json != NULL, "get_tools 回调返回了串");

  check(json_shape_ok(json), "手拼的 JSON 形状正确（引号/括号/无裸控制字符）");
  check(json[0] == '[' && json[strlen(json) - 1] == ']',
        "清单是一个 JSON 数组（框架要的就是数组）");
  check(count_occurrences(json, "{\"name\":\"") == 2,
        "两个工具元素都以 {\"name\":\" 开头");
  check(count_occurrences(json, "\"name\":") == 2, "清单里 2 个工具名");
  check(count_occurrences(json, "\"description\":") >= 2,
        "两个工具都带 description（schema 内部的字段描述不计）");
  check(count_occurrences(json, "\"input_schema\":") == 2, "2 个 input_schema");

  check(strstr(json, "\"name\":\"" AI_TOOL_SET_LIGHT "\"") != NULL,
        "set_light 还在清单里");
  check(strstr(json, "\"name\":\"" AI_TOOL_REPORT_EMERGENCY "\"") != NULL,
        "report_emergency 在清单里");
  check(strstr(json, "\"required\":[\"command\"]") != NULL,
        "set_light 仍要求 command");
  check(strstr(json, "\"enum\":[\"on\",\"off\"]") != NULL,
        "set_light 的 command 枚举还是 on/off");
  check(strstr(json, "\"required\":[\"reason\"]") != NULL,
        "report_emergency 要求 reason");
  check(strstr(json, "\"enum\":[\"high\",\"normal\"]") != NULL,
        "level 枚举是 high/normal");
  check(strstr(json, "detail") != NULL, "detail 字段在 schema 里");
  check(strstr(json, "确认") != NULL && strstr(json, "不要反复调用") != NULL,
        "description 里写明了「要用户确认、不要反复调用」（防模型编成功）");

  free(json);
}

/* ── E. 不是我们的工具 ───────────────────────────────────── */

static void test_unknown_tool(void)
{
  char out[512];

  printf("[E] 不是我们的工具名\n");

  check(exec_tool("light_on", "{}", out, sizeof(out)) == ERROR,
        "认不出来的名字回 ERROR（后面的 provider 才有机会）");
  check(exec_tool(NULL, "{}", out, sizeof(out)) == ERROR, "name 为 NULL 回 ERROR");
  check(exec_tool(AI_TOOL_SET_LIGHT, NULL, NULL, 0) == ERROR,
        "output/output_size 不合法回 ERROR（不写空指针）");
}

/* ── C. set_light 回归 ───────────────────────────────────── */

static void test_set_light(void)
{
  char out[512];

  printf("[C] set_light 回归\n");

  host_stub_reset_calls();
  ai_tools_provider_init();
  ai_tools_provider_set_network_ctx(&g_ctx);

  check(exec_tool(AI_TOOL_SET_LIGHT, "{}", out, sizeof(out)) == OK,
        "缺 command 回 OK（把原因写进文本，不当成未知工具）");
  check(strstr(out, "command") != NULL, "打回文本里点明了 command");
  check(g_stub.cmd_calls == 0, "参数不合法时一包都不发");

  check(exec_tool(AI_TOOL_SET_LIGHT, "{\"command\":\"blink\"}", out,
                  sizeof(out)) == OK, "非法 command 回 OK");
  check(strstr(out, "on") != NULL && strstr(out, "off") != NULL,
        "打回文本里给出了合法取值");
  check(g_stub.cmd_calls == 0, "非法 command 也不发");

  check(exec_tool(AI_TOOL_SET_LIGHT, "{\"command\":\"on\"}", out,
                  sizeof(out)) == OK, "command=on 执行成功");
  check(g_stub.cmd_calls == 1, "走了一次 send_device_command");
  check(strcmp(g_stub.last_device, AI_TOOLS_LIGHT_DEVICE_ID) == 0,
        "没说设备时用默认灯 living_room_light");
  check(strcmp(g_stub.last_command, "on") == 0, "command 是裸 on 串");
  check(strstr(out, "已交给网络层") != NULL, "回文本只说「交给网络层」不下结论");

  check(exec_tool(AI_TOOL_SET_LIGHT,
                  "{\"device_id\":\"kitchen_light\",\"command\":\"off\"}",
                  out, sizeof(out)) == OK, "带着 device_id 执行成功");
  check(strcmp(g_stub.last_device, "kitchen_light") == 0, "device_id 透传");
  check(strcmp(g_stub.last_command, "off") == 0, "command=off 透传");

  g_stub.cmd_ret = -1;
  check(exec_tool(AI_TOOL_SET_LIGHT, "{\"command\":\"on\"}", out,
                  sizeof(out)) == OK, "网络层失败也回 OK");
  check(strstr(out, "没执行") != NULL, "失败时如实说没执行");
  g_stub.cmd_ret = 0;

  ai_tools_provider_set_network_ctx(NULL);
  check(exec_tool(AI_TOOL_SET_LIGHT, "{\"command\":\"on\"}", out,
                  sizeof(out)) == OK, "ctx 为空回 OK");
  check(strstr(out, "网络") != NULL, "ctx 为空时说明没接上网络模块");
  check(g_stub.cmd_calls == 3, "ctx 为空时不再调发送口");
}

/* ── D. report_emergency ────────────────────────────────── */

static void test_report_emergency_bad_args(void)
{
  char out[512];

  printf("[D1] report_emergency 参数打回\n");

  host_stub_reset_calls();
  ai_tools_provider_init();
  ai_tools_provider_set_network_ctx(&g_ctx);

  check(exec_tool(AI_TOOL_REPORT_EMERGENCY, "{}", out, sizeof(out)) == OK,
        "缺 reason 回 OK（模型能读懂原因，而不是收到英文未知工具）");
  check(strstr(out, "reason") != NULL, "打回文本里点明了 reason");
  check(g_stub.ask_calls == 0, "reason 都没有时不弹询问框");
  check(g_stub.report_calls == 0, "reason 都没有时不上报");

  check(exec_tool(AI_TOOL_REPORT_EMERGENCY, "{\"reason\":\"\"}", out,
                  sizeof(out)) == OK, "reason 空串回 OK");
  check(g_stub.ask_calls == 0 && g_stub.report_calls == 0, "空串不弹框不上报");

  check(exec_tool(AI_TOOL_REPORT_EMERGENCY, "{\"reason\":\"   \"}", out,
                  sizeof(out)) == OK, "reason 只有空格也算空");
  check(g_stub.ask_calls == 0, "纯空格不弹框");

  check(exec_tool(AI_TOOL_REPORT_EMERGENCY, "{\"reason\":123}", out,
                  sizeof(out)) == OK, "reason 不是字符串时回 OK（打回）");
  check(g_stub.ask_calls == 0, "类型不对时不弹框");

  check(exec_tool(AI_TOOL_REPORT_EMERGENCY, NULL, out, sizeof(out)) == OK,
        "input_json 为 NULL 时也不崩，回 OK");
  check(strstr(out, "reason") != NULL, "NULL 输入同样打回 reason");
}

static void test_report_emergency_ok(void)
{
  char out[2048];

  printf("[D2] report_emergency 正常路径（high / normal）\n");

  host_stub_reset_calls();
  ai_tools_provider_init();
  ai_tools_provider_set_network_ctx(&g_ctx);

  /* high */
  check(exec_tool(AI_TOOL_REPORT_EMERGENCY,
                  "{\"reason\":\"老人呼救\",\"level\":\"high\","
                  "\"detail\":\"连着喊了三声\"}", out, sizeof(out)) == OK,
        "high 级别执行成功");

  if (asks_available())
    {
      check(g_stub.ask_calls == 1, "调了一次二次确认入口");
      check(strcmp(g_stub.last_ask_reason, "老人呼救") == 0,
            "询问框拿到的是 reason 原文");
    }
  else
    {
      check(g_stub.ask_calls == 0, "对面没落地时不假装弹过框");
      check(strstr(out, "没能请用户确认") != NULL,
            "回文本如实说询问框没弹出来");
    }

  check(g_stub.report_calls == 1, "上报了一次事件");
  check(strcmp(g_stub.last_sound_type, "unconfirmed_llm_sos_high") == 0,
        "high 用 unconfirmed_llm_sos_high");
  check(g_stub.last_confidence == 100, "high 的 confidence 是 100");
  check(strstr(out, "老人呼救") != NULL, "回文本里有 reason");
  check(strstr(out, "连着喊了三声") != NULL, "回文本里有 detail");
  check(strstr(out, "high") != NULL, "回文本里说明了 level");
  check(strstr(out, "还没有真正报警") != NULL, "回文本写清「还没有真正报警」");
  check(strstr(out, "不要说已经报警成功") != NULL,
        "回文本明确禁止模型编造「已经报警成功」");
  check(strstr(out, "不要再调用本工具") != NULL, "回文本禁止模型重复调用");
  check(utf8_valid(out), "回文本是合法 UTF-8");

  /* normal：不传 level */
  host_stub_reset_calls();
  ai_tools_provider_set_network_ctx(&g_ctx);
  check(exec_tool(AI_TOOL_REPORT_EMERGENCY, "{\"reason\":\"疑似跌倒\"}", out,
                  sizeof(out)) == OK, "不传 level 也能执行");
  check(strcmp(g_stub.last_sound_type, "unconfirmed_llm_sos") == 0,
        "默认按 normal 上报");
  check(g_stub.last_confidence == 60, "normal 的 confidence 是 60");

  /* level 是别的词：按 normal 走，但在文本里说一声 */
  host_stub_reset_calls();
  ai_tools_provider_set_network_ctx(&g_ctx);
  check(exec_tool(AI_TOOL_REPORT_EMERGENCY,
                  "{\"reason\":\"胸口疼\",\"level\":\"urgent\"}", out,
                  sizeof(out)) == OK, "level 认不出来时照样执行（不打回）");
  check(strcmp(g_stub.last_sound_type, "unconfirmed_llm_sos") == 0,
        "认不出来的 level 按 normal 上报");
  check(strstr(out, "level") != NULL && strstr(out, "normal") != NULL,
        "回文本里说明了 level 被按 normal 处理");

  /* level 给空串同样走 normal */
  host_stub_reset_calls();
  check(exec_tool(AI_TOOL_REPORT_EMERGENCY,
                  "{\"reason\":\"头晕\",\"level\":\"\"}", out,
                  sizeof(out)) == OK, "level 空串回 OK");
  check(g_stub.report_calls == 1, "level 空串仍然上报（紧急链路不因装饰字段断）");
}

static void test_report_emergency_net(void)
{
  char out[2048];

  printf("[D3] report_emergency 上报出口的三种结果\n");

  host_stub_reset_calls();
  ai_tools_provider_init();
  ai_tools_provider_set_network_ctx(&g_ctx);

  /* 上报失败 */
  g_stub.report_ret = -1;
  check(exec_tool(AI_TOOL_REPORT_EMERGENCY, "{\"reason\":\"老人呼救\"}", out,
                  sizeof(out)) == OK, "上报失败也回 OK");
  check(g_stub.report_calls == 1, "确实试过上报");
  check(strstr(out, "网络层") != NULL && strstr(out, "-1") != NULL,
        "回文本写清事件没交给网络层（含返回值）");
  g_stub.report_ret = 0;

  /* ctx 为空：框照弹，事件没上报 */
  host_stub_reset_calls();
  ai_tools_provider_set_network_ctx(NULL);
  check(exec_tool(AI_TOOL_REPORT_EMERGENCY, "{\"reason\":\"老人呼救\"}", out,
                  sizeof(out)) == OK, "ctx 为空回 OK");
  check(g_stub.report_calls == 0, "ctx 为空时不调上报口");
  check(strstr(out, "网络上下文") != NULL, "回文本说明网络上下文没注册");
  if (asks_available())
    {
      check(g_stub.ask_calls == 1, "ctx 为空也照样请用户确认（本地那条路不依赖网络）");
    }

  /* 事件名：确认不会走上 /alarm（会推手机的那条） */
  host_stub_reset_calls();
  ai_tools_provider_set_network_ctx(&g_ctx);
  check(exec_tool(AI_TOOL_REPORT_EMERGENCY, "{\"reason\":\"老人呼救\"}", out,
                  sizeof(out)) == OK, "再跑一次正常路径");
  check(g_stub.last_sound_type[0] != '\0' &&
        strncmp(g_stub.last_sound_type, "unconfirmed_", 12) == 0,
        "上报的事件名带 unconfirmed_ 前缀（家人那边一眼看出不是正式报警）");
}

static void test_report_emergency_long_reason(void)
{
  char input[1024];
  char out[2048];
  char *w = input;
  int i;
  int ret;

  printf("[D4] report_emergency 超长 reason 的截断\n");

  host_stub_reset_calls();
  ai_tools_provider_init();
  ai_tools_provider_set_network_ctx(&g_ctx);

  w += sprintf(w, "{\"reason\":\"");
  for (i = 0; i < 120; i++)
    {
      memcpy(w, "啊", 3);        /* 120 个汉字 = 360 字节，远超 96 字节上限 */
      w += 3;
    }
  w += sprintf(w, "\"}");

  ret = exec_tool(AI_TOOL_REPORT_EMERGENCY, input, out, sizeof(out));

  check(ret == OK, "超长 reason 不判错（紧急链路不许被长文本堵死）");
  check(count_occurrences(out, "啊") < 120, "回文本里的 reason 确实被截断了");
  check(utf8_valid(out), "截断后回给模型的文本仍是合法 UTF-8（没留半个汉字）");
  check(g_stub.report_calls == 1, "截断之后照样上报");

  if (asks_available())
    {
      check(utf8_valid(g_stub.last_ask_reason),
            "询问框拿到的 reason 也是合法 UTF-8");
      check(strlen(g_stub.last_ask_reason) < 96,
            "询问框拿到的 reason 在缓冲上限之内");
    }
}

int main(void)
{
  printf("== ai_tools_provider host 测试%s ==\n",
         asks_available() ? "" : "（ui_post_ask_alarm 未落地版）");

  test_register();
  test_tools_json();
  test_unknown_tool();
  test_set_light();
  test_report_emergency_bad_args();
  test_report_emergency_ok();
  test_report_emergency_net();
  test_report_emergency_long_reason();

  printf("全部通过：%d 项检查\n", g_checks);
  return 0;
}
