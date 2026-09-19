/* 主机侧驱动：把板级 lcd_mirror.c 原样跑起来，喂它"LVGL 会喂的那种脏矩形"，
   真正的接收端是 _flash/lcd_mirror.py 里的 Receiver（Python 驱动脚本起的）。

   跑法（在 WSL 里）：
       bash /mnt/d/apply/claw/_lcd_mirror_host_test/run.sh
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>

#include <nuttx/clock.h>

#include "lcd_mirror.h"

/* 期望画面：C 侧和 Python 侧各算一遍，逐像素比。
   全程整数运算，两边一模一样。 */
static uint16_t expect565(int x, int y)
{
  unsigned r = (unsigned)((x * 31) / (LCD_MIRROR_PANEL_W - 1));
  unsigned g = (unsigned)((y * 63) / (LCD_MIRROR_PANEL_H - 1));
  unsigned b = (unsigned)((((x * 7) + (y * 13)) & 0x7ff) * 31 / 2047);

  return (uint16_t)((r << 11) | (g << 5) | b);
}

/* 把一块矩形填成期望像素（RGB565 小端，行距 stride 字节） */
static void fill_rect(uint8_t *buf, int x, int y, int w, int h, int stride)
{
  int row;
  int col;

  for (row = 0; row < h; row++)
    {
      for (col = 0; col < w; col++)
        {
          uint16_t v = expect565(x + col, y + row);

          buf[row * stride + col * 2 + 0] = (uint8_t)(v & 0xff);
          buf[row * stride + col * 2 + 1] = (uint8_t)(v >> 8);
        }
    }
}

/* 把一块矩形填成同一个颜色（RGB565 小端，行距 stride 字节，目标就是 (x, 0) 起）。
   给"RLE 压得最狠"的那个场景用（整块一个像素 -> 长 run）。 */
static void fill_solid(uint8_t *buf, int x, int w, int h, int stride,
                       uint16_t v)
{
  int row;
  int col;

  for (row = 0; row < h; row++)
    {
      for (col = 0; col < w; col++)
        {
          buf[row * stride + (x + col) * 2 + 0] = (uint8_t)(v & 0xff);
          buf[row * stride + (x + col) * 2 + 1] = (uint8_t)(v >> 8);
        }
    }
}

/* 相邻像素**保证不一样**的一块（低字节就是列号，一路递增）。
   给"压不下去、必须退回原样"那个场景用：run 全是 1 个像素 -> 3 字节/像素，
   比原样 2 字节/像素还大，板端必须放弃压缩、按 flags=0 原样发。 */
static void fill_noise(uint8_t *buf, int x, int w, int h, int stride)
{
  int row;
  int col;

  for (row = 0; row < h; row++)
    {
      for (col = 0; col < w; col++)
        {
          uint16_t v = (uint16_t)((col & 0xff) |
                                  (uint16_t)((((col * 37) + (row * 11)) & 0xff)
                                             << 8));

          buf[row * stride + (x + col) * 2 + 0] = (uint8_t)(v & 0xff);
          buf[row * stride + (x + col) * 2 + 1] = (uint8_t)(v >> 8);
        }
    }
}

