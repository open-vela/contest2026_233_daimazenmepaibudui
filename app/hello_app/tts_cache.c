/**
 * tts_cache.c - 固定文案的 TTS 预缓存（文件名规则 + ROMFS 查表）
 *
 * 文件名规则（归一化 → FNV-1a 32 → %08x.pcm）的完整说明、以及"PC 侧脚本
 * 为什么必须照抄这一套"都写在 tts_cache.h 头上；本文件是它在板端的实现。
 *
 * 本文件只碰文件、不碰音频设备、不加锁、不分配常驻内存：读一次就是一次 open +
 * 几十 KB 的 XIP read。相对"现调云端 TTS 一次阻塞 HTTPS"，这是它存在的理由。
 */

#include "tts_cache.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* FNV-1a 32 位的两个常数（和 PC 侧脚本里那两行必须一样） */

#define TTS_CACHE_FNV_OFFSET  2166136261u
#define TTS_CACHE_FNV_PRIME   16777619u

/* "<8 位哈希>.pcm" + '\0'（TTS_CACHE_NAME_MAX）再加上前缀目录那一段。
 * 用 sizeof(TTS_CACHE_ROMFS_DIR) 现算而不是写个常数：host 测试会把那个目录
 * 指到仓库里（绝对路径近百字节），写死 64 就会在这里撞 -ENOSPC。 */

#define TTS_CACHE_PATH_MAX    (sizeof(TTS_CACHE_ROMFS_DIR) + TTS_CACHE_NAME_MAX)

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * @brief  这个字节算不算"空白"（见 tts_cache.h 归一化第 a 条）
 *
 * 只认 6 个 ASCII 空白字节 + 全角空格 U+3000（那三字节在 walk 里单独判）。
 */

static bool tts_is_ascii_space(unsigned char c)
{
  return c == 0x20 || c == 0x09 || c == 0x0a ||
         c == 0x0b || c == 0x0c || c == 0x0d;
}

/**
 * @brief  归一化后逐字节交给 sink
 *
 * 走的是**流式**而不是"先落一整份归一化缓冲"：文案长度没有上限，而且哈希那条路
 * 只需要字节流，多一份缓冲纯属浪费（板子的堆要留给音频）。
 *
 * 规则逐条对应 tts_cache.h 那三条：
 *   - U+3000（E3 80 80）和 6 个 ASCII 空白字节都算空白；
 *   - 首尾空白丢掉：只有"已经出过正文"（have）之后的空白才立 pending；
 *   - 连续空白折成一个 0x20：pending 是个电平，只在下一个正文字节前补一次。
 *
 * 越界读：p[0] == 0xe3 时才可能看 p[1]/p[2]，而 p[1] == '\0' 会让 && 短路，
 * 所以永远读不到结尾 '\0' 之后（多字节序列在字符串中间被截断也正是这样处理的）。
 */

static void tts_normalize_walk(const char *text,
                               void (*sink)(void *ctx, unsigned char b),
                               void *ctx)
{
  const unsigned char *p = (const unsigned char *)text;
  bool have = false;        /* 已经出过正文字节 */
  bool pending = false;     /* 正文之间攒着一个空白 */

  if (p == NULL)
    {
      return;
    }

  while (*p != '\0')
    {
      if (p[0] == 0xe3 && p[1] == 0x80 && p[2] == 0x80)   /* U+3000 全角空格 */
        {
          if (have)
            {
              pending = true;
            }

          p += 3;
          continue;
        }

      if (tts_is_ascii_space(*p))
        {
          if (have)
            {
              pending = true;
            }

          p++;
          continue;
        }

      if (pending)
        {
          sink(ctx, 0x20);
          pending = false;
        }

      sink(ctx, *p);
      have = true;
      p++;
    }
}

/* --- 两个 sink：一个喂哈希，一个写进调用方的缓冲 --------------------------- */

struct tts_hash_sink_s
{
  uint32_t hash;
};

static void tts_hash_sink(void *ctx, unsigned char b)
{
  struct tts_hash_sink_s *s = (struct tts_hash_sink_s *)ctx;

  s->hash ^= (uint32_t)b;
  s->hash *= TTS_CACHE_FNV_PRIME;   /* 32 位无符号乘法天然就是 mod 2^32 */
}

struct tts_buf_sink_s
{
  char  *out;
  size_t cap;
  size_t wrote;      /* 实际写进去的（被 cap 截断后就停在这儿） */
  size_t emitted;    /* 逻辑上应该有的字节数（用于把"截断了"报出来） */
};

