"""两个阶段的假 PC 端（WSL 侧跑，不需要 Pillow）：

协议是 **v2**（板端 board/contest_board/src/lcd_mirror.h 冻结的那个）：每帧 20 字节
小端头（'L''M' ver=2 flags + x/y/w/h + seq + payload_len），载荷在 flags bit0=1 时
是 RLE 流（重复 {u8 run(1..255), u16 像素}）、否则是原样 w*h*2 字节 RGB565。
本脚本只负责**原样落盘**（字节流是 client 解析的原始素材，不在这里解码；
权威解析在 _flash/lcd_mirror.py，逐像素校验在 check_frames.py），落盘之后顺手
按 v2 数一遍帧、算一下 RLE 压缩比，跑测试时一眼能看到压缩有没有生效。

  连接 1：收 2.0 秒，然后**发 RST 硬断**（SO_LINGER 0），验证板子会不会自己重连、
          重连后会不会整屏重发；
  连接 2：一条后台线程一直收（这样板子的帧照常往外走，触摸消息才是真的
          "夹在帧中间"到达），主线程等 5 秒后在反向通道上按脚本发触摸。

脚本覆盖任务要求的那几种（见 TOUCH_SCRIPT）：
  正常点按 / 拖动 / 快速点一下（按下和抬起同一瞬间）/ 越界坐标 /
  坏字节（不该断连接）/ 坏字节之后还能收到 / 8 字节消息分片到达。

两边共享的事实：C 侧从"重连成功"算起 ~4.5 秒进入触摸轮询阶段、一直到 ~14.5 秒；
本脚本在 accept 之后 5 秒开始发、约 4.5 秒发完 —— 前后各留约 0.5 / 5 秒余量。
"""
import socket
import struct
import sys
import threading
import time

port = int(sys.argv[1])
out1 = sys.argv[2]
out2 = sys.argv[3]
sent_path = sys.argv[4] if len(sys.argv) > 4 else None

CUT_AFTER = 2.0
TOUCH_START_DELAY = 5.0
GAP = 0.30           # 相邻消息间隔；比板端一轮循环（20ms）和一次发送都大得多


def t(x, y, pressed):
    """一条合法触摸消息（PC -> 板，8 字节小端）。"""
    return struct.pack("<BBBBHH", ord("L"), ord("T"), 1, 1 if pressed else 0,
                       x & 0xFFFF, y & 0xFFFF)


# ---------------------------------------------------------------- v2 小解析
#
# 只做"数帧 + 算压缩比"这件小事，**不在这里解码像素**（那是客户端
# _flash/lcd_mirror.py 和 check_frames.py 的事）。所以这里只认头，不碰载荷。
# 硬断那条连接的最后可能是半帧（对端 RST 卡在发送中间），所以解析是**容错的**：
# 凑不齐就停下，报告还剩多少字节，不当错误。

HDR_V1 = 16
FLAG_RLE = 0x01


def summarize(path):
    data = open(path, "rb").read()
    off = 0
    n = rle_n = raw_n = 0
    rle_in = rle_out = 0
    while True:
        if len(data) - off < HDR_V1:
            break
        ver = data[off + 2]
        hs = 20 if ver >= 2 else HDR_V1
        if len(data) - off < hs:
            break
        flags = data[off + 3]
        x, y, w, h, seq = struct.unpack_from("<HHHHI", data, off + 4)
        plen = (struct.unpack_from("<I", data, off + 16)[0]
                if ver >= 2 else w * h * 2)
        if data[off] != ord("L") or data[off + 1] != ord("M") or ver not in (1, 2):
            print(f"[dump] {path}: 偏移 {off} 处不是 v2 帧头 -> 停下", flush=True)
            break
        if len(data) - off - hs < plen:
            break
        n += 1
        if flags & FLAG_RLE:
            rle_n += 1
            rle_in += w * h * 2
            rle_out += plen
        else:
            raw_n += 1
        off += hs + plen

    tail = len(data) - off
    print(f"[dump] {path}: 完整 {n} 帧（RLE {rle_n} / 原样 {raw_n}）"
          + (f"，RLE 帧 {rle_in} -> {rle_out} 字节（{rle_in / rle_out:.2f}x）"
             if rle_out else "")
          + (f"，尾部还有 {tail} 字节（半帧/无关键，正常）" if tail else ""),
          flush=True)


