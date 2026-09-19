# SF32LB52 音频驱动使用说明

> 智爱陪伴 —— openvela 音频输入输出接口（成员一交付物）
> 硬件：SF32LB52-DevKit-LCD（板载 MEMS 麦克风 + NS4150B Class-D 功放 + 外接喇叭）
> 状态：**播放与录音均已在真机验证通过**；录音长跑断流问题（2026-09-15 定案）
> **已在 2026-09-16 的重写里消除**（板上实测：连着录 180 秒零异常、20/20 轮起停
> 无 EBUSY、录-放-录半双工全过）—— 当时的调查记录见**第 11 节**，现在**当历史读**。
>
> ⚠️ **第 11 节（录音长跑后 RX 永久断流）是 2026-09-15 当时的现场记录**：那套
> "每帧 arm/abort + 分片等待 + 三级恢复"的实现已经在 2026-09-16 整个删掉，
> 现在驱动里只有**一条路径**（一次成型 read，形状见驱动文件顶部"录音 RX 通路"那段）。
> 拿那张表判读**今天的**日志会读错 —— 仪表字段（`since_ok_ms`/`d_*`/`evt`/`lost`
> 等）随那套代码一起没了。
>
> 录音的应用层封装是板级模块 `board/contest_board/src/sf32lb52_audio_in.{h,c}`
> （`audio_in_start/read/stop/abandon`，接口与示例见 **第 9 节**），原理与坑见 **3.1 / 3.2 节**；
> 命令行自检：`audio_test record` / `hw_test audio`（后者已改成调这个封装）
>
> 本文里的 `sf32lb52_audio.c:` 行号按当前 HEAD 标注，会随提交漂移；
> 对不上时**以函数名/注释为准**（驱动正文里的注释比行号详细得多）。

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

## 3.1 录音：用板级封装 `audio_in_*`

`app/hello_app/ai_audio.c` 里的录音以前是假的（open/ioctl 全是注释、
`record_fd = 1` 占位、`memset` 造静音），**2026-09-13 已经改成调下面这个板级封装**
（详见第 10 节），不再是"用不了"的状态：

- 头文件：`board/contest_board/src/sf32lb52_audio_in.h`
- 实现：`board/contest_board/src/sf32lb52_audio_in.c`（已加入板级 `CMakeLists.txt`）
- 接口语义、参数约束与"录 3 秒存文件"的完整示例：**第 9 节**

固定参数：**16 kHz / 单声道 / 16bit 小端（s16le）**，设备路径
**`/dev/audio/audio0`（带 `audio/` 子目录）**。

四个函数（签名与本模块头文件一致）：

```c
int     audio_in_start(int sample_rate, int channels, int bits);  /* 0 = 成功 */
ssize_t audio_in_read(FAR void *buf, size_t len);                 /* 实际字节数 */
int     audio_in_stop(void);                                      /* 没录时是空操作 */
int     audio_in_abandon(void);   /* 会话不是本层停的（read 返回 0）时的收尾入口 */
```

典型用法（**1 秒一块读；`0` 要跳出，`-1` + `errno=ETIMEDOUT` 要接着读**，
需 `#include <errno.h>`）：

```c
int16_t *buf = malloc(nsamples * sizeof(int16_t));
int offset = 0;

audio_in_start(16000, 1, 16);
while (offset < nsamples * 2)
  {
    int chunk = nsamples * 2 - offset;
    ssize_t n;

    if (chunk > AUDIO_IN_CHUNK_BYTES)          /* 1 秒 = 32000 字节 */
      {
        chunk = AUDIO_IN_CHUNK_BYTES;
      }

    errno = 0;                                 /* 判超时只看 errno，必须先清 0 */
    n = audio_in_read((char *)buf + offset, chunk);

    if (n == 0)                                /* 0 = 会话结束（被 STOP 打断）→ 跳出 */
      {
        break;
      }

    if (n < 0)                                 /* 负值：只有超时才是"跳过接着读" */
      {
        if (errno != ETIMEDOUT)
          {
            break;                             /* 别的错误才算这一代会话坏了 */
          }

        continue;                              /* 这一帧没等到数据，接着读下一帧 */
      }

    offset += (int)n;
  }

/* 自己把设备停掉时用 audio_in_stop()；循环若是"被别人抢走/停掉"（read 返回 0）
 * 而退出的，收尾该用 audio_in_abandon() —— 分工见第 9.1 节。 */
audio_in_stop();
free(buf);
```

`app/audio_test/main.c` 是已经在真机跑通的参考实现，`app/hw_test/main.c` 的
`hw_test audio` 现在也改成调这套函数，所以那次自检本身就是对这个封装的验证。

### 缓冲区大小怎么算

- 16k 单声道 16bit = **每秒 32000 字节**（`16000 × 1 × 2`）。
- 要录 N 秒，一次 `malloc(N * 32000)`：
  2 秒 = 64 KB，10 秒 = 320 KB（板子有 PSRAM，但别一次 malloc 太大，
  建议**边录边处理**，比如分块送给语音识别）。
- **单次 `audio_in_read()` 不要超过 1 秒（32000 字节）**，原因见 3.2 坑 3。
  循环写法见 3.1 节；封装头文件里的 `AUDIO_IN_CHUNK_BYTES` 就是这 1 秒的上限。
- 缓冲**别放栈上**：默认任务栈常常只有几 KB，32 KB 一块直接爆栈；用 `malloc`
  （完整示例见第 9 节）。

### 阻塞语义

- `read()`（以及 `audio_in_read()`）**会一直阻塞到读满你要求的字节数**，
  期间任务处于等待状态，不占 CPU。
- 驱动下层一次 read 内部还有一个 **5 秒上限**（一次等满：`sf32lb52_audio_read()`
  里那次 `nxsem_clockwait_uninterruptible`，预算就是文件顶部的
  `SF32LB52_AUDIO_RX_READ_TIMEOUT_MS`）。
  **超时不再返回 0**：它返回 `-ETIMEDOUT`（经 POSIX `read()` 那层变成 `-1` +
  `errno=110`，驱动日志里就是 `ret=-110`），语义是"**这一次没等到数据、会话还活着**"；
  返回 **0** 才是"会话真的结束了"（被 STOP 打断 / 设备没在跑 / 换了新会话）。
  两者必须分开处理，见 3.2 坑 2。"读 6 秒"仍然必须拆成 6 次 1 秒，不能一次读（坑 3）。
