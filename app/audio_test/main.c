/****************************************************************************
 * app/audio_test/main.c
 *
 * SF32LB52 /dev/audio/audio0 音频通路测试 + 验收测试
 *
 * 用法:
 *   audio_test                          播放 1kHz 1 秒
 *   audio_test <ms> [freq]              播放正弦
 *   audio_test record [ms] [file]       录音（给 file 则存成裸 PCM: 16k mono s16le）
 *   audio_test playfile <file>          回放裸 PCM 文件
 *   audio_test loop [n]                 连续 open/config/start/stop/close n 次
 *   audio_test stopwait [ms]            录音 read 阻塞中从另一个任务发 STOP，看能否退出
 *
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/audio/audio.h>
#include <nuttx/sched.h>

#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define AUDIO_DEV      "/dev/audio/audio0"
#define SAMPLE_RATE    16000
#define CHANNELS       1
#define BITS           16

/* stopwait 用：录音任务的状态 */
static volatile int     g_rd_done;
static volatile ssize_t g_rd_n;
static int              g_rd_fd;
static FAR int16_t     *g_rd_buf;
static int              g_rd_len;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int gen_sine(FAR int16_t *buf, int nsamples, int freq)
{
  int i;

  for (i = 0; i < nsamples; i++)
    {
      /* 幅度 0.2（约 -14dBFS）：原来 0.5 太吵，测试时刺耳 */
      buf[i] = (int16_t)(32767.0 * 0.2 *
               sin(2.0 * M_PI * freq * (double)i / SAMPLE_RATE));
    }

  return nsamples;
}

static void print_caps(int fd)
{
  struct audio_caps_s caps;
  int ret;

  memset(&caps, 0, sizeof(caps));
  caps.ac_len     = sizeof(caps);
  caps.ac_type    = AUDIO_TYPE_QUERY;
  caps.ac_subtype = AUDIO_TYPE_QUERY;

  ret = ioctl(fd, AUDIOIOC_GETCAPS, (unsigned long)&caps);
  printf("GETCAPS ret=%d type=0x%02x fmt=0x%08lx\n",
         ret, caps.ac_controls.b[0],
         (unsigned long)caps.ac_format.hw);
}

static int configure_path(int fd, bool record)
{
  struct audio_caps_desc_s capdesc;
  int ret;

  memset(&capdesc, 0, sizeof(capdesc));
  capdesc.caps.ac_len      = sizeof(struct audio_caps_s);
  capdesc.caps.ac_type     = record ? AUDIO_TYPE_INPUT : AUDIO_TYPE_OUTPUT;
  capdesc.caps.ac_channels = CHANNELS;
  capdesc.caps.ac_controls.hw[0] = SAMPLE_RATE;
  capdesc.caps.ac_controls.b[2]  = BITS;

  ret = ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&capdesc);
  printf("CONFIGURE ret=%d (%dHz %dch %dbit%s)\n",
         ret, SAMPLE_RATE, CHANNELS, BITS, record ? " input" : "");
  return ret;
}

/* 打开 -> 配置 -> START，成功返回 0 */
static int start_path(bool record, FAR int *fdp)
{
  int fd;
  int ret;

  fd = open(AUDIO_DEV, record ? O_RDONLY : O_WRONLY);
  if (fd < 0)
    {
      printf("open %s failed: %d\n", AUDIO_DEV, fd);
      return -1;
    }

  print_caps(fd);

  ret = configure_path(fd, record);
  if (ret < 0)
    {
      close(fd);
      return -1;
    }

  ret = ioctl(fd, AUDIOIOC_START, 0);
  printf("START ret=%d\n", ret);
  if (ret < 0)
    {
      close(fd);
      return -1;
    }

  *fdp = fd;
  return 0;
}

static void stop_path(int fd)
{
  ioctl(fd, AUDIOIOC_STOP, 0);
  printf("STOP done\n");
  close(fd);
}

/* 统计 peak/avg，判断是否真的有声音 */
static void report_level(FAR const int16_t *buf, int nsamples)
{
  int i;
  int peak = 0;
  long sum = 0;

  for (i = 0; i < nsamples; i++)
    {
      int v = buf[i];

      if (v < 0)
        {
          v = -v;
        }

      if (v > peak)
        {
          peak = v;
        }

      sum += v;
    }

  printf("RECORD peak=%d avg=%ld (16k mono 16bit)\n",
         peak, nsamples > 0 ? sum / nsamples : 0);
  printf("RECORD %s\n", peak > 500 ? "OK: 检测到声音" :
         "静音: 麦克风无信号或通路未通");
}

