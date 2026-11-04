# 排障手册（串口指纹 → 定位）

> 先看串口。这个项目的绝大多数问题在日志里都有**唯一指纹**；没有日志时不要猜。
> 采集方法见 [BUILD_AND_FLASH.md §7](BUILD_AND_FLASH.md#7-串口采集)。

## 1. 录音链路卡死 / 录音不活跃

**指纹**

```
[监听守护] 录音线程卡在设备读里 8019 ms（驱动一次等满只有 5 秒上界，说明那次等待的超时丢了）
[AUDIO] stop: 录音线程 1000ms 内没退出，放弃 join（它卡在设备调用里；detach 交给内核回收）
[录音] 录音异常中断（read 返回 -1，errno=19，不是本层在停）
AUDIO_IN: 本次 stop **空转**：线程 N 仍在收尾这一次会话
audio: STOP 没停到硬件（status=5 nstate=5） / 会话[0] #N priv->state=5 ★就是它在挡
```

**根因（2026-11-01 修）**：旧单读者守卫 `rx_busy && !rx_aborted` 在 stop 之后
放行新读者；新读者进门 `nxsem_reset + wd_cancel` 把旧读者那记 5 秒心跳拆掉，
旧读者从此没有上界。现在驱动用 `rx_owner_gen`：只有 owner 能清 `rx_busy`，
新读者一律让路，`SF32LB52_AUDIO_RX_BUSY_STEAL_MS`（6.5s）才抢占。

**排查顺序**：

1. 看是否有 `AUDIO: read 撞上另一个读者…-EBUSY` —— 有就说明守卫在按设计工作；
2. 看 `AUDIO: 上一个读者占着 rx_busy 已 … ms，判它已死，抢占` —— 抢占生效；
3. 仍反复卡死且 `rx_busy` 在 HAL 里 → 厂商 HAL 的无界自旋（见 `sf32lb52_audio.c`
   注释），需要复现时抓 `dumpstack` 定位 PC；
4. 上层的 `audio_reclaim_stale("…")` 是最后一道兜底，不要当主修。

## 2. 有字不出声（TTS 静音）

按时间顺序看三种指纹：

| 指纹 | 根因 | 处理 |
|---|---|---|
| `[mimo_voice] TTS: HTTP 429 … quota exhausted` | MiMo TTS 配额耗尽 | 配火山 TTS（见 CONFIGURATION） |
| `[mimo_voice] TTS: 响应里没有完整的 audio.data（…超过 896KB 被截断？）` | 分块太大/MiMo 响应超缓冲 | 已把 MiMo 分块降到 32 字；`-EPROTO` 不再触发降级 |
| `[volc_tts] TTS HTTP 403 … requested resource not granted` | 火山资源 ID 用了旧豆包 `10029` | 改为 `seed-tts-2.0` |
| `resource ID is mismatched with speaker related resource` | 音色不属于该资源（如 `zh_female_cancan`） | 用 2.0 音色 `zh_female_vv_uranus_bigtts` |
| `[TTS] MiMo 合成失败…降级到火山引擎` 之后仍无声 | 火山未开通 TTS | 开通 TTS 2.0，或修好 MiMo 配额 |

正常判据：`[volc_tts] TTS V3 request` → `TTS: synthesized … PCM bytes` →
`[TTS] 首字开始播放`，且没有 403。

## 3. 问一句要一两分钟（velaclaw -110）

**指纹**

```
[LLM] AI请求失败: -110（velaclaw 那条），改走 mimo_chat 直连兜底
[轮次] 直连兜底开始，LLM 相位重新起算
[轮次] ★ 轮 2 卡在 LLM 相位 45053 ms（上限 45000），作废
[基线] ③ LLM 回复到手（距说完 99xxx ms）
```

**说明**：`-110` 是 velaclaw/ai_agent 那条链的等待超时（健康轮约 11.5 秒），
不是配置错误；直连兜底会重跑一遍工具，带工具的题很容易 40 秒以上。
**天气已走快速通道**（`[天气快路]`），不再经过这条链。其它复杂工具题仍可能慢，
属于已知限制。

## 3.1 新闻/搜索答「网络不太稳」

**指纹**

```
[web_search] 新闻（新浪免 key）：5 条 / 682 字节      ← 搜索成功
[web_search] Tavily OK: 8191 bytes
[agent] LLM call failed on backend 0, trying failover
[agent] LLM call failed (iter 2)
[LLM] AI回复: 哎呀，网络好像不太稳，这一轮没能答上来。
```

**含义**：搜索接口是好的，失败在**最后那次 LLM 汇总**（把搜索结果组织成回答）。
**处理**：

- 新闻/头条类已走 `[新闻快路]`，直接念新浪头条，不经过 LLM；
- 普通搜索（非新闻）仍可能踩到，重试一次或换网络；
- 如果频繁出现，看 `[llm]`/`[agent]` 的 backend 与 `-110`，按 §3 处理。

## 3.2 新闻只走新浪，Tavily 没生效

**指纹**

```
[web_search] News search: 国际新闻
[web_search] 新闻（新浪免 key）：5 条 / 682 字节
（没有 "News via Tavily OK"）
```

**根因（2026-11-01）**：`tavily_key` 配置是对的，但
`tool_web_search.c` 的 `s_tavily_key` 是**每个 app 各一份静态变量**：

- ai_agent 进程启动时调过 `tool_web_search_init()`，它的 web_search 能用 Tavily
  （日志 `Tavily OK`）；
- hello_app 的新闻快路直接调 `tool_news_search_execute()`，从没初始化过 →
  它这份 key 是空的 → 只能落到免 key 的新浪。

另外新闻工具**原来就是新浪优先**，即使 key 有值也会先走新浪。

**修复**：hello_app 启动时 `tool_web_search_init()`；新闻工具改为
**Tavily 优先、新浪兜底**。判据：

```
[web_search] Tavily key configured        ← 初始化生效
[web_search] News via Tavily OK: N bytes  ← 真走 Tavily
```


### 3.3 实时信息只说一句 / 念的是网站首页

**指纹**

```
[web_search] Tavily OK: 1023 bytes
[实时] 取到结果…：1. 主页 - BBC News 中文
```

**含义**：

- `1023 bytes` 是**调用方缓冲只有 1024 字节**被截断（不是 Tavily 只返回一条）。
  v42 起缓冲放大到 12 KB，串口应看到明显更大的字节数。
- 如果标题是「XX网/首页」，说明 Tavily 对这句 query 只给了导航页。v42 起
  实时信息不再按娱乐/体育/财经猜分类，而是把**用户原话**交给通用
  `web_search`；仍不行会退 `news_search`（新浪财经免 key），再不行回落 LLM。
- 版权/页脚行（`Copyright`/`版权所有`/`免责声明`…）已在提取时过滤，不再念出来。

## 4. 自己报警 / 误报

| 指纹 | 含义 | 当前策略 |
|---|---|---|
| `★命中 fall/knock（置信度 0.55~0.79）`（无人操作、单发） | 环境噪声/撞击被判出来 | 单次只记时间，**15 秒内第二次**才报；单次 ≥0.80 才立刻报 |
| `★命中 fall`（连续 >6s 的持续声） | 持续环境噪声被判跌倒 | `⚠声音还在持续（未回落），先不报`；短敲不再被这条挡住 |
| `★命中 knock`（说话/喊叫之后） | 人声尾音被判敲击 | 人声窗只允许 scream；人声后 2.5s 余温；撞击两次确认 |
| `人声窗 scores … scream=0.00` | 模型没有可用的尖叫检测 | 无词大喊不报警是设计；请清楚地说「救命/报警/着火」 |
| 单下敲击不报警 | 设计 | 15 秒内连敲两下（fall/knock 都算） |
| 喊「救命」没反应，日志无 `[急救词]` | ASR 没转出文本（嘶吼/长音频返回空） | 看 `[AI] 识别结果`；正常清楚地说；见 §1/§5 |

## 5. 说「没事」没反应

**指纹**

```
[急救] #N 听 2800 ms 起（距触发 …，录音链路 死的/活着，这一窗有没有音频流过）
[急救] 听窗内收到人声 -> 开麦收「没事」        ← 好现象
（没有 [AI] 识别结果）
```

- 若听窗里录音是"死的/没收到音频" → 先修 §1 录音链路；
- 若门控开了麦但没有识别结果 → 看 `[识别] 上一段还在识别中`、`[轮次] ASR 相位`；
- 老人说「没事」时铃必须停：现在是 1.2s 响 / 2.8s 听，**说的时候别压着铃**；
- 只有屏幕按钮能停说明语音链路没走通，把上面几行日志发出来。

## 6. 网络（RNDIS）

| 指纹 | 根因 | 处理 |
|---|---|---|
| `lsusb` 无 `584e:5342` | 原生 USB 没插/没枚举 | 插好两根线（CH343 + 原生 USB） |
| `RX: 0 packets`、`TX errors`、`ip neigh FAILED`、`NETDEV WATCHDOG` | 数据通道死 | 拔电 5 秒冷启动，**不要按复位键** |
| `connect: Error 101`（ENETUNREACH） | 开机头几秒网络还没就绪 | 等 10 秒 / 重跑 `~/rndis_net_up.sh` |
| `MQTT DNS 解析失败` | 同上，瞬态 | 看后续是否 `MQTT connected` |
| `new_id` 中毒 | 手动写 driver new_id | `sudo rmmod rndis_host && sudo modprobe rndis_host` |

## 7. 对时

| 指纹 | 含义 | 处理 |
|---|---|---|
| `[TimeSync] 取 … Date 头失败: -1` | 网络未通/HTTP 失败 | 网络通了会自动重试（60s） |
| `[TimeSync] 已对时 -> …` | 成功 | 屏幕时钟 10 秒内刷新 |
| 屏幕时间不对但 TLS `UNIX=` 正确 | UI 时钟/时区 | 检查 `UI_TZ_OFFSET_SEC`（+8h）和时钟槽是否满 |

## 8. 连接池 / 越用越慢

**指纹**

```
[TCP] pool=28/128 act=27 st[D25 …] peers[443:27]
[vela_tls] net_connect … ret=0x42 errno=12   （ENOMEM）
```

- `D` = `TCP_CLOSED` 但仍占着 `tcp_conn` 槽位；`pool` 接近 128 或
  `socket()` 返回 ENOMEM → 出站连接没回收。
- **根因（v42 已修，2026-11-02）**：`tcp_close_work` 先置 `TCP_CLOSE_DIRECT`，
  执行时状态还是 `FIN_WAIT_2`，于是跳过 `tcp_free`，之后没人再回调；
  2MSL 后 `tcp_timer` 置 `TCP_CLOSED` 并 `tcp_callback(TCP_TIMEDOUT)`，
  但 close 回调已释放 → 条目永远挂在 active 链表上。修复：
  - `net/tcp/tcp_timer.c`：TIME_WAIT 到期且 `crefs==0 && TCP_CLOSE_DIRECT`
    时直接 `tcp_free`；
  - `net/tcp/tcp_conn.c`：池满回收条件放宽，允许复用已跑过 close work 的
    CLOSED 条目；并在 `tcp_alloc` 里**每新建一条连接主动回收一条**
    `crefs==0 + CLOSED + TCP_CLOSE_DIRECT` 的僵尸条目（服务端 RST/短连接
    会绕过 TIME_WAIT，仅靠定时器不够）。
- 验证：修复后 45 分钟真机运行 `pool` 稳定在 `5~7`、`D` 为 `0~1`；
  修复前同长度测试为 `pool 6→29 / D0→21`。仍单调上涨就把
  `[TCP] pool` 行和 `[TLS-FD] open/close` 成对发出来。

## 9. 烧录 / 启动

| 现象 | 处理 |
|---|---|
| `Failed to download stub` | 断电 10 秒重试；`--compat true`；`--before no_reset` |
| 烧完黑屏/不启动 | 确认 ftab 和 nuttx 都写了；冷启动；看串口有无 boot banner |
| 开机没有 `[版本]` | 刷的是旧固件，重刷并核对筛查串 |
| 改了 assets 没生效 | ROMFS 没重建，见 BUILD_AND_FLASH §3 |
| 串口无输出 | 确认 1000000 8N1、RTS/DTR 拉低、没有 sftool 占着 |

## 10. 抓证据的最小命令

```bash
python3 ~/serial_log.py                  # 只读记录
grep -aE "急救|PUSH|Assertion|卡在|放弃 join|reclaim|监控" ~/board_serial.log | tail -50
grep -a "\[TCP\] pool" ~/board_serial.log | tail -5
```

> 改代码前先确认"是哪一条指纹"；没有指纹的改动是在猜。
