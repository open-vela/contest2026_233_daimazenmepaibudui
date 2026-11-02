# 系统架构

> 本文回答"这台设备由哪些模块组成、一次语音/一次报警/一次联网分别走哪条路"。
> 想按文件读代码，见 [CODE_TOUR.md](CODE_TOUR.md)；想编译烧录，见
> [BUILD_AND_FLASH.md](BUILD_AND_FLASH.md)。

## 1. 总体结构

```
┌──────────────────────────── 应用层（本仓库 app/）────────────────────────────┐
│  hello_app (ai_companion)                 robot_ui (LVGL 界面/网络/报警页)   │
│  ├─ ai_companion_main.c  主循环/状态机    ├─ robot_ui.c   主屏/表情/时钟     │
│  ├─ mimo_voice.c         对话/ASR/TTS     ├─ touch_ui.c   菜单/设置/提醒     │
│  ├─ ai_audio.c           半双工音频       ├─ network_comm.c MQTT/Bark/推送   │
│  ├─ ai_sound_detect.c    门控+小模型旁路  ├─ time_sync.c  联网对时           │
│  ├─ sound_event.c        本地声音分类     └─ reminder_sched.c RTC 定时提醒    │
│  ├─ voice_emerg.c        语音急救报警                                        │
│  └─ prompt_ack.c         提示音                                              │
├──────────────────────────── 框架层（packages/ai_agent）──────────────────────┤
│  agent_loop / llm_proxy（大模型与工具）  voice_channel / volc_asr / volc_tts │
│  infra/vela_tls.c（HTTPS + 连接池）      channels/ws_server / mqtt            │
├──────────────────────────── 板级层（board/contest_board）────────────────────┤
│  sf32lb52_audio.c / sf32lb52_audio_in.c   音频驱动（半双工、DMA、看门狗）     │
│  sf32lb52_alarm.c                        板级报警音（独立 fd）               │
│  sifli_ap.c                              上电顺序、RNDIS gadget、任务拉起      │
├──────────────────────────── 上游（repo 管理的三棵树）────────────────────────┤
│  nuttx/    RTOS、TCP/IP、audio 核心      vendor/sifli/  芯片/板级 HAL         │
└──────────────────────────────────────────────────────────────────────────────┘
```

板子上电后由 `sifli_ap.c` 的 `board_late_initialize()` 依次拉起：

| 任务 | 优先级 | 作用 |
|---|---|---|
| `robot_ui` | 110 | LVGL 界面、触摸、网络任务、报警页；`lv_timer_handler` 每秒统计耗时 |
| `ai_agent` | 100 | 框架的 message_bus / agent_loop / LLM 工具链 |
| `hello_app` | 100 | 常开麦、VAD、ASR、对话、TTS、声音门控、语音急救 |

> 顺序是硬要求：`hello_app` 初始化时就会用 `ai_agent` 的总线，先起 ai_agent
> 再起 hello_app，否则会撞未初始化 mutex（见 `sifli_ap.c` 注释）。

## 2. 一次语音问答

```
麦克风 ──► ai_audio 常开录音线程 ──► VAD ──► speech_capture_begin/complete
                                                        │
                          ┌─────────────────────────────┘
                          ▼
                 dialogue_worker（识别线程）
                          │  voice_asr_recognize()  ← 火山 v3 WebSocket（方言）
                          │  （失败/为空按既定策略回落 MiMo）
                          ▼
                 轻意图：灯控 / 急救词 /「没事」/ 天气·新闻快速通道
                          │
                          ▼
             velaclaw（ai_agent ReAct）──失败(-110)──► mimo_chat 直连兜底
                          │
                          ▼
             llm_complete_callback：逐块 TTS（见下）
                          │
                          ▼
             audio_play_start() ──► 喇叭（半双工，播完自动还麦）
```

**TTS 流水线**（`ai_companion_main.c` 的"流式 TTS"一节）：

- 生产者 = LLM 回调线程：按句切块、逐块合成，队列深度 2、3 个 PCM 槽；
- 消费者 = 播放线程回调：取块、起播，播放完成再取下一块；
- 屏幕上"念到哪儿、显示到哪儿"（`g_tts_shown` + 队列里的 `end`）；
- `record_hold`：整轮播报期间不还麦，避免旁边的声音把 AI 从"说"切到"听"。

**后端选择**（2026-11-01）：

| 能力 | 首选 | 兜底 |
|---|---|---|
| ASR | 火山 v3（`volc.seedasr.sauc.duration`，方言自动判别） | MiMo `mimo-v2.5-asr` |
| TTS | 火山「语音合成模型2.0」`seed-tts-2.0` + `zh_female_vv_uranus_bigtts` | MiMo（按块兜底） |
| LLM | velaclaw → ai_agent ReAct | `mimo_chat` 直连 |

**天气/新闻快速通道**（2026-11-01）：在送 LLM 之前识别意图，直接取数据并交给
TTS 回调，绕过 velaclaw/ReAct 和"搜完再总结"的 LLM 轮：

- 天气：Open-Meteo geocode + forecast（默认济南；已知城市表外的说法自动回落）；
- 新闻：复用 ai_agent 的新浪免 key 接口，取前几条头条直接念；
- 日志：`[天气快路]` / `[新闻快路]`；任一失败自动回落原路径。

## 3. 一次报警

有两条来源，最终汇合到同一份实现（`voice_emerg.c`）：

