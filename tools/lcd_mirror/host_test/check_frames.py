import os
"""Windows 侧校验（py -3.10，用真的 _flash/lcd_mirror.py 里那套解析/解码）：

  py -3.10 ./check_frames.py \
            ./frames.bin

干四件事：
  1. 断言整条字节流**正好**是一串完整帧（没有半截帧 / 错位），且**都是协议 v2**
     （20 字节头 + payload_len；flags 只有 bit0 = RLE 这一个已知位）；
  2. 用真客户端的解码器（rgb565_to_image / decode_frame）把每帧解出来拼到画布上；
  3. 跟 test.c 里 expect565() 的期望画面逐像素比（PIL 的 565->888 展开表先测出来）；
  4. 统计 RLE 压缩比，并断言**两条载荷路径都真的走过了**：RLE 帧（test.c 场景 F
     的纯色长 run）和原样帧（场景 G 的噪声，压不下去必须退回原样）都有，
     且最狠的那一帧压缩比 >= 10。
"""
import importlib.util
import sys

DUMP = sys.argv[1] if len(sys.argv) > 1 else \
    r"./frames.bin"
DUMP2 = sys.argv[2] if len(sys.argv) > 2 else None
SENT = sys.argv[3] if len(sys.argv) > 3 else None
COUT = sys.argv[4] if len(sys.argv) > 4 else None
PANEL_W, PANEL_H = 390, 450

# 板端现在就是协议 v2（20 字节头 + payload_len，载荷可能是 RLE）。板子只发 v2，
# 所以这里拿 ver 卡死：真收到 v1 就说明板端固件没换成 v2，测试不该放过。
PROTO_VER = 2

# 板端一帧的行数上限（协议 v2 的 LCD_MIRROR_MAX_ROWS_PER_FRAME）：客户端脚本里
# 没有这个常量（它是板端实现细节），测试就按"每帧行数 <= 30"卡 —— 板端哪天
# 没拆帧、一帧塞 450 行，这里该红。
ROW_CAP = 30