- **从别的任务发 `AUDIOIOC_STOP` 能把阻塞中的 read 唤醒**
  （驱动已修：`sf32lb52_audio_hw_stop()` 里先 `nxsem_post(&priv->rx_sem)`
  把等待放掉，再把还挂在 DMA 上的 buffer 通过 `AUDIO_CALLBACK_DEQUEUE`
  还给上层，于是 read 返回 0 而不是一直等满）。
  这也是做"录音必须带超时/必须能取消"的基础。
  用 3.1 的封装时，就是**从别的任务调 `audio_in_stop()`**（内部先
  `AUDIOIOC_STOP` 再 `close`）；因为 `audio_in_read()` 阻塞期间**不持锁**，
  这个"救场"调用不会被锁挡住（见第 9 节）。
- `read` 返回 **0 不是超时、而是"这一代会话结束了"**，上层必须跳出循环；
  返回正数是读到的字节数；返回 `-1` 且 `errno=ETIMEDOUT` 是"这一次没等到数据"，
  跳过这一帧接着读（**别拿它去拆设备重录**）。

### 不能同时录放

`AUDIOIOC_STOP` 会**同时关掉播放和录音两条通路**
（`sf32lb52_audio_hw_stop()` 里 TX/RX DMA 一起停），
而且 codec 通路在硬件上也是分时复用的。
所以：
- 不要一个任务 `write()`、另一个任务 `read()` 想全双工；
- 要"边说边听"（打断识别）请**顺序做**：先录完 → 停止 → 再播。
- 播放和录音各自 `open()` 的时候注意：同一个 `/dev/audio/audio0`，
  但**不能用同一个 fd 同时读写**。

## 3.2 录音的坑（按重要性排序）

1. **设备路径带子目录**：是 `/dev/audio/audio0`。
   写成 `/dev/audio0` 一定 `open` 失败（`audio_register("audio0")` 的结果，
   见 `sf32lb52_audio.c` 的 `audio_register()` 那行）。
2. **`read` 的 0 和负值语义不同，别混**：
   - **0 = 这一代会话结束了**（被 `AUDIOIOC_STOP` 打断 / 设备没在跑 /
     换了新会话）→ **立刻跳出**，别再当"这次没数据"继续读，否则就是死循环；
   - **`-1` + `errno=ETIMEDOUT`（110）= 这次没等到数据、会话还活着** →
     **跳过这一帧接着读**。真机上大约 10% 的读会走到这里，把它当"会话死了"
     去拆设备重录，就会每 5 秒拆一次、一句话永远攒不齐（这正是修过的老 bug）。
   调 `audio_in_read()` 前**先把 `errno` 清 0**，读后立刻取 `errno` 快照再判
   （`sf32lb52_audio_in.c` 只是把 POSIX `read()` 的结果原样返回，超时判据全在
   `errno` 上）；"连续超时到上限才算会话死"的完整策略见第 9 节。
3. **单次 read 不要超过 5 秒**（下层一次等待的上界就是 5 秒）。
   建议一律拆成 1 秒（32000 字节）一块，这也是 `audio_test` 验证过的大小。
4. **必须能取消**：录音任务阻塞在 read 里时，只能由**另一个任务**
   发 `AUDIOIOC_STOP` 来救。所以"录 N 秒"要带看门狗：
   起一个读任务，主任务等 N + 余量秒，超时就 STOP。
   `app/audio_test/main.c` 的 `stopwait` 和 `app/hw_test/main.c` 的
   `step_audio()` 都是这么写的。
5. **不要在中断/DMA 回调里 printf**（会因控制台锁死锁整机，
   见下面第 4 节第 4 条）。
6. **STOP 后要重新 CONFIGURE + START** 才能再录（`audio_test loop` 验证过
   可以连续 open/config/start/stop/close）。但**别把"反复重开会话"当零成本**：
   "每帧 DMA abort/re-arm + 每帧把 ADC 通路掰断一次"正是第 11 节那个 RX 永久
   断流问题的嫌疑根因，长跑或高频重开会话前先读第 11 节。
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

**板级驱动就在本比赛仓库里，不需要打任何板级补丁。**

| 内容 | 位置 |
|------|------|
| 播放 + 录音驱动 | `board/contest_board/src/sf32lb52_audio.{c,h}` |
| 录音封装（上层直接用这个） | `board/contest_board/src/sf32lb52_audio_in.{c,h}` |
| bringup（注册设备、RNDIS、挂载、按键…） | `board/contest_board/src/sifli_ap.c` |
| 板级 defconfig | `board/contest_board/configs/sf32lb52_ai/defconfig` |

openvela 工作区里的 `vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/`（整目录，含 `src/`）
是**指向 `board/contest_board` 的软链接**（固件按
`CONFIG_ARCH_BOARD_CUSTOM_DIR=.../sf32lb52_devkit_lcd` 取板级源码），两边本来
就是同一份文件 —— 改仓库文件就等于改构建源，**不要为板级生成补丁、也不要 `git apply` 板级补丁**。

早先的 `patches/vendor_sifli-audio-driver.patch` 已经**删除**（它是一份严重脱节的旧驱动快照）：

- 在现在的工作区里它一个字也打不进去 —— 目标路径穿过软链接，`git apply` 直接拒绝：
  `error: affected file '.../src/sifli_ap.c' is beyond a symbolic link`；
- 在干净的上游树上它**反而**能打进去，代价是把 `sf32lb52_audio.c` 与 `sifli_ap.c`
  退回旧版本，**覆盖掉当前固件里的全部音频修复**（半双工串行化、`audio_in` 的接入、
  DMAStop 复核……），而这一轮新增的板级源文件（`sf32lb52_audio_in.c`、
  `sf32lb52_backlight.c` 等）根本不在那份补丁里。

原因与量级对比见 `patches/README.md` 的「板级改动不走补丁了」一节。

vendor 公共树（`chips/`）确实要打一条补丁 —— 它和音频无关，但不打就编不过/起不来：

```bash
cd <openvela 工作区>/vendor/sifli
git apply <本仓库>/patches/vendor_sifli-boot-fixes.patch    # 上游编译/启动修复（chips/）
```

