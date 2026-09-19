"""训练 DSCNN-lite 声音事件分类器。

数据目录: <data>/{other,fall,knock,scream}/*.wav   （16 kHz 单声道最佳，其它格式会自动重采样/降混）
切窗    : 每个 wav 先算 log-mel，再按 64 帧窗、20 帧跳滑窗，每窗做 (x-mean)/(std+1e-5) 归一化。
划分    : 按【文件】划分训练/验证集 —— 同一个文件的窗绝不会跨集，指标才可信。

输出 best.pt（含 state_dict + 类序 + mel 参数 + 建议阈值），并打印：
  * 窗口级准确率 / 混淆矩阵 / 每类 recall
  * other 被误判成 fall/knock/scream 的误报率（窗口级 + 加后处理后的文件级）
  * 后处理模拟：3/4 投票 + 每类阈值 + 同类不应期 8 s

用法:
  py -3.10 train.py --data D:/apply/claw/_audio_event/datasets --out D:/apply/claw/_audio_event/best.pt
"""

import argparse
import glob
import os
import sys
import time

import numpy as np
import torch
import torch.nn as nn

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dscnn  # noqa: E402
import mel as mel_front  # noqa: E402

torch.set_num_threads(max(1, min(8, (os.cpu_count() or 4))))
torch.set_num_interop_threads(1)

CLASSES = ["other", "fall", "knock", "scream"]      # 固定类序，不可改
NCLASS = len(CLASSES)
VOTE_NEED, VOTE_OF = 3, 4
REFRACTORY_S = 8.0
WINDOW_S = mel_front.NFRAME * mel_front.FRAME_HOP / mel_front.SAMPLE_RATE   # 0.64
HOP_S = mel_front.WIN_HOP * mel_front.FRAME_HOP / mel_front.SAMPLE_RATE     # 0.20


# ---------------------------------------------------------------- 数据

def collect_files(data_dir, max_files_per_class=0, seed=42):
    """扫描四个类目录；max_files_per_class>0 时每类随机抽样，控制内存/训练时间。"""
    rng = np.random.RandomState(seed)
    out = []
    for ci, name in enumerate(CLASSES):
        d = os.path.join(data_dir, name)
        files = sorted(glob.glob(os.path.join(d, "*.wav")) + glob.glob(os.path.join(d, "*.WAV")))
        if max_files_per_class and len(files) > max_files_per_class:
            files = [files[i] for i in sorted(rng.choice(len(files), max_files_per_class,
                                                         replace=False))]
        print("  %-7s %4d 个 wav  <- %s" % (name, len(files), d))
        for f in files:
            out.append((f, ci))
    return out


def file_windows(path, win_per_file=0):
    """wav -> (X (n_win,2560) 已归一化, E (n_win,) 响度代理)。
    不足一窗返回 None。win_per_file>0 时每文件均匀抽这么多窗。
    E 是窗口内 log-mel 的均值（未归一化），用来判断这个窗到底有没有声音。"""
    pcm = mel_front.read_wav_mono16k(path)
    lm = mel_front.log_mel(pcm)
    if lm.shape[0] < mel_front.NFRAME:
        return None
    starts = list(range(0, lm.shape[0] - mel_front.NFRAME + 1, mel_front.WIN_HOP))
    if win_per_file and len(starts) > win_per_file:
        idx = np.unique(np.linspace(0, len(starts) - 1, win_per_file).round().astype(int))
        starts = [starts[i] for i in idx]
    E = np.array([float(lm[s:s + mel_front.NFRAME].mean()) for s in starts])
    X = np.stack([mel_front.normalize_window(lm[s:s + mel_front.NFRAME].T) for s in starts])
    return X.astype(np.float32), E


def build_features(files, win_per_file=0, event_pctile=35):
    """按文件返回 [(path, label, X, dur_s)]，X 为 (n_win,2560)。

    事件类（非 other）会丢掉最安静的 event_pctile% 个窗：ESC-50 这类 5 s 片段里
    事件只占 1 s 左右，其余全是底噪，但整段都被打上事件标签 —— 这些噪声窗是
    "other 被误判成事件" 的主要来源。event_pctile=0 表示不过滤。"""
    out, skipped = [], []
    for path, ci in files:
        try:
            r = file_windows(path, win_per_file)
        except Exception as e:                       # 坏文件不要让整轮训练挂掉
            skipped.append((path, repr(e)))
            continue
        if r is None:
            skipped.append((path, "短于一窗(0.64s)"))
            continue
        X, E = r
        if ci != 0 and event_pctile > 0 and X.shape[0] > 2:
            X = X[E >= np.percentile(E, event_pctile)]
        out.append((path, ci, X, X.shape[0] * HOP_S + WINDOW_S - HOP_S))
    if skipped:
        print("跳过 %d 个文件：" % len(skipped))
        for p, why in skipped[:5]:
            print("   ", os.path.basename(p), why)
    return out


