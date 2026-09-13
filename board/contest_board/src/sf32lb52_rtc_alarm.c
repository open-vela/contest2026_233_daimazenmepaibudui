/****************************************************************************
 * board/contest_board/src/sf32lb52_rtc_alarm.c
 *
 * SF32LB52 板级 RTC 每日定时提醒（"每天 08:00 提醒吃药"这类）
 *
 * 组成：
 *   - 一个常驻工作线程（栈 4096，优先级 115）：唯一碰 /dev/rtc0 的地方。
 *     它负责：读当前时间 -> 算下一个 hh:mm -> RTC_SET_ALARM -> 等 SIGUSR1
 *     -> 到点在工作线程里回调上层 -> 再排下一天。
 *   - rtc_alarm_at_daily() / rtc_alarm_cancel() 只改状态 + 唤醒工作线程，
 *     非阻塞。
 *
 * 为什么必须是"工作线程 + 信号"：
 *   - /dev/rtc0 的 read() 永远返回 0（EOF），而且没有 poll()，等 alarm
 *     只能靠信号：ioctl 参数里的 struct sigevent 由 upper half 在到点时
 *     用 nxsig_notification() 通知 pid（rtc.c 的 rtc_alarm_callback）。
 *   - alarminfo->pid == 0 表示"通知设 alarm 的那个任务"。所以**设 alarm
 *     的动作必须发生在将来收信号的那个线程里**，否则信号会发给一个
 *     已经退出的任务、没人收。这就是本模块要单独起工作线程的原因。
 *
 * "算下一个 hh:mm"为什么不用 mktime()/localtime()：
 *   本固件 CONFIG_LIBC_LOCALTIME 未开，localtime() 就是 gmtime()，
 *   日历换算只支持 1970 以后；time_t 又是 32 位，2099 年附近会溢出。
 *   这里只做"比较当前时分 + 必要时日期 +1 天"，用月长表 + 闰年判断手算
 *   进位，并自己按日期算 tm_wday。全程不依赖 libc 日历，年/月/日/时/分/
 *   秒/星期都由本文件保证正确。
 *
 * 工作线程优先级 115 与 sf32lb52_alarm 一致：NuttX 里数值越小优先级越高，
 * 115 低于 robot_ui(110)、高于 lpwork(120)——每日提醒不需要抢在 USB RNDIS
 * 收发（lpwork）前面。它绝大部分时间阻塞在信号量/信号上，不占 CPU。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>

#include <nuttx/clock.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/sched.h>
#include <nuttx/timers/rtc.h>

#include "sf32lb52_rtc_alarm.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RTC_ALARM_DEV              "/dev/rtc0"

/* alarm 到点由 RTC upper half 用信号通知（read() 是 EOF、没有 poll）。 */

#define RTC_ALARM_SIGNO            SIGUSR1

#define RTC_ALARM_WORKER_NAME      "rtcalarm"
#define RTC_ALARM_WORKER_PRIORITY  115
#define RTC_ALARM_WORKER_STACK     4096

/* tm_year >= 100 即 >= 2000 年。板子没有 RTC 备份电池，掉电后读出来是
 * 2000-01-01 附近的垃圾值，所以这是"时间是否已经对过"的判据。
 * （下层已修读回世纪位，读回来的年份就是对的，不要再自己加 100。） */

#define RTC_ALARM_VALID_YEAR       100

/* 等工作线程状态变化的检查粒度（ms）。等 alarm 期间每秒醒一次，用于发现
 * "硬件 alarm 漏触发"；alarm 信号和 cancel/新任务都能立刻把它唤醒。 */

#define RTC_ALARM_WAIT_SLICE_MS    1000

/* 读时间 / 设 alarm 失败后的重试间隔（ms），避免失败时忙等。 */

