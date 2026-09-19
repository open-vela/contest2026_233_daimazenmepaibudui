#!/usr/bin/env python3.10
# -*- coding: utf-8 -*-
"""
lcd_mirror.py —— 把开发板的屏幕内容实时显示到电脑上，并用鼠标模拟板子的触摸

用法（Windows，Pillow 已装的话；只依赖 Pillow + tkinter 标准库）：

    py -3.10 D:/apply/claw/_flash/lcd_mirror.py

    常用参数：
        --port 5600          监听端口（默认 5600，必须和板端一致）
        --host 0.0.0.0       监听地址（默认全开；只想收本机就 127.0.0.1）
        --scale 1.5          窗口放大倍数（默认 1.0，屏只有 390x450，想看清字可放大）
        --rotate 0           显示旋转 0/90/180/270（默认 0；板端不做旋转）
        --quiet              不每秒打一行流量统计
        --timing             每秒多打一行"解码 / 重画各多少 ms"（窗口里按 t 也能开关）

    串口模式（网络/USB RNDIS 反复挂的时候走这条 —— 串口是唯一没出过问题的链路）：

        py -3.10 D:/apply/claw/_flash/lcd_mirror.py --serial COM4 --baud 1000000
        py -3.10 D:/apply/claw/_flash/lcd_mirror.py --serial COM4 --scale 1.5

        帧格式和 TCP 那条一模一样（20 字节 v2 头 + 载荷），区别只在链路上：
          * 串口里**混着 nsh 提示符和内核日志文本**（板子同一个 console 口既打
            日志又吐帧），所以脚本按 magic 'L','M' 逐字节扫描重同步：候选头必须
            ver==2、flags 只有 bit0、x+w<=390、y+h<=450、payload_len 合理
            （原样帧 plen==w*h*2；RLE 帧是 3 的倍数、且长度能盖住 w*h 个像素）；
            不合格就丢掉这**一个字节**、从下一个字节继续扫。坏帧只丢自己，
            绝不影响后面的帧 —— 板端每 3 秒补一帧整屏关键帧自愈。
          * 鼠标触摸不再发 8 字节二进制，而是往串口写一条 nsh 命令行：
            `hw_test lcdtap <x> <y> <1|0>`（拖动最多 ~20 条/秒；抬起立刻发、
            绝不被合并掉）。
          * **不再 bind 5600**，TCP 那条路完全不碰。串口 / TCP 二选一：
            不给 --serial 就是原来的 TCP 服务端。
          * 串口会随 USB 重枚举"消失又回来"，所以打开失败 / 读失败都只是每 2 秒
            重试，不当致命错误（恢复路径不加门）。
          * 需要 pyserial（`py -3.10 -m pip install pyserial`）；没装也不影响
            TCP 模式。

    界面上：
        按住鼠标左键 = 手指按下（拖动 = 滑动，可以划出主菜单）
        松开        = 抬起
        空格 = 暂停显示，t = 开关计时打印，Esc = 退出

    ⚠ 本机触摸 IC(FT6146) 已经不应答、/dev/input0 都没了，**这条鼠标通道就是
      现场唯一能操作界面的路**。它不依赖 /dev/input0，将来真触摸回来了两者可以同时用。

板端（默认已经在跑了）：
    `hw_test lcdmirror status`            看状态（会打印收到多少触摸事件、最后坐标）
    `hw_test lcdmirror stop / start`      停 / 起（停掉 = 连鼠标注入一起停）
    `hw_test lcdmirror start 192.168.137.1 5600`   换目标再起

协议（和板端 board/contest_board/src/lcd_mirror.h 严格一致），两种版本都收：

    板 -> PC，每帧一个小端头 + 载荷：

      v1（16 字节头，板子上现在跑的还是这个）：
        u8  magic0='L'  u8 magic1='M'  u8 ver=1  u8 flags=0
        u16 x  u16 y  u16 w  u16 h        (LE)
        u32 seq                           (LE)
        之后紧跟 w*h*2 字节 RGB565 小端、行优先、无行间填充。

      v2（20 字节头，多一个 payload_len）：
        u8  magic0='L'  u8 magic1='M'  u8 ver=2  u8 flags
        u16 x  u16 y  u16 w  u16 h        (LE)
        u32 seq  u32 payload_len           (LE)
        flags bit0=1 时，紧跟 payload_len 字节的 RLE 流：
          重复 { u8 run(1..255), u16 pixel_le }，一共覆盖 w*h 个像素；
        flags bit0=0 时就是原样的 w*h*2 字节 RGB565（payload_len = w*h*2）。

    帧可以是整屏（0,0,390,450），也可以是任意脏矩形 —— 本脚本按矩形贴到画布上。

    PC -> 板，**同一条 TCP 连接**上反向发，固定 8 字节：
        u8  magic0='L'  u8 magic1='T'  u8 ver=1  u8 pressed
        u16 x  u16 y                      (LE，面板坐标 0..389 / 0..449)
    鼠标在窗口里的坐标会先按"旋转 + 缩放"反算回面板坐标（见 widget_to_panel）。

窗口标题里的数：
    fps=当前接收帧率   #seq=帧序号（跳号说明"板子发了但没收到/没收全"）
    分辨率 / 缩放 / 累计字节 / 已发触摸事件数（底部那一行是触摸的详细状态）

连接断了会自动回到监听状态（板子那边每 2.5 秒重连一次）。
"""

import argparse
import struct
import socket
import sys
import threading
import time
import queue

try:
    import tkinter as tk
    from PIL import Image, ImageTk
except ImportError as e:  # pragma: no cover
    print("需要 Pillow 和 tkinter：", e)
    print("装 Pillow：py -3.10 -m pip install pillow")
    sys.exit(2)

# 串口模式（--serial）才需要 pyserial；没装也不影响 TCP 那条路，
# 所以这里不退出，等真要用串口时再报一声。
try:
    import serial
    import serial.tools.list_ports
except ImportError:  # pragma: no cover
    serial = None


# ---------------------------------------------------------------- 协议常量

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

# 反向通道：鼠标 -> 板子的触摸，固定 8 字节（和板端 lcd_mirror.h 严格一致）
TOUCH_MAGIC0 = ord("L")
TOUCH_MAGIC1 = ord("T")
TOUCH_VER = 1
TOUCH_FMT = struct.Struct("<BBBBHH")    # 4 + 2 + 2 = 8 字节
assert TOUCH_FMT.size == 8