/* 录音任务：read 会一直阻塞到读满 */
static int record_reader(int argc, FAR char *argv[])
{
  g_rd_n    = read(g_rd_fd, g_rd_buf, g_rd_len);
  g_rd_done = 1;
  return 0;
}

/****************************************************************************
 * 验收测试 1：录 N 毫秒 -> 存文件 -> 回放
 ****************************************************************************/

static int do_record(int duration_ms, FAR const char *file)
{
  int nsamples = SAMPLE_RATE * duration_ms / 1000;
  FAR int16_t *buf;
  int fd;
  ssize_t n;

  printf("audio_test: record %dms%s%s\n", duration_ms,
         file ? " -> " : "", file ? file : "");

  buf = (FAR int16_t *)malloc(nsamples * sizeof(int16_t));
  if (buf == NULL)
    {
      printf("malloc failed\n");
      return 1;
    }

  if (start_path(true, &fd) < 0)
    {
      free(buf);
      return 1;
    }

  n = read(fd, buf, nsamples * sizeof(int16_t));
  printf("READ done: %zd of %d bytes\n", n, nsamples * 2);
  report_level(buf, nsamples);
  stop_path(fd);

  if (file != NULL && n > 0)
    {
      FILE *fp = fopen(file, "wb");

      if (fp == NULL)
        {
          printf("open %s for write failed\n", file);
        }
      else
        {
          size_t w = fwrite(buf, 1, n, fp);

          fclose(fp);
          printf("SAVED %zu bytes -> %s\n", w, file);
        }
    }

  free(buf);
  return 0;
}

/****************************************************************************
 * 验收测试：回放裸 PCM 文件
 ****************************************************************************/

static int do_playfile(FAR const char *file)
{
  FAR int16_t *buf;
  long size;
  int nsamples;
  int fd;
  ssize_t n;
  FILE *fp;

  fp = fopen(file, "rb");
  if (fp == NULL)
    {
      printf("open %s failed\n", file);
      return 1;
    }

  fseek(fp, 0, SEEK_END);
  size = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  buf = (FAR int16_t *)malloc(size);
  if (buf == NULL)
    {
      printf("malloc %ld failed\n", size);
      fclose(fp);
      return 1;
    }

  n = fread(buf, 1, size, fp);
  fclose(fp);
  nsamples = n / 2;

  printf("audio_test: playfile %s (%ld bytes, %d ms)\n",
         file, (long)n, nsamples * 1000 / SAMPLE_RATE);

  if (start_path(false, &fd) < 0)
    {
      free(buf);
      return 1;
    }

  n = write(fd, buf, nsamples * 2);
  printf("WRITE done: %zd of %d bytes\n", n, nsamples * 2);
  stop_path(fd);

  free(buf);
  return 0;
}

/****************************************************************************
 * 验收测试 2：连续启动/停止 n 次，验证还能再次打开
 ****************************************************************************/

static int do_loop(int count)
{
  int i;
  int ok = 0;
  int nsamples = SAMPLE_RATE * 200 / 1000;   /* 每次 200ms */
  FAR int16_t *buf;
  int fd;
  ssize_t n;

  printf("audio_test: loop %d times (200ms each)\n", count);

  buf = (FAR int16_t *)malloc(nsamples * sizeof(int16_t));
  if (buf == NULL)
    {
      printf("malloc failed\n");
      return 1;
    }

  gen_sine(buf, nsamples, 1000);

  for (i = 1; i <= count; i++)
    {
      printf("--- round %d/%d ---\n", i, count);

      if (start_path(false, &fd) < 0)
        {
          printf("ROUND %d FAILED: 打不开了\n", i);
          break;
        }

      n = write(fd, buf, nsamples * 2);
      printf("WRITE done: %zd of %d bytes\n", n, nsamples * 2);
      stop_path(fd);
      ok++;
    }

  printf("LOOP RESULT: %d/%d 轮成功\n", ok, count);
  free(buf);
  return ok == count ? 0 : 1;
}

/****************************************************************************
 * 验收测试 3：录音 read 阻塞中发 STOP，看线程能否退出
 ****************************************************************************/

