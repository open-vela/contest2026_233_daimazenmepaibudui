/****************************************************************************
 * board/contest_board/src/sf32lb52_boardbtn.c
 *
 * SF32LB52 板级按键模块：PA11(KEY) / PA34(HOME) 用 GPIO 轮询 + 消抖，
 * 事件走回调，**不经过 /dev/buttons**。
 *
 * 为什么另起一条路（真机现象）写在 sf32lb52_boardbtn.h 文件头：
 * `/dev/buttons` 的 poll()/read() 在真机上会让整机静默卡死，所以这里
 * 直接读 GPIO，完全不碰那个字符设备。`sf32lb52_buttons.c` 的注册保持原样。
 *
 * 读法（两个脚都只有 2 个寄存器操作：读 pad 电平 + 比较）：
 *
 *   PA11：bsp_pinmux.c:173 已经配成 GPIO_A11 输入 + 下拉，本模块 init 时
 *         用 HAL_PIN_Set() 重申同样的配置（不变方向、不变功能，只是把
 *         下拉恢复成板级 pinmux 的值 —— sf32lb52_buttons.c 的
 *         sifli_gpio_config() 会把输入脚设成 NOPULL，按键是高有效，
 *         悬空就会乱报"按下"）。
 *
 *   PA34：bsp_pinmux.c:171 是**故意注释掉**的（保持默认下拉，见文件头），
 *         本模块 init 时把它切成 GPIO_A34 输入**并保持下拉**，
 *         不申请中断、不写电平、不做任何独占动作。
 *
 * 高有效：两个脚都是"按下 = 高"（PA11 与既有 /dev/buttons 驱动的
 * SF32LB52_BUTTON_KEY2_ACTIVE_LOW=0 一致；PA34 与参考实现
 * xiaozhi-sf32 的 CONFIG_BSP_KEY1_ACTIVE_HIGH=y 一致）。如果现场发现
 * 某个脚是反的，把 g_btn_pin[] 里对应那条的 active_high 改成 false 即可。
 *
 * 消抖：连续 BOARD_BTN_DEBOUNCE_SAMPLES 次采样一致才认电平变化
 * （轮询 BOARD_BTN_POLL_MS 一次，即最坏 ~30ms 消抖）。
 * 长按：按住超过 BOARD_BTN_LONGPRESS_MS 报一次 BOARD_BTN_LONGPRESS，
 * 松手时照常报 BOARD_BTN_RELEASE（带按住时长）。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/clock.h>
#include <nuttx/mutex.h>
#include <nuttx/sched.h>

#include "bf0_hal.h"              /* HAL_PIN_Set / PAD_PAxx / GPIO_Axx */
#include "sifli_gpio.h"           /* sifli_gpio_read */

#include "sf32lb52_boardbtn.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 驱动 pin 号（sifli_gpio_read 用）：PA11 / PA34 都在 GPIO1 上。
 * 和 sf32lb52_buttons.c:44 的 GET_PIN_2(hwp_gpio1, 11) 是同一个脚。 */

#define BOARD_BTN_KEY_PIN   (GET_PIN_2(hwp_gpio1, 11))
#define BOARD_BTN_HOME_PIN  (GET_PIN_2(hwp_gpio1, 34))

/* 输入配置：板级 pinmux 给这两个脚用的都是下拉（bsp_pinmux.c:173），
 * 高有效按键空闲时必须被拉住，否则会乱报按下。 */

#define BOARD_BTN_PAD_FLAGS PIN_PULLDOWN

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct board_btn_pin_s
{
  FAR const char *name;       /* 日志/打印用 */
  int             drivenum;   /* sifli_gpio_read() 的入参 */
  int             pad;        /* HAL_PIN_Set() 的 pad */
  pin_function    func;       /* HAL_PIN_Set() 的功能 */
  bool            active_high;/* true = 高电平 = 按下 */
  bool            need_mux;   /* true = 只有 init 之后才能读 */
};

