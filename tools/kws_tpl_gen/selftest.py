#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""kws_tpl_gen 自检 —— 证明「PC 上能出板子读得懂的模板」这条链真的通。

**它验的是工具链，不是唤醒词效果。** 用合成信号跑通格式，不代表模板能让板子
认出「你好，openvela」—— 那必须真录音 + 上板实测。这两件事别混。

判据（三个用例，覆盖四件事，全过才算通）：
  ① 正例：一段「有静音包裹的合成语音」能生成 slot0.tpl，并且
     ①a 文件头逐字段核对：magic / version / dim / q / rate / hop_ms 都对得上
        kws_dtw 的口径（错一个，板子的 kws_tpl_load 会整条拒绝）；
     ①b 校验和重算比对：sum = Σ 每个 int16 按 u16 累加。这条才证明载荷没写坏
        —— 只看文件大小是不够的，"长度对、内容半截"正是 kws_tpl_save 那段
        注释专门防的形态。
  ② **负例：纯静音必须失败** —— 这条是防"脚本恒绿"的。工具要是无条件成功，
     这个自检就什么都没证明。
  ③ **负例：极短爆音必须失败**，且必须败在 kws_enroll 的有效语音长度门
     —— ②挡在工具自己的峰值门，③才验到 kws_dtw 本身。两层各测各的。

⚠ 三个用例都强制要求**过了 WAV 解析**。2026-10-30 第一版就栽在这儿：解析里
   有个 seek 漏了，正例负例都以"缺 fmt 块"收场，负例于是"如期失败"地绿了，
   其实连特征提取都没跑到。

