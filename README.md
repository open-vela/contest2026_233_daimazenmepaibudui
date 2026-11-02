# 智爱守护——基于 OpenVela 的多模态 AI 居家看护终端

> 面向独居/高龄老人：**开口就能聊天、问天气、设提醒；异常声音/摔倒/喊「救命」会响铃，
> 10 秒内说「没事」可取消，否则自动打电话找人。** 基于 SF32LB52-DevKit-LCD + OpenVela
> （NuttX RTOS），端云协同，断网时本地报警仍然可用。

---

## 项目现状（2026-11-02）

这一版是可演示、可复测的稳定版本：

| 能力 | 当前实现 |
|---|---|
| 语音问答 | 火山 ASR 2.0（方言自动判别）→ 大模型（velaclaw/ReAct，超时回落直连）→ 火山「语音合成2.0」TTS，MiMo 单块兜底 |
| 天气 | **快速通道**：识别到天气意图直接查 Open-Meteo，跳过 ReAct/LLM，首声约 15~20 秒（原 105 秒） |
| 新闻 | **快速通道**：Tavily 优先（`topic=news`，按类别查询），新浪兜底；念标题+内容摘要；「详细说说第X条」可概括 |
| 急救报警 | 关键词「救命/报警/着火」+ 本地撞击；响铃、10 秒确认、Bark 通知、30 秒重发、3 分钟自停 |
| 语音取消 | 报警听窗内短句（≤15 字）即当「没事」停铃；屏幕按钮也停铃（detached 线程即刻掐铃） |
| 本地声音 | DSCNN-lite：fall/knock/scream；fall/knock 都算撞击（单次 ≥0.80 或 15 秒内两次）；人声只允许 scream |
| 屏幕/交互 | 390×450 圆角屏、绿色主题、时钟（UTC+8）、菜单、设置、提醒、报警页 |
| 联网 | USB RNDIS + PC 侧 NAT；联网后 HTTPS `Date:` 头自动对时 |
| 稳定性 | 录音单读者 `owner_gen` 修复（stop 不再取消旧读者的 5 秒心跳）；报警抢设备只走 `audio_play_stop()` |

> 详细模块/数据流见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)；
> 验收判据见 [docs/TESTING.md](docs/TESTING.md)；
> 常见故障见 [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)。

## 一、作品简介

面向独居及高龄老人，依托 SF32LB52-DevKit-LCD 运行 OpenVela 系统打造的智能居家安全看护终端。
本板实际具备**语音陪伴、天气/搜索、定时提醒、异常声音与摔倒检测、紧急报警与家属通知、
联网对时**；摄像头视觉识别、毫米波雷达、独立 Wi-Fi 属于方案愿景，当前硬件不具备。

**核心功能：**

- **语音陪伴**：常开麦 + VAD，无需唤醒词；方言 ASR、自然 TTS。
- **生活助手**：天气快速查询、上网搜索、语音设提醒、音量/亮度语音调整。
- **安全守护**：敲击/尖叫/摔倒声本地识别；喊「救命」无条件报警；报警响铃 + 家属 Bark。
- **可取消**：10 秒内说「没事」（短句即可）或点屏幕按钮即停铃，不打扰家人。
- **联网自治**：RNDIS 联网、自动对时、MQTT 状态回传、故障自愈（录音守护/看门狗）。

## 二、选题方向

**AI 硬件产品创新** —— 基于 openvela + ai_agent，开发「能主动、会执行」的嵌入式 AI Agent 应用。

## 三、系统架构

```
麦克风 ─ VAD/门控 ─ ASR(火山) ─ 轻意图/天气快路 ─ LLM(velaclaw/mimo_chat)
                                                   │
喇叭 ◄─ 流式 TTS(火山2.0, 逐块) ◄──────────────────┘

本地声音门控 ─ fall/knock/scream ─┐
语音关键词「救命」 ──────────────┼─► voice_emerg：响铃 → 10s 确认 → Bark → 30s 重发 → 180s 自停
屏幕大红按钮 / MQTT 报警 ────────┘

USB RNDIS(584e:5342) ─ 192.168.137.2 ⇄ PC 192.168.137.1 ─ NAT ─ 互联网
```

