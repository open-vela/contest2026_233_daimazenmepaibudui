# 智爱守护——基于 OpenVela 的多模态非接触式 AI 居家看护终端

## 一、作品简介

本作品面向独居和空巢老人，基于 OpenVela（NuttX）系统和 SF32LB52-DevKit-LCD 开发板，实现语音陪伴、异常声音检测、主动提醒、报警确认和远程通知。

当前版本以板载麦克风持续采集为基础：人声进入云端 ASR、大模型和 TTS 链路；非人声高能信号进入端侧声音事件模型。检测到疑似异常后，系统先询问老人，再根据屏幕、MQTT 或超时策略决定是否报警。

已实现并有代码或测试依据的能力包括：

- 16 kHz 单声道麦克风采集、VAD 和人声/非人声分流；
- 端侧声音事件识别，类别为 `other`、`fall`、`knock`、`scream`；
- 云端 ASR、LLM 对话和 TTS 播放链路；
- 检测、响铃、询问、二次确认、报警和手机推送闭环；
- RTC 定时提醒和断网时的本地响铃；
- LVGL 图形界面、触摸/按键输入和屏幕镜像调试；
- USB RNDIS 联网、MQTT 通信和板级音频、显示、RTC、按键适配；
- 音频 DMA/RX 异常恢复、录音读停滞诊断和手动接管路径。

本板不带摄像头和 IMU，因此视觉识别、毫米波雷达检测和基于 IMU 的姿态跌倒检测不属于当前版本功能。当前的跌倒相关能力指端侧的跌倒撞击声识别。

## 二、选题方向

AI 硬件产品创新。

项目基于 OpenVela 和 `ai_agent`，将端侧声音感知、云端语音交互、主动提醒和报警联动组合成面向老年人的嵌入式 AI Agent 终端。

## 三、目录结构

```text
contest2026_233_daimazenmepaibudui/
├── .github/                               # GitHub 配置和工作流
├── app/                                   # 作品应用代码
│   ├── hello_app/                         # AI 陪伴、语音链路和端侧声音检测
│   ├── zhi_ai/                            # 智爱业务入口
│   ├── audio_test/                        # 音频录制、播放和循环测试命令
│   ├── drvtest/                           # 音频录放和恢复测试命令
│   └── hw_test/                           # 显示、按键、RTC、音频等硬件自检
├── board/contest_board/                   # SF32LB52-DevKit-LCD 板级适配
├── docs/                                  # 接口、驱动、报警和测试文档
├── flash/                                 # sftool/Impeller 烧录材料
├── logs/                                  # AI Coding 日志
├── model_output/                          # 训练侧模型和验证产物
├── patches/                               # vendor、NuttX、ai_agent 补丁
├── quickapp/hello_quickapp/               # 快应用示例目录（无用）
├── src/                                   # SiFli SDK 工程入口
├── tools/                                 # 音频训练、对拍、镜像和 PC 诊断工具
├── README.md                              # 本说明
├── contest2026_233_daimazenmepaibudui.xml # 比赛仓库 manifest
└── openvela.xml                           # OpenVela 工程清单
```   

`nuttx/`、`apps/`、`packages/`、`vendor/` 和 `prebuilts/` 属于通过 repo 拉取的 OpenVela 工作区，不是本作品仓库需要重复提交的目录。

## 四、运行方式

### 1. 获取完整工程

```bash
repo init -u https://github.com/gaoxiaoying0207/contest2026_233_daimazenmepaibudui \
  -b dev-ai-contest-2026 \
  -m contest2026_233_daimazenmepaibudui.xml
repo sync -c -j8
```

本仓库只在自己的目录中开发。manifest 会把 `app/` 和 `board/contest_board/` 映射到 OpenVela 编译树，不要手动复制文件。

### 2. 应用补丁和板级软链

补丁应用顺序、目标目录和作用见 `patches/README.md`。应用前可执行：

```bash
git apply --check <patch-file>
```

板级目录需要逐项软链：

```bash
bash board/contest_board/scripts/link_board_impl.sh
```

### 3. 配置密钥

编译前，将本机密钥文件放到 `board/contest_board/src/etc/assets/`：

- `agent_config.json`：大模型、ASR、TTS 和可选 MQTT 配置；
- `push_key.txt`：Bark 手机推送密钥。

密钥不提交到 Git。修改 `src/etc/assets/` 后，需要删除构建目录中的 ROMFS 产物再重新编译，否则新文件可能不会进入固件。

### 4. 配置和编译

```bash
cd <工作区>
export PATH=<工作区>/prebuilts/tools/python/bin:<工作区>/prebuilts/tools/linux/x86_64:$PATH
export PYTHONPATH=<工作区>/prebuilts/tools/python/dist-packages/kconfiglib

cmake -S <工作区>/nuttx \
  -B <工作区>/cmake_out/contest2026_233_board_sf32lb52_ai \
  -GNinja \
  -DBOARD_CONFIG=../vendor/openvela/boards/contest2026_233_board/configs/sf32lb52_ai \
  -DEXTRA_FLAGS="-Wno-cpp -Wno-deprecated-declarations"

cmake --build <工作区>/cmake_out/contest2026_233_board_sf32lb52_ai
```

