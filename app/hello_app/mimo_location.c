/****************************************************************************
 * app/hello_app/mimo_location.c
 *
 * IP 定位：板子自己打一个免 key 的 HTTPS 接口，问出"现在大概在哪个城市"，
 * 结果缓存起来给 mimo_chat() 的 system prompt 用（见 mimo_voice.c 的
 * chat_build_system_prompt）。对外只有一个 mimo_location_get()。
 *
 * 用哪两个接口（都不用 key、都是 HTTPS，按顺序试）：
 *   ipapi.co/json/   ->  {"city":"Hangzhou","latitude":30.29,"longitude":120.16,...}
 *   ipwho.is/        ->  {"city":"Hangzhou","latitude":30.29,"longitude":120.16,...}
 * 两个回包的字段名一样（city / latitude / longitude），所以一套解析就够；
 * 第一个不通（被墙 / 限流 / 5xx）就试第二个 —— 除非第一个一次就拖过好几秒，
 * 那时判定"这个网络够不着"，不再拿第二个再赌一轮（见 MIMO_LOC_SLOW_SEC）。
 * 不用 ip-api.com 那类：免费档只有明文 http，本文件的 host 表里只放 HTTPS 的。
 *
 * 为什么自己写字符串查找、不引 cJSON：hello_app 的编译参数里没有 cJSON 的
 * include 路径（mimo_voice.c 里也是这个理由），而且这个回包就一两百字节、
 * 结构固定，找键取值足够。
 *
 * 缓存与退避：成功缓存 MIMO_LOC_TTL_SEC（6 小时，IP 定位不会分钟级变化），
 * 失败退避 MIMO_LOC_BACKOFF_SEC（5 分钟）。退避是必须的 —— 一次 TLS 失败
 * 最坏要等满 socket 的连接 / 读超时（默认 15 + 30 秒），不能让每一轮对话都
 * 等一遍；另外一次请求拖过 MIMO_LOC_SLOW_SEC 就判定"这个网络够不着"，不再
 * 拿第二个接口再赌一轮（见 loc_fetch）。所以最坏情况是：某一次对话多等十几
 * 秒，之后 5 分钟内不再为定位花任何时间。
 *
 * 并发：整次网络请求都放在一把互斥体里串行。vela_tls 的连接池是**全局**
 * 状态（池子清理会把别的线程在飞的连接一起清掉），两个线程同时打会互相踩；
 * 宁可让并发调用者等一下。实际调用方只有 mimo_chat()（语音工作线程）。
 *
 * 失败一律返回负值，调用方拿到负值就一个字都不往 prompt 里写：用户问
 * "我在哪"时宁可不答，也不要答错城市。日志只打城市 / 经纬度 / host，
 * 不涉及任何密钥。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "mimo_location.h"

#include "agent_compat.h"
#include "infra/config_store.h"
#include "infra/vela_tls.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 城市名缓冲（"Hangzhou" 这类最长也就十几字节，64 给足） */

#define MIMO_LOC_CITY_MAX 64

/* 定位接口的响应就一两百字节（ipapi.co 的 /json/ 约 400 字节），
 * 4 KB 足够；overflow 会被当成失败，不用留更大。 */

#define MIMO_LOC_RESP_CAP (4 * 1024)

/* 缓存有效期：6 小时。过期后下一次 mimo_chat() 会重新查一次（一次握手 +
 * 一次 GET，几百毫秒）。 */

#define MIMO_LOC_TTL_SEC (6 * 3600)

/* 失败退避：5 分钟。这期间不再打网络，直接返回 -ENOENT。 */

#define MIMO_LOC_BACKOFF_SEC (5 * 60)

/* 一次请求拖过这么多秒就算"这个网络够不着"，不再试下一个接口（见 loc_fetch
 * 的函数头）。取 5 秒：正常的 IP 定位接口 1~2 秒就回来了，5 秒还没回来基本
 * 就是连不上、在等 socket 的连接超时（默认 15 秒）。 */

#define MIMO_LOC_SLOW_SEC 5

/* 解析失败时最多往串口打多少字节（排错用，成功不打） */

