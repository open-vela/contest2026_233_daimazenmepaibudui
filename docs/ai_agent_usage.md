# ai_agent（端侧 AI Agent）使用说明

> 智爱陪伴 —— openvela 板级/应用接口（可交付给队友直接照抄）
> 状态：**本文结论都在真机上验证过**（2026-09-13，见文末"验证记录"），日志在
> `docs/test_logs/2026-09-13-ai_agent-llm-verify.log`（只读）。
> 相关：`docs/network_api_usage.md`（网络）、`README.md` 第 6 节（ICS 共享，含换 Wi-Fi 的坑）

---

## 0. 一句话总览

| 想做的事 | 怎么做 |
|---|---|
| 启动 agent | `nsh>` 里敲 `ai_agent`（**前台，不要加 `&`**） |
| 看它认到网没有 | `net_status` |
| 配大模型后端 | `set_llm <preset\|host> [model] [key]` |
| 问它一句话 | `ask <文本>` |
| 看内存 | `heap_info` |
| 退出 | `quit` |

⚠️ **`ask` 目前有一个已知 bug**：请求真的发出去、模型也真的回了，但客户端把耗时
统计成天文数字（实测 `955862003 ms`）从而判超时，**回复被丢弃**。详见第 5 节。
在它修好之前，**`ask` 还不能算"可用"**，但云端通路已经证明是通的。

---

## 1. 怎么启动（以及别启动第二次）

```text
nsh> ps                # 先看有没有 ai_agent 已经在跑
nsh> net_test          # 4/4 PASS 再往下走
nsh> ai_agent          # 前台启动，不要加 &
```

启动成功的标志（真机输出）：

```
[agent] AI Agent ready. Type 'help' in NSH for commands.
[netmgr] Found iface eth0 addr 192.168.137.2
[netmgr] Network connected: 192.168.137.2
[agent] Agent loop started, free heap: 7387056
[agent] Buffers: ctx=8192 hist=8192 tool=8192
[agent] Tool output pool: 2 x 16384 bytes
[agent] Tools JSON loaded: 9985 bytes
[ws] WebSocket server started on port 28789
[mqtt] No mqtt_broker configured, MQTT disabled
[cli] NSH CLI started. Type 'help' for commands.
```

- **别启动第二个实例**：先 `ps` 确认没有 `ai_agent` 的进程。
- 它打印的是 `Type 'help' in NSH for commands`，但**同时也会接管出一个 `vela>` 提示符**——
  两种入口都能收命令，见下一条。
- 它会起一个 WebSocket 服务（`28789`）和一个 network watcher 线程。

## 2. 两个入口：`nsh>` 和 `vela>` 都能用

这一点和"只有 `vela>` 能用"的直觉不同，**本固件里两种都生效**：

| 入口 | 说明 |
|---|---|
| `nsh>`（NSH 命令行） | `ai_agent` 把命令**注册进了 NSH**，所以 `help` 里能看到它们 |
| `vela>`（agent 自己的 CLI） | 启动后出现的提示符，直接敲同样的命令也行 |

`help` 的实际输出（真机）：

```
  net_status           - Show network connection status
  set_llm <preset|host> [model] [key] - Switch LLM backend
  set_vision_llm <preset|host> [model] [key] - Set independent vision model
  heap_info            - Show memory usage
  ask <text>           - Chat with AI Agent
  quit                 - Exit agent
```

> 注意：因为有 `vela>` 在收输入，**别同时用两个终端抢同一个串口**（会互相抢输入）。

## 3. `set_llm` 的用法（以板子打印为准）

不带参数执行 `set_llm`，板子会把完整用法打出来（真机原文）：

```
Presets:
  kimi     - api.moonshot.cn          (kimi-k2.5)
  qwen     - dashscope.aliyuncs.com   (qwen-turbo)
  deepseek - api.deepseek.com         (deepseek-chat)
  glm      - open.bigmodel.cn         (glm-4-flash)
  openai   - api.openai.com           (gpt-4o)
  openrouter - openrouter.ai          (openrouter/hunter-alpha)
  mimo     - api.xiaomimimo.com       (MiMo-v2-Flash)
URL format (for custom endpoints):
  set_llm http://host:port/path model key
  set_llm http://<your-endpoint>/v1 model-name sk-xxx
Note: set_llm writes to router slot 0 and applies immediately.
```

