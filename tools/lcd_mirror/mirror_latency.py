#!/usr/bin/env python3.10
# -*- coding: utf-8 -*-
"""量一次"点一下 -> 镜像画面跟着变"的端到端延迟。

用法: py -3.10 mirror_latency.py <端口> <前缀> <x> <y>

做法：先等一帧整屏当基线（板子新连上来会补发整屏），记下基线像素；
到 t0 从 PC 发一次按下+抬起；之后每收到一帧就跟基线比"变了多少像素"，
第一个变化超过阈值的帧的时间减去 t0 = 端到端延迟（含板子渲染 + 发送 + 本机解析）。
"""
import socket, struct, sys, time
from PIL import Image, ImageChops

port = int(sys.argv[1]) if len(sys.argv) > 1 else 5601
prefix = sys.argv[2] if len(sys.argv) > 2 else "lat"
tx = int(sys.argv[3]) if len(sys.argv) > 3 else 195
ty = int(sys.argv[4]) if len(sys.argv) > 4 else 371

W, H = 390, 450
canvas = Image.new("RGB", (W, H), (0, 0, 0))
base = None
frames = 0
frames_after = 0
t0 = None
first_change = None
first_change_px = 0

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", port))
srv.listen(1)
srv.settimeout(0.5)
print("listening on", port, flush=True)


# ==== 共享解码块 A BEGIN（四个脚本里必须逐字一致；_mirror_v2_selftest.py 会核对）====
# 板 -> PC 两种版本都要收得下（板子上现在跑的仍然是 v1）：
#   v1 = 16 字节头 + 原样 w*h*2 字节 RGB565 小端、行优先、无行间填充
#   v2 = 20 字节头（多一个 u32 payload_len），flags bit0=1 时载荷是 RLE 流
#        RLE = 重复 {u8 run(1..255), u16 pixel_le}，一共覆盖 w*h 个像素
MAGIC0 = ord("L")
MAGIC1 = ord("M")
FLAG_RLE = 0x01
HDR_V1_FMT = struct.Struct("<BBBBHHHHI")    # 4 + 8 + 4 = 16 字节
HDR_V2_FMT = struct.Struct("<BBBBHHHHII")   # 4 + 8 + 4 + 4 = 20 字节
HDR_V1_SIZE = HDR_V1_FMT.size
HDR_V2_SIZE = HDR_V2_FMT.size
assert HDR_V1_SIZE == 16 and HDR_V2_SIZE == 20


def header_size(ver):
    """这一帧的头有多长：v1 = 16，v2 起 = 20。读版本号前至少要攒够 16 字节。"""
    return HDR_V2_SIZE if ver >= 2 else HDR_V1_SIZE


def parse_header(h):
    """字节串头部 -> (头长, m0, m1, ver, flags, x, y, w, h, seq, payload_len)。

    调用方保证 len(h) >= header_size(h[2])。v1 的载荷长度由 w*h*2 算出来。
    """
    ver = h[2]
    if ver >= 2:
        m0, m1, ver, flags, x, y, w, hh, seq, plen = HDR_V2_FMT.unpack_from(h, 0)
        return HDR_V2_SIZE, m0, m1, ver, flags, x, y, w, hh, seq, plen
    m0, m1, ver, flags, x, y, w, hh, seq = HDR_V1_FMT.unpack_from(h, 0)
    return HDR_V1_SIZE, m0, m1, ver, flags, x, y, w, hh, seq, w * hh * 2


def rle_expand(data, npixels):
    """RLE 流 -> 原样 RGB565 字节。返回 (bytes, 是否正好覆盖 npixels 个像素)。

    整段 run 用切片赋值一次写进目标（C 层 memcpy），不走逐像素的 Python 循环。
    另有一条快路：全是 run=1 的"没压住的"流（噪点帧就长这样），
    整段用两次切片赋值拼出来，别让十几万条记录去走 Python 循环。
    """
    need = npixels * 2
    if len(data) == need // 2 * 3 and set(data[0::3]) == {1}:
        out = bytearray(need)
        out[0::2] = data[1::3]
        out[1::2] = data[2::3]
        return bytes(out), True
    out = bytearray(need)
    o = 0
    i = 0
    n = len(data)
    while o < need:
        if i + 3 > n:
            return bytes(out), False
        run = data[i]
        end = o + run * 2
        if run == 0 or end > need:
            return bytes(out), False
        out[o:end] = data[i + 1:i + 3] * run
        o = end
        i += 3
    return bytes(out), True


def rgb565_to_image(data, w, h):
    """RGB565 小端 -> PIL RGB 图。

    `Image.frombytes(..., 'raw', 'BGR;16')` 就是干这个的：PIL 的 'BGR;16'
    按下标读两个字节、高字节在前，正好等于"小端 RGB565"。
    （本机实测过：编码 (255,0,0) -> 0xF800 小端 f8 00 -> 解出 (255,0,0)。
    低位的取整和逐像素手算差最多 1/通道，见 _mirror_v2_selftest.py。）
    """
    return Image.frombytes("RGB", (w, h), data, "raw", "BGR;16")


