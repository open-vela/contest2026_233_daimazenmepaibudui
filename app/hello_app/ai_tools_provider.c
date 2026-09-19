/****************************************************************************
 * AI Tools Provider Implementation
 * 智爱陪伴 - 把我们的工具注册进 openvela 框架的 ai_agent 工具表
 *
 * 实现要点（背景见 ai_tools_provider.h 的「运行时机」）：
 *
 *   1. 本文件是 hello_app 编的，但 execute 回调**跑在 ai_agent 的任务里** ——
 *      ai_agent 的 agent_loop_task 收到消息后会重新取一次工具清单
 *      （tool_registry_get_tools_json()），把清单拼进请求；模型要求调用工具时
 *      由 tool_registry_execute() 先查内置工具、再查 MCP、最后才轮到我们这些
 *      注册进来的 provider。所以回调和 ai_companion 的主循环是并发的：
 *      这里既不能碰 LVGL，也不能碰 hello_app 的界面状态。
 *
 *   2. JSON 全部手拼，**不用 cJSON**：hello_app 的编译参数里没有 cJSON 的
 *      include 路径（mimo_voice.c / mimo_location.c 的文件头都记着这条），
 *      而且工具参数只有两个短字符串，引 cJSON 换来的是一份额外的解析代码和
 *      堆拷贝，板子上不划算。
 *
 *   3. 工具清单的 JSON 形状是框架定的：数组元素取 name / description /
 *      input_schema 三个键（llm_parse.c 的 build_openai_tools_array() 就是按
 *      这三个键转成 OpenAI 的 function 格式）。input_schema 是标准 JSON
 *      Schema。
 *
 *   4. 工具执行**不吞错**：网络没连上、参数不合法，都把原因写进 output 回给
 *      模型，让它用自己的话告诉老人（返回 ERROR 会让工具调用链断掉，老人
 *      只会等到一句干巴巴的失败）。只有「这个名字不是我们的工具」才回
 *      ERROR，让后面注册的 provider 有机会兜。
 *
 * 返回给框架的清单字符串必须是用 malloc 出来的（框架那边按 free() 释放）。
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "ai_tools_provider.h"

/* 框架的工具 provider 接口（tool_registry_register_provider / _invalidate）。
 * -I<ai_agent>/src 只在开了 CONFIG_HELLO_APP_LLM_AI_AGENT 时才加进编译命令，
 * 所以这一条 include 和下面所有用到框架的部分都得跟着条件编译 —— 否则
 * 换个不启用 ai_agent 的配置，hello_app 会因为找不到头文件直接编不过。 */

#ifdef CONFIG_HELLO_APP_LLM_AI_AGENT
#  include "tools/tool_registry.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 日志。工具执行在 ai_agent 的任务里，printf 走的是同一条串口（平铺构建），
 * 所以这里的输出会和 ai_agent 的日志混在一起，加个前缀方便捞。 */

#define ai_tools_info(fmt, ...)  printf("[AI_TOOLS] " fmt "\n", ##__VA_ARGS__)

/* device_id 最长多少字节（够装 "living_room_light" 这类标识，多出来的截断
 * 反而会发出一台不存在的设备，不如直接打回去让模型重说） */
#define AI_TOOLS_DEVICE_MAX     64

/* command 最长多少字节。on / off 各 2 字节，留点余量只为判断"用户给的长"
 * 这种情况，多余的一律按不合法处理。 */
#define AI_TOOLS_COMMAND_MAX    16

/* report_emergency 的参数上限（字节，含结尾 '\0'）。
 * reason 会显示在询问框上、也会进日志（"老人呼救""疑似跌倒"），detail 是模型
 * 补的描述。两个都超长**截断**而不是打回 —— 走到这里说明模型已经认定这是紧急
 * 情况，不能因为一句话太长把这条链堵住（见 ai_tools_copy_text()）。 */
#define AI_TOOLS_REASON_MAX     96
#define AI_TOOLS_DETAIL_MAX     128

/* level 只认这两个值。拷贝走严格版（纯 ASCII 记号），认不出来的一律按 normal
 * 处理并在回给模型的文本里说一声 —— 同样是不为了装饰性字段把链路打回。 */
#define AI_TOOLS_LEVEL_MAX      16
#define AI_TOOLS_LEVEL_HIGH     "high"
#define AI_TOOLS_LEVEL_NORMAL   "normal"

