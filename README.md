# 智爱守护 — 基于 OpenVela 的 AI 居家看护终端

基于 SF32LB52-DevKit-LCD + OpenVela（NuttX RTOS）的端云协同 AI 看护设备。
面向独居/高龄老人：语音问答、天气/新闻/搜索、定时提醒、异常声音与摔倒检测、
紧急报警与家属通知、联网对时。断网时本地报警仍然可用。

---

## 1. 功能概览

| 功能 | 实现 |
|---|---|
| 语音问答 | 火山 ASR 2.0 与 MiMo ASR **并行赛跑**（先返回非空文本者用）→ 大模型（velaclaw/ReAct，超时回落直连）→ 火山「语音合成2.0」TTS，MiMo 单块兜底 |
| 天气 | **直接问 MiMo**（get_weather 工具：IP 定位 + Open-Meteo 免 key）；MiMo 失败时回落本地 Open-Meteo 快路 |
| 实时信息 | 用户原话直接交 Tavily 通用检索 → 新浪财经免 key 兜底；念前 3 条标题 + 第一段正文（过滤版权/页脚） |
| 搜索 | Tavily → SerpAPI → Exa（按配置的 key 依次尝试） |
| 急救报警 | 关键词「救命/报警/着火」（20+ 变体）+ 本地撞击检测；响铃 → 10 秒确认 → Bark 通知 → 30 秒重发 → 3 分钟自停 |
| 语音取消 | 报警听窗内短句按 UTF-8 字符数（≤15 字）判「没事」；低电平短句补静音+最多 10× 增益；火山空结果时 MiMo 二次识别（只认「没事/不用/没有」）；屏幕按钮也停铃 |
| 本地声音 | DSCNN-lite：fall/knock/scream；撞击需单次 ≥0.80 或 15 秒内两次 |
| 提醒 | RTC 日循环闹钟 + 语音建提醒（RAM，掉电丢失） |
| 屏幕/交互 | 390×450 圆角屏、**黄白色暖调主题**（奶油黄白底 + 白色圆角卡片 + 暖阳黄主色）、时钟（UTC+8）、菜单、设置、报警页 |
| 联网 | USB RNDIS + PC 侧 NAT；TLS 长连接开启 TCP keep-alive；联网后 HTTPS `Date:` 头自动对时 |

## 2. 使用的模型与服务

### 2.1 语音识别（ASR）

| 优先级 | 服务 | 模型 / 资源 ID | 说明 |
|---|---|---|---|
| 并行 1 | 火山引擎 | `volc.seedasr.sauc.duration`（流式语音识别 2.0） | 支持方言自动判别 |
| 并行 2 | 小米 MiMo | `mimo-v2.5-asr` | 单次 HTTPS POST，与火山同时发，先返回非空文本者用 |

> 两个后端并行跑，12 秒超时；任一端不可用时自动退回串行识别。
> 只有 `mimo-v2.5-asr` 是真 ASR；用 `mimo-v2.6-pro` 当 ASR 会吐描述而不是转写。
> 弱音/噪声下 MiMo 可能编字，火山返回 `-61`（ENODATA，没听出字）时不宜无条件采信 MiMo。

### 2.2 语音合成（TTS）

| 优先级 | 服务 | 模型 / 音色 | 说明 |
|---|---|---|---|
| 1 | 火山引擎 | `seed-tts-2.0` + `zh_female_vv_uranus_bigtts` | 语音合成 2.0，逐块流式 |
| 2 | 小米 MiMo | `mimo-v2.5-tts` | 火山单块失败时兜底 |

### 2.3 大语言模型（LLM）

| 优先级 | 服务 | 模型 | 说明 |
|---|---|---|---|
| 1 | velaclaw（ai_agent ReAct） | `mimo-v2.6-flash` | 支持工具调用；超时 -110 后回落直连 |
| 2 | 小米 MiMo 直连 | `mimo-v2.6-flash` | velaclaw 挂死时直接 POST |

### 2.4 外部服务

| 服务 | 用途 | 是否必需 |
|---|---|---|
| Tavily | 新闻 / 搜索 | 强烈建议配置 |
| 新浪财经 | 新闻免 key 兜底 | 否（免 key） |
| Open-Meteo | 天气免 key 兜底 | 否（免 key） |
| Bark | 报警推送到家属手机 | 建议配置 |

## 3. 密钥配置

### 3.1 文件位置

所有密钥放在 **`board/contest_board/src/etc/assets/`** 下，编译时打进 ROMFS，
开机由板级代码拷到 `/data/ai_agent/config/config.json`。

