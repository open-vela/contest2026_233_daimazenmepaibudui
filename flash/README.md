# SF32LB52-DevKit-LCD openvela 烧录说明

> **关键结论**:SF32LB52 的 ROM 引导(SFBL)**必须读到 0x12000000 处的分区表(ftab)才会启动镜像**。只烧 `nuttx.bin@0x12010000` 而不烧 ftab 会导致板子完全静默(SFBL 不跳转)。官方文档"无需分区表"的说法在本板实测不成立。

## 烧录命令(一次性)

Windows:

```bat
flash_openvela.bat nuttx.bin
```

Linux:

```bash
./flash_openvela.sh nuttx.bin /dev/ttyUSB0
```

等价于 sftool 原生命令:

```bash
sftool -p COMx -c SF32LB52 -m nor --before default_reset --after soft_reset \
    write_flash "ftab.bin@0x12000000" "nuttx.bin@0x12010000"
```

| 文件 | 烧录地址 | 说明 |
|------|---------|------|
| `ftab.bin` | `0x12000000` | 分区表,启动必需(SFBL 靠它找到镜像) |
| `nuttx.bin` | `0x12010000` | openvela 镜像(平面 XIP,链接地址 0x12010000) |

## 验证

- 串口 **1000000 8N1**(不是 115200!)连 UART1(PA18/PA19,板上 CH343 = COMx)
- 复位后应看到:`SFBL` → `ABCD` → NuttX 启动日志 → `NuttShell (NSH)` / `nsh>`
- `ABCD` 是 NuttX 启动进度:`A` 数据段初始化完成 → `B` 缓存使能 → `C` HAL 初始化完成 → `D` 即将启动系统

## 文件来源

- `ftab.bin`:由 SiFli-SDK 构建生成(任何板级工程 `build_*/ftab/ftab.bin`),也可由 `ptab.json` 重新生成
- `ptab.json`:分区表源文件(SDK `customer/boards/sf32lb52-lcd_n16r8/ptab.json`)
- `nuttx.bin`:openvela 构建产物,见下方 openvela 构建步骤

## openvela 构建

```bash
# 需要 openvela 工作区(dev-ai-contest-2026 分支)
cmake -B cmake_out/sf32lb52_devkit_lcd -S "$PWD/nuttx" -GNinja \
  -DBOARD_CONFIG=../vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/configs/nsh \
  -DEXTRA_FLAGS="-Wno-cpp -Wno-deprecated-declarations"
cmake --build cmake_out/sf32lb52_devkit_lcd
# 产物:cmake_out/sf32lb52_devkit_lcd/nuttx.bin
```

## 工具要求

- `sftool`:建议 0.2.5(<https://github.com/OpenSiFli/sftool/releases>);SDK 自带的 0.1.16 也可用
- 串口终端:Windows 用 PowerShell/串口助手,Linux 用 `picocom -b 1000000 --noreset --lower-rts --lower-dtr /dev/ttyUSB0`(RTS 控制板子供电负载开关,普通 minicom/screen 可能把板子按在复位态)

## 厂商 GUI / 升级包(`pkg/`)

`pkg/` 是一个**可直接烧的升级包**(`Impeller.exe` 选这个目录即可),内部就是上面那两个文件加一份地址表:

| 文件 | 烧录地址 | 说明 |
|------|---------|------|
| `pkg/bootloader/bootloader.bin` | `0x12010000` | 即 `nuttx.bin`(本包 sha256 `c4400618dee58e25d87a28a32d50fa33610f5b490938f224e16a17acdfff21f8`) |
| `pkg/ftab/ftab.bin` | `0x12000000` | 与上面 `ftab.bin` 同一个文件 |
| `pkg/ImgBurnList.ini` | — | 给 GUI 读的地址表(`FILE0`/`FILE1` + `ADDR0`/`ADDR1`) |

步骤与判据见根目录 `README.md` 的「5. 烧录 → 5.1 另一条路:厂商 GUI(Impeller)」。三个要点:
**① 烧之前先断开占用 COM 口的串口工具;② 失败就硬断电 10 秒;③ 烧完拔插一次原生 USB。**