#define RTC_ALARM_FAIL_RETRY_MS    (30 * 1000)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rtc_alarm_priv_s
{
  mutex_t        lock;        /* 保护下面所有状态（模块内只有这一把锁） */
  sem_t          wake;        /* at_daily/cancel 唤醒工作线程 */
  bool           initialized; /* 工作线程已创建 */
  pid_t          worker;
  bool           running;     /* 有每日提醒在跑 */
  uint32_t       seq;         /* at_daily/cancel 时自增；工作线程据此判定"任务变了" */
  int            hour;
  int            min;
  rtc_alarm_cb_t cb;
  FAR void      *cb_arg;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 锁和信号量静态初始化，这样 rtc_alarm_now()/rtc_alarm_time_valid()
 * 在模块初始化前也能安全调用。 */

static struct rtc_alarm_priv_s g_rtc_alarm =
{
  .lock = NXMUTEX_INITIALIZER,
  .wake = SEM_INITIALIZER(0),
};

/* alarm 到点标志：信号处理函数写、工作线程读（都在工作线程上下文）。 */

static volatile sig_atomic_t g_rtc_alarm_fired;

/* 硬件 alarm 槽当前是否被本模块占着（只由工作线程读写）。 */

static bool g_rtc_alarm_armed;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void rtc_alarm_sighandler(int signo)
{
  (void)signo;
  g_rtc_alarm_fired = 1;
}

/****************************************************************************
 * Name: rtc_alarm_read_time
 *
 * Description: 读一次 RTC 时间。成功返回 OK，失败返回负 errno。
 *
 ****************************************************************************/

static int rtc_alarm_read_time(int fd, FAR struct rtc_time *out)
{
  memset(out, 0, sizeof(*out));

  if (ioctl(fd, RTC_RD_TIME, (unsigned long)out) < 0)
    {
      return -errno;
    }

  return OK;
}

static bool rtc_alarm_year_ok(FAR const struct rtc_time *t)
{
  return t->tm_year >= RTC_ALARM_VALID_YEAR;
}

static bool rtc_alarm_is_leap(int year)
{
  return (year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0));
}

/****************************************************************************
 * Name: rtc_alarm_days_in_month
 *
 * Description: 给定公历年月，返回该月天数。mon 是 1..12。
 *
 ****************************************************************************/

static int rtc_alarm_days_in_month(int year, int mon)
{
  static const uint8_t days[12] =
  {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
  };

  if (mon < 1 || mon > 12)
    {
      return 0;
    }

  if (mon == 2 && rtc_alarm_is_leap(year))
    {
      return 29;
    }

  return days[mon - 1];
}

/****************************************************************************
 * Name: rtc_alarm_weekday
 *
 * Description:
 *   由公历日期直接算星期（0 = 周日 .. 6 = 周六，与 struct rtc_time 一致）。
 *   Sakamoto 算法，不依赖 libc。tm_wday 虽然被下层的 AlarmMask 屏蔽掉了，
 *   但以后可能去掉那个屏蔽，这里必须填对。
 *
 ****************************************************************************/

static int rtc_alarm_weekday(int year, int mon, int day)
{
  static const int t[12] =
  {
    0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4
  };
  int y = year;

  if (mon < 3)
    {
      y -= 1;
    }

  return (y + y / 4 - y / 100 + y / 400 + t[mon - 1] + day) % 7;
}

/****************************************************************************
 * Name: rtc_alarm_cmp
 *
 * Description: 按 年/月/日/时/分/秒 比较两个时间。a<b 返回 -1，a==b 返回 0。
 *
 ****************************************************************************/

static int rtc_alarm_cmp(FAR const struct rtc_time *a,
                         FAR const struct rtc_time *b)
{
  if (a->tm_year != b->tm_year)
    {
      return a->tm_year < b->tm_year ? -1 : 1;
    }

  if (a->tm_mon != b->tm_mon)
    {
      return a->tm_mon < b->tm_mon ? -1 : 1;
    }

  if (a->tm_mday != b->tm_mday)
    {
      return a->tm_mday < b->tm_mday ? -1 : 1;
    }

  if (a->tm_hour != b->tm_hour)
    {
      return a->tm_hour < b->tm_hour ? -1 : 1;
    }

  if (a->tm_min != b->tm_min)
    {
      return a->tm_min < b->tm_min ? -1 : 1;
    }

  if (a->tm_sec != b->tm_sec)
    {
      return a->tm_sec < b->tm_sec ? -1 : 1;
    }

  return 0;
}