```
board/contest_board/src/etc/assets/
├── agent_config.json   # 所有 API 密钥（不进 Git）
└── push_key.txt        # Bark 推送 key（不进 Git）
```

**这两个文件已在 `.gitignore`，不会上传到 GitHub。** 别人拿到代码后需要自己填。

### 3.2 `agent_config.json` 完整字段

```json
{
  "model": "mimo-v2.6-flash",
  "llm_host": "api.xiaomimimo.com",
  "llm_path": "/v1/chat/completions",
  "llm_port": "443",
  "api_key": "<小米 MiMo API Key>",
  "llm_backend_0": "{\"host\":\"api.xiaomimimo.com\",\"path\":\"/v1/chat/completions\",\"port\":\"443\",\"api_key\":\"<同 api_key>\",\"model\":\"mimo-v2.6-flash\",\"priority\":0,\"cost_tier\":1}",

  "asr_model": "mimo-v2.5-asr",
  "asr_language": "zh",

  "volc_api_key": "<火山新控制台 API Key（36 位 UUID）>",
  "volc_asr_resource": "volc.seedasr.sauc.duration",
  "volc_speaker": "zh_female_vv_uranus_bigtts",

  "tavily_key": "<你的 Tavily API Key（tvly- 开头）>"
}
```

### 3.3 各密钥获取方式

