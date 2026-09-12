/**
 * net_test.c - 板子网络连通性测试（与 zhi_ai 无关，纯测网络）
 *
 * 在 NSH 里执行：net_test
 *
 * 依次验证四层，从网卡一直到 MQTT 应用层：
 *   1. 网卡配置     eth0 有 IP / 网关
 *   2. DNS 解析     能解析 broker.emqx.io
 *   3. TCP 连接     能连上 broker.emqx.io:1883
 *   4. MQTT 握手    能收到 CONNACK（真正跑通应用层协议）
 *
 * 第 4 步会依次尝试几个公共 broker，任意一个回 CONNACK 就算通过：
 * 公共 broker（尤其 emqx 的免费公共实例）会限流或临时抽风，
 * 只测一个的话演示会"假失败"——板子明明是好的。
 *
 * 用来演示/验收 USB RNDIS 上网链路：USB -> RNDIS -> IP -> DNS -> TCP -> MQTT
 */

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <netutils/netlib.h>

#define NETTEST_IFNAME  "eth0"
#define NETTEST_BROKER  "broker.emqx.io"
#define NETTEST_PORT    1883
#define NETTEST_TIMEOUT 5   /* 秒 */

/* 第 4 步依次尝试这些公共 broker */
static const char *g_brokers[] =
{
  "broker.emqx.io",
  "test.mosquitto.org",
  "broker.hivemq.com"
};

#define NETTEST_NBROKERS ((int)(sizeof(g_brokers) / sizeof(g_brokers[0])))

static int g_pass;
static int g_total;

static void report(const char *name, int ok, const char *detail)
{
  g_total++;
  if (ok)
    {
      g_pass++;
    }

  printf("      [%s] %s", ok ? "PASS" : "FAIL", name);
  if (detail && detail[0])
    {
      printf("  (%s)", detail);
    }

  printf("\n");
}

static uint32_t mono_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/****************************************************************************
 * Name: tcp_connect_timeout
 *
 * Description:
 *   非阻塞 connect + select 超时，避免网络不通时测试卡死。
 *   成功返回已连接的 fd（已恢复阻塞模式），失败返回 -1。
 *
 ****************************************************************************/

static int tcp_connect_timeout(const char *ip, uint16_t port, uint32_t *ms)
{
  struct sockaddr_in addr;
  struct timeval tv;
  fd_set wset;
  socklen_t elen;
  int fd;
  int ret;
  int err;
  uint32_t t0;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    {
      return -1;
    }

  if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
    {
      close(fd);
      return -1;
    }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(port);
  inet_pton(AF_INET, ip, &addr.sin_addr);

  t0  = mono_ms();
  ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
  if (ret < 0 && errno != EINPROGRESS)
    {
      close(fd);
      return -1;
    }

  FD_ZERO(&wset);
  FD_SET(fd, &wset);
  tv.tv_sec  = NETTEST_TIMEOUT;
  tv.tv_usec = 0;

  ret = select(fd + 1, NULL, &wset, NULL, &tv);
  if (ret <= 0)
    {
      close(fd);
      return -1;
    }

  err  = 0;
  elen = sizeof(err);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0)
    {
      close(fd);
      return -1;
    }

  fcntl(fd, F_SETFL, 0);

  if (ms)
    {
      *ms = mono_ms() - t0;
    }

  return fd;
}

/****************************************************************************
 * Name: resolve
 *
 * Description:
 *   解析主机名，结果写进 buf。成功返回 0。
 *
 ****************************************************************************/

static int resolve(const char *host, char *buf, size_t buflen)
{
  struct hostent *he = gethostbyname(host);

  if (he == NULL || he->h_addr_list[0] == NULL)
    {
      return -1;
    }

  strncpy(buf, inet_ntoa(*(struct in_addr *)he->h_addr_list[0]), buflen - 1);
  buf[buflen - 1] = '\0';
  return 0;
}

/****************************************************************************
 * Name: mqtt_handshake
 *
 * Description:
 *   发一个最小的 MQTT CONNECT，等 CONNACK（0x20 0x02 0x00 <return code>）。
 *   返回 -1 失败；否则返回 CONNACK 的 return code（0 = 连接被接受）。
 *
 ****************************************************************************/