def split_by_file(feats, val_ratio, seed):
    rng = np.random.RandomState(seed)
    tr, va = [], []
    for ci in range(NCLASS):
        idx = np.array([i for i, f in enumerate(feats) if f[1] == ci])
        if idx.size == 0:
            continue
        perm = rng.permutation(idx)
        n_val = max(1, int(round(idx.size * val_ratio))) if idx.size > 1 else 0
        for i in perm[:n_val]:
            va.append(feats[i])
        for i in perm[n_val:]:
            tr.append(feats[i])
    return tr, va


def augment(x):
    """训练期增广（推理/导出完全不受影响）：循环时间平移 + 频率/时间遮挡 + 加噪。
    合成/小数据集上不加这个会严重过拟合（实测验证集能从 0.54 拉到 0.8+）。"""
    B, _, F, T = x.shape
    out = x.clone()
    for i in range(B):
        if np.random.rand() < 0.5:                       # 时间轴循环平移（dim 2 是帧，dim 1 是 mel）
            out[i] = torch.roll(out[i], int(np.random.randint(1, T)), dims=2)
        for _ in range(int(np.random.randint(0, 3))):    # 频率遮挡
            w = int(np.random.randint(2, 6))
            f0 = int(np.random.randint(0, F - w))
            out[i, :, f0:f0 + w, :] = 0.0
        for _ in range(int(np.random.randint(0, 3))):    # 时间遮挡
            w = int(np.random.randint(4, 12))
            t0 = int(np.random.randint(0, T - w))
            out[i, :, :, t0:t0 + w] = 0.0
        if np.random.rand() < 0.5:
            out[i] = out[i] + 0.1 * torch.randn_like(out[i])
    return out


def stack(feats):
    X = np.concatenate([f[2] for f in feats], axis=0)
    y = np.concatenate([np.full(f[2].shape[0], f[1], dtype=np.int64) for f in feats])
    return X, y


# ---------------------------------------------------------------- 后处理模拟

def postprocess(probs, thr):
    """probs (n_win,4) 概率 -> 触发的事件列表 [(win_idx, cls)]，含 3/4 投票 + 不应期。"""
    events, hist, last = [], [], {}
    for t in range(probs.shape[0]):
        hist.append(int(np.argmax(probs[t])))
        hist = hist[-VOTE_OF:]
        for c in range(1, NCLASS):                    # other 不触发事件
            if hist.count(c) >= VOTE_NEED and probs[t, c] >= thr[c]:
                if t * HOP_S - last.get(c, -1e9) >= REFRACTORY_S:
                    events.append((t, c))
                    last[c] = t * HOP_S
    return events


def simulate_files(val_feats, probs_by_file, thr):
    """文件级指标：事件文件是否有命中；other 文件是否误报。"""
    hit = {c: 0 for c in range(1, NCLASS)}
    total = {c: 0 for c in range(1, NCLASS)}
    fa_files, other_total = 0, 0
    for f, p in zip(val_feats, probs_by_file):
        label, evs = f[1], postprocess(p, thr)
        if label == 0:
            other_total += 1
            if evs:
                fa_files += 1
        else:
            total[label] += 1
            if any(c == label for _, c in evs):
                hit[label] += 1
    return hit, total, fa_files, other_total