/* 事件上报用的 sound_type。走 /sound_alarm 这条通道（不上报 /alarm、不推手机），
 * 命名沿用 ai_companion_main.c 里"未确认"那套 unconfirmed_<type>：一眼能看出
 * 这条**不是**正式报警。level=high 单独一个名字，家人那边不看 confidence 也能
 * 分出紧急程度。
 *
 * 为什么不上报 /alarm：那条路会推手机推送 + 弹报警页（ai_network_send_alarm()
 * 内部是 report_alarm() -> push_send_alarm()），用户还没确认就推过去，二次确认
 * 就白做了。"未确认"走 /sound_alarm 是 ai_companion_main.c 里已经定下来的规矩
 * （ask_flow_unclear() 那段有原话）。 */
#define AI_TOOLS_EVENT_SOS       "unconfirmed_llm_sos"
#define AI_TOOLS_EVENT_SOS_HIGH  "unconfirmed_llm_sos_high"

/* 工具清单的 JSON。description 是写给模型看的，里面一律用中文标点 —— 这是
 * 手写的 JSON 字面量，一个 ASCII 引号或反斜杠就能把它拼坏。
 *
 * 清单里每个元素是框架要的形状：name + description + input_schema。 */

#define AI_TOOLS_SET_LIGHT_JSON                                          \
  "{\"name\":\"" AI_TOOL_SET_LIGHT "\","                                 \
  "\"description\":\"打开或关掉屋子里的灯。用户说开灯、关灯、把灯打开、"                       \
  "把灯关掉、太暗了、屋里黑之类的话时调用。device_id 一般不用传，"                        \
  "用户没说具体哪盏灯就用默认的那盏。\","                                        \
  "\"input_schema\":{\"type\":\"object\",\"properties\":{"               \
  "\"device_id\":{\"type\":\"string\",\"description\":\"灯的标识，例如 "     \
  AI_TOOLS_LIGHT_DEVICE_ID "；用户没说哪盏灯就不要传这个字段，"                 \
  "板子会自己用默认值。\"},"                                              \
  "\"command\":{\"type\":\"string\",\"description\":\"on 表示开灯，"      \
  "off 表示关灯，这两个之外的值一律无效。\","                                   \
  "\"enum\":[\"on\",\"off\"]}},"                                        \
  "\"required\":[\"command\"]}}"

/* 紧急报告工具。description 必须写清两件事：什么时候该调（老人求救这类），
 * 以及"调它只是请用户确认、不是已经报了警" —— 少了后半句，模型很容易在老人
 * 喊救命时反复调用它，或者回一句"已经帮您报警了"（用户实际还没点确认）。 */
#define AI_TOOLS_REPORT_EMERGENCY_JSON                                   \
  "{\"name\":\"" AI_TOOL_REPORT_EMERGENCY "\","                          \
  "\"description\":\"报告一次紧急情况，让板子马上去问用户「是否要报警？」、"          \
  "并在用户确认之后真正报警。老人喊救命、呼救、说胸口疼、说摔倒了、"                     \
  "或者你判断用户正处于危险之中（疑似跌倒、受伤、出血）时调用。"                        \
  "注意：调用本工具只是**请用户确认**，报警要等用户点头，所以不要反复调用，"              \
  "也不要说已经报过警了。\","                                             \
  "\"input_schema\":{\"type\":\"object\",\"properties\":{"               \
  "\"reason\":{\"type\":\"string\",\"description\":\"这次紧急情况是什么，" \
  "一句简短中文，例如「老人呼救」「疑似跌倒」。\"},"                            \
  "\"level\":{\"type\":\"string\",\"description\":\"紧急程度："          \
  "high 表示非常紧急（正在呼救、可能已经受伤），normal 表示需要再确认；"          \
  "不确定就不传。\","                                                    \
  "\"enum\":[\"" AI_TOOLS_LEVEL_HIGH "\",\"" AI_TOOLS_LEVEL_NORMAL "\"]}," \
  "\"detail\":{\"type\":\"string\",\"description\":\"补充描述，"         \
  "例如老人原话、你的判断依据。可选。\"}},"                                 \
  "\"required\":[\"reason\"]}}"

#define AI_TOOLS_JSON_ARRAY                                              \
  "[" AI_TOOLS_SET_LIGHT_JSON "," AI_TOOLS_REPORT_EMERGENCY_JSON "]"