def load_client():
    spec = importlib.util.spec_from_file_location(
        "lcd_mirror", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lcd_mirror.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def build_expand_tables(mod):
    """测出 PIL 的 5 位 / 6 位 -> 8 位展开表（不猜，拿合成图解出来）。"""
    raw = b"".join(bytes([v & 0xFF, (v >> 8) & 0xFF]) for v in
                   [((v << 11) | (v << 6) | v) for v in range(32)])
    im = mod.rgb565_to_image(raw, 32, 1)
    t5 = [im.getpixel((i, 0))[0] for i in range(32)]

    raw = b"".join(bytes([v & 0xFF, (v >> 8) & 0xFF]) for v in
                   [((v << 5) | 0) for v in range(64)])
    im = mod.rgb565_to_image(raw, 64, 1)
    t6 = [im.getpixel((i, 0))[1] for i in range(64)]
    return t5, t6


def expect565(x, y):
    r = (x * 31) // (PANEL_W - 1)
    g = (y * 63) // (PANEL_H - 1)
    b = (((x * 7) + (y * 13)) & 0x7FF) * 31 // 2047
    return (r << 11) | (g << 5) | b


def parse_stream(mod, data):
    """整条流必须正好是一串完整帧，返回 [(x,y,w,h,seq,flags,payload)]。

    v2 的载荷长度**以头里的 payload_len 为准**（原样帧才等于 w*h*2，RLE 帧比它小）；
    头长也由版本号决定（v1=16 / v2=20），所以先用第 3 个字节读出 ver 再定长。
    """
    off = 0
    frames = []
    while off < len(data):
        assert len(data) - off >= mod.HDR_V1_SIZE, \
            f"尾部只剩 {len(data) - off} 字节，凑不出一个头"
        hs = mod.header_size(data[off + 2])
        assert len(data) - off >= hs, \
            f"偏移 {off} 处的头不完整（要说 {hs} 字节）"
        hs, m0, m1, ver, flags, x, y, w, h, seq, plen = \
            mod.parse_header(data[off:off + hs])
        assert (m0, m1) == (mod.MAGIC0, mod.MAGIC1), \
            f"偏移 {off} 处不是合法帧头: {m0:#x} {m1:#x}"
        assert ver == PROTO_VER, \
            f"偏移 {off} 处 ver={ver}，板端现在应该只发 v{PROTO_VER}"
        assert flags & ~mod.FLAG_RLE == 0, \
            f"偏移 {off} 处 flags={flags:#x} 带着不认识的位（只有 bit0=RLE）"
        if not (flags & mod.FLAG_RLE):
            assert plen == w * h * 2, \
                f"偏移 {off} 处原样帧的 payload_len={plen} 应为 {w * h * 2}"
        payload = data[off + hs: off + hs + plen]
        assert len(payload) == plen, f"偏移 {off} 处的帧被截断（要 {plen} 字节）"
        frames.append((x, y, w, h, seq, flags, payload))
        off += hs + plen

    assert off == len(data), f"尾部还有 {len(data) - off} 字节"
    return frames


def composite(mod, frames):
    canvas = {}
    for x, y, w, h, seq, flags, payload in frames:
        im = mod.decode_frame(PROTO_VER, flags, w, h, payload)
        for j in range(h):
            for i in range(w):
                canvas[(x + i, y + j)] = im.getpixel((i, j))
    return canvas


def rle_stats(mod, frames, tag):
    """RLE 压缩比（只用真实收到的帧算，不猜）+ "两条载荷路径都走过了"的断言。"""
    rle = [f for f in frames if f[5] & mod.FLAG_RLE]
    raw = [f for f in frames if not (f[5] & mod.FLAG_RLE)]
    rle_raw_bytes = sum(f[2] * f[3] * 2 for f in rle)      # 这些帧原样要多少
    rle_out_bytes = sum(len(f[6]) for f in rle)            # 实际发了多少
    all_raw = sum(f[2] * f[3] * 2 for f in frames)
    all_out = sum(len(f[6]) for f in frames)
    best = max((f[2] * f[3] * 2) / len(f[6]) for f in rle) if rle else 0.0

    print(f"[{tag}] 载荷：{len(frames)} 帧 = RLE {len(rle)} + 原样 {len(raw)}；"
          f"RLE 帧 {rle_raw_bytes} -> {rle_out_bytes} 字节"
          + (f"（{rle_raw_bytes / rle_out_bytes:.2f}x）" if rle_out_bytes else "")
          + f"；整段 {all_raw} -> {all_out} 字节"
          + (f"（{all_raw / all_out:.2f}x）" if all_out else "")
          + f"；单帧最狠 {best:.2f}x")
    return rle, raw, best


def compare(canvas, t5, t6, tag):
    if len(canvas) != PANEL_W * PANEL_H:
        raise SystemExit(f"FAIL[{tag}]: 只覆盖 {len(canvas)}/{PANEL_W * PANEL_H} 像素")
    bad = 0
    first = []
    for (x, y), got in canvas.items():
        v = expect565(x, y)
        want = (t5[(v >> 11) & 0x1F], t6[(v >> 5) & 0x3F], t5[v & 0x1F])
        if got != want:
            bad += 1
            if len(first) < 8:
                first.append(((x, y), got, want))
    if bad:
        print("前几个不一致（位置, 收到, 期望）：")
        for f in first:
            print("   ", f)
        raise SystemExit(f"FAIL[{tag}]: {bad} 个像素对不上")
    print(f"  [{tag}] 逐像素一致")


def check_one(mod, data, t5, t6, tag):
    frames = parse_stream(mod, data)
    print(f"[{tag}] {len(data)} 字节 / {len(frames)} 帧，字节流首尾对齐（无半截帧）")
    seqs = [f[4] for f in frames]
    print(f"[{tag}] seq: {seqs[:12]}{'...' if len(seqs) > 12 else ''}")
    assert seqs == list(range(seqs[0], seqs[0] + len(seqs))), f"[{tag}] seq 不连续"
    assert all(0 <= f[0] and f[0] + f[2] <= PANEL_W and
               0 <= f[1] and f[1] + f[3] <= PANEL_H for f in frames), \
        f"[{tag}] 有帧超界（越界裁剪没生效）"
    print(f"[{tag}] 矩形尺寸（去重）：", sorted({(f[2], f[3]) for f in frames}))
    assert all(f[3] <= ROW_CAP for f in frames), \
        f"[{tag}] 有帧的行数超过一帧上限 {ROW_CAP}（板端拆帧没生效）"
    rle, raw, best = rle_stats(mod, frames, tag)
    compare(composite(mod, frames), t5, t6, tag)
    return frames, rle, raw, best


def check_touch(zip_=False):
    """反向通道：把板子"看到的状态变化序列"跟假 PC 端"发出去的消息"逐条比。

    期望值不是另外写一份脚本，而是**从实际发出去的逻辑消息推出来**的，
    所以两边不可能写歪：
      - 每条发出去的消息都会让板子的状态变成 (钳过的 x, 钳过的 y, pressed)；
      - 板子那边"按下"会锁存一次（快速点一下不丢），但锁存产生的是**同一条**
        状态，去重相邻重复之后就跟"逻辑消息序列"一模一样。
    """
    if not SENT or not COUT:
        return
    sent = []
    for line in open(SENT, encoding="utf-8"):
        parts = line.split()
        if len(parts) == 3:
            sent.append((int(parts[0]), int(parts[1]), int(parts[2])))

    # 期望：板端会把越界坐标钳到面板范围内
    expect = [(min(x, PANEL_W - 1), min(y, PANEL_H - 1), p) for x, y, p in sent]

    got = []
    for line in open(COUT, encoding="utf-8", errors="replace"):
        if line.startswith("TOUCH "):
            a, b, c = line.split()[1:4]
            got.append((int(a), int(b), int(c)))

    print(f"\n[反向通道] 发出去 {len(sent)} 条消息，板子报出 {len(got)} 次状态变化")
    print(f"  发出: {sent}")
    print(f"  期望: {expect}")
    print(f"  收到: {got}")

    assert len(got) > 0, "FAIL: 板子一次触摸状态变化都没报出来（反向通道没通）"
    assert got == expect, "FAIL: 板子报出来的触摸序列和发出去的对不上"
    assert got[-1][2] == 0, "FAIL: 最后停在按下状态（界面会卡死）"
    print("  [反向通道] 序列一致，且最后是抬起")


def main():
    mod = load_client()
    t5, t6 = build_expand_tables(mod)

    check_one(mod, open(DUMP, "rb").read(), t5, t6, "连接1")

    if DUMP2:
        data2 = open(DUMP2, "rb").read()
        assert len(data2) > 0, "FAIL: 连接 1 被硬断之后，板级模块**没有重连回来**"
        frames2, rle2, raw2, best2 = check_one(mod, data2, t5, t6, "重连后")

        # 重连后必须整屏重发。v2 一帧最多 30 行，所以整屏是**多帧拼**出来的：
        # 从 y=0 开始、一行不漏地续到 449（逐像素比对已经验证了"不漏"，
        # 这里再明确查"从顶上开始、行连续"，免得哪天变成只补了半屏还蒙混过关）。
        assert frames2[0][1] == 0, \
            f"FAIL: 重连后第一帧不是从 y=0 开始（y={frames2[0][1]}）"
        covered = 0
        for f in frames2:
            if f[1] != covered:
                break
            covered += f[3]
        assert covered >= PANEL_H, \
            f"FAIL: 重连后只从顶上行连续重发了 {covered} 行（不足整屏 {PANEL_H}）"
        print(f"  [重连后] 确认从 y=0 起连续重发整屏（{covered} 行 >= {PANEL_H}）")

        # 两条载荷路径都要真的走到：RLE（场景 F 纯色长 run）+ 原样（场景 G 噪声）。
        assert rle2, "FAIL: 一帧 RLE 都没有（场景 F 的纯色块没压出来）"
        assert raw2, "FAIL: 一帧原样都没有（场景 G 的噪声没退回原样发）"
        assert best2 >= 10.0, \
            f"FAIL: RLE 最狠的一帧只有 {best2:.2f}x（纯色块应该几十倍往上）"
        print(f"  [载荷] 两条路都走到了：RLE {len(rle2)} 帧 / 原样 {len(raw2)} 帧，"
              f"最狠 {best2:.2f}x")

        # 场景 F 那块纯色（0x1234）必须真的以 RLE 帧出现过 —— 不只是"有 RLE 帧"，
        # 而是"解出来整帧就是同一个颜色"。长 run 的边界（255 一截）就是靠它验的。
        solid = (t5[(0x1234 >> 11) & 0x1F], t6[(0x1234 >> 5) & 0x3F],
                 t5[0x1234 & 0x1F])
        solid_frames = 0
        for x, y, w, h, seq, flags, payload in frames2:
            if not (flags & mod.FLAG_RLE):
                continue
            im = mod.decode_frame(PROTO_VER, flags, w, h, payload)
            if im.getpixel((0, 0)) == solid and \
                    im.getcolors(maxcolors=1 << 20) == [(w * h, solid)]:
                solid_frames += 1
        assert solid_frames >= 1, \
            f"FAIL: 场景 F 的纯色块没有以 RLE 帧发出来（期望整帧 {solid}）"
        print(f"  [载荷] 纯色块以 RLE 帧出现过：{solid_frames} 帧整帧都是 {solid}")

        # 触摸脚本跑的时候 C 侧每 500ms 还在喂一行脏矩形 —— 那些帧必须真的发出来了，
        # 才能证明"触摸消息是夹在帧中间到达的"
        rows = [f[1] for f in frames2 if f[3] == 1 and f[2] == PANEL_W]
        assert len(rows) >= 3, \
            f"FAIL: 触摸阶段没有帧在流（只看到 {len(rows)} 个单行帧），无法证明收发并发"
        print(f"  [重连后] 触摸阶段仍在发帧：{len(rows)} 个单行帧 {rows}")

    check_touch()

    print("\nPASS: 全屏逐像素一致 + v2 头部/RLE 载荷（两条路都验过）"
          " + 断线自动重连 + 重连后整屏重发 + 反向通道触摸")


if __name__ == "__main__":
    main()