成功后应生成：

```text
cmake_out/contest2026_233_board_sf32lb52_ai/nuttx.bin
```

### 5. 烧录

使用 `sftool` 时必须同时写入分区表和固件：

```bash
sftool -p <COM口> -c SF32LB52 -m nor \
  --before default_reset --after soft_reset write_flash \
  "flash/ftab.bin@0x12000000" \
  "cmake_out/contest2026_233_board_sf32lb52_ai/nuttx.bin@0x12010000"
```

也可以使用 `flash/pkg/` 中的 Impeller 升级包。烧录完成后，先等待板子启动，再重新插拔原生 USB，使 RNDIS 重新枚举。

### 6. 上电顺序

1. 先连接 CH343 UART；
2. 板子上电；
3. 等待串口出现 `NuttShell (NSH)` 或 `nsh>`；
4. 最后连接原生 USB RNDIS；
5. 在 PC 侧配置 `192.168.137.1` 和 NAT。

串口参数为 `1000000 8N1`。不要在原生 USB 已连接时按复位键，否则 Windows 可能将 RNDIS 设备显示为 Code 10。

## 五、主要功能验证

| 功能 | 验证方式 | 预期结果 |
|---|---|---|
| 音频输入 | `hw_test audio 2` | 看到 peak/avg，讲话时判断为有声音 |
| 音频播放 | `audio_test 1000 1000` | 喇叭播放 1 kHz 音调 |
| 网络 | `net_test` | 网卡、DNS、TCP、MQTT 逐级通过 |
| ASR | `hw_test asr /etc/assets/test_16k.wav` | 输出识别文本 |
| TTS | `hw_test tts "你好，今天天气不错"` | 输出合成字节数并播放 |
| 语音闭环 | 对着板子讲话 | VAD → ASR → LLM → TTS |
| 报警确认 | `hw_test alarm 3 3` | 报警页、警音和 MQTT 上报 |
| RTC | `hw_test rtc 3` | 读回 20xx 年并收到提醒信号 |
| 屏幕镜像 | `py -3.10 tools/lcd_mirror/lcd_mirror.py --scale 1.5` | PC 显示板端画面并可注入触摸 |
| 音频恢复 | 连续录音并观察串口 | 异常时出现 RX 恢复日志，录音继续 |

端侧模型的训练、导出和逐窗对拍工具位于 `tools/audio_event/`。当前固件运行的是 `sound_event.c + sound_event_model.h` 这条 C 侧 DSCNN-lite 路线；`model_output/` 中的 PyTorch/ONNX 文件属于训练和复现实验材料，不应直接写成板端已经运行的模型。

## 六、音频异常恢复说明

麦克风由 `hello_app` 常驻录音线程负责采集，`robot_ui` 不再接管或归还麦克风。系统记录最近一次 `read` 结果、录音等待时间、空读次数以及 DMA/RX 计数。

当检测到持续读停滞时，系统执行有界停止、设备层 RX/DMA 恢复和录音线程重新启动；播放失败也会执行完成回调，避免界面长期停留在“正在播放”或“正在说话”。该机制提供可观测和自愈路径，但长期稳定性仍以真机日志为准。

## 七、AI Coding 使用说明

项目使用 AI 工具辅助需求拆解、OpenVela/NuttX 适配、音频 DMA 调试、模型前端对拍、编译排错和文档整理。对话日志按成员和日期保存在 `logs/` 目录，并随最终提交材料提交。

## 八、已知限制

- 本板不带摄像头、IMU 和独立 Wi-Fi，因此当前不提供视觉识别、姿态跌倒检测和独立 Wi-Fi 上网；
- 唤醒词模板默认不随固件提供，当前主要通过 VAD 常听或界面入口交互；
- `/data` 为 tmpfs，运行时提醒、设置和录音唤醒词重启后会丢失；
- 板端联网依赖 USB RNDIS 和 PC 侧 NAT；
- 公共 MQTT broker 可能限流，建议测试时配置自己的 broker；
- 中文字库覆盖 GB2312 常用范围，少量 GBK 生僻字可能显示为方块；
- TTS 单次文本长度有限，过长文本会被截断；
- 音频恢复依赖板级 DMA 和厂商 HAL，仍需更多长时间运行数据；
- `audio_test audfix` 属于诊断期手动恢复入口，不应作为普通用户操作流程；
- 模型弱势类别训练数据仍需补充，当前准确率、召回率和误报率应以仓库中的模型说明及验证日志为准。
## 九、项目地址

- 仓库：https://github.com/open-vela/contest2026_233_daimazenmepaibudui
- 开发板：SF32LB52-DevKit-LCD
- 系统：OpenVela（NuttX RTOS）
- 团队：代码怎么跑不队