/****************************************************************************
 * External Symbols
 ****************************************************************************/

/* 板端二次确认入口（实现在 app/robot_ui/main.c，原型声明在那边的 robot_ui.h）。
 *
 * 这里**不 include robot_ui.h**：那个头文件要 <lvgl.h>，而 hello_app 的编译
 * 命令里没有 LVGL 的头文件路径（理由见 robot_ui_bridge.h / fall_alarm.h 的
 * 文件头），只能自己声明一份，签名必须和对面一模一样。
 *
 * weak 的理由：这个符号长在另一个模块里，谁先合进来不该决定整机镜像能不能
 * 链起来。加 weak 之后，对面没落地时函数地址是 NULL（下面判空后如实告诉模型
 * "询问框没弹出来"，而不是假装弹过），落地后自动绑到对面那个强符号上 ——
 * 对面现在已经落地（main.c 的 ui_post_ask_alarm()），host 测试里验过这条绑定。
 *
 * 线程约定（对面给的）：任何任务线程可调，内部只投递不碰 LVGL；不许在中断/
 * 音频回调里调。execute 回调跑在 ai_agent 的 agent_loop 任务里，符合。 */

__attribute__((weak))
void ui_post_ask_alarm(const char *reason);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 往哪张网络上下文上发设备命令。由 ai_tools_provider_set_network_ctx() 在
 * ai_companion 初始化阶段写一次，之后只读 —— 读它的可能是 ai_agent 的任务，
 * 所以约定"设完不再改"（见头文件）。 */

static ai_network_context_t *g_net_ctx = NULL;

/* 已经注册过没有。provider 表只有 4 个名额，而且它没有反注册接口，
 * 重复注册的后果是工具清单里出现两份同名工具（服务端可能直接 400）。
 * 只有真的能注册的那种配置才需要这个标志，否则它就是个没人看的静态变量。 */

#ifdef CONFIG_HELLO_APP_LLM_AI_AGENT
static bool g_registered = false;
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * @brief  在 JSON 串里找一个字符串字段的原始字节（不复制、不反转义）
 *
 * 写法照抄 mimo_voice.c 里那份最小的实现（那边不引 cJSON 的理由在这里同样
 * 成立）：先找 "key"，跳过冒号和空白，拿引号之间的原始字节，跳过 \" 。
 * 找不到 / 类型不对都返回 NULL。
 *
 * @param  json     被查的 JSON（NULL 当"没有"）
 * @param  key      字段名
 * @param  out_len  输出：字段值的字节数（不含引号）
 * @return 指向值的指针，NULL 表示没有这个字段
 */

static const char *ai_tools_json_find_string(const char *json, const char *key,
                                             size_t *out_len)
{
  char pat[32];
  const char *p;

  if (json == NULL || key == NULL || out_len == NULL)
    {
      return NULL;
    }

  if (strlen(key) + 3 > sizeof(pat))
    {
      return NULL;
    }

  snprintf(pat, sizeof(pat), "\"%s\"", key);

  p = strstr(json, pat);
  if (p == NULL)
    {
      return NULL;
    }

  p += strlen(pat);

  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    {
      p++;
    }

  if (*p != ':')
    {
      return NULL;
    }

  p++;

  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    {
      p++;
    }

  if (*p != '"')
    {
      return NULL;
    }

  p++;

  {
    const char *start = p;

    while (*p != '\0' && *p != '"')
      {
        if (*p == '\\' && p[1] != '\0')
          {
            p++;
          }

        p++;
      }

    if (*p != '"')
      {
        return NULL;
      }

    *out_len = (size_t)(p - start);
    return start;
  }
}

/**
 * @brief  把一个字段值拷进定长缓冲
 *
 * 只接受**纯 ASCII 记号**：带反斜杠（转义序列）或者长度越界的都当场判不合法。
 * 我们的参数本来就是 on / off / 设备标识这类记号，没有必要实现完整的反转义；
 * 反过来，装作支持转义却只做半套，会让 \" 这种输入悄悄变成一个引号，不如直接
 * 拒绝、让模型重新说一遍。
 *
 * @param  src      ai_tools_json_find_string() 给的原始字节
 * @param  len      字节数
 * @param  dst      输出缓冲（一定以 '\0' 结尾）
 * @param  dst_cap  输出缓冲大小
 * @return true 拷贝成功；false 值不合法或装不下
 */