上电顺序（`sifli_ap.c`）：`robot_ui(110)` → `ai_agent(100)` → `hello_app(100)`；
`hello_app` 依赖 `ai_agent` 的消息总线，顺序不能换。完整说明见
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

## 四、目录结构

```text
contest2026_233_daimazenmepaibudui/
├── app/
│   ├── hello_app/        # 常开麦、ASR/LLM/TTS、音频半双工、声音检测、语音急救
│   ├── robot_ui/         # LVGL 界面、菜单、提醒、MQTT/Bark、对时
│   ├── hw_test/          # 硬件自检命令（显示/按键/RTC/音频/ASR/TTS）
│   ├── zhi_ai/           # 智爱业务入口
│   └── audio_test/       # 录放/循环测试命令
├── board/contest_board/  # SF32LB52 板级适配 + 音频/报警驱动
├── docs/                 # 架构、配置、编译烧录、测试、排障、代码导读
├── patches/              # 三棵上游树的权威补丁 + 一致性/凭据校验脚本
├── flash/                # ftab.bin 等烧录材料
├── logs/                 # AI Coding 过程日志
├── model_output/         # 端侧声音模型的训练/导出/对拍产物
├── tools/                # PC 侧训练、镜像、诊断工具
└── README.md
```

## 五、快速开始

### 1. 获取代码与补丁

```bash
repo init -u https://github.com/gaoxiaoying0207/contest2026_233_daimazenmepaibudui \
  -b dev-ai-contest-2026 -m contest2026_233_daimazenmepaibudui.xml
repo sync -c -j8

cd <工作区>
git -C nuttx apply <仓库>/patches/nuttx-working-tree.patch
git -C vendor/sifli apply <仓库>/patches/vendor-sifli-working-tree.patch
git -C packages/ai_agent apply <仓库>/patches/apps-ai-agent-working-tree.patch
bash <仓库>/patches/verify_upstream_trees.sh   # 三绿才提交
```

> 只打这三份 working-tree 补丁；不要叠加 `patches/` 里的 per-fix 历史补丁
> （会冲突，原因见 `patches/README.md` 顶部）。

### 2. 放密钥（不放进 Git）

```bash
cp <你的密钥>/agent_config.json  <仓库>/board/contest_board/src/etc/assets/
cp <你的密钥>/push_key.txt       <仓库>/board/contest_board/src/etc/assets/
```

字段与火山 2.0 参数见 [docs/CONFIGURATION.md](docs/CONFIGURATION.md)。
**这两个文件已在 `.gitignore`，提交前确认 `git status` 里没有它们。**

### 3. 编译

```bash
export PATH=<工作区>/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:\
<工作区>/prebuilts/build-tools/linux-x86_64/bin:$PATH
cmake --build <工作区>/cmake_out/contest2026_233_board_sf32lb52_ai -j$(nproc)
```

产出 `cmake_out/contest2026_233_board_sf32lb52_ai/nuttx.bin`。

