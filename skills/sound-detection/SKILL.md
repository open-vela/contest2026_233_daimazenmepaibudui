---
name: sound-detection
description: 检测老人居家环境中的异常声音（摔倒/敲击/尖叫）并触发安全提醒
---

# 异常声音检测 Skill

## 功能

检测摔倒撞击声、敲击求救声、异常喊叫声，触发本地报警与家属通知。

## 输入

- 采样率：16 kHz
- 声道：单声道
- 格式：16 位 PCM（s16le）
- 来源：板载麦克风

## 处理流程

```
麦克风 ──► 门控（能量/峰值/浊音/ZCR/自相关）──► 本地小模型（DSCNN-lite）
                                                        │
                          ┌─────────────────────────────┘
                          ▼
                 3/4 投票 + 每类阈值 + 8 秒不应期
                          │
                          ▼
              fall/knock（撞击）──► 15 秒内两次 或 单次 ≥0.80
              scream ─────────────► 单次 ≥0.55
                          │
                          ▼
                 报警（响铃 → 10 秒确认 → Bark）
```

## 关键判据

| 类别 | 阈值 | 触发条件 |
|---|---|---|
| fall（摔倒） | 0.55 | 15 秒内两次撞击 或 单次 ≥0.80 |
| knock（敲击） | 0.55 | 15 秒内两次撞击 或 单次 ≥0.80 |
| scream（尖叫） | 0.55 | 单次 ≥0.55 |

- **人声窗只允许 SCREAM 类投票**（避免说话被误判成敲击/摔倒）
- **人声后 2.5 秒的"余温"也只允许 SCREAM**
- **连续 >6 秒的持续声不报撞击**（避免风扇/摩擦误报）

## 模型

- 架构：DSCNN-lite（mel 前端 40 维 + 卷积 + 全连接）
- 输入：64 帧 mel（0.64 秒窗口），滑窗步长 20 帧（200ms）
- 权重：`sound_event_model.h`（训练侧脚本导出）
- 类别：0=other, 1=fall, 2=knock, 3=scream

## 相关代码

- `app/hello_app/ai_sound_detect.c` —— 门控 + 小模型旁路
- `app/hello_app/sound_event.c` —— 本地声音分类（mel + DSCNN-lite + 投票）
- `app/hello_app/sound_event.h` —— 接口与类别定义
- `app/hello_app/voice_emerg.c` —— 命中后的报警状态机

## 调参入口

- 阈值：`sound_event.c` 的 `g_threshold[SE_CLASS]`
- 不应期：`SE_REFRACT_WIN`（scream 8s）/ `SE_REFRACT_WIN_IMPACT`（撞击 2s）
- 投票：`SE_VOTE_N`（4）/ `SE_VOTE_NEED`（3）
- 门控能量门槛：`SOUND_GATE_ANOMALY_RMS` / `SOUND_GATE_ANOMALY_PEAK`
