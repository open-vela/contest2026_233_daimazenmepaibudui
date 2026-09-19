"""纯 numpy 参考前向 —— 给 C 侧对拍用。

读 sound_event_model.json（export_c.py 产出）里的权重，对一段 PCM 跑一遍
log-mel + DSCNN-lite 前向，输出 D:/apply/claw/_audio_event/golden.h：
  g_golden_mel[2560]  —— 契约里的 float mel[40*64]（已归一化，mel[m*64+t]）
  g_golden_logit[4]   —— 4 类 logit（未过 softmax）
输入 PCM 同时落一份 golden.pcm（s16le 单声道 16 kHz），C 侧喂同一段字节即可对拍。
整个前向用 float64 累加（比板端 float32 更准，误差只来自板端自己）。

用法:
  py -3.10 ref_infer.py                                  # 内置合成音频自检
  py -3.10 ref_infer.py --wav some.wav --start-frame 0
  py -3.10 ref_infer.py --pcm some.pcm
"""

import argparse
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mel as mel_front  # noqa: E402

SE_NMEL, SE_NFRAME, SE_NCLASS, SE_W_LEN = 40, 64, 4, 2372
CLASSES = ["other", "fall", "knock", "scream"]

# 契约的权重顺序 + 形状（行主序展平）
LAYOUT = [
    ("c1", 144), ("b1", 16),
    ("dw2", 144), ("bdw2", 16),
    ("pw2", 512), ("bpw2", 32),
    ("dw3", 288), ("bdw3", 32),
    ("pw3", 1024), ("bpw3", 32),
    ("fc", 128), ("bfc", 4),
]
SHAPES = {
    "c1": (16, 1, 3, 3), "b1": (16,),
    "dw2": (16, 1, 3, 3), "bdw2": (16,),
    "pw2": (32, 16, 1, 1), "bpw2": (32,),
    "dw3": (32, 1, 3, 3), "bdw3": (32,),
    "pw3": (32, 32, 1, 1), "bpw3": (32,),
    "fc": (4, 32), "bfc": (4,),
}
assert sum(n for _, n in LAYOUT) == SE_W_LEN


def conv2d(x, w, b, stride=1, pad=0, groups=1):
    """x (C,H,W) / w (OC, CG, KH, KW) / b (OC,) -> (OC, OH, OW)，float64。"""
    cin, h, wd = x.shape
    oc, cg, kh, kw = w.shape
    xp = np.pad(x, ((0, 0), (pad, pad), (pad, pad))) if pad else x
    oh = (h + 2 * pad - kh) // stride + 1
    ow = (wd + 2 * pad - kw) // stride + 1
    ocg = oc // groups
    out = np.empty((oc, oh, ow), dtype=np.float64)
    for o in range(oc):
        g = o // ocg
        acc = np.zeros((oh, ow), dtype=np.float64)
        for ic in range(cg):
            row = xp[g * cg + ic]
            for a in range(kh):
                for bb in range(kw):
                    acc += w[o, ic, a, bb] * row[a:a + (oh - 1) * stride + 1:stride,
                                                   bb:bb + (ow - 1) * stride + 1:stride]
        out[o] = acc + b[o]
    return out


def _relu(x):
    return np.maximum(x, 0.0)


def init_weights():
    return {k: np.zeros(s, dtype=np.float64) for k, s in SHAPES.items()}


def load_weights_from_flat(flat):
    """契约顺序的展平数组 -> {name: ndarray}。"""
    flat = np.asarray(flat, dtype=np.float64).reshape(-1)
    assert flat.size == SE_W_LEN, flat.size
    w, off = {}, 0
    for name, n in LAYOUT:
        w[name] = flat[off:off + n].reshape(SHAPES[name])
        off += n
    return w, {}


def load_weights(path):
    """读 json -> {name: ndarray}，按契约顺序校验长度。"""
    with open(path, "r", encoding="utf-8") as f:
        meta = json.load(f)
    raw = meta["weights"]
    if not isinstance(raw, dict):
        return load_weights_from_flat(raw)[0], meta
    w = {}
    for name, n in LAYOUT:
        a = np.asarray(raw[name], dtype=np.float64).reshape(-1)
        assert a.size == n, "%s: %d != %d" % (name, a.size, n)
        w[name] = a.reshape(SHAPES[name])
    return w, meta


def numpy_forward(mel_flat, w):
    """mel_flat (2560,) 归一化窗口 -> logit (4,)，float64。"""
    x = np.asarray(mel_flat, dtype=np.float64).reshape(1, SE_NMEL, SE_NFRAME)
    x = _relu(conv2d(x, w["c1"], w["b1"], stride=2, pad=1))
    x = _relu(conv2d(x, w["dw2"], w["bdw2"], stride=1, pad=1, groups=16))
    x = _relu(conv2d(x, w["pw2"], w["bpw2"], stride=1, pad=0))
    x = _relu(conv2d(x, w["dw3"], w["bdw3"], stride=1, pad=1, groups=32))
    x = _relu(conv2d(x, w["pw3"], w["bpw3"], stride=1, pad=0))
    gap = x.mean(axis=(1, 2))
    return w["fc"] @ gap + w["bfc"]


def softmax(logit):
    e = np.exp(np.asarray(logit, dtype=np.float64) - float(np.max(logit)))
    return e / e.sum()


def cfmt(v):
    """float32 -> C 字面量文本（保证能 round-trip）。"""
    s = "%.9g" % float(np.float32(v))
    if ("." not in s) and ("e" not in s) and ("E" not in s) and ("nan" not in s) and ("inf" not in s):
        s += ".0"
    return s + "f"


