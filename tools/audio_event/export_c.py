"""导出 C 侧权重头文件 + json。

读训练产物 best.pt，把 BatchNorm 折进卷积权重，按契约顺序拼成 2372 个 float32，
写出：
  app/hello_app/sound_event_model.h     —— 板端编译用（SE_W_LEN 必须正好 2372）
  app/hello_app/sound_event_model.json  —— ref_infer.py 用（纯 numpy 前向）

导出后会自检：用折好的权重跑 numpy 参考前向，和 PyTorch 前向对比，误差必须 < 1e-4。

用法:
  py -3.10 export_c.py [--ckpt D:/apply/claw/_audio_event/best.pt]
"""

import argparse
import json
import os
import sys

import numpy as np
import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dscnn  # noqa: E402
import mel as mel_front  # noqa: E402
import ref_infer  # noqa: E402
from ref_infer import carray  # noqa: E402


def load_ckpt(path):
    ckpt = torch.load(path, map_location="cpu")
    model = dscnn.DSCNNLite()
    model.load_state_dict(ckpt["state_dict"])
    model.eval()
    return model, ckpt


def verify(model, flat, n=8):
    """折 BN 后的 numpy 前向 vs PyTorch 前向。"""
    rng = np.random.RandomState(7)
    x = rng.randn(n, 1, dscnn.SE_NMEL, dscnn.SE_NFRAME).astype(np.float32)
    with torch.no_grad():
        ref = model(torch.from_numpy(x)).numpy()
    w, _ = ref_infer.load_weights_from_flat(flat)
    got = np.stack([ref_infer.numpy_forward(x[i].reshape(-1), w) for i in range(n)])
    return float(np.abs(ref - got).max())


