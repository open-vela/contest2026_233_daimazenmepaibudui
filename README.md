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

### 1. 环境准备

```bash
# 安装依赖
sudo apt-get update
sudo apt-get install -y gcc-arm-none-eabi make

# 设置交叉编译工具链路径
export PATH=/path/to/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH
```

### 2. 配置工程

```bash
cd nuttx
./tools/configure.sh -l ../board/contest_board/configs/sf32lb52_ai
```

### 3. 编译固件

```bash
make -j$(nproc)
```

编译完成后生成：
- `nuttx` - ELF可执行文件
- `nuttx.bin` - 二进制固件（约706KB）

### 4. 烧录到开发板

使用SF32LB52专用烧录工具，将 `nuttx.bin` 烧录到 SF32LB52-DevKit-LCD 开发板。

### 5. 运行验证

- 开发板上电后自动启动AI陪伴系统
- 串口控制台可访问 NuttShell (NSH)
- 支持语音交互、触摸操作、LCD显示

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

---

**项目地址**: https://github.com/gaoxiaoying0207/contest2026_233_daimazenmepaibudui  
**开发者**: gaoxiaoying0207  
**开发板**: SF32LB52-DevKit-LCD  
**系统**: OpenVeLA (NuttX RTOS)
