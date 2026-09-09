# hello_app - AI陪伴系统核心模块

## 模块简介

AI陪伴系统的核心应用模块，实现多模态交互和智能看护功能。

## 目录结构

```
hello_app/
├── ai_companion_main.c    # 主程序入口，系统初始化
├── ai_llm.c/h            # 大语言模型接口，对接Xiaomi MiMo
├── ai_audio.c/h          # 音频处理，语音识别与合成
├── ai_care.c/h           # 智爱守护核心逻辑
├── ai_sound_detect.c/h   # 声音检测，异常声音识别
├── ai_state_machine.c/h  # 状态机管理，行为逻辑控制
├── Makefile              # NuttX构建配置
├── Kconfig               # 内核配置选项
└── CMakeLists.txt        # CMake构建配置
```

## 核心功能

### 1. AI语音交互
- 语音唤醒词检测
- 语音识别（ASR）
- 语音合成（TTS）
- 大语言模型对话

### 2. 智能看护
- 跌倒检测与报警
- 异常行为识别
- 睡眠质量监测
- 用药提醒

### 3. 情感计算
- 语音情感分析
- 情绪状态识别
- 个性化陪伴策略

## 编译配置

在NuttX配置中启用：
```
CONFIG_HELLO_APP=y
CONFIG_AI_COMPANION=y
CONFIG_AI_AUDIO=y
CONFIG_AI_LLM=y
```

## 依赖模块

- `nuttx/drivers/audio/` - 音频驱动
- `nuttx/drivers/sensors/` - 传感器驱动
- `vendor/sifli/` - SF32LB52 HAL库