/****************************************************************************
 * Name: rtc_alarm_same_minute
 *
 * Description:
 *   两个时间是否落在同一分钟（可比 cmp 宽松一点用）。alarm 是秒精度的
 *   信号，可能比 RTC 读回的秒早那么一点点，用它避免把一个正常到点的
 *   回调误判成"早到的陈旧信号"。
 *
 ****************************************************************************/

static bool rtc_alarm_same_minute(FAR const struct rtc_time *a,
                                  FAR const struct rtc_time *b)
{
  return a->tm_year == b->tm_year && a->tm_mon  == b->tm_mon &&
         a->tm_mday == b->tm_mday && a->tm_hour == b->tm_hour &&
         a->tm_min  == b->tm_min;
}

/****************************************************************************
 * Name: rtc_alarm_next_time
 *
 * Description:
 *   从当前时间 now 算出"下一个 hour:min:00"。
 *   - 今天这个时刻还没到（含正好等于的情况要排明天，避免立刻重触发）就
 *     用今天，否则日期 +1 天；
 *   - 日期进位用月长表 + 闰年判断手算，跨月/跨年都正确；
 *   - tm_wday 按最终日期重算，保证填对。
 *
 * Returned Value:
 *   OK on success; -EINVAL 如果 now 不是个合法时间。
 *
 ****************************************************************************/

static int rtc_alarm_next_time(FAR const struct rtc_time *now,
                               int hour, int min,
                               FAR struct rtc_time *out)
{
  int year = now->tm_year + 1900;
  int mon  = now->tm_mon + 1;

  if (now->tm_year < RTC_ALARM_VALID_YEAR ||
      mon < 1 || mon > 12 ||
      now->tm_mday < 1 ||
      now->tm_mday > rtc_alarm_days_in_month(year, mon) ||
      now->tm_hour < 0 || now->tm_hour > 23 ||
      now->tm_min < 0 || now->tm_min > 59)
    {
      return -EINVAL;
    }

  *out = *now;
  out->tm_hour = hour;
  out->tm_min  = min;
  out->tm_sec  = 0;

  /* 当前分钟 >= 目标分钟：今天这个点已经过了（或正是这一分钟），排明天。
   * 用 ">=" 而不是 ">"：如果现在是 08:00:00，设"今天 08:00:00"会立刻
   * 触发甚至不再触发，所以直接排明天。 */

  if (now->tm_hour * 60 + now->tm_min >= hour * 60 + min)
    {
      int mday = out->tm_mday + 1;

      if (mday > rtc_alarm_days_in_month(year, mon))
        {
          mday = 1;
          mon++;
          if (mon > 12)
            {
              mon = 1;
              year++;
            }
        }

      out->tm_year = year - 1900;
      out->tm_mon  = mon - 1;
      out->tm_mday = mday;
    }

  out->tm_wday  = rtc_alarm_weekday(out->tm_year + 1900,
                                    out->tm_mon + 1, out->tm_mday);
  out->tm_yday  = 0;
  out->tm_isdst = 0;
  return OK;
}

/****************************************************************************
 * Name: rtc_alarm_arm
 *
 * Description:
 *   把绝对时间 when 设进硬件 alarm 槽（id 只能是 0）。到点驱动会给
 *   本工作线程发 RTC_ALARM_SIGNO。成功返回 OK，失败返回负 errno。
 *
 ****************************************************************************/

static int rtc_alarm_arm(int fd, FAR const struct rtc_time *when)
{
  struct rtc_setalarm_s abs;

  memset(&abs, 0, sizeof(abs));
  abs.id                 = 0;                 /* 只有 1 个槽，必须 0 */
  abs.pid                = 0;                 /* 0 = 通知调用者（本工作线程）自己 */
  abs.event.sigev_notify = SIGEV_SIGNAL;
  abs.event.sigev_signo  = RTC_ALARM_SIGNO;
  abs.time               = *when;

  /* 清掉上一个 alarm 可能遗留的信号标志，再设新的：
   * 这之后收到的信号就都属于这次的 alarm。 */

  g_rtc_alarm_fired = 0;

  if (ioctl(fd, RTC_SET_ALARM, (unsigned long)&abs) < 0)
    {
      return -errno;
    }

  return OK;
}

/****************************************************************************
 * Name: rtc_alarm_disarm
 *
 * Description: 取消硬件 alarm 槽（只在确实占着时调）。
 *
 ****************************************************************************/

