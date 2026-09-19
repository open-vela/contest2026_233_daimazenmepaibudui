# 智爱守护——基于OpenVeLA的多模态非接触式AI居家看护终端

## 一、作品简介

面向独居及高龄老人，依托SF32LB52-DevKit-LCD运行OpenVeLA系统打造的智能居家安全看护终端。联动大屏和米家生态，解决老人突发意外无反馈的居家痛点。

**核心功能：**
- 多模态AI陪伴：语音交互 + 视觉识别 + 情感计算
- 非接触式健康监测：毫米波雷达跌倒检测、睡眠监测
- 智能看护：异常行为识别、紧急呼叫、用药提醒
- 米家生态联动：智能家居控制、环境监测

## 二、选题方向

**AI 硬件产品创新**

基于 openvela + ai_agent，开发「能主动、会执行」的嵌入式 AI Agent 应用。

## 三、目录结构

```
contest2026_233_daimazenmepaibudui/
├── app/                          # 应用代码目录
│   ├── hello_app/                # AI陪伴系统核心模块
│   │   ├── ai_companion_main.c   # 主程序入口
│   │   ├── ai_llm.c/h           # 大语言模型接口
│   │   ├── ai_audio.c/h         # 音频处理模块
│   │   ├── ai_care.c/h          # 智爱守护核心逻辑
│   │   ├── ai_sound_detect.c/h  # 声音检测模块
│   │   └── ai_state_machine.c/h # 状态机管理
│   ├── robot_ui/                 # 机器人界面模块
│   └── zhi_ai/                   # 智爱应用模块
├── board/                        # 板级适配代码
│   └── contest_board/            # SF32LB52-DevKit-LCD适配
├── quickapp/                     # 快应用代码
│   └── hello_quickapp/           # 快应用示例
├── logs/                         # AI Coding 日志
│   └── gaoxiaoying0207/          # 开发者日志目录
├── nuttx/                        # OpenVeLA内核（通过repo sync获取）
├── vendor/                       # 厂商适配代码（通过repo sync获取）
├── apps/                         # 系统应用（通过repo sync获取）
└── README.md                     # 本文件
```

## 四、运行方式

> 这一节是**从拿到代码到板子跑起来**的完整流程。真正跑起来是五件事：
> **给上游打补丁 → 放密钥文件 → CMake 配置编译 → sftool 烧录 → 按固定顺序上电**。
> 少任何一件，现象都不是"编译不过"，而是"起来了但没网 / 不认人话 / 黑屏"这类很难猜的问题。
>
> 下文两个占位符：**`<工作区>`** = openvela 工作区根目录（下面同时有 `nuttx/`、`apps/`、
> `vendor/`、`packages/`、`prebuilts/` 和本仓库 `contest2026_233_daimazenmepaibudui/`）；
> **`<COM口>`** = 板上 CH343 串口在 PC 上的端口号（例如 `COM4`）。
>
> 板子之外的 PC 侧配套（屏幕镜像、不需要板子的协议测试、网络/串口脚本）有单独一份说明，
> 见 **`tools/README.md`**；板级音频/显示用法与踩坑见 `docs/audio_driver_usage.md`、
> `docs/display_touch_gpio_usage.md`。本节只讲"把固件跑起来"这条主线。

### 1. 环境与获取代码

**主机**：Linux（x86_64）+ `git` / `repo` / `cmake`(≥3.16) / `ninja`。
ARM 交叉工具链、`genromfs`、CMake、Ninja 都在工作区自带的 `prebuilts/` 里，
**不需要另外 apt 装** `gcc-arm-none-eabi`。

**获取代码**：本仓库根有一份 repo manifest `contest2026_233_daimazenmepaibudui.xml`
（`<include name="openvela.xml"/>`，默认 `remote="openvela" revision="dev-ai-contest-2026"`）。
按标准 repo 流程 init + sync 之后，目录布局是：

```
<工作区>/
├── nuttx/  apps/  vendor/  packages/  prebuilts/   # openvela 主树（repo 拉的）
└── contest2026_233_daimazenmepaibudui/             # 本仓库（比赛作品代码）
```

manifest 用 `<linkfile>` 把本仓库的目录映射进 openvela 的两处位置，**不用手工拷**：

| 本仓库 | 工作区里的位置 |
|---|---|
| `app/hello_app`、`app/robot_ui`、`app/zhi_ai`、`app/hw_test`、`app/audio_test` | `packages/demos/contest2026_233_*` |
| `board/contest_board` | `vendor/openvela/boards/contest2026_233_board` |
| `board/contest_board/{CMakeLists.txt,Kconfig,configs,include,scripts,src}` | `vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/` 下的**同名软链** |

> ⚠ 最后一行是**逐项目录软链**，不是整目录软链，别自己改成
> `ln -s board/contest_board .../sf32lb52_devkit_lcd`：`nuttx/CMakeLists.txt` 要靠
> `<board>/../drivers` 去找芯片驱动，整目录软链会让 `..` 按 POSIX 规则解析到**软链目标的
> 父目录**，构建直接报 `drivers/platform/input/ft6146.c missing`。
>
> `repo sync` 之后如果 `vendor/sifli` 把原始目录签了回来（覆盖掉这些软链），重跑一次
> `bash board/contest_board/scripts/link_board_impl.sh` 即可；脚本自己会找 openvela
> 工作树，找不到就用 `OPENVELA_DIR=<工作区>` 指定。

### 2. 给上游打补丁（12 个，按 `cd` 目标分三棵树）

`patches/` 下的补丁**必须打**，否则要么编不过、要么起来就不工作。
每个补丁的 `cd` 目标与作用见下表；**表里的编号就是应用顺序**（同一个文件上有多条时不能颠倒）。

