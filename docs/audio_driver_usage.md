# SF32LB52 音频驱动使用说明

> 智爱陪伴 —— openvela 音频输入输出接口（成员一交付物）
> 硬件：SF32LB52-DevKit-LCD（板载 MEMS 麦克风 + NS4150B Class-D 功放 + 外接喇叭）
> 状态：**播放与录音均已在真机验证通过**（2026-09-10）
> 录音的应用层封装见 **3.1 节**（`audio_in_start/read/stop`，可直接照抄），
> 坑见 **3.2 节**；命令行自检：`audio_test record` / `hw_test audio`

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

> **功放型号说明**：`patches/README.md` 和驱动源码注释
> （`board/contest_board/src/sf32lb52_audio.c:213,390`）把功放写成了 **AW8155**，
> 属于早期笔误；**功放型号以板载 NS4150B 为准**（PA10 使能，高有效）。
>
> 另外注意节点路径：驱动里是 `audio_register("audio0")`，内核会拼成
> **`/dev/audio/audio0`**（不是 `/dev/audio0`）。

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
/* 完整封装见 3.1 节：audio_in_start() / audio_in_read() / audio_in_stop() */
```

命令自检（NSH）：

```
nsh> audio_test 2000 1000      # 播放 1kHz 正弦 2000ms
nsh> audio_test record         # 录音 1s（打印 peak/avg 与是否有声音）
nsh> hw_test audio 2           # 录 2 秒，打印 peak/avg 和"是否检测到声音"，不写文件
```

## 3.1 录音：三个可直接照抄的函数

成员二的 `app/hello_app/ai_audio.c` 里 open/ioctl/音量**全是注释**、
`record_fd = 1` 是占位，所以**用不了**。下面这三个函数是能直接抄的版本
（`app/audio_test/main.c` 是已经在真机跑通的参考实现，
`app/hw_test/main.c` 的 `hw_test audio` 也用同一套）。

固定参数：**16 kHz / 单声道 / 16bit 小端（s16le）**，设备路径
**`/dev/audio/audio0`（带 `audio/` 子目录）**。

```c
/* audio_in.c —— 录音封装 */
#include <nuttx/audio/audio.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#define AUDIO_IN_DEV       "/dev/audio/audio0"   /* 注意：不是 /dev/audio0 */
#define AUDIO_IN_RATE      16000                 /* Hz */
#define AUDIO_IN_CHANNELS  1
#define AUDIO_IN_BITS      16
#define AUDIO_IN_CHUNK     (AUDIO_IN_RATE * 2)   /* 1 秒 = 32000 字节，见下面说明 */

static int audio_in_fd = -1;

/* 打开 + 配置 + START。返回 0 成功，<0 失败。 */
int audio_in_start(void)
{
  struct audio_caps_desc_s capdesc;

  if (audio_in_fd >= 0)
    {
      return 0;                                /* 已经开着 */
    }

  audio_in_fd = open(AUDIO_IN_DEV, O_RDONLY);
  if (audio_in_fd < 0)
    {
      printf("open %s failed: %d\n", AUDIO_IN_DEV, audio_in_fd);
      return -1;
    }

  memset(&capdesc, 0, sizeof(capdesc));
  capdesc.caps.ac_len            = sizeof(struct audio_caps_s);
  capdesc.caps.ac_type           = AUDIO_TYPE_INPUT;   /* 录音 */
  capdesc.caps.ac_channels       = AUDIO_IN_CHANNELS;
  capdesc.caps.ac_controls.hw[0] = AUDIO_IN_RATE;      /* 采样率 */
  capdesc.caps.ac_controls.b[2]  = AUDIO_IN_BITS;      /* 位深 */

  if (ioctl(audio_in_fd, AUDIOIOC_CONFIGURE, (unsigned long)&capdesc) < 0)
    {
      printf("AUDIOIOC_CONFIGURE failed: %d\n", errno);
      close(audio_in_fd);
      audio_in_fd = -1;
      return -1;
    }

  if (ioctl(audio_in_fd, AUDIOIOC_START, 0) < 0)
    {
      printf("AUDIOIOC_START failed: %d\n", errno);
      close(audio_in_fd);
      audio_in_fd = -1;
      return -1;
    }

  return 0;
}

/* 阻塞读 len 字节 PCM。
 * 返回实际读到的字节数；**返回 0 表示被打断/出错**，不是"读到 0 字节数据"。
 * 上层要录 N 秒，就循环调用，每次不超过 AUDIO_IN_CHUNK。 */
ssize_t audio_in_read(void *buf, size_t len)
{
  if (audio_in_fd < 0 || buf == NULL || len == 0)
    {
      return -1;
    }

  return read(audio_in_fd, buf, len);
}