> ⚠️ 改了 `src/etc/assets/` 后，ROMFS **不会自动更新**：要同步暂存目录、删
> `romfs.img`/`romfs_etc.c` 再编（否则新密钥不进固件）。完整步骤见
> [docs/BUILD_AND_FLASH.md §3](docs/BUILD_AND_FLASH.md#3-romfs-与密钥同步必看)。

### 4. 烧录与冷启动

```bash
sftool -c SF32LB52 -p /dev/ttyACM0 -b 1000000 write_flash \
  <仓库>/flash/ftab.bin@0x12000000 \
  <工作区>/cmake_out/contest2026_233_board_sf32lb52_ai/nuttx.bin@0x12010000
```

烧完**拔电 5 秒再插回**（冷启动）；不要按复位键（warm reset 会卡死 USB 数据通道）。
细节与失败重试见 [docs/BUILD_AND_FLASH.md](docs/BUILD_AND_FLASH.md)。

### 5. 联网

```bash
lsusb | grep 584e           # 必须看到 NuttX RNDIS gadget
~/rndis_net_up.sh          # 配 192.168.137.1/24 + NAT
ping -c 2 192.168.137.2    # 通
```

串口 1000000 8N1。开机判据：`[版本]` 标记、`[netmgr] Network connected`、
`[TimeSync] 已对时`、`[信标] up=… sm=…` 持续在走。

## 六、功能与验收

| 功能 | 怎么测 | 通过判据 |
|---|---|---|
| 语音问答 | 说「给我讲个小故事」 | `[AI] 识别结果` → `[volc_tts] TTS V3 request` → 出声，无 403 |
| 天气 | 说「济南天气怎么样」 | `[天气快路] 城市=济南…`，20 秒内出声 |
| 救援关键词 | 喊「救命」 | `[急救词] 命中` → 响铃 |
| 撞击求救 | 15 秒内敲两下 | `第一次撞击…先不报警` → `第二次撞击…确认求救` |
| 新闻 | 说「今天有什么娱乐新闻」 | `[新闻快路]` + `News via Tavily OK`，念标题+摘要 |
| 新闻概括 | 说「详细说说第一条」 | `[新闻概括]`，念内容摘要 |
| 取消报警 | 铃停空档说「没事」或点「我没事,取消」 | `[急救] 老人说没事 -> 停铃` |
| 家属通知 | 报警 10 秒不吭声 | `[PUSH] OK: HTTP 200`，30 秒重发，3 分钟自停 |
| 断网报警 | 拔 RNDIS 后喊「救命」 | 本地响应/响铃仍工作 |

逐条日志判据与已知不达标项见 [docs/TESTING.md](docs/TESTING.md)。

## 七、AI Coding 使用说明

项目使用 AI 工具辅助需求拆解、OpenVela/NuttX 适配、音频 DMA 调试、端侧模型前端对拍、
编译排错与文档整理。对话日志按成员/日期保存在 `logs/`，随提交材料一并提交。
本项目坚持"**先证据、后改动**"：每个修复都对应串口指纹（见 `docs/TROUBLESHOOTING.md`），
没有日志的猜测不改代码。

## 八、已知限制与排障

- 本板无摄像头/IMU/独立 Wi-Fi：视觉识别、姿态跌倒、Wi-Fi 联网不在本板范围内；
- 唤醒词已按用户要求取消；交互靠 VAD 常听 + 界面入口；
- `/data` 是 tmpfs：提醒、设置、唤醒词模板重启丢失；
- 联网依赖 USB RNDIS + PC NAT；RNDIS 数据通道需冷启动恢复；
- 普通问答首字约 20 秒（云端 ASR/LLM 往返，未做 token 级流式）；
- 定时提醒仅 RAM，语音删/改提醒未实现；
- 新闻已走快速通道；普通搜索仍可能卡在"搜完再让 LLM 总结"这一步；
- 「救命」请清楚地说：嘶吼/长音频的 ASR 可能返回空（真机 10.3s 喊叫实测）；
- 音频 8 秒卡死已加 `owner_gen` 修复，仍需长时间真机验证；
- TLS 出站连接偶发堆积（`D25` 指纹），已列入排查。

排障先看串口指纹：[docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)。

## 九、代码导读

先读文件头注释，再看函数；按功能找文件的路线见
[docs/CODE_TOUR.md](docs/CODE_TOUR.md)。核心入口：

- `app/hello_app/ai_companion_main.c`：主循环、VAD→ASR→LLM→TTS、天气快路、看门狗；
- `app/hello_app/mimo_voice.c`：MiMo/火山后端、分块与 HTTP；
- `app/hello_app/voice_emerg.c`：报警状态机（响铃/确认/Bark/自停）；
- `app/hello_app/ai_audio.c` + `board/.../sf32lb52_audio.c`：半双工音频与单读者守卫；
- `app/robot_ui/`：界面、菜单、提醒、MQTT/Bark、对时。

## 十、项目地址

- 仓库：https://github.com/gaoxiaoying0207/contest2026_233_daimazenmepaibudui
- 开发板：SF32LB52-DevKit-LCD
- 系统：OpenVela（NuttX RTOS）
- 团队：代码怎么跑不队