```
本地小模型命中 / 摔倒 / 敲击 / 喊叫 ──┐
语音关键词「救命/报警/着火」 ────────┤──► alarm_escalate()
屏幕红色大圆按钮 / MQTT start_alarm ──┘        │
                                              ▼
                             ┌──────── voice_emerg ────────┐
                             │ 响 1.2s / 听（确认期 2.8s）  │
                             │ 10s 内说「没事」或点按钮 → 停 │
                             │ 10s 未响应 → Bark 通知家人    │
                             │ 之后每 30s 重发，180s 自停    │
                             └──────────────────────────────┘
```

判据/常量都在 `app/hello_app/voice_emerg.h`：

| 常量 | 值 | 含义 |
|---|---|---|
| `VOICE_EMERG_CONFIRM_MS` | 10000 | 10 秒确认窗 |
| `VOICE_EMERG_RING_MS` | 1200 | 每轮响 |
| `VOICE_EMERG_LISTEN_CONFIRM_MS` | 2800 | 确认期听（够 VAD 收完「没事」） |
| `VOICE_EMERG_LISTEN_MS` | 1200 | 通知后听 |
| `VOICE_EMERG_RENOTIFY_MS` | 30000 | Bark 重发 |
| `VOICE_EMERG_AUTOSTOP_MS` | 180000 | 3 分钟自停 |

**"说没事"的链路**：报警听窗开始时，`gate_voice_tick()` 在铃停的间隙直接
`speech_capture_begin()`（放行 `record_hold`），VAD 收完后无条件
`dialogue_request()` → ASR → `voice_emerg_on_text()`。不依赖状态机从
ALARM/AI_TALKING 切换。

**本地声音分类**（`sound_event.c` + `ai_sound_detect.c`）：

- 门控每 200ms 判一窗（能量/峰值/浊音/自相关/ZCR）；
- 人声窗**只允许 SCREAM 类投票**；人声后 2.5 秒的"余温"也按人声处理；
- **fall/knock 统一按"撞击"计数**（门槛 0.55）：单次 ≥0.80 立刻报；
  否则 15 秒内第二次撞击（fall/knock 任意组合）才报；单次 0.55~0.80 只记时间；
- **连续 >6 秒**都过能量门槛的持续声才抑制撞击（短敲不被误伤）；
  单发误报由上面"两次确认"兜；
- 误报/漏报的取舍见 [TROUBLESHOOTING.md](TROUBLESHOOTING.md)。

## 4. 一次联网与对时

```
USB 原生口 ── RNDIS gadget（584e:5342）── PC 侧 enx… 192.168.137.1
板侧 eth0 192.168.137.2  ── NAT（ens33）── 互联网
```

- `sifli_ap.c` 开机 `usbdev_rndis_initialize()`；板侧 `netinit` 一次性配好 eth0；
- PC 侧运行 `~/rndis_net_up.sh`：绑定网卡、加 192.168.137.1/24、MASQUERADE；
- ⛔ 永远不要往 `/sys/bus/usb/drivers/rndis_host/new_id` 写 ID（dynid 会让
  `usbnet_probe` 静默 -ENODEV）；中招后 `rmmod/modprobe rndis_host`；
- 烧录/复位键属于 warm reset，会把 USB 数据通道卡死 → **拔电 5 秒冷启动**；
- 对时：`robot_ui/time_sync.c` 用 HTTPS `HEAD` 取 `Date:` 头，60 秒重试到成功，
  成功后 `clock_settime()` 并重算关怀任务。屏幕时钟加 UTC+8。

## 5. 内存与音频约束

- 半双工只有一台 `/dev/audio/audio0`：录和放必须串行；播放前停录、播完还麦；
- 录音 RX 一次 `read` 等 5 秒（私有看门狗），上层 8 秒停滞判据；
- TTS 每块 PCM 槽约 682KB（2MB 三等分）；火山响应缓冲 512KB；
- MiMo 响应缓冲 896KB → 分块上限 32 字；火山分块 64 字；
- 当前固件 flash ~42%、SRAM ~85%（以 `nuttx.bin` 实际构建为准）。

## 6. 关键不变量（改代码前先读）

1. **报警抢音频只许走 `audio_play_stop()`**。`audio_record_hold_end()` 内部的
   `audio_drop_play_device()` 判据是 `!playing` 而非 `play_exited`，会跨线程
   close 正在播放的 fd → 一报警整机卡死（2026-10-31 踩过）。
2. **录音只有一个读者**。驱动用 `rx_owner_gen` 保证：只有 owner 能清
   `rx_busy`，新读者一律让路，6.5 秒才抢占（2026-11-01 修）。
3. **不要在 LVGL 线程做阻塞动作**（HTTPS/TLS/大文件 IO/join）。跨线程改界面
   一律走 `ui_post_*` / `ui_async_call`。
4. **半双工播放期间麦是聋的**。报警确认窗必须用"响-停-听"循环，听窗够长。
5. **凭据只在 ROMFS 的 `/etc/assets/`**；`agent_config.json`/`push_key.txt`
   不进 Git（`.gitignore` 已挡）。

## 7. 相关文档

- [CONFIGURATION.md](CONFIGURATION.md) —— 密钥、后端、ROMFS 资产
- [BUILD_AND_FLASH.md](BUILD_AND_FLASH.md) —— 编译、ROMFS 同步、烧录、冷启动
- [TESTING.md](TESTING.md) —— 六项功能 + 报警 + TTS 的验收判据
- [TROUBLESHOOTING.md](TROUBLESHOOTING.md) —— 已知问题与串口指纹
- [CODE_TOUR.md](CODE_TOUR.md) —— 按功能找文件的阅读路线
