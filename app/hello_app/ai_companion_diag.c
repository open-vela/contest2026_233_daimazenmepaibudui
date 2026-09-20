/**
 * ai_companion_diag.c - 把 hello_app 交来的状态行翻成一行 JSON
 *
 * 为什么单独一个文件（不直接写进 ai_companion_main.c）：
 *   robot_ui 的 network_comm.c 要调 ai_companion_diag_snapshot()，而 hello_app
 *   那些状态全是 ai_companion_main.c 的 static，那边只能再开一个内部门面
 *   （ai_companion_state_snapshot_impl()）。本文件是中间的翻译层：只认
 *   "key=value" 那一行文本，不 include 任何 hello_app / NuttX 的类型定义，
 *   所以它的逻辑在 PC 上就能编出来跑（报文长什么样不该等到上板子才发现）。
 * 契约（返回值、字段、缓冲小的时候怎么办）、为什么要它、三条纪律，
 * 全写在 ai_companion_diag.h 的头上，改这里之前先读那一段。
 *
 * 为什么自己拼 JSON 而不用 cJSON：hello_app 这个 app 的编译单元里没有 cJSON 的
 * 头文件路径（那是 robot_ui 的依赖），为一个两百来字节的对象多引一个库不划算；
 * 自己拼还有个好处 —— 长度完全可控、不走堆（诊断这条路上最不该出现"内存不足"）。
 */

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai_companion_diag.h"

/* ai_companion_state_snapshot_impl() 交来的那一行状态。
 * 尺寸按"所有键都写全、每个数字都到 int/uint 的位数、状态名取最长的
 * CARE_REMIND"留：2026-09-16 加上 rxi..rxl 那六个计数器之后是 316 字节，
 * 2026-09-21 删掉让路那五个键（hold/busy/req/wd/wdead）之后是 259 字节，
 * 离 384 有 120 多字节余量。
 * 以后字段再加，超了也只是尾巴被截断、那几个字段按"取不到"（-1）输出，
 * 不会写出半截 JSON（那一行本身也还是完整的 C 字符串）—— 所以 impl 那边
 * 把新字段一律接在最后，被切掉的先是最不关键的那些。 */
#define DIAG_SNAP_BUF   384

/* 能用的最小缓冲：连 "{}" 加结尾 '\0' 都放不下的，按"取不到"报，别写半个对象。
 * 真实调用方给的是 128（心跳那个探针）和 512（diag 报文），走不到这条。 */
#define DIAG_BUF_MIN    8

/* "收尾说明" ,"trunc":1 的字节数。字段那一轮会先把它扣出来（见下面 w.cap
 * 那两行），这样"真发生了截断就一定还写得下这句说明" —— 看报文的人才知道
 * 缺掉的字段是"没装下"而不是"值就是 0"。 */
#define DIAG_TRUNC_ITEM 10

/****************************************************************************
 * 解析 ai_companion_state_snapshot_impl() 交来的那一行
 ****************************************************************************/

/**
 * @brief  在 "key=value key=value ..." 里找 key，拿到它后面那段值的首地址和长度
 * @return true = 找着了、而且值不是空的；false = 没有这个键（或值空着）
 *
 * 键只在**词的边界**上比（每次都从上一个空格之后开始比），所以 "rec" 不会误命中
 * "ract"。截断留下的半截词（没有 '=' 的尾巴）自然被跳过 —— 那正是"这个键没写进来"，
 * 调用方会按 -1 输出。
 */

static bool diag_find(const char *snap, const char *key,
                      const char **val, size_t *vlen)
{
  size_t      klen = strlen(key);
  const char *p = snap;

  while (*p != '\0')
    {
      const char *sp;
      const char *v;

      if (strncmp(p, key, klen) == 0 && p[klen] == '=')
        {
          v  = p + klen + 1;
          sp = strchr(v, ' ');
          *val  = v;
          *vlen = (sp != NULL) ? (size_t)(sp - v) : strlen(v);
          return *vlen > 0;
        }

      sp = strchr(p, ' ');
      if (sp == NULL)
        {
          break;
        }

      p = sp + 1;
    }

  return false;
}

/**
 * @brief  取一个整数值；取不到返回 false（调用方按 -1 输出）
 *
 * 值都是短数字（最长的是 uint32 的 10 位），16 字节的栈上副本绰绰有余；
 * 超出这个长度说明那一行已经乱了，按取不到处理。
 */