| # | 补丁 | `cd` 到 | 不修会怎样 |
|---|------|---------|-----------|
| 1 | `vendor_sifli-boot-fixes.patch` | `vendor/sifli` | 编不过 / 起不来（`sifli_uart.c`、`sifli_irq.c`、`sf32lb_flash.c` 三处） |
| 2 | `vendor_sifli-rtc-alarm-fix.patch` | `vendor/sifli` | RTC alarm 永不触发（`hw_test rtc 3` 收不到 SIGUSR1，年份读成 1926） |
| 3 | `vendor_sifli-lcd-brightness.patch` | `vendor/sifli` | 亮度只有 0/100（中间值报 `-38`）。**2026-09-16 起还带**：面板重新初始化 `hw_test lcdreinit`（黑屏救回）、12 步 `[lcdreinit]` 诊断日志、关掉"开机第三次面板复位被 SETPOWER 插队"的竞态、面板 QSPI 时钟 48MHz→24MHz |
| 4 | `vendor_sifli-usb-rndis.patch` | `vendor/sifli` | USB 枚举失败（设备管理器 Code 10），RNDIS 网卡出不来 |
| 5 | `nuttx-usbdev-rndis.patch` | `nuttx` | RNDIS RX 永久停摆（网卡 link up，但板子一个包都不回） |
| 6 | `nuttx-usbdev-rndis-nomem.patch` | `nuttx` | 内存被挤空时 `rndis.c` 断言 → **整个 USB 设备栈死掉**（**必须在 5 之后**） |
| 7 | `apps-ai-agent-velaclaw-sdk.patch` | `packages/ai_agent` | `velaclaw_client_open` 链不上，`hello_app` 编不进来 |
| 8 | `apps-ai-agent-llm-clock-fix.patch` | `packages/ai_agent` | `ask` 永远报超时（墙钟被对时跳变污染了耗时计算） |
| 9 | `apps-ai-agent-vela-tls-chunked-end.patch` | `packages/ai_agent` | chunked 响应卡满 120 秒（TTS 不出声） |
| 10 | `apps-ai-agent-vela-tls-no-block-close.patch` | `packages/ai_agent` | 语音永远停在「处理中」（**必须在 9 之后**） |
| 11 | `apps-ai-agent-vela-tls-pool-owner.patch` | `packages/ai_agent` | TLS 连接池跨 task group 误关别人的 fd（提醒播报丢音）（**必须在 9、10 之后**） |
| 12 | `vendor_sifli-uart-rx-dma-reinit.patch` | `vendor/sifli` | 音频恢复里那记"复位整块 DMAC1"会把**控制台串口的 RX DMA**一起抹掉 → 串口永久收不到命令（板子还活着、日志照打） |

```bash
# ① vendor/sifli（5 个；改的文件互不重叠，顺序只为可复现）
cd <工作区>/vendor/sifli
git apply <工作区>/contest2026_233_daimazenmepaibudui/patches/vendor_sifli-boot-fixes.patch
git apply <工作区>/contest2026_233_daimazenmepaibudui/patches/vendor_sifli-rtc-alarm-fix.patch
git apply <工作区>/contest2026_233_daimazenmepaibudui/patches/vendor_sifli-lcd-brightness.patch
git apply <工作区>/contest2026_233_daimazenmepaibudui/patches/vendor_sifli-usb-rndis.patch
git apply <工作区>/contest2026_233_daimazenmepaibudui/patches/vendor_sifli-uart-rx-dma-reinit.patch

# ② nuttx（2 个，改同一个文件，顺序不能换）
cd <工作区>/nuttx
git apply <工作区>/contest2026_233_daimazenmepaibudui/patches/nuttx-usbdev-rndis.patch
git apply <工作区>/contest2026_233_daimazenmepaibudui/patches/nuttx-usbdev-rndis-nomem.patch

# ③ packages/ai_agent（5 个，同一个文件上的多条，按 7→8→9→10→11 的顺序）
cd <工作区>/packages/ai_agent
git apply <工作区>/contest2026_233_daimazenmepaibudui/patches/apps-ai-agent-velaclaw-sdk.patch
git apply <工作区>/contest2026_233_daimazenmepaibudui/patches/apps-ai-agent-llm-clock-fix.patch
git apply <工作区>/contest2026_233_daimazenmepaibudui/patches/apps-ai-agent-vela-tls-chunked-end.patch
git apply <工作区>/contest2026_233_daimazenmepaibudui/patches/apps-ai-agent-vela-tls-no-block-close.patch
git apply <工作区>/contest2026_233_daimazenmepaibudui/patches/apps-ai-agent-vela-tls-pool-owner.patch
```

几条容易踩的：

- 打之前先 `git apply --check <补丁>`。**报 `already applied`、或者 `--check --reverse` 通过**，
  说明这套改动已经在树里了（本队的开发工作区就是这种状态：改动直接改在三棵上游树上、未提交）。
- `packages/ai_agent` 就是 `apps/packages/ai_agent`（后者是指向前者的软链），两边都能进去；
  但补丁里的路径是 `src/...`，所以必须在 `ai_agent/` 这一层 `git apply`。
- **板级（`boards/`）不走补丁**：`board/contest_board` 是本仓库里的文件，
  `vendor/sifli/boards/.../sf32lb52_devkit_lcd` 只是指向它的软链（见上一节的表）。
  板级要改就改本仓库，**不要为它生成/应用补丁**。早期那份 `vendor_sifli-audio-driver.patch`
  已删除 —— 它会把树带回一份只改到一半的旧驱动（1287 行，现在是 2484 行）。
- 每个补丁的完整背景（现象 / 根因 / 行号 / 一键回退开关 / 验证方式）见 **`patches/README.md`**。

### 3. 放密钥文件（这一步不做，大模型和语音全废）

固件**不带任何密钥**。下面两个文件是**本机生成、故意不进版本库**的
（被 `.git/info/exclude` 排除），要在**编译之前**放好：

| 文件（放在 `board/contest_board/src/etc/assets/` 下） | 干什么 | 不放的后果 |
|---|---|---|
| `agent_config.json` | ai_agent 的凭据（大模型 key、火山 ASR/TTS 凭据、可选 MQTT broker） | 大模型 / ASR / TTS **全部不可用**（`llm_host` 与 `api_key` 缺任一个就整体判不可用），界面会弹"没配密钥"的报错页 |
| `push_key.txt` | 手机推送（Bark）的 device key | 报警只上 MQTT，手机收不到推送 |