static int mqtt_handshake(int fd, const char *client_id)
{
  uint8_t pkt[64];
  uint8_t resp[4];
  size_t cidlen = strlen(client_id);
  int pos = 0;
  int got = 0;
  int n;

  pkt[pos++] = 0x10;                      /* CONNECT */
  pkt[pos++] = (uint8_t)(12 + cidlen);    /* 剩余长度 */
  pkt[pos++] = 0x00;                      /* 协议名长度 = 4 */
  pkt[pos++] = 0x04;
  memcpy(&pkt[pos], "MQTT", 4);
  pos += 4;
  pkt[pos++] = 0x04;                      /* 协议级别 3.1.1 */
  pkt[pos++] = 0x02;                      /* 连接标志：清理会话 */
  pkt[pos++] = 0x00;                      /* keepalive = 60s */
  pkt[pos++] = 0x3c;
  pkt[pos++] = (uint8_t)((cidlen >> 8) & 0xff);
  pkt[pos++] = (uint8_t)(cidlen & 0xff);
  memcpy(&pkt[pos], client_id, cidlen);
  pos += cidlen;

  if (send(fd, pkt, pos, 0) != pos)
    {
      return -1;
    }

  while (got < (int)sizeof(resp))
    {
      n = recv(fd, resp + got, sizeof(resp) - got, 0);
      if (n <= 0)
        {
          return -1;
        }

      got += n;
    }

  if ((resp[0] & 0xf0) != 0x20)           /* 不是 CONNACK */
    {
      return -1;
    }

  return resp[3];
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct in_addr ipaddr;
  struct in_addr gwaddr;
  struct timeval rcvtimeo;
  char broker_ip[32];
  uint32_t ms = 0;
  int net_layer_ok = 0;
  int mqtt_ok = 0;
  int fd = -1;
  int cur;
  int rc;
  int i;

  g_pass  = 0;
  g_total = 0;

  printf("\n");
  printf("========================================\n");
  printf("   板子网络连通性测试  (net_test)\n");
  printf("   USB RNDIS -> IP -> DNS -> TCP -> MQTT\n");
  printf("========================================\n\n");

  /* 1. 网卡配置 */

  printf("[1/4] 网卡 %s 配置\n", NETTEST_IFNAME);
  if (netlib_get_ipv4addr(NETTEST_IFNAME, &ipaddr) == OK &&
      netlib_get_dripv4addr(NETTEST_IFNAME, &gwaddr) == OK)
    {
      printf("      IP   = %s\n", inet_ntoa(ipaddr));
      printf("      网关 = %s\n", inet_ntoa(gwaddr));
      report("网卡已配置", 1, NULL);
    }
  else
    {
      report("网卡已配置", 0, "取不到 IP/网关");
      goto out;
    }

  /* 2. DNS 解析 */

  printf("\n[2/4] DNS 解析 %s\n", NETTEST_BROKER);
  if (resolve(NETTEST_BROKER, broker_ip, sizeof(broker_ip)) == 0)
    {
      printf("      %s -> %s\n", NETTEST_BROKER, broker_ip);
      report("DNS 解析成功", 1, NULL);
    }
  else
    {
      report("DNS 解析成功", 0, "解析失败，检查 DNS 服务器是否可达");
      goto out;
    }

  /* 3. TCP 连接 */

  printf("\n[3/4] TCP 连接 %s:%d\n", broker_ip, NETTEST_PORT);
  fd = tcp_connect_timeout(broker_ip, NETTEST_PORT, &ms);
  if (fd >= 0)
    {
      printf("      耗时 %u ms\n", (unsigned)ms);
      report("TCP 连接成功", 1, NULL);
      net_layer_ok = 1;
    }
  else
    {
      report("TCP 连接成功", 0, "连不上，检查网关/ICS 共享是否正常");
      goto out;
    }

  /* 4. MQTT 握手：依次试几个公共 broker，任意一个回 CONNACK 就算过 */

  printf("\n[4/4] MQTT CONNECT -> CONNACK\n");
  rcvtimeo.tv_sec  = NETTEST_TIMEOUT;
  rcvtimeo.tv_usec = 0;

  for (i = 0; i < NETTEST_NBROKERS && mqtt_ok == 0; i++)
    {
      if (i == 0)
        {
          /* 复用第 3 步已经连上的 socket */
          cur = fd;
          fd  = -1;
        }
      else
        {
          if (resolve(g_brokers[i], broker_ip, sizeof(broker_ip)) < 0)
            {
              printf("      %-22s DNS 解析失败\n", g_brokers[i]);
              continue;
            }

          cur = tcp_connect_timeout(broker_ip, NETTEST_PORT, NULL);
          if (cur < 0)
            {
              printf("      %-22s TCP 连接失败\n", g_brokers[i]);
              continue;
            }
        }

      setsockopt(cur, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
      rc = mqtt_handshake(cur, "sf32lb52_net_test");
      close(cur);

      if (rc == 0)
        {
          char detail[64];

          mqtt_ok = 1;
          snprintf(detail, sizeof(detail), "%s 回了 CONNACK", g_brokers[i]);
          printf("      %-22s OK\n", g_brokers[i]);
          report("MQTT 握手成功", 1, detail);
        }
      else
        {
          printf("      %-22s 没有 CONNACK (rc=%d)\n", g_brokers[i], rc);
        }
    }

  if (mqtt_ok == 0)
    {
      report("MQTT 握手成功", 0, "试过的公共 broker 都没回 CONNACK");
    }

out:
  if (fd >= 0)
    {
      close(fd);
    }

  printf("\n========================================\n");
  printf("   结果: %d/%d PASS", g_pass, g_total);
  if (g_pass == g_total)
    {
      printf("   >>> 板子网络正常 <<<\n");
    }
  else if (net_layer_ok)
    {
      printf("   >>> 板子网络正常(IP/DNS/TCP 全通); 只有 MQTT 没通,\n");
      printf("       公共 broker 限流或临时故障, 与板子无关 <<<\n");
    }
  else
    {
      printf("   >>> 网络不通，见上面 FAIL 项 <<<\n");
    }

  printf("========================================\n\n");

  return g_pass == g_total ? EXIT_SUCCESS : EXIT_FAILURE;
}