static bool ai_tools_copy_token(const char *src, size_t len, char *dst,
                                size_t dst_cap)
{
  size_t i;

  if (src == NULL || dst == NULL || dst_cap == 0)
    {
      return false;
    }

  dst[0] = '\0';

  if (len == 0 || len >= dst_cap)
    {
      return false;
    }

  for (i = 0; i < len; i++)
    {
      unsigned char c = (unsigned char)src[i];

      if (c < 0x20 || c == '\\')
        {
          return false;
        }

      dst[i] = (char)c;
    }

  dst[len] = '\0';
  return true;
}

/**
 * @brief  把一个**人话**字段（reason / detail）拷进定长缓冲
 *
 * 和 ai_tools_copy_token() 的区别：那个是给 on / off / 设备标识这种纯 ASCII
 * 记号用的，超长和反斜杠一律判不合法；这里收的是中文描述，规则反过来：
 *
 *   - 0x80 以上的字节（UTF-8 中文）原样收；ASCII 控制字符（含 \n \t）丢掉；
 *   - 空白只去首尾（模型有时会带空格），中间的原样保留；
 *   - 超长**截断**而不是打回：能走到这里说明模型已经认定这是紧急情况，不能
 *     因为一句话太长就把这条链堵住（何况 detail 只是个补充）；
 *   - 截断一定落在字符边界上：装不下时把正在写的那个多字节字符整个回退。
 *     留半个 UTF-8 字符比少半句话更坏 —— 它会一路进到 cJSON 拼的 MQTT 报文
 *     和界面的显示缓冲里，那边都不做校验；
 *   - 反斜杠原样保留（不做转义处理，也不否定它）：下游的 cJSON 会正确转义。
 *
 * @param  src      ai_tools_json_find_string() 给的原始字节（可以是 NULL）
 * @param  len      字节数
 * @param  dst      输出缓冲（一定以 '\0' 结尾）
 * @param  dst_cap  输出缓冲大小（至少留一格给 '\0'）
 * @return 真正拷进 dst 的字节数；0 表示没有可用内容
 */

static size_t ai_tools_copy_text(const char *src, size_t len, char *dst,
                                 size_t dst_cap)
{
  size_t used = 0;
  size_t i;
  size_t seq_start = 0;
  size_t seq_need = 0;

  if (dst == NULL || dst_cap == 0)
    {
      return 0;
    }

  dst[0] = '\0';

  if (src == NULL)
    {
      return 0;
    }

  for (i = 0; i < len; i++)
    {
      unsigned char c = (unsigned char)src[i];

      if (c < 0x20)
        {
          continue;
        }

      if (used + 2 > dst_cap)
        {
          /* 装不下（还要留一格给结尾的 '\0'）：正在写的多字节字符整个回退 */

          if (seq_need > 0)
            {
              used = seq_start;
            }

          break;
        }

      dst[used++] = (char)c;

      if (c >= 0xc2 && c <= 0xf4)
        {
          /* 一个多字节序列的引导字节，记住它从哪开始、还要几个续字节 */

          seq_start = used - 1;
          seq_need  = (c < 0xe0) ? 1 : ((c < 0xf0) ? 2 : 3);
        }
      else if (c >= 0x80 && c <= 0xbf)
        {
          if (seq_need > 0)
            {
              seq_need--;
            }
        }
      else
        {
          seq_need = 0;      /* ASCII：不可能还在半个序列里 */
        }
    }

  while (used > 0 && (dst[used - 1] == ' ' || dst[used - 1] == '\t'))
    {
      used--;
    }

  dst[used] = '\0';
  return used;
}

/**
 * @brief  框架的 get_tools 回调：交回我们的工具清单
 *
 * 每次重建清单都会调一次，所以这里每次都返回一份新的堆内存（框架负责 free）。
 * 清单是编译期常量，不用加锁。
 *
 * @return malloc 出来的 JSON 数组字符串；NULL 表示这次没有工具
 */

static char *ai_tools_provider_get_tools(void)
{
  size_t len = strlen(AI_TOOLS_JSON_ARRAY) + 1;
  char *out = malloc(len);

  if (out == NULL)
    {
      return NULL;
    }

  memcpy(out, AI_TOOLS_JSON_ARRAY, len);
  return out;
}

