# tools/audio_event/record_guide.py —— 板端自录数据向导

公开数据集覆盖不到**我们自己的麦克风域**（这颗麦、这个壳、这间屋子），也覆盖不到
"老人喊救命""敲我们这张桌子"这种具体情形，所以必须**用板子自录一批**。
这条脚本就是干这个的：提示你出声 → 板子录 N 秒 → 自动把 PCM 从串口取回 PC →
转成 16 kHz 单声道 wav → 显示每类进度。

向导模式全程只需要你**按回车**和**做动作**。

---

## 1. 一分钟上手

在仓库根目录（Windows 侧，能看到 `tools/` 的那个目录）：

```bash
py -3.10 tools/audio_event/record_guide.py                    # 四类按推荐顺序录（默认每类 20 条、每条 2 秒）
py -3.10 tools/audio_event/record_guide.py --class knock --count 25
py -3.10 tools/audio_event/record_guide.py --list             # 只看 PC 上已录了多少，不碰串口
py -3.10 tools/audio_event/record_guide.py --dry-run --auto --count 2   # 不连板子，自测全流程
```

前置条件：

1. **COM4 空着** —— 先把 `serial_term.py` / `lcd_mirror.py --serial COM4` / `cmd_cap.py` 关掉，
   串口被占用时脚本会直接报错退出（不会去抢）。
2. 板子跑着固件、能进 nsh（脚本启动时会发一次 `help` 探活，顺便看 `hexdump` 在不在）。
3. 第一次真跑，建议先来两条小的：
   `py -3.10 tools/audio_event/record_guide.py --class knock --count 2 --seconds 1`

每一步都会打印：串口探测结果 → 板上目录准备 → 每条录音的**板端 peak / PC 端 peak / RMS / 文件路径**。
峰值太低（默认 < 150）会被判成静音**自动丢弃并让你重来**，不会把垃圾样本混进数据集。

---

## 2. 录音清单（照这个录，20~30 分钟收工）

| 类别 | 条数 | 每条 | 怎么制造声音 | 注意 |
|---|---|---|---|---|
| `fall` 跌倒撞击声 | 20 | 2 s | 把**一本厚书或靠垫**从桌面高度摔到地上，一条里摔 1~2 次 | 别摔板子/手机/玻璃杯；换个位置摔（桌上、桌边、1 m 外） |
| `knock` 敲击声 | 20 | 2 s | **指节敲桌面三下**（咚、咚、咚，间隔约 0.3 s） | 板子放桌上或桌边；变换敲击点（正面/侧面/隔一点距离） |
| `scream` 尖叫/呼救 | 15 | 2 s | 自己喊一声"救命"或"啊——"，离板子 1~3 米 | 让家人/同事也帮忙喊，男女都要有 |
| `other` 负样本 | 20 | 2 s | 正常说话、放 TTS 播报、关门、走路、搬东西、翻书、放杯子 | 只要"不该报警"的声音都算；这条决定了误报率 |

合计 75 条 ≈ 20~30 分钟（每条：2 s 录音 + 3~8 s 取回 + 几秒摆姿势）。

几条经验：

- **不要录得太整齐**：距离、力度、朝向、房间里的底噪都要有变化，模型才学得泛化。
- **别开静音空调/关窗**去录 —— 训练和线上要在同一个噪声条件下。
- 一条没录好（太静/削顶/只拼回一半）直接重来，脚本会提示原因。
- 中途想歇就 `Ctrl-C`，已经落盘的 wav 都在（松手前它会收尾关串口）；回来接着跑会自动接着编号。
- 录完用 `--list` 看一遍数量。

---

## 3. 数据是怎么从板子回到 PC 的（以及为什么不能 `cat`）

- 板端**没有** base64 / xxd / hexdump 之类的命令行给外部用（`mbedtls` 的 base64 只在
  `mimo_voice.c` 内部用，没开放成 NSH 命令），`cat` 裸 PCM 会被控制台日志吃掉字节、还无法定位丢在哪。
