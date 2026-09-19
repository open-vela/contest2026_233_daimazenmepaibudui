/**
 * robot_ui_bridge.c - 语音链路推界面的同进程直调实现（编在 robot_ui 这个 app 里）
 *
 * 头文件里写了"为什么要有这个文件"。这里只补三件实现上的事：
 *
 * 1) 四个语音入口**不直接碰 LVGL 控件**，只做
 *    「清洗文本 -> 调 touch_ui_xxx() / robot_ui_bridge_post_status()」，
 *    后两者内部全是 lv_async_call，真正的控件操作发生在 LVGL 线程。
 *    所以 ai_companion 的录音线程、主循环任务随便调都不会踩
 *    `_lv_inv_area: Invalidate area is not allowed during rendering` 那个断言
 *    （2026-09-14 就是被这个断言把整个 app 打死的）。
 *    本文件里唯一直接碰 LVGL 的是结尾那个 robot_ui_bridge_panel_reinit() 的
 *    重绘回调 —— 它自己也跑在 LVGL 线程里（lv_async_call 投过去），
 *    同样是安全的。
 *
 * 2) 闸门（g_ui_ready）之外还有一道 up_interrupt_context() 检查：万一以后有人
 *    图省事在音频回调/中断里调进来，宁可丢一次显示也不能在中断里 malloc + 取锁。
 *
 * 3) 顺序有讲究：先把面板投出去、再投文字。两者都是 lv_async_call，投递顺序
 *    就是执行顺序；反过来先写后弹，"你说：…"会落在面板还没建好的空档里丢掉。
 */

#include <nuttx/config.h>
#include <nuttx/arch.h>

#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>

#include "robot_ui_bridge.h"

#include "robot_ui.h"
#include "touch_ui.h"

/* 跨线程"投到 LVGL 线程"的唯一投递口（ui_async.c）。面板重初始化后要请界面
 * 重画一整屏，那一跳必须走它 —— 理由见 ui_async.h 开头。 */
#include "ui_async.h"

/* 面板重新初始化（sf32lb_lcd_panel_reinit）的声明在板级头文件里，
 * 实现在 vendor/sifli/boards/sf32lb52/drivers/lcd/sf32lb_lcd.c。
 * 头文件路径由 CMakeLists.txt 的 ${NUTTX_BOARD_ABS_DIR}/src 提供。 */
#include "sf32lb52_devkit_lcd.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 界面就绪标志。robot_ui 的 main() 建完控件才置起来（见头文件）。
 * volatile：一个任务写、别的任务读，不加锁 —— 它只是一位"开关"，
 * 读到旧值最坏是"这一次推送被丢掉"，不会读到半截值。 */
static volatile bool g_ui_ready;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * @brief  这次推送能不能做（闸门 + 中断上下文）
 *
 * why = 不能做的原因，给日志用；能返回 true 时 why 不动。
 */

