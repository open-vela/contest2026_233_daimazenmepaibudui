# 编译、ROMFS 与烧录

> 目标：从干净工作区到板子上跑起来。按顺序做，每一步都有判据。
> 本文命令以本机工作区 `/home/xiaoyingyu/openvela` 为例，换机器只改路径。

## 1. 工作区与补丁

工作区由 `repo` 管理，本仓库只负责 `app/` 和 `board/contest_board/`。
三棵上游树（`nuttx`、`vendor/sifli`、`packages/ai_agent`）的改动以
`patches/*-working-tree.patch` 为准：

```bash
cd <工作区>
# 只打这三份，别再叠加 per-fix 补丁（会冲突）
git -C nuttx apply <仓库>/patches/nuttx-working-tree.patch
git -C vendor/sifli apply <仓库>/patches/vendor-sifli-working-tree.patch
git -C packages/ai_agent apply <仓库>/patches/apps-ai-agent-working-tree.patch
```

改完上游树、上传前跑：

```bash
bash <仓库>/patches/verify_upstream_trees.sh
```

三条判据全绿才提交：补丁↔工作树逐字对应、改动文件齐全、没有凭据形状。

## 2. 配置与编译

```bash
export PATH=<工作区>/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:\
<工作区>/prebuilts/build-tools/linux-x86_64/bin:$PATH

cmake --build <工作区>/cmake_out/contest2026_233_board_sf32lb52_ai -j$(nproc)
```

判据：退出码 0，且生成

```
cmake_out/contest2026_233_board_sf32lb52_ai/nuttx.bin
```

> 第一次配置（`cmake -S ... -B ...`）用的是 repo manifest 里的
> `boards/contest2026_233_board/configs/sf32lb52_ai`；已经有 `cmake_out/` 时
> 日常只需要 `cmake --build`，**不用重新 configure**。

## 3. ROMFS 与密钥同步（必看）

构建目录里的 ROMFS 是**从 `vendor/sifli/.../src/etc` 拷到暂存目录再 genromfs**
的。改了 `board/contest_board/src/etc/assets/`（例如 `agent_config.json`、
`push_key.txt`）后，普通 `cmake --build` **不会**把新文件装进去。

正确步骤：

```bash
SRC=<仓库>/board/contest_board/src/etc
DST=<工作区>/cmake_out/contest2026_233_board_sf32lb52_ai/boards/exclude_board/src/romfs_etc
cp -a "$SRC/." "$DST/"

# 强制重建 ROMFS（否则 romfs.img 的时间戳可能让它跳过）
rm -f <工作区>/cmake_out/contest2026_233_board_sf32lb52_ai/boards/exclude_board/src/romfs.img \
      <工作区>/cmake_out/contest2026_233_board_sf32lb52_ai/boards/exclude_board/src/romfs_etc.c

cmake --build <工作区>/cmake_out/contest2026_233_board_sf32lb52_ai -j$(nproc)
```

判据（在 `cmake_out/.../` 下）：

```bash
python3 -c "import json;print(json.load(open('boards/exclude_board/src/romfs_etc/assets/agent_config.json'))['volc_speaker'])"
# 应打印你配置的音色
grep -acF '<你的资源/音色/标记串>' nuttx.bin   # 非 0
grep -acF 'push_key.txt' boards/exclude_board/src/romfs.img   # 非 0
```

## 4. 烧录（必须手动）

先停掉串口记录（**Ctrl+C，不要 Ctrl+Z**；挂起的 sftool 会占串口），确认没有
T 状态的 `sftool`：

```bash
ps aux | grep sftool | grep -v grep
```

烧录命令（同时写分区表 + 固件）：

```bash
sftool -c SF32LB52 -p /dev/ttyACM0 -b 1000000 write_flash \
  <仓库>/flash/ftab.bin@0x12000000 \
  <工作区>/cmake_out/contest2026_233_board_sf32lb52_ai/nuttx.bin@0x12010000
```

- 粘贴长命令后**先核对再回车**（终端可能掉字符）；
- `Failed to download stub` = 错过约 2 秒窗口 → 断电 10 秒重试；
- 还不行加 `--compat true`；再不行 `--before no_reset`；
- 烧完**拔电 5 秒再插回**（冷启动）。不要按复位键：warm reset 会把 USB
  数据通道卡死，表现是 RNDIS 枚举得到但 ping 不通。

## 5. 联网（RNDIS）

板子原生 USB 是 RNDIS gadget `584e:5342`，板侧固定 `192.168.137.2`，
PC 侧固定 `192.168.137.1`。冷启动后：

```bash
lsusb | grep 584e                 # 必须看到 NuttX RNDIS gadget
~/rndis_net_up.sh                 # 绑网卡、配 192.168.137.1/24、NAT
ping -c 2 192.168.137.2           # 必须通
```

常见状态：

| 现象 | 含义 | 处理 |
|---|---|---|
| `lsusb` 看不到 `584e:5342` | 原生 USB 没插/没枚举 | 插好原生 USB（和 CH343 串口是两根） |
| 网卡在、ping 不通、`RX: 0 packets` + TX errors | 数据通道死 | 拔电 5 秒冷启动，**不要按复位键** |
| `ip neigh` 显示 `FAILED` + `NETDEV WATCHDOG` | 同上 | 同上 |
| NM 抢管网卡 | 静态 IP 被 DHCP 刷掉 | 脚本会 `nmcli device set ... managed no` |

⛔ 永远不要往 `/sys/bus/usb/drivers/rndis_host/new_id` 写 ID（dynid 毒化，
会让 probe 静默 -ENODEV）。中招后：`sudo rmmod rndis_host && sudo modprobe rndis_host`。

## 6. 烧完怎么确认是新固件

串口（1000000 8N1）开机应看到本版标记，例如：

```
[版本] 人声余温 2.5 秒只认尖叫：…（2026-11-01）
[版本] TTS 首选火山语音合成2.0（seed-tts-2.0 / vv_uranus），MiMo 单块兜底（2026-11-01）
[版本] 天气快速通道：跳过 velaclaw/LLM 直接查 Open-Meteo（2026-11-01）
[版本] 录音 owner_gen 单读者守卫：stop 不再取消旧读者的 5s 心跳（2026-11-01）
[初始化] TTS 后端: 火山引擎 (volcengine, seed-tts-2.0, zh_female_vv_uranus_bigtts)
```

**看不到旧串**才是刷对了（旧串列表见每次改版的提交说明/发布说明）。

## 7. 串口采集

```bash
python3 ~/serial_log.py             # 只读追加 ~/board_serial.log
python3 ~/wait_and_capture.py       # 等板子上线，专抓开机日志
python3 /tmp/boot_capture_loop.py   # 断线重连版（抓冷启动/崩溃）
```

铁律：只读、先拉低 RTS/DTR 再 open、**烧录前停掉**。