- 板上真正可用的是 **NSH 自带的 `hexdump`**（`CONFIG_NSH_CMDOPT_HEXDUMP=y`，
  实现在 `apps/nshlib/nsh_dbgcmds.c: cmd_hexdump`）：

  ```
  /data/rec/knock_01.pcm at 00000100:      <- 行头：这个 08x 是文件**绝对偏移**
  0000: 41 42 43 ... 44 45  ABCD...        <- 每行 16 字节，前面的 4 位是**块内偏移**
  ```

  两者相加就能定位每一行，所以**日志插进来只会丢那一行，不会让后面全部错位**。
- 取回策略：整文件 `hexdump` **逐轮抓** → 按偏移合并 → 直到 `[0, N)` 连续无洞为止
  （默认最多 3 轮，丢的行靠下一轮补）。行首被日志糊住的行，靠"偏移刚好接得上"救回来。
- 故意**不用** nsh 的 `skip=`/`count=`：`cmd_hexdump` 在 skip 跨块时 `position` 已经加过了，
  行头偏移会指错位置（源码里的 skip 分支），所以只做整文件抓取。
- 文件长度以板子自己打印的 `SAVED <n> bytes -> <file>` 为准（`audio_test record` 的
  `do_record()`），取不到再退到 `ls -l`，都没有才用"连续前缀"并告警。
- 完整性：同一个字节只认一个来源；两轮取值不一致会打 `[!]`（那一次传输有问题，建议重录）。
- 串口读循环照抄 `tools/pc_env/cmd_cap.py` 的规矩：**发命令前先读到安静、发完一直读到安静、
  绝不带着未读输出关端口**（否则板侧 nsh 的 `write()` 会阻塞，之后命令全部排在积压后面，
  现象是"命令好像没生效"）。

> 板上的 `/data` 是 **tmpfs（RAM）**（`board/contest_board/src/sifli_ap.c` 里 `nx_mount("/data","tmpfs")`）：
> 重启就空，而且 2 秒录音 = 64 KB 内存。所以默认**取回成功就 `rm` 板上的 pcm**；
> 想留就加 `--no-delete`，但别一次囤几十条。

---

## 4. 参数一览

| 参数 | 默认 | 说明 |
|---|---|---|
| `--class` | 全部（按 `fall,knock,scream,other`） | 可多次给或用逗号分隔 |
| `--count N` | 20 | **每类**录几条 |
| `--seconds S` | 2 | 每条录多久（秒） |
| `--port` / `--baud` | `COM4` / `1000000` | 串口和波特率（板子就是 1 Mbaud，不是 115200） |
| `--out DIR` | `D:/apply/claw/_audio_event/selfrecord` | PC 输出根目录 |
| `--gain 0..1000` | 不动板子 | 先执行 `audio_test vol N` 设录音增益（0=-36 dB，1000=+6 dB） |
| `--passes N` | 3 | 每个文件最多抓几轮 |
| `--min-peak N` | 150 | 峰值低于此值当静音丢弃、重录 |
| `--no-delete` | 关 | 取回后不删板上的 pcm |
| `--auto` | 关 | 不等人按回车（配 `--dry-run` 用） |
| `--fail-fast` | 关 | 一条失败就停本类 |
| `--list` | — | 只统计 PC 上已录进度，不碰串口 |
| `--dry-run` | — | 不连板子，用假板子（故意造日志污染）跑通全流程 |

录音过程中可用的按键：**回车**=开始录这一条，**q**=换下一类，**l**=看进度。

---

## 5. 出错了怎么办

