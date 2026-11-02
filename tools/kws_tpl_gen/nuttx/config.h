/* 空壳 —— 只为了让 app/hello_app/kws_dtw.{c,h} 能编到 PC 上。
 *
 * kws_dtw.h 第一行 `#include <nuttx/config.h>` 是它唯一的 NuttX 依赖，
 * 而那个头里**没有任何宏被 kws_dtw.c 用到**（已核对：全文件无 CONFIG_* 引用）。
 * 所以这个空文件就够了，不用去动 kws_dtw 本身。
 *
 * build.sh 用 -I 这个目录把它排在系统头之前。 */