/**
 * @brief  report_emergency 的执行体：请用户确认 + 上报一条「未确认」事件
 *
 * 这一层只做两件事，**真正报警不在这里**：
 *   1. ui_post_ask_alarm(reason) —— 让界面弹「是否报警？」询问框（对面实现的
 *      入口，任何任务线程可调）；用户点了确认，才由界面那一侧正式报警；
 *   2. ai_network_report_sound_alarm() —— 往 /sound_alarm 上报一条**不推手机**
 *      的事件，让家人/云端知道"模型报了紧急情况、正在等用户确认"。
 *
 * 两步都失败也要各自如实说清：模型会照着我们的文本跟老人说话，这里含糊一句，
 * 老人那边就会听到"已经帮您报警了"。
 *
 * @param  input_json   模型给的参数（JSON 对象串；可能为 NULL）
 * @param  output       工具结果（回给模型看的文本）
 * @param  output_size  output 的大小
 * @return 恒为 OK（工具名在 execute 里已经认过；失败原因全部写进 output）
 */

static int ai_tools_report_emergency(const char *input_json, char *output,
                                     size_t output_size)
{
  char reason[AI_TOOLS_REASON_MAX];
  char detail[AI_TOOLS_DETAIL_MAX];
  char level[AI_TOOLS_LEVEL_MAX];
  char net_state[96];
  const char *p;
  const char *ask_state;
  size_t len = 0;
  bool high = false;
  bool level_unknown = false;
  int ret;

  output[0] = '\0';

  /* reason 必填：它是询问框上那句话，也是给界面的唯一线索。缺了就把原因写成
   * 一句人话回给模型，让它重说一遍（返回 ERROR 会走成"未知工具"，模型只会
   * 看到一句英文报错，老人那边什么都等不到）。 */

  p = ai_tools_json_find_string(input_json, "reason", &len);

  if (ai_tools_copy_text(p, len, reason, sizeof(reason)) == 0)
    {
      snprintf(output, output_size,
               "没执行：reason 参数缺失或者是空的。请用一句简短中文说清这次"
               "紧急情况（例如「老人呼救」「疑似跌倒」）之后重新调用一次。");
      ai_tools_info("report_emergency: reason 缺失，打回");
      return OK;
    }

  /* level 可选：只认 high / normal。认不出来（没给、给了别的词、给了非字符串）
   * 一律按 normal 走，并在回给模型的文本里说一声 —— 这是紧急链路，不为了一个
   * 装饰性字段把上报打回。 */

  p = ai_tools_json_find_string(input_json, "level", &len);

  if (p != NULL)
    {
      if (ai_tools_copy_token(p, len, level, sizeof(level)))
        {
          if (strcmp(level, AI_TOOLS_LEVEL_HIGH) == 0)
            {
              high = true;
            }
          else if (strcmp(level, AI_TOOLS_LEVEL_NORMAL) != 0)
            {
              level_unknown = true;
            }
        }
      else
        {
          level_unknown = true;
        }
    }

  /* detail 可选：模型的补充描述。只进日志和回给模型的文本，不进上报报文
   * （现成的上报入口只收 sound_type + confidence 两个参数，见下）。 */

  p = ai_tools_json_find_string(input_json, "detail", &len);
  ai_tools_copy_text(p, len, detail, sizeof(detail));

  /* 1. 请用户确认。weak 符号为空时（对面还没落地、或者将来被摘掉）**不许**
   * 假装弹过框：这句话会经模型进到老人耳朵里（"您按下确认就行"），而屏幕上
   * 什么都没有。 */

  if (ui_post_ask_alarm != NULL)
    {
      ui_post_ask_alarm(reason);
      ask_state = "已经请用户确认（询问框已经弹出来）";
      ai_tools_info("report_emergency: 已请用户确认「%s」", reason);
    }
  else
    {
      ask_state = "没能请用户确认（询问框这一侧还没接上，屏幕上什么都没有）";
      ai_tools_info("report_emergency: ui_post_ask_alarm 没接上，询问框没弹");
    }

  /* 2. 上报一条「未确认」事件。conf 用 100 / 60 只是给家人那边分紧急程度，
   * 不是声学置信度。 */

  if (g_net_ctx == NULL)
    {
      snprintf(net_state, sizeof(net_state),
               "事件没有上报（网络上下文还没注册）");
      ai_tools_info("report_emergency: 网络上下文为空，事件没上报");
    }
  else
    {
      const char *event = high ? AI_TOOLS_EVENT_SOS_HIGH : AI_TOOLS_EVENT_SOS;
      int conf = high ? 100 : 60;

      ret = ai_network_report_sound_alarm(g_net_ctx, event, conf);

      if (ret < 0)
        {
          snprintf(net_state, sizeof(net_state),
                   "事件没能交给网络层（返回 %d），这条事件没发出去", ret);
          ai_tools_info("report_emergency: 事件 %s 上报失败(%d)", event, ret);
        }
      else
        {
          /* 进了发送队列不等于发出去了（真正 publish 是 network_task 那条
           * socket 干的），所以只说"交给网络层"。 */

          snprintf(net_state, sizeof(net_state),
                   "事件已经交给网络层上报（%s）", event);
          ai_tools_info("report_emergency: 事件 %s conf=%d 已交给网络层",
                        event, conf);
        }
    }

  /* 回给模型的文本。最后那几句是这一整个工具的**主要产物**：模型会照着自己
   * 看到的东西跟老人说话，所以必须写清"还没报警、要等确认"，并且明确禁止
   * 重复调用和编造成功 —— 不然老人听到的就是"已经帮您报警了"。 */

  snprintf(output, output_size,
           "已受理这次紧急报告：%s%s%s（紧急程度 %s）。%s。%s。"
           "现在**还没有真正报警**：要等用户在询问框里确认之后板子才会报警。"
           "请把这句话告诉用户并等他确认，不要再调用本工具，"
           "也不要说已经报警成功。",
           reason,
           (detail[0] != '\0') ? "；补充：" : "",
           detail,
           high ? AI_TOOLS_LEVEL_HIGH
                : (level_unknown ? AI_TOOLS_LEVEL_NORMAL
                                   "（模型给的 level 不认识，已按 normal 处理）"
                                 : AI_TOOLS_LEVEL_NORMAL),
           ask_state, net_state);

  return OK;
}

