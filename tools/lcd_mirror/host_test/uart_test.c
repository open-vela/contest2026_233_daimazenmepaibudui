/* 串口(UART)传输的主机侧冒烟测试（不需要板子、不需要网络）。

   做法：把 UART 的"设备节点"指成一个**普通文件** —— 板级代码走的是
   open(O_WRONLY|O_NONBLOCK) + write()，这在普通文件上和串口一样是字节流，
   所以写出来的文件就是 PC 端会从串口收到的那条字节流，可以直接喂给
   check_frames.py 做帧级/像素级校验。

   验四件事：
     1. 切传输 -> 打开节点 -> 帧真的出来了（同一份 lcd_mirror.c 的串口分支）；
     2. 策略①：中间**不喂任何脏区**，3 秒后文件还在长（整屏关键帧在发）；
     3. 帧内容逐像素对（交给 check_frames.py 比）；
     4. lcd_mirror_inject_touch()（串口模式的触摸入口）能进到 get_touch，
        含"按下锁存一次"和"抬手"。

   注意：普通文件不会返回 EAGAIN/短写，所以策略②（写不动就地重发）在主机上
   跑不出来，那条得真串口（TX 缓冲 1024 字节）才碰得到。

   停镜像之后文件就不长了，但**末尾可能停在一帧中间**（停的时候正好在发）。
   run_uart.sh 会把尾部那半截帧剪掉，再交给 check_frames.py。

   跑法（在 WSL 里）：
       bash /mnt/d/apply/claw/_lcd_mirror_host_test/run_uart.sh
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>

#include <nuttx/clock.h>

#include "lcd_mirror.h"

static long file_size(const char *path)
{
  struct stat st;

  if (stat(path, &st) != 0)
    {
      return -1;
    }

  return (long)st.st_size;
}

static uint16_t expect565(int x, int y)
{
  unsigned r = (unsigned)((x * 31) / (LCD_MIRROR_PANEL_W - 1));
  unsigned g = (unsigned)((y * 63) / (LCD_MIRROR_PANEL_H - 1));
  unsigned b = (unsigned)((((x * 7) + (y * 13)) & 0x7ff) * 31 / 2047);

  return (uint16_t)((r << 11) | (g << 5) | b);
}

static void fill_rect(uint8_t *buf, int y, int h, int stride)
{
  int row;
  int col;

  for (row = 0; row < h; row++)
    {
      for (col = 0; col < LCD_MIRROR_PANEL_W; col++)
        {
          uint16_t v = expect565(col, y + row);

          buf[row * stride + col * 2 + 0] = (uint8_t)(v & 0xff);
          buf[row * stride + col * 2 + 1] = (uint8_t)(v >> 8);
        }
    }
}

/* 取当前触摸状态，并断言它等于期望值 */
static int check_touch(uint16_t wx, uint16_t wy, bool wp, const char *what)
{
  uint16_t x = 0;
  uint16_t y = 0;
  bool     p = false;

  if (!lcd_mirror_get_touch(&x, &y, &p))
    {
      printf("FAIL [%s]: get_touch 返回 false（没在跑 / 没收到过触摸）\n", what);
      return 1;
    }

  if (x != wx || y != wy || p != wp)
    {
      printf("FAIL [%s]: 收到 (%u, %u, %d)，期望 (%u, %u, %d)\n", what,
             (unsigned)x, (unsigned)y, p ? 1 : 0, (unsigned)wx, (unsigned)wy,
             wp ? 1 : 0);
      return 1;
    }

  printf("ok   [%s]: (%u, %u, %d)\n", what, (unsigned)x, (unsigned)y,
         p ? 1 : 0);
  return 0;
}