static void rtc_alarm_disarm(int fd)
{
  if (ioctl(fd, RTC_CANCEL_ALARM, 0) < 0)
    {
      syslog(LOG_WARNING, "RTCALARM: RTC_CANCEL_ALARM failed: %d\n", errno);
    }
}

/****************************************************************************
 * Name: rtc_alarm_wait_wake
 *
 * Description:
 *   带超时地等工作线程信号量。OK = 被唤醒（cancel 或新任务）；
 *   -ETIMEDOUT = 超时；-EINTR = 被 alarm 信号打断。
 *
 ****************************************************************************/

static int rtc_alarm_wait_wake(uint32_t ms)
{
  return nxsem_tickwait(&g_rtc_alarm.wake, MSEC2TICK(ms));
}

/****************************************************************************
 * Name: rtc_alarm_job_changed
 *
 * Description:
 *   判断当前任务是否已不是 gen 那一代（被 cancel 或换了新的每日提醒）。
 *
 ****************************************************************************/

static bool rtc_alarm_job_changed(uint32_t gen)
{
  bool changed;

  nxmutex_lock(&g_rtc_alarm.lock);
  changed = !g_rtc_alarm.running || g_rtc_alarm.seq != gen;
  nxmutex_unlock(&g_rtc_alarm.lock);

  return changed;
}

/****************************************************************************
 * Name: rtc_alarm_dispatch
 *
 * Description:
 *   在**工作线程**上下文里回调上层。回调前再确认这一代任务还在跑，
 *   避免"刚 cancel 完又被回调"。
 *
 ****************************************************************************/

static void rtc_alarm_dispatch(uint32_t gen)
{
  rtc_alarm_cb_t cb  = NULL;
  FAR void      *arg = NULL;

  nxmutex_lock(&g_rtc_alarm.lock);
  if (g_rtc_alarm.running && g_rtc_alarm.seq == gen)
    {
      cb  = g_rtc_alarm.cb;
      arg = g_rtc_alarm.cb_arg;
    }

  nxmutex_unlock(&g_rtc_alarm.lock);

  if (cb != NULL)
    {
      cb(arg);
    }
  else
    {
      syslog(LOG_INFO, "RTCALARM: daily alarm fired (no callback)\n");
    }
}

/****************************************************************************
 * Name: rtc_alarm_wait_valid
 *
 * Description:
 *   时间还没对过（< 2000 年）时的等待循环：每 RTC_ALARM_TIME_RETRY_MS
 *   重试一次，等别处把 RTC 对好；期间能被 cancel / 新任务立刻打断。
 *
 * Returned Value:
 *   0  = 时间已经有效；
 *   1  = 被 cancel 或换了新任务，调用者应退出本轮；
 *   -1 = 读时间一直失败（同样让调用者退出本轮重来）。
 *
 ****************************************************************************/

static int rtc_alarm_wait_valid(int fd, uint32_t gen)
{
  bool warned = false;

  for (;;)
    {
      struct rtc_time now;
      int ret = rtc_alarm_read_time(fd, &now);

      if (ret == OK && rtc_alarm_year_ok(&now))
        {
          if (warned)
            {
              syslog(LOG_INFO,
                     "RTCALARM: RTC time is now valid: "
                     "%04d-%02d-%02d %02d:%02d:%02d\n",
                     now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
                     now.tm_hour, now.tm_min, now.tm_sec);
            }

          return 0;
        }

      if (!warned)
        {
          if (ret == OK)
            {
              syslog(LOG_WARNING,
                     "RTCALARM: RTC time not set (year %d < 2000); "
                     "waiting for time sync, retry every %u s\n",
                     now.tm_year + 1900, RTC_ALARM_TIME_RETRY_MS / 1000);
            }
          else
            {
              syslog(LOG_WARNING,
                     "RTCALARM: RTC_RD_TIME failed: %d; "
                     "retry every %u s\n",
                     ret, RTC_ALARM_TIME_RETRY_MS / 1000);
            }

          warned = true;
        }

      ret = rtc_alarm_wait_wake(RTC_ALARM_TIME_RETRY_MS);

      if (ret == OK && rtc_alarm_job_changed(gen))
        {
          return 1;
        }
      else if (ret != OK && ret != -ETIMEDOUT && ret != -EINTR)
        {
          /* 信号量本身出错（不该发生）：让调用者退出本轮，下轮重来 */

          syslog(LOG_ERR, "RTCALARM: wake wait failed: %d\n", ret);
          return -1;
        }
    }
}