两种用法：

```text
# ① 用预设（内置的那几个）
set_llm kimi <key>
set_llm deepseek <key>

# ② 用自定义端点（推荐，见下面的踩坑）
set_llm <完整URL> <模型名> <key>
```

### ⚠️ `mimo` 预设别直接当"我们那套"用

预设里**确实有 `mimo`**，但它指向的是 **`api.xiaomimimo.com` + 模型名 `MiMo-v2-Flash`**
（老站点 + 老模型名）。**我们工程实际验证可用的是另一套**（Token Plan 端点）：

```text
set_llm https://token-plan-cn.xiaomimimo.com/v1/chat/completions mimo-v2.5 <key>
```

配置成功的回显（**这句不打印密钥**，可以放心录）：

```
[llm] LLM config updated atomically: token-plan-cn.xiaomimimo.com/v1/chat/completions (model: mimo-v2.5)
```

> `mimo-v2.5` 本身是有效的（小米官方模型列表里有 MiMo-V2.5 系列），但**板上预设可能
> 仍是旧模型名**——能不能调通要跟账户实际支持的模型对应，**不能只看本地 Kconfig**。

## 4. ⚠️ 密钥的处理（重要）

- **`set_llm` 必须给密钥**，板子里没有内置可用的；不给就是
  `[llm_router] No available backend`，`ask` 直接失败（连网络都不会发）。