def write_header(path, flat, meta):
    body = []
    body.append("/* 自动生成，请勿手改。由 tools/audio_event/export_c.py 从 best.pt 导出。\n")
    body.append("   网络: DSCNN-lite float32，BatchNorm 已折进卷积权重，C 侧无 BN。\n")
    body.append("   类序固定: 0=other 1=fall 2=knock 3=scream\n")
    body.append("   验证集准确率: %s */\n\n" % meta.get("val_acc_str", "n/a"))
    body.append("#ifndef SOUND_EVENT_MODEL_H\n#define SOUND_EVENT_MODEL_H\n\n")
    body.append("#define SE_NMEL 40\n#define SE_NFRAME 64\n#define SE_NCLASS 4\n")
    body.append("#define SE_W_LEN 2372\n\n")
    body.append("/* ---- log-mel 前端参数，必须和 sound_event.c / tools/audio_event/mel.py 一致 ----\n")
    body.append("   16 kHz 单声道 s16le；帧长 400 样本(25ms)，跳 160(10ms)，symmetric Hann 窗；\n")
    body.append("   512 点 FFT -> 257 点功率谱 |X|^2（整体缩放无所谓，log 后被归一化消掉）；\n")
    body.append("   40 个三角 mel 滤波器，HTK 刻度 mel=2595*log10(1+f/700)，f_min=0 f_max=8000，\n")
    body.append("   峰值为 1（线性插值在 Hz 域，不做面积归一化）；每帧 log(max(x,1e-10)) 自然对数；\n")
    body.append("   一个窗口 = 连续 64 帧；滑窗步长 20 帧(200ms)；\n")
    body.append("   归一化 (x-mean)/(std+1e-5) 对整个 40x64 窗口；\n")
    body.append("   输入张量布局 x[m][t] = mel[m*64+t]，m=mel 维 0..39，t=帧 0..63。 */\n\n")
    body.append("/* ---- 权重拼接顺序（float32 行主序，共 2372）----\n")
    body.append("   c1 144, b1 16, dw2 144, bdw2 16, pw2 512, bpw2 32,\n")
    body.append("   dw3 288, bdw3 32, pw3 1024, bpw3 32, fc 128, bfc 4\n")
    body.append("   w[oc][ic][kh][kw] / fc w[o][i]，均按 PyTorch 展平顺序 */\n\n")
    th = meta.get("thresholds") or [0.5] * dscnn.SE_NCLASS
    vote = meta.get("vote") or {"need": 3, "of": 4}
    body.append("/* ---- 建议后处理（PC 端按验证集调出，可自行调整）----\n")
    body.append("   每类置信度阈值: other %.3f fall %.3f knock %.3f scream %.3f\n"
                % (th[0], th[1], th[2], th[3]))
    body.append("   投票: %d/%d 个连续窗口同判该类；同类不应期 %.0f s */\n\n"
                % (vote["need"], vote["of"], meta.get("refractory_s", 8.0)))
    body.append(carray("g_sound_event_w", flat))
    body.append("\n#endif /* SOUND_EVENT_MODEL_H */\n")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(body))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(here))
    p = argparse.ArgumentParser()
    p.add_argument("--ckpt", default="D:/apply/claw/_audio_event/best.pt")
    p.add_argument("--out-h", default=os.path.join(repo, "app", "hello_app", "sound_event_model.h"))
    p.add_argument("--out-json", default=os.path.join(repo, "app", "hello_app", "sound_event_model.json"))
    args = p.parse_args()

    model, ckpt = load_ckpt(args.ckpt)
    parts = dscnn.export_weights(model)
    flat = np.concatenate([a.reshape(-1) for _, a in parts]).astype(np.float32)
    print("导出权重长度 SE_W_LEN =", flat.size)
    assert flat.size == dscnn.SE_W_LEN == 2372, flat.size

    # 顺序必须和 ref_infer.py 的 LAYOUT 一致
    off = 0
    for name, n, (rname, rn) in zip([k for k, _ in dscnn.EXPORT_LAYOUT],
                                    [v for _, v in dscnn.EXPORT_LAYOUT], ref_infer.LAYOUT):
        assert (name, n) == (rname, rn), (name, n, rname, rn)
        off += n
    print("权重顺序与 ref_infer.LAYOUT 一致，累计", off)

    dmax = verify(model, flat)
    print("折 BN 后 numpy 前向 vs PyTorch 前向，最大绝对误差 = %.3g" % dmax)
    assert dmax < 1e-4, dmax

    meta = {k: v for k, v in ckpt.items() if k != "state_dict"}
    meta["w_len"] = int(flat.size)
    meta["export_order"] = [k for k, _ in dscnn.EXPORT_LAYOUT]
    meta["mel"] = {
        "sr": mel_front.SAMPLE_RATE, "n_fft": mel_front.N_FFT,
        "frame_len": mel_front.FRAME_LEN, "frame_hop": mel_front.FRAME_HOP,
        "nmel": mel_front.NMEL, "nframe": mel_front.NFRAME, "win_hop": mel_front.WIN_HOP,
        "fmin": mel_front.FMIN, "fmax": mel_front.FMAX,
        "log_floor": mel_front.LOG_FLOOR, "norm_eps": mel_front.NORM_EPS,
        "window": "symmetric hann, np.hanning(400)",
        "mel_scale": "htk: mel = 2595*log10(1+f/700)",
        "mel_filterbank": "40 三角滤波器，峰值归一化到 1.0，线性插值在 Hz 域做（不做面积归一化）",
        "power": "|X|^2（整体缩放无所谓，log 后被归一化消掉）",
        "layout": "mel[m*64+t]",
    }
    meta["weights"] = {k: [float(v) for v in a.reshape(-1)] for k, a in parts}

    os.makedirs(os.path.dirname(os.path.abspath(args.out_h)), exist_ok=True)
    write_header(args.out_h, flat, meta)
    with open(args.out_json, "w", encoding="utf-8", newline="\n") as f:
        json.dump(meta, f, ensure_ascii=False, indent=1)
    print("已写出:", os.path.abspath(args.out_h))
    print("已写出:", os.path.abspath(args.out_json))


if __name__ == "__main__":
    main()
