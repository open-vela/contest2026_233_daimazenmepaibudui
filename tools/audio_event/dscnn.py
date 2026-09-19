"""DSCNN-lite —— 按接口契约搭的 PyTorch 模型 + 导出用的 BN 折叠。

网络结构（输入 1x40x64，40 是 mel 维、64 是帧数）:
  c1  conv 3x3 pad1 stride2 1->16   -> 16x20x32  +ReLU
  dw2 depthwise 3x3 pad1 s1 16->16  -> 16x20x32  +ReLU
  pw2 conv 1x1 16->32               -> 32x20x32  +ReLU
  dw3 depthwise 3x3 pad1 s1 32->32  -> 32x20x32  +ReLU
  pw3 conv 1x1 32->32               -> 32x20x32  +ReLU
  gap 全局平均池化(空间)            -> 32
  fc  32->4

训练时每个 conv 后面挂 BatchNorm2d 帮助收敛；导出时 fold_bn() 把 BN 折进 conv
的权重和偏置，所以 C 侧只有 conv + bias + ReLU，没有任何 BN 参数。

类序固定：0=other 1=fall 2=knock 3=scream
"""

import numpy as np
import torch
import torch.nn as nn

SE_NMEL = 40
SE_NFRAME = 64
SE_NCLASS = 4
SE_W_LEN = 2372

# 导出顺序（全部 float32 行主序）：name, 元素个数
EXPORT_LAYOUT = [
    ("c1", 144), ("b1", 16),
    ("dw2", 144), ("bdw2", 16),
    ("pw2", 512), ("bpw2", 32),
    ("dw3", 288), ("bdw3", 32),
    ("pw3", 1024), ("bpw3", 32),
    ("fc", 128), ("bfc", 4),
]


class ConvBN(nn.Module):
    """conv -> BN -> ReLU，导出时折成 conv(带 bias) -> ReLU。"""

    def __init__(self, cin, cout, k, stride=1, pad=0, groups=1):
        super().__init__()
        self.conv = nn.Conv2d(cin, cout, k, stride=stride, padding=pad, groups=groups, bias=True)
        self.bn = nn.BatchNorm2d(cout)

    def forward(self, x):
        return torch.relu(self.bn(self.conv(x)))


class DSCNNLite(nn.Module):
    def __init__(self, nclass=SE_NCLASS, drop=0.3):
        super().__init__()
        self.c1 = ConvBN(1, 16, 3, stride=2, pad=1)
        self.dw2 = ConvBN(16, 16, 3, pad=1, groups=16)
        self.pw2 = ConvBN(16, 32, 1)
        self.dw3 = ConvBN(32, 32, 3, pad=1, groups=32)
        self.pw3 = ConvBN(32, 32, 1)
        self.drop = nn.Dropout(drop)     # 只在训练时生效，导出/推理是恒等，不影响权重数量
        self.fc = nn.Linear(32, nclass)

    def forward(self, x):
        x = self.c1(x)
        x = self.dw2(x)
        x = self.pw2(x)
        x = self.dw3(x)
        x = self.pw3(x)
        x = x.mean(dim=(2, 3))              # gap：空间维全局平均 -> (B, 32)
        return self.fc(self.drop(x))


def count_params(model):
    return sum(p.numel() for p in model.parameters())


def _fold_convbn(block):
    """把 BN 折进 conv：W' = W * s，b' = (b - mean) * s + beta，s = gamma / sqrt(var + eps)。"""
    w = block.conv.weight.detach().numpy().astype(np.float64)
    b = block.conv.bias.detach().numpy().astype(np.float64)
    bn = block.bn
    gamma = bn.weight.detach().numpy().astype(np.float64)
    beta = bn.bias.detach().numpy().astype(np.float64)
    mean = bn.running_mean.detach().numpy().astype(np.float64)
    var = bn.running_var.detach().numpy().astype(np.float64)
    s = gamma / np.sqrt(var + float(bn.eps))
    return (w * s.reshape(-1, 1, 1, 1)).astype(np.float32), ((b - mean) * s + beta).astype(np.float32)


def export_weights(model):
    """-> [(name, ndarray)]，顺序与契约一致，每个都是行主序 float32。"""
    model.eval()
    out = []
    for name, bname, blk in (("c1", "b1", model.c1), ("dw2", "bdw2", model.dw2),
                             ("pw2", "bpw2", model.pw2), ("dw3", "bdw3", model.dw3),
                             ("pw3", "bpw3", model.pw3)):
        w, b = _fold_convbn(blk)
        out.append((name, w))
        out.append((bname, b))
    out.append(("fc", model.fc.weight.detach().numpy().astype(np.float32)))
    out.append(("bfc", model.fc.bias.detach().numpy().astype(np.float32)))

    for (name, want), (got_name, arr) in zip(EXPORT_LAYOUT, out):
        assert name == got_name, (name, got_name)
        assert arr.size == want, "权重 %s 元素数 %d != %d" % (name, arr.size, want)
    return out


def weights_flat(model):
    """-> (2372,) float32，可直接按契约顺序写进 C 数组。"""
    flat = np.concatenate([a.reshape(-1) for _, a in export_weights(model)]).astype(np.float32)
    assert flat.size == SE_W_LEN, flat.size
    return flat


if __name__ == "__main__":
    m = DSCNNLite()
    x = torch.zeros(1, 1, SE_NMEL, SE_NFRAME)
    print("输出形状:", tuple(m(x).shape), " 前向后空间维 (应为 20x32):", tuple(m.c1(x).shape))
    print("参数量:", count_params(m), " 导出展平长度:", weights_flat(m).size, "(应为", SE_W_LEN, ")")
    assert tuple(m(x).shape) == (1, SE_NCLASS)