- **格式**：就是 ai_agent 的 `config.json` —— 扁平字符串键 + 值，`llm_backend_0` 的值本身是一段 JSON 字符串。
  下面**只列字段名与结构，不要填真实密钥**：

  ```jsonc
  {
    "api_key":   "<大模型 API key>",
    "llm_host":  "<大模型 API 主机名>",
    "llm_path":  "/v1/chat/completions",   // 缺省有默认值
    "llm_port":  "443",                    // 缺省有默认值
    "model":     "<对话模型名>",
    "mqtt_broker": "<host 或 host:port>",  // 配了就只连这一台、不再自动降级轮换
    "volc_appkey": "<火山 ASR/TTS appkey>",
    "volc_token":  "<火山 token>",
    "volc_asr_cluster": "<火山 ASR cluster>",
    "volc_api_key": "<火山 api key>",
    "volc_speaker": "<火山音色>"
  }
  ```

  火山这一组是**可选**的：本固件的语音链路也支持不经过火山、用同一把大模型 key
  直接打 ASR/TTS（实现见 `app/hello_app/mimo_voice.c`）。
- 这个素材**不是被原样读取的**：开机时板级函数 `sf32lb52_install_agent_config()`
  （`board/contest_board/src/sifli_ap.c`）把它拷成 `/data/ai_agent/config/config.json`
  （权限 0600，日志只报字节数、从不打印内容），之后就等价于手敲过 `set_llm` / `set_volc_*`。
  **目标已存在则不覆盖**（保留运行时改过的值）；素材不存在就安静跳过，不影响开机。
  `/data` 是 tmpfs、重启即清空，所以每次开机都从 ROMFS 补装一次。
- **改了 `src/etc/` 下的任何东西，必须先删掉 ROMFS 产物再编**，否则新内容进不了固件
  （构建系统感知不到 `PATH` 目录里文件的增删）：

  ```bash
  rm -f  <工作区>/cmake_out/contest2026_233_board_sf32lb52_ai/boards/exclude_board/src/romfs.img \
         <工作区>/cmake_out/contest2026_233_board_sf32lb52_ai/boards/exclude_board/src/romfs_etc.c
  rm -rf <工作区>/cmake_out/contest2026_233_board_sf32lb52_ai/boards/exclude_board/src/romfs_etc
  # 编完之后必须回头确认新素材真的进去了（时间戳应是刚才）：
  ls -l <工作区>/cmake_out/contest2026_233_board_sf32lb52_ai/boards/exclude_board/src/romfs_etc/assets/
  ```

- **上板确认**：串口里 `ls -l /data/ai_agent/config/` 能看到 `config.json`（看不到 = 素材没进固件）。
- 唤醒词模板（可选）走**同一个机制**：`src/etc/assets/kws/slot0.tpl`（「你好，openvela」）、
  `slot1.tpl`（「Hello，openvela」），开机自动补装到 `/data/kws/`（目标已存在不覆盖，
  目录为空就静默跳过 = 没有唤醒词）。细节见 `board/contest_board/src/etc/assets/README.txt`。

### 4. 配置与编译（CMake + Ninja）

**先设工具环境**（`genromfs` 在 `prebuilts/tools/linux/x86_64/`，
`kconfiglib` 在 `prebuilts/tools/python/dist-packages/kconfiglib`）——
**这两条不设，编译（或触发 reconfigure）一定失败**：

```bash
export PATH=<工作区>/prebuilts/tools/python/bin:<工作区>/prebuilts/tools/linux/x86_64:$PATH
export PYTHONPATH=<工作区>/prebuilts/tools/python/dist-packages/kconfiglib
```

**首次配置**（**改过 `defconfig` 之后也必须重跑这一步**）：

```bash
cmake -S <工作区>/nuttx \
      -B <工作区>/cmake_out/contest2026_233_board_sf32lb52_ai \
      -GNinja \
      -DBOARD_CONFIG=../vendor/openvela/boards/contest2026_233_board/configs/sf32lb52_ai \
      -DEXTRA_FLAGS="-Wno-cpp -Wno-deprecated-declarations"
```

**每次编译**：

```bash
cmake --build <工作区>/cmake_out/contest2026_233_board_sf32lb52_ai
```

**判据**：命令返回 `0`，并且生成
`<工作区>/cmake_out/contest2026_233_board_sf32lb52_ai/nuttx.bin`
—— 当前约 **5.9 MB**，占 16 MB flash 的约 **35%**（大头是三档中文字库）。

几个已经踩过、容易白等的点：

- `-DBOARD_CONFIG` 的**相对路径是相对 `<工作区>/nuttx` 解析的**
  （`nuttx/CMakeLists.txt` 里 `get_filename_component(... ABSOLUTE BASE_DIR ${NUTTX_DIR})`），
  所以上面那条命令在哪个目录下执行都一样。
- `-GNinja` 与官方入口一致（`<工作区>/build.sh` 的 `VELA_CMAKE_GENERATOR` 默认就是 `-GNinja`，
  现有构建目录的 `CMakeCache.txt` 里也是 `Ninja`）；`-DEXTRA_FLAGS` 同样是官方入口给这块板的默认值。
- `config.h` 是 **configure 阶段**生成的：只改 `defconfig` / `.config` 再 `cmake --build`
  **不生效**。改完先 `grep <CONFIG_XXX> <build>/include/nuttx/config.h` 确认新值真进去了再编。
- `defconfig` 里的注释**只能 ASCII，且不能出现双引号 / 括号 / 分号**：OpenVela 的
  `nuttx/cmake/nuttx_kconfig.cmake` 用**不带引号**的 `string(REGEX MATCH ...)` 解析这些行，
  注释里的 `"` 会把 CMake 的参数吃掉，报
  `string sub-command REGEX, mode MATCH needs at least 5 arguments`。
- defconfig 里的网络地址是配合 PC 侧固定网段 `192.168.137.x` 定的，**换 PC / 换网络都不用改**：
  `CONFIG_NETINIT_IPADDR=0xc0a88902`（板子 `192.168.137.2`）、
  `CONFIG_NETINIT_DRIPADDR=0xc0a88901`（网关 `192.168.137.1`）、
  DNS 写死 `CONFIG_NETDB_DNSSERVER_IPv4ADDR=0xdf050505`（= `223.5.5.5`）。