struct board_btn_state_s
{
  bool    usable;         /* 这个脚现在能不能读 */
  bool    down;           /* 消抖后的状态 */
  uint8_t diff;           /* 与 down 不同的连续采样数 */
  bool    press_reported; /* 本次按住报过 PRESS（没报过的松开不报 RELEASE） */
  bool    lp_reported;    /* 本次按住是否已经报过 LONGPRESS */
  clock_t down_tick;      /* down 变 true 的时刻（用于算 held_ms） */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 本板的两个按键。name 里带上引脚号，现场看日志就能对上。 */

static const struct board_btn_pin_s g_btn_pin[BOARD_BTN_COUNT] =
{
  {
    "KEY(PA11)", BOARD_BTN_KEY_PIN, PAD_PA11, GPIO_A11,
    true,                                   /* active_high */
    false                                   /* 板级 pinmux 已经配好了 */
  },
  {
    "HOME(PA34)", BOARD_BTN_HOME_PIN, PAD_PA34, GPIO_A34,
    true,                                   /* active_high */
    true                                    /* init 里才切成 GPIO 输入；
                                               BOARD_BTN_ENABLE_HOME=0
                                               时 pin_setup 会跳过它 */
  },
};

static struct board_btn_state_s g_btn_st[BOARD_BTN_COUNT];

/* 回调槽与模块状态，都用 g_btn_lock 保护。
 * 注意：**回调本身不在锁里调用**（它可能会回头调 set_callback）。 */

static mutex_t        g_btn_lock = NXMUTEX_INITIALIZER;
static bool           g_btn_inited;
static board_btn_cb_t g_btn_cb;
static FAR void      *g_btn_cb_arg;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_btn_pin_setup
 *
 * Description:
 *   把两个按键脚切成 GPIO 输入（下拉）。**调用者必须已持有 g_btn_lock**
 *   或保证自己是唯一调用者（init 里只调一次）。
 *
 *   - PA11：重申板级 pinmux（bsp_pinmux.c:173）的配置。方向/功能不变，
 *     只恢复下拉 —— 既有 /dev/buttons 驱动调 sifli_gpio_config() 时会把
 *     Pull 设成 NOPULL，高有效按键悬空就会乱报按下。
 *   - PA34：bsp_pinmux 故意没配它（见 .h 文件头），这里把它切成 GPIO
 *     输入但**保持下拉**（PIN_PULLDOWN = pad 复位默认值），
 *     不碰 PMU / 唤醒 / 中断的任何配置。
 *
 ****************************************************************************/

static void board_btn_pin_setup(void)
{
  int i;

  for (i = 0; i < BOARD_BTN_COUNT; i++)
    {
      if (i == BOARD_BTN_HOME && !BOARD_BTN_ENABLE_HOME)
        {
          continue;                       /* 明确不碰 PA34 */
        }

      HAL_PIN_Set(g_btn_pin[i].pad, g_btn_pin[i].func,
                  BOARD_BTN_PAD_FLAGS, 1);
      g_btn_st[i].usable = true;
    }
}

/****************************************************************************
 * Name: board_btn_read_raw
 *
 * Description:
 *   读一次电平，**没有消抖**。1 = 按下，0 = 松开，< 0 = 现在读不了。
 *
 ****************************************************************************/

static int board_btn_read_raw(int idx)
{
  bool level;

  if (idx < 0 || idx >= BOARD_BTN_COUNT || !g_btn_st[idx].usable)
    {
      return -ENODEV;
    }

  level = sifli_gpio_read((uint32_t)g_btn_pin[idx].drivenum);

  if (g_btn_pin[idx].active_high)
    {
      return level ? 1 : 0;
    }

  return level ? 0 : 1;
}

/****************************************************************************
 * Name: board_btn_emit
 *
 * Description:
 *   把事件发给注册的回调。在轮询任务上下文里运行。
 *   先在锁里取一份 cb/arg 的副本，**出了锁再调**（回调可能改注册）。
 *
 ****************************************************************************/

static void board_btn_emit(int idx, enum board_btn_event_e ev,
                           uint32_t held_ms)
{
  board_btn_cb_t cb;
  FAR void *arg;

  nxmutex_lock(&g_btn_lock);
  cb  = g_btn_cb;
  arg = g_btn_cb_arg;
  nxmutex_unlock(&g_btn_lock);

  if (cb != NULL)
    {
      cb((enum board_btn_e)idx, ev, held_ms, arg);
    }
}

/****************************************************************************
 * Name: board_btn_poll_once
 *
 * Description:
 *   采样一轮：对每个可读的脚做消抖，电平稳定变化时上报事件，
 *   并在按住期间检查长按。
 *
 ****************************************************************************/

static void board_btn_poll_once(void)
{
  clock_t now = clock_systime_ticks();
  int i;

  for (i = 0; i < BOARD_BTN_COUNT; i++)
    {
      int raw = board_btn_read_raw(i);

      if (raw < 0)
        {
          continue;
        }

      if ((bool)raw != g_btn_st[i].down)
        {
          /* 电平变了：要求连续几次都一样才算数 */

          if (++g_btn_st[i].diff >= BOARD_BTN_DEBOUNCE_SAMPLES)
            {
              g_btn_st[i].diff       = 0;
              g_btn_st[i].down       = (bool)raw;
              g_btn_st[i].lp_reported = false;

              if (raw != 0)
                {
                  g_btn_st[i].down_tick      = now;
                  g_btn_st[i].press_reported = true;
                  board_btn_emit(i, BOARD_BTN_PRESS, 0);
                }
              else
                {
                  /* 只有报过 PRESS 的按住才报配对的 RELEASE；开机时手就
                   * 按着（init 时 down = true、没报过 PRESS）的松开不报。
                   * down_tick 要等这里用完再收尾，所以先算 held_ms。 */

                  if (g_btn_st[i].press_reported)
                    {
                      board_btn_emit(i, BOARD_BTN_RELEASE,
                                     (uint32_t)TICK2MSEC(now -
                                                         g_btn_st[i].down_tick));
                    }

                  g_btn_st[i].press_reported = false;
                }
            }
        }
      else
        {
          g_btn_st[i].diff = 0;
        }

      /* 长按：按住期间到点报一次（只报一次） */

      if (g_btn_st[i].down && !g_btn_st[i].lp_reported &&
          TICK2MSEC(now - g_btn_st[i].down_tick) >= BOARD_BTN_LONGPRESS_MS)
        {
          g_btn_st[i].lp_reported = true;
          board_btn_emit(i, BOARD_BTN_LONGPRESS,
                         (uint32_t)TICK2MSEC(now - g_btn_st[i].down_tick));
        }
    }
}

/****************************************************************************
 * Name: board_btn_task
 *
 * Description:
 *   轮询任务：只做"读两个 GPIO + 比较 + 发回调"，不做别的。
 *
 ****************************************************************************/

static int board_btn_task(int argc, FAR char *argv[])
{
  (void)argc;
  (void)argv;

  for (; ; )
    {
      board_btn_poll_once();
      usleep(BOARD_BTN_POLL_MS * 1000);
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int board_btn_init(void)
{
  pid_t pid;
  int i;

  nxmutex_lock(&g_btn_lock);
  if (g_btn_inited)
    {
      nxmutex_unlock(&g_btn_lock);
      return OK;
    }

  nxmutex_unlock(&g_btn_lock);

  /* 先配脚，再把初始状态读一次：避免"开机时手正按着"被当成一次按下事件
   * （press_reported / lp_reported 都按"已经报过"处理，所以既不报 PRESS，
   * 也不会有孤立的 RELEASE / LONGPRESS）。 */

  board_btn_pin_setup();

  {
    clock_t now = clock_systime_ticks();

    for (i = 0; i < BOARD_BTN_COUNT; i++)
      {
        int raw = board_btn_read_raw(i);

        g_btn_st[i].down           = (raw > 0);
        g_btn_st[i].diff           = 0;
        g_btn_st[i].press_reported = false;
        g_btn_st[i].lp_reported    = true;
        g_btn_st[i].down_tick      = now;
      }
  }

  pid = task_create(BOARD_BTN_TASK_NAME, BOARD_BTN_TASK_PRIORITY,
                    BOARD_BTN_TASK_STACK, (main_t)board_btn_task, NULL);
  if (pid < 0)
    {
      syslog(LOG_ERR, "BOARDBTN: task_create failed: %d\n", (int)pid);
      return (int)pid;
    }

  nxmutex_lock(&g_btn_lock);
  g_btn_inited = true;
  nxmutex_unlock(&g_btn_lock);

  syslog(LOG_INFO, "BOARDBTN: poll task started (pid=%d, %dms, "
         "debounce=%d, long=%dms)\n",
         (int)pid, BOARD_BTN_POLL_MS, BOARD_BTN_DEBOUNCE_SAMPLES,
         BOARD_BTN_LONGPRESS_MS);

  return OK;
}

int board_btn_set_callback(board_btn_cb_t cb, FAR void *arg)
{
  int ret;

  ret = board_btn_init();           /* 懒初始化：调用方不必先 init */
  if (ret < 0)
    {
      return ret;
    }

  nxmutex_lock(&g_btn_lock);
  g_btn_cb     = cb;
  g_btn_cb_arg = (cb != NULL) ? arg : NULL;
  nxmutex_unlock(&g_btn_lock);

  return OK;
}

int board_btn_is_pressed(enum board_btn_e btn)
{
  if (btn < 0 || btn >= BOARD_BTN_COUNT)
    {
      return -EINVAL;
    }

  /* 任务起来了就给消抖后的状态（更稳），没起来就给即时电平 */

  nxmutex_lock(&g_btn_lock);
  if (g_btn_inited)
    {
      int down = g_btn_st[btn].down ? 1 : 0;

      nxmutex_unlock(&g_btn_lock);
      return down;
    }

  nxmutex_unlock(&g_btn_lock);

  return board_btn_read_raw((int)btn);
}

FAR const char *board_btn_name(enum board_btn_e btn)
{
  if (btn < 0 || btn >= BOARD_BTN_COUNT)
    {
      return "?";
    }

  return g_btn_pin[btn].name;
}

FAR const char *board_btn_event_name(enum board_btn_event_e ev)
{
  switch (ev)
    {
      case BOARD_BTN_PRESS:
        return "PRESS";
      case BOARD_BTN_RELEASE:
        return "RELEASE";
      case BOARD_BTN_LONGPRESS:
        return "LONGPRESS";
      default:
        return "?";
    }
}
