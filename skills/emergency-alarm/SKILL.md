---
name: emergency-alarm
description: 语音/本地声音触发的急救报警（响铃 → 10 秒确认 → Bark → 3 分钟自停）
---

# 急救报警 Skill

## 功能

老人喊「救命」或检测到摔倒/敲击时，响铃报警；10 秒内说「没事」或点按钮可取消，
否则自动通知家属（Bark 推送），持续响铃 3 分钟。

## 状态机

```
IDLE ──trigger()──► CONFIRM（响 1.2s / 听 3.6s 循环）
                       ├─ 说「没事」/点按钮 → CANCELLED（停铃，不通知）
                       ├─ 10 秒没人应 → NOTIFIED（发 Bark，继续响）
                       │                  └─ 每 30 秒重发
                       │                  └─ 期间说「没事」仍可停
                       └─ 180 秒到点 → EXPIRED（自动停）
```

## 触发来源

| 来源 | 入口 | 说明 |
|---|---|---|
| 语音关键词 | `emerg_classify()` → `voice_emerg_trigger("voice_sos")` | 「救命/报警/着火」+ 20+ 变体 |
| 本地声音 | `sound_event_cb()` → `voice_emerg_trigger()` | fall/knock 撞击两次 |
| 屏幕大红按钮 | `emergency_call_handler()` → `alarm_escalate()` | 同时触发板级报警 |
| MQTT | `start_alarm` 命令 | 手机远程触发 |

## 关键常量

| 常量 | 值 | 说明 |
|---|---|---|
| `VOICE_EMERG_CONFIRM_MS` | 10000 | 10 秒确认窗 |
| `VOICE_EMERG_RING_MS` | 1200 | 每轮响 |
| `VOICE_EMERG_LISTEN_CONFIRM_MS` | 3600 | 确认期听 |
| `VOICE_EMERG_LISTEN_MS` | 2800 | 通知后听 |
| `VOICE_EMERG_RENOTIFY_MS` | 30000 | Bark 重发 |
| `VOICE_EMERG_AUTOSTOP_MS` | 180000 | 3 分钟自停 |

## 「没事」识别

ASR 会把「没事」听成各种变体（冤枉/好是又调/收到几个呀）。修法：

1. **短句一律当「没事」**（≤15 字，且不是急救词）
2. 长句（>15 字）判为噪音忽略

## 相关代码

- `app/hello_app/voice_emerg.c` —— 状态机、响铃、Bark 线程
- `app/hello_app/ai_companion_main.c` —— 急救词匹配、主循环 180 秒兜底
- `app/robot_ui/main.c` —— 报警页、按钮、询问页
- `board/contest_board/src/sf32lb52_alarm.c` —— 板级报警音

## 安全约束

- ⛔ 报警抢音频**只走 `audio_play_stop()`**（`audio_record_hold_end` 会踩 fd）
- ⛔ 跨线程调 UI 一律走 `ui_post_*`
- ⛔ 报警线程里不调 `audio_record_hold_end`