#define MIMO_LOC_ERR_DUMP_LEN 200

/* 开关：配置键 enable_ip_location。**缺省算开**（这个功能是用户要的），
 * 配成 "0" / "false" / "off" 才关 —— 网络够不着这两个接口的话就关掉，
 * 一次网都不打。 */

#define MIMO_CFG_KEY_LOCATION "enable_ip_location"

#define MIMO_LOC_PORT "443"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char *TAG = "mimo_location";

/* 免 key 的 IP 定位接口，按顺序试。两个的字段名一样（city / latitude /
 * longitude）。路径末尾的斜杠不能省：服务端会对 // 或缺斜杠的路径做 301
 * 跳转，而我们这一层不跟跳转（vela_https_get 只看第一个响应的状态码）。 */

static const struct
{
  const char *host;
  const char *path;
} s_loc_hosts[] =
{
  { "ipapi.co", "/json/" },
  { "ipwho.is",  "/" },
};

#define MIMO_LOC_HOST_NUM (sizeof(s_loc_hosts) / sizeof(s_loc_hosts[0]))

/* 缓存（成功）和退避（失败）的时间戳 / 内容都在这把锁里读写，网络请求本身
 * 也在锁里做（见文件头的"并发"那段）。 */

static pthread_mutex_t s_loc_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_loc_valid = false;
static char s_loc_city[MIMO_LOC_CITY_MAX];
static double s_loc_lat = 0.0;
static double s_loc_lon = 0.0;
static time_t s_loc_ok_ts = 0;
static time_t s_loc_retry_ts = 0;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: loc_json_find_string
 *
 * Description:
 *   在 json 里找 "key" 对应的**字符串**值，返回值的起点（不复制、不反转义），
 *   *out_len 是引号之间的字节数；找不到、或者值的引号没配对（响应被 cap
 *   截断）时返回 NULL。
 *
 *   和 mimo_voice.c 里的 json_find_string() 是同一套做法，只是那个是
 *   static 的、跨不了文件，这里照抄一份最小的（见文件头：不引 cJSON）。
 *
 ****************************************************************************/

static const char *loc_json_find_string(const char *json, const char *key,
                                        size_t *out_len)
{
  char pat[64];
  const char *p;

  if (json == NULL || key == NULL || out_len == NULL)
    {
      return NULL;
    }

  snprintf(pat, sizeof(pat), "\"%s\"", key);

  p = json;
  while ((p = strstr(p, pat)) != NULL)
    {
      const char *q = p + strlen(pat);

      while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
        {
          q++;
        }

      if (*q != ':')
        {
          p = q;
          continue;
        }

      q++;

      while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
        {
          q++;
        }

      if (*q != '"')
        {
          p = q;
          continue;
        }

      q++;
      p = q;                      /* 值的起点（不含开引号） */

      while (*q != '\0' && *q != '"')
        {
          if (*q == '\\' && q[1] != '\0')
            {
              q += 2;                 /* 转义序列整对跳过 */
            }
          else
            {
              q++;
            }
        }

      if (*q != '"')
        {
          return NULL;                /* 引号没配对：响应被截断了 */
        }

      *out_len = (size_t)(q - p);
      return p;
    }

  return NULL;
}

/****************************************************************************
 * Name: loc_hex4 / loc_utf8_put / loc_json_unescape
 *
 * Description:
 *   把 JSON 字符串里的转义还原成 UTF-8：\\ \" \/ \b \f \n \r \t 和 \uXXXX。
 *   城市名一般是纯 ASCII（Hangzhou），但 ipapi.co 之类对非 ASCII 会用
 *   \uXXXX，所以这一个得还原，否则 prompt 里会出现字面的 \u676d。
 *   不处理代理对（U+D800~U+DFFF 成对出现的那种）：城市名里不会有。
 *
 ****************************************************************************/