| 字段 | 获取位置 | 格式示例 |
|---|---|---|
| `api_key` | [小米 MiMo 开放平台](https://xiaomimimo.com) → API Key | `sk-xxxxxxxx...`（按控制台签发为准） |
| `volc_api_key` | 火山引擎控制台 → API Key 管理 | `xxxxxxxx-xxxx-...-xxxxxxxxxxxx`（36 位 UUID） |
| `volc_asr_resource` | 固定值 | `volc.seedasr.sauc.duration` |
| `volc_speaker` | 固定值（TTS 2.0 音色） | `zh_female_vv_uranus_bigtts` |
| `tavily_key` | [Tavily 控制台](https://app.tavily.com) → API Keys | `tvly-xxxxxxxx...`（58 位） |
| `push_key.txt` | Bark App → 设备 → 复制 Key | 一行纯文本 |

### 3.4 天气：MiMo + Open-Meteo（不再接和风）

天气问题直接交给 MiMo 的 `get_weather` 工具：MiMo 用 IP 定位到城市（城市级），
再调 Open-Meteo 免 key 接口取实况；MiMo 失败时回落到本地 Open-Meteo 快路。
**不再使用和风天气**，配置里不需要 `qweather_host` / `qweather_api_key`。

### 3.5 Tavily 配置说明

1. 注册 [app.tavily.com](https://app.tavily.com)
2. 控制台 →「API Keys」→ 创建
3. 填入 `agent_config.json` 的 `tavily_key`（`tvly-` 开头）

> 未配置时新闻走新浪财经（免 key，只有财经要闻）、搜索不可用。

## 4. 编译

### 4.1 获取代码

```bash
repo init -u https://github.com/gaoxiaoying0207/contest2026_233_daimazenmepaibudui \
  -b dev-ai-contest-2026 -m contest2026_233_daimazenmepaibudui.xml
repo sync -c -j8
```

### 4.2 打补丁

```bash
cd <工作区>
git -C nuttx apply <仓库>/patches/nuttx-working-tree.patch
git -C vendor/sifli apply <仓库>/patches/vendor-sifli-working-tree.patch
git -C packages/ai_agent apply <仓库>/patches/apps-ai-agent-working-tree.patch
bash <仓库>/patches/verify_upstream_trees.sh   # 三绿才提交
```

### 4.3 编译

```bash
export PATH=<工作区>/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:\
<工作区>/prebuilts/build-tools/linux-x86_64/bin:$PATH
cmake --build <工作区>/cmake_out/contest2026_233_board_sf32lb52_ai -j$(nproc)
```

产出 `cmake_out/contest2026_233_board_sf32lb52_ai/nuttx.bin`。

### 4.4 ROMFS 同步（修改密钥后必做）

```bash
SRC=<仓库>/board/contest_board/src/etc
DST=<工作区>/cmake_out/contest2026_233_board_sf32lb52_ai/boards/exclude_board/src/romfs_etc
cp -a "$SRC/." "$DST/"
rm -f <工作区>/cmake_out/.../romfs.img <工作区>/cmake_out/.../romfs_etc.c
cmake --build <工作区>/cmake_out/contest2026_233_board_sf32lb52_ai -j$(nproc)
```

## 5. 烧录与运行

```bash
sftool -c SF32LB52 -p /dev/ttyACM0 -b 1000000 write_flash \
  <仓库>/flash/ftab.bin@0x12000000 \
  <工作区>/cmake_out/contest2026_233_board_sf32lb52_ai/nuttx.bin@0x12010000
```

烧完**拔电 15 秒再插回**（冷启动），不要按复位键（warm reset 会卡死 USB 数据通道）。

联网：

```bash
lsusb | grep 584e           # 看到 NuttX RNDIS gadget
~/rndis_net_up.sh          # 配 192.168.137.1/24 + NAT
ping -c 2 192.168.137.2    # 通
```

串口 1000000 8N1。开机判据：`[版本]` 标记、`[netmgr] Network connected`、
`[TimeSync] 已对时`、`[信标] up=… sm=…` 持续在走。

## 6. 验收判据

| 功能 | 怎么测 | 通过判据 |
|---|---|---|
| 语音问答 | 说「给我讲个小故事」 | `[AI] 识别结果` → `[volc_tts] TTS V3 request` → 出声 |
| 短句秒回 | 说「你好」 | `[短句快路]` → 1 秒内出声 |
| 天气 | 说「济南天气怎么样」 | `[天气] 直接问 MiMo`（MiMo 失败后回落 `[天气快路]`），20 秒内出声 |
| 实时信息 | 说「今天有什么新闻」 | `[实时] Tavily 查询：…` + `[实时] 取到结果`，念标题+正文 |
| 急救关键词 | 喊「救命」 | `[急救词] 命中` → 响铃 |
| 撞击求救 | 15 秒内敲两下 | `第一次撞击…` → `第二次撞击…确认求救` |
| 取消报警 | 说「没事」或点「我没事,取消」 | `[急救] 老人说没事 -> 停铃` |
| 家属通知 | 报警 10 秒不吭声 | `[PUSH] OK: HTTP 200`，30 秒重发，3 分钟自停 |
| 断网报警 | 拔 RNDIS 后喊「救命」 | 本地响铃仍工作 |

详细判据见 [docs/TESTING.md](docs/TESTING.md)。

## 7. 目录结构

```text
contest2026_233_daimazenmepaibudui/
├── app/
│   ├── hello_app/        # 常开麦、ASR/LLM/TTS、音频半双工、声音检测、语音急救
│   ├── robot_ui/         # LVGL 界面、菜单、提醒、MQTT/Bark、对时
│   ├── hw_test/          # 硬件自检命令
│   └── audio_test/       # 录放/循环测试命令
├── board/contest_board/  # SF32LB52 板级适配 + 音频/报警驱动
├── docs/                 # 架构、配置、编译烧录、测试、排障、代码导读
├── patches/              # 三棵上游树的权威补丁 + 校验脚本
├── flash/                # ftab.bin 等烧录材料
├── logs/                 # AI Coding 过程日志
├── skills/               # AI Skill 定义
├── model_output/         # 端侧声音模型训练/导出产物
├── tools/                # PC 侧训练、镜像、诊断工具
└── README.md
```

## 8. 关键设计约束

1. **报警抢音频只走 `audio_play_stop()`** —— `audio_record_hold_end()` 会跨线程踩 fd，一报警整机卡死。
2. **录音单读者 `rx_owner_gen`** —— 只有 owner 能清 `rx_busy`，新读者一律让路。
3. **UI 线程不做阻塞动作** —— HTTPS/TLS/join 一律投递到工作线程；所有锁用二值信号量（无持有者跟踪）。
4. **半双工播放期间麦是聋的** —— 报警确认窗用"响-停-听"循环，听窗 ≥ 3.6 秒。
5. **凭据只在 ROMFS**、`.gitignore` 已挡。

## 8.1 板级适配与驱动要点

1. **芯片/架构**：SF32LB52 Cortex-M33（ARMv8-M），SiFli HAL；修正 M33 systick 配置名（`CONFIG_ARM_M_SYSTICK` → `CONFIG_ARM_SYSTICK`）。
2. **内存/存储**：内部 SRAM 512 KB + 8 MB OPI-PSRAM（链接脚本 `psram @0x60000000, LENGTH=8M`）；16 MB NOR，`ftab.bin@0x12000000`、`nuttx.bin@0x12010000`；ASR/TTS 大缓冲放 PSRAM。
3. **显示**：CO5300 AMOLED 390×450，QSPI 4 线 24 MHz（全屏约 29 ms），VADD_EN PA37、复位 PA00；亮度走面板 `0x51 WBRIGHT`；CO5300 不吃 MADCTL 换轴，横屏只能 flush 转置。
4. **触摸**：FT6146 挂 I2C0，IRQ pin 31，复位 PA09；LCD 初始化完成后再初始化触摸；触摸 IC 偶发不应答时，PC 镜像的虚拟触摸兜底。
5. **音频**：AUDCODEC（DAC/ADC 模拟）+ AUDPRC（TX0 播放 / RX0 录音 DMA）+ AW8155 功放（PA10）；固定 16 kHz / 单声道 / s16le；半双工（播放停 ADC、录音停 DAC）；录音单读者 `rx_owner_gen`；报警抢音频只走 `audio_play_stop()`；`write()` 原子串行化，TTS 与提醒/报警不再并发写坏共享等待。
6. **USB/网络**：MUSB 设备控制器；RNDIS gadget（`584e:5342`，全速 12 Mbps，MAC `00:e0:4c:53:42:31`）+ CDC-ACM 串口；板侧 192.168.137.2，PC 侧 192.168.137.1；补丁解决 TXPKTRDY/FIFO flush、bulk OUT 双提交、RNDIS QUERY stall；warm reset 会卡数据通道，烧录后必须拔电冷启动。
7. **RTC/报警/按键**：`/dev/rtc0` 的 `read()` 永远 EOF，只能用 `RTC_SET_ALARM` + 信号；板级单独 worker 设 alarm、收信号、回调；未开 `CONFIG_LIBC_LOCALTIME`，UTC+8 与日期进位在板级手算；板级报警 worker 优先级 115，每 50 ms 写一块 PCM，`alarm_trigger/clear` 非阻塞。
8. **启动/ROMFS/交付**：`board_late_initialize` 初始化 RTC/ADC/audio/GPIO/PWM/buttons/I2C/SPI/LCD 异步任务/看门狗/MTD/CDC-ACM；挂载 tmpfs `/data`，把只读 ROMFS 的 `agent_config.json` 拷入；`task_create` 拉起 `robot_ui(110)`、`ai_agent(100)`、`hello_app(100)` 和 `hello_app_wd(110)`；改 `etc/` 必须删 ROMFS 三个产物再编；上游改动以三份 working-tree 补丁交付。

## 9. 已知限制

- 本板无摄像头/IMU/独立 Wi-Fi。
- `/data` 是 tmpfs：提醒、设置重启丢失。
- 普通问答首出约 15~25 秒（ASR 4~12s + velaclaw 5~11.5s + TTS 首块 ~2.2s）；本地时间快路约 9 秒，短句 1 秒内。
- 实时检索条数受 Tavily 索引限制（中文约 1~3 条）；已取消「第X条详细说说」专门路径。
- TLS 出站连接堆积（`[TCP] pool` / `st[D…]` 指纹）已在内核修复：TIME_WAIT 到期补 `tcp_free`，并在每次新建连接时主动回收僵尸 CLOSED 条目；45 分钟真机回归 `pool` 稳定在 5~7、`D` 为 0~1。
- 播放看门狗不再误杀 TTS 尾音（DONE 阶段跳过停滞判据）；驱动 `write()` 已原子串行化，TTS 与提醒/报警并发写不再卡死——长故事已能完整念完。
- 极短语音（<0.8 秒）与轻声说话仍可能只转出单字或返回 -61；已用补静音、低增益与 MiMo 二次识别兜底，但识别率仍受麦克风距离与云端能力限制。
- 录音 8 秒卡读已加 `owner_gen` 修复；极端负载下 HAL 读等待仍偶发卡读，监听守护会重开会话，需继续收口。

## 10. 文档索引

| 文档 | 内容 |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 模块、线程、数据流、关键不变量 |
| [docs/CONFIGURATION.md](docs/CONFIGURATION.md) | 密钥字段详解、后端优先级 |
| [docs/BUILD_AND_FLASH.md](docs/BUILD_AND_FLASH.md) | 编译、ROMFS 同步、烧录、RNDIS |
| [docs/TESTING.md](docs/TESTING.md) | 逐项验收判据与日志指纹 |
| [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | 故障串口指纹 → 定位 |
| [docs/CODE_TOUR.md](docs/CODE_TOUR.md) | 按功能找文件的阅读路线 |
| [docs/RELEASE_NOTES_2026-11-02.md](docs/RELEASE_NOTES_2026-11-02.md) | v41/v42 稳定性修复清单与日志证据 |
| [patches/README.md](patches/README.md) | 三份权威补丁与校验脚本 |

## 11. 项目地址

- 仓库：https://github.com/gaoxiaoying0207/contest2026_233_daimazenmepaibudui
- 开发板：SF32LB52-DevKit-LCD
- 系统：OpenVela（NuttX RTOS）
- 团队：代码怎么跑不队