> USB RNDIS 的 bring-up（`#ifdef CONFIG_RNDIS` 那段 `usbdev_rndis_initialize()`，MAC
> `00:e0:4c:53:42:31`）也**不在任何补丁里**，它就在 `board/contest_board/src/sifli_ap.c:694`
> （调用在 `:711`）。

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

---

## 8. 2026-09-13 修复：`close()` 曾会让整机静默卡死

### 现象

`close()` 一个 `/dev/audio/audio0` 的 fd，**整机静默卡死**：无 panic、无 backtrace、
不复位、串口探活毫无响应，只能重新烧录复位。

### 根因

`nuttx/audio/audio.c` 的 `audio_close()`（`:235` 起）在**最后一个 fd** 被关闭时：

```
nxmutex_lock(&upper->lock);                       /* :235 */
flags = spin_lock_irqsave(&upper->spinlock);      /* :241 — 本构建非 SMP，等价 up_irq_save() */
...
if (upper->head == NULL)
    lower->ops->shutdown(lower);                  /* :272 — 在「持锁 + 关中断」下调我们 */
```

也就是 `shutdown()` 是在**关中断**上下文里被调的。而我们的
`sf32lb52_audio_shutdown()` 原来又走了一遍**完整的 `hw_stop()`** —— 可
`AUDIOIOC_STOP` 时已经做过一次了。在关中断的上下文里**重复关闭已经关掉的
音频模拟通路**（`HAL_AUDCODEC_Close_Analog_DACPath()` / `..._ADCPath()` 等
HAL 调用）就卡死了。

### 修复

`board/contest_board/src/sf32lb52_audio.c`：

- `hw_stop()` **幂等**：已经停过（`!running` 且没有挂着没收尾的
  buffer/busy 标志）就直接 `return OK`。
- 新增 `sf32lb52_audio_hw_shutdown()`：**只做关中断上下文里安全的事** ——
  停 DMA、禁用 AUDPRC、复位通道状态、关功放（普通 GPIO 写）；
  **不回调上层**（`AUDIO_CALLBACK_DEQUEUE`）、**不碰模拟通路**。
- `sf32lb52_audio_shutdown()` 改调这个最小化函数；
  `sf32lb52_audio_stop()`（`AUDIOIOC_STOP`，中断是开的）保持调完整 `hw_stop()`，
  所以"STOP 唤醒阻塞 `read()`"的行为不变。

### 为什么现在才发现

只有当 fd 是**最后一个**时上层才调 `shutdown()`。上层应用（`hello_app`）
通常一直占着同一个设备，所以 `audio_test` / `hw_test audio` 的 `close()`
从来没走到过这条路。报警模块是"开一次、关一次"的用法，才把它踩出来。

### 回归验证（2026-09-13，真机，同一次烧录）

```
hw_test audio 2       -> read 返回 64000/64000, 1/1 PASS
audio_test 2000 1000  -> WRITE done: 64000/64000 / STOP done / audio_test: done
hw_test rtc 3         -> 2/2 PASS
hw_test alarm 1|2|3   -> 各 4/4 PASS（见 docs/alarm_usage.md）
```

### 排查手法（留个记录）

现象是"整机静默无输出"，**先怀疑自旋/死锁，不要先怀疑崩溃**：
本项目 `HAL_ASSERT()` 展开成 `while(1){}`（`USE_FULL_ASSERT` 是注释掉的），
而且关中断上下文里的死循环连串口都出不来。定位手段是在可疑函数里
**逐步打 `syslog(LOG_ERR, ...)`**，最后一条打印就是卡死点
（本次最后一条是 `close` 之前的 `C3`，直接指到 `close(fd)`）。

### 仍存疑（没验证过）

`HAL_AUDPRC_DMAStop()` / `__HAL_AUDPRC_DISABLE()` 在关中断上下文里是否绝对安全，
**没有证据**，只是风险低于模拟通路关闭，先保留在新函数里。
若以后又出现"close 卡死"，下一步就是把这两个也挪出 `shutdown()` 路径。

---

## 9. 封装好的录音接口 `audio_in_*`（2026-09-13）

背景：`app/hello_app/ai_audio.c` 早先的录音是假的（open/ioctl 全是注释、
`record_fd = 1` 占位），所以把录音收成一个板级模块，上层不必再碰驱动的 ioctl 细节。
**2026-09-13 起 `ai_audio.c` 已经改用下面这套接口**（第 10 节），这份文档的用法不变：

| 文件 | 内容 |
|------|------|
| `board/contest_board/src/sf32lb52_audio_in.h` | 接口声明 + 行为约定 |
| `board/contest_board/src/sf32lb52_audio_in.c` | 实现（已加入板级 `CMakeLists.txt` 的 `SRCS`） |

别的 app 要用它，需要在 `CMakeLists.txt` 里加
`INCLUDE_DIRECTORIES ${NUTTX_BOARD_ABS_DIR}/src`
（`app/hw_test` 和 `app/hello_app` 都是这个写法）。

### 9.1 接口

```c
/* 打开 + 配置 + START。
 * 成功 0；参数非法 -EINVAL；open/ioctl 失败返回负 errno；
 * 已经在录音中返回 -EBUSY（**不会**先停再开，避免打断别人正在录的会话）。 */
int audio_in_start(int sample_rate, int channels, int bits);

/* 阻塞读一段 PCM，返回实际读到的字节数；负值 = POSIX read() 失败
 *（返回 -1，原因看 errno），未 start 时直接返回 -EINVAL。
 *
 * 两种"读不到"必须分开：
 *   返回 0                    = 这一代会话结束了（被 AUDIOIOC_STOP 打断 /
 *                               设备没在跑 / 换了新会话）→ **跳出循环**；
 *   -1 且 errno = ETIMEDOUT  = 只是这一次没等到数据、会话还活着
 *                               → 跳过这一帧接着读，**不要**拆设备重录。
 * 判超时**只能看 errno**：调用前先 errno = 0，调用后立刻取快照。
 * 单次不要超过 AUDIO_IN_CHUNK_BYTES（1 秒 = 32000 字节）。 */
ssize_t audio_in_read(FAR void *buf, size_t len);

/* STOP + close；没在录音时是安全的空操作，返回 OK。
 * 可以从别的任务调它，唤醒正阻塞在 audio_in_read() 里的任务。 */
int audio_in_stop(void);

/* 只清本层状态 + close 自己的 fd，**绝不发设备级 AUDIOIOC_STOP**。
 * 用在"这次会话不是被本层停的"那类收尾路径上（read 返回 0/EOF，说明
 * 设备已被别人停掉或抢走）—— 这时再发一次 STOP 会把刚接管设备的那个
 * 会话一起打死。自己主动停设备仍然用 audio_in_stop()。 */
int audio_in_abandon(void);
```