> 官方一键入口也可以（`build.sh` 自己会 `source build/envsetup.sh`，把工具链 / CMake / Ninja
> 的 PATH 设好）：`cd <工作区> && ./build.sh contest2026_233_board:sf32lb52_ai --cmake`
> （用法见 `build.sh` 的 `usage()`）。**本次没有实测过这一条**；上面那套 cmake 命令是实测跑通的。

### 5. 烧录

**工具**：`sftool` **0.2.5**（<https://github.com/OpenSiFli/sftool/releases>）。
⚠ 老版本 **0.1.16 在这块板上进不了下载模式**（一直 `Failed to connect to the chip`）。

**本板必须烧两个东西**：ROM 引导（SFBL）要读到 `0x12000000` 处的分区表（ftab）才会跳转镜像，
**只烧 `nuttx.bin` 会让板子完全静默**。

```bash
sftool -p <COM口> -c SF32LB52 -m nor --before default_reset --after soft_reset \
    write_flash "<工作区>/contest2026_233_daimazenmepaibudui/flash/ftab.bin@0x12000000" \
                "<工作区>/cmake_out/contest2026_233_board_sf32lb52_ai/nuttx.bin@0x12010000"
```

仓库里也有现成脚本（等价于上面那条）：`flash/flash_openvela.bat <nuttx.bin>`（Windows，
会提示输入端口号）、`flash/flash_openvela.sh <nuttx.bin> <串口设备>`（Linux）。

| 文件 | 烧到 | 说明 |
|---|---|---|
| `flash/ftab.bin` | `0x12000000` | 分区表，**启动必需**（SFBL 靠它找到镜像） |
| `nuttx.bin` | `0x12010000` | openvela 镜像（平面 XIP，链接地址 `0x12010000`） |

**判据**：输出里有 `flash done`、退出码 `0`。

- `Failed to connect to the chip` / `timeout while waiting for RAM command response` /
  `Failed to download stub`：SoC 错过了 RTS 复位后约 2 秒的 `ATSF32` 监听窗口。
  **按一下板子 RESET，或硬断电 10 秒再来一次**（实测这是最有效的解药，别写循环空转重试）。
  另外，**板子上电 / 复位时不要插着原生 USB**（见下一节）。

### 6. 上电顺序与首次运行现象

**顺序是铁律，反了就会得到一堆"看起来像固件坏了"的现象**：

> **① 先插 UART（CH343，`<COM口>`）→ ② 板子上电 → ③ 等固件跑起来（日志出主菜单 / `nsh>`）
> → ④ 最后才插原生 USB（RNDIS 网卡）**

- **不要先把原生 USB 插上再上电**：USB 网卡会在板子还没枚举好时被主机判失败，
  设备管理器里报 **Code 10 / `CM_PROB_FAILED_START`**（指纹 `DEVPKEY_Device_ProblemCode=10`）。
  遇到就：拔 USB → 板子**断电重上电** → 等固件跑起来 → 最后插 USB
  （或先跑一遍 `tools/pc_env/remove_rndis.cmd`）。
- **原生 USB 插着时不要按板子的 RESET**：同样会把 RNDIS 弄成 Code 10。要复位就先拔 USB。
- **每次烧录之后，必须物理拔插一次原生 USB**：烧录会让板子复位、USB 核回到地址 0 重新初始化，
  但 Windows 不会因此重新枚举，主机仍停在旧枚举状态（表现为「网卡 Up / 200Mbps 已连接，
  但 ping 不通、ARP 完全不回」，很容易误判成固件坏了）。

**串口**：**1000000 8N1**（不是 115200！波特率写错看到的是"二进制乱码 + 回车无反应"，
**很像板子卡死**）。这台板子的串口 RTS 控制 SoC 的供电负载开关，普通 minicom / screen / PuTTY
可能在 open() 时就把板子按在复位态 —— 仓库里的 `tools/pc_env/serial_term.py <COM口>`
和 `tools/pc_env/cmd_cap.py` 就是为绕开这件事写的。

**复位后串口里应当依次出现**：

```
SFBL
ABCD                     ← NuttX 启动进度：A 数据段 / B 缓存 / C HAL / D 即将启动系统
...                      ← 内核启动日志
NuttShell (NSH)
nsh>
```

固件**不需要在 NSH 里敲命令**，开机就自启（实现在 `board/contest_board/src/sifli_ap.c`
的 `board_late_initialize()`），顺序是：

1. `usleep(2s)` → `task_create("robot_ui", 110, …)`（触摸 UI + 显示）
2. `task_create("ai_agent", 100, …)` → `usleep(300ms)`
3. `task_create("hello_app", 100, …)`（= `ai_companion`：常听 / VAD / ASR / 大模型 / TTS）

> ⚠ **`ai_agent` 必须在 `hello_app` 之前起来**（代码注释里写明了原因）：`hello_app` 的大模型
> 这一步走 openvela 框架的 `message_bus`，而**总线只在 `ai_agent` app 里初始化**。
> 顺序反了，`hello_app` 一调就锁到一个从未初始化的 mutex → `NXSEM_IS_MUTEX` 断言把语音任务
> 当场打死。现场现象很迷惑 —— **ASR 能认出人话，但界面钉在「正在想」永远不回话**，
> 而 MQTT 线程还活着，从外面看像"网络不通"。**判据**：`ps` 里只有 `robot_ui` / `hello_app`、
> 没有 `ai_agent`，就是这个。
>
> `robot_ui` 的优先级是 **110**（本树**数值越大越优先**），比后台的 `net_task` / `lpwork` /
> 其它 app（都是 100）高，就是为了网络有流量时不把 UI 挤到 12 次/秒；但它仍低于
> `hpwork`(224，触摸驱动用)，所以触摸中断的 worker 依然能及时送上采样点。

之后串口里还会看到这些（判网络、判密钥是否就绪）：