int main(int argc, char **argv)
{
  uint8_t *band;
  uint8_t *small;
  uint8_t *odd;
  int      y;
  int      ret;

  if (argc < 3)
    {
      fprintf(stderr, "用法: %s <ip> <port>\n", argv[0]);
      return 2;
    }

  /* 整屏一条带 60 行（= LVGL 那块 46800 字节 draw buffer 装得下的行数） */
  band  = malloc((size_t)LCD_MIRROR_ROW_BYTES * 60);
  /* 行距有填充的小脏区：50 像素宽却按 128 字节一行（走逐行拷贝那条路） */
  small = malloc(128 * 8);
  /* 越界裁剪用的大缓冲 */
  odd   = malloc(200 * 100);

  if (band == NULL || small == NULL || odd == NULL)
    {
      fprintf(stderr, "malloc failed\n");
      return 2;
    }

  ret = lcd_mirror_set_target(argv[1], (uint16_t)atoi(argv[2]));
  printf("set_target -> %d (%s:%d)\n", ret, lcd_mirror_target_ip(),
         (int)lcd_mirror_target_port());

  ret = lcd_mirror_start();
  printf("start -> %d, running=%d\n", ret, (int)lcd_mirror_is_running());
  if (ret < 0)
    {
      return 1;
    }

  usleep(300 * 1000);       /* 让任务先连上 */

  /* --- 场景 A：LVGL 分带整屏重绘（stride == 780，整行对齐的最快路径） --- */
  for (y = 0; y < LCD_MIRROR_PANEL_H; y += 60)
    {
      int h = LCD_MIRROR_PANEL_H - y;

      if (h > 60)
        {
          h = 60;
        }

      fill_rect(band, 0, y, LCD_MIRROR_PANEL_W, h, LCD_MIRROR_ROW_BYTES);
      lcd_mirror_capture(0, (uint16_t)y, LCD_MIRROR_PANEL_W, (uint16_t)h,
                         band, LCD_MIRROR_ROW_BYTES);
      usleep(20 * 1000);
    }

  /* --- 场景 B：小脏区 + 行距填充（走逐行拷贝，位置必须对上） --- */
  fill_rect(small, 100, 10, 50, 6, 128);
  lcd_mirror_capture(100, 10, 50, 6, small, 128);
  usleep(250 * 1000);

  /* --- 场景 C：只有最后一行 --- */
  fill_rect(band, 0, LCD_MIRROR_PANEL_H - 1, LCD_MIRROR_PANEL_W, 1,
            LCD_MIRROR_ROW_BYTES);
  lcd_mirror_capture(0, LCD_MIRROR_PANEL_H - 1, LCD_MIRROR_PANEL_W, 1,
                     band, LCD_MIRROR_ROW_BYTES);
  usleep(250 * 1000);

  /* --- 场景 D：越界（x=380,y=440,w=100,h=100 要被裁成 10x10） --- */
  fill_rect(odd, 380, 440, 100, 100, 200);
  lcd_mirror_capture(380, 440, 100, 100, odd, 200);
  usleep(250 * 1000);

  /* --- 这一段长睡是留给接收端的：它会在这期间把连接**硬断掉**（RST），
   *     接下来这轮整屏发送就应该发现 socket 坏了 -> 自己 close -> 重连。 --- */
  usleep(2000 * 1000);

  /* --- 场景 E：整屏重绘（此时对面已经把我们踢了） --- */
  for (y = 0; y < LCD_MIRROR_PANEL_H; y += 60)
    {
      int h = LCD_MIRROR_PANEL_H - y;

      if (h > 60)
        {
          h = 60;
        }

      fill_rect(band, 0, y, LCD_MIRROR_PANEL_W, h, LCD_MIRROR_ROW_BYTES);
      lcd_mirror_capture(0, (uint16_t)y, LCD_MIRROR_PANEL_W, (uint16_t)h,
                         band, LCD_MIRROR_ROW_BYTES);
      usleep(10 * 1000);
    }

  usleep(2000 * 1000);      /* 等发送端发现断线、重连、把整屏重发一遍 */

  /* ==================== 场景 F/G：协议 v2 的两条载荷路径 ====================
   * F：纯色块（长 run）—— 压缩比最高的一档；40 行 > 一帧上限
   *    （LCD_MIRROR_MAX_ROWS_PER_FRAME = 30），所以会被拆成 30+10 两帧。
   * G：相邻像素全不一样的噪声 —— 压不下去，**必须退回原样**（flags bit0=0）；
   *    正好 30 行 = 一帧装得下。
   * F 顺带验证"脏区比 scratch 大就拆帧、一行都不丢"。
   * 每个场景发完纯色/噪声之后再按期望画面把那几行刷回来：画布的**最终**内容
   * 于是跟别的场景一样（check_frames 逐像素比的就是这个），而中间那几帧就是
   * 压缩用例（RLE 帧和原样帧都要真的出现过，check_frames 会各自断言）。 */

  {
    int fy = 200;
    int fh = LCD_MIRROR_MAX_ROWS_PER_FRAME + 10;   /* 40 行 > 一帧上限，必拆帧 */
    int gy = 300;
    int gh = LCD_MIRROR_MAX_ROWS_PER_FRAME;        /* 30 行，正好一帧 */

    /* --- F1：40 行纯色 0x1234 --- */
    fill_solid(band, 0, LCD_MIRROR_PANEL_W, fh, LCD_MIRROR_ROW_BYTES, 0x1234);
    lcd_mirror_capture(0, (uint16_t)fy, LCD_MIRROR_PANEL_W, (uint16_t)fh,
                       band, LCD_MIRROR_ROW_BYTES);
    usleep(300 * 1000);

    /* --- F2：刷回期望画面 --- */
    fill_rect(band, 0, fy, LCD_MIRROR_PANEL_W, fh, LCD_MIRROR_ROW_BYTES);
    lcd_mirror_capture(0, (uint16_t)fy, LCD_MIRROR_PANEL_W, (uint16_t)fh,
                       band, LCD_MIRROR_ROW_BYTES);
    usleep(300 * 1000);

    /* --- G1：30 行噪声（每个 run 只有 1 个像素） --- */
    fill_noise(band, 0, LCD_MIRROR_PANEL_W, gh, LCD_MIRROR_ROW_BYTES);
    lcd_mirror_capture(0, (uint16_t)gy, LCD_MIRROR_PANEL_W, (uint16_t)gh,
                       band, LCD_MIRROR_ROW_BYTES);
    usleep(300 * 1000);

    /* --- G2：刷回期望画面 --- */
    fill_rect(band, 0, gy, LCD_MIRROR_PANEL_W, gh, LCD_MIRROR_ROW_BYTES);
    lcd_mirror_capture(0, (uint16_t)gy, LCD_MIRROR_PANEL_W, (uint16_t)gh,
                       band, LCD_MIRROR_ROW_BYTES);
    usleep(300 * 1000);
  }

  /* ==================== 第二阶段：反向通道（鼠标模拟触摸） ====================
   * 假 PC 端从"重连成功"算起 5 秒开始按脚本发触摸，发约 4.5 秒。
   * 这里从当前时刻（重连后约 3.2 秒：整屏重发 + 场景 F/G + 各自的等待）开始
   * 轮询 10 秒，把**每一次状态变化**打一行出来，交给 Windows 侧的
   * check_frames.py 跟"发出去的逻辑消息"逐条比对。前面那些等待别随便加长：
   * 轮询必须早于对面第一条第 5 秒的触摸，不然开头几条状态就被漏掉了。
   *
   * 轮询周期 5ms 远小于对面 300ms 的间隔，所以不会有状态被跳过。
   * 顺便每 500ms 喂一块脏矩形：这样板子在收触摸的同时还在发帧，
   * "触摸消息夹在帧中间到达"才是真的。 */

  printf("TOUCHPOLL begin\n");

  {
    uint32_t t0     = clock_systime_ticks();
    uint32_t lastfb = t0;
    int      have   = 0;
    int      lx     = -1;
    int      ly     = -1;
    int      lp     = -1;
    int      nrec   = 0;
    int      row    = 0;

    while (TICK2MSEC(clock_systime_ticks() - t0) < 10000)
      {
        uint16_t tx = 0;
        uint16_t ty = 0;
        bool     tp = false;

        if (lcd_mirror_get_touch(&tx, &ty, &tp))
          {
            if (!have || (int)tx != lx || (int)ty != ly ||
                (int)tp != lp)
              {
                printf("TOUCH %u %u %d\n", (unsigned)tx, (unsigned)ty,
                       tp ? 1 : 0);
                fflush(stdout);
                lx   = (int)tx;
                ly   = (int)ty;
                lp   = tp ? 1 : 0;
                have = 1;
                nrec++;
              }
          }

        /* 每 500ms 喂一行脏矩形：让帧继续在发（顺带覆盖"发送和收报并发"） */
        if (TICK2MSEC(clock_systime_ticks() - lastfb) >= 500)
          {
            lastfb = clock_systime_ticks();
            fill_rect(band, 0, row, LCD_MIRROR_PANEL_W, 1,
                      LCD_MIRROR_ROW_BYTES);
            lcd_mirror_capture(0, (uint16_t)row, LCD_MIRROR_PANEL_W, 1,
                               band, LCD_MIRROR_ROW_BYTES);
            row = (row + 37) % LCD_MIRROR_PANEL_H;
          }

        usleep(5 * 1000);
      }

    printf("TOUCHPOLL end %d\n", nrec);
  }

  printf("喂完，status：\n");
  lcd_mirror_status();

  lcd_mirror_stop();
  free(band);
  free(small);
  free(odd);

  printf("SCENARIO DONE\n");
  return 0;
}
