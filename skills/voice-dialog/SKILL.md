---
name: voice-dialog
description: 语音对话全链路（VAD → ASR → LLM → TTS），含快路与超时回落
---

# 语音对话 Skill

## 功能

老人开口说话即可对话，无需唤醒词。链路：VAD → ASR → 轻意图/快路 → LLM → TTS。

## 数据流

```
麦克风 ──► 常开录音线程 ──► VAD ──► speech_capture_begin/complete
                                              │
                        ┌─────────────────────┘
                        ▼
              dialogue_worker（识别线程）
                        │  voice_asr_recognize()  ← 火山 v3（方言）
                        │  （失败/为空回落 MiMo）
                        ▼
        轻意图：灯控 / 急救词 /「没事」/ 快路
                        │
        ┌───────────────┼───────────────┐
        ▼               ▼               ▼
    短句快路        天气/新闻/自介       大模型
   （秒回）        快路（15~20秒）    velaclaw → 直连
                        │
                        ▼
           llm_complete_callback：逐块 TTS
                        │
                        ▼
           audio_play_start ──► 喇叭
```

## 快路（免 LLM）

| 快路 | 触发词 | 延迟 |
|---|---|---|
| 短句 | 你好/谢谢/再见/好的/晚安 | <1 秒 |
| 自我介绍 | 你能做什么/你是谁 | <1 秒 |
| 天气 | 天气/气温/下雨/冷不冷 | ~15 秒 |
| 新闻 | 新闻/头条/时事 | ~15 秒 |
| 新闻概括 | 详细说说第X条 | ~15 秒 |

## 延迟构成

| 环节 | 耗时 | 可否优化 |
|---|---|---|
| ASR（火山云端） | 7~8 秒 | 否（云端推理） |
| LLM（云端推理） | 8~10 秒 | 否（云端推理） |
| TTS 首块（10 字） | 2~3 秒 | 已从 20 字降到 10 字 |

## 相关代码

- `app/hello_app/ai_companion_main.c` —— 主循环、VAD→ASR→LLM→TTS、各快路
- `app/hello_app/mimo_voice.c` —— MiMo/火山后端、分块与 HTTP
- `app/hello_app/ai_audio.c` —— 半双工音频
- `board/contest_board/src/sf32lb52_audio.c` —— 音频驱动

## 关键常量和调参

| 常量 | 位置 | 值 | 说明 |
|---|---|---|---|
| `AUDIO_VAD_MIN_SPEECH_MS` | ai_audio.h | 300 | VAD 最短语音 |
| `MIMO_TTS_HEAD_CHUNK_CHARS` | mimo_voice.h | 10 | TTS 首块字数 |
| `MIMO_TTS_CHUNK_CHARS` | mimo_voice.h | 32 | TTS 后续块字数 |
| `SPEECH_MIN_FRAMES_FOR_ASR` | ai_companion_main.c | 1600 | 最短送 ASR 长度 |
