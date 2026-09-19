#!/usr/bin/env python3.10
# -*- coding: utf-8 -*-
"""串口模式自测：混着日志文本 / 半帧 / 坏帧的字节流，能不能重同步出正确画面。

    py -3.10 ./check_serial_resync.py

不烧板子、不开窗口 —— 解析那一段（lcd_mirror.FrameScanner）本来就是脱离 Tk 单独调。

干六件事：
  1. 造一条"日志文本 + 完整帧 + 半帧 + 坏头 + 又一帧 + 坏载荷帧 + 再一帧"的字节流；
  2. 整条一次喂给真的 FrameScanner，断言解出来的帧序列 / 字段 / 载荷和发出去的一样；
  3. 把解出来的每一帧用真的 decode_frame 解成画面，和**独立算出来**的期望画面
     （RGB565->RGB888 的展开表单独测出来，不借 decode_frame）逐像素比；
     再把所有帧贴到同一块 390x450 画布上比一次 —— 坏帧那一块必须是干净的黑色，
     说明"坏帧被丢掉、没画上去，而且后面的帧一个都没受影响"；
  4. 同一条流按 1/2/3/5/7/64/4096 字节切块喂进去，结论必须一模一样
     （真实串口 read() 回来的是任意碎块）；
  5. 半帧 / 坏头 / 坏载荷 各自单独演示一遍；
  6. 反向通道（触摸）：touch_cmd 的文本、拖动合并、~20 条/秒限流、
     "抬起绝不被吞"，用一个假串口验；
  7. 串口链路端到端：假的 pyserial 把这条流按碎块喂进真的 SerialReceiver.run()，
     读线程 -> FrameScanner -> 队列 -> 解码 -> 画布，逐像素比；
  8. 真 GUI 接线：--serial 时用 SerialReceiver（不 bind 端口）、标题/状态行显示
     当前链路；不给 --serial（那些老 Namespace 连 serial 字段都没有）仍走 TCP。
"""
import importlib.util
import queue
import random
import struct
import sys
import threading
import time

from PIL import Image

CLIENT = r"lcd_mirror.py"
PANEL_W, PANEL_H = 390, 450
HDR2 = struct.Struct("<BBBBHHHHII")          # 20 字节 v2 头

FAILS = []


def ok(msg):
    print("  [OK]   " + msg, flush=True)


def bad(msg):
    print("  [FAIL] " + msg, flush=True)
    FAILS.append(msg)