```
[KWS] 没有模板（/data/kws 是空的，/data 是 tmpfs 重启就丢）→ 唤醒词功能未启用，只靠 VAD 触发…
network_task started
MQTT connecting: broker.emqx.io:1883
MQTT connected
```

- 有 `MQTT connected` = 网络这一环通了；
- 唤醒词那条**默认就是"没有"**（见第六节），不是故障。

### 7. 演示与验证清单

烧录完、按顺序上电之后，照这张表走一遍就能把作品的功能全演示出来。
**每条都给"敲什么 + 期望看到什么"**；逐行判读见 `docs/` 下对应文档。

| 想验什么 | 在哪跑 | 怎么验 | 期望看到什么 |
|---|---|---|---|
| 板子活着 / 外设清单 | 串口 | `hw_test status` | 一份只读快照：网络 / MQTT / ROM 素材 / `/data` / 音频 / 显示 / 触摸 / 按键 / RTC / 运行时间。**只有"网络拿到非回环 IPv4 地址"算 PASS/FAIL**，其它设备缺失只打 `[提示]` |
| 麦克风通不通 | 串口 | `hw_test audio 2` | 录 2 秒，打印 `peak=… avg=… (16k mono 16bit)` + "有声音 / 静音"（对着板子说话，peak 应显著大于 500） |
| 喇叭通不通 / 播放 / 录音 | 串口 | `audio_test 1000 1000`（1kHz 响 1 秒）、`audio_test record 3000`、`audio_test vol <0..1000>` | 喇叭出声；录音打印读到的字节数 |
| 显示与亮度 | 串口 + 看屏 | `hw_test lcd 0` → `30` → `60` → `100`；`hw_test lcdcolor` | 四档都 PASS 且**肉眼可见亮度变化**、回读值等于设定值；`lcdcolor` 刷红/绿/蓝/白四条色带（退出前清屏成纯黑） |
| 触摸 | 看屏 / 镜像窗口 | 直接在界面上点；或 `hw_test touch 10` | 界面有响应。**本队现场这块屏模组的显示/触摸硬件已损坏**，所以改用屏幕镜像 + 鼠标注入（见下两行） |
| 屏幕镜像（本队演示路径） | 板子 + PC | 板子：`hw_test lcdmirror status`；PC：`py -3.10 tools/lcd_mirror/lcd_mirror.py --scale 1.5` | PC 窗口出现板子的画面；**在画布上按/拖 = 板子的触摸**。实测"点一下 → 画面变化 **≈9 ms**"、帧率约 **20~30 fps** |
| 屏幕镜像（断网时的备胎） | 同上 | `py -3.10 tools/lcd_mirror/lcd_mirror.py --serial <COM口> --scale 1.5` | 脚本自己把板子切到串口腿、退出时自己切回（不用手敲命令）；整屏切换约 **1~1.5 秒** |
| 唤醒词 | 串口 | `hw_test kws test`、`hw_test kws selftest`、`hw_test kws enroll 0 4`、`hw_test kws live 10` | **默认只有"没有模板"**（见第六节）。`enroll`/`live`/`selftest` 要**先停掉 `ai_companion`**（麦克风半双工、独占），`threshold` 是例外、可以在跑着的时候改 |
| 云端 ASR | 串口（要联网 + 密钥） | `hw_test asr /etc/assets/test_16k.wav` | 打印识别出的文本（直连大模型，不走火山） |
| 云端 TTS + 出声 | 串口（要联网 + 密钥） | `hw_test tts "你好，今天天气不错"` | 打印合成字节数，并**直接从喇叭放出来** |
| 整条语音闭环 | 板子 + 说话 | 开机后对着板子说一句（或看 `ai_companion` 的日志） | VAD → ASR → 大模型 → TTS 出声；串口出现 `识别结果: …` 与 `AI 回复: …` |
| 网络四层 | 串口 | `net_test` | `[1/4]` 网卡 → `[2/4]` DNS → `[3/4]` TCP → `[4/4]` MQTT CONNACK 逐级 PASS；断了能看出断在哪层 |
| 板子状态（**不占串口**） | PC | `py -3.10 tools/pc_env/diag_query.py [broker]` | 往 `zhi_ai/zhi_ai_001/command` 发 `{"action":"diag"}`，12 秒内收到 `{"type":"diag",…}` 回执（走 MQTT，不碰串口） |
| 报警端到端 | 板子 / 手机 | `hw_test alarm 3 3` | 红色报警页 + 警音 + MQTT 上报（配了 `push_key.txt` 还会推手机） |
| 按键 | 串口 | `hw_test button 15` | 按 PA11(KEY) / PA34(HOME) → 打键名 + 事件类型 + 按住时长 + `[PASS] 按键`；超时 FAIL |
| RTC / 每日提醒 | 串口 | `hw_test rtc 3`、`hw_test rtcday <时> <分>` | 时间读回正确年份（20xx），约 3 秒后打印 `收到 SIGUSR1` |
| 镜像协议（**不需要板子**） | PC | `cd tools/lcd_mirror/host_test && bash run.sh`，再跑 `check_frames.py` / `check_mapping.py` / `check_serial_resync.py` | `check_frames.py` 出 `PASS: 全屏逐像素一致 + v2 头部/RLE 载荷…`；`check_mapping.py` 出 `PASS: 4 个旋转 × 4 个缩放…` |

PC 侧那套东西（镜像的两条腿、串口命令行工具、网络恢复脚本）的完整说明**都在
`tools/README.md`**，本节不重复。

### 8. 板子上网（PC 侧：Windows 自带 NetNat）

板子上网 = `板子 eth0 192.168.137.2 ──USB RNDIS── PC「以太网 2」192.168.137.1 ──NAT── PC 的 WLAN`
（**不依赖板载 WiFi 模块**）。**规范做法是用 Windows 自带的 NetNat（纯 IP 转发），不用 ICS**：