static int loc_hex4(const char *p, unsigned int *out)
{
  unsigned int v = 0;
  int i;

  for (i = 0; i < 4; i++)
    {
      char c = p[i];

      v <<= 4;

      if (c >= '0' && c <= '9')
        {
          v |= (unsigned int)(c - '0');
        }
      else if (c >= 'a' && c <= 'f')
        {
          v |= (unsigned int)(c - 'a' + 10);
        }
      else if (c >= 'A' && c <= 'F')
        {
          v |= (unsigned int)(c - 'A' + 10);
        }
      else
        {
          return -1;
        }
    }

  *out = v;
  return 0;
}

static size_t loc_utf8_put(unsigned int cp, char *dst)
{
  if (cp < 0x80)
    {
      dst[0] = (char)cp;
      return 1;
    }

  if (cp < 0x800)
    {
      dst[0] = (char)(0xc0 | (cp >> 6));
      dst[1] = (char)(0x80 | (cp & 0x3f));
      return 2;
    }

  if (cp < 0x10000)
    {
      dst[0] = (char)(0xe0 | (cp >> 12));
      dst[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
      dst[2] = (char)(0x80 | (cp & 0x3f));
      return 3;
    }

  dst[0] = (char)(0xf0 | (cp >> 18));
  dst[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
  dst[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
  dst[3] = (char)(0x80 | (cp & 0x3f));
  return 4;
}

static size_t loc_json_unescape(const char *src, size_t len, char *dst,
                                size_t dst_cap)
{
  size_t i = 0;
  size_t o = 0;

  if (src == NULL || dst == NULL || dst_cap == 0)
    {
      return 0;
    }

  while (i < len && o + 1 < dst_cap)
    {
      char c = src[i];

      if (c != '\\')
        {
          dst[o++] = c;
          i++;
          continue;
        }

      i++;
      if (i >= len)
        {
          break;                      /* 末尾一个孤零零的反斜杠：丢掉 */
        }

      c = src[i++];

      switch (c)
        {
          case 'n':  dst[o++] = '\n'; break;
          case 't':  dst[o++] = '\t'; break;
          case 'r':  dst[o++] = '\r'; break;
          case 'b':  dst[o++] = '\b'; break;
          case 'f':  dst[o++] = '\f'; break;
          case '"':  dst[o++] = '"';  break;
          case '\\': dst[o++] = '\\'; break;
          case '/':  dst[o++] = '/';  break;

          case 'u':
            {
              unsigned int cp = 0;

              if (i + 4 > len || loc_hex4(src + i, &cp) != 0)
                {
                  break;              /* 坏的 \u：整段丢掉，别写脏字节 */
                }

              i += 4;

              if (o + 4 > dst_cap - 1)
                {
                  i = len;            /* 装不下了，收工 */
                  break;
                }

              o += loc_utf8_put(cp, dst + o);
            }
            break;

          default:
            dst[o++] = c;             /* 不认识的转义：原样留下那个字符 */
            break;
        }
    }

  dst[o] = '\0';
  return o;
}

/****************************************************************************
 * Name: loc_json_find_number
 *
 * Description:
 *   在 json 里找 "key" 对应的**数字**值，写进 *out。只认裸数字（可以有负号
 *   和小数点），值是字符串（带引号）时不当数字 —— 免得把 "latitude":"30"
 *   这种当数值用。找不到返回 -ENOENT。
 *
 ****************************************************************************/

static int loc_json_find_number(const char *json, const char *key, double *out)
{
  char pat[64];
  const char *p;
  char tmp[32];
  size_t n;
  char *end = NULL;
  double v;

  if (json == NULL || key == NULL || out == NULL)
    {
      return -EINVAL;
    }

  snprintf(pat, sizeof(pat), "\"%s\"", key);

  p = json;
  while ((p = strstr(p, pat)) != NULL)
    {
      const char *q = p + strlen(pat);

      while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
        {
          q++;
        }

      if (*q != ':')
        {
          p = q;
          continue;
        }

      q++;

      while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
        {
          q++;
        }

      n = 0;
      while (n + 1 < sizeof(tmp)
             && ((*q >= '0' && *q <= '9') || *q == '-' || *q == '+'
                 || *q == '.' || *q == 'e' || *q == 'E'))
        {
          tmp[n++] = *q++;
        }

      if (n == 0)
        {
          p = q;
          continue;
        }

      tmp[n] = '\0';
      v = strtod(tmp, &end);
      if (end == tmp)
        {
          p = q;
          continue;
        }

      *out = v;
      return OK;
    }

  return -ENOENT;
}

/****************************************************************************
 * Name: loc_flag_off
 *
 * Description:
 *   读一个"缺省算开"的开关：配置键读不到、或者值不是 0 / false / off
 *   （大小写不敏感）就算开。claw_config_get() 失败时不动缓冲区，所以先自己
 *   置空。
 *
 ****************************************************************************/

static bool loc_flag_off(const char *key)
{
  char val[16];
  const char *p;
  size_t i;

  val[0] = '\0';

  if (claw_config_get(key, val, sizeof(val)) != OK)
    {
      return false;                   /* 键缺省 = 开 */
    }

  /* 整个值折成小写（只处理 ASCII 字母，和 mimo_voice.c 的
   * chat_config_flag_on 一个做法），再跳过前面的空白比那三个"关"值 */

  for (i = 0; val[i] != '\0'; i++)
    {
      if (val[i] >= 'A' && val[i] <= 'Z')
        {
          val[i] = (char)(val[i] - 'A' + 'a');
        }
    }

  p = val;
  while (*p == ' ' || *p == '\t')
    {
      p++;
    }

  return strcmp(p, "0") == 0 || strcmp(p, "false") == 0
         || strcmp(p, "off") == 0;
}

/****************************************************************************
 * Name: loc_fetch
 *
 * Description:
 *   真去打网络：按 s_loc_hosts 的顺序试，第一个能解析出城市 + 经纬度的就
 *   算成功。每次请求前清一次 vela_tls 的连接池，理由和 mimo_voice.c 的
 *   mimo_post() / weather_http_get() 一样 —— 池子复用死连接时排空只用 10ms
 *   超时、不重连，踩上要等满 socket 读超时（默认 30 秒）。
 *
 *   请求本身没法定超时（vela_https_get 的超时是 socket 级的全局配置），所以
 *   这里自己收一道"最坏时间"：一次 TLS 连接拖过 MIMO_LOC_SLOW_SEC 秒，就当
 *   这个网络够不着这两个接口，**不再拿第二个接口再赌一次**（否则最坏要连着
 *   等两轮连接超时，用户那边就是"AI 一直在转圈"）。接口能通但回包没用（限流
 *   / 字段变了）不算慢失败，照常试下一个 —— 那种情况是毫秒级返回。
 *
 * Returned Value:
 *   OK 时 city / lat / lon 都已经填好；<0 时 city 是空串。
 *
 ****************************************************************************/

static int loc_fetch(char *city, size_t cap, double *lat, double *lon)
{
  char *resp;
  size_t h;
  int last = -EIO;

  resp = calloc(1, MIMO_LOC_RESP_CAP);
  if (resp == NULL)
    {
      syslog(LOG_ERR, "[%s] 响应缓冲 %d 字节分配失败\n", TAG,
             (int)MIMO_LOC_RESP_CAP);
      return -ENOMEM;
    }

  for (h = 0; h < MIMO_LOC_HOST_NUM; h++)
    {
      char name[MIMO_LOC_CITY_MAX];
      const char *val;
      time_t t0;
      bool slow;
      size_t n = 0;
      double la = 0.0;
      double lo = 0.0;
      int status;

      resp[0] = '\0';
      name[0] = '\0';

      vela_tls_pool_cleanup();

      t0 = time(NULL);

      status = vela_https_get(s_loc_hosts[h].host, MIMO_LOC_PORT,
                              s_loc_hosts[h].path, resp, MIMO_LOC_RESP_CAP);

      if (status != 200)
        {
          last = (status < 0) ? -EIO : -ENOENT;

          /* 拖过一次长的（连接超时那类）就收手：见函数头"最坏时间"那段 */

          slow = (time(NULL) - t0) >= MIMO_LOC_SLOW_SEC;

          syslog(LOG_WARNING, "[%s] %s%s 返回 %d%s\n", TAG,
                 s_loc_hosts[h].host, s_loc_hosts[h].path, status,
                 (slow || h + 1 >= MIMO_LOC_HOST_NUM)
                     ? "，不试别的接口了" : "，换下一个接口试");

          if (slow || h + 1 >= MIMO_LOC_HOST_NUM)
            {
              break;
            }

          continue;
        }

      val = loc_json_find_string(resp, "city", &n);
      if (val != NULL && n > 0)
        {
          n = loc_json_unescape(val, n, name, sizeof(name) - 1);
          name[n] = '\0';
        }

      if (name[0] != '\0'
          && loc_json_find_number(resp, "latitude", &la) == OK
          && loc_json_find_number(resp, "longitude", &lo) == OK
          && la >= -90.0 && la <= 90.0 && lo >= -180.0 && lo <= 180.0)
        {
          snprintf(city, cap, "%s", name);

          if (lat != NULL)
            {
              *lat = la;
            }

          if (lon != NULL)
            {
              *lon = lo;
            }

          syslog(LOG_INFO, "[%s] IP 定位: %s lat=%.4f lon=%.4f（%s）\n", TAG,
                 city, la, lo, s_loc_hosts[h].host);
          free(resp);
          return OK;
        }

      /* 通了但没用：限流 / 错包 / 字段改了。把前面一段打出来排错，然后试
       * 下一个接口（这一条不是网络失败，所以不算 -EIO）。 */

      syslog(LOG_WARNING, "[%s] %s 的回包里没有能用的城市 / 经纬度: %.*s\n",
             TAG, s_loc_hosts[h].host, MIMO_LOC_ERR_DUMP_LEN,
             (resp[0] != '\0') ? resp : "(空)");
      last = -ENOENT;
    }

  free(resp);
  return last;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int mimo_location_get(char *city, size_t cap, double *lat, double *lon)
{
  time_t now;
  int ret;

  if (city == NULL || cap == 0)
    {
      return -EINVAL;
    }

  city[0] = '\0';

  if (lat != NULL)
    {
      *lat = 0.0;
    }

  if (lon != NULL)
    {
      *lon = 0.0;
    }

  /* 开关关掉的话一次网都不打（见文件头：网络够不着这两个接口的人可以关） */

  if (loc_flag_off(MIMO_CFG_KEY_LOCATION))
    {
      return -ENOENT;
    }

  pthread_mutex_lock(&s_loc_lock);

  now = time(NULL);

  if (s_loc_valid && (now - s_loc_ok_ts) < MIMO_LOC_TTL_SEC)
    {
      snprintf(city, cap, "%s", s_loc_city);

      if (lat != NULL)
        {
          *lat = s_loc_lat;
        }

      if (lon != NULL)
        {
          *lon = s_loc_lon;
        }

      pthread_mutex_unlock(&s_loc_lock);
      return OK;
    }

  if (now < s_loc_retry_ts)
    {
      /* 上一次失败，还在退避期：直接说不成功，不打网络 */
      pthread_mutex_unlock(&s_loc_lock);
      return -ENOENT;
    }

  ret = loc_fetch(city, cap, lat, lon);

  if (ret == OK)
    {
      snprintf(s_loc_city, sizeof(s_loc_city), "%s", city);
      s_loc_lat = (lat != NULL) ? *lat : 0.0;
      s_loc_lon = (lon != NULL) ? *lon : 0.0;
      s_loc_valid = true;
      s_loc_ok_ts = now;
      s_loc_retry_ts = 0;
    }
  else
    {
      s_loc_retry_ts = now + MIMO_LOC_BACKOFF_SEC;

      city[0] = '\0';

      if (lat != NULL)
        {
          *lat = 0.0;
        }

      if (lon != NULL)
        {
          *lon = 0.0;
        }

      syslog(LOG_WARNING, "[%s] IP 定位没查到 %d，%d 秒内不再试\n", TAG, ret,
             MIMO_LOC_BACKOFF_SEC);
    }

  pthread_mutex_unlock(&s_loc_lock);
  return ret;
}