参数约束（不做隐式纠正，非法直接 `-EINVAL`）：

| 参数 | 支持的值 | 说明 |
|------|---------|------|
| `sample_rate` | 8000 / 16000 / 44100 / 48000 | 驱动 codec 时钟表里的档位；本板用 **16000** |
| `channels` | 1 | 只支持单声道 |
| `bits` | 16 | s16le |

行为约定（选的是"哪一种"都写在这里）：

- **重复调用 `audio_in_start()` 返回 `-EBUSY`**（不是"先停再开"）。
  要重录就先 `audio_in_stop()`。
- 内部用**一把 `nxmutex`** 保护 fd 与状态；但 `audio_in_read()` 做阻塞读时
  **不持锁**，只短暂取锁拿一个 fd 快照。否则"一个任务在读、另一个任务调
  `audio_in_stop()` 救场"会直接死锁。
- `audio_in_stop()` 只做 `ioctl(AUDIOIOC_STOP)` + `close(fd)`，
  **不做**任何在关中断上下文里危险的事。注意：如果本模块是设备上**唯一**的
  使用者，`close()` 会走到驱动的 `shutdown()` 路径 —— 那条路径曾经让整机
  静默卡死，现已修成最小化版本（第 8 节）。
  另外它内部会取锁，**不要在中断上下文里调用**。
- 未 `start` 就 `read` 返回 `-EINVAL`；`stop` 在未 `start` 时是空操作。

### 9.2 完整示例：录 3 秒存文件

16k / 16bit / 单声道，1 秒 = 32000 字节，3 秒 = 96000 字节。
分 3 次各读 1 秒，边读边写，内存里只留 1 秒的缓冲：

```c
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include "sf32lb52_audio_in.h"

#define REC_SECONDS   3
#define SAMPLE_RATE   16000
#define CHUNK_BYTES   (SAMPLE_RATE * 2)      /* 1 秒 = 32000 字节 */

int record_3s_to_file(const char *path)
{
  int16_t *chunk;                            /* 1 秒缓冲：malloc，别放栈上 */
  int fd;
  int sec;
  int ret;

  ret = audio_in_start(SAMPLE_RATE, 1, 16);
  if (ret < 0)
    {
      printf("audio_in_start failed: %d\n", ret);
      return ret;
    }

  chunk = malloc(CHUNK_BYTES);
  fd    = (chunk != NULL) ? open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
  if (chunk == NULL || fd < 0)
    {
      printf("malloc/open %s failed: %d\n", path, errno);
      free(chunk);
      audio_in_stop();
      return -1;
    }

  for (sec = 0; sec < REC_SECONDS; )
    {
      ssize_t n;

      errno = 0;                             /* 判超时只看 errno，先清 0 */
      n = audio_in_read(chunk, CHUNK_BYTES);

      if (n == 0)                            /* 0 = 会话结束（被 STOP 打断）→ 退出 */
        {
          printf("read chunk %d: EOF（会话已结束）\n", sec);
          break;
        }

      if (n < 0)
        {
          if (errno == ETIMEDOUT)            /* 只是这一帧没数据：跳过、不占这一秒 */
            {
              continue;
            }

          printf("read chunk %d failed: %d\n", sec, errno);
          break;
        }

      if (write(fd, chunk, (size_t)n) != n)
        {
          printf("write chunk %d failed: %d\n", sec, errno);
          break;
        }

      printf("chunk %d: %zd bytes\n", sec, n);
      sec++;                                 /* 只有真读到一秒才推进 */
    }

  ret = (sec == REC_SECONDS) ? 0 : -1;

  close(fd);
  audio_in_stop();
  free(chunk);
  return ret;
}
```

要点：

- **缓冲不要放栈上**：默认任务栈常常只有几 KB，32 KB 一块会爆栈。
  上面的例子用 `malloc`；块更小（比如 100ms = 3200 字节）也可以，
  只是 `read` 次数更多。
- 写文件就是标准 POSIX `open`/`write`，要挑一个**可写目录**
  （比如 `/data` 或 `/mnt`，取决于板子挂载了什么）。
- 不管成功还是失败都要收尾：`close(fd)` + `audio_in_stop()`（若这次循环是因为
  **read 返回 0、设备被别人停掉/抢走**而退出的，收尾该用 `audio_in_abandon()`，
  见 9.1 节的入口分工），否则设备一直占着，`close()` 的 shutdown 路径也不会被触发。
- 要"边说边听"的请**顺序做**（先录完 → `audio_in_stop()` → 再播），
  `AUDIOIOC_STOP` 会把播放和录音两条通路一起关掉（第 3.1 节末）。

---

## 10. 应用层（`ai_audio.c`）现在接上了（2026-09-13）

`app/hello_app/ai_audio.c` 从"空壳"改成了真实现：

| 做的事 | 怎么做的 |
|--------|----------|
| 录音 | 调板级封装 `audio_in_start()` / `audio_in_read()` / `audio_in_stop()`（第 3.1 / 9 节），设备路径原来写的 `/dev/sound/pcmC0D0c` 本板不存在，已删 |
| 播放 | 播放线程里真的 `open(/dev/audio/audio0, O_WRONLY)` → `CONFIGURE(AUDIO_TYPE_OUTPUT)` → `CONFIGURE(AUDIO_FU_VOLUME)` → `START` → 按 100 ms 分块 `write()` → `STOP` → `close`（以前只有一个 `usleep`） |
| 停止录音 | **先** `audio_in_stop()`（它会唤醒阻塞在 `audio_in_read()` 里的线程）**再** `pthread_join`。顺序反了会自锁死 |
| 音量 | 走 `AUDIOIOC_CONFIGURE + AUDIO_TYPE_FEATURE + AUDIO_FU_VOLUME`（0~100 → 0~1000）。注意 NuttX 上层**没有**实现 `AUDIOIOC_SETVOLUME`，别用它 |
| 缓冲 | TTS 缓冲 256 KB、播放缓冲 8 秒，都是**堆分配**（静态放 BSS 会把内核 SRAM 顶到约 96%） |