int main(int argc, char **argv)
{
  uint8_t *band;
  long     s_feed;
  long     s_idle;
  int      fails = 0;
  int      y;

  if (argc < 2)
    {
      fprintf(stderr, "用法: %s <落盘文件路径（要已存在）>\n", argv[0]);
      return 2;
    }

  band = malloc((size_t)LCD_MIRROR_ROW_BYTES * 60);
  if (band == NULL)
    {
      fprintf(stderr, "malloc failed\n");
      return 2;
    }

  printf("=== 串口传输冒烟：节点 = %s ===\n", argv[1]);

  /* ---- 1) 切到串口传输并起任务 ---- */

  if (lcd_mirror_use_uart(argv[1]) != 0)
    {
      printf("FAIL: lcd_mirror_use_uart 失败\n");
      return 1;
    }

  printf("is_uart=%d, uart_dev=%s\n", (int)lcd_mirror_is_uart(),
         lcd_mirror_uart_dev());

  if (!lcd_mirror_is_uart() || strcmp(lcd_mirror_uart_dev(), argv[1]) != 0)
    {
      printf("FAIL: 传输没切过去\n");
      return 1;
    }

  if (lcd_mirror_start() < 0 || !lcd_mirror_is_running())
    {
      printf("FAIL: 镜像起不来\n");
      return 1;
    }

  usleep(300 * 1000);

  /* ---- 2) 喂一整屏期望画面（分 60 行一条带，照 LVGL 的刷法） ---- */

  for (y = 0; y < LCD_MIRROR_PANEL_H; y += 60)
    {
      int h = LCD_MIRROR_PANEL_H - y;

      if (h > 60)
        {
          h = 60;
        }

      fill_rect(band, y, h, LCD_MIRROR_ROW_BYTES);
      lcd_mirror_capture(0, (uint16_t)y, LCD_MIRROR_PANEL_W, (uint16_t)h,
                         band, LCD_MIRROR_ROW_BYTES);
      usleep(20 * 1000);
    }

  /* 4 秒：足够把这一整屏（15 帧）发完，顺便跨过第一个 3 秒关键帧 */
  usleep(4000 * 1000);
  s_feed = file_size(argv[1]);
  printf("喂完整屏、再等 4 秒：文件 %ld 字节\n", s_feed);

  if (s_feed <= 0)
    {
      printf("FAIL: 串口节点上什么都没写出去（传输没通）\n");
      return 1;
    }

  /* ---- 3) 策略①：什么都不喂，再等 3.6 秒，文件必须又长出来 ---- */

  usleep(3600 * 1000);
  s_idle = file_size(argv[1]);
  printf("空转 3.6 秒：文件 %ld 字节（多出 %ld 字节）\n",
         s_idle, s_idle - s_feed);

  if (s_idle <= s_feed)
    {
      printf("FAIL: 3 秒关键帧没发（文件没长）—— 策略①没生效\n");
      fails++;
    }
  else
    {
      printf("ok   [策略① 整屏关键帧]: 一条脏区都不喂，它自己在重发整屏\n");
    }

  /* ---- 4) lcd_mirror_inject_touch（串口模式的触摸入口） ----
   * 每次注入之间都读一次 —— 这就是 LVGL 的 read_cb 每个周期都在干的事
   * （read_cb 是"读当前状态"，不读就看不到变化）。 */

  printf("--- 注入触摸 ---\n");

  lcd_mirror_inject_touch(100, 200, true);
  fails += check_touch(100, 200, true, "按下");

  lcd_mirror_inject_touch(300, 77, true);
  fails += check_touch(300, 77, true, "拖到 (300, 77)");

  lcd_mirror_inject_touch(300, 77, false);
  fails += check_touch(300, 77, false, "抬手");

  /* 按下和抬起落在两次读之间：第一次读要先补一个"按下"（锁存），第二次才是
   * 抬起 —— 不做这个的话 PC 那一行 `lcdtap x y 1` + `lcdtap x y 0` 会被
   * LVGL 看成"只有抬起"，这一下点击就丢了。 */
  lcd_mirror_inject_touch(10, 20, true);
  lcd_mirror_inject_touch(10, 20, false);

  {
    uint16_t x = 0;
    uint16_t y = 0;
    bool     p = false;

    (void)lcd_mirror_get_touch(&x, &y, &p);
    if (!p || x != 10 || y != 20)
      {
        printf("FAIL [锁存]: 第一次读应该先补一个按下，收到 (%u, %u, %d)\n",
               (unsigned)x, (unsigned)y, p ? 1 : 0);
        fails++;
      }
    else
      {
        printf("ok   [锁存]: 快速点一下先补了按下\n");
      }

    fails += check_touch(10, 20, false, "紧跟着的第二次读是抬起");
  }

  /* 越界坐标要钳住（面板 390x450） */
  lcd_mirror_inject_touch(9999, 9999, true);
  fails += check_touch(LCD_MIRROR_PANEL_W - 1, LCD_MIRROR_PANEL_H - 1, true,
                       "越界钳位");
  lcd_mirror_inject_touch(0, 0, false);

  printf("喂完，status：\n");
  lcd_mirror_status();

  lcd_mirror_stop();
  usleep(300 * 1000);          /* 等任务退干净、fd 关掉 */
  free(band);

  printf("\nUART SMOKE %s（最终 %ld 字节）\n", fails ? "FAILED" : "PASSED",
         file_size(argv[1]));
  return fails != 0;
}