- **板子会把整条命令明文回显到串口上**，所以：
  - **别录屏、别开串口录制、别让 AI 自动采集**这一段；
  - **别把这条命令、也别把密钥提交进 `logs/`**；
  - 密钥来源是[小米 MiMo 开放平台](https://platform.xiaomimimo.com/)的控制台，
    不是 VS Code 插件名、不是登录密码。
- 推荐做法：**让脚本自己去读密钥、只发送、不读回显、不落盘**。
  本仓库的做法见 `_flash/_setup_llm.py`（运行时从构建目录的 `.config` 里把 key 读进内存，
  全程不打印）。这样密钥不会进任何日志文件。
- 换一把新 key 时，只要重新 `set_llm` 一次即可（**立即生效**，写的是 router slot 0，不用重启板子）。

## 5. 已知问题：`ask` 收到回复却判超时（**待修**）

真机实测（配置成功之后）：

```
[trace:386d46b435c3b13a] BEGIN chat=console chan=cli
[llm] OpenAI API with tools (model: mimo-v2.5, 14925 bytes)     ← 请求真的发出去了
[llm] Response: 12 bytes text, 0 tool calls, finish=end_turn      ← ★ 收到真实回复
[agent] LLM watchdog: call took 955862003 ms (limit 60s), treating as timeout   ← ★ 耗时是脏值
[trace:...] END status=timeout ... llm_ms=955862003 elapsed=825589583s
[Agent]: 请求超时，LLM 响应时间过长。请稍后重试，或尝试简化你的问题。
```

**判读**：DNS ✅ / TLS·HTTPS ✅ / 鉴权 ✅（没有 401/403，带 tools 的 14925 字节请求被接受）/
**模型回复 ✅**（`12 bytes` 正好是 4 个汉字，就是要求它回答的"连接成功"）。
**唯一的失败点是客户端的耗时统计**：`955862003 ms` ≈ 11 天，明显是脏值，把已经到达的
响应判成超时丢掉了。

排查方向（供负责 SDK 的同学参考）：

1. **tick 当 ms 用**（或反过来）——`clock_systime_ticks()` 是 tick，要 `TICK2MSEC()` 换算；
2. **秒差当 ms 用**；
3. **回调返回后才去读已释放/已复用的结构体**（读到脏内存）；
4. ⚠️ 还有一个容易被忽略的：**`RTC_SET_TIME` 会顺带同步系统时钟**
   （`clock_synchronize()`）。如果这里的耗时是用**墙钟**（`CLOCK_REALTIME` / `gettimeofday`）算的，
   而请求期间恰好有人对了时，差值就会变成天文数字。**建议统一改用单调时钟**
   （`CLOCK_MONOTONIC` 或 `clock_systime_ticks()`）。

> 结论：**云端通路已经验证通了，不要切 mock 去"凑"一个成功**；要修的是这个耗时统计。

## 6. 网络前提与判断

- 先 `net_test`：**4/4 PASS** 才继续（网卡 / DNS / TCP / MQTT 四层）。
- `net_status` 的判据是 **"有没有非回环的 IPv4 地址"**，它**不限定必须是 Wi-Fi**，
  所以 RNDIS 的 `eth0`（`192.168.137.2`）会被认到 —— 实测输出：

  ```
  [netmgr] Found iface eth0 addr 192.168.137.2
  Network connected: yes
  IP: 192.168.137.2
  ```

- ⚠️ 但它**只证明"有地址"，不证明互联网或 HTTPS 可用**。真要把"能不能出网"钉死，
  用 `net_test` 的第 3、4 步（TCP + MQTT CONNACK）。
- ⚠️ **板子 DNS 全失败时，先去看 PC 侧的 ICS DNS 代理**（不是板子的问题）：
  ```powershell
  nslookup api.day.app 192.168.137.1     # 必须在 PC 上问这个地址
  ```
  详见 `README.md` 第 6 节 + `docs/network_api_usage.md` 第 9 节。

## 7. 内存

`heap_info`（真机）：

```
Heap: arena=8648648  fordblks(free)=7274224  uordblks(used)=1374424
```

约 **8.65 MB 堆 / 空闲 7.27 MB / 占用 1.37 MB** —— 内存**不是**瓶颈。
agent 自己还会占：`ctx/hist/tool` 各 8192 字节、tool 输出池 `2 x 16384` 字节、
tools JSON 9985 字节。

## 8. 排错速查

| 现象 | 原因 / 怎么办 |
|---|---|
| `[llm_router] No available backend` | **还没配 LLM**：先 `set_llm ...`（不带参数可看用法） |
| `ask` 打出 `请求超时` 但 trace 里有 `Response: N bytes` | 第 5 节那个耗时统计 bug（回复已到、被判超时） |
| `Network connected: no` | 板子没有非回环 IPv4 → 查 RNDIS/USB 枚举、查 `net_test` |
| MQTT/`ask` 全部 DNS 失败 | **PC 侧 ICS DNS 代理死了**（换 Wi-Fi 后高发）→ `nslookup ... 192.168.137.1` |
| `[mqtt] No mqtt_broker configured, MQTT disabled` | agent 自带的 MQTT 没配 broker；**这是正常的**，应用侧的 MQTT 走 `network_comm`，两回事 |
| 启动后命令没人应 | 是不是启动了第二个实例 / 是不是另一个终端在抢串口 |

## 9. 验证记录（2026-09-13，真机）

| 项目 | 结果 |
|---|---|
| `ps` | 启动前无 `ai_agent` ✓（未启动第二个实例） |
| `net_test` | **4/4 PASS**（网卡 / DNS / TCP / MQTT CONNACK） |
| `ai_agent` 启动 | 认到 `eth0 192.168.137.2`，`free heap: 7387056`，WebSocket 28789 起来了 |
| `net_status` | `Network connected: yes`，`IP: 192.168.137.2` |
| `set_llm`（无参） | 打出了上面第 3 节的完整用法（含 `mimo` 预设） |
| `set_llm <自定义端点>` | `LLM config updated atomically: token-plan-cn.xiaomimimo.com/v1/chat/completions (model: mimo-v2.5)` |
| `ask` | 请求发出（14925 B）→ **收到 12 字节真实回复 `finish=end_turn`** → **被判超时丢弃**（第 5 节 bug） |
| `heap_info` | `arena=8648648 free=7274224 used=1374424` |

完整原始串口日志（只读，**不含密钥**）：
`docs/test_logs/2026-09-13-ai_agent-llm-verify.log`