def carray(name, arr, ctype="float"):
    vals = np.asarray(arr).reshape(-1)
    lines, row = [], []
    for i, v in enumerate(vals):
        row.append(cfmt(v) if ctype == "float" else str(int(v)))
        if len(row) == 8 or i == len(vals) - 1:
            lines.append("    " + ", ".join(row) + ",")
            row = []
    return "%s%s[%d] = {\n%s\n};\n" % ("static const " + ctype + " ", name, len(vals), "\n".join(lines))


def build_mel_window(pcm, start_frame=0):
    """PCM -> 第 start_frame 起的 64 帧归一化窗口 (2560,)。"""
    lm = mel_front.log_mel(pcm)
    n = lm.shape[0]
    if n < SE_NFRAME:
        raise SystemExit("音频太短：只有 %d 帧，至少需要 %d 帧（约 %.2f s）"
                         % (n, SE_NFRAME, (400 + 159 * 160) / 16000.0))
    if start_frame + SE_NFRAME > n:
        raise SystemExit("start_frame=%d 越界：总共只有 %d 帧" % (start_frame, n))
    return mel_front.window_from_frames(lm, start_frame), n


def synthetic_pcm():
    """内置确定性测试音频：1 kHz 正弦 + 白噪，1.0 s。"""
    rng = np.random.RandomState(1234)
    t = np.arange(16000) / 16000.0
    x = 6000 * np.sin(2 * np.pi * 1000.0 * t) + rng.randn(16000) * 300
    return np.clip(x, -32768, 32767).astype(np.int16)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(here))
    p = argparse.ArgumentParser()
    p.add_argument("--model", default=os.path.join(repo, "app", "hello_app", "sound_event_model.json"))
    p.add_argument("--wav", help="16 kHz 单声道 wav")
    p.add_argument("--pcm", help="s16le 单声道 16 kHz 裸字节")
    p.add_argument("--start-frame", type=int, default=0, help="窗口起始帧，默认 0")
    p.add_argument("--out", default="D:/apply/claw/_audio_event/golden.h")
    args = p.parse_args()

    w, meta = load_weights(args.model)
    if args.wav:
        pcm = mel_front.read_wav_mono16k(args.wav)
        src = args.wav
    elif args.pcm:
        with open(args.pcm, "rb") as f:
            pcm = mel_front.pcm_from_bytes(f.read())
        src = args.pcm
    else:
        pcm = synthetic_pcm()
        src = "<内置合成 1kHz 正弦+噪声 1.0s>"

    pcm_i16 = np.clip(np.round(pcm), -32768, 32767).astype(np.int16)

    mel1, nframes = build_mel_window(pcm_i16, args.start_frame)
    logit1 = numpy_forward(mel1, w)
    # 验收项：同一段音频跑两次必须完全一致（含 bit）
    mel2, _ = build_mel_window(pcm_i16, args.start_frame)
    logit2 = numpy_forward(mel2, w)
    same = np.array_equal(mel1.view(np.uint8), mel2.view(np.uint8)) and \
        np.array_equal(logit1.view(np.uint8), logit2.view(np.uint8))
    print("两次运行结果完全一致:", same)
    if not same:
        raise SystemExit("非确定性！mel 最大差 %.3g logit 最大差 %.3g"
                         % (np.abs(mel1 - mel2).max(), np.abs(logit1 - logit2).max()))

    need = 400 + (args.start_frame + SE_NFRAME - 1) * 160
    used = pcm_i16[:need]
    print("音频:", src)
    print("总帧数 %d，取帧 [%d, %d)，用样本 %d 个 (%.3f s)"
          % (nframes, args.start_frame, args.start_frame + SE_NFRAME, used.size, used.size / 16000.0))
    print("logit:", " ".join("%+.6f" % v for v in logit1))
    print("softmax:", " ".join("%s=%.4f" % (CLASSES[i], v) for i, v in enumerate(softmax(logit1))))
    print("预测:", CLASSES[int(np.argmax(logit1))])

    out = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    pcm_path = os.path.join(os.path.dirname(out), "golden.pcm")
    used.astype("<i2").tofile(pcm_path)

    body = []
    body.append("/* 自动生成，请勿手改。由 tools/audio_event/ref_infer.py 产出。 */\n")
    body.append("#ifndef SOUND_EVENT_GOLDEN_H\n#define SOUND_EVENT_GOLDEN_H\n\n")
    body.append("#define GOLDEN_NMEL %d\n#define GOLDEN_NFRAME %d\n" % (SE_NMEL, SE_NFRAME))
    body.append("#define GOLDEN_MEL_LEN %d\n#define GOLDEN_LOGIT_LEN %d\n" % (SE_NMEL * SE_NFRAME, SE_NCLASS))
    body.append("#define GOLDEN_PCM_LEN %d\n\n" % used.size)
    body.append("/* 输入 PCM (%s)\n" % src)
    body.append("   : %s\n" % pcm_path)
    body.append("   样本格式 s16le 单声道 16000 Hz，长度 %d 个样本，就是第 0..%d 帧的输入。\n"
                % (used.size, SE_NFRAME - 1))
    body.append("   g_golden_mel 是契约里的 float mel[40*64]，索引 mel[m*64+t]，已归一化。\n")
    body.append("   g_golden_logit 是 float64 累加的前向输出（未过 softmax），板端 float32 允许 1e-3 级误差。 */\n\n")
    body.append(carray("g_golden_mel", mel1))
    body.append("\n")
    body.append(carray("g_golden_logit", logit1))
    body.append("\n#endif /* SOUND_EVENT_GOLDEN_H */\n")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(body))
    print("已写出:", out, " 和", pcm_path)


if __name__ == "__main__":
    main()