def load_client():
    spec = importlib.util.spec_from_file_location("lcd_mirror", CLIENT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# ------------------------------------------------- 造帧（编码这一侧，独立实现）

def rle_encode(pix, npix):
    """RGB565 原样字节 -> RLE 流。每段最多 255 个像素（协议里 run 是 u8）。"""
    out = bytearray()
    i = 0
    while i < npix:
        px = pix[i * 2:i * 2 + 2]
        run = 1
        while run < 255 and i + run < npix and \
                pix[(i + run) * 2:(i + run) * 2 + 2] == px:
            run += 1
        out.append(run)
        out += px
        i += run
    return bytes(out)


def frame_v2(x, y, w, h, seq, pix, use_rle):
    body = rle_encode(pix, w * h) if use_rle else pix
    return HDR2.pack(ord("L"), ord("M"), 2, 1 if use_rle else 0,
                     x, y, w, h, seq, len(body)) + body


def make_pixels(w, h, kind, seed=7):
    if kind == "rows":                       # 每行一个颜色：RLE 压得狠
        out = bytearray()
        for y in range(h):
            out += struct.pack("<H", (0x1000 + y * 2691) & 0xFFFF) * w
        return bytes(out)
    if kind == "grad":                       # 每个像素都不同：走原样那条
        out = bytearray()
        for y in range(h):
            out += struct.pack("<%dH" % w,
                               *[((xx * 37 + y * 1031) & 0xFFFF)
                                 for xx in range(w)])
        return bytes(out)
    if kind == "noise":
        return random.Random(seed).randbytes(w * h * 2)
    raise ValueError(kind)


def expand_tables(mod):
    """测出 PIL 的 5 位 / 6 位 -> 8 位展开表（不猜，拿合成图解出来）。"""
    raw = b"".join(bytes([v & 0xFF, (v >> 8) & 0xFF]) for v in
                   [((v << 11) | (v << 6) | v) for v in range(32)])
    t5 = [p[0] for p in mod.rgb565_to_image(raw, 32, 1).getdata()]
    raw = b"".join(bytes([v & 0xFF, (v >> 8) & 0xFF]) for v in
                   [((v << 5) | 0) for v in range(64)])
    t6 = [p[1] for p in mod.rgb565_to_image(raw, 64, 1).getdata()]
    return t5, t6


def expect_rgb(pix, w, h, t5, t6):
    """RGB565 小端 -> RGB888 的期望画面。用测出来的展开表，不借 decode_frame。"""
    out = bytearray()
    for i in range(w * h):
        v = pix[i * 2] | (pix[i * 2 + 1] << 8)
        out += bytes((t5[(v >> 11) & 31], t6[(v >> 5) & 63], t5[v & 31]))
    return bytes(out)


# --------------------------------------------------------------- 造这条字节流

class Stream:
    """一条流 + 它的期望。"""

    def __init__(self):
        frames = {}
        spec = [
            # 名字 / x / y / w / h / seq / 内容 / 是否 RLE
            ("A", 0, 0, 6, 4, 1, "grad", False),
            ("B", 10, 20, 8, 3, 2, "rows", True),
            ("C", 100, 200, 12, 5, 3, "rows", True),
            ("D", 150, 100, 20, 8, 4, "grad", False),
            ("E", 300, 400, 40, 30, 5, "noise", False),
        ]
        for name, x, y, w, h, seq, kind, use_rle in spec:
            pix = make_pixels(w, h, kind)
            frames[name] = dict(name=name, x=x, y=y, w=w, h=h, seq=seq,
                                pix=pix, use_rle=use_rle,
                                raw=frame_v2(x, y, w, h, seq, pix, use_rle))
        self.frames = frames

        # 坏头：结构上能解出 20 字节头，但 x+w = 380+390 = 770 > 390，必须被丢掉；
        # 后面还跟 24 个字节的垃圾（一起被逐字节跳过）
        self.bad_hdr = HDR2.pack(ord("L"), ord("M"), 2, 0, 380, 10, 390, 30,
                                 999, 390 * 30 * 2) + b"\xa5" * 24
        # 坏载荷：头完全合法（8x3, RLE, plen=6），但 6 字节只覆盖 2 个像素 != 24，
        # 所以它**会被扫出来**，由调用方 decode_frame 报错丢掉
        self.bad_payload = HDR2.pack(ord("L"), ord("M"), 2, 1, 200, 300, 8, 3,
                                     998, 6) + b"\x01\x11\x22\x01\x33\x44"

        garbage_boot = (b"\r\nnsh> hw_test lcdmirror status\r\n"
                        b"lcdmirror: running, 12 touch events, last (123, 321)\r\n"
                        b"[uart] mirror tx: queued 12345 bytes, dropped 0\r\n"
                        b"LM"              # 凑巧的 magic，第 3 字节是 '[' 不是 ver
                        b"[uart] ring full, backpressure\r\n"
                        b"\x00\xff\x10")
        garbage_mid = (b"nsh> LMX\r\n" + b"\x05\x06\x07"
                       + b"kernel: usb cdc rx 4096\r\n")
        garbage_tiny = b"nsh> \r\nuart: 0 frames dropped\r\n"

        braw = frames["B"]["raw"]
        self.b_half1, self.b_half2 = braw[:24], braw[24:]

        self.parts = [
            (garbage_boot, "日志文本（nsh + 内核日志 + 假 'LM'）"),
            (frames["A"]["raw"], "完整帧 A（v2 原样 6x4）"),
            (garbage_mid, "日志文本 + 假 'LM'"),
            (self.b_half1, "帧 B 的前半（20 字节头 + 9 字节载荷里的 4 字节）"),
            (self.b_half2, "帧 B 的后半（剩下 5 字节载荷）"),
            (self.bad_hdr, "坏头（x+w=770 越界）+ 24 字节垃圾"),
            (frames["C"]["raw"], "帧 C（v2 RLE 12x5）"),
            (self.bad_payload, "坏帧（头合法、RLE 载荷只盖住 2/24 个像素）"),
            (frames["D"]["raw"], "帧 D：坏帧之后那帧，必须还能逐像素解对"),
            (garbage_tiny, "一小段日志"),
            (frames["E"]["raw"], "帧 E（v2 原样 40x30 噪点）"),
        ]
        self.stream = b"".join(p for p, _ in self.parts)
        self.garbage = (len(garbage_boot) + len(garbage_mid)
                        + len(self.bad_hdr) + len(garbage_tiny))

        bx, by, bw, bh = 200, 300, 8, 3
        self.expect = [
            (frames["A"], True), (frames["B"], True), (frames["C"], True),
            (dict(name="坏帧", x=bx, y=by, w=bw, h=bh, seq=998, pix=None,
                  use_rle=True, raw=self.bad_payload), False),
            (frames["D"], True), (frames["E"], True),
        ]
        self.nframe = sum(1 for _, dec in self.expect if dec)   # 能解码的帧数


# ----------------------------------------------------- 比对：解出来的 == 期望的

def compare(mod, got, expect, t5, t6, quiet=False):
    """返回"逐像素全对"的帧数。"""
    if len(got) != len(expect):
        bad("解出来的帧数 %d != 期望的 %d" % (len(got), len(expect)))
        return 0
    exact = 0
    for fr, (want, decodable) in zip(got, expect):
        gx, gy, gw, gh, gseq, gflags, gver, payload = fr
        tag = want["name"]
        if (gx, gy, gw, gh, gseq) != (want["x"], want["y"], want["w"],
                                      want["h"], want["seq"]):
            bad("%s：矩形/序号对不上 got %s want %s"
                % (tag, (gx, gy, gw, gh, gseq),
                   (want["x"], want["y"], want["w"], want["h"], want["seq"])))
            continue
        if gver != 2 or gflags != (1 if want["use_rle"] else 0):
            bad("%s：ver/flags 不对 ver=%d flags=%#x" % (tag, gver, gflags))
            continue
        if payload != want["raw"][20:]:
            bad("%s：载荷字节和发出去的不一样（%d vs %d 字节）"
                % (tag, len(payload), len(want["raw"]) - 20))
            continue
        try:
            im = mod.decode_frame(gver, gflags, gw, gh, payload)
            dec_ok = True
        except ValueError:
            im, dec_ok = None, False
        if dec_ok != decodable:
            bad("%s：该%s解码的却%s了" % (tag, "能" if decodable else "不能",
                                        "能" if dec_ok else "没能"))
            continue
        if not decodable:
            if not quiet:
                print("    %-4s %3dx%-3d @(%3d,%3d) seq=%-4d RLE   解码报错 "
                      "-> 丢掉（不画上去）" % (tag, gw, gh, gx, gy, gseq),
                      flush=True)
            continue
        ref = expect_rgb(want["pix"], gw, gh, t5, t6)
        if im.tobytes() != ref:
            diff = max(abs(a - b) for a, b in zip(im.tobytes(), ref))
            bad("%s：逐像素比对不一致，最大差 %d" % (tag, diff))
            continue
        exact += 1
        if not quiet:
            print("    %-4s %3dx%-3d @(%3d,%3d) seq=%-4d %s   逐像素一致"
                  % (tag, gw, gh, gx, gy, gseq,
                     "RLE" if gflags else "原样"), flush=True)
    return exact


class FakePort:
    """假的串口：只记下写进来的字节，够验反向通道了。"""

    def __init__(self):
        self.written = []

    def write(self, data):
        self.written.append(bytes(data))

    def flush(self):
        pass


# ----------------------------- 假 pyserial：把 SerialReceiver.run 整条链跑起来

class FakeSerialPort:
    """假装是真串口：read() 按碎块吐预先塞好的字节，write() 记下来。"""

    def __init__(self, chunks, written):
        self.chunks = list(chunks)
        self.written = written
        self.closed = False

    def read(self, _n):
        if self.chunks:
            c = self.chunks.pop(0)
            time.sleep(0.002)              # 假装收这一块花了点时间
            return c
        time.sleep(0.005)
        return b""

    def write(self, data):
        self.written.append(bytes(data))

    def flush(self):
        pass

    def close(self):
        self.closed = True


class _ListPorts:
    @staticmethod
    def comports():
        return []


class _Tools:
    list_ports = _ListPorts()


class FakeSerialMod:
    """假装是 pyserial 模块本身（只用到 Serial / tools.list_ports）。"""

    tools = _Tools()

    def __init__(self, chunks, written):
        self.chunks = chunks
        self.written = written

    def Serial(self, _port, _baud, **_kw):      # noqa: N802（名字照抄 pyserial）
        return FakeSerialPort(self.chunks, self.written)


# ------------------------------------------------------------------ 主流程

def main():
    mod = load_client()
    t5, t6 = expand_tables(mod)
    st = Stream()
    print("客户端：%s" % CLIENT, flush=True)

    # ---- 1) 这条流长什么样 ----
    print("\n1) 造出来的字节流（%d 字节，%d 段）"
          % (len(st.stream), len(st.parts)), flush=True)
    for p, what in st.parts:
        print("    %6d B  %s" % (len(p), what), flush=True)
    print("    其中“帧以外”的垃圾字节一共 %d B" % st.garbage, flush=True)

    # ---- 2) 整体喂一次 ----
    print("\n2) 整条流一次喂进去", flush=True)
    sc = mod.FrameScanner(lambda m: print("    [scanner] " + m, flush=True))
    got = sc.feed(st.stream)
    exact = compare(mod, got, st.expect, t5, t6)
    if exact != st.nframe:
        bad("只有 %d/%d 帧逐像素正确" % (exact, st.nframe))
    else:
        ok("%d 帧全部：字段/载荷与发出去的逐字节一致，画面逐像素一致"
           % st.nframe)
    print("    resyncs=%d（丢垃圾对齐帧头的次数） bad_headers=%d（没过的候选头）"
          " dropped=%d（丢掉的字节）" % (sc.resyncs, sc.bad_headers, sc.dropped),
          flush=True)
    if sc.dropped != st.garbage:
        bad("丢掉的垃圾字节 %d != 造流时塞进去的 %d" % (sc.dropped, st.garbage))
    else:
        ok("丢掉的字节数正好等于塞进去的垃圾：%d B（分毫不差）" % sc.dropped)
    if sc.resyncs != 4:
        bad("重同步次数 %d != 4（垃圾段：开机日志 / 中段日志 / 坏头 / 尾段日志）"
            % sc.resyncs)
    else:
        ok("重同步 4 次 = 4 段垃圾，一次不多一次不少")
    if sc.buf or sc.pending is not None:
        bad("喂完之后缓冲还有残留：%d 字节，pending=%s"
            % (len(sc.buf), sc.pending))
    else:
        ok("喂完之后没有残留：缓冲空、没有半帧挂着")

    # ---- 3) 整块画布 + 坏帧那一块 ----
    print("\n3) 所有帧贴到同一块 %dx%d 画布上比" % (PANEL_W, PANEL_H), flush=True)
    canvas = Image.new("RGB", (PANEL_W, PANEL_H), (0, 0, 0))
    ref = Image.new("RGB", (PANEL_W, PANEL_H), (0, 0, 0))
    for fr, (want, decodable) in zip(got, st.expect):
        gx, gy, gw, gh, gseq, gflags, gver, payload = fr
        if not decodable:
            continue
        canvas.paste(mod.decode_frame(gver, gflags, gw, gh, payload), (gx, gy))
        ref.paste(Image.frombytes("RGB", (gw, gh),
                                  expect_rgb(want["pix"], gw, gh, t5, t6)),
                  (gx, gy))
    if canvas.tobytes() != ref.tobytes():
        d = [abs(a - b) for pa, pb in zip(canvas.getdata(), ref.getdata())
             for a, b in zip(pa, pb)]
        bad("整块画布和期望差最多 %d（%d 个通道不一致）"
            % (max(d), sum(1 for v in d if v)))
    else:
        ok("整块画布逐像素一致（%d 帧各自落在该落的位置）" % st.nframe)
    hole = canvas.crop((200, 300, 208, 303)).tobytes()
    if hole != bytes(8 * 3 * 3):
        bad("坏帧那块区域被画上了东西：%r" % hole[:12])
    else:
        ok("坏帧那块 (200,300 8x3) 还是纯黑 —— 它被丢掉了，没画上去")
    dfr = got[4]
    dim = mod.decode_frame(dfr[6], dfr[5], dfr[2], dfr[3], dfr[7])
    if dim.tobytes() != expect_rgb(st.frames["D"]["pix"], 20, 8, t5, t6):
        bad("坏帧之后的帧 D 逐像素不一致")
    else:
        ok("坏帧之后的帧 D（20x8 @150,100）逐像素一致 —— 坏帧没影响后续帧")

    # ---- 4) 碎块喂：结论必须一模一样 ----
    print("\n4) 同一条流按不同块长喂，结论必须一模一样", flush=True)
    base = None
    for size in (1, 2, 3, 5, 7, 64, 4096):
        sc2 = mod.FrameScanner()
        out = []
        for i in range(0, len(st.stream), size):
            out += sc2.feed(st.stream[i:i + size])
        sig = (tuple((f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7])
                     for f in out), sc2.resyncs, sc2.bad_headers, sc2.dropped,
               bytes(sc2.buf), sc2.pending)
        if base is None:
            base = sig
            print("    块长 %5d：%d 帧  resyncs=%d bad_headers=%d dropped=%d"
                  % (size, len(out), sc2.resyncs, sc2.bad_headers, sc2.dropped),
                  flush=True)
        elif sig != base:
            bad("块长 %d 的结果和块长 1 不一样（%d 帧 resyncs=%d bad=%d dropped=%d）"
                % (size, len(out), sc2.resyncs, sc2.bad_headers, sc2.dropped))
        else:
            print("    块长 %5d：%d 帧  resyncs=%d bad_headers=%d dropped=%d"
                  "  一模一样" % (size, len(out), sc2.resyncs, sc2.bad_headers,
                                  sc2.dropped), flush=True)
    if len(base[0]) == len(st.expect):
        ok("7 种块长（含一次只喂 1 个字节）解出来的帧序列/载荷/丢字节数完全相同")
    else:
        bad("碎块喂时帧数变了：%d != %d" % (len(base[0]), len(st.expect)))
    if base[3] != st.garbage:
        bad("碎块喂时丢的字节数变了：%d != %d" % (base[3], st.garbage))
    else:
        ok("碎块喂时丢的字节数仍是 %d（和整条一次喂一样）" % base[3])

    # ---- 5) 半帧 / 坏头 / 坏载荷 各自单独演示 ----
    print("\n5) 三件事各自单独演示一遍", flush=True)
    sc3 = mod.FrameScanner()
    if sc3.feed(st.b_half1) or sc3.pending is None:
        bad("只喂帧 B 的前半截就吐帧了（不该）")
    else:
        print("    只喂帧 B 的前 24 字节（头 + 4/9 载荷）-> 0 帧，挂起等载荷",
              flush=True)
    frames_b = sc3.feed(st.b_half2)
    if len(frames_b) != 1 or frames_b[0][4] != 2 or \
            frames_b[0][7] != st.frames["B"]["raw"][20:]:
        bad("补上后半截之后没能完整解出帧 B：%r" % (len(frames_b),))
    else:
        print("    补上剩下 5 字节 -> 帧 B 完整出来（seq=2，载荷 %d 字节）"
              % len(frames_b[0][7]), flush=True)
        ok("半帧不吐、补齐才吐（不会把半截当一帧，也不会错位）")

    sc5 = mod.FrameScanner()
    got5 = sc5.feed(st.bad_hdr + st.parts[6][0])     # 坏头 + 帧 C
    if len(got5) != 1 or got5[0][4] != 3:
        bad("坏头那一段之后没能重同步到帧 C：%d 帧" % len(got5))
    else:
        print("    坏头 + 24 字节垃圾（共 %d 字节）-> 全部丢掉，重同步到帧 C"
              % len(st.bad_hdr), flush=True)
        ok("坏头：逐字节跳过（dropped=%d bad_headers=%d），下一帧照收"
           % (sc5.dropped, sc5.bad_headers))

    sc6 = mod.FrameScanner()
    got6 = sc6.feed(st.bad_payload + st.parts[8][0])     # 坏载荷 + 帧 D
    if len(got6) != 2:
        bad("坏载荷那一段之后帧数不对：%d" % len(got6))
    else:
        try:
            mod.decode_frame(got6[0][6], got6[0][5], got6[0][2], got6[0][3],
                             got6[0][7])
            bad("坏载荷那一帧竟然解码成功了（不该）")
        except ValueError as e:
            print("    坏载荷帧解码报错：%s" % e, flush=True)
        try:
            im = mod.decode_frame(got6[1][6], got6[1][5], got6[1][2],
                                  got6[1][3], got6[1][7])
            good = im.tobytes() == expect_rgb(st.frames["D"]["pix"], 20, 8,
                                              t5, t6)
        except ValueError:
            good = False
        if not good:
            bad("坏载荷之后的帧 D 没能逐像素解对")
        else:
            ok("坏载荷帧只报错丢自己、下一帧 D 逐像素照解（靠板端整屏自愈）")

    # ---- 6) 反向通道：触摸 -> nsh 命令行 ----
    print("\n6) 反向通道（触摸写成 nsh 命令行；不碰真串口，用假口）", flush=True)
    if mod.touch_cmd(12, 34, True) != b"hw_test lcdtap 12 34 1\r\n" or \
            mod.touch_cmd(12, 34, False) != b"hw_test lcdtap 12 34 0\r\n":
        bad("touch_cmd 的文本不对：%r" % mod.touch_cmd(12, 34, True))
    else:
        ok("命令文本：%r / %r"
           % (mod.touch_cmd(12, 34, True), mod.touch_cmd(12, 34, False)))

    rec = mod.SerialReceiver("COMTEST", 1000000, queue.Queue(maxsize=64),
                             lambda m: None)
    fake = FakePort()
    rec.ser = fake
    rec.is_open = True

    rec.send_touch(12, 34, True)
    rec.send_touch(12, 34, False)
    rec._flush_touch()
    # 接收线程写出去的除了触摸，还可能有"命令类"文本（例如开串口时自动发的
    # `hw_test lcdmirror uart`、坏帧后的 `resend`）—— 只看触摸这两条。
    taps = [w for w in fake.written if b"lcdtap" in w]
    if taps != [b"hw_test lcdtap 12 34 1\r\n",
                b"hw_test lcdtap 12 34 0\r\n"]:
        bad("按下/抬起没按原样写出去：%r" % fake.written)
    else:
        ok("按下 (12,34) -> 抬起 (12,34)：两条命令行按顺序写出去")

    fake.written.clear()
    for i in range(200):                     # 200 次拖动事件挤在同一瞬间
        rec.send_touch(10 + i % 5, 20 + i, True)
    rec.send_touch(50, 60, False)
    rec._flush_touch()
    if len(fake.written) != 2:
        bad("同一瞬间的 200 条拖动只该合并成 2 条（1 条移动 + 1 条抬起），实发 %d"
            % len(fake.written))
    else:
        ok("同一瞬间 200 条拖动 -> 合并成 2 条命令（%r + %r）"
           % (fake.written[0].strip(), fake.written[1].strip()))

    fake.written.clear()
    rec.send_touch(1, 1, True)
    rec._flush_touch()
    for i in range(50):
        rec.send_touch(2 + i, 3, True)
    rec.send_touch(9, 9, False)
    rec._flush_touch()                       # 距上一条按下 0ms，正在限流窗口里
    if not fake.written or not fake.written[-1].endswith(b" 0\r\n"):
        bad("限流窗口里的抬起被吞了：%r" % fake.written)
    else:
        ok("距上一条按下 0ms 就抬起：抬起照样立刻发（%r），"
           "只有中间那条拖动被限流" % fake.written[-1].strip())

    fake.written.clear()
    t0 = time.monotonic()
    n = 0
    while time.monotonic() - t0 < 1.0:       # 1 秒里拼命拖动
        rec.send_touch(100 + (n % 40), 200, True)
        rec._flush_touch()
        n += 1
        time.sleep(0.005)
    dt = time.monotonic() - t0
    if not (16 <= len(fake.written) <= 24):
        bad("1 秒拖动（%d 次事件）发了 %d 条命令，不在 ~20 条/秒附近"
            % (n, len(fake.written)))
    else:
        ok("1 秒里 %d 次拖动事件 -> 实发 %d 条命令（%.1f 条/秒，板子只要最新位置）"
           % (n, len(fake.written), len(fake.written) / dt))

    fake.written.clear()
    rec.ser = None                           # 口没了：不该崩、也不该记数
    before = rec.sent_events
    rec.send_touch(5, 6, False)
    rec._flush_touch()
    if rec.sent_events != before:
        bad("口没开的时候还记了发送数")
    else:
        ok("口没开时 _flush_touch 不崩、也不虚报发送数")

    # ---- 7) 串口链路端到端：读线程 -> FrameScanner -> 队列 -> 解码 ----
    print("\n7) 串口链路端到端（假 pyserial，不碰真口）", flush=True)
    written = []
    chunks = []
    i = 0
    rnd = random.Random(4242)
    while i < len(st.stream):                # 按 1..40 字节的碎块喂，模拟真串口
        n = rnd.randint(1, 40)
        chunks.append(st.stream[i:i + n])
        i += n
    q = queue.Queue(maxsize=64)
    logs2 = []
    rec2 = mod.SerialReceiver("COMFAKE", 1000000, q,
                              lambda m: logs2.append(m))
    real_serial = mod.serial
    mod.serial = FakeSerialMod(chunks, written)
    try:
        th = threading.Thread(target=rec2.run, daemon=True)
        th.start()
        deadline = time.monotonic() + 15
        while rec2.total_frames < len(st.expect) and time.monotonic() < deadline:
            time.sleep(0.01)
        got_all = rec2.total_frames
        rec2.send_touch(11, 22, True)
        rec2.send_touch(11, 22, False)
        time.sleep(0.4)
        rec2.stop_flag = True
        th.join(5)
    finally:
        mod.serial = real_serial

    print("    收帧线程：收 %d 字节 / %d 帧，resyncs=%d dropped=%d 跳号 %d"
          % (rec2.total_bytes, got_all, rec2.scanner.resyncs,
             rec2.scanner.dropped, rec2.seq_gaps), flush=True)
    print("    日志：%s" % (logs2[0] if logs2 else "（没有）"), flush=True)
    if got_all != len(st.expect):
        bad("串口收帧线程只收到 %d/%d 帧" % (got_all, len(st.expect)))
    else:
        ok("串口收帧线程把 %d 帧（含 1 个坏帧）全收下来了" % got_all)
    if rec2.seq_gaps != 2:
        bad("跳号统计应为 2（seq=998 的坏帧一进一出），实际 %d" % rec2.seq_gaps)
    else:
        ok("跳号统计 2 次 —— 正是那个 seq=998 的坏帧一进一出，"
           "其它帧 seq 连续（1,2,3,4,5）")
    got2 = []
    while True:
        try:
            kind, payload = q.get_nowait()
        except queue.Empty:
            break
        if kind == "frame":
            got2.append(payload)
    ex2 = compare(mod, got2, st.expect, t5, t6, quiet=True)
    if ex2 != st.nframe:
        bad("走串口链路解出来的只有 %d/%d 帧逐像素对" % (ex2, st.nframe))
    else:
        ok("%d 帧逐像素一致（第 4 帧按 GUI 的做法解码报错丢掉）" % st.nframe)
    canvas2 = Image.new("RGB", (PANEL_W, PANEL_H), (0, 0, 0))
    errs = 0
    for fr in got2:
        fx, fy, fw, fh, _seq, fflags, fver, data = fr
        try:
            im = mod.decode_frame(fver, fflags, fw, fh, data)
        except ValueError:
            errs += 1
            continue                         # 和 GUI 的 pump 一样：报错丢这一帧
        canvas2.paste(im, (fx, fy))
    if errs != 1:
        bad("串口链路上该有 1 帧解码失败，实际 %d" % errs)
    if canvas2.tobytes() != ref.tobytes():
        bad("走串口链路画出来的整块画布和第 3 步的期望不一致")
    else:
        ok("串口链路画出来的整块画布逐像素一致（坏帧没画上去）")
    taps = [w for w in written if b"lcdtap" in w]
    if taps != [b"hw_test lcdtap 11 22 1\r\n", b"hw_test lcdtap 11 22 0\r\n"]:
        bad("串口链路的触摸没写出去：%r" % written)
    else:
        ok("串口链路上触摸也通了：%r（另有 %d 条命令类文本，如自动切换/补发）"
           % ([w.strip() for w in taps], len(written) - len(taps)))

    # ---- 8) 真 GUI 接线：--serial 走串口、不给 --serial 走 TCP，标题有链路名 ----
    print("\n8) 真 GUI 接线（Tk 窗口 --withdraw 起来又关上，不在桌面弹出来）",
          flush=True)
    import argparse
    import contextlib
    import io

    mw = load_client()
    chunks8 = []
    i = 0
    while i < len(st.stream):
        chunks8.append(st.stream[i:i + 97])
        i += 97
    w8 = []
    real_serial8 = mw.serial
    mw.serial = FakeSerialMod(chunks8, w8)
    args = argparse.Namespace(host="127.0.0.1", port=5697, scale=1.0, rotate=0,
                              quiet=True, timing=False, serial="COMFAKE",
                              baud=1000000)
    cap = io.StringIO()
    try:
        with contextlib.redirect_stdout(cap):
            win = mw.MirrorWindow(args)
            win.root.withdraw()
            end = time.monotonic() + 15
            while win.shown_frames < len(st.expect) and time.monotonic() < end:
                win.root.update()
                time.sleep(0.005)
            title = win.root.title()
            status = win.status_var.get()
            shown = win.shown_frames
            transport = type(win.recv).__name__
            img_ok = win.img.tobytes() == ref.tobytes()
            win.on_close()
    finally:
        mw.serial = real_serial8
    print("    窗口标题：%s" % title, flush=True)
    print("    底部状态：%s" % status, flush=True)
    if transport != "SerialReceiver":
        bad("给了 --serial 却用了 %s" % transport)
    else:
        ok("--serial COMFAKE 走的是 SerialReceiver（不是 TCP 服务端）")
    if "串口 COMFAKE" not in title:
        bad("标题里没显示串口链路：%r" % title)
    else:
        ok("标题里显示了当前传输：含 %r" % "串口 COMFAKE @1000000")
    if "串口 COMFAKE" not in status:
        bad("底部状态行里没显示串口链路：%r" % status)
    else:
        ok("底部状态行里也显示了当前传输")
    if shown != len(st.expect) or not img_ok:
        bad("GUI 只画了 %d 帧 / 画面不一致：%s" % (shown, img_ok))
    else:
        ok("串口模式下 GUI 画出来的整块画布逐像素一致（坏帧那一帧被报错丢掉）")
    if "解码这一帧失败" not in cap.getvalue():
        bad("GUI 没有对坏帧报错：%r" % cap.getvalue()[-200:])
    else:
        ok("GUI 对坏帧只打一行「解码这一帧失败」，不影响后面的帧")

    args_tcp = argparse.Namespace(host="127.0.0.1", port=5697, scale=1.0,
                                  rotate=0, quiet=True, timing=False)
    with contextlib.redirect_stdout(io.StringIO()):
        win_tcp = mw.MirrorWindow(args_tcp)     # 老自测就是这么捏 Namespace 的
        tcp_transport = type(win_tcp.recv).__name__
        win_tcp.on_close()
    if tcp_transport != "Receiver":
        bad("不给 --serial 时没用 TCP Receiver，而是 %s" % tcp_transport)
    else:
        ok("不给 --serial（连 serial/baud 字段都没有的老 Namespace）仍走 TCP "
           "Receiver —— 老调用方没被破坏")

    print()
    if FAILS:
        print("有 %d 项没过：" % len(FAILS))
        for f in FAILS:
            print("  - " + f)
    else:
        print("PASS：重同步 + 坏帧隔离 + 坏帧后逐像素正确 + 碎块一致 + "
              "触摸合并/限流")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