/* STOP + close。read 正阻塞时，这一步能把它唤醒（见 3.2 坑 2）。 */
void audio_in_stop(void)
{
  if (audio_in_fd >= 0)
    {
      ioctl(audio_in_fd, AUDIOIOC_STOP, 0);
      close(audio_in_fd);
      audio_in_fd = -1;
    }
}
```

### 缓冲区大小怎么算

- 16k 单声道 16bit = **每秒 32000 字节**（`16000 × 1 × 2`）。
- 要录 N 秒，一次 `malloc(N * 32000)`：
  2 秒 = 64 KB，10 秒 = 320 KB（板子有 PSRAM，但别一次 malloc 太大，
  建议**边录边处理**，比如分块送给语音识别）。
- **单次 `audio_in_read()` 不要超过 1 秒（32000 字节）**，原因见坑 3。
  正确的循环是：

```c
int nsamples = 16000 * seconds;
int16_t *buf = malloc(nsamples * sizeof(int16_t));
int offset = 0;

audio_in_start();
while (offset < nsamples * 2)
  {
    int chunk = nsamples * 2 - offset;
    ssize_t n;

    if (chunk > AUDIO_IN_CHUNK)
      {
        chunk = AUDIO_IN_CHUNK;               /* 1 秒一块 */
      }

    n = audio_in_read((char *)buf + offset, chunk);
    if (n <= 0)                               /* 0 / 负值都必须跳出，否则死循环 */
      {
        break;
      }

    offset += (int)n;
  }

audio_in_stop();
free(buf);
```

### 阻塞语义

- `read()`（以及 `audio_in_read()`）**会一直阻塞到读满你要求的字节数**，
  期间任务处于等待状态，不占 CPU。
- 驱动下层一次 read 内部还有一个 **5 秒超时**
  （`board/contest_board/src/sf32lb52_audio.c:1130`
  的 `nxsem_tickwait_uninterruptible(..., MSEC2TICK(5000))`），
  超时会**返回 0**，所以"读 6 秒"必须拆成 6 次 1 秒，不能一次读（坑 3）。
- **从别的任务发 `AUDIOIOC_STOP` 能把阻塞中的 read 唤醒**
  （驱动已修：`sf32lb52_audio.c:665-721` 的 `sf32lb52_audio_hw_stop()`，
  先 `nxsem_post(&priv->rx_sem)`（`:686`），再把还挂在 DMA 上的 buffer
  通过 `AUDIO_CALLBACK_DEQUEUE` 还给上层（`:698-704`），
  于是 read 返回 0 而不是一直等满）。
  这也是做"录音必须带超时/必须能取消"的基础。
- `read` 返回 **0 不是错误码，而是"这次没读到任何字节"**（被打断或超时），
  上层必须跳出循环；返回正数才是读到的字节数。

### 不能同时录放

`AUDIOIOC_STOP` 会**同时关掉播放和录音两条通路**
（`sf32lb52_audio_hw_stop()` 里 TX/RX DMA 一起停，
`sf32lb52_audio.c:667-668`），而且 codec 通路在硬件上也是分时复用的。
所以：
- 不要一个任务 `write()`、另一个任务 `read()` 想全双工；
- 要"边说边听"（打断识别）请**顺序做**：先录完 → 停止 → 再播。
- 播放和录音各自 `open()` 的时候注意：同一个 `/dev/audio/audio0`，
  但**不能用同一个 fd 同时读写**。

## 3.2 录音的坑（按重要性排序）

1. **设备路径带子目录**：是 `/dev/audio/audio0`。
   写成 `/dev/audio0` 一定 `open` 失败（`audio_register("audio0")` 的结果，
   见 `sf32lb52_audio.c:1205`）。
2. **`read` 返回 0 要立刻跳出**。0 = 被 `AUDIOIOC_STOP` 打断，
   或下层 5 秒超时，或 `!priv->running` 的提前返回
   （`sf32lb52_audio.c:1107-1112`）。**别把 0 当成"这次没数据、继续读"**，
   否则就是死循环。
3. **单次 read 不要超过 5 秒**（下层 `rx_sem` 超时是 5 秒，
   `sf32lb52_audio.c:1130`）。建议一律拆成 1 秒（32000 字节）一块，
   这也是 `audio_test` 验证过的大小。
4. **必须能取消**：录音任务阻塞在 read 里时，只能由**另一个任务**
   发 `AUDIOIOC_STOP` 来救。所以"录 N 秒"要带看门狗：
   起一个读任务，主任务等 N + 余量秒，超时就 STOP。
   `app/audio_test/main.c` 的 `stopwait` 和 `app/hw_test/main.c` 的
   `step_audio()` 都是这么写的。
5. **不要在中断/DMA 回调里 printf**（会因控制台锁死锁整机，
   见下面第 4 节第 4 条）。
6. **STOP 后要重新 CONFIGURE + START** 才能再录（`audio_test loop` 验证过
   可以连续 open/config/start/stop/close）。
7. 音量接口用 `AUDIOIOC_CONFIGURE` + `AUDIO_TYPE_FEATURE` +
   `AUDIO_FU_VOLUME`（`ac_controls.hw[0]` = 0..1000，
   0 = -36dB、1000 = +6dB），录音时它改的是**麦克风数字增益**；
   参考 `audio_test vol <0..1000>`。

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