上层入口没变：`audio_record_start/stop()`、`audio_play_start/stop()`、`audio_set_volume()`、
`audio_play_file()`（这条从 `-ENOSYS` 实现成了"读裸 PCM 播放"）。

### 怎么验（按层级，由下到上）

```text
hw_test audio 2                     # 板级录音封装：应打印 read 64000/64000、peak/avg、PASS
audio_test 1000 1000                # 板级播放通路：应打印 WRITE 32000/32000 并听到 1kHz 音
hw_test tts "你好，今天天气不错"      # 上层 TTS 后端（MiMo）+ 播放，喇叭出声
hw_test asr /etc/assets/test_16k.wav # 上层 ASR 后端（MiMo），打印识别文字
```

界面上则是「菜单 → 语音聊天」：弹窗打开即录音、显示计时，点「提交」走
ASR → 大模型 → TTS → 播放（实现见 `app/robot_ui/main.c` 的语音聊天那一节）。

⚠️ 麦克风是**半双工独占**的：录音和播放不能并行，两个 app 也不能同时占着
`/dev/audio/audio0`（所以 `hello_app` 的开机自启被关掉了，界面所在的 `robot_ui` 当唯一入口）。

---

## 11. 【历史记录】录音长跑后 RX 永久断流（2026-09-15 定案，2026-09-16 已消除）

> ⚠️ **本节是 2026-09-15 当时的现场记录，不是当前实现。** 当时驱动里是"每帧
> arm/abort + 100ms 分片等待 + 三级恢复"，本节描述的仪表字段与判据都长在那套
> 代码上；那套代码已在 2026-09-16 整个删除，问题随之消除（真机连续录音 180 秒
> 无异常）。读今天的日志请用驱动自己的失败日志字段，别照本节那张表对号入座。
>
> 本节是**上板实测**的定案（冻结现场原始日志 + 分诊仪表），不是推测。
> 当时的结论把范围收窄到一个具体动作（每帧的 DMA 通道 abort/re-arm），
> 现场碰到它的唯一恢复手段是真断电。

### 11.1 现象

录音会话正常跑一段时间（M1 之前几秒~几十秒就中；M1 之后**拉长了 13~39 倍，但仍然会中**）
之后，RX 通路**永久停住**：

- `read()` 开始**每次固定等满 5 秒**才返回，`errno = ETIMEDOUT(110)`；
  串口/驱动日志里的原文就是 `ret=-110`，而且**之后每一次读都这样，不会自己好**；
- 驱动的两个心跳计数 `irq` / `half` **冻死**（不再增长）；
- 驱动内建的自愈（`HAL_AUDPRC` **软复位** + 重建会话）**无效** ——
  现场出现"复位风暴"（第 7 次自愈、距上次成功仅 7670ms），只压症状、压不住根因；
- **唯一有效的恢复手段是真断电重新上电**。进了这个状态之后，
  起新会话 / open / close / STOP 全都没用。

**附带效应（查网络问题时极容易被带偏）**：断流之后板子的 USB/RNDIS 会一起废掉 ——
Windows 侧网卡显示「**200Mbps 已连接**」，链路看着是好的，但**一个包都不通**；
板子侧发起连接打印 `connect: Error 101`。这是"链路在、流量死"的典型形态：
**先看串口有没有 `ret=-110` 在刷，再去怀疑 ICS/DNS/MQTT**
（网络侧速查表也补了这一条，见 `docs/network_api_usage.md` 第 9 节）。

### 11.2 判读指纹（现场日志原文 + 逐字段读法）

实际是一条 `syslog`，这里按字段折行显示：

```
AUDIO: read 等 DMA 失败（ret=-110 err_te=0） irq=330 half=354 not_armed=0 dma_err=0
       irq_delta=0 half_delta=0 CNDTR=160 CCR=0x2aaf ISR=0x0
       aprc.State[RX]=0x8 hdma[RX].State=0x2 running=1
```

| 字段 | 值 | 读法 |
|------|----|------|
| `CNDTR` | **160**（满值） | 一帧 = 320 样本 = 640 字节 = **160 个 32bit 字**。160 表示这个 arm 周期**一个字节都没搬** ⇒ **DMA 的请求线从头到尾没被拉高**，坏在数据源头（AUDPRC 的 RX/ADC 一侧），和 DMA 通道本身无关 |
| `irq_delta` | **0** | 这 5 秒里完成中断一次都没来（`irq=330` 是开机以来的累计值，看增量） |
| `half_delta` | **0** | 半满中断也没来 ⇒ **排除**"数据其实在流、只是完成通知丢了" |
| `hdma[RX].State` | `0x2` = **BUSY** | DMA 通道一直武装着，没被人 abort 掉 |
| `not_armed` | **0** | 这一帧是**真的武装成功**了（不是"没起来"那条提前返回 0 的路） |
| `err_te` / `dma_err` | **0** | **排除 DMA 侧传输错误（TE）** |
| `CFG` bit7 | **1** | `CFG.ADC_PATH_EN` 是开的 —— M1 开关确实生效（见 11.4） |
| `aprc.State[RX]` | `0x8` | `HAL_AUDPRC_STATE_BUSY_RX`，AUDPRC 自己也认为"还在收" |
| `CCR` | `0x2aaf` | 停机这一刻通道还武装着：EN / TCIE / HTIE / TEIE / CIRC 都在，没人动过 |
| `ISR` | `0x0` | 该通道连一条 TC/HT/TE 标志都没挂 ⇒ 中断**压根没产生**，不是"产生了没人处理" |

一句话：**DMA 侧一切正常（武装着、无错误、状态机说在跑），但这 5 秒里
AUDPRC 一个数据字都没往 DMA 送、连中断都没冒出来** ⇒ 停的不是 DMA，
是 **AUDPRC 的 RX/ADC 侧不再发 DMA 请求**。

驱动还会另打一条形状判词（只在状态变化时打，不是周期日志）：
`AUDIO: RX 数据停了：连续 N 次 arm 一个字节都没搬到… → 形状(a|b|c)`。

### 11.3 已经排除的（都做过实验）