static bool diag_get_int(const char *snap, const char *key, long *out)
{
  const char *v;
  size_t      vlen;
  char        tmp[16];
  char       *end = NULL;

  if (!diag_find(snap, key, &v, &vlen) || vlen >= sizeof(tmp))
    {
      return false;
    }

  memcpy(tmp, v, vlen);
  tmp[vlen] = '\0';

  *out = strtol(tmp, &end, 10);

  /* 整个词都得是数字（strtol 会把 "12abc" 读成 12，那种就算取不到）。 */

  return (end != NULL && end != tmp && *end == '\0');
}

/****************************************************************************
 * 拼 JSON 的小工具
 *
 * 规矩只有一条：**写不下就把这一项整个丢掉**（置 dropped），宁可少一个字段，
 * 也不留半截 JSON —— 调用方要 cJSON_Parse 它，半截的只能降级成字符串贴出来，
 * 看报文的人反而更难读。收尾时补一个 "trunc":1 说明"这份不完整"。
 ****************************************************************************/

typedef struct
{
  char  *buf;       /* 调用方的缓冲 */
  size_t cap;       /* buf 的字节数（含结尾 '\0' 占的那一格） */
  size_t used;      /* 已经写进去的字节数（不含结尾 '\0'） */
  bool   first;     /* 还没写过字段 —— 下一个字段前面不用打逗号 */
  bool   dropped;   /* 有字段没装下（收尾时补一个 trunc 说明） */
} diag_jw_t;

/* 追加一段原文；装不下返回 false、一个字节都不写。
 * 每次判据里都 +2：给"收尾大括号"和"结尾 '\0'"各留一格 —— 有这个不变量，
 * 收尾那个 '}' 在任何一条成功路径上都一定放得下。 */

static bool diag_jw_put(diag_jw_t *w, const char *s, size_t n)
{
  if (w->used + n + 2 > w->cap)
    {
      return false;
    }

  memcpy(w->buf + w->used, s, n);
  w->used += n;
  w->buf[w->used] = '\0';
  return true;
}

static void diag_jw_num(diag_jw_t *w, const char *key, long val)
{
  char item[40];
  int  n = snprintf(item, sizeof(item), "%s\"%s\":%ld",
                    w->first ? "" : ",", key, val);

  if (n <= 0 || (size_t)n >= sizeof(item) ||
      !diag_jw_put(w, item, (size_t)n))
    {
      w->dropped = true;
      return;
    }

  w->first = false;
}

/* 字符串字段。值只接受 [A-Za-z0-9_]（状态机名字正好是这种），别的字符一律写成
 * '?'：不为一个理论上可能出现的引号把转义那一套搬进来（真转义错了会写出非法
 * JSON，那比少一个字符严重得多）。 */

static void diag_jw_str(diag_jw_t *w, const char *key,
                        const char *val, size_t vlen)
{
  char   safe[24];
  char   item[48];
  size_t i = 0;
  int    n;

  while (i < vlen && i + 1 < sizeof(safe))
    {
      char c = val[i];

      safe[i] = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '_') ? c : '?';
      i++;
    }

  if (i == 0)
    {
      safe[i++] = '?';
    }

  safe[i] = '\0';

  n = snprintf(item, sizeof(item), "%s\"%s\":\"%s\"",
               w->first ? "" : ",", key, safe);

  if (n <= 0 || (size_t)n >= sizeof(item) ||
      !diag_jw_put(w, item, (size_t)n))
    {
      w->dropped = true;
      return;
    }

  w->first = false;
}

/****************************************************************************
 * 输出哪些字段、按什么顺序（键名和 ai_companion_diag.h 头上那张表一一对应）
 *
 * 表驱动的意义：impl 那边**没写进来的键在这里自动变成 -1**，不用给每个字段写
 * 一遍"取不到怎么办"的分支 —— vad / asr 现在就是靠这个恒为 -1 的
 * （ai_companion_main.c 里还没记这两个时间戳，等它记了这里一个字节都不用改）。
 ****************************************************************************/

