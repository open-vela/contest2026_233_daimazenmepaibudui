/* 主机侧桩：把 board/contest_board/src/lcd_mirror.c 原样编到 Linux 上跑。

   为什么能这么干：lcd_mirror.c 只用到 NuttX 的这几样东西 ——
   config / clock_systime_ticks / TICK2MSEC / nxmutex / task_create / syslog。
   全部在这里换成 pthread + 单调时钟，socket 那份直接用真 Linux socket
   （非阻塞 connect + send 的语义和 NuttX 一致）。所以"脏行记账 -> 帧打包 ->
   非阻塞发送"这条链在主机上跑的就是板子上的同一份代码。 */

#ifndef __LCD_MIRROR_HOST_STUB_H
#define __LCD_MIRROR_HOST_STUB_H

#include <stdint.h>
#include <stddef.h>

#define FAR

/* 真 NuttX 里 OK 由 <nuttx/errno.h> 经系统头带进来（本板源码就是这么用的） */
#ifndef OK
#  define OK 0
#endif

#endif