# (间隔秒, [要发的字节块...], 这一批对应的"逻辑消息"[(x, y, pressed)])
TOUCH_SCRIPT = [
    # 1) 正常点一下
    (GAP, [t(10, 20, 1)], [(10, 20, 1)]),
    # 2) 拖动
    (GAP, [t(200, 100, 1)], [(200, 100, 1)]),
    (GAP, [t(389, 449, 1)], [(389, 449, 1)]),
    (GAP, [t(389, 449, 0)], [(389, 449, 0)]),
    # 3) 快速点一下：按下和抬起在同一批里发出去（板子那边靠"锁存"补一次按下）
    (GAP, [t(50, 50, 1), t(50, 50, 0)], [(50, 50, 1), (50, 50, 0)]),
    # 4) 越界坐标：x=5000 / y=3 -> 板子应钳成 (389, 3)
    (GAP, [t(5000, 3, 1)], [(5000, 3, 1)]),
    (GAP, [t(5000, 3, 0)], [(5000, 3, 0)]),
    # 5) 坏字节：8 个垃圾字节。**不该断连接**，板子只该把不认识的字节丢掉
    (GAP, [b"XY\x01\x00" + struct.pack("<HH", 1, 2)], []),
    # 6) 坏字节之后必须还能正常收到（证明只是重新同步、连接还活着）
    (GAP, [t(123, 321, 1)], [(123, 321, 1)]),
    (GAP, [t(123, 321, 0)], [(123, 321, 0)]),
    # 7) 8 字节消息分三片到达（TCP 分包的样子），片与片之间板子还在发帧
    (GAP, [(t(300, 77, 1))[0:1]], []),
    (0.15, [(t(300, 77, 1))[1:3]], []),
    (0.15, [(t(300, 77, 1))[3:8]], [(300, 77, 1)]),
    (GAP, [t(300, 77, 0)], [(300, 77, 0)]),
]


def drain(conn, seconds, path):
    """收 seconds 秒（或对端断开为止），字节流写 path。"""
    t0 = time.time()
    conn.settimeout(0.05)
    data = bytearray()
    while time.time() - t0 < seconds:
        try:
            chunk = conn.recv(65536)
        except socket.timeout:
            continue
        except OSError as e:
            print(f"[dump] {path}: recv 出错 {e}", flush=True)
            break
        if not chunk:
            print(f"[dump] {path}: 对端关闭", flush=True)
            break
        data += chunk
    with open(path, "wb") as f:
        f.write(bytes(data))
    print(f"[dump] {path}: {len(data)} 字节", flush=True)
    summarize(path)
    return len(data)


class Reader(threading.Thread):
    """后台把连接 2 的帧收下来（写盘），主线程同时往同一条连接上发触摸。

    不这么做的话，主线程发触摸的时候没人收帧 -> TCP 窗口堵死 -> 板子那边
    一直在重试发送，"触摸消息夹在帧中间到达"这个场景就假了。
    """

    def __init__(self, conn, path):
        super().__init__(daemon=True)
        self.conn = conn
        self.path = path
        self.data = bytearray()
        self.stop_flag = False

    def run(self):
        self.conn.settimeout(0.05)
        while not self.stop_flag:
            try:
                chunk = self.conn.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                print("[dump] 连接 2: 对端关闭", flush=True)
                break
            self.data += chunk

    def save(self):
        with open(self.path, "wb") as f:
            f.write(bytes(self.data))
        print(f"[dump] {self.path}: {len(self.data)} 字节", flush=True)
        summarize(self.path)


srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", port))
srv.listen(2)

print(f"[dump] 监听 127.0.0.1:{port}", flush=True)
conn, peer = srv.accept()
print(f"[dump] 连接 1: {peer}", flush=True)
drain(conn, CUT_AFTER, out1)

conn.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
conn.close()
print("[dump] 已 RST 断开连接 1，等板级模块重连…", flush=True)

srv.settimeout(25)
try:
    conn2, peer2 = srv.accept()
except socket.timeout:
    print("[dump] 等重连超时", flush=True)
    open(out2, "wb").close()
    sys.exit(1)

print(f"[dump] 连接 2（重连）: {peer2}", flush=True)
reader = Reader(conn2, out2)
reader.start()

print(f"[dump] 反向通道 {TOUCH_START_DELAY}s 后开始发触摸脚本", flush=True)
time.sleep(TOUCH_START_DELAY)

logical = []
try:
    for gap, chunks, msgs in TOUCH_SCRIPT:
        time.sleep(gap)
        for c in chunks:
            conn2.sendall(c)
        logical.extend(msgs)
        if msgs:
            body = " ".join(f"({x},{y},{p})" for x, y, p in msgs)
            print(f"[touch] 发出 {body}", flush=True)
        else:
            print(f"[touch] 发出 {sum(len(c) for c in chunks)} 个字节"
                  f"（不是合法消息，看板子会不会乱）", flush=True)
except OSError as e:
    print(f"[dump] 反向通道写失败：{e}", flush=True)

if sent_path:
    with open(sent_path, "w", encoding="utf-8") as f:
        for x, y, p in logical:
            f.write(f"{x} {y} {p}\n")
    print(f"[dump] 发出去的逻辑消息写到 {sent_path}（{len(logical)} 条）", flush=True)

time.sleep(2.0)
reader.stop_flag = True
time.sleep(0.2)
reader.save()
conn2.close()
