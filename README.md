# 智爱陪伴——基于 OpenVeLA 的多模态 AI 居家看护终端

> 2026 OpenVeLA 开发者大赛 · 新硬件适配赛道
> 硬件平台:SF32LB52-DevKit-LCD(思澈科技,1.85" 390×450 CO5300 AMOLED + FT6146 触摸)

## 一、作品简介

面向独居及高龄老人,依托 SF32LB52-DevKit-LCD 运行 OpenVeLA 系统打造的智能居家安全看护终端。通过语音交互、异常声音检测、主动关怀提醒,联动大屏与米家生态,解决老人突发意外无反馈的居家痛点。

## 二、选题方向

**AI 硬件产品创新 · 新硬件适配赛道**——将 OpenVeLA 首次适配到 SF32LB52 平台(SF32LB52-DevKit-LCD),完成系统移植、驱动适配与硬件能力验证,并在此基础上开发 AI 陪伴应用。

## 三、目录结构

```
├── app/
│   ├── hello_app/        # AI 陪伴应用(状态机/语音/异常检测/LLM/关怀)
│   ├── robot_ui/         # LVGL 机器人界面(表情/状态/AI 回复显示)
│   └── zhi_ai/           # 早期应用骨架
├── board/
│   ├── contest_board/    # openvela 板级适配(映射到 vendor/openvela/boards)
│   └── sf32lb52-lcd_n16r8/  # SDK 板级配置参考(SDK 官方副本)
├── flash/                # openvela 烧录方案(ftab + 脚本 + 说明)
├── patches/              # vendor_sifli 上游修复补丁
├── quickapp/             # quickapp 示例
├── logs/                 # AI Coding 日志(官方 mimo 生成)
└── *.xml                 # repo manifest(openvela 工程清单)
```

## 四、运行方式

### 4.1 openvela 系统(大赛主线,已验证可启动)

**环境准备**:openvela 工作区(`dev-ai-contest-2026` 分支),含 vendor_sifli 板级支持;`repo sync` 后需确认 manifest 链接存在(`packages/demos/contest2026_233_hello_app`、`contest2026_233_robot_ui`、`vendor/openvela/boards/contest2026_233_board`)。

**应用 vendor 补丁**(openvela 工作区同步后必须,按顺序):

```bash
cd <openvela 工作区>/vendor/sifli
git apply <本仓库>/patches/vendor_sifli-boot-fixes.patch       # 上游 5 处编译/启动 bug
git apply <本仓库>/patches/vendor_sifli-audio-driver.patch      # /dev/audio0 音频驱动(依赖前者)
```

> `patches/vendor_sifli-boot-fixes.patch`: 上游 vendor_sifli 的编译/启动 bug 修复,详见 `patches/README.md`(建议提交上游 PR)。
> `patches/vendor_sifli-audio-driver.patch`: SF32LB52-DevKit-LCD 音频驱动(播放+录音,DAC/ADC+AUDPRC+DMA),注册 `/dev/audio0`。

> app 命令(`robot_ui`/`hello_app`/`audio_test`/`zhi_ai`)由 CMake 配置时自动注册,无需手动改 builtin 表;若 nsh 下 `command not found`,删除 `cmake_out/<board>/.config` 后重新 `cmake -B` 即可。

**编译**:

```bash
cmake -B cmake_out/contest2026_233_board_sf32lb52_ai -S "$PWD/nuttx" -GNinja \
  -DBOARD_CONFIG=../vendor/openvela/boards/contest2026_233_board/configs/sf32lb52_ai
cmake --build cmake_out/contest2026_233_board_sf32lb52_ai
# 产物:cmake_out/contest2026_233_board_sf32lb52_ai/nuttx.bin
```

> 构建注意:需要把 `prebuilts/tools/python/bin`(kconfig 工具)、`prebuilts/tools/linux/x86_64`(genromfs)加入 PATH;修改 defconfig 后需删除 `cmake_out/.../.config` 重新生成。

**烧录(关键)**:

SF32LB52 的 ROM 引导(SFBL)**必须读到 0x12000000 处的分区表(ftab)才会启动镜像**——只烧 `nuttx.bin@0x12010000` 而不烧 ftab 会导致板子完全静默。使用仓库内脚本:

```bash
cd flash
./flash_openvela.sh nuttx.bin            # Linux
flash_openvela.bat nuttx.bin             # Windows
```

等价命令:

```bash
sftool -p COMx -c SF32LB52 -m nor --before default_reset --after soft_reset \
    write_flash "ftab.bin@0x12000000" "nuttx.bin@0x12010000"
```

> `sftool` 建议 0.2.5(<https://github.com/OpenSiFli/sftool/releases>);`ftab.bin` 由 SDK 构建生成(来源见 `flash/README.md`)。

**验证**:串口 1000000 8N1 连接 UART1(板上 CH343),复位后应看到:

```
SFBL → ABCD → ADC init → NOR MTD registered → NuttShell (NSH) → nsh>
```

设备节点验证:`ls /dev` 应含 `fb0 lcd0 input0 i2c0 i2c1 ttyACM0 ttyS0 adc0 buttons rtc0 watchdog0 timer0`;`free` 应显示约 8.7 MB 内存(PSRAM)。

### 4.2 SDK 硬件自测固件(SDK/RT-Thread 路线)

用于硬件独立验证(屏幕/触摸/LED/串口),不依赖 openvela:

```bash
cd board
. $SIFLI_SDK/export.sh        # 设置 SDK 环境(SiFli-SDK release/v2.4)
scons --board=sf32lb52-lcd_n16r8 -j8
# 烧录:build_sf32lb52-lcd_n16r8_hcpu/flash.bat(Windows,输 COM 口号)
```

固件功能:AMOLED 彩条/灰度/纯色循环、FT6146 触摸坐标打印、LED 闪烁、1M 波特率控制台。

## 五、当前进度与已知问题

| 模块 | 状态 |
|------|------|
| openvela 板级适配 + 启动 | ✅ 真机验证通过(NSH 控制台、外设全注册) |
| 硬件自测(SDK 路线) | ✅ 屏幕/触摸/串口/LED 全通 |
| 上游修复 | ✅ 5 处 vendor_sifli bug(见 `patches/`),建议提交上游 |
| hello_app(AI 应用) | 🔧 已接入构建;待修:`ai_llm.c` openssl→mbedtls、3 个未实现 simulate_* 函数 |
| robot_ui(LVGL 界面) | 🔧 已接入构建;待修:中文标识符、LVGL 字体配置、lv_msgbox_create API |
| 快速烧录方案 | ✅ `flash/` 目录(ftab 启动关键,已文档化) |

## 六、AI Coding 使用说明

本作品深度借助 AI Coding 辅助开发:AI 协助完成环境搭建、板级适配调试(定位 ftab 启动问题、修复上游编译 bug)、硬件验证脚本与烧录方案文档化;团队成员通过 AI 生成应用代码并在此基础上迭代。完整对话日志见 `logs/` 目录。