用法：python3 selftest.py
"""

import os
import struct
import subprocess
import sys
import tempfile
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "kws_tpl_gen")

# 口径必须和 app/hello_app/kws_dtw.{c,h} 一致；对不上说明那边改了而这边没跟上
RATE = 16000
DIM = 13
Q = 64
HOP_MS = 10
FEAT_MS = HOP_MS * 2      # KWS_DECIM=2 → 一个特征点 20ms
MIN_FEAT = 8
MAX_FEAT = 100
TPL_VERSION = 1           # kws_dtw.c 的 KWS_TPL_VERSION


def synth_wav(path, seconds_speech=1.3, gain=0.30):
    """一段「前后有静音的合成语音」：基频扫频 + 6 次谐波 + 音节包络。

    不是真语音，但能量起落够触发端点检测 —— 这正是本自检要的（验格式，
    不验识别率）。"""
    pre = int(RATE * 0.4)
    body = int(RATE * seconds_speech)
    post = int(RATE * 0.4)
    total = pre + body + post

    import math
    out = []
    for n in range(total):
        if n < pre or n >= pre + body:
            out.append(0)
            continue
        t = (n - pre) / RATE
        f0 = 180.0 + 80.0 * (t / seconds_speech)      # 基频慢慢升
        s = 0.0
        for h in range(1, 7):
            s += math.sin(2 * math.pi * f0 * h * t) / h
        # 音节包络：约 4 个音节，让它有起有落（端点检测靠这个）
        env = 0.5 * (1.0 - math.cos(2 * math.pi * 4.0 * t / seconds_speech))
        out.append(int(max(-32768, min(32767, s * env * gain * 12000))))

    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(struct.pack("<%dh" % total, *out))
    return path


def validate_tpl(path):
    """逐字段核对模板文件。返回 (ok, 说明)。"""
    with open(path, "rb") as f:
        blob = f.read()

    if len(blob) < 24:
        return False, f"文件只有 {len(blob)} 字节，连 24 字节头都不够"

    magic, ver, dim, frames, q, rate, ssum, hop = struct.unpack_from("<IHHHHIII", blob, 0)

    if blob[:4] != b"KWST":
        return False, f"magic 不对：{blob[:4]!r}"
    if ver != TPL_VERSION:
        return False, f"version {ver} != {TPL_VERSION}"
    if dim != DIM:
        return False, f"dim {dim} != {DIM}"
    if q != Q:
        return False, f"q {q} != {Q}"
    if rate != RATE:
        return False, f"rate {rate} != {RATE}"
    if hop != FEAT_MS:
        return False, f"hop_ms {hop} != {FEAT_MS}"
    if not (MIN_FEAT <= frames <= MAX_FEAT):
        return False, f"frames {frames} 不在 [{MIN_FEAT}, {MAX_FEAT}]"

    want = 24 + frames * dim * 2
    if len(blob) != want:
        return False, f"长度 {len(blob)} != 24+{frames}×{dim}×2 = {want}"

    payload = blob[24:]
    calc = 0
    for (v,) in struct.iter_unpack("<H", payload):
        calc = (calc + v) & 0xFFFFFFFF
    if calc != ssum:
        return False, f"校验和不对：头里 {ssum}，重算 {calc}"

    return True, (f"{frames} 特征点 = {frames * FEAT_MS}ms，{len(blob)} 字节；"
                  f"头字段全对（magic/version/dim/q/rate/hop），"
                  f"校验和 {ssum} 重算一致")


def run(label, wav_path, expect_ok, fail_markers=()):
    """跑一次。expect_ok=False 时，fail_markers 里**任意一条**出现在输出里
    就算败在了预想的那一层（不同输入该败在不同层，所以做成可指定的）。
    无论哪一层，都强制要求**过了 WAV 解析** —— 否则就是 2026-10-30 那种
    "解析坏了，正例负例一起红/一起绿，其实什么都没测到"。"""
    outdir = tempfile.mkdtemp(prefix="kws_selftest_")
    r = subprocess.run([TOOL, "-o", outdir, "-p", "300", wav_path],
                       capture_output=True, text=True)
    tpl = os.path.join(outdir, "kws_out", "slot0.tpl")
    exists = os.path.exists(tpl)
    out = r.stdout + r.stderr

    # 解析过了没的证据：只有 wav_read 成功、report_level 跑完才会打印"峰值"
    parsed = "峰值" in out

    if expect_ok:
        if r.returncode != 0 or not exists:
            print(f"  ✗ {label}：本该成功，实际 rc={r.returncode}，"
                  f"模板{'在' if exists else '不在'}")
            print("     ---- 工具输出 ----")
            print("     " + out.strip().replace("\n", "\n     "))
            return False
        ok, why = validate_tpl(tpl)
        if not ok:
            print(f"  ✗ {label}：模板出来了但校验不过 —— {why}")
            return False
        print(f"  ✓ {label}：{why}")
        return True

    # 负例
    if r.returncode == 0 and exists:
        print(f"  ✗ {label}：**不该成功却成功了** —— 工具在无条件通过，"
              f"这正是自检要防的那种假绿")
        return False

    if not parsed:
        print(f"  ✗ {label}：失败了，但**败在 WAV 解析**（没走到特征提取）—— "
              f"这条负例等于没测")
        print("     " + out.strip().replace("\n", "\n     "))
        return False

    if fail_markers and not any(m in out for m in fail_markers):
        print(f"  ✗ {label}：失败了，但不在预想的那几层里")
        print(f"     期望出现之一：{fail_markers}")
        print("     " + out.strip().replace("\n", "\n     "))
        return False

    hit = next((m for m in fail_markers if m in out), "（未指定层）")
    print(f"  ✓ {label}：如期失败（rc={r.returncode}，挡在「{hit}」）")
    return True


def main():
    if not os.path.exists(TOOL):
        print("★ 没有 kws_tpl_gen，先跑 ./build.sh")
        return 1

    print("=== kws_tpl_gen 自检（验工具链，不验唤醒效果）===")
    tmp = tempfile.mkdtemp(prefix="kws_selftest_in_")

    good = synth_wav(os.path.join(tmp, "good.wav"), seconds_speech=1.3)

    # 纯静音：该被工具自己的"峰值太低"那道前置检查挡下
    silence = os.path.join(tmp, "silence.wav")
    with wave.open(silence, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(struct.pack("<%dh" % (RATE * 2), *([0] * (RATE * 2))))

    # 极短爆音：有能量、过了峰值门，但只够 ~6 个特征点（< KWS_MIN_FEAT=8），
    # 该被 kws_enroll 的有效语音长度门挡下 —— 这条才验到 kws_dtw 本身
    short = synth_wav(os.path.join(tmp, "short.wav"), seconds_speech=0.12)

    allok = True
    allok &= run("① 正例：合成语音（含头字段+校验和核对）", good, True)
    allok &= run("② 负例：纯静音", silence, False,
                 fail_markers=("基本是静音", "有效语音只有"))
    allok &= run("③ 负例：极短爆音（120ms）", short, False,
                 fail_markers=("有效语音只有", "端点检测没找到有效语音"))

    print()
    if allok:
        print("★ 判据全过：工具链通了 —— 两层把关各自都在干活，格式也对得上。")
        print("  ⚠ 但这只说明「格式对、该拒的拒」。真要板子认出「你好，openvela」，")
        print("    还得：录真话 → 生成模板 → 放进 ROMFS → 烧录 → 对板子说一句")
        print("    → 看串口有没有唤醒命中 → 再按实测类内距离调阈值。")
        return 0

    print("★ 有判据没过 —— 别拿这个工具出的模板去打包。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
