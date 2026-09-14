/*
 * time_sync.c —— 联网后自动对时（取 HTTPS 响应的 Date: 头）
 *
 * 为什么不用 NTP：板子只有 RNDIS 这一条上行，走 HTTPS 顺便拿到时间最省事；
 * 而且 ai_agent 里已经有 vela_https_head_date()，直接复用，不额外引入依赖。
 *
 * 注意：本构建没开 CONFIG_LIBC_LOCALTIME，`localtime` 实际就是 `gmtime`，
 * 所以这里写入的就是 UTC；界面显示本地时间是自己加偏移的。
 */

#include <nuttx/config.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

#include "infra/vela_tls.h"
#include "infra/config_store.h"

#include "time_sync.h"

/* 拿 Date 头用的默认目标：能通就行，只发一个 HEAD。
 * 优先用配置里的大模型 host（它一定是我们能连上的那个），拿不到再退到 Bark。 */
#define TIME_SYNC_HOST_DEFAULT "api.day.app"
#define TIME_SYNC_PATH         "/"
#define TIME_SYNC_PORT         "443"

static const char *g_months[12] =
{
  "Jan", "Feb", "Mar", "Apr", "May", "Jun",
  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/* Howard Hinnant 的 days_from_civil：把 (年,月,日) 换成"距 1970-01-01 的天数" */
static long days_from_civil(int y, unsigned m, unsigned d)
{
  long era;
  unsigned yoe;
  unsigned doy;
  unsigned doe;

  y -= (m <= 2);
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = (unsigned)(y - era * 400);
  doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

  return era * 146097L + (long)doe - 719468L;
}

/* "Sun, 13 Sep 2026 15:30:00 GMT" -> epoch（UTC）。失败返回 -1。 */
static long parse_http_date(const char *s)
{
  const char *p;
  char mon[8];
  int day = 0;
  int year = 0;
  int hh = 0;
  int mm = 0;
  int ss = 0;
  int i;
  int monidx = -1;

  if (s == NULL)
    {
      return -1;
    }

  /* 跳过 "Sun," 这一段可选的部分 */
  p = strchr(s, ',');
  p = (p != NULL) ? (p + 1) : s;
  while (*p == ' ')
    {
      p++;
    }

  if (sscanf(p, "%d %3s %d %d:%d:%d",
             &day, mon, &year, &hh, &mm, &ss) != 6)
    {
      return -1;
    }

  for (i = 0; i < 12; i++)
    {
      if (strncmp(mon, g_months[i], 3) == 0)
        {
          monidx = i;
          break;
        }
    }

  /* 明显不合理就拒绝：板子的假时间会解析出 2000 年，服务器不会 */
  if (monidx < 0 || day < 1 || day > 31 || year < 2020 || year > 2099 ||
      hh > 23 || mm > 59 || ss > 60)
    {
      return -1;
    }

  return days_from_civil(year, monidx + 1, (unsigned)day) * 86400L +
         hh * 3600L + mm * 60L + ss;
}

int time_sync_once(void)
{
  char host[96];
  char date[96];
  long epoch;
  struct timespec ts;
  int ret;

  host[0] = '\0';
  if (claw_config_get("llm_host", host, sizeof(host)) != 0 || host[0] == '\0')
    {
      strncpy(host, TIME_SYNC_HOST_DEFAULT, sizeof(host) - 1);
      host[sizeof(host) - 1] = '\0';
    }

  date[0] = '\0';
  ret = vela_https_head_date(host, TIME_SYNC_PORT, TIME_SYNC_PATH,
                             date, sizeof(date));
  if (ret != 0)
    {
      printf("[TimeSync] 取 %s 的 Date 头失败: %d\n", host, ret);
      return -1;
    }

  epoch = parse_http_date(date);
  if (epoch < 0)
    {
      printf("[TimeSync] Date 头解析失败: '%s'\n", date);
      return -1;
    }

  ts.tv_sec = (time_t)epoch;
  ts.tv_nsec = 0;
  if (clock_settime(CLOCK_REALTIME, &ts) != 0)
    {
      printf("[TimeSync] clock_settime 失败: %d\n", errno);
      return -1;
    }

  /* 只打服务器给的那串时间，不打别的 */
  printf("[TimeSync] 已对时 -> %s (epoch=%ld)\n", date, epoch);
  return 0;
}