| 假设 | 证据 | 结论 |
|------|------|------|
| 内存不够（分配失败） | `free`：总 8.65 MB、只用了 938 KB | **排除** |
| DMA 传输错误把通道打坏 | `err_te=0` / `dma_err=0`；TE 的错误回调与自愈路径都正常 | **排除** |
| 会话边界 / 软复位 / 换会话 / 播放切向 | 冻结现场是**形状(a)**：`d_gen=0`、`d_sreset=0`、`d_hwstop=0`、`d_play=0`（四个计数全 0）⇒ 冻在**会话中间自己身上**，中间没有任何 stop、复位、换会话、起播放 | 这三种形状**排除** |
| 每帧的 `ADCPATH` 闸门（每 20ms 把 ADC 通路关一次再开） | 去掉它（M1）之后，冻结出现前的时长**提升 13~39 倍**，但**没有消除** | 是**加速器**，不是唯一根因 |

### 11.4 当时的结论与方向

- **剩下的唯一嫌疑：每帧的 DMA 通道 abort / re-arm。**
  驱动当时每收一帧（20ms）做一次 `HAL_DMA_Abort()` + 通道 `FreeChannel()` +
  NVIC 关/开，下一帧再重新武装；厂商参考驱动**整场会话都不做这件事**
  （只在会话开始时武装、结束才收），我们的频率高了几个数量级。
  → **这一条就是后来改掉的根因**：现在 read 每次一次性武装到调用者 buffer、
  收尾只 abort 通道，通道的拆装只在会话边界发生。
- **当时用的"每帧别碰 ADCPATH"开关（M1）**：它去掉了每帧的 `ADCPATH` 关/开，
  明显拉长了冻结前的存活时间，但没有根治。这个开关在 2026-09-16 清理时删掉，
  对应行为**固化进唯一那条实现**（`sf32lb52_audio_rx_dma_stop_frame()` 只做
  `HAL_DMA_Abort`）。
- **当时指的下一步：整场只武装一次 DMA。已经落地**（方式见上面那条）。
  而"为什么必须这么改、厂商到底怎么做、我们还错在哪"定案在 **第 12 节**
  （厂商参考实现对照）—— 再碰录音驱动之前先读它。
- 当时失败日志里的读数（`arm_sess`/`arm_boot`/`irq`/`half`/`irq_delta`/
  `half_delta`/`CNDTR`/`CCR`/`ISR`/`CFG`/两个 `State`）：**今天只剩其中一部分**
  （新日志字段见驱动 read 的失败分支），读法见 11.2 的表 —— 那张表是**当时**的口径。

### 11.5 最小复现

两种都能触发，任选：

1. **长录音**：让一个录音会话**连续跑超过 5 分钟**，期间一直循环
   `audio_in_read()`（界面停在「语音聊天」不动，或敲
   `hw_test audio 3600` 让它一口气录一小时），
   盯串口有没有刷 `read 等 DMA 失败（ret=-110 …）`。
2. **反复换会话**：反复"起录音 → 停止（或被播放切走）→ 再起"几十上百次
   —— 命中的是"会话边界 + 多帧 abort"那个形态，通常更快。

复现后按 11.2 的表确认是不是同一枚指纹。**恢复必须真断电**，
别把时间耗在软复位或反复重开会话上。

### 11.6 现场操作建议

- 上台前**别让录音长时间空跑**（一直开着的会话最容易中招）；
- 一旦看到 `ret=-110` 连续刷屏 + 网卡"已连接但一个包都不通"：**直接断电重上电**；
- 因为恢复只有断电能做，**上电后先确认"录音正常 + 网络通"再开始演示**。

> 上面三条是**当时**的现场建议。这套故障已经在 2026-09-16 的实现里消除
> （真机连续录音 180 秒零异常、20/20 轮起停无 EBUSY）；留着是为了下次碰到
> 类似形态时知道**当时**是怎么判、怎么救的。


---

## 12. 厂商参考实现对照：流级启停 ≠ 每帧取数（2026-09-15）

> 这一节回答第 11 节那个"AUDPRC 不再拉 DMA 请求线"的坑。
> **我们当初是没先读厂商实现就把驱动写出来的** —— 把厂商的**流级启停**
> （`HAL_AUDPRC_DMAStop()` + `Receive_DMA()`）当成了**每帧取数**的 API 用，
> 每 20 ms 把音频通路拆装一次，跑一阵之后外设不再拉 DMA 请求线，
> 软复位救不回、只有断电能恢复。
> 结论不重复第 11 节的现场指纹，只写"厂商怎么用、我们错在哪、怎么对齐"。

### 12.1 参考实现在哪（同一颗芯片，三条独立来源）

| 参考 | 路径 | 它回答什么 |
|------|------|-----------|
| 厂商 rt-thread 音频驱动 | `/home/youdian/SiFli-SDK/SiFli-SDK/rtos/rtthread/bsp/sifli/drivers/drv_audprc.c` | AUDPRC 数据通路**整场会话怎么维护**（本板 `sf32lb52_audio.c` 的直接对照物） |
| 厂商 HAL | `/home/youdian/SiFli-SDK/SiFli-SDK/drivers/hal/bf0_hal_audprc.c` | 每个 HAL API 到底动了哪几个寄存器 |
| 同芯片产品（小智） | `/home/youdian/xiaozhi/xiaozhi-sf32/app/src/xiaozhi_audio.c` | 上层真实产品怎么用：整个录音/放音交给 audio server（`audio_server.h` / `audio_write()`），**一次都没直接碰 AUDPRC 的 DMA API** |

**行号偏移警告**：同一份 HAL 在 openvela 工作区里是
`vendor/sifli/chips/drivers/hal/bf0_hal_audprc.c`，正文与上表那份**逐字节相同**，
只是少了许可证头，**行号整体 −40**（`sf32lb52_audio.c` 注释里引的 `:774` /
`:846` / `:850` 就是 openvela 这一份的号；第 12 节一律用上表 SiFli-SDK 那份的号）。
对照时先认清手上是哪一份。

### 12.2 厂商的正确用法（每条都有行号）