static int do_stopwait(int wait_ms)
{
  int nsamples = SAMPLE_RATE * 3;       /* 要读 3 秒，故意读不满 */
  FAR int16_t *buf;
  int fd;
  int i;

  printf("audio_test: stopwait %dms (read 目标是 3 秒，中途发 STOP)\n",
         wait_ms);

  buf = (FAR int16_t *)malloc(nsamples * sizeof(int16_t));
  if (buf == NULL)
    {
      printf("malloc failed\n");
      return 1;
    }

  memset(buf, 0, nsamples * sizeof(int16_t));

  if (start_path(true, &fd) < 0)
    {
      free(buf);
      return 1;
    }

  g_rd_fd   = fd;
  g_rd_buf  = buf;
  g_rd_len  = nsamples * 2;
  g_rd_done = 0;
  g_rd_n    = -999;

  if (task_create("aud_rd", 100, 4096, record_reader, NULL) < 0)
    {
      printf("task_create failed\n");
      free(buf);
      stop_path(fd);
      return 1;
    }

  usleep(wait_ms * 1000);

  printf("read 还在阻塞中，现在发 STOP ...\n");
  ioctl(fd, AUDIOIOC_STOP, 0);
  printf("STOP 已发出\n");

  for (i = 0; i < 30 && !g_rd_done; i++)
    {
      usleep(100 * 1000);
    }

  if (g_rd_done)
    {
      printf("STOPWAIT OK: read 返回了 (n=%zd)，线程能退出\n", g_rd_n);
    }
  else
    {
      printf("STOPWAIT FAIL: STOP 之后 3 秒 read 仍未返回，线程卡住\n");
    }

  close(fd);
  free(buf);
  return g_rd_done ? 0 : 1;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  /* 验收测试模式 */
  if (argc > 1 && strcmp(argv[1], "record") == 0)
    {
      int        ms   = (argc > 2) ? atoi(argv[2]) : 1000;
      FAR const char *f = (argc > 3) ? argv[3] : NULL;

      return do_record(ms, f);
    }

  if (argc > 1 && strcmp(argv[1], "playfile") == 0)
    {
      if (argc < 3)
        {
          printf("用法: audio_test playfile <file>\n");
          return 1;
        }

      return do_playfile(argv[2]);
    }

  if (argc > 1 && strcmp(argv[1], "loop") == 0)
    {
      return do_loop((argc > 2) ? atoi(argv[2]) : 5);
    }

  if (argc > 1 && strcmp(argv[1], "stopwait") == 0)
    {
      return do_stopwait((argc > 2) ? atoi(argv[2]) : 1000);
    }

  /* 标准音量接口：AUDIOIOC_SETPARAMTER + AUDIO_TYPE_FEATURE + AUDIO_FU_VOLUME
   * 用法: audio_test vol <0..1000>     (0=-36dB 最小, 1000=+6dB 最大)
   * 播放时改 DAC 音量；录音时改麦克风数字增益。 */

  if (argc > 1 && strcmp(argv[1], "vol") == 0)
    {
      struct audio_caps_desc_s capdesc;
      int vol  = (argc > 2) ? atoi(argv[2]) : 500;
      int vret;
      int fd  = open(AUDIO_DEV, O_WRONLY);

      if (fd < 0)
        {
          printf("open %s failed: %d\n", AUDIO_DEV, fd);
          return 1;
        }

      memset(&capdesc, 0, sizeof(capdesc));
      capdesc.caps.ac_len          = sizeof(struct audio_caps_s);
      capdesc.caps.ac_type         = AUDIO_TYPE_FEATURE;
      capdesc.caps.ac_format.hw    = AUDIO_FU_VOLUME;
      capdesc.caps.ac_controls.hw[0] = (uint16_t)vol;

      vret = ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&capdesc);
      printf("SETVOLUME %d/1000 ret=%d\n", vol, vret);
      close(fd);
      return 0;
    }

  /* 原来两种模式：播放正弦 */
  {
    int duration_ms = 1000;
    int freq        = 1000;
    int nsamples;
    FAR int16_t *buf;
    int fd;
    ssize_t n;

    if (argc > 1)
      {
        duration_ms = atoi(argv[1]);
      }

    if (argc > 2)
      {
        freq = atoi(argv[2]);
      }

    nsamples = SAMPLE_RATE * duration_ms / 1000;

    printf("audio_test: play %dHz %dms via %s\n", freq, duration_ms,
           AUDIO_DEV);

    buf = (FAR int16_t *)malloc(nsamples * sizeof(int16_t));
    if (buf == NULL)
      {
        printf("malloc failed\n");
        return 1;
      }

    gen_sine(buf, nsamples, freq);

    if (start_path(false, &fd) < 0)
      {
        free(buf);
        return 1;
      }

    n = write(fd, buf, nsamples * sizeof(int16_t));
    printf("WRITE done: %zd of %d bytes\n", n, nsamples * 2);
    stop_path(fd);

    free(buf);
  }

  printf("audio_test: done\n");
  return 0;
}
