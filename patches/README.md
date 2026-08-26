# vendor_sifli 补丁

`vendor_sifli-boot-fixes.patch` — 让 openvela 在 SF32LB52-DevKit-LCD 上可编译/可启动的 5 处上游修复。

## 修复内容

1. `chips/sf32lb52/sifli_uart.c` — `void up_putc()` 中删除非法的 `return ch;`
2. `chips/sf32lb52/sifli_irq.c` — 补充 `arm_lowprintf` 的 extern 声明(定义在 sifli_start.c)
3. `chips/sf32lb52/sf32lb_flash.c` — `HAL_FLASH_CONFIG_FULL_AHB_READ` → `HAL_FLASH_CONFIG_AHB_READ`(头文件中正确的函数名)
4. `boards/.../sf32lb52_devkit_lcd/src/sifli_ap.c` — 补 `sifli_i2cbus_initialize` extern 声明 + `<nuttx/i2c/i2c_master.h>`
5. `boards/.../sf32lb52_devkit_lcd/src/sifli_ap.c` — 删除错误的 `HAL_PIN_Set(PAD_PA37, I2C1_SCL, ...)`(DevKit-LCD 触摸 I2C1 SCL 应为 PA30,PA37 是 LCD 数据线;正确引脚已在 bsp_pinmux.c 配置)

## 应用方式

```bash
cd <openvela 工作区>/vendor/sifli
git apply patches/vendor_sifli-boot-fixes.patch
```

## 说明

- 这些是上游 vendor_sifli 的 bug(开启 I2C/DEBUG 等配置后编译必现),建议以团队名义向 [open-vela/vendor_sifli](https://github.com/open-vela/vendor_sifli) 提交 PR
- 工作区中已直接应用了这些修复(未提交),本补丁用于留存/提交
