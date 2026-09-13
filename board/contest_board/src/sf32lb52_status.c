/****************************************************************************
 * board/contest_board/src/sf32lb52_status.c
 *
 * SF32LB52 统一外设状态查询：只读、轻量、不初始化设备。
 *
 * 每个字段的探法（都很轻，见 sf32lb52_status.h 的约定）：
 *
 *   网络      SIOCGIFCONF 枚举 netdev（标准 netdev API，socket + ioctl），
 *             只列"UP 且有 IPv4 地址"的接口，跳过回环，取第一个；
 *             拿到地址就算 net_link_up，并把 IP 抄进 net_ip。
 *   存储      stat("/etc/assets") + S_ISDIR；/data 则是真的 open(O_CREAT)
 *             建一个 0 字节探针文件后立刻 close + unlink（/data 是 tmpfs，
 *             不写 flash）。
 *   音频      open("/dev/audio/audio0", O_WRONLY / O_RDONLY) 后立刻 close。
 *   显示      open("/dev/lcd0", O_RDONLY) 后立刻 close。
 *   触摸      open("/dev/input0", O_RDONLY | O_NONBLOCK) 后立刻 close。
 *   按键      board_btn_is_pressed(BOARD_BTN_KEY)（PA11，不用先 init）。
 *   RTC       open("/dev/rtc0") + ioctl(RTC_RD_TIME)，年份 >= 2000 才算有效。
 *   运行时间  clock_systime_ticks() 换算成秒。
 *
 * 注意 **不要把 access() 当成"可写"判断**：这个 NuttX 版的 access() 对
 * 目录一律返回 0（lib_access.c 的注释：NuttX 里没有用户，永远成功），
 * 所以 /data 必须真的写一个文件才知道能不能写。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/clock.h>
#include <nuttx/timers/rtc.h>

#include "sf32lb52_boardbtn.h"
#include "sf32lb52_status.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* RTC 年份下限：tm_year 是"1900 起的年数"，>= 100 即 >= 2000 年。
 * 板子没有 RTC 备份电池，掉电后读出来是 1970 / 1990 这类值。 */

#define STATUS_RTC_VALID_YEAR_MIN 100

/* /data 写探针的文件名（每次探完立刻删掉） */

#define STATUS_DATA_PROBE_FILE    BOARD_STATUS_DATA_DIR "/.status_probe"

/* SIOCGIFCONF 一次最多列几个接口：本板最多 eth0 + lo，给 4 个足够 */

#define STATUS_MAX_IFREQ          4

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* MQTT 连接状态：板级模块不认识应用层的 MQTT 客户端，只能由使用方喂 */

static volatile bool g_status_mqtt;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: status_node_openable
 *
 * Description:
 *   open() 一下立刻 close()，只回答"这个节点现在能不能打开"。
 *   不做任何 ioctl / read / write，不动设备状态。
 *
 ****************************************************************************/

static bool status_node_openable(FAR const char *path, int flags)
{
  int fd;

  fd = open(path, flags);
  if (fd < 0)
    {
      return false;
    }

  close(fd);
  return true;
}

/****************************************************************************
 * Name: status_probe_net
 *
 * Description:
 *   用标准 netdev API 查"有没有非回环的 IPv4 地址"。
 *   SIOCGIFCONF 只会返回 UP 且已配 IPv4 的接口，回环单独按名字跳过。
 *
 ****************************************************************************/

static void status_probe_net(FAR struct board_status_s *out)
{
#if defined(CONFIG_NET) && defined(CONFIG_NET_IPv4)
  struct ifreq ifr[STATUS_MAX_IFREQ];
  struct ifconf ifc;
  int nreq;
  int fd;
  int i;

  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    {
      syslog(LOG_WARNING, "STATUS: socket() failed: %d\n", errno);
      return;
    }

  memset(ifr, 0, sizeof(ifr));
  memset(&ifc, 0, sizeof(ifc));
  ifc.ifc_len = sizeof(ifr);          /* 入参 = 缓冲区大小 */
  ifc.ifc_buf = (FAR char *)ifr;

  if (ioctl(fd, SIOCGIFCONF, (unsigned long)&ifc) < 0)
    {
      syslog(LOG_WARNING, "STATUS: SIOCGIFCONF failed: %d\n", errno);
      close(fd);
      return;
    }

  close(fd);

  nreq = (int)(ifc.ifc_len / sizeof(struct ifreq));

  for (i = 0; i < nreq && i < STATUS_MAX_IFREQ; i++)
    {
      FAR struct sockaddr_in *sin =
        (FAR struct sockaddr_in *)&ifr[i].ifr_addr;

      if (strcmp(ifr[i].ifr_name, "lo") == 0)
        {
          continue;                     /* 回环不算"有网" */
        }

      if (sin->sin_addr.s_addr == 0)
        {
          continue;
        }

      out->net_link_up = true;
      snprintf(out->net_ip, sizeof(out->net_ip), "%s",
               inet_ntoa(sin->sin_addr));
      break;
    }
#else
  (void)out;                            /* 没编网络：net_link_up 保持 false */
#endif
}

/****************************************************************************
 * Name: status_data_writable
 *
 * Description:
 *   真的往 /data 写一个 0 字节文件，再删掉。
 *   （不能用 access()：这一版的 access() 对目录永远返回 0，见文件头。）
 *
 ****************************************************************************/