static bool bridge_can_push(const char **why)
{
  if (!g_ui_ready)
    {
      *why = "界面还没就绪";
      return false;
    }

  if (up_interrupt_context())
    {
      *why = "在中断上下文里，不能投界面";
      return false;
    }

  return true;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void robot_ui_bridge_set_ready(bool ready)
{
  g_ui_ready = ready;
  printf("[Bridge] 语音直调通道%s\n", ready ? "已就绪" : "关闭");
}

bool robot_ui_bridge_is_ready(void)
{
  return g_ui_ready;
}

/**
 * @brief  显示用户刚说的话（ASR 原文）
 */

void robot_ui_bridge_voice_user_said(const char *text)
{
  char shown[ROBOT_UI_BRIDGE_TEXT_MAX];
  const char *why = NULL;

  if (text == NULL || text[0] == '\0')
    {
      return;
    }

  if (!bridge_can_push(&why))
    {
      printf("[Bridge] user_said 没上屏（%s）: %s\n", why, text);
      return;
    }

  /* 先弹面板再写文字：面板没开着的话 touch_ui_set_voice_user_text()
   * 本身是空操作，这条文字就没了 —— 顺序不能反。 */

  robot_ui_bridge_panel_autoshow();

  if (robot_ui_bridge_sanitize_text(text, shown, sizeof(shown)) == 0)
    {
      /* 整句都被清洗掉了（比如只订了 emoji）：界面不动，别拿空串去刷对话区
       * —— 那会把上一句擦掉，看着像面板坏了。 */
      printf("[Bridge] user_said 清洗后为空，界面不动\n");
      return;
    }

  touch_ui_set_voice_user_text(shown);
  printf("[Bridge] 用户原话已上屏: %s\n", shown);
}

/**
 * @brief  显示 AI 的回复正文
 */

void robot_ui_bridge_voice_reply(const char *text)
{
  char shown[ROBOT_UI_BRIDGE_TEXT_MAX];
  const char *why = NULL;

  if (text == NULL || text[0] == '\0')
    {
      return;
    }

  if (!bridge_can_push(&why))
    {
      printf("[Bridge] ai_reply 没上屏（%s）: %s\n", why, text);
      return;
    }

  if (robot_ui_bridge_sanitize_text(text, shown, sizeof(shown)) == 0)
    {
      printf("[Bridge] ai_reply 清洗后为空，界面不动\n");
      return;
    }

  /* 和 MQTT 那条路一致：状态栏/表情按"正在回复"走，对话区写进主屏。
   * 状态行（面板上那一行）不在这里改 —— 那是 voice_state 的事，保持一个来源。 */

  robot_ui_bridge_post_status(ROBOT_STATUS_SPEAKING, ROBOT_FACE_HAPPY, shown);

  /* 镜像面板开着时，同一段回复也写进面板的对话区（没开就是空操作） */
  touch_ui_set_voice_reply(shown);

  printf("[Bridge] AI 回复已上屏: %s\n", shown);
}

/**
 * @brief  更新语音状态
 */

void robot_ui_bridge_voice_state(int state)
{
  const char *why = NULL;

  if (!bridge_can_push(&why))
    {
      printf("[Bridge] voice_state=%d 没上屏（%s）\n", state, why);
      return;
    }

  /* 映射规则与 robot_ui/main.c 的 MQTT 分支逐字一致。
   * 这里说的"状态栏"是历史上的叫法：状态栏那三样 2026-09-14 已被用户删掉，
   * 这些 ROBOT_STATUS_* 现在落到**主界面「AI 回复区」里那一行语音状态小字**上
   * （robot_ui_set_status，四档：空闲/在听/在想/在说）。
   *   listening 小字「在听…」蓝 普通表情 / thinking 小字「在想…」橙 思考表情
   *   speaking  小字「在说…」绿 普通表情 / idle     小字「空闲」灰 普通表情
   *（2026-09-16 之前 thinking 借的是 LISTENING 那一档，因为那行小字还没显示出来、
   *  靠表情区分就够了；现在四档各显各的，不再借。）
   *
   * 「在说」要把语音镜像面板弹出来（MQTT 那条路也是这么做的，见
   * on_ai_command_received 的 voice_state 分支）：面板不弹出来，老人根本
   * 不知道要去哪里看"它听清了什么、回了什么"。
   *
   * ⚠️ **「在听」绝对不能弹**：hello_app 是**开机自启**的，它一启动就报一条
   * LISTENING —— 那样开机第一眼看到的就是语音面板，而不是
   * 主菜单（用户实测反馈："为什么一开机就是语音聊天页面？我希望看到主菜单"）。
   * 现在的策略：只在"确实有语音活动"时才弹 ——
   *   - 用户说的话到了（`robot_ui_bridge_voice_user_said()` → 界面侧那处 autoshow）
   *   - 机器人正在回话（SPEAKING，下面这一处）
   * thinking 也不弹（识别一出来就先走 user_said 那处）。
   * 抑制规则（用户关过就不再弹）在 voice_mirror_autoshow 里。 */

  switch (state)
    {
      case ROBOT_UI_BRIDGE_VOICE_LISTENING:
        robot_ui_bridge_post_status(ROBOT_STATUS_LISTENING, ROBOT_FACE_HAPPY,
                                    NULL);
        touch_ui_set_voice_state(TOUCH_VOICE_STATE_LISTENING);
        break;

      case ROBOT_UI_BRIDGE_VOICE_THINKING:
        /* 主界面那行小字走"在想…"这一档（表情仍然是"思考"，和 MQTT 那条路
         * 逐字一致）。桥接层自己的语义没变：进来的还是 ROBOT_UI_BRIDGE_VOICE_
         * THINKING 这一档，只是显示目标从 LISTENING 换成了 THINKING。 */
        robot_ui_bridge_post_status(ROBOT_STATUS_THINKING, ROBOT_FACE_THINKING,
                                    NULL);
        touch_ui_set_voice_state(TOUCH_VOICE_STATE_THINKING);
        break;

      case ROBOT_UI_BRIDGE_VOICE_SPEAKING:
        robot_ui_bridge_panel_autoshow();
        robot_ui_bridge_post_status(ROBOT_STATUS_SPEAKING, ROBOT_FACE_HAPPY,
                                    NULL);
        touch_ui_set_voice_state(TOUCH_VOICE_STATE_SPEAKING);
        break;

      case ROBOT_UI_BRIDGE_VOICE_IDLE:
        robot_ui_bridge_post_status(ROBOT_STATUS_IDLE, ROBOT_FACE_HAPPY, NULL);
        touch_ui_set_voice_state(TOUCH_VOICE_STATE_IDLE);
        break;

      default:
        /* 不认识的状态不改界面：宁可少刷一次，也不要把界面停在一个错的字上 */
        printf("[Bridge] voice_state 不认识的档位: %d\n", state);
        break;
    }
}

/**
 * @brief  自动弹出语音镜像面板（按界面那边的抑制规则）
 */

void robot_ui_bridge_voice_panel_open(void)
{
  const char *why = NULL;

  if (!bridge_can_push(&why))
    {
      printf("[Bridge] 弹面板被挡（%s）\n", why);
      return;
    }

  robot_ui_bridge_panel_autoshow();
}

/****************************************************************************
 * 面板重新初始化（黑屏救回）
 ****************************************************************************/

/**
 * @brief  面板重初始化之后，把整屏判成脏区重推一次
 *
 * 跑在 LVGL 线程里（ui_async_call 投过来的），所以这里碰控件是安全的。
 *
 * 为什么必须显式 invalidate 整屏：面板被复位过，GRAM 里的内容是随机的，
 * 面板侧"配置修好了"不等于"画面对了"。LVGL 只重画脏区，而黑屏期间界面
 * 往往已经没有脏区了（用户没在操作），光靠"下一帧"可能什么都等不到。
 *
 * 不在这里调 lv_refr_now()：没必要要求"投递后立刻刷完"，
 * LVGL 自己的刷新定时器几十毫秒内就会跑，不值得为省这几十毫秒去冒
 * 从定时器回调里再进一次刷新的风险。
 */

static void panel_reinit_full_redraw(void *arg)
{
  (void)arg;

  lv_obj_invalidate(lv_screen_active());
}

int robot_ui_bridge_panel_reinit(void)
{
  int ret;

  if (up_interrupt_context())
    {
      printf("[Bridge] 面板重初始化被挡（在中断上下文里）\n");
      return -EINVAL;
    }

  /* 面板侧：重发初始化序列 + 拉一次 RESET 脚 + 重设像素格式/亮度/DisplayOn。
   * 驱动里用 panel_lock 和刷新路径互斥，所以 LVGL 正在推屏也不会打架。 */
  ret = sf32lb_lcd_panel_reinit();
  if (ret < 0)
    {
      printf("[Bridge] 面板重初始化失败: %d"
             "（-19=面板驱动还没绑上，-38=这个固件里编掉了）\n", ret);
      return ret;
    }

  if (!g_ui_ready)
    {
      printf("[Bridge] 面板已重初始化；界面还没就绪，"
             "全屏重绘等 LVGL 自己刷\n");
      return 0;
    }

  if (ui_async_call(panel_reinit_full_redraw, NULL) != LV_RESULT_OK)
    {
      printf("[Bridge] 面板已重初始化，但全屏重绘投递失败（内存不够？）"
             "—— 先等下一个脏区\n");
      return -ENOMEM;
    }

  printf("[Bridge] 面板已重初始化，已投一次全屏重绘\n");
  return 0;
}

/**
 * @brief  每帧看一眼"面板内容是不是已经对不上了"，是就整屏重绘
 *
 * 为什么需要：驱动侧 putrun/putarea 在**面板还没就绪**的时候是**静默丢弃**
 * （vendor/sifli/boards/sf32lb52/drivers/lcd/sf32lb_lcd.c 里那两个
 *  `if (s_fb_registering || !s_lcd_hw_ready) return OK;`）。开机时 /dev/fb0
 * 一注册好界面就起来了，而面板要等那次完整 Init 才置就绪 —— LVGL 的整屏
 * 首帧正好被丢掉，而它自己以为已经画上去了。之后它只重画脏区，于是屏幕
 * 一直保持"只有那几个小脏区有内容"：整屏黑、只有状态栏那一小块/刚点开的
 * 页面有画面（实测现象）。面板被重初始化（SWRESET 清 GRAM）同理。
 *
 * 跑在 LVGL 线程里（robot_ui 主循环），所以这里碰控件是安全的。
 *
 * Returned Value: 这次有没有投一次整屏重绘（只给日志/调试用）。
 */

bool robot_ui_bridge_lcd_check(void)
{
  static uint32_t last_ms;
  static uint32_t redraw_cnt;
  uint32_t now;

  if (!g_ui_ready)
    {
      return false;
    }

  /* 限速：面板还没就绪时，重绘出去的像素同样会被丢 ⇒ 标志马上又会被置上。
   * 不限速就变成"每几毫秒渲染一整屏"的空转，把 LVGL 线程整个占住。
   * 250ms 一次：最多白画几帧，面板一就绪下一轮就是完整的一屏。 */
  now = lv_tick_get();
  if (last_ms != 0 && (uint32_t)(now - last_ms) < 250)
    {
      return false;                 /* 标志留着，下一轮再看 */
    }

  last_ms = now;

  if (!sf32lb_lcd_take_content_lost())
    {
      return false;
    }

  lv_obj_invalidate(lv_screen_active());

  redraw_cnt++;
  if (redraw_cnt <= 3)
    {
      printf("[Bridge] 面板内容缺失（丢帧或面板复位）⇒ 整屏重绘第 %u 次\n",
             (unsigned)redraw_cnt);
    }

  return true;
}