# 一帧最多允许多少字节（防坏包把内存吃光）：4096x4096x2 已经远大于 390x450x2
MAX_FRAME_BYTES = 8 * 1024 * 1024
MAX_DIM = 4096

DEFAULT_W, DEFAULT_H = 390, 450


# ------------------------------------------------------- 显示 / 坐标映射
#
# 屏幕上的样子是：面板图 --rotate(expand)--> 旋转后的图 --resize(scale)--> 窗口里的图。
# 鼠标点的是窗口里的图，要反算回面板坐标，就得把这两个变换各反一遍。
#
# 下面的 panel_to_shown / shown_to_panel 是**实测出来**的，不是推导的：
# 用一张每个像素颜色唯一的合成图喂给 PIL 的 rotate(expand=True)，
# 看每个源像素落到哪个目标像素（见 _lcd_mirror_host_test/roundtrip_check.py）。
#   0°  : src(x,y) -> (x, y)                 目标尺寸 (W, H)
#   90° : src(x,y) -> (y, W-1-x)             目标尺寸 (H, W)
#   180°: src(x,y) -> (W-1-x, H-1-y)         目标尺寸 (W, H)
#   270°: src(x,y) -> (H-1-y, x)             目标尺寸 (H, W)
# 缩放那一步 PIL 用最近邻：目标像素 d 取源像素 floor((d+0.5) * W1 / W2)。
# 反算用同一个式子；它跟 PIL 内部取整在个别边界上可能差 1 个像素
# （scale=1.0 时两者都退化成恒等，一像素都不差 —— 默认就是 1.0）。


def shown_size(panel_w, panel_h, rotate):
    """旋转之后、还没缩放时的图尺寸。"""
    if rotate in (90, 270):
        return panel_h, panel_w
    return panel_w, panel_h


def panel_to_shown(x, y, panel_w, panel_h, rotate):
    """面板坐标 -> 旋转后的图上的坐标。"""
    if rotate == 90:
        return y, panel_w - 1 - x
    if rotate == 180:
        return panel_w - 1 - x, panel_h - 1 - y
    if rotate == 270:
        return panel_h - 1 - y, x
    return x, y


def shown_to_panel(rx, ry, panel_w, panel_h, rotate):
    """旋转后的图上的坐标 -> 面板坐标（panel_to_shown 的逆）。"""
    if rotate == 90:
        return panel_w - 1 - ry, rx
    if rotate == 180:
        return panel_w - 1 - rx, panel_h - 1 - ry
    if rotate == 270:
        return ry, panel_h - 1 - rx
    return rx, ry


def render_display(img, rotate, scale):
    """面板图 -> 窗口里那张图。GUI 和离板测试都用这一个函数，保证测的就是跑的。"""
    im = img
    if rotate:
        im = im.rotate(rotate, expand=True)
    if scale != 1.0:
        im = im.resize((max(1, int(im.width * scale)),
                        max(1, int(im.height * scale))), Image.NEAREST)
    return im


def widget_to_panel(wx, wy, panel_w, panel_h, rotate, scale):
    """窗口里的坐标（canvas 左上角为原点）-> 面板坐标。

    越界的点钳到面板范围内（拖到窗口外面也照样是一次合法触摸）。
    """
    sw, sh = shown_size(panel_w, panel_h, rotate)
    dw = max(1, int(sw * scale))
    dh = max(1, int(sh * scale))

    rx = int((wx + 0.5) * (sw / dw))
    ry = int((wy + 0.5) * (sh / dh))

    rx = 0 if rx < 0 else (sw - 1 if rx >= sw else rx)
    ry = 0 if ry < 0 else (sh - 1 if ry >= sh else ry)

    x, y = shown_to_panel(rx, ry, panel_w, panel_h, rotate)
    x = 0 if x < 0 else (panel_w - 1 if x >= panel_w else x)
    y = 0 if y < 0 else (panel_h - 1 if y >= panel_h else y)
    return x, y


# ------------------------------------------- 串口重同步（板端 UART 那条链路）
#
# 串口里帧流**和 nsh 提示符、内核日志文本混在同一条线上**，所以不能像 TCP 那样
# "顺着从头读"：得自己找帧头。做法：
#   * 按 magic 'L','M' 逐字节扫描，拿到一个候选头就用 check_header 校验；
#   * 不合格就把这**一个字节**丢掉、从下一个字节继续扫（绝不整段丢）；
#   * 头合格但载荷坏掉（RLE 没覆盖满等），也只丢这一帧，后面的帧照收；
#   * 板端每 3 秒补发一帧整屏关键帧，所以丢了任何一帧都会自己长回来。
# 这里刻意**不加门**：没有"连续 N 帧不合格就放弃 / 就重连"之类的判断。

PANEL_MAX_X = DEFAULT_W      # 帧矩形不许超出面板：x + w <= 390
PANEL_MAX_Y = DEFAULT_H      #                        y + h <= 450