| 现象 | 原因 | 处理 |
|---|---|---|
| `PermissionError` / 打不开 COM4 | 串口被别的工具占着 | 关掉 `serial_term.py`、`lcd_mirror.py --serial COM4`、`cmd_cap.py` 等 |
| 串口开了但 nsh 没回话 | 波特率不对，或板子没在跑 | 波特率固定 1000000；看板子是不是还在启动/重启中 |
| `help` 里没看到 `hexdump` | 固件没开 `CONFIG_NSH_CMDOPT_HEXDUMP` | 换固件；或先只当"录音+提示"用（数据取不回来） |
| 一直没等到 `SAVED` | 麦克风被应用占着（唤醒/播放/TTS 中），或 `/data` 没挂上 | 等应用空闲（别在放音乐时录）；`--seconds 1` 试一条排查 |
| 峰值只有几十 | 离麦太远、增益小 | `--gain 700`（或直接 `audio_test vol 700`）后重录 |
| 峰值 32767 削顶 | 增益太大 | `--gain 400` 或站远一点 |
| 只拼回 60%~80% | 那一刻串口日志太吵 | 加大 `--passes`；让应用安静下来再录 |
| 只拼回 0 字节 | `hexdump` 不可用 / 文件不存在 / 串口被占 | 先跑 `--list` 和 `help` 看 `hexdump` 在不在 |

---

## 6. 产出物长什么样

```
D:/apply/claw/_audio_event/selfrecord/
├── fall/    fall_01.wav  fall_02.wav  ...
├── knock/   knock_01.wav ...
├── scream/  scream_01.wav ...
├── other/   other_01.wav ...
└── index.csv        class,file,index,seconds,bytes,peak_pc,rms_pc,peak_board,board_path,ts
```

- wav 一律 **16 kHz 单声道 16 bit**，和 `mel.py` 那个前端契约一致（不需要重采样）。
- 目录名就是类别名；模型类序固定 **0=other 1=fall 2=knock 3=scream**（见 `dscnn.py` / `export_c.py`）。
- `index.csv` 是每次成功录音追加一行，带板端/PC 端峰值，便于事后挑异常样本。
- 编号是接着已有文件往下走的，重复跑不会覆盖老数据。

---

## 7. 验证到什么程度了（诚实边界）

**已经验过的**（都在 Windows 上用 `py -3.10` 跑过）：

- `py -3.10 -m py_compile` / AST 解析通过；`--help`、`--list`、`--dry-run` 参数解析正常。
- `--dry-run --auto` 全流程跑通（录 → 取回 → wav → index.csv → 进度显示），
  带交互（管道喂回车）也跑通。
- 解析器单独压力测试（`D:/apply/claw/_audio_event/check_hexdump_parse.py`，不进仓库）：
  5000 字节随机数据 + 整行日志污染 + 行首被日志糊住 + 行被切断 + 全 0 / 全 ff / 含 CR/LF，
  块大小 64 / 256，逐字节比对**零错位**，无污染时一轮 100% 拿全。
- 字节精确性（`check_fetch_exact.py`）：假板子文件 → fetch → wav，**逐字节相同**，
  wav 头 ch=1 / width=2 / rate=16000 正确。

**没有验过的**：**没有连真板子、没有占用 COM4 跑过一次**（板子可能正被别的任务用）。
所以下面这些是照源码/配置推断的，第一次真跑请盯一眼：

1. `hexdump` 在这块板子的固件里确实可用（依据是构建配置
   `cmake_out/contest2026_233_board_sf32lb52_ai/.config` 里 `CONFIG_NSH_CMDOPT_HEXDUMP=y`
   且 `# CONFIG_NSH_DISABLE_HEXDUMP is not set`）；
2. hexdump 行头/行内偏移的实际格式（依据 `apps/nshlib/nsh_dbgcmds.c: cmd_hexdump` 与 `nsh_dumpbuffer`）；
3. 1 Mbaud 下抓一个 2 秒文件（64 KB，hexdump 文本约 280 KB）大约几秒 —— 脚本按
   ~6 KB/s 留的超时余量，实测更快就会提前结束；
4. `/data/rec` 可写、`audio_test record` 能拿到麦克风（这条是另一个任务实测过的前提）。

第一次真跑最省事的做法就是 `--class knock --count 2 --seconds 1`，看那一行
`[取回] 第 1 轮：32000/32000 字节 (100%)` 是不是 1 轮就满 —— 若是，后面直接批量录。

---

## 8. 依赖

`pyserial`（本机 3.5）+ 标准库（`wave` / `array` / `re` / `time`）。
**不装新包、不依赖 numpy/torch/scipy/librosa。**
