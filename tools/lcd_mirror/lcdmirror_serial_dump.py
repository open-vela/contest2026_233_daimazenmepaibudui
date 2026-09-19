#!/usr/bin/env python3.10
# -*- coding: utf-8 -*-
"""无头串口收帧器：直接读 COM 口，重同步解帧，拼成画布存 PNG。

用法: py -3.10 lcdmirror_serial_dump.py <COM口> <秒数> <输出前缀>

为什么单独一个脚本（不直接用 lcd_mirror.py）：那是 Tk 窗口版，跑起来要人看着；
这个是我"看真帧"的眼睛 —— 串口模式下也能不依赖 GUI 抓一张图下来核对。
串口里混着 nsh/内核日志文本，所以走 lcd_mirror.py 里那份 FrameScanner 重同步
（同一份代码，不另写一套解码）。
"""
import importlib.util
import sys
import time

import serial
from PIL import Image, ImageChops

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM4"
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0
PREFIX = sys.argv[3] if len(sys.argv) > 3 else "serial_dump"
TAP = None
if len(sys.argv) > 5:
    TAP = (int(sys.argv[4]), int(sys.argv[5]))   # 收完基线后往这儿点一下，量延迟

spec = importlib.util.spec_from_file_location(
    "lcd_mirror", r"D:/apply/claw/_flash/lcd_mirror.py")
lm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lm)

W, H = 390, 450
canvas = Image.new("RGB", (W, H), (0, 0, 0))
sc = lm.FrameScanner()
frames = 0
bad = 0
garbage = 0
cover = [0] * H          # 每一行收到过几次
shown = 0
payload_bytes = 0
key_rows = 6             # 这个行数以上算"大带子"（关键帧带）
big_frames = 0
big_bytes = 0

ser = serial.Serial(PORT, 1000000, timeout=0.05)
try:
    ser.set_buffer_size(rx_size=1 << 18, tx_size=1 << 12)
except Exception:
    pass
print("opened", PORT, flush=True)

base = None          # 点之前的画面（用来算"点一下 -> 画面变"的延迟）
t_tap = None
lat = None

end = time.time() + SECS
t_start = time.time()
while time.time() < end:
    data = ser.read(4096)
    if not data:
        continue
    for (x, y, w, h, seq, flags, ver, payload) in sc.feed(data):
        try:
            rect = lm.decode_frame(ver, flags, w, h, payload)
        except ValueError as e:
            bad += 1
            print("bad frame seq=%d y=%d h=%d: %s" % (seq, y, h, e), flush=True)
            continue
        canvas.paste(rect, (x, y))
        frames += 1
        payload_bytes += len(payload)
        if h >= key_rows:
            big_frames += 1
            big_bytes += len(payload)
        if TAP is not None and t_tap is not None and lat is None:
            diff = ImageChops.difference(canvas, base).convert("L")
            if sum(1 for v in diff.getdata() if v > 8) > 200:
                lat = (time.time() - t_tap) * 1000.0
                print("点后第一帧变化 = %.0f ms" % lat, flush=True)
        if TAP is not None and t_tap is None and (
                y + h >= 400 or (time.time() - t_start) > 4.0):
            # 基线够用了（整屏扫描到下半屏）再点，免得拿半张旧画面当基线
            base = canvas.copy()
            ser.write(("hw_test lcdtap %d %d 1\r\n" % TAP).encode("ascii"))
            time.sleep(0.12)
            ser.write(("hw_test lcdtap %d %d 0\r\n" % TAP).encode("ascii"))
            t_tap = time.time()
            print("已点 (%d, %d)" % TAP, flush=True)
        for r in range(y, min(y + h, H)):
            cover[r] += 1
        if shown < 12:
            shown += 1
            print("frame seq=%d x=%d y=%d w=%d h=%d flags=%d payload=%d"
                  % (seq, x, y, w, h, flags, len(payload)), flush=True)

print("frames=%d bad=%d resyncs=%d garbage_bytes=%d"
      % (frames, bad, sc.resyncs, sc.dropped), flush=True)
print("载荷字节 = %d（%.0f B/s），其中 %d 行以上的大带子 = %d 帧 / %d 字节"
      % (payload_bytes, payload_bytes / max(SECS, 0.001),
         key_rows, big_frames, big_bytes), flush=True)
print("行覆盖: 0 次的行数 = %d / %d" % (cover.count(0), H), flush=True)
print("前 8 段覆盖（每 30 行一档）:",
      [sum(cover[i:i + 30]) for i in range(0, H, 30)], flush=True)
canvas.resize((W * 2, H * 2), Image.NEAREST).save(PREFIX + ".png")
print("saved", PREFIX + ".png", flush=True)
ser.close()
