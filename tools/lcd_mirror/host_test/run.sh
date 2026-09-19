#!/bin/bash
# 主机侧端到端测试（不需要板子）：
#   1) 用 stubs/ 里的 NuttX 桩，把**真实的** board/contest_board/src/lcd_mirror.c
#      编到 Linux 上（真 socket、真非阻塞 send）；
#   2) 起一个裸 socket 接收端，把收到的字节流原样落盘；
#   3) 跑 C 侧驱动，喂它"LVGL 会喂的那种脏矩形"。
#   4) 落盘的字节流交给 Windows 侧 check_frames.py（py -3.10 + 真 lcd_mirror.py 的
#      解码器）逐像素比对。
#
# 用法： bash /mnt/d/apply/claw/_lcd_mirror_host_test/run.sh
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
BOARD="$HERE/../../../board/contest_board/src"
PORT=5612
DUMP="$HERE/frames.bin"
DUMP2="$HERE/frames_after_reconnect.bin"
SENT="$HERE/touch_sent.txt"
COUT="$HERE/c_test_output.txt"

cd "$HERE" || exit 2

echo "== 1) 编译（真板级源码 + 主机桩） =="
gcc -O1 -g -Wall -Wextra -Wno-unused-parameter \
    -I "$HERE/stubs" -I "$BOARD" \
    -o lcd_mirror_host_test \
    test.c stub.c "$BOARD/lcd_mirror.c" -lpthread
RC=$?
echo "gcc rc=$RC"
if [ $RC -ne 0 ]; then echo "编译失败"; exit 1; fi

rm -f "$DUMP" "$DUMP2" "$SENT" "$COUT"

echo
echo "== 2) 起假 PC 端（第 1 段收 2 秒后硬断；重连后按脚本发反向触摸） =="
python3 dump_frames.py "$PORT" "$DUMP" "$DUMP2" "$SENT" &
DUMPPID=$!
sleep 0.4

echo
echo "== 3) 跑板级模块（喂 LVGL 风格的脏矩形 + 轮询反向触摸） =="
./lcd_mirror_host_test 127.0.0.1 "$PORT" 2>&1 | tee "$COUT"
RC=$?
echo "C 侧退出码 = $RC"

wait $DUMPPID
echo
ls -la "$DUMP" "$DUMP2" "$SENT" 2>&1
echo
echo "下一步（Windows 侧，两个都要跑）："
echo "  py -3.10 D:/apply/claw/_lcd_mirror_host_test/check_frames.py \\"
echo "      D:/apply/claw/_lcd_mirror_host_test/frames.bin \\"
echo "      D:/apply/claw/_lcd_mirror_host_test/frames_after_reconnect.bin \\"
echo "      D:/apply/claw/_lcd_mirror_host_test/touch_sent.txt \\"
echo "      D:/apply/claw/_lcd_mirror_host_test/c_test_output.txt"
echo "  py -3.10 D:/apply/claw/_lcd_mirror_host_test/check_mapping.py"