**① 整场会话只武装一次循环 DMA。**
录音的武装只在 `bf0_audio_start()` 里发生一次：`HAL_AUDPRC_Config_RChanel()`
（`drv_audprc.c:1395`）+ `HAL_AUDPRC_Receive_DMA()`（`:1396`，RX1 在 `:1411`）。
之后**整场会话再没有任何停/起动作** —— 数据靠 HT/TC 回调往上交，直到 stream
stop 才 `HAL_AUDPRC_DMAStop()`（RX0 = `:1564`；8 条通道合计在 `:1564`–`:1654`，
整段是 `bf0_audio_stop()` 的收尾，且开头 `:1542` 对"没开着的会话"直接早退）。

**② 每个通知交半块。**
TC 回调 `HAL_AUDPRC_RxCpltCallback()`（`drv_audprc.c:1941`）与 HT 回调
`HAL_AUDPRC_RxHalfCpltCallback()`（`:1980`）各交 **`bufRxSize / 2`**
（`:1959` / `:1962` / `:1967`、`:1994` / `:1999`）。回调里**只往上交数据**，
不停任何东西。

**③ 环路参数：640 字节的环 = 两个 320 字节半块 = 每 10 ms 一次通知。**
`CFG_AUDIO_RECORD_PIPE_SIZE = 320`（`rtos/rtthread/components/drivers/include/drivers/audio.h:118`），
`bufRxSize = CFG_AUDIO_RECORD_PIPE_SIZE * 2 = 640` 字节（`drv_audprc.c:1229`）。
16k 单声道 16bit 下每个半块 = 320 字节 = 160 样本 = **10 ms**（整环 20 ms）——
即"一次武装的循环 DMA，每半圈发一次通知"。

**④ `ADCPATH` 的通、断各只有一处，都落在会话边界上。**
整个 HAL 里 `ADCPATH` 的宏调用只有两处：`Receive_DMA()` 里使能
（`bf0_hal_audprc.c:830`，仅 RX_CH0/RX_CH1）、`DMAStop()` 里关闭（`:886`）。
按厂商的模型（①）读，就是**一个会话里"通一次、断一次"**。

**⑤ 软复位（SRESET）不在会话中途发生。**
只在会话收尾：`bf0_audio_stop()` 里先 `__HAL_AUDPRC_DISABLE()` + 清通道，
**判到 `channel_ref == 0`**（`drv_audprc.c:1670`）才脉冲 SRESET（`:1676`–`:1677`）——
即"整块 AUDPRC 上一条通道都不剩了"才复位；另一处是整机级的
`bf0_audprc_stop()`（`:519`–`:520`）。两者都不是"跑着跑着复位一下试试"。

### 12.3 我们的错误用法

驱动原来每收一帧（20 ms）做一次：

```
HAL_AUDPRC_DMAStop(RX)   → HAL_DMA_Abort + 清该通道全部标志 + DMA_FreeChannel
                           （含 HAL_NVIC_DisableIRQ + 通道池回收）
                           + __HAL_AUDPRC_ADCPATH_DISABLE()      bf0_hal_audprc.c:832-889
下一帧 HAL_AUDPRC_Receive_DMA(RX) → DMA_AllocChannel → Start_IT
                           → 重写 CNDTR/CPAR/CM0AR/CCR → ADCPATH_ENABLE
```

**50 次/秒**地穿过"刚拆掉、刚装上"的窗口。

**这在厂商 SDK 里一处都没有。** 把 SiFli-SDK（含 openvela 的 vendor 树、小智的
SDK 树）整体 grep 一遍的实测结果：

- `HAL_AUDPRC_DMAStop()` 的调用者**只有** `drv_audprc.c` 的 stream-stop 路径
  （12.2 ① 那 8 行）；
- `HAL_AUDPRC_Receive_DMA()` 的调用者**只有** `bf0_audio_start()`（`:1396` /
  `:1411` / `:1423` / `:1435`），外加 `drv_audprc.c:1840` 一行被注释掉的；
- 小智的产品代码连这两个 API 都没出现（它走上层 audio server）。
- 同一份代码里 `drv_audprc.c` 并不孤单：同一个 `drivers/` 目录下还有
  `drv_audcodec.c` / `drv_audcodec_m.c` / `drv_i2s_audio.c` / `drv_i2s_mic.c` /
  `drv_pdm_audio.c` 五份并列的音频驱动，它们**一次都没碰 AUDPRC 的 DMA API**
  —— 各自走 codec / I2S / PDM 自己的 DMA。所以"整场只武装一次"这条结论的
  对照面不止一份驱动，是这一整层驱动的共同做法。

所以"每帧停/起"是本驱动的发明，没有任何厂商先例可依。后果就是第 11 节那份
指纹：窗口里落下的完成通知要么被丢、要么被提前服务；跑到某一刻，AUDPRC 的 RX
侧干脆不再拉 DMA 请求线（CNDTR 满值、HT/TC 一次不来、无 TE）—— 坏的不是 DMA
通道，是它上游。

### 12.4 两个机制性坑（现象 + 判据）

**坑 1：`HAL_AUDPRC_DMAStop()` 之后必须把 `aprc.State[RX]` 掰回 READY，
否则下一次 `Receive_DMA()` 会静默地什么都不做。**

- 闸门在 `bf0_hal_audprc.c:814`：`HAL_AUDPRC_Receive_DMA()` 一进门就查
  `haprc->State[did] & HAL_AUDPRC_STATE_BUSY_RX`，命中直接 `return HAL_BUSY`
  （`:816`）。
- 而 `HAL_AUDPRC_DMAStop()` 里那行复位 **被注释掉了** ——
  `bf0_hal_audprc.c:890` 原文就是 `//haprc->State = HAL_AUDPRC_STATE_READY;`：
  abort 只清 DMA 句柄，`aprc.State` 会留在 BUSY。
- 更阴的是**调用侧把返回值丢了**：`Receive_DMA()` 内部调 `HAL_DMA_Start_IT()` 时
  没接返回值（`:818`），末尾无条件 `return HAL_OK`（`:828`）。所以"被 BUSY 挡下
  这一帧"对外**和成功一模一样** —— 上层只会在 5 秒后看到 `-110`，看不到任何错误。
- 厂商怎么绕过的：它的 stream stop 在收尾时**统一把 8 条通道的 State 刷成
  READY**（`drv_audprc.c:1697`，`bf0_audio_stop()` 末尾那个 `for` 循环）。
  本驱动对应的三处是 `hw_stop()` 的显式赋值、`hw_shutdown()` 的显式赋值，以及
  read 入口那道兜底（`sf32lb52_audio.c:1760` / `:1961` / `:3567`）。

