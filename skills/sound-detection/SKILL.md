---
name: sound-detection
description: 检测老人居家环境中的异常声音并触发安全提醒
---

# 异常声音检测 Skill

## 功能

检测呼救声、异常喊叫、跌倒撞击声和敲击求救声。

## 输入

- 采样率：16 kHz
- 声道：单声道
- 格式：16 位 PCM 音频
- 来源：板载麦克风

## 处理流程

1. 持续采集麦克风数据。
2. 判断是否存在有效声音。
3. 识别异常声音类别。
4. 融合连续检测结果。
5. 触发界面提示、警报和消息上报。

## 相关代码

- `app/hello_app/ai_audio.c`
- `app/hello_app/ai_sound_detect.c`
- `app/robot_ui/sound_classifier.c`
- `app/robot_ui/sound_fusion.c`

## 输出

返回声音类别、分类置信度、报警结果和音频设备状态。

## 使用限制

该 Skill 依赖板载麦克风和声音分类模型，检测结果不能替代人工确认。