/**
 * @brief  框架的 execute 回调：执行一个工具调用
 *
 * @param  name         模型要调的工具名
 * @param  input_json   模型给的参数（JSON 对象串；可能为 NULL）
 * @param  output       工具结果（回给模型看的文本，必须留 '\0' 的位置）
 * @param  output_size  output 的大小
 * @return OK     - 名字是我们的（不管执行成功还是失败，结果都在 output 里）
 *         ERROR  - 不是我们的工具，让框架继续问下一个 provider
 */

static int ai_tools_provider_execute(const char *name, const char *input_json,
                                     char *output, size_t output_size)
{
  char device[AI_TOOLS_DEVICE_MAX];
  char command[AI_TOOLS_COMMAND_MAX];
  const char *p = NULL;
  size_t len = 0;
  int ret;

  if (name == NULL || output == NULL || output_size == 0)
    {
      return ERROR;
    }

  /* 紧急报告有自己的入口（参数多一个 level/detail，且要动界面），先分出去，
   * 下面那段灯控的取值逻辑一行都不用改。 */

  if (strcmp(name, AI_TOOL_REPORT_EMERGENCY) == 0)
    {
      return ai_tools_report_emergency(input_json, output, output_size);
    }

  if (strcmp(name, AI_TOOL_SET_LIGHT) != 0)
    {
      /* 不是我们的工具：**必须**回 ERROR，否则框架会以为已经被处理掉了，
       * 排在后面的 provider（还有后加的）就永远轮不上。 */

      return ERROR;
    }

  output[0] = '\0';

  /* command 必填。缺了 / 值是 on/off 之外的，都把原因写成一句人话回给模型，
   * 让它自己跟老人解释或者再问一遍（返回 ERROR 会走成"未知工具"，模型只
   * 会看到一句英文报错）。 */

  p = ai_tools_json_find_string(input_json, "command", &len);

  if (!ai_tools_copy_token(p, len, command, sizeof(command)))
    {
      snprintf(output, output_size,
               "没执行：command 参数缺失或者不是合法的 on / off。"
               "请按 on 或 off 重新调用一次。");
      ai_tools_info("set_light: 参数不合法，打回");
      return OK;
    }

  if (strcmp(command, "on") != 0 && strcmp(command, "off") != 0)
    {
      snprintf(output, output_size,
               "没执行：command 只能是 on 或者 off，收到的是别的值。"
               "请换算成 on（开灯）或 off（关灯）重新调用。");
      ai_tools_info("set_light: command 值不合法，打回");
      return OK;
    }

  /* device_id 可选：模型没给（或者给得不合法）就用和 ai_companion_main.c 里
   * LIGHT_DEVICE_ID 同一个默认设备 —— 队友的灯模拟器只认那一台。 */

  snprintf(device, sizeof(device), "%s", AI_TOOLS_LIGHT_DEVICE_ID);

  p = ai_tools_json_find_string(input_json, "device_id", &len);
  if (p != NULL && !ai_tools_copy_token(p, len, device, sizeof(device)))
    {
      snprintf(device, sizeof(device), "%s", AI_TOOLS_LIGHT_DEVICE_ID);
    }

  if (g_net_ctx == NULL)
    {
      /* 网络模块还没起 / 已经拆了。这条命令不会发出去，但工具本身执行过了 ——
       * 老实告诉模型，别让它对老人说"灯打开了"。 */

      snprintf(output, output_size,
               "没执行：灯控还没接上网络模块（网络上下文没注册），"
               "这次命令没有发出去。");
      ai_tools_info("set_light: 网络上下文为空，命令没发");
      return OK;
    }

  /* 发到 zhi_ai/<client_id>/device_cmd。command 必须是**裸字符串** on/off：
   * 队友的灯模拟器收的是 network_comm.c 里 send_device_command() 拼的
   * {"device_id":..., "command":"on"}，这里再包一层 JSON 就没法解析了。 */

  ret = ai_network_send_device_command(g_net_ctx, device, command);

  if (ret < 0)
    {
      /* 负值 = 命令**没进发送队列**（-ENOTCONN 网络没连上、-ENOSPC 队列满、
       * -EMSGSIZE 载荷超长、-EINVAL 参数被拒），设备那边一点动静都没有。 */

      snprintf(output, output_size,
               "没执行：命令没能交给网络层（返回 %d，网络没连上、发送队列满或者"
               "载荷超长），这条命令不会发出去。", ret);
      ai_tools_info("set_light: device_cmd %s=%s 没交给网络层(%d)", device,
                    command, ret);
      return OK;
    }

  /* 走到这里只说明消息**进了发送队列**，真正发出去是 network_task 那条
   * socket 干的；设备收没收到、执行没执行都没有回执。所以不能回"已下发" ——
   * 模型会照着这句话跟老人说，而灯可能压根没动。 */

  snprintf(output, output_size,
           "已交给网络层发送：发给 %s 的灯控命令 %s。发出去而已，设备有没有"
           "执行这边看不到回执，别对老人说已经生效。", device, command);
  ai_tools_info("set_light: device_cmd %s=%s 已交给网络层", device, command);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void ai_tools_provider_set_network_ctx(ai_network_context_t *ctx)
{
  g_net_ctx = ctx;
  ai_tools_info("网络上下文已%s", (ctx != NULL) ? "注册" : "撤销");
}

int ai_tools_provider_init(void)
{
#ifdef CONFIG_HELLO_APP_LLM_AI_AGENT

  if (g_registered)
    {
      /* 幂等：重复注册会在工具清单里塞进两份同名工具。 */

      ai_tools_info("已经注册过，跳过");
      return OK;
    }

  tool_registry_register_provider(AI_TOOLS_PROVIDER_NAME,
                                  ai_tools_provider_get_tools,
                                  ai_tools_provider_execute);

  g_registered = true;

  /* 清单是带缓存的：注册完必须打脏，否则 ai_agent 那边一直用着旧清单，
   * 模型看不到新工具。下一次 ai_agent 收消息时（它每条消息都会重新取一次
   * 清单）set_light 就跟着一起下发了。 */

  tool_registry_invalidate();

  ai_tools_info("已注册 provider「%s」，工具 %s / %s", AI_TOOLS_PROVIDER_NAME,
                AI_TOOL_SET_LIGHT, AI_TOOL_REPORT_EMERGENCY);
  return OK;

#else

  /* 这个配置里 hello_app 没连 ai_agent，工具表都不存在 —— 明确报错，
   * 别让调用方以为注册成功了。 */

  ai_tools_info("本构建没开 CONFIG_HELLO_APP_LLM_AI_AGENT，工具无法注册");
  return -ENOSYS;

#endif
}