- **为什么换掉 ICS**：ICS 的 NAT + DNS 代理会**反复失效** —— 换一次 Wi-Fi / 挪一次位置之后，
  `以太网 2` 还**是** `192.168.137.1`、`SharedAccess` 服务**也**显示 Running、
  板子 `net_test` 的"网卡已配置"**也**是 PASS，但**板子的 DNS 解析全部失败**
  （MQTT 连不上、大模型问不了、推送发不出），因为 ICS 的代理还绑在旧网络上。
  所以固件改成**把 DNS 直接写死成 `223.5.5.5`**（`CONFIG_NETDB_DNSSERVER_IPv4ADDR=0xdf050505`），
  PC 侧只需要纯 NAT 转发，不再需要 DNS 代理。
- **ICS 方案保留为备选**：`tools/pc_env/ics_on.cmd`（万一某台机器上 NetNat 不可用再退回去）。

**PC 侧要做的事（都要"以管理员身份运行"）**：

```bat
:: ① 建 NAT：先关掉 ICS、把 RNDIS 网卡设成静态 192.168.137.1/24、再建 ZhiAi NetNat，最后 ping 一下板子
tools\pc_env\netnat_on.cmd

:: ② 该网卡设 Private + 按网段放行 + 关掉该网卡的 IPv6
tools\pc_env\fix_net.cmd
```

`fix_net.cmd` 那三步各自都有原因：

1. 不设 Private，Windows 会把它归到「未识别 / 公用」配置文件，防火墙**丢掉一切来自板子的包**
   （镜像连不上、板子 ping 不通）；
2. 放行规则除了绑网卡，还额外绑一条 `192.168.137.0/24` 网段 —— RNDIS 网卡被重新创建后
   网卡的配置文件会退回 Public，网段那条能继续生效；
3. **必须关掉该网卡的 IPv6**：板子是纯 IPv4，Windows 发的每个 IPv6 包都会让 RNDIS 驱动打一行
   ERROR，以约 10~20 KB/s 把板子控制台刷满，把 `nsh` 的输出和命令回显全埋掉。

**判据（每次都要这三条全过才算通）**：

```powershell
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like '*VID_584E*' }   # 两个都是 OK
Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -like '192.168.137.*' }  # 192.168.137.1 在
ping 192.168.137.2                                                              # 通
```

然后板子串口里 `net_test` 应 4/4 全 PASS；`nslookup broker.emqx.io` 能出地址。

> 其它环境脚本（RNDIS 卡在 Code 10、USB 网卡睡死）见 `tools/README.md` 第三节的表。

### 9. 排障表

| 现象 | 大概率原因 / 处置 |
|---|---|
| 编译报 `genromfs: command not found`，或触发 reconfigure 时报 `No module named 'olddefconfig'` | 忘了设 `PATH` / `PYTHONPATH`，回到第 4 节那两条 `export` |
| 改了 `defconfig` 但板上行为没变 | 没有重跑 configure（`config.h` 是 configure 阶段生成的）→ 重跑第 4 节的配置命令，并 `grep` 确认 `config.h` |
| 改了 `src/etc/assets/` 里的文件，上板还是旧的 | 没有删 ROMFS 产物（`romfs.img` / `romfs_etc.c` / `romfs_etc/`）再编 |
| `sftool` 报 `Failed to connect to the chip` / `Failed to download stub` | 按一下板子 RESET、或硬断电 10 秒重试；确认用的是 **0.2.5**；确认复位时没插着原生 USB |
| 板子完全静默、串口一行输出都没有 | 只烧了 `nuttx.bin`，没烧 `ftab.bin@0x12000000` |
| 串口"二进制乱码 + 回车无反应" | 波特率不是 **1000000**（写成 115200 就是这样），很像板子卡死 |
| 串口完全没输出 / 板子像被按住 | 串口工具在 open() 时拉 RTS 复位了板子（本板 RTS 控制供电负载开关）→ 用 `tools/pc_env/serial_term.py`（或 picocom `--no-reset --lower-rts`） |
| 设备管理器里 RNDIS 报 Code 10 | 上电时插着 USB、或插着 USB 按了 RESET → 拔 USB、板子断电重上电、等固件起来、最后插 USB；仍不行跑 `tools/pc_env/remove_rndis.cmd` |
| 网卡 Up / 200Mbps 已连接，但 ping 不通、ARP 完全不回 | 烧录后没有物理拔插一次原生 USB（主机枚举状态陈旧） |
| 板子有 IP 但 DNS 超时 / 板子报 `MQTT DNS 解析失败` | NAT 绑定丢了（换 Wi-Fi、拔插、重建网卡都会）→ 重跑 `tools/pc_env/netnat_on.cmd` |
| 板子→PC 不通（镜像"连上过 0 次"） | 网卡掉回 Public 防火墙配置 → 跑 `tools/pc_env/fix_net.cmd` |
| USB 网卡睡死 / Code 10 反复出现 | Windows 省电把设备关了 → `tools/pc_env/usb_nosleep.cmd` |
| 大模型 / ASR / TTS 全部失败，界面弹"没配密钥" | `agent_config.json` 没放，或放了但没删 ROMFS 产物重编（上板 `ls -l /data/ai_agent/config/` 看不到 `config.json`） |
| ASR 能认出人话，但界面钉在「正在想」永不回话 | `ai_agent` 没起来（消息总线没初始化）→ 串口 `ps` 确认；属自启顺序问题 |
| 语音永远停在「处理中」 | `apps-ai-agent-vela-tls-chunked-end.patch` / `apps-ai-agent-vela-tls-no-block-close.patch` 没打 |
| `ask` 永远报超时 | `apps-ai-agent-llm-clock-fix.patch` 没打 |
| 整屏黑、但串口和触摸还活着 | 敲 `hw_test lcdreinit` 试救（重发面板初始化序列）；救不回就真断电。**本队现场那块屏模组是硬件损坏**，见第六节 |
| 麦克风录着录着没声音了 | 驱动会自愈（见第六节）：约 5 秒后日志出现 `AUDIO: RX 恢复 …` + `[录音] 读已恢复`，上层只丢一帧 |
| 敲完一条命令之后控制台再也不回显（板子还活着、日志照打） | 音频恢复那记"复位整块 DMAC1"把控制台串口的 RX DMA 抹掉了 → `vendor_sifli-uart-rx-dma-reinit.patch` 没打 |

