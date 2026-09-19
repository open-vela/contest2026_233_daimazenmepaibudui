#!/usr/bin/env python3.10
# -*- coding: utf-8 -*-
"""板子串口终端（Windows GUI）：1 Mbaud 连 COM4，能在窗口里直接敲 nsh 命令。

用法: py -3.10 serial_term.py [COM口] [波特率]

为什么要这个而不是用 PuTTY：这台机器上不一定装了串口工具，而 pyserial + tkinter
我们已经在用（镜像窗口就是），装都不用装。功能刚好够用：

  * 上方是板子输出（自动去掉 ANSI 颜色码，串口里的二进制帧会被替换成 · 之类的
    可见字符，不至于把窗口搞花）；只保留最后 4000 行，跑久了不会吃内存；
  * 下方一行输入框：**回车发送**（自动补 CRLF —— nsh 只认 \\n，但 CR 会被当普通
    字符粘在参数尾巴上，所以发 \\r\\n 并由板端解析时容错，跟 PC 端镜像脚本一致）；
  * 按钮/快捷键：`清屏`(Ctrl-L) / `发 Ctrl-C`(Esc 键发 0x03，用来打断正在跑的
    命令) / 关窗即断开；
  * 顶部状态行显示：口名、波特率、已收字节数、当前是否连着。

注意：这个窗口**独占 COM4**。开着我这边（cmd_cap/raw_cap 等）就打不开串口了 ——
要我用串口的时候先关掉它；只是想查状态的话，我走 MQTT 的 diag 通道就行。
"""
import queue
import re
import sys
import threading
import time

import serial
import tkinter as tk
from tkinter import font as tkfont

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM4"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 1000000
MAX_LINES = 4000

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b\][^\x07]*\x07")

rxq = queue.Queue()
stop_flag = False
recv_bytes = 0


def reader(ser):
    """读线程：把原始字节切行塞进队列。读循环只做这一件事，别让它被 Tk 拖住。"""
    global recv_bytes
    buf = b""
    while not stop_flag:
        try:
            data = ser.read(4096)
        except Exception:
            break
        if not data:
            continue
        recv_bytes += len(data)
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            rxq.put(line.rstrip(b"\r"))
        if len(buf) > 8192:            # 没有换行的超长片段（二进制帧）也吐出去
            rxq.put(buf)
            buf = b""


def printable(b: bytes) -> str:
    """把一行字节变成人看得懂的文本：ANSI 去掉，非可打印字节换成 '·'。"""
    try:
        s = b.decode("utf-8", "replace")
    except Exception:
        s = b.decode("latin-1", "replace")
    s = ANSI.sub("", s)
    out = []
    for ch in s:
        o = ord(ch)
        if ch == "\ufffd" or (o < 32 and ch != "\t") or o == 0x7F:
            out.append("·")
        else:
            out.append(ch)
    return "".join(out)


class Term:
    def __init__(self):
        self.root = tk.Tk()
        self.root.title("板子串口终端 — %s @%d" % (PORT, BAUD))
        self.root.geometry("980x600")

        self.status = tk.Label(self.root, anchor="w", font=("Consolas", 10))
        self.status.pack(fill="x")

        f = tkfont.Font(family="Consolas", size=10)
        self.text = tk.Text(self.root, wrap="none", font=f, background="#101418",
                            foreground="#d0d6dc", insertbackground="#d0d6dc")
        self.text.pack(fill="both", expand=True)

        bottom = tk.Frame(self.root)
        bottom.pack(fill="x")
        tk.Label(bottom, text="命令:", font=("Consolas", 10)).pack(side="left")
        self.entry = tk.Entry(bottom, font=f)
        self.entry.pack(side="left", fill="x", expand=True)
        self.entry.bind("<Return>", self.send)
        self.entry.focus_set()
        tk.Button(bottom, text="发送", command=self.send).pack(side="left")
        tk.Button(bottom, text="清屏", command=self.clear).pack(side="left")
        tk.Button(bottom, text="Ctrl-C", command=lambda: self.raw(b"\x03")).pack(side="left")

        self.root.bind("<Control-l>", lambda e: self.clear())
        self.root.bind("<Escape>", lambda e: self.raw(b"\x03"))

        self.ser = None
        self.connect()

        self.root.after(50, self.pump)
        self.root.after(500, self.tick)
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

    # -- 串口 ----------------------------------------------------------

    def connect(self):
        try:
            self.ser = serial.Serial(PORT, BAUD, timeout=0.05)
            try:
                self.ser.set_buffer_size(rx_size=1 << 18, tx_size=1 << 12)
            except Exception:
                pass
            self.ser.write(b"\r\n")
            threading.Thread(target=reader, args=(self.ser,), daemon=True).start()
            self.log_line("[终端] 已打开 %s @ %d（回车发送；Esc/Ctrl-C 按钮发 Ctrl-C）"
                          % (PORT, BAUD))
        except Exception as e:
            self.ser = None
            self.log_line("[终端] 打开 %s 失败：%s（换口名重试：py serial_term.py COM3）"
                          % (PORT, e))

    def raw(self, data: bytes):
        if self.ser is None:
            return
        try:
            self.ser.write(data)
        except Exception as e:
            self.log_line("[终端] 写失败：%s" % e)

    def send(self, _evt=None):
        cmd = self.entry.get()
        self.entry.delete(0, "end")
        if self.ser is None:
            self.log_line("[终端] 串口没开，发不出去")
            return
        self.log_line("nsh> " + cmd)
        self.raw(cmd.encode("utf-8", "replace") + b"\r\n")

    # -- 显示 ----------------------------------------------------------

    def log_line(self, s):
        self.text.insert("end", s + "\n")
        self.trim()

    def trim(self):
        # 只在超限时裁一次，别每行都数
        n = int(self.text.index("end-1c").split(".")[0])
        if n > MAX_LINES:
            self.text.delete("1.0", "%d.0" % (n - MAX_LINES))

    def pump(self):
        """主线程把队列里的行贴上去（不在读线程里碰 Tk）。"""
        n = 0
        while n < 400:
            try:
                line = rxq.get_nowait()
            except queue.Empty:
                break
            self.text.insert("end", printable(line) + "\n")
            n += 1
        if n:
            self.trim()
            self.text.see("end")
        self.root.after(50, self.pump)

    def tick(self):
        state = "已连接" if self.ser is not None else "未连接"
        self.status.config(text="%s @%d    %s    已收 %d 字节"
                                % (PORT, BAUD, state, recv_bytes))
        self.root.after(500, self.tick)

    def clear(self):
        self.text.delete("1.0", "end")

    def on_close(self):
        global stop_flag
        stop_flag = True
        try:
            if self.ser is not None:
                self.ser.close()
        except Exception:
            pass
        self.root.destroy()


if __name__ == "__main__":
    t = Term()
    t.root.mainloop()