def decode_frame(ver, flags, w, h, payload):
    """一帧载荷 -> PIL RGB 图。v1/v2 都走这里；v2 的 RLE 先展开成原样字节。"""
    if ver >= 2 and (flags & FLAG_RLE):
        raw, ok = rle_expand(payload, w * h)
        if not ok:
            raise ValueError("RLE 流没覆盖满 %d 个像素" % (w * h))
        return rgb565_to_image(raw, w, h)
    return rgb565_to_image(payload[:w * h * 2], w, h)
# ==== 共享解码块 A END ====

# ==== 共享收帧块 B BEGIN（三个脚本里必须逐字一致）====

def recvn(conn, n):
    buf = b""
    while len(buf) < n:
        c = conn.recv(n - len(buf))
        if not c:
            return None
        buf += c
    return buf


def recv_frame(conn):
    """收一整帧。返回 (x, y, w, h, seq, flags, ver, payload)；
    连接断了返回 None；magic 不对返回 "bad"（调用方自己决定重连）。"""
    h = recvn(conn, HDR_V1_SIZE)
    if h is None:
        return None
    hs = header_size(h[2])
    if hs > HDR_V1_SIZE:
        rest = recvn(conn, hs - HDR_V1_SIZE)
        if rest is None:
            return None
        h += rest
    _, m0, m1, ver, flags, x, y, w, hh, seq, plen = parse_header(h)
    if m0 != MAGIC0 or m1 != MAGIC1:
        return "bad"
    pix = recvn(conn, plen)
    if pix is None:
        return None
    return (x, y, w, hh, seq, flags, ver, pix)
# ==== 共享收帧块 B END ====


def paint(x, y, w, h, pix, flags=0, ver=1):
    if x + w <= W and y + h <= H:
        canvas.paste(decode_frame(ver, flags, w, h, pix), (x, y))


conn = None
addr = None
while conn is None:
    try:
        conn, addr = srv.accept()
        conn.settimeout(2.0)
        print("connected from", addr, flush=True)
    except socket.timeout:
        continue

# --- 1) 等一帧整屏，当基线 ---
t_wait = time.time()
while base is None and time.time() - t_wait < 8.0:
    try:
        fr = recv_frame(conn)
        if fr is None:
            break
        if fr == "bad":
            print("BAD MAGIC", flush=True)
            break
        x, y, w, hgt, seq, flags, ver, pix = fr
        frames += 1
        paint(x, y, w, hgt, pix, flags, ver)
        if (x, y, w, hgt) == (0, 0, W, H):
            base = canvas.copy()
    except socket.timeout:
        continue
    except OSError:
        break

if base is None:
    print("没等到整屏基线帧，测不了", flush=True)
    canvas.resize((W * 2, H * 2), Image.NEAREST).save(prefix + "_base.png")
    sys.exit(2)

canvas.resize((W * 2, H * 2), Image.NEAREST).save(prefix + "_base.png")
print("基线到手（收帧 %d），1 秒后注入点按 x=%d y=%d" % (frames, tx, ty), flush=True)
time.sleep(1.0)

# --- 2) 注入一次点按，记 t0 ---
conn.sendall(struct.pack("<BBBBHH", ord("L"), ord("T"), 1, 1, tx, ty))
t0 = time.time()
time.sleep(0.12)
conn.sendall(struct.pack("<BBBBHH", ord("L"), ord("T"), 1, 0, tx, ty))
print("SENT press+release, t0 记下", flush=True)

# --- 3) 之后每帧跟基线比 ---
end = time.time() + 4.0
while time.time() < end:
    try:
        fr = recv_frame(conn)
        if fr is None:
            break
        if fr == "bad":
            print("BAD MAGIC", flush=True)
            break
        x, y, w, hgt, seq, flags, ver, pix = fr
        frames += 1
        frames_after += 1
        paint(x, y, w, hgt, pix, flags, ver)
        if first_change is None:
            diff = ImageChops.difference(canvas, base).convert("L")
            npx = sum(1 for v in diff.getdata() if v > 8)
            if npx > 200:
                first_change = time.time() - t0
                first_change_px = npx
                print("第一帧变化: %.0f ms（%d 像素）"
                      % (first_change * 1000, npx), flush=True)
    except socket.timeout:
        continue
    except OSError:
        break

canvas.resize((W * 2, H * 2), Image.NEAREST).save(prefix + "_after.png")
print("总收帧 %d（点后 %d）" % (frames, frames_after), flush=True)
if first_change is None:
    print("点完之后画面一直没变（>=4s）—— 这一下要么没生效，要么落在空白处", flush=True)
else:
    print("端到端延迟 = %.0f ms" % (first_change * 1000), flush=True)
try:
    conn.close()
except Exception:
    pass
