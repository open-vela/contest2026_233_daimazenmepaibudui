#!/bin/bash
# 串口(UART)传输的主机侧冒烟测试 —— 和 run.sh 是**两条独立的腿**：
#   run.sh      ：TCP 传输那条（真 socket、假 PC 端、反向触摸）；协议和它的
#                 验证脚本一个字都没动过
#   run_uart.sh ：串口传输那条（把"节点"指成一个普通文件，跑的是同一份
#                 lcd_mirror.c 的串口分支），落盘的字节流交给同一个
#                 check_frames.py 校验
#
# 用法： bash /mnt/d/apply/claw/_lcd_mirror_host_test/run_uart.sh
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
BOARD="$HERE/../../../board/contest_board/src"
# 节点名走的是板上的 open()，所以这里用一个**短**路径：板端的节点名缓冲只有
# LCD_MIRROR_UART_DEV_MAX(32) 字节（板上真节点名是 /dev/console 这种），太长
# 会被 lcd_mirror_use_uart() 直接拒掉。落盘之后复制到 Windows 侧目录再看。
NODE=/tmp/lmuart.bin
DUMP="$HERE/uart_frames.bin"
COUT="$HERE/uart_c_output.txt"

cd "$HERE" || exit 2

echo "== 1) 编译（真板级源码 + 主机桩） =="
gcc -O1 -g -Wall -Wextra -Wno-unused-parameter \
    -I "$HERE/stubs" -I "$BOARD" \
    -o uart_test uart_test.c stub.c "$BOARD/lcd_mirror.c" -lpthread
RC=$?
echo "gcc rc=$RC"
if [ $RC -ne 0 ]; then echo "编译失败"; exit 1; fi

echo
echo "== 2) 清空落盘文件（串口节点必须已存在：板端 open 不带 O_CREAT） =="
: > "$NODE"

echo
echo "== 3) 跑板级模块（切 UART 传输 + 喂整屏 + 空转等关键帧 + 注入触摸） =="
./uart_test "$NODE" 2>&1 | tee "$COUT"
RC=$?
echo "C 侧退出码 = $RC"

echo
echo "== 4) 剪掉尾部那半截帧（停镜像时可能正发到一半）再落盘 =="
python3 - "$NODE" "$DUMP" <<'PY'
import sys

# 一帧最大 = 20 字节头 + 30 行 x 780 字节原样载荷
HDR, MAXRAW = 20, 30 * 780
src, dst = sys.argv[1], sys.argv[2]
data = open(src, "rb").read()

off = last = 0
while off < len(data):
    if len(data) - off < HDR:
        break
    if data[off] != ord('L') or data[off + 1] != ord('M') or data[off + 2] != 2:
        break
    plen = int.from_bytes(data[off + 16:off + 20], "little")
    if plen == 0 or plen > MAXRAW or off + HDR + plen > len(data):
        break
    off += HDR + plen
    last = off

tail = len(data) - last
print("落盘 %d 字节，完整帧到 %d，尾部残留 %d 字节" % (len(data), last, tail))
# 尾部只允许是"最后那一帧没写完的那点字节"；超出就说明中间就有坏数据，
# 那是真问题，不能靠剪掉来蒙过去。
if tail > HDR + MAXRAW:
    raise SystemExit("FAIL: 尾部残留 %d 字节远超一帧上限，说明中间有坏帧" % tail)

open(dst, "wb").write(data[:last])
print("写到 %s：%d 字节" % (dst, last))
PY
RC2=$?
if [ $RC2 -ne 0 ]; then echo "尾部帧处理失败"; exit $RC2; fi

ls -la "$DUMP"

echo
echo "下一步（Windows 侧，用同一个校验脚本，只比这一条腿的帧）："
echo "  py -3.10 D:/apply/claw/_lcd_mirror_host_test/check_frames.py \\"
echo "      D:/apply/claw/_lcd_mirror_host_test/uart_frames.bin"

exit $RC