**坑 2："复活一条卡死的 DMA"和"软复位整个模块"是两件事，别拿错。**

- 厂商专门留了一个**窄口径**的复活原语：
  `void bf0_audprc_dma_restart(uint16_t chann_used)`（`drv_audprc.c:2076`）。
  它只做三件事：把 DMA 句柄 `State` 标回 `HAL_DMA_STATE_BUSY`（`:2081`）、
  重开 TC/TE 中断（`:2082`）、**重设 CNDTR**（`:2089`，RX 用 `bufRxSize >> 2`）。
  **它不 abort、不关通道、不关 ADC 通路，也不经过 `Receive_DMA()`** ——
  所以坑 1 那道 `aprc.State` 闸门根本不参与。它假定通道本来就武装着，
  只把"搬运计数 + 中断"重新摆正。（本树里没有调用者，注释写着
  `//tc_drv_audprc.c used`；但它是官方给这条路径留的唯一直通手段。）
- 我们的自愈走的是另一条：**整模块软复位**（`__HAL_AUDPRC_SRESET_START/STOP`，
  `sf32lb52_audio.c:1158` 的 `sf32lb52_audio_aprc_soft_reset()`）。它复位的是
  **整块 AUDPRC 数字模块**，包括 ADC 通路配置（`CFG.ADC_PATH_EN`、
  `ADC_PATH_CFG0` 的录音数字增益、`RX_CH0_CFG` 的通道使能/格式、时钟分频）——
  这些寄存器是 `hw_configure()` 那次写的，复位即丢。它**也不会**顺手把
  `aprc.State[]`（软件数组）刷回 READY。
- 于是铁律：**SRESET 之后必须按
  `Config_ADCPath → Config_RChanel → Receive_DMA → __HAL_AUDPRC_ENABLE`
  重建**，少一步就是"复位完更死"（DMA 起得来、数据通路是空的）。
  本驱动已经把这条重建序列单独拎成
  `sf32lb52_audio_aprc_restore_adc()`（`sf32lb52_audio.c:1194`），
  并在 `hw_start()`（`:1486`）与自愈路径（`:1412`）里都调它，
  而且与软复位**共用同一个"有没有配置可恢复"的判据**。

### 12.5 给后来人的铁律

**写驱动之前，先把厂商参考实现读一遍**（12.1 那三份，至少第 1、2 份）。
具体到本项目一句话：**"流级启停 API"绝不能当"每帧取数 API"用。**

判据很直白：`HAL_AUDPRC_Receive_DMA()` 的语义是"**武装一条流**"，
配套的 `HAL_AUDPRC_DMAStop()` 是"**拆掉这条流**"，
两者的正确频率是**每个会话各一次**；要"每帧取数"，正确机制是让**一次武装的
循环 DMA 自己转**，靠 HT/TC 回调各交半块（12.2 ②③）。
拿不准某个 HAL API 该多久调一次时，就去数厂商调用它的地方有几个 —— 这也是
这次把两个坑认出来的方法本身。

### 12.6 要对齐厂商，还差哪几件事

| # | 事项 | 必须/可选 | 现状 |
|---|------|-----------|------|
| 1 | 会话内不再碰 `ADCPATH`（去掉每帧关/开） | **必须** | **已做**：`sf32lb52_audio_rx_dma_stop_frame()` 只做 `HAL_DMA_Abort`（不跟 `DMAStop` 的 `ADCPATH_DISABLE`），只在"会话 / 一次读"的边界被调到 |
| 2 | 会话内不再逐帧 abort/re-arm DMA 通道 | **必须** | **已做**：`sf32lb52_audio_read()` 每次一把 `Receive_DMA` 到**调用者的 buffer**，收尾只 abort 通道；通道的拆装只发生在会话边界（`hw_start` / `hw_stop` / `hw_shutdown`）。ISR 只做"过会话闸 + 计数 + post"，**不做任何恢复** |
| 3 | 停过 DMA 之后把 `aprc.State[RX]` 掰回 READY | **必须** | **已做**：`sf32lb52_audio_rx_dma_stop_if_armed()` 一处收口（abort 只在句柄确实还武装着时做，收完摆回 READY），`hw_stop` / `hw_shutdown` 也各自显式赋值 |
| 4 | 软复位只留在会话边界，且复位后按序重建 | **必须** | **已做**：SRESET 只在 `hw_stop` / `hw_shutdown` 两处；重建由 `sf32lb52_audio_aprc_restore_adc()` 在 `hw_start` 负责 |
| 5 | 等待侧不借用内核的定时看门狗 | **必须** | **已做**：write 那条等待由自己的看门狗（`wr_wait_wdog`，见驱动文件顶部那段说明）产生"到点了"；录音 read 那条等待一次等满、由内核定时等待兜（暴露面 = 每次 read 一次武装 + 一次 ISR post） |
| 6 | 把每帧 abort/re-arm 的老路径整段删掉 | **已完成** | **已做（2026-09-16 清理）**：常驻接收环与"每帧 arm/abort + 100ms 分片 + 三级恢复"两条旧路径连同它们的开关**整段删除**，旧实现交给 git 历史 |
| 7 | 环路参数逐字对齐厂商（640 B 环） | —— | **作废**：本驱动没有常驻环（read 直接把数据收进调用者 buffer），这一条不再适用 |
| 8 | 用 `bf0_audprc_dma_restart()` 做窄口径复活，替代整模块 SRESET 自愈 | 可选 | 未做：这是"如果第 11 节的冻结再次出现"时的下一步备选 —— 它不丢 ADC 通路配置、比 SRESET 窄得多（12.4 坑 2），但要真机验证，**没验证之前不要动自愈路径** |

> 表里 1–5 都是**当前实现**（代码里就是这一条，没有"改回 0"的开关了 —— 老的开关
> 连同它们编掉的旧分支已在 2026-09-16 清理时删除，旧行为正是第 11 节那个只有断电
> 能恢复的故障，只能从 git 历史里找回来）。
>
> 本表与 12.4 里 `sf32lb52_audio.c` 的行号按 **HEAD `0e4f5e6`** 标注（和第 11 节
> 同一口径），**那是 2026-09-16 重写之前的行号，现在对不上** —— 以函数名/注释为准。
