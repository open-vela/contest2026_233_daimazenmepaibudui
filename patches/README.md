# vendor_sifli 补丁

两个补丁按顺序应用(`git apply` 相对 `<openvela 工作区>/vendor/sifli` 目录):
先 `vendor_sifli-boot-fixes.patch`,再 `vendor_sifli-audio-driver.patch`。

## vendor_sifli-boot-fixes.patch

让 openvela 在 SF32LB52-DevKit-LCD 上可编译/可启动的 5 处上游修复。

### 修复内容

1. `chips/sf32lb52/sifli_uart.c` — `void up_putc()` 中删除非法的 `return ch;`
2. `chips/sf32lb52/sifli_irq.c` — 补充 `arm_lowprintf` 的 extern 声明(定义在 sifli_start.c)
3. `chips/sf32lb52/sf32lb_flash.c` — `HAL_FLASH_CONFIG_FULL_AHB_READ` → `HAL_FLASH_CONFIG_AHB_READ`(头文件中正确的函数名)
4. `boards/.../sf32lb52_devkit_lcd/src/sifli_ap.c` — 补 `sifli_i2cbus_initialize` extern 声明 + `<nuttx/i2c/i2c_master.h>`
5. `boards/.../sf32lb52_devkit_lcd/src/sifli_ap.c` — 删除错误的 `HAL_PIN_Set(PAD_PA37, I2C1_SCL, ...)`(DevKit-LCD 触摸 I2C1 SCL 应为 PA30,PA37 是 LCD 数据线;正确引脚已在 bsp_pinmux.c 配置)

## vendor_sifli-audio-driver.patch

SF32LB52-DevKit-LCD 音频驱动,注册 `/dev/audio0`(NuttX audio_lowerhalf),支持播放与录音。

### 内容

- 新增 `boards/.../sf32lb52_devkit_lcd/src/sf32lb52_audio.c` / `.h` — audio_lowerhalf 实现
  (codec 模拟通路 AUDCODEC + 数字通路 AUDPRC TX0/RX0 DMA + AW8155 功放 GPIO)
- `boards/.../src/CMakeLists.txt` — 加入音频源文件
- `boards/.../src/sifli_ap.c` — bringup 中调用 `sf32lb52_audio_initialize()`(`CONFIG_AUDIO`)

### 验证

- 板级配置: `CONFIG_AUDIO=y` 等(见 `board/contest_board/configs/sf32lb52_ai/defconfig`)
- 播放: `audio_test 3000 1000`(1 kHz 3 秒);录音: `audio_test record 3000`
- 此补丁基于 boot 补丁已应用的状态生成,顺序不可颠倒

## 应用方式

```bash
cd <openvela 工作区>/vendor/sifli
git apply patches/vendor_sifli-boot-fixes.patch
git apply patches/vendor_sifli-audio-driver.patch
```

## 说明

- 补丁修复/功能均为上游 vendor_sifli 缺口(开启 I2C/DEBUG 等配置后编译必现),建议以团队名义向 [open-vela/vendor_sifli](https://github.com/open-vela/vendor_sifli) 提交 PR
- 工作区中已直接应用了这些修复(未提交),补丁用于留存/提交/队友复现