def check_header(m0, m1, ver, flags, x, y, w, h, seq, plen):
    """候选帧头合不合格。合格返回 None，不合格返回一句"哪里不对"。

    这是串口重同步的唯一判据。日志文本里凑巧出现 'L','M' 两个字节是常事，
    但紧接着的第 3 个字节就是 ver —— ASCII 文本里几乎不可能是 0x02，再叠上
    flags 的已知位、几何范围、载荷长度这几道，就足够"宁可错杀一个候选头，
    也不把日志字节当成帧"。
    """
    if m0 != MAGIC0 or m1 != MAGIC1:
        return "magic"
    if ver not in (1, 2):
        return "ver=%d" % ver
    if flags & ~FLAG_RLE:
        return "flags=%#x" % flags
    if w == 0 or h == 0 or w > MAX_DIM or h > MAX_DIM:
        return "尺寸 %dx%d" % (w, h)
    if x + w > PANEL_MAX_X or y + h > PANEL_MAX_Y:
        return "越界 x+w=%d y+h=%d" % (x + w, y + h)
    npix = w * h
    if npix * 2 > MAX_FRAME_BYTES or plen > MAX_FRAME_BYTES:
        return "一帧太大 payload_len=%d" % plen
    if ver >= 2:
        if flags & FLAG_RLE:
            # RLE 流是 3 字节一条记录：长度必须是 3 的倍数；
            # 下界 = 每条 run 都吃满 255 个像素，上界 = 每像素一条 run=1
            if plen < 3 or plen % 3 or plen > 3 * npix:
                return "RLE 长度 %d 不合理" % plen
            if (plen // 3) * 255 < npix:
                return "RLE 长度 %d 盖不住 %d 个像素" % (plen, npix)
        elif plen != npix * 2:
            return "原样帧长度 %d != w*h*2=%d" % (plen, npix * 2)
    return None


class FrameScanner:
    """字节流 -> 完整帧。串口用它；自测里也能脱离窗口单独调。

    每项的字段顺序和 Receiver 塞进队列的完全一样：
        (x, y, w, h, seq, flags, ver, payload)
    半帧（头齐了、载荷还没攒够）留在内部缓冲里，等后面的字节补齐。
    """

    def __init__(self, log=None):
        self.buf = bytearray()
        self.log = log or (lambda _m: None)
        self.pending = None      # 头已解出来、正在等载荷的字段元组
        self.resyncs = 0         # "丢垃圾才对上帧头"的次数（= 夹在帧之间的垃圾段数）
        self.dropped = 0         # 一共丢掉多少个字节
        self.bad_headers = 0     # 校验没过的候选头个数
        self.in_garbage = False  # 上一步是不是在丢垃圾（用来数"段"而不是"字节"）

    def _drop(self, n):
        """丢掉缓冲开头 n 个字节（就是"这个字节不合格，跳过它"）。"""
        if n <= 0:
            return
        del self.buf[:n]
        self.dropped += n
        self.in_garbage = True

    def _scan(self):
        """找一个合格的帧头。返回 (偏移, 头长, (x,y,w,h,seq,flags,ver,plen))。

        找不到就把"可能是半截头"的尾巴留下（最多 20-1 = 19 字节），其余当垃圾
        丢掉 —— 所以缓冲不会跟着日志文本一起无限涨。留下的 19 字节这个数是有讲究
        的：v2 头 20 字节，任何长度 <= 19 的残头都必然整个落在最后 19 字节里。
        """
        buf = self.buf
        n = len(buf)
        i = 0
        while i + HDR_V1_SIZE <= n:
            if buf[i] != MAGIC0 or buf[i + 1] != MAGIC1:
                i += 1
                continue
            if i + HDR_V2_SIZE > n:
                break            # 还看不出这是 v1 还是 v2，等更多字节
            hs, m0, m1, ver, flags, x, y, w, h, seq, plen = \
                parse_header(bytes(buf[i:i + HDR_V2_SIZE]))
            why = check_header(m0, m1, ver, flags, x, y, w, h, seq, plen)
            if why is None:
                return i, hs, (x, y, w, h, seq, flags, ver, plen)
            self.bad_headers += 1
            i += 1               # 这一个字节不合格 -> 从下一个字节接着扫
        if n > HDR_V2_SIZE - 1:
            self._drop(n - (HDR_V2_SIZE - 1))
        return None

    def feed(self, chunk):
        """喂一段字节，返回这次新解出来的完整帧列表。

        任意切块（包括一次一个字节）喂进来，解出来的帧序列、丢掉的字节数、
        重同步次数都完全一样 —— 真实串口 read() 回来的就是任意长度的碎块。
        """
        if chunk:
            self.buf += chunk
        out = []
        while True:
            if self.pending is None:
                fr = self._scan()
                if fr is None:
                    break
                off, hs, fields = fr
                if off:
                    self._drop(off)
                if self.in_garbage:
                    # 从"在丢垃圾"变成"对上帧头"，这才算一次重同步 ——
                    # 按段数数（不是按丢了多少字节数），切块方式就不影响它了
                    self.resyncs += 1
                    self.in_garbage = False
                del self.buf[:hs]
                self.pending = fields
            else:
                plen = self.pending[7]
                if len(self.buf) < plen:
                    break
                payload = bytes(self.buf[:plen])
                del self.buf[:plen]
                x, y, w, h, seq, flags, ver, _plen = self.pending
                self.pending = None
                out.append((x, y, w, h, seq, flags, ver, payload))
        return out


# ---------------------------------------------------------------- 接收线程

class Receiver(threading.Thread):
    """TCP server：等板子连上来，收帧，塞进队列交给 Tk 主线程画。"""

    def where_text(self):
        """标题 / 状态行里显示"当前这条链路在哪"。"""
        if self.peer:
            return "TCP %s:%d" % (self.peer[0], self.peer[1])
        return "TCP 端口 %d（等待连接…）" % self.port

    def __init__(self, host, port, out_queue, log):
        super().__init__(name="lcd-mirror-recv", daemon=True)
        self.host = host
        self.port = port
        self.out = out_queue
        self.log = log
        self.stop_flag = False
        self.peer = None
        self.total_bytes = 0
        self.total_frames = 0
        self.last_seq = None
        self.seq_gaps = 0

        # 待发（反向通道：鼠标 -> 板子触摸）。GUI 线程只往这里塞，
        # 真正写 socket 的是本线程 —— 一个 socket 一个线程写，不抢。
        self.tx_lock = threading.Lock()
        self.txq = []            # [(x, y, pressed), ...]
        self.sent_events = 0
        self.last_touch = None   # 最后一次发出去的 (x, y, pressed)
        self.send_errors = 0

    # -- 反向通道 -------------------------------------------------------

    def send_touch(self, x, y, pressed):
        """GUI 线程调用：排一条触摸事件。**不碰 socket、不会阻塞 GUI**。

        连续拖动会产生大量同类型事件，而板子只关心"最新状态"，
        所以队列里最后一条还是按下时就直接把它替换掉（合并），
        不合并的话一秒钟能排上百条、白占内存。
        抬起（pressed=0）永远单独排队，绝不被合并/丢弃。
        """
        ev = (int(x), int(y), 1 if pressed else 0)
        with self.tx_lock:
            if self.txq and self.txq[-1][2] == 1 and ev[2] == 1:
                self.txq[-1] = ev
            else:
                self.txq.append(ev)
            # 兜底：socket 堵住时不让队列无限长（丢的是老的"按下"，
            # 最新的状态永远留着）
            if len(self.txq) > 64:
                del self.txq[:len(self.txq) - 64]

    def _flush_touch(self, sock):
        """接收线程调用：把排队的触摸事件写出去（socket 非阻塞）。"""
        with self.tx_lock:
            pending, self.txq = self.txq, []

        for x, y, pressed in pending:
            msg = TOUCH_FMT.pack(TOUCH_MAGIC0, TOUCH_MAGIC1, TOUCH_VER,
                                 pressed, x, y)
            try:
                sock.sendall(msg)
            except OSError as e:
                self.send_errors += 1
                self.log(f"反向通道写失败：{e}")
                raise
            self.sent_events += 1
            self.last_touch = (x, y, pressed)

    def _serve(self, sock):
        peer = sock.getpeername()
        self.peer = peer
        self.last_seq = None
        self.log(f"板子已连接：{peer[0]}:{peer[1]}")
        self.out.put(("connect", peer))

        # 一次收一小段就回来处理发送，这样反向的触摸消息最多等 20ms
        sock.settimeout(0.02)

        buf = bytearray()
        pending = None           # 已经解出头、正在等像素的那一帧
        while not self.stop_flag:
            # ---- 1) 收：非阻塞地读一点 ----
            try:
                chunk = sock.recv(65536)
                if not chunk:
                    return
                buf += chunk
            except socket.timeout:
                pass
            except OSError:
                return

            # ---- 2) 解析出所有完整的帧 ----
            while True:
                if pending is None:
                    if len(buf) < HDR_V1_SIZE:
                        break
                    hs = header_size(buf[2])
                    if len(buf) < hs:
                        break
                    hs, m0, m1, ver, flags, x, y, w, h, seq, plen = \
                        parse_header(buf)
                    if m0 != MAGIC0 or m1 != MAGIC1 or ver not in (1, 2):
                        self.log(f"坏头 -> 断开重来 (magic={m0:#x},{m1:#x} "
                                 f"ver={ver} flags={flags:#x})")
                        return
                    if w == 0 or h == 0 or w > MAX_DIM or h > MAX_DIM:
                        self.log(f"尺寸不合理 {w}x{h} -> 断开重来")
                        return
                    nbytes = w * h * 2
                    if nbytes > MAX_FRAME_BYTES or plen > MAX_FRAME_BYTES:
                        self.log(f"一帧 {plen} 字节太大（原样 {nbytes}）"
                                 f" -> 断开重来")
                        return
                    if ver >= 2 and not (flags & FLAG_RLE) and plen != nbytes:
                        # 只报一声，不踢连接：宁可这一帧画歪，也不给"恢复"加门
                        self.log(f"v2 原样帧 payload_len={plen} 和 w*h*2={nbytes} "
                                 f"对不上，按收到的长度继续")
                    del buf[:hs]
                    pending = (x, y, w, h, seq, plen, flags, ver, hs)
                else:
                    x, y, w, h, seq, plen, flags, ver, hs = pending
                    if len(buf) < plen:
                        break
                    payload = bytes(buf[:plen])
                    del buf[:plen]
                    pending = None

                    if self.last_seq is not None and seq != (self.last_seq + 1) & 0xFFFFFFFF:
                        self.seq_gaps += 1
                    self.last_seq = seq
                    self.total_frames += 1
                    self.total_bytes += hs + plen

                    # 队列满了就丢最老的（GUI 卡了也不至于卡住接收线程）
                    item = ("frame", (x, y, w, h, seq, flags, ver, payload))
                    try:
                        self.out.put_nowait(item)
                    except queue.Full:
                        try:
                            self.out.get_nowait()
                        except queue.Empty:
                            pass
                        try:
                            self.out.put_nowait(item)
                        except queue.Full:
                            pass

            # ---- 3) 发：把排队的触摸事件写回去 ----
            self._flush_touch(sock)

    def run(self):
        srv = None
        try:
            srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.bind((self.host, self.port))
            srv.listen(1)
            srv.settimeout(0.5)
        except OSError as e:
            self.log(f"监听 {self.host}:{self.port} 失败：{e}")
            self.out.put(("fatal", str(e)))
            return

        self.log(f"监听 {self.host}:{self.port}，等板子连上来"
                 f"（板子 192.168.137.2，开机自动连）")

        while not self.stop_flag:
            try:
                conn, _ = srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break

            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            conn.settimeout(5.0)
            try:
                self._serve(conn)
            except (OSError, socket.timeout) as e:
                self.log(f"连接中断：{e}")
            finally:
                try:
                    conn.close()
                except OSError:
                    pass
                self.peer = None
                self.out.put(("disconnect", None))
                if not self.stop_flag:
                    self.log("回到监听状态，等板子重连（板端每 2.5 秒试一次）")

        try:
            srv.close()
        except OSError:
            pass


# ---------------------------------------------------------------- 串口接收

SERIAL_BAUD_DEFAULT = 1000000
SERIAL_RETRY_S = 2.0          # 串口打不开 / 断了之后的重新打开间隔
TOUCH_MOVE_HZ = 20            # 拖动时最多每秒 20 条 nsh 触摸命令
CMD_MIN_GAP_S = 0.25          # 往串口写"命令类"文本的最小间隔（每条都要唤醒一次 NSH）
                              # 例如坏帧后发的 `hw_test lcdmirror resend <y0> <rows>`


def touch_cmd(x, y, pressed):
    """串口模式的反向通道：一条 nsh 命令行（不是 TCP 那 8 字节二进制）。

    板端 `hw_test lcdtap <x> <y> <1|0>` 注入一次触摸（1=按下，0=抬起），
    坐标是面板坐标。末尾必须 \r\n —— nsh 收到回车才执行。
    """
    return ("hw_test lcdtap %d %d %d\r\n"
            % (int(x), int(y), 1 if pressed else 0)).encode("ascii")


class SerialReceiver(threading.Thread):
    """串口收帧，并把触摸以 nsh 命令行写回**同一个口**。

    和 Receiver（TCP）的区别只有三处：
      * 帧流里混着 nsh / 内核日志文本 -> 用 FrameScanner 边扫边重同步；
      * 反向的触摸写的是 nsh 命令行，不是 8 字节二进制；
      * 串口会随 USB 重枚举而"消失又回来"，打开失败 / 读失败都只是重试，
        不当致命错误（恢复路径不加门）。
    对外暴露的字段（total_bytes / total_frames / seq_gaps / sent_events /
    last_touch / send_errors / stop_flag / peer）和 Receiver 一一对应，
    GUI 那边不用分叉。
    """

    def __init__(self, port_name, baud, out_queue, log, raw_log=None):
        super().__init__(name="lcd-mirror-serial", daemon=True)
        self.port_name = port_name
        self.baud = baud
        self.out = out_queue
        self.log = log
        self.raw_log = raw_log        # 原始字节流的落盘路径（None = 不存）
        self.raw_fp = None
        self.stop_flag = False
        self.ser = None                # 打开的串口对象；没开就是 None
        self.is_open = False
        self.peer = None               # 串口没有对端地址，留着只为和 Receiver 同形状
        self.total_bytes = 0
        self.total_frames = 0
        self.last_seq = None
        self.seq_gaps = 0
        self.scanner = FrameScanner(log)
        self.reopen_errors = 0

        # 待发（反向通道：鼠标 -> 板子触摸）。GUI 线程只往这里塞，
        # 真正写串口的是本线程 —— 一个口一个线程写，不抢。
        self.tx_lock = threading.Lock()
        self.txq = []
        self.cmdq = []                 # 要板子执行的 nsh 命令行（精确补发请求等）
        self.last_cmd_t = 0.0
        self.sent_events = 0
        self.last_touch = None         # 最后一次发出去的 (x, y, pressed)
        self.last_press_t = 0.0
        self.send_errors = 0

    # -- 显示用的名字 ---------------------------------------------------

    def where_text(self):
        if self.is_open:
            return "串口 %s @%d" % (self.port_name, self.baud)
        return "串口 %s 未打开" % self.port_name

    @staticmethod
    def _port_list():
        """打开失败时把现成的口列出来 —— 十次有九次是 COM 号写错了。"""
        try:
            return ", ".join(p.device for p in
                             serial.tools.list_ports.comports()) or "（一个都没有）"
        except Exception:              # noqa: BLE001
            return "（列不出来）"

    # -- 反向通道：触摸 -> nsh 命令行 ------------------------------------

    def send_touch(self, x, y, pressed):
        """GUI 线程调用：排一条触摸。**不碰串口、不会阻塞 GUI**。

        连续拖动会产生大量同类型事件，而板子只关心"最新状态"，
        所以队列尾已经是按下时就直接替换掉（合并）。
        抬起（pressed=0）永远单独排队，绝不被合并 / 丢弃。
        """
        ev = (int(x), int(y), 1 if pressed else 0)
        with self.tx_lock:
            if self.txq and self.txq[-1][2] == 1 and ev[2] == 1:
                self.txq[-1] = ev
            else:
                self.txq.append(ev)
            # 兜底：口堵住时不让队列无限长（丢的是老的"按下"，
            # 最新的状态永远留着）
            if len(self.txq) > 64:
                del self.txq[:len(self.txq) - 64]

    def send_cmd(self, line):
        """GUI 线程调用：排一条要板子执行的 nsh 命令行（例如"这条带子坏了，补发"）。

        不碰串口、不会阻塞 GUI；真正写口的是接收线程（_flush_touch 里一起 drain）。
        """
        with self.tx_lock:
            self.cmdq.append(line)
            # 兜底：口堵住时不留无限长（只保留最新的几条）
            if len(self.cmdq) > 16:
                del self.cmdq[:len(self.cmdq) - 16]

    def _flush_touch(self):
        """接收线程调用：把排队的触摸写成 nsh 命令行，拖动按 ~20 条/秒限流。

        抬起永远立刻发、绝不被限流丢掉 —— 丢一条抬起，板子那边就永远停在
        "按着"，界面等于卡死。
        """
        with self.tx_lock:
            pending, self.txq = self.txq, []
            cmds, self.cmdq = self.cmdq, []
        if not pending and not cmds:
            return
        if self.ser is None:
            return
        now = time.monotonic()

        # 命令队列（精确补发这类请求）：每条都会唤醒一次 NSH，所以限流比触摸更狠；
        # 同一批里只发最新一条 —— 旧的补发请求早被后面那条覆盖了。
        if cmds and (now - self.last_cmd_t) >= CMD_MIN_GAP_S:
            try:
                self.ser.write((cmds[-1] + "\r\n").encode("ascii"))
                self.ser.flush()
                self.last_cmd_t = now
            except Exception as e:         # noqa: BLE001（pyserial 各种口味）
                self.send_errors += 1
                self.log("串口写命令失败：%s" % e)

        for x, y, pressed in pending:
            if (pressed and self.last_touch is not None
                    and self.last_touch[2] == 1
                    and (now - self.last_press_t) < 1.0 / TOUCH_MOVE_HZ):
                # 拖动限流：板子只要最新位置；被跳过的坐标由后面那条抬起
                # 或者下一次合并一起带过去
                continue
            try:
                self.ser.write(touch_cmd(x, y, pressed))
                self.ser.flush()
            except Exception as e:         # noqa: BLE001（pyserial 各种口味）
                self.send_errors += 1
                self.log("串口写触摸失败：%s" % e)
                return
            self.sent_events += 1
            self.last_touch = (x, y, pressed)
            if pressed:
                self.last_press_t = now

    # -- 收帧 -----------------------------------------------------------

    def _emit(self, fr):
        """一帧 -> 队列（满了就丢最老的，别让串口那边等 GUI）。"""
        x, y, w, h, seq, flags, ver, payload = fr
        if self.last_seq is not None and seq != (self.last_seq + 1) & 0xFFFFFFFF:
            self.seq_gaps += 1
        self.last_seq = seq
        self.total_frames += 1
        item = ("frame", (x, y, w, h, seq, flags, ver, payload))
        try:
            self.out.put_nowait(item)
        except queue.Full:
            try:
                self.out.get_nowait()
            except queue.Empty:
                pass
            try:
                self.out.put_nowait(item)
            except queue.Full:
                pass

    def _read_loop(self, ser):
        """口开着的时候一直跑：读一点 -> 刮帧 -> 排队；再把触摸写回去。

        read 带 0.02 秒超时，所以一次收一小段就回来处理发送 ——
        反向的触摸最多等 20ms。
        """
        while not self.stop_flag:
            try:
                chunk = ser.read(65536)
            except Exception as e:             # noqa: BLE001
                self.log("串口读失败：%s（多半是 USB 重枚举把口抽走了），重开" % e)
                return
            if chunk:
                self.total_bytes += len(chunk)
                if self.raw_log is not None:
                    # 顺带把原始流存一份：串口模式下控制台文本会被帧字节淹没，
                    # 存下来才能 grep 板子自己的日志（诊断时才开）。
                    try:
                        if self.raw_fp is None:
                            self.raw_fp = open(self.raw_log, "ab")
                        self.raw_fp.write(chunk)
                        self.raw_fp.flush()
                    except Exception:      # noqa: BLE001
                        self.raw_log = None
                for fr in self.scanner.feed(chunk):
                    self._emit(fr)
            self._flush_touch()

    def _sleep_retry(self):
        """等一会儿再重开。返回 True = 已经被要求停（别再重试了）。"""
        end = time.monotonic() + SERIAL_RETRY_S
        while time.monotonic() < end:
            if self.stop_flag:
                return True
            time.sleep(0.05)
        return self.stop_flag

    def run(self):
        if serial is None:
            msg = ("没装 pyserial，串口模式用不了（TCP 模式不受影响）。"
                   "装它：py -3.10 -m pip install pyserial")
            self.log(msg)
            self.out.put(("fatal", msg))
            return

        while not self.stop_flag:
            try:
                ser = serial.Serial(self.port_name, self.baud, timeout=0.02,
                                    write_timeout=1.0,
                                    rtscts=False, dsrdtr=False, xonxoff=False)
            except Exception as e:             # noqa: BLE001
                self.reopen_errors += 1
                self.is_open = False
                self.log("打开串口 %s 失败：%s；%.0f 秒后重试。现成的口：%s"
                         % (self.port_name, e, SERIAL_RETRY_S, self._port_list()))
                self.out.put(("disconnect", None))
                if self._sleep_retry():
                    return
                continue

            self.ser = ser
            self.is_open = True

            # 接收缓冲开大：板子会一口气推几千字节的带子，而 GUI 重画那一下会占住
            # GIL、读线程被饿住 —— Windows 默认 4096 字节的接收缓冲一满就丢字节，
            # 整帧被截断（实测整屏关键帧因此批量报废、下半屏长期黑）。开大之后
            # 同样的停顿不再丢字节。老 pyserial 没这个 API，失败就算了。
            try:
                ser.set_buffer_size(rx_size=1 << 18, tx_size=1 << 12)
            except Exception:              # noqa: BLE001
                pass
            self.scanner = FrameScanner(self.log)   # 新的一段流，从零开始扫
            self.last_seq = None
            self.out.put(("connect", self.where_text()))
            self.log("串口已开：%s @ %d baud（帧流里混着 nsh / 内核日志，"
                     "脚本自己找帧头）" % (self.port_name, self.baud))

            # **由本端发起切换**：板子开机默认 TCP（控制台干净），这里替用户发一条
            # `hw_test lcdmirror uart` —— 板子会切到串口传输并把整屏标脏重发。
            # 为什么不由板子开机就默认串口：镜像一旦把控制台 UART 的 TX 环形缓冲占满，
            # nsh 的 write() 就永远排不上队，连 `hw_test lcdmirror stop` 都发不进去，
            # 控制台等于死了（2026-09-17 真机踩到）。切换放这边，进退都可控。
            try:
                ser.write(b"hw_test lcdmirror uart\r\n")
                ser.flush()
            except Exception as e:         # noqa: BLE001
                self.log("发切换命令失败（板子可能还是 TCP 模式）：%s" % e)
            try:
                self._read_loop(ser)
            finally:
                self._flush_touch()        # 停之前把最后那条抬起发出去
                self.is_open = False
                self.ser = None
                try:
                    ser.close()
                except Exception:          # noqa: BLE001
                    pass
                self.out.put(("disconnect", None))
                if not self.stop_flag:
                    self.log("串口 %s 断了，重开" % self.port_name)


# ---------------------------------------------------------------- GUI

class MirrorWindow:
    def __init__(self, args):
        self.args = args
        self.frames = queue.Queue(maxsize=64)
        # --serial 时走串口那条链路（不再 bind 任何端口）；否则就是原来的 TCP 服务端。
        # 用 getattr：老的自测脚本直接捏一个 Namespace 进来，没有 serial/baud 这两个字段
        self.serial_name = getattr(args, "serial", None)
        if self.serial_name:
            self.recv = SerialReceiver(
                self.serial_name,
                getattr(args, "baud", SERIAL_BAUD_DEFAULT),
                self.frames, lambda m: print(m, flush=True),
                getattr(args, "raw_log", None))
        else:
            self.recv = Receiver(args.host, args.port, self.frames,
                                 lambda m: print(m, flush=True))

        self.img = Image.new("RGB", (DEFAULT_W, DEFAULT_H), (0, 0, 0))
        self.canvas_w, self.canvas_h = DEFAULT_W, DEFAULT_H

        self.root = tk.Tk()
        self.root.title("lcd_mirror")
        self.root.configure(bg="black")
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

        self.scale = max(0.1, args.scale)
        self.canvas = tk.Canvas(self.root,
                                width=int(self._shown_w() * self.scale),
                                height=int(self._shown_h() * self.scale),
                                highlightthickness=0, bd=0, bg="black")
        self.canvas.pack(fill="both", expand=True)

        # 底部一行状态：触摸事件数 / 最后坐标，现场一眼能判断"鼠标到底传过去没有"
        self.status_var = tk.StringVar(value="触摸：尚未发送（在画布上按住鼠标 = 点屏幕）")
        self.status_lbl = tk.Label(self.root, textvariable=self.status_var,
                                   anchor="w", bg="#111111", fg="#cccccc",
                                   font=("Consolas", 10))
        self.status_lbl.pack(side="bottom", fill="x")

        self.photo = ImageTk.PhotoImage(self._display_image())
        self.item = self.canvas.create_image(0, 0, anchor="nw", image=self.photo)

        # ---- 鼠标 = 触摸 ----
        # 按下/拖动/抬起都发给板子。抬起绑在**整个画布**上（不是图像元素上），
        # 因为拖到图像外面松手时，只有画布还能收到那个抬起事件 ——
        # 漏掉抬起的后果是板子那边永远停在"按着"，界面会卡死。
        self.touch_down = False
        self.last_panel = None
        self.canvas.bind("<ButtonPress-1>", self.on_mouse_down)
        self.canvas.bind("<B1-Motion>", self.on_mouse_move)
        self.canvas.bind("<ButtonRelease-1>", self.on_mouse_up)
        self.canvas.bind("<Leave>", self.on_mouse_leave)

        # 统计
        self.dirty = True
        self.fps = 0.0
        self.fps_frames = 0
        self.fps_t0 = time.monotonic()
        self.last_seq = -1
        self.shown_frames = 0
        self.connected = False
        self.connected_since = None
        self.paused = False

        # 计时（--timing / 按 t 开关）：解码一帧多少 ms、重画一次多少 ms
        self.dec_ms = 0.0
        self.dec_n = 0
        self.draw_ms = 0.0
        self.draw_n = 0

        self.root.bind("<space>", self.toggle_pause)
        self.root.bind("<t>", self.toggle_timing)
        self.root.bind("<r>", self.request_keyframe)
        self.root.bind("<Escape>", lambda e: self.on_close())

        self.recv.start()
        self.root.after(10, self.pump)
        # 马上先 tick 一次，好让标题 / 状态行第一时间就把"走的是串口还是 TCP"
        # 显示出来（tick 自己会再排下一次）
        self.root.after(0, self.tick)

    # -- 内部 ----------------------------------------------------------

    def _display_image(self):
        return render_display(self.img, self.args.rotate, self.scale)

    def _shown_w(self):
        """屏幕上实际占的宽度（旋转 90/270 时长短边换过来）。"""
        return shown_size(self.canvas_w, self.canvas_h, self.args.rotate)[0]

    def _shown_h(self):
        return shown_size(self.canvas_w, self.canvas_h, self.args.rotate)[1]

    def _grow_canvas(self, w, h):
        """板子换了分辨率（或收到超范围的矩形）时把画布放大。"""
        nw = max(self.canvas_w, w)
        nh = max(self.canvas_h, h)
        if nw == self.canvas_w and nh == self.canvas_h:
            return
        old = self.img
        self.canvas_w, self.canvas_h = nw, nh
        self.img = Image.new("RGB", (nw, nh), (0, 0, 0))
        self.img.paste(old, (0, 0))
        self.canvas.config(width=int(self._shown_w() * self.scale),
                           height=int(self._shown_h() * self.scale))
        self.dirty = True

    def toggle_pause(self, _evt=None):
        self.paused = not self.paused
        print("显示已暂停" if self.paused else "显示已恢复", flush=True)

    def request_keyframe(self, _evt=None):
        """按 r：请板子重发一次整屏（旧条纹/坏带子留久了用它一键刷新）。

        串口模式下就是往口上发一条 nsh 命令；TCP 模式没有这个入口（板子那边
        TCP 腿本来就不需要，重连时自己会整屏重发），按了只提示一句。
        """
        try:
            self.recv.send_cmd("hw_test lcdmirror keyframe")
            print("已请求整屏关键帧（按 r）", flush=True)
        except AttributeError:
            print("当前链路（TCP）不支持按需整屏：重连一次就会整屏重发", flush=True)

    def toggle_timing(self, _evt=None):
        """按 t：开关"解码 / 重画各多少 ms"的每秒打印。"""
        self.args.timing = not self.args.timing
        if not self.args.timing:
            self.dec_ms = self.dec_n = self.draw_ms = self.draw_n = 0
        print("计时打印：" + ("开" if self.args.timing else "关"), flush=True)

    # -- 鼠标 -> 触摸 ---------------------------------------------------

    def _to_panel(self, evt):
        """窗口坐标 -> 面板坐标（旋转 + 缩放都反算回去）。"""
        return widget_to_panel(evt.x, evt.y, self.canvas_w, self.canvas_h,
                               self.args.rotate, self.scale)

    def _send_touch(self, x, y, pressed):
        self.last_panel = (x, y, 1 if pressed else 0)
        # 只入队，写 socket 是接收线程的事 —— GUI 永远不会被网络卡住
        self.recv.send_touch(x, y, pressed)

    def on_mouse_down(self, evt):
        x, y = self._to_panel(evt)
        self.touch_down = True
        self._send_touch(x, y, True)
        print(f"[touch] 按下 -> 面板 ({x}, {y})", flush=True)

    def on_mouse_move(self, evt):
        if not self.touch_down:
            return
        x, y = self._to_panel(evt)
        self._send_touch(x, y, True)

    def on_mouse_up(self, evt):
        if not self.touch_down:
            return
        self.touch_down = False
        x, y = self._to_panel(evt)
        self._send_touch(x, y, False)
        print(f"[touch] 抬起 -> 面板 ({x}, {y})", flush=True)

    def on_mouse_leave(self, _evt=None):
        """鼠标划出窗口：补一个抬起，否则板子那边会一直按着。"""
        if self.touch_down:
            self.touch_down = False
            x, y = self.last_panel[0], self.last_panel[1]
            self._send_touch(x, y, False)
            print("[touch] 鼠标离开窗口 -> 补发抬起", flush=True)

    def pump(self):
        """每 10ms 把队列里的帧贴到画布上。收得比画得快就只画最后几帧。"""
        n = 0
        while n < 64:
            try:
                kind, payload = self.frames.get_nowait()
            except queue.Empty:
                break

            if kind == "frame":
                x, y, w, h, seq, flags, ver, data = payload
                self._grow_canvas(x + w, y + h)
                if not self.paused:
                    try:
                        t_dec = time.perf_counter() if self.args.timing else 0.0
                        rect = decode_frame(ver, flags, w, h, data)
                        self.img.paste(rect, (x, y))
                        if self.args.timing:
                            self.dec_ms += (time.perf_counter() - t_dec) * 1e3
                            self.dec_n += 1
                        self.last_seq = seq
                        self.dirty = True
                    except (ValueError, MemoryError) as e:
                        print(f"解码这一帧失败：{e}", flush=True)
                        # 串口模式下这一帧被日志字节插坏了 —— 板子知道该补哪几行
                        # （头已经解出来了），发一条精确补发请求，别等 30 秒的兜底
                        # 关键帧。TCP 那边没有这个入口，send_cmd 会是空操作。
                        try:
                            self.recv.send_cmd("hw_test lcdmirror resend %d %d"
                                               % (y, h))
                        except AttributeError:
                            pass
                self.shown_frames += 1
                self.fps_frames += 1
            elif kind == "connect":
                self.connected = True
                self.connected_since = time.monotonic()
            elif kind == "disconnect":
                self.connected = False
                self.connected_since = None
            elif kind == "fatal":
                self.root.after(0, self.on_close)
                return

            n += 1

        if self.dirty:
            self.dirty = False
            t_draw = time.perf_counter() if self.args.timing else 0.0
            self.photo = ImageTk.PhotoImage(self._display_image())
            self.canvas.itemconfig(self.item, image=self.photo)
            if self.args.timing:
                self.draw_ms += (time.perf_counter() - t_draw) * 1e3
                self.draw_n += 1

        self.root.after(10, self.pump)

    def tick(self):
        now = time.monotonic()
        dt = now - self.fps_t0
        if dt >= 1.0:
            self.fps = self.fps_frames / dt
            self.fps_frames = 0
            self.fps_t0 = now

        where = self.recv.where_text()
        self.root.title(
            f"lcd_mirror  |  {where}  |  {self.canvas_w}x{self.canvas_h} RGB565  |  "
            f"fps {self.fps:5.1f}  |  帧 #{self.last_seq}  |  "
            f"收 {self.recv.total_bytes / 1e6:.1f} MB / {self.recv.total_frames} 帧"
            f"  跳号 {self.recv.seq_gaps}  |  触摸 {self.recv.sent_events} 个"
        )

        # 底部那一行：现场判断"鼠标到底有没有传过去"就看它
        if self.recv.last_touch is None:
            self.status_var.set(
                f"[{where}] 触摸：尚未发送（在画布上按住鼠标 = 点屏幕；"
                f"缩放 {self.scale:g} 旋转 {self.args.rotate}°）")
        else:
            tx, ty, tp = self.recv.last_touch
            self.status_var.set(
                f"[{where}] 触摸：已发 {self.recv.sent_events} 个事件  |  "
                f"最后坐标 ({tx}, {ty}) {'按下' if tp else '抬起'}  |  "
                f"写失败 {self.recv.send_errors}  |  "
                f"缩放 {self.scale:g} 旋转 {self.args.rotate}°")

        if self.args.timing and dt >= 1.0:
            avg_dec = (self.dec_ms / self.dec_n) if self.dec_n else 0.0
            avg_draw = (self.draw_ms / self.draw_n) if self.draw_n else 0.0
            print(f"[计时] 解码 {avg_dec:6.2f} ms/帧（{self.dec_n} 帧）  "
                  f"重画 {avg_draw:6.2f} ms/次（{self.draw_n} 次）", flush=True)
            self.dec_ms = self.dec_n = 0
            self.draw_ms = self.draw_n = 0

        if not self.args.quiet and dt >= 1.0:
            mbps = 0.0
            if self.connected_since:
                el = max(0.001, now - self.connected_since)
                mbps = self.recv.total_bytes / el / 1e6
            print(f"[{time.strftime('%H:%M:%S')}] fps={self.fps:5.1f} "
                  f"帧总数={self.recv.total_frames} seq={self.last_seq} "
                  f"累计={self.recv.total_bytes / 1e6:.2f} MB "
                  f"平均={mbps:.2f} MB/s 跳号={self.recv.seq_gaps} "
                  f"{'已连接' if self.connected else '未连接'}",
                  flush=True)

        self.root.after(1000, self.tick)

    def on_close(self):
        # 关窗口前补一个抬起，板子那边不会留着"按着"
        if self.touch_down:
            self.touch_down = False
            x, y = self.last_panel[0], self.last_panel[1]
            self._send_touch(x, y, False)
            if isinstance(self.recv, SerialReceiver):
                # 串口那条路上，抬起是接收线程写出去的 —— 等它落地再退进程，
                # 不然进程一退，板子会永远停在"按着"。
                # （TCP 那条路不等，保持原样）
                deadline = time.monotonic() + 0.3
                while time.monotonic() < deadline and \
                        self.recv.last_touch != (x, y, 0):
                    time.sleep(0.005)

        # 串口那条路：**先**把板子的镜像停掉，再让接收线程停 —— 不然帧会一直往
        # 控制台吐，nsh 的输出全被淹（连 stop 都发不进去）。顺序不能反：
        # send_cmd 只是排队，真正写串口的是接收线程，所以必须赶在它停之前排进去。
        if isinstance(self.recv, SerialReceiver):
            try:
                self.recv.send_cmd("hw_test lcdmirror stop")
                deadline = time.monotonic() + 0.4
                while time.monotonic() < deadline:
                    time.sleep(0.02)
            except Exception:              # noqa: BLE001
                pass

        self.recv.stop_flag = True

        try:
            self.root.destroy()
        except tk.TclError:
            pass

    def run(self):
        self.root.mainloop()


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(
        description="把开发板屏幕镜像到电脑上显示（板端 hw_test lcdmirror）")
    ap.add_argument("--host", default="0.0.0.0", help="监听地址，默认 0.0.0.0")
    ap.add_argument("--port", type=int, default=5600, help="监听端口，默认 5600")
    ap.add_argument("--serial", metavar="COM", default=None,
                    help="走串口收帧（例 --serial COM4），这时不再监听 TCP 端口；"
                         "帧流里混着 nsh/内核日志，脚本自己找帧头重同步，"
                         "触摸改为写 nsh 命令 hw_test lcdtap")
    ap.add_argument("--baud", type=int, default=SERIAL_BAUD_DEFAULT,
                    help="串口波特率，默认 %d（必须和板端一致）"
                         % SERIAL_BAUD_DEFAULT)
    ap.add_argument("--scale", type=float, default=1.0,
                    help="显示放大倍数，默认 1.0（390x450 在电脑上偏小）")
    ap.add_argument("--rotate", type=int, default=0, choices=[0, 90, 180, 270],
                    help="显示旋转，默认 0")
    ap.add_argument("--quiet", action="store_true", help="不每秒打流量统计")
    ap.add_argument("--raw-log", metavar="FILE", default=None,
                    help="串口模式下顺带把原始字节流存一份到 FILE（控制台日志会被帧字节"
                         "淹没，存下来才能 grep 板子的输出；不定时就是纯镜像）")
    ap.add_argument("--timing", action="store_true",
                    help="每秒打一行「解码/重画各多少 ms」（窗口里按 t 也能开关）")
    args = ap.parse_args()

    print(__doc__.strip().splitlines()[0])
    if args.serial:
        print(f"参数：串口 {args.serial} @ {args.baud} baud（不再监听 TCP），"
              f"缩放 {args.scale}，旋转 {args.rotate}°。"
              f"空格=暂停显示，Esc=退出。")
    else:
        print(f"参数：监听 {args.host}:{args.port}，缩放 {args.scale}，"
              f"旋转 {args.rotate}°。空格=暂停显示，Esc=退出。")

    MirrorWindow(args).run()
    print("退出。")


if __name__ == "__main__":
    main()
