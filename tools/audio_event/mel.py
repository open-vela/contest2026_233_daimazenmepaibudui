"""log-mel 前端（PC 参考实现）—— 严格按接口契约实现。

契约参数（动任何一项都必须同步改板端 sound_event.c）:
  * 16 kHz / 单声道 / s16le
  * 帧长 400 样本 (25 ms)，帧跳 160 样本 (10 ms)，乘 symmetric Hann 窗 (np.hanning(400))
  * 512 点 FFT -> 功率谱 257 点（= |X|^2，不除 N）
  * 40 个三角 mel 滤波器，f_min = 0 Hz，f_max = 8000 Hz，峰值归一化到 1.0（不做面积归一化）
    频率刻度用 HTK 公式 mel = 2595 * log10(1 + f / 700)
  * 每帧取 log(max(x, 1e-10))（自然对数 ln）
  * 一个窗口 = 连续 64 帧 = 0.64 s；滑窗步长 20 帧 = 200 ms
  * 归一化：对整个 40x64 窗口做 (x - mean) / (std + 1e-5)（mean/std 是总体统计量，ddof=0）
  * 输出布局：mel[m * 64 + t]，m = mel 维 0..39，t = 帧 0..63（即行主序，行是 mel 维）

关于 FFT 的缩放系数：log 之后整体缩放只是常数平移，会被窗口归一化完全消掉，
所以 |X|^2 要不要除 N 无所谓，C 侧不必和这里逐位一致；但 mel 滤波器形状必须一致。
"""

import wave

import numpy as np

SAMPLE_RATE = 16000
N_FFT = 512
N_BINS = N_FFT // 2 + 1          # 257
FRAME_LEN = 400
FRAME_HOP = 160
FMIN = 0.0
FMAX = 8000.0

NMEL = 40
NFRAME = 64
WIN_HOP = 20                     # 窗口滑窗步长（帧）
MEL_LEN = NMEL * NFRAME          # 2560

LOG_FLOOR = 1e-10
NORM_EPS = 1e-5

_fb_cache = None
_hann_cache = None


def hz_to_mel(f):
    return 2595.0 * np.log10(1.0 + np.asarray(f, dtype=np.float64) / 700.0)


def mel_to_hz(m):
    return 700.0 * (10.0 ** (np.asarray(m, dtype=np.float64) / 2595.0) - 1.0)


def hann_window():
    """symmetric Hann 窗，长度 400。"""
    global _hann_cache
    if _hann_cache is None:
        _hann_cache = np.hanning(FRAME_LEN).astype(np.float64)
    return _hann_cache


def mel_filterbank():
    """(40, 257) 三角滤波器组，float32。线性插值在 Hz 域做，峰值为 1。"""
    global _fb_cache
    if _fb_cache is None:
        freqs = np.arange(N_BINS, dtype=np.float64) * (SAMPLE_RATE / N_FFT)
        edges = mel_to_hz(np.linspace(hz_to_mel(FMIN), hz_to_mel(FMAX), NMEL + 2))
        fb = np.zeros((NMEL, N_BINS), dtype=np.float64)
        for m in range(NMEL):
            lo, ce, hi = edges[m], edges[m + 1], edges[m + 2]
            up = (freqs - lo) / (ce - lo)
            down = (hi - freqs) / (hi - ce)
            fb[m] = np.clip(np.minimum(up, down), 0.0, None)
        _fb_cache = fb.astype(np.float32)
    return _fb_cache


def frame_power(pcm):
    """PCM(float/int) -> (n_frames, 257) 功率谱，float64。"""
    x = np.asarray(pcm, dtype=np.float64).reshape(-1)
    if x.size < FRAME_LEN:
        return np.zeros((0, N_BINS), dtype=np.float64)
    n = 1 + (x.size - FRAME_LEN) // FRAME_HOP
    idx = np.arange(FRAME_LEN)[None, :] + FRAME_HOP * np.arange(n)[:, None]
    spec = np.fft.rfft(x[idx] * hann_window(), n=N_FFT, axis=1)
    return spec.real ** 2 + spec.imag ** 2