static void tts_buf_sink(void *ctx, unsigned char b)
{
  struct tts_buf_sink_s *s = (struct tts_buf_sink_s *)ctx;

  if (s->wrote < s->cap)
    {
      s->out[s->wrote++] = (char)b;
    }

  s->emitted++;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int tts_cache_normalize(const char *text, char *out, size_t out_len)
{
  struct tts_buf_sink_s s;

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  s.out = out;
  s.cap = out_len;
  s.wrote = 0;
  s.emitted = 0;

  tts_normalize_walk(text, tts_buf_sink, &s);

  /* 截断是"截在哪儿算哪儿"：调用方拿到的是放得下的部分，返回值仍是
   * 逻辑长度，于是它能自己判断有没有被截（本文案都很短，实际到不了）。 */

  return (int)s.emitted;
}

uint32_t tts_cache_hash(const char *text)
{
  struct tts_hash_sink_s s;

  s.hash = TTS_CACHE_FNV_OFFSET;

  tts_normalize_walk(text, tts_hash_sink, &s);

  return s.hash;
}

int tts_cache_name(const char *text, char *name, size_t name_len)
{
  int n;

  if (name == NULL)
    {
      return -EINVAL;
    }

  if (name_len < TTS_CACHE_NAME_MAX)
    {
      return -ENOSPC;
    }

  n = snprintf(name, name_len, "%08x.pcm", (unsigned)tts_cache_hash(text));
  if (n < 0 || (size_t)n >= name_len)
    {
      return -ENOSPC;
    }

  return 0;
}

int tts_cache_path(const char *text, char *path, size_t path_len)
{
  char name[TTS_CACHE_NAME_MAX];
  int n;
  int ret;

  if (path == NULL)
    {
      return -EINVAL;
    }

  ret = tts_cache_name(text, name, sizeof(name));
  if (ret < 0)
    {
      return ret;
    }

  n = snprintf(path, path_len, "%s/%s", TTS_CACHE_ROMFS_DIR, name);
  if (n < 0 || (size_t)n >= path_len)
    {
      return -ENOSPC;
    }

  return 0;
}

int tts_cache_load(const char *text, unsigned char **pcm_out, size_t *len_out)
{
  char path[TTS_CACHE_PATH_MAX];
  struct stat st;
  unsigned char *buf = NULL;
  size_t want;
  size_t got = 0;
  int fd;
  int ret;

  if (pcm_out == NULL || len_out == NULL)
    {
      return -EINVAL;
    }

  *pcm_out = NULL;
  *len_out = 0;

  if (text == NULL)
    {
      return -EINVAL;
    }

  ret = tts_cache_path(text, path, sizeof(path));
  if (ret < 0)
    {
      return ret;
    }

  fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
      /* 没有这个文件就是"没预生成"（或者忘了删 ROMFS 产物再编 / 没重新烧录）：
       * 调用方据此回落 live TTS。**这里不打日志** —— 回落的每一条路都自己会
       * 打一行说明为什么没走缓存，这里再打就是重复。 */
      return -ENOENT;
    }

  if (fstat(fd, &st) != 0 || st.st_size <= 0)
    {
      (void)close(fd);
      return -EIO;
    }

  /* 裸 s16le：长度必须是偶数，否则这一份素材本身就是坏的（生成脚本写坏了 /
   * 拷贝时被截断）。宁可当"没缓存"报错，也不要把半截采样点喂给播放层。 */

  if ((st.st_size & 1) != 0)
    {
      (void)close(fd);
      return -EIO;
    }

  want = (size_t)st.st_size;
  buf = (unsigned char *)malloc(want);
  if (buf == NULL)
    {
      (void)close(fd);
      return -ENOMEM;
    }

  /* ROMFS 是普通只读文件，read 正常返回；这里仍然按"循环读满"写，
   * 免得将来素材挪到别的文件系统上（比如 /data）时读不满半截。 */

  while (got < want)
    {
      ssize_t n = read(fd, buf + got, want - got);

      if (n < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          free(buf);
          (void)close(fd);
          return -EIO;
        }

      if (n == 0)
        {
          break;      /* 文件比 fstat 报的短：下面按"读不满"处理 */
        }

      got += (size_t)n;
    }

  (void)close(fd);

  if (got == 0 || (got & 1) != 0)
    {
      free(buf);
      return -EIO;
    }

  *pcm_out = buf;
  *len_out = got;

  return 0;
}