/****************************************************************************
 * Name: rtc_alarm_wait_fire
 *
 * Description:
 *   等这一次 alarm 到点。返回：
 *     RTC_ALARM_FIRED     = 收到信号，到点了；
 *     RTC_ALARM_CANCELLED = 被 cancel / 换了新任务；
 *     RTC_ALARM_MISSED    = 已经过了目标时刻还没收到信号（漏触发），
 *                           需要重排。
 *
 *   每秒醒一次做"是否漏触发"检查；cancel/新任务能立刻唤醒它。
 *
 ****************************************************************************/

enum rtc_alarm_wait_e
{
  RTC_ALARM_FIRED = 0,
  RTC_ALARM_CANCELLED,
  RTC_ALARM_MISSED,
};

static int rtc_alarm_wait_fire(int fd, FAR const struct rtc_time *target,
                               uint32_t gen)
{
  for (;;)
    {
      struct rtc_time now;
      int ret;

      if (g_rtc_alarm_fired)
        {
          g_rtc_alarm_fired = 0;
          return RTC_ALARM_FIRED;
        }

      if (rtc_alarm_job_changed(gen))
        {
          return RTC_ALARM_CANCELLED;
        }

      ret = rtc_alarm_wait_wake(RTC_ALARM_WAIT_SLICE_MS);

      if (ret == OK)
        {
          /* 被 cancel / 新任务唤醒：回循环顶部统一判断 */
          continue;
        }

      if (ret == -ETIMEDOUT)
        {
          /* 目标时刻已过还是没信号：硬件 alarm 没触发（可能被别处覆盖了
           * 槽、或时间被改过）。报一条 WARNING 后重排，别永远哑掉。 */

          if (rtc_alarm_read_time(fd, &now) == OK &&
              rtc_alarm_cmp(&now, target) >= 0)
            {
              syslog(LOG_WARNING,
                     "RTCALARM: alarm missed (target %04d-%02d-%02d "
                     "%02d:%02d:00, now %04d-%02d-%02d %02d:%02d:%02d); "
                     "re-arming\n",
                     target->tm_year + 1900, target->tm_mon + 1,
                     target->tm_mday, target->tm_hour, target->tm_min,
                     now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
                     now.tm_hour, now.tm_min, now.tm_sec);
              return RTC_ALARM_MISSED;
            }
        }
      else if (ret != -EINTR)
        {
          syslog(LOG_ERR, "RTCALARM: wake wait failed: %d\n", ret);
        }
    }
}

/****************************************************************************
 * Name: rtc_alarm_run
 *
 * Description:
 *   一个每日提醒的完整生命周期：算下一个 hh:mm -> 设 alarm -> 等到点 ->
 *   回调 -> 再排下一天。被 cancel / 换新任务时返回。
 *
 *   整个函数只在工作线程里跑，是唯一碰 /dev/rtc0 和硬件 alarm 槽的地方。
 *
 ****************************************************************************/

