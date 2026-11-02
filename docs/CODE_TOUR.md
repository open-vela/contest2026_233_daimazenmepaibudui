# 代码导读

> 本项目的注释风格：**每个关键文件开头先讲取舍和踩过的坑，再看实现**。
> 第一次读代码建议按本文顺序；每一节都标了"先读哪段注释、再看哪个函数"。

## 1. 启动链路

| 顺序 | 文件 | 看什么 |
|---|---|---|
| 1 | `board/contest_board/src/sifli_ap.c` | `board_late_initialize()`：RNDIS gadget、`robot_ui`/`ai_agent`/`hello_app` 的拉起顺序与理由 |
| 2 | `app/robot_ui/main.c` | `main()`：LVGL、网络任务、报警回调、`voice_emerg_set_hook` |
| 3 | `app/hello_app/ai_companion_main.c` | `main()` 尾部：后端注册、TTS/ASR 选择、`[版本]` 标记 |
| 4 | `app/hello_app/ai_companion_main.c` | `state_beacon_task`：`[信标] up=… sm=…` 的来历 |

## 2. 语音对话

```
app/hello_app/ai_companion_main.c
├─ speech_capture_begin / speech_capture_complete   录音累积 + 快照
├─ dialogue_worker / process_ai_dialogue            ASR → 轻意图 → LLM/天气快路
├─ weather_fast_try                                 天气快速通道（2026-11-01）
├─ news_fast_try                                    新闻快速通道（新浪免 key）
├─ llm_complete_callback                            流式 TTS 生产者
├─ llm_speak_chunk_complete                         流式 TTS 消费者
├─ gate_voice_tick                                  门控人声 → 开麦
└─ round_watchdog_tick                              整轮看门狗（ASR/LLM/TTS 相位）
```

先读 `ai_companion_main.c` 文件头 + "流式 TTS"那一大段（约 4300 行起）：
它解释了队列深度 2/三槽、`record_hold`、8 秒空档还麦、90 秒放弃上限。

`app/hello_app/mimo_voice.c`：

- 文件头讲 MiMo ASR/TTS/chat 的接口与内存取舍；
- `mimo_tts_speak_head` —— **每一块都走它**（首块 20 字、后续 32/64 字）；
- `mimo_post` / `tts_synth_chunk` —— MiMo 请求与 TLS 复用；
- `weather_lookup` / `mimo_weather_lookup` —— Open-Meteo（天气快路的底层）；
- `chat_build_system_prompt` —— 常开麦那条的 system prompt。

## 3. 音频（半双工）

| 文件 | 先读 |
|---|---|
| `app/hello_app/ai_audio.c` | 文件头：半双工契约、`record_hold`、`audio_play_stop` 为什么是唯一安全收法 |
| `app/hello_app/ai_audio.h` | `audio_context_t` 字段、各开关的语义 |
| `board/contest_board/src/sf32lb52_audio.c` | 文件头"录音 RX 通路"；`sf32lb52_audio_read`；单读者守卫/`rx_owner_gen` |
| `board/contest_board/src/sf32lb52_audio_in.c` | 上层 read 循环、stop 空转、session 记账 |
| `app/hello_app/ai_sound_detect.c` | 门控线程 `sound_detect_bypass_start` 那一大段：能量/浊音/峰值判据、人声余温、持续声 veto |
| `app/hello_app/sound_event.c` | 文件头：mel 前端 + DSCNN-lite + 投票；`se_postprocess` 的阈值/不应期 |

**三条最容易踩的雷**（注释里都有）：

1. 报警抢设备只许 `audio_play_stop()`；
2. 录音单读者，`rx_owner_gen` 不是摆设；
3. 厂商 HAL 有对 post/看门狗无感的自旋，卡死时先抓 `dumpstack` 定 PC。

## 4. 报警

