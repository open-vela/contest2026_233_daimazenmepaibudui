# SF32LB52 音频驱动使用说明

> 智爱陪伴 —— openvela 音频输入输出接口（成员一交付物）
> 硬件：SF32LB52-DevKit-LCD（板载 MEMS 麦克风 + NS4150B Class-D 功放 + 外接喇叭）
> 状态：**播放与录音均已在真机验证通过**（2026-09-10）

## 1. 概述

在 openvela（NuttX）上为 SF32LB52 实现了标准音频设备驱动，注册为 **`/dev/audio/audio0`**，
应用层用 NuttX 标准音频接口操作；另提供 `audio_test` 命令做自检。

- **播放**（喇叭）：`write()` 写入 PCM → **codec 自带 DMA**（AUDCODEC DAC_CH0）→ codec DAC 模拟输出 → NS4150B 功放 → 喇叭
- **录音**（麦克风）：板上 MEMS MIC → codec ADC → AUDPRC RX0 DMA → `read()` 读出 PCM
- 格式：单声道 16bit，采样率 8k / 16k / 44.1k / 48k（默认 16k）

## 2. 硬件通路

```
播放：内存 → codec 自带 DMA(DAC_CH0) → codec DAC 模拟 → NS4150B 功放(PA10 使能) → SPK 喇叭
录音：板上 MEMS MIC → codec ADC 模拟 → AUDPRC RX0 DMA → 内存
```

- **codec（AUDCODEC）**：模拟前端 + DAC/ADC 数字通路，**播放用它自带的 DMA**（不是 AUDPRC）
- **AUDPRC**：数字音频处理器，**仅用于录音 RX0**
- **功放**：NS4150B（Class-D），使能脚 = **PA10 / AU_PA_EN，高电平有效**
- 喇叭接 **SPK**（2.0mm HDR 母座），支持 4Ω/3W（4Ω 更响）

## 3. 应用层用法

```c
#include <nuttx/audio/audio.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

/* ---------- 播放 ---------- */
int fd = open("/dev/audio/audio0", O_WRONLY);

struct audio_caps_desc_s capdesc;
memset(&capdesc, 0, sizeof(capdesc));
capdesc.caps.ac_len           = sizeof(struct audio_caps_s);
capdesc.caps.ac_type          = AUDIO_TYPE_OUTPUT;
capdesc.caps.ac_channels      = 1;
capdesc.caps.ac_controls.hw[0] = 16000;   /* 采样率 */
capdesc.caps.ac_controls.b[2]  = 16;      /* 位深 */
ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&capdesc);
ioctl(fd, AUDIOIOC_START, 0);

write(fd, pcm_buf, pcm_len);              /* 阻塞直到这段 PCM 播完 */

ioctl(fd, AUDIOIOC_STOP, 0);
close(fd);

/* ---------- 录音 ---------- */
fd = open("/dev/audio/audio0", O_RDONLY);
/* 同样方式 CONFIGURE（ac_type = AUDIO_TYPE_INPUT）+ START */
read(fd, pcm_buf, want_len);               /* 阻塞直到采满 */
ioctl(fd, AUDIOIOC_STOP, 0);
close(fd);
```

命令自检（NSH）：

```
nsh> audio_test 2000 1000      # 播放 1kHz 正弦 2000ms
nsh> audio_test record         # 录音 1s（打印 peak/avg 与是否有声音）
```

## 4. 关键实现要点（踩过的坑，改代码前务必先看）

1. **DMAC1 时钟必须使能**：`hal_init` 里要 `HAL_RCC_EnableModule(RCC_MOD_DMAC1)`。
   否则 DMA 启动返回 `HAL_OK` 但**永不传输**，`write()` 返回 0、无声。
2. **DMA 句柄必须在 `HAL_AUDCODEC_Init()` / `HAL_AUDPRC_Init()` 之前挂上**，
   由 HAL 用音频参数（WORD 对齐 + CIRCULAR + 高优先级）初始化；
   且 **`hdma->Instance` 必须填一个合法 DMAC 通道**——
   `DMA_AllocChannel()` 靠 `Instance` 定位通道池，为 NULL 会进 `HAL_ASSERT(0)`（`while(1)`）**死循环黑屏**。
   （通道被占用时分配器会自动改选空闲通道）
3. **模拟级参考源**：播放前必须 `HAL_TURN_ON_PLL()`（内部含 `HAL_AUCODEC_Refgen_Init()`），
   并把 `BG_CFG0.VREF_SEL` 设为 `0xc`（本板 AVDD 按 3.3V）。缺这步 DAC 模拟级无输出，只有爆音。
4. **中断上下文里不要 `printf`**：DMA 完成回调里打印会因控制台锁死锁整机（表现为控制台无响应）。
5. **播放必须走 codec 自带 DMA**（`HAL_AUDCODEC_Transmit_DMA(codec, buf, len, HAL_AUDCODEC_DAC_CH0)`），
   完成回调 `HAL_AUDCODEC_TxCpltCallback` 里 post 信号量；
   **不要用 AUDPRC TX → codec** 这条路（在 SF32LB52X 上实测无输出）。

## 5. 注意事项与限制

- 功放使能 PA10 **高有效**；喇叭插在 SPK 座，需 **USB 供电**（功放由板上 5V 供电）。
- 默认音量 `SF32LB52_AUDIO_DEFAULT_VOL`（-6dB），可调整。
- `AUDIOIOC_STOP` 会同时关闭播放与录音通路，**不要依赖"同时全双工"**；
  需要连续播放时请顺序调用（一个进程写、另一个进程读会出现互相打断）。
- 无声排查顺序：① `ls /dev/audio/audio0` 是否存在 → ② `audio_test 2000 1000` 是否有 `WRITE done: N of N`
  → ③ 换一副耳机/喇叭或量 SPK 座静态电压（Class-D BTL 约为 VDD/2）判断硬件。

## 6. 落地方式（队友如何获取）

驱动不在 upstream `vendor/sifli` 里，需要打补丁（仓库 `patches/`）：

```bash
cd <openvela 工作区>/vendor/sifli
git apply <本仓库>/patches/vendor_sifli-boot-fixes.patch    # 上游编译/启动修复（chips/）
git apply <本仓库>/patches/vendor_sifli-audio-driver.patch   # 音频驱动（boards/）
```

注意：`vendor_sifli-audio-driver.patch` 里的 `sifli_ap.c` 还带有**实验性的 USB RNDIS 初始化钩子**
（与音频无关，如不需要可自行删掉那几行 include 与调用）。

板级 defconfig 需要：`CONFIG_AUDIO=y`、`CONFIG_EXAMPLES_AUDIO_TEST=y`（见本仓库 `board/.../defconfig`）。

## 7. 验证记录（2026-09-10，真机）

```
nsh> audio_test 2000 1000
audio_test: play 1000Hz 2000ms via /dev/audio/audio0
WRITE done: 64000 of 64000 bytes        ← 16k 单声道 16bit × 2s
nsh> audio_test record
READ done: 32000 of 32000 bytes
RECORD peak=1636 avg=43 → RECORD OK（检测到声音）
```

寄存器回读（播放启动后）：`PLL_STAT=0`（PLL 已锁）、`CFG=0x1f`、`DAC1_CFG=0x01f78001`（DAC1 内部功放使能）。