static const char *const g_diag_keys[] =
{
  "audio", "start", "rec", "ract", "died", "exit",
  "idle", "lres", "wait", "empty",
  "want", "net", "nrt",
  "sm",                       /* 唯一的字符串字段：状态机名字 */
  "cap", "sp", "kws", "vad", "asr",
  /* 2026-09-16 加：驱动侧的录音分诊计数（完成 / 半满 / read 进入 / 等待超时 /
   * TE 错误 / 被 DMA 覆盖的帧数）。名字和顺序跟 ai_companion_diag.h 的字段表、
   * 以及 ai_companion_main.c 那一行的写法一致；impl 那边取不到（驱动还没起来）
   * 时不写这几个键，这里就自动输出 -1。 */
  "rxi", "rxh", "rxr", "rxt", "rxe", "rxl",
  /* 2026-09-20 加：主循环心跳的年龄（毫秒，-1 = 没有实例 / 位子空着）。
   * ⚠️ 它**只是仪表，不决定任何动作**（自动接管已删除，理由见 ai_companion_main.c
   * 的 g_beat_ms 那一段）：正常时是几十~几百 ms（主循环一拍 100ms），
   * 涨起来就是那一拍在处理一轮对话（ASR + 大模型 + TTS 都在这条线上、各自几十秒超时）、
   * 或者真的卡住了 —— 这两个用时间分不开，所以谁也不许拿它当判据。
   * 看护那边只用它的一件事：-1（位子空着）= 没有任何实例在跑 ⇒ 重新拉一个。 */
  "lbeat"
};

int ai_companion_diag_snapshot(char *buf, size_t len)
{
  char        snap[DIAG_SNAP_BUF];
  diag_jw_t   w;
  const char *val;
  size_t      vlen;
  long        v;
  size_t      i;

  /* buf 没给 / 小到连 "{}" 都放不下：没法写，按"取不到"报。 */

  if (buf == NULL || len < DIAG_BUF_MIN)
    {
      return -1;
    }

  buf[0] = '\0';                /* 任何一条路径上都是个合法（哪怕空）的 C 字符串 */

  /* 1) 问 hello_app 要那一行状态。纯读、不碰设备、立刻返回。 */

  ai_companion_state_snapshot_impl(snap, sizeof(snap));

  /* 2) hello_app 在不在？audio 是它初始化音频的标记（main() 里 audio_init()
   *    失败就直接返回了，所以没有"在跑但音频没起来"这种中间态），run 是它还没
   *    退场。两个都要满足，否则报负值 —— 调用方会把它原样说成 "unavailable"。
   *    键取不到（快照被截断）同样按"没在跑"处理：那是保守的方向，宁可说
   *    "取不到"，也不要凭空给它报一份健康。 */

  if (!diag_get_int(snap, "run", &v) || v != 1)
    {
      return -1;
    }

  if (!diag_get_int(snap, "audio", &v) || v != 1)
    {
      return -1;
    }

  /* 3) 拼 JSON。第一个字段固定是 alive（形状和 network_comm.h 里那个例子对上）。
   *    字段这一轮特意按 len 减去"收尾说明"来算容量（DIAG_TRUNC_ITEM）：
   *    只要真的发生截断，后面那句 trunc 就一定还有地方写（见第 4 步）。
   *    缓冲小到连这个余量都留不出来（比 DIAG_BUF_MIN+说明还小）时不预留 ——
   *    那种情况本来就只剩一个 "{}" 可写。 */

  w.buf = buf;
  w.cap = (len > DIAG_BUF_MIN + DIAG_TRUNC_ITEM) ? (len - DIAG_TRUNC_ITEM) : len;
  w.used = 0;
  w.first = true;
  w.dropped = false;

  if (!diag_jw_put(&w, "{", 1))
    {
      return -1;
    }

  diag_jw_num(&w, "alive", 1);

  for (i = 0; i < sizeof(g_diag_keys) / sizeof(g_diag_keys[0]); i++)
    {
      const char *key = g_diag_keys[i];

      if (strcmp(key, "sm") == 0)
        {
          if (diag_find(snap, key, &val, &vlen))
            {
              diag_jw_str(&w, key, val, vlen);
            }
          else
            {
              diag_jw_str(&w, key, "?", 1);
            }
        }
      else
        {
          diag_jw_num(&w, key, diag_get_int(snap, key, &v) ? v : -1);
        }
    }

  /* 4) 收尾说明。容量放回完整的 len：字段那一轮已经少算了 DIAG_TRUNC_ITEM 个
   *    字节，所以"dropped 为真时这句一定写得下"（每个字段写入都留了 2 格，
   *    推导见 diag_jw_put）。有字段没装下就明说 —— 少几个字段的 JSON 仍然是
   *    合法对象，内嵌之后看报文的人一眼知道"这份不完整"，不会把缺失当成 0。 */

  w.cap = len;

  if (w.dropped)
    {
      diag_jw_num(&w, "trunc", 1);
    }

  /* 5) 收尾。到这一步一定放得下：每个字段写入时都留了 2 格（'}' 和 '\0'，
   *    见 diag_jw_put），而装不下的时候它一个字节都不写。 */

  if (!diag_jw_put(&w, "}", 1))
    {
      return -1;
    }

  return (int)w.used;
}