| 文件 | 看什么 |
|---|---|
| `app/hello_app/voice_emerg.c/.h` | 状态机 IDLE→CONFIRM→NOTIFIED→EXPIRED；响/听节奏；Bark 线程；`ve_seize_audio` |
| `app/hello_app/prompt_ack.c` | "嗯，我在"提示音（预缓存 + live TTS 回落） |
| `app/robot_ui/main.c` | `alarm_escalate` / `alarm_unify_hook` / `ui_post_panel` |
| `board/contest_board/src/sf32lb52_alarm.c` | 板级报警音（自己的 fd，辅助模式用） |
| `app/hello_app/ai_sound_detect.c` | 本地小模型命中 → `sound_event_cb` → `voice_emerg_trigger` |

## 5. 界面

| 文件 | 看什么 |
|---|---|
| `app/robot_ui/robot_ui.c` | 主屏、表情、时钟槽（`ui_clock_label_create`，UTC+8） |
| `app/robot_ui/touch_ui.c` | 菜单、设置、提醒、语音面板 |
| `app/robot_ui/ui_async.c` | `ui_async_call` 的线程纪律（为什么不能直接碰 LVGL） |
| `app/robot_ui/reminder_sched.c` | RTC 日循环闹钟；`/data` 是 tmpfs |

## 6. 网络 / 推送 / 对时

| 文件 | 看什么 |
|---|---|
| `app/robot_ui/network_comm.c` | MQTT、Bark/PushPlus、`push_send_alarm`；`push_key.txt` |
| `app/robot_ui/time_sync.c` | HTTPS `Date:` 头 → `clock_settime`；60 秒重试 |
| `packages/ai_agent/src/infra/vela_tls.c` | TLS、连接池、`vela_https_*`；Q1/Q2 的池注释 |
| `packages/ai_agent/src/llm/llm_proxy.c` | LLM 直连/代理、`-110` 兜底链 |
| `packages/ai_agent/src/voice/volc_asr.c` | 火山 v3 WebSocket、超时、方言 |
| `packages/ai_agent/src/voice/volc_tts.c` | 火山 TTS v3（`seed-tts-2.0`） |

## 7. 补丁与上游改动

- `patches/*-working-tree.patch` 三份是权威补丁（nuttx / vendor-sifli / ai_agent），
  从工作树 `git diff` 直接生成；
- 改上游树后运行 `patches/verify_upstream_trees.sh`（一致性/完备性/凭据扫描）；
- 不要再叠加 `patches/` 里的 per-fix 历史补丁（见 `patches/README.md` 顶部警告）。

## 8. 注释约定

- **先写"为什么"再写"怎么做"**：每个不显然的常量/分支都要能回答"不这样会怎样"；
- 真机证据写成 `日期 + 串口指纹 + 结论`，例如：
  ```
  2026-11-01 真机：TTS 块 140 字节 → 响应超 896KB 被截断 → 粘性降级火山 403 → 无声
  ```
- 放弃的方案也写（"不要在这里补 audio_record_hold_end"），避免后人再踩；
- 只写事实，不写"大概/可能"；不确定的标 TODO 并写判据。

## 9. 想改某功能，先看哪里

| 想改… | 先看 |
|---|---|
| 回复长度/语气 | `packages/ai_agent/src/core/context_builder.c`、`mimo_voice.c` 的 prompt |
| 天气城市/默认值 | `mimo_voice.c` 的 `MIMO_DEFAULT_CITY`、`ai_companion_main.c` 的 `g_known_cities` |
| TTS 音色/资源 | `agent_config.h` 的 `AGENT_DOUBAO_TTS_*`、`agent_config.json` 的 `volc_speaker` |
| 报警时长/节奏 | `voice_emerg.h` 的时间参数 |
| 误报/漏报 | `sound_event.c` 的 `g_threshold`/不应期、`ai_sound_detect.c` 的门控 |
| 屏幕文案 | `robot_ui.c` / `touch_ui.c` / `main.c` 的 `ui_post_*` |
| 网络/推送 | `network_comm.c`、`time_sync.c` |