### 10. 队友怎么在自己的代码里用网络

接口怎么用、有哪些坑（回调在哪个任务里跑、LVGL 不能跨任务调），
见 **`docs/network_api_usage.md`**。

## 五、AI Coding 使用说明

### 1. 开发工具

本项目使用 **Claude Code** 进行AI辅助开发，全程记录对话日志。

### 2. AI协助环节

| 环节 | AI协助内容 | 效率提升 |
|------|-----------|---------|
| **需求分析** | 功能模块拆解、技术方案设计 | 节省30%设计时间 |
| **代码实现** | HAL驱动适配、NuttX系统集成 | 节省50%编码时间 |
| **调试优化** | 编译错误修复、性能优化 | 节省40%调试时间 |
| **文档编写** | 代码注释、README生成 | 节省60%文档时间 |

### 3. 关键技术突破

通过AI协作解决的核心问题：
- **HAL库集成**：修复SF32LB52芯片Make.defs，正确引入HAL源文件
- **SysTick驱动**：配置ARMv8M_SYSTICK，解决系统时钟初始化
- **LCD/触摸驱动**：适配bsp_lcd_tp.c，实现屏幕显示和触摸交互
- **内置应用系统**：恢复builtin注册机制，支持NSH命令行

### 4. 日志管理

AI对话日志自动归集到 `logs/` 目录，格式：
```
logs/<github_login>/<date>/<tool>__<session_id>.jsonl
```

提交时执行：
```bash
git add logs/
git commit -s -m "logs: sync AI sessions"
git push
```

## 六、验证与已知限制

> 这一节是**验收清单**：每条都写了"怎么验 + 期望看到什么"，可以逐条打勾。
> 判据都来自真机实测；命令的完整用法见第四节的表格和 `docs/` 下的对应文档。

### 1. 可勾选验收清单

**构建与烧录**

- [ ] **配置 + 编译**：第四节第 4 小节的配置与编译命令全部返回 `0`，并且生成
      `cmake_out/contest2026_233_board_sf32lb52_ai/nuttx.bin`（约 **5.9 MB**，flash 占用约 **35%**）。
- [ ] **烧录**：`sftool` 输出里出现 `flash done`、退出码 `0`；
      `ftab.bin@0x12000000` 与 `nuttx.bin@0x12010000` **两个都写了**。
- [ ] **启动**：串口（1000000 8N1）依次出现 `SFBL` → `ABCD` → NuttX 启动日志 →
      `NuttShell (NSH)` / `nsh>`。

**开机自启**

- [ ] `ps` 里同时能看到 `robot_ui`（优先级 110）、`ai_agent`、`hello_app`（后两个都是 100）
      三个任务，串口有 `MQTT connected`。**期望的顺序**：robot_ui → ai_agent → hello_app。
- [ ] 密钥素材真的进了固件：串口 `ls -l /data/ai_agent/config/` 能看到 `config.json`。

**外设**

- [ ] `hw_test status`：网络一行是"已获取 IPv4 192.168.137.2"（**只有这一项算 PASS/FAIL**），
      其它设备缺失只打 `[提示]`。
- [ ] `hw_test audio 2`：**对着板子说话**，`peak` 应明显大于 500、结论是"有声音"。
- [ ] `hw_test lcd 0` / `30` / `60` / `100`：四档都 PASS、回读值等于设定值，
      而且**肉眼可见亮度变化**（0 = 关屏、100 = 全亮）。
- [ ] `hw_test lcdcolor`：红/绿/蓝/白四条色带正常（退出前会清屏成纯黑）。
- [ ] `hw_test button 15`：按 PA11(KEY) 或 PA34(HOME) → 打键名 + 事件类型 + 按住时长 +
      `[PASS] 按键`；**不按会 FAIL**（这是故意的，用来真验按键）。
- [ ] `hw_test rtc 3`：读回的年份是 20xx（不是 1926），约 3 秒后打印 `收到 SIGUSR1`。

**音频长时间稳定性**（"麦克风跑一阵就永久聋"那条真 bug）

- [ ] 连续录十几分钟不再"永久聋"。真出现时，**期望约 5 秒自愈**，串口依次出现
      `AUDIO: RX 恢复 …`（`RX 恢复 L5 recov=N armed=…`）→
      `RX 健康指纹 … CNDTR=… armed_ms=50` → `[录音] 读已恢复：上一帧之后又有数据了`，
      **上层只丢一帧**（单次 `read` 的超时上界就是 5 秒，之后 50 ms 内就又有数据）。
      判"这一级生效了没有"看的是 `irq` / `half` 计数在下一帧重新开始增长。

**网络**

- [ ] `net_test` 4/4 全 PASS（网卡 → DNS → TCP → MQTT CONNACK）。
- [ ] PC 侧三条判据全过：两个 `VID_584E` 都是 OK、`192.168.137.1` 在、`ping 192.168.137.2` 通。

**语音**

- [ ] `hw_test asr /etc/assets/test_16k.wav`：打印识别出的文本（需要联网 + 密钥）。
- [ ] `hw_test tts "你好，今天天气不错"`：打印合成字节数，并**从喇叭放出来**。
- [ ] 端到端：对着板子说话 → 串口出现 `识别结果: …` 与 `AI 回复: …` → 听到 TTS。

**屏幕镜像**（本队现场就是靠它演示的）

- [ ] PC 端 `py -3.10 tools/lcd_mirror/lcd_mirror.py --scale 1.5` 出现板子画面
      （板子开机默认就是 TCP 腿，不用先在板子上敲命令）。
- [ ] **端到端延迟**：在窗口里点一下 → 画面变化 **≈ 9 ms**（串口腿实测值）。
- [ ] **帧率**：窗口里的 `fps` 读数约 **20~30 fps**
      （LVGL 本身推屏 20~30 帧/s；面板 QSPI 降到 24MHz 之后上界约 34）。