static void rtc_alarm_run(void)
{
  uint32_t gen;
  int      hour;
  int      min;
  int      fd;

  nxmutex_lock(&g_rtc_alarm.lock);
  gen  = g_rtc_alarm.seq;
  hour = g_rtc_alarm.hour;
  min  = g_rtc_alarm.min;
  nxmutex_unlock(&g_rtc_alarm.lock);

  fd = open(RTC_ALARM_DEV, O_RDONLY);
  if (fd < 0)
    {
      syslog(LOG_ERR, "RTCALARM: open %s failed: %d; retry in %u s\n",
             RTC_ALARM_DEV, errno, RTC_ALARM_FAIL_RETRY_MS / 1000);

      /* 可被打断地等一会，避免 open 一直失败时忙等 */

      rtc_alarm_wait_wake(RTC_ALARM_FAIL_RETRY_MS);
      return;
    }

  for (;;)
    {
      struct rtc_time now;
      struct rtc_time target;
      int ret;

      if (rtc_alarm_job_changed(gen))
        {
          break;
        }

      if (rtc_alarm_read_time(fd, &now) < 0)
        {
          syslog(LOG_ERR,
                 "RTCALARM: RTC_RD_TIME failed: %d; retry in %u s\n",
                 errno, RTC_ALARM_FAIL_RETRY_MS / 1000);
          rtc_alarm_wait_wake(RTC_ALARM_FAIL_RETRY_MS);
          continue;
        }

      if (!rtc_alarm_year_ok(&now))
        {
          /* 时间还没对：不排 alarm，等对时（可被 cancel 打断） */

          if (rtc_alarm_wait_valid(fd, gen) != 0)
            {
              break;
            }

          continue;
        }

      ret = rtc_alarm_next_time(&now, hour, min, &target);
      if (ret < 0)
        {
          syslog(LOG_ERR,
                 "RTCALARM: bad RTC time %04d-%02d-%02d %02d:%02d:%02d: %d; "
                 "retry in %u s\n",
                 now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
                 now.tm_hour, now.tm_min, now.tm_sec, ret,
                 RTC_ALARM_FAIL_RETRY_MS / 1000);
          rtc_alarm_wait_wake(RTC_ALARM_FAIL_RETRY_MS);
          continue;
        }

      ret = rtc_alarm_arm(fd, &target);
      if (ret < 0)
        {
          syslog(LOG_ERR,
                 "RTCALARM: RTC_SET_ALARM failed: %d; retry in %u s\n",
                 ret, RTC_ALARM_FAIL_RETRY_MS / 1000);
          rtc_alarm_wait_wake(RTC_ALARM_FAIL_RETRY_MS);
          continue;
        }

      g_rtc_alarm_armed = true;

      syslog(LOG_INFO,
             "RTCALARM: daily %02d:%02d armed for %04d-%02d-%02d "
             "(wday=%d, now %04d-%02d-%02d %02d:%02d:%02d)\n",
             hour, min,
             target.tm_year + 1900, target.tm_mon + 1, target.tm_mday,
             target.tm_wday,
             now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
             now.tm_hour, now.tm_min, now.tm_sec);

      ret = rtc_alarm_wait_fire(fd, &target, gen);

      if (ret == RTC_ALARM_CANCELLED)
        {
          break;                    /* 槽还占着，下面统一取消 */
        }

      /* FIRED / MISSED：硬件槽已释放（或被覆盖），回到循环顶部重排下一天 */

      g_rtc_alarm_armed = false;

      if (ret == RTC_ALARM_FIRED)
        {
          /* 再读一次时间确认这不是一个早到的/陈旧的信号：正常到点应该
           * now >= target（或至少和 target 在同一分钟）。这样即使以后
           * 重排后收到上一个 alarm 的迟到信号，也不会误触发回调。 */

          struct rtc_time check;

          if (rtc_alarm_read_time(fd, &check) == OK &&
              rtc_alarm_cmp(&check, &target) < 0 &&
              !rtc_alarm_same_minute(&check, &target))
            {
              syslog(LOG_WARNING,
                     "RTCALARM: early/stale alarm signal ignored\n");
            }
          else
            {
              rtc_alarm_dispatch(gen);
            }
        }
    }

  if (g_rtc_alarm_armed)
    {
      rtc_alarm_disarm(fd);
      g_rtc_alarm_armed = false;
    }

  close(fd);
}

/****************************************************************************
 * Name: rtc_alarm_worker
 *
 * Description:
 *   模块工作线程主循环。信号处理函数必须装在**这个线程**里，因为
 *   RTC alarm 的 pid=0 会解析成"设 alarm 的任务"，本线程才是收信号的人。
 *
 ****************************************************************************/