def log_mel(pcm):
    """PCM -> (n_frames, 40) log-mel，float32（未归一化）。"""
    p = frame_power(pcm)
    if p.shape[0] == 0:
        return np.zeros((0, NMEL), dtype=np.float32)
    m = p @ mel_filterbank().T.astype(np.float64)
    return np.log(np.maximum(m, LOG_FLOOR)).astype(np.float32)


def normalize_window(frames_40xT):
    """(40, 64) log-mel -> (2560,) float32，已做 (x-mean)/(std+1e-5)，行主序 mel[m*64+t]。"""
    x = np.asarray(frames_40xT, dtype=np.float64)
    y = (x - x.mean()) / (x.std() + NORM_EPS)
    return y.reshape(-1).astype(np.float32)


def window_from_frames(logmel, start_frame):
    """从 (n_frames, 40) 里切出 [start_frame, start_frame+64) 并归一化 -> (2560,)。"""
    seg = logmel[start_frame:start_frame + NFRAME]
    if seg.shape[0] != NFRAME:
        raise ValueError("需要 %d 帧，只有 %d 帧" % (NFRAME, seg.shape[0]))
    return normalize_window(seg.T)


def iter_windows(logmel, hop=WIN_HOP):
    """滑窗遍历，yield (start_frame, (2560,) float32)。"""
    n = logmel.shape[0]
    for s in range(0, n - NFRAME + 1, hop):
        yield s, normalize_window(logmel[s:s + NFRAME].T)


def resample_linear(x, sr_in, sr_out):
    """线性插值重采样（stdlib 级实现，避免引入 scipy）。"""
    if sr_in == sr_out or len(x) == 0:
        return x
    n_out = int(round(len(x) * float(sr_out) / sr_in))
    t = np.arange(n_out, dtype=np.float64) * (float(sr_in) / sr_out)
    i0 = np.clip(np.floor(t).astype(np.int64), 0, len(x) - 1)
    i1 = np.clip(i0 + 1, 0, len(x) - 1)
    frac = t - i0
    return x[i0] * (1.0 - frac) + x[i1] * frac


def read_wav_mono16k(path, target_sr=SAMPLE_RATE):
    """读 wav -> float32 单声道 16 kHz 波形（幅度范围约 +-32768）。"""
    with wave.open(str(path), "rb") as w:
        nch = w.getnchannels()
        sw = w.getsampwidth()
        sr = w.getframerate()
        raw = w.readframes(w.getnframes())
    if sw != 2:
        raise ValueError("%s: 只支持 16bit PCM wav（sampwidth=%d）" % (path, sw))
    x = np.frombuffer(raw, dtype="<i2").astype(np.float64)
    if nch > 1:
        x = x[: (len(x) // nch) * nch].reshape(-1, nch).mean(axis=1)
    if sr != target_sr:
        x = resample_linear(x, sr, target_sr)
    return x.astype(np.float32)


def pcm_from_bytes(buf):
    """s16le 原始字节 -> float32 波形。"""
    x = np.frombuffer(buf, dtype="<i2").astype(np.float64)
    return x.astype(np.float32)


if __name__ == "__main__":
    # 自检：滤波器组形状 / 帧数 / 两次结果一致性
    import struct
    sr = SAMPLE_RATE
    t = np.arange(sr * 1.0) / sr
    tone = (8000.0 * np.sin(2 * np.pi * 1000.0 * t)).astype("<i2")
    lm = log_mel(pcm_from_bytes(tone.tobytes()))
    print("滤波器组形状:", mel_filterbank().shape, "能量和/sqrt:",
          float(np.sqrt((mel_filterbank() ** 2).sum())))
    print("1s 音频 ->", lm.shape, "帧（期望 (98, 40)）")
    w = list(iter_windows(lm))
    print("窗口数:", len(w), "窗口长度:", w[0][1].shape, "dtype:", w[0][1].dtype)
    a = window_from_frames(lm, 0)
    b = window_from_frames(lm, 0)
    print("两次结果完全一致:", bool(np.array_equal(a, b)), " mean=%.6f std=%.6f" % (a.mean(), a.std()))
    assert mel_filterbank().shape == (NMEL, N_BINS)
    assert lm.shape[0] == 1 + (sr - FRAME_LEN) // FRAME_HOP