static bool status_data_writable(void)
{
  int fd;

  fd = open(STATUS_DATA_PROBE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    {
      return false;
    }

  close(fd);
  unlink(STATUS_DATA_PROBE_FILE);       /* 删不掉不影响"能写"的结论 */
  return true;
}

/****************************************************************************
 * Name: status_rtc_read
 *
 * Description:
 *   读一次 RTC 时间。`out` 可为 NULL（只判有效性）。有效返回 true。
 *
 ****************************************************************************/

static bool status_rtc_read(FAR struct rtc_time *out)
{
  struct rtc_time tm;
  int fd;

  fd = open(BOARD_STATUS_RTC_DEV, O_RDONLY);
  if (fd < 0)
    {
      return false;
    }

  memset(&tm, 0, sizeof(tm));
  if (ioctl(fd, RTC_RD_TIME, (unsigned long)&tm) < 0)
    {
      close(fd);
      return false;
    }

  close(fd);

  if (out != NULL)
    {
      *out = tm;
    }

  return tm.tm_year >= STATUS_RTC_VALID_YEAR_MIN;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int board_status_get(FAR struct board_status_s *out)
{
  if (out == NULL)
    {
      return -EINVAL;
    }

  memset(out, 0, sizeof(*out));

  /* 网络：网卡是 netdev，不是 /dev 节点 */

  status_probe_net(out);

  /* MQTT：使用方喂进来的值 */

  out->mqtt_connected = g_status_mqtt;

  /* 存储 */

  {
    struct stat st;

    out->rom_assets_ready =
      (stat(BOARD_STATUS_ROM_ASSETS, &st) == 0) && S_ISDIR(st.st_mode);
  }

  out->data_ready = status_data_writable();

  /* 音频：本板播放和录音是同一个节点，方向只体现在 open 的 flags 上 */

  out->audio_play_ready =
    status_node_openable(BOARD_STATUS_AUDIO_DEV, O_WRONLY);
  out->audio_rec_ready =
    status_node_openable(BOARD_STATUS_AUDIO_DEV, O_RDONLY);

  /* 显示 / 触摸：只敲一下门，不读不写 */

  out->lcd_ready = status_node_openable(BOARD_STATUS_LCD_DEV, O_RDONLY);
  out->touch_ready = status_node_openable(BOARD_STATUS_TOUCH_DEV,
                                          O_RDONLY | O_NONBLOCK);

  /* 按键：模块没 init 时给的就是 PA11 的即时电平 */

  {
    int pressed = board_btn_is_pressed(BOARD_BTN_KEY);

    out->btn_key_pressed = (pressed > 0);
  }

  /* 时间 */

  out->rtc_valid = status_rtc_read(NULL);
  out->uptime_s  = (uint32_t)(TICK2MSEC(clock_systime_ticks()) / 1000);

  return OK;
}

int board_status_dump(void)
{
  struct board_status_s st;
  struct rtc_time tm;
  bool have_tm;
  int ret;

  ret = board_status_get(&st);
  if (ret < 0)
    {
      printf("外设状态: board_status_get 失败: %d\n", ret);
      return ret;
    }

  have_tm = status_rtc_read(&tm);

  printf("外设状态（board_status_get 快照）\n");

  if (st.net_link_up)
    {
      printf("  网络        : 已获取 IPv4 %s\n", st.net_ip);
    }
  else
    {
      printf("  网络        : 没有非回环 IPv4 地址"
             "（USB RNDIS 没枚举起来？）\n");
    }

  printf("  MQTT        : %s\n",
         st.mqtt_connected ? "已连接"
                           : "未连接（由使用方 board_status_set_mqtt() 喂）");

  printf("  ROM 素材    : %s\n", st.rom_assets_ready
         ? BOARD_STATUS_ROM_ASSETS " 可读" : BOARD_STATUS_ROM_ASSETS " 读不到");
  printf("  /data       : %s\n", st.data_ready
         ? "可写" : "写不了（tmpfs 没挂上？）");

  printf("  音频播放    : %s\n", st.audio_play_ready
         ? BOARD_STATUS_AUDIO_DEV " 可打开" : BOARD_STATUS_AUDIO_DEV " 打不开");
  printf("  音频录音    : %s\n", st.audio_rec_ready
         ? BOARD_STATUS_AUDIO_DEV " 可打开" : BOARD_STATUS_AUDIO_DEV " 打不开");

  printf("  显示        : %s\n", st.lcd_ready
         ? BOARD_STATUS_LCD_DEV " 可打开" : BOARD_STATUS_LCD_DEV " 打不开");
  printf("  触摸        : %s\n", st.touch_ready
         ? BOARD_STATUS_TOUCH_DEV " 可打开" : BOARD_STATUS_TOUCH_DEV " 打不开");

  printf("  按键 PA11   : %s\n", st.btn_key_pressed ? "按下" : "松开");

  if (have_tm)
    {
      printf("  RTC         : 有效 %04d-%02d-%02d %02d:%02d:%02d\n",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
  else
    {
      printf("  RTC         : 无效（年份 < 2000，RTC 还没对过时）\n");
    }

  printf("  运行时间    : %u s\n", (unsigned)st.uptime_s);

  return OK;
}

void board_status_set_mqtt(bool connected)
{
  g_status_mqtt = connected;
}