static int rtc_alarm_worker(int argc, FAR char *argv[])
{
  struct sigaction sa;

  (void)argc;
  (void)argv;

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = rtc_alarm_sighandler;
  sigemptyset(&sa.sa_mask);

  if (sigaction(RTC_ALARM_SIGNO, &sa, NULL) < 0)
    {
      syslog(LOG_ERR, "RTCALARM: sigaction(SIGUSR1) failed: %d; "
             "alarm signals will not be received\n", errno);
    }

  for (;;)
    {
      bool running;

      nxmutex_lock(&g_rtc_alarm.lock);
      running = g_rtc_alarm.running;
      nxmutex_unlock(&g_rtc_alarm.lock);

      if (!running)
        {
          /* 没有任务时纯等唤醒；偶发的 -EINTR 忽略，重判状态 */

          nxsem_wait(&g_rtc_alarm.wake);
          continue;
        }

      rtc_alarm_run();

      /* run() 返回说明被 cancel 或换了新任务；回顶部重判 */
    }

  return 0;
}

/****************************************************************************
 * Name: rtc_alarm_lazy_init
 *
 * Description: 懒初始化：创建常驻工作线程。幂等。
 *
 ****************************************************************************/

static int rtc_alarm_lazy_init(void)
{
  pid_t pid;

  nxmutex_lock(&g_rtc_alarm.lock);
  if (g_rtc_alarm.initialized)
    {
      nxmutex_unlock(&g_rtc_alarm.lock);
      return OK;
    }

  g_rtc_alarm.initialized = true;
  nxmutex_unlock(&g_rtc_alarm.lock);

  pid = task_create(RTC_ALARM_WORKER_NAME, RTC_ALARM_WORKER_PRIORITY,
                    RTC_ALARM_WORKER_STACK, (main_t)rtc_alarm_worker, NULL);
  if (pid < 0)
    {
      nxmutex_lock(&g_rtc_alarm.lock);
      g_rtc_alarm.initialized = false;
      nxmutex_unlock(&g_rtc_alarm.lock);

      syslog(LOG_ERR, "RTCALARM: task_create failed: %d\n", (int)pid);
      return (int)pid;
    }

  g_rtc_alarm.worker = pid;
  syslog(LOG_INFO, "RTCALARM: worker started (pid=%d)\n", (int)pid);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int rtc_alarm_at_daily(int hour, int min, rtc_alarm_cb_t cb, FAR void *arg)
{
  int ret;

  if (hour < 0 || hour > 23 || min < 0 || min > 59)
    {
      syslog(LOG_ERR, "RTCALARM: invalid daily time %02d:%02d\n", hour, min);
      return -EINVAL;
    }

  ret = rtc_alarm_lazy_init();
  if (ret < 0)
    {
      return ret;
    }

  nxmutex_lock(&g_rtc_alarm.lock);
  g_rtc_alarm.hour   = hour;
  g_rtc_alarm.min    = min;
  g_rtc_alarm.cb     = cb;
  g_rtc_alarm.cb_arg = arg;
  g_rtc_alarm.running = true;
  g_rtc_alarm.seq++;
  nxmutex_unlock(&g_rtc_alarm.lock);

  nxsem_post(&g_rtc_alarm.wake);
  return OK;
}

int rtc_alarm_cancel(void)
{
  bool was_running;

  nxmutex_lock(&g_rtc_alarm.lock);
  was_running = g_rtc_alarm.running;
  g_rtc_alarm.running = false;
  g_rtc_alarm.seq++;
  nxmutex_unlock(&g_rtc_alarm.lock);

  if (was_running)
    {
      nxsem_post(&g_rtc_alarm.wake);
    }

  return OK;
}

int rtc_alarm_now(FAR struct rtc_time *out)
{
  int fd;
  int ret;

  if (out == NULL)
    {
      return -EINVAL;
    }

  fd = open(RTC_ALARM_DEV, O_RDONLY);
  if (fd < 0)
    {
      syslog(LOG_ERR, "RTCALARM: open %s failed: %d\n",
             RTC_ALARM_DEV, errno);
      return -errno;
    }

  ret = rtc_alarm_read_time(fd, out);
  close(fd);

  if (ret < 0)
    {
      syslog(LOG_ERR, "RTCALARM: RTC_RD_TIME failed: %d\n", ret);
    }

  return ret;
}

int rtc_alarm_time_valid(void)
{
  struct rtc_time now;

  if (rtc_alarm_now(&now) < 0)
    {
      return 0;
    }

  return rtc_alarm_year_ok(&now) ? 1 : 0;
}