# ---------------------------------------------------------------- 训练

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default="D:/apply/claw/_audio_event/datasets")
    ap.add_argument("--out", default="D:/apply/claw/_audio_event/best.pt")
    ap.add_argument("--epochs", type=int, default=60)
    ap.add_argument("--batch", type=int, default=32)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--aug", type=int, default=1, help="1=训练期增广(时间平移/遮挡/加噪)")
    ap.add_argument("--val-ratio", type=float, default=0.25)
    ap.add_argument("--max-files", type=int, default=0, help=">0 时每类最多用这么多个文件")
    ap.add_argument("--win-per-file", type=int, default=8, help="每个文件最多取多少窗（0=全部）")
    ap.add_argument("--event-pctile", type=float, default=35.0,
                    help="事件类丢掉的安静窗比例（0=不过滤）")
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    torch.manual_seed(args.seed)
    np.random.seed(args.seed)

    print("扫描数据目录:", args.data)
    files = collect_files(args.data, args.max_files, args.seed)
    if not files:
        raise SystemExit("没有找到任何 wav，先准备 datasets/ 或先用合成数据跑通。")
    feats = build_features(files, args.win_per_file, args.event_pctile)
    if not feats:
        raise SystemExit("所有 wav 都不可用（太短或格式不对）。")
    tr_f, va_f = split_by_file(feats, args.val_ratio, args.seed)
    Xtr, ytr = stack(tr_f)
    Xva, yva = stack(va_f) if va_f else (np.zeros((0, 2560), np.float32), np.zeros(0, np.int64))
    print("训练集: %d 个文件 / %d 个窗   验证集: %d 个文件 / %d 个窗"
          % (len(tr_f), len(ytr), len(va_f), len(yva)))
    for ci, name in enumerate(CLASSES):
        print("    %-7s 训练 %5d 窗  验证 %5d 窗" % (name, int((ytr == ci).sum()), int((yva == ci).sum())))
    missing = [CLASSES[c] for c in range(NCLASS) if int((ytr == c).sum()) == 0]
    if missing:
        print("!! 警告：以下类别没有任何训练数据，对应的输出头是未训练的，不能上板: %s"
              % "、".join(missing))

    device = torch.device("cpu")
    model = dscnn.DSCNNLite().to(device)
    print("参数量:", dscnn.count_params(model))

    # 类别不均衡：按训练窗数的 sqrt 反比加权（比纯反比温和，10:1 不均衡下更稳）
    cnt = np.bincount(ytr, minlength=NCLASS).astype(np.float64)
    wcls = np.where(cnt > 0, np.sqrt(cnt.sum() / np.maximum(cnt, 1.0)), 0.0)
    if (cnt > 0).any():
        wcls = wcls / wcls[cnt > 0].mean()
    print("类别权重:", " ".join("%s=%.3f" % (CLASSES[i], wcls[i]) for i in range(NCLASS)))
    crit = nn.CrossEntropyLoss(weight=torch.tensor(wcls, dtype=torch.float32))

    Xtr_t = torch.from_numpy(Xtr).reshape(-1, 1, mel_front.NMEL, mel_front.NFRAME)
    Xva_t = torch.from_numpy(Xva).reshape(-1, 1, mel_front.NMEL, mel_front.NFRAME)
    ytr_t, yva_t = torch.from_numpy(ytr), torch.from_numpy(yva)

    opt = torch.optim.Adam(model.parameters(), lr=args.lr, weight_decay=1e-3)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, T_max=args.epochs)
    n = Xtr_t.shape[0]
    best = {"acc": float("nan"), "epoch": -1, "vloss": float("inf"), "state": None}
    bad = 0
    t0 = time.time()
    for ep in range(1, args.epochs + 1):
        model.train()
        perm = torch.randperm(n)
        tot = 0.0
        for i in range(0, n, args.batch):
            idx = perm[i:i + args.batch]
            opt.zero_grad()
            loss = crit(model(augment(Xtr_t[idx]) if args.aug else Xtr_t[idx]), ytr_t[idx])
            loss.backward()
            opt.step()
            tot += float(loss) * idx.numel()
        sched.step()

        model.eval()
        with torch.no_grad():
            if yva_t.numel():
                lv = model(Xva_t)
                vacc = float((lv.argmax(1) == yva_t).float().mean())
                vloss = float(nn.functional.cross_entropy(lv, yva_t))
            else:
                vacc, vloss = float("nan"), float("nan")
            tracc = float((model(Xtr_t).argmax(1) == ytr_t).float().mean())
        flag = ""
        cur = vloss if yva_t.numel() else -tracc        # 越小越好；没验证集就按训练准确率选
        if cur < best["vloss"] - 1e-9:
            best = {"acc": vacc, "epoch": ep, "vloss": cur,
                    "state": {k: v.clone() for k, v in model.state_dict().items()}}
            bad = 0
            flag = "  <- best"
        else:
            bad += 1
        if ep % 5 == 0 or flag:
            print("epoch %3d  loss %.4f  train_acc %.4f  val_loss %.4f  val_acc %.4f%s  (%.0fs)"
                  % (ep, tot / n, tracc, vloss, vacc, flag, time.time() - t0))
        if bad >= 12:
            print("验证集 %d 轮没提升，提前停。" % bad)
            break

    model.load_state_dict(best["state"])
    model.eval()
    print("最佳 epoch %d，验证集窗口准确率 %.4f" % (best["epoch"], best["acc"]))

    # ---------------- 评估
    with torch.no_grad():
        ltr = model(Xtr_t).numpy()
        lva = model(Xva_t).numpy() if yva_t.numel() else np.zeros((0, NCLASS))
    def sm(l):
        if l.shape[0] == 0:
            return l
        e = np.exp(l - l.max(1, keepdims=True))
        return e / e.sum(1, keepdims=True)
    ptr, pva = sm(ltr), sm(lva)

    def report(y, p, tag):
        pred = p.argmax(1) if p.shape[0] else np.zeros(0, np.int64)
        cm = np.zeros((NCLASS, NCLASS), np.int64)
        for t, q in zip(y, pred):
            cm[t, q] += 1
        acc = float((pred == y).mean()) if y.size else float("nan")
        print("\n[%s] 窗口级准确率 %.4f" % (tag, acc))
        print("            " + "".join("%9s" % ("预测" + c) for c in CLASSES) + "   样本   recall")
        for i, c in enumerate(CLASSES):
            rec = cm[i, i] / cm[i].sum() if cm[i].sum() else float("nan")
            print("    真实%-6s" % c + "".join("%9d" % v for v in cm[i]) +
                  "  %6d   %.4f" % (cm[i].sum(), rec))
        if cm[0].sum():
            fa = cm[0, 1:].sum()
            print("    other -> fall/knock/scream 窗口级误报: %d/%d = %.4f"
                  % (fa, cm[0].sum(), fa / cm[0].sum()))
        else:
            fa = 0
        return cm, acc, fa

    cm_tr, acc_tr, _ = report(ytr, ptr, "训练集")
    if yva.size:
        cm_va, acc_va, fa_va = report(yva, pva, "验证集")
    else:
        cm_va, acc_va, fa_va = cm_tr, acc_tr, 0

    # ---------------- 阈值搜索 + 后处理模拟（只用验证集，避免过拟合）
    thr = [0.5] * NCLASS
    sim = None
    if yva.size:
        probs_by_file = []
        off = 0
        for f in va_f:
            k = f[2].shape[0]
            probs_by_file.append(pva[off:off + k])
            off += k
        base = simulate_files(va_f, probs_by_file, thr)
        # 每类阈值：让"非该类"的窗口分数超过它的比例 <= 2%（分位数法，不搜索，避免在验证集上过拟合）
        for c in range(1, NCLASS):
            s = pva[yva != c, c]
            if s.size:
                thr[c] = float(np.clip(np.quantile(s, 0.98), 0.30, 0.95))
        print("\n每类阈值(按非该类窗 98 分位取): " +
              " ".join("%s=%.2f" % (CLASSES[i], thr[i]) for i in range(NCLASS)))
        sim = simulate_files(va_f, probs_by_file, thr)
        hit, tot, faf, oft = sim
        print("[验证集·文件级] 加后处理(3/4 投票 + 阈值 + %.0fs 不应期)后:" % REFRACTORY_S)
        for c in range(1, NCLASS):
            print("    %-7s 命中 %d/%d  召回 %.3f" % (CLASSES[c], hit[c], tot[c],
                                                     hit[c] / tot[c] if tot[c] else float("nan")))
        print("    other   误报文件 %d/%d = %.3f   <- 演示最关心的指标" % (faf, oft, faf / max(oft, 1)))
        print("    (统一阈值 0.5 时是 %d/%d)" % (base[2], base[3]))

    # ---------------- 保存
    ckpt = {
        "state_dict": model.state_dict(),
        "classes": CLASSES,
        "mel": {"sr": mel_front.SAMPLE_RATE, "n_fft": mel_front.N_FFT,
                "frame_len": mel_front.FRAME_LEN, "frame_hop": mel_front.FRAME_HOP,
                "nmel": mel_front.NMEL, "nframe": mel_front.NFRAME,
                "win_hop": mel_front.WIN_HOP, "norm_eps": mel_front.NORM_EPS},
        "thresholds": [float(v) for v in thr],
        "vote": {"need": VOTE_NEED, "of": VOTE_OF},
        "refractory_s": REFRACTORY_S,
        "n_train_win": int(ytr.size), "n_val_win": int(yva.size),
        "val_acc_str": "%.4f (窗口级, %d 训练窗 / %d 验证窗)" % (acc_va, ytr.size, yva.size),
        "data_dir": os.path.abspath(args.data),
        "cm_val": cm_va.tolist(),
    }
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    torch.save(ckpt, args.out)
    print("\n已保存:", os.path.abspath(args.out))


if __name__ == "__main__":
    main()