- [ ] **鼠标当触摸**：在画布上按住 / 拖动 → 板子界面跟着动。
- [ ] **串口腿可用**（断网 / 换地方时的备胎）：`--serial <COM口>` 能连上，整屏切换约 1~1.5 秒。
- [ ] **主机侧协议测试（不需要板子）**：`cd tools/lcd_mirror/host_test && bash run.sh`，
      再 `py -3.10 check_frames.py …` / `py -3.10 check_mapping.py`：
      前者出 `PASS: 全屏逐像素一致 + v2 头部/RLE 载荷…`，后者出 `PASS: 4 个旋转 × 4 个缩放…`。

**自检命令速查**

| 命令 | 干什么 |
|---|---|
| `hw_test status` | 统一外设状态快照（网络算 PASS/FAIL，其它只提示） |
| `hw_test audio 2` | 录 2 秒，打录音电平 peak/avg 与有没有声音 |
| `net_test` | 网卡 → DNS → TCP → MQTT 四级连通性自检 |
| `hw_test lcdmirror status` / `start` / `stop` / `uart` / `tcp <ip> [port]` | 屏幕镜像（默认 TCP 到 `192.168.137.1:5600`） |
| `hw_test lcdtap <x> <y> <0\|1>` | 注入一次触摸（串口腿下的触摸入口） |
| `hw_test kws test` / `selftest` / `enroll <slot> [秒]` / `live [秒]` / `threshold <值\|default>` | 唤醒词：查看模板 / 自检 / 录制 / 实时听 / 改阈值 |
| `hw_test asr <wav>` / `hw_test tts <文本>` | 云端识别 / 云端合成并播放 |
| `audio_test <ms> [freq]`、`audio_test record …`、`audio_test vol …`、`audio_test audfix` | 播放 / 录音 / 音量 / 手动打一次 RX 恢复（诊断期临时件） |
| `py -3.10 tools/pc_env/diag_query.py [broker]`（PC 侧） | **不占串口**拉板子状态（走 MQTT 的 `{"action":"diag"}`） |
| `py -3.10 tools/pc_env/serial_term.py <COM口>` / `cmd_cap.py` / `cmd_seq.py` / `raw_cap.py`（PC 侧） | 串口交互终端与抓取（1 Mbaud，带 RNDIS 噪声过滤） |

`hw_test` 每个子命令的细节（哪些"单独运行"、哪些会真开外设、哪些要先停 `ai_companion`）
见 `docs/display_touch_gpio_usage.md` 第 10 节与 `app/hw_test/main.c` 顶部的用法注释。

### 2. 已知限制（照实写）

- **唤醒词（KWS）现在等于没有**：代码已经接线，但 `/data/kws` 里没有模板，而 `/data` 是 tmpfs、
  重启就清空 → **目前没有唤醒词**，交互靠 VAD 常听（说话照样进 ASR）或点界面。
  想开机即用，要么现场 `hw_test kws enroll 0 4`（只对本次开机有效），
  要么把 `slotN.tpl` 放进 `src/etc/assets/kws/` 一起打进固件（见第四节第 3 小节）。
  没有模板时串口会明说一句 `[KWS] 没有模板…→ 唤醒词功能未启用`，这不是故障。
- **跌倒检测没有在本板运行**：本板模组**不带加速度计**（`docs/sensor_rtc_usage.md` 第 1 节），
  "靠 IMU 判跌倒"在这块板子上没有硬件可做；模型这一路的输入尺寸与板端能拿到的数据也不匹配，
  所以不产生有效判定。声音那条弱一些的判据（长时间无声音 / 异常声响）是现成可用的。
- **`/data` 是 tmpfs**：提醒列表、语音设置（`/data/zhi_ai_settings.dat`）、现场录的唤醒词模板
  —— **断电全丢**；开机从 ROMFS 补装的 `config.json` 下一次开机也会重新补装，
  但运行时改过的配置同样不保留。
- **本队现场这块屏模组是硬件损坏的**：显示间歇性黑 / 只剩细笔画，触摸 IC 不应答 I2C
  （`ls /dev/input0` 都没有）。所以才做了"屏幕镜像 + 鼠标注入触摸"，拿 PC 当显示器用。
  **评委手上的好板子不受影响**：镜像默认就不改变板子的显示，PC 端开或不开都行。
- **串口那条镜像腿会和控制台抢同一条线**：所以**默认走 TCP**，串口只在网络坏了 / 换地方时
  由 PC 端 `--serial` 主动切过去（退出时自动切回）。串口腿下整屏切换约 1~1.5 秒，
  这是 1 Mbaud 的物理上限。
- **公共 MQTT broker 会限流**：实测 `broker.emqx.io` 被高频重连后会"TCP 连上、立刻被对端
  关闭、不给 CONNACK"。固件带退避 + broker 自动降级（emqx → mosquitto → hivemq），
  串口会打印 `MQTT broker 切换到 xxx`；也可以在 `agent_config.json` 里用 `mqtt_broker`
  钉死一台自己的 broker。
- **中文字库只覆盖 GB2312 全集**（7010 个字形）：GBK 独有的生僻汉字仍会显示成方块。
  显示前会先做一次清洗（丢掉 emoji / 渲染不了的符号、把 Markdown 降级成纯文本），
  **只洗显示这一份，送给 TTS 的原文不动**。
- **TTS 单次合成文本上限 120 字**，超了会被截断并打日志。
- **板子上网走 USB RNDIS**（不依赖板载 WiFi 模块），所以 PC 侧必须有一台机器做 NAT + 放行；
  板子本身没有独立的上网能力。
- **音频 RX 恢复目前还带"诊断期临时件"**：`audio_test audfix` 是手动入口，
  恢复级在代码里已经收敛成一级（复位整块 DMAC1，且只在没有播放时执行）。
  另有一条**遗留待验**：真机出现过一次"开机之后控制台变聋"，怀疑与恢复里把 `head/tail`
  直接归零有关（见 `patches/README.md` 末尾"⚠️ 遗留"那一节）。

---

**项目地址**: https://github.com/gaoxiaoying0207/contest2026_233_daimazenmepaibudui  
**开发者**: gaoxiaoying0207  
**开发板**: SF32LB52-DevKit-LCD  
**系统**: OpenVeLA (NuttX RTOS)
