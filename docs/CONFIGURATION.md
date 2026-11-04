# 配置与凭据

> 所有密钥都放在 `board/contest_board/src/etc/assets/` 下，编译时打进 ROMFS，
> 开机由板级代码拷到 `/data/ai_agent/config/config.json`。**这两个文件不进 Git**
> （`.gitignore` 已挡）。拿到的代码里没有密钥时，大模型/ASR/TTS/Bark 都会失败，
> 这是预期行为，不是固件坏了。

## 1. 需要放哪些文件

| 文件 | 必需 | 用途 |
|---|---|---|
| `agent_config.json` | 是 | LLM、ASR、TTS（MiMo/火山）的 host/key/model/音色 |
| `push_key.txt` | 建议 | Bark/PushPlus 推送 key（报警通知家人） |
| `kws/`（模板目录） | 否 | 本地唤醒词模板（当前唤醒词已取消，保留备用） |

> ⚠️ 改完 `src/etc/assets/` 之后，**构建目录里的 ROMFS 不会自动更新**：
> 需要把 etc 同步到构建暂存目录并删掉 `romfs.img` 再编，步骤见
> [BUILD_AND_FLASH.md](BUILD_AND_FLASH.md#3-romfs-与密钥同步必看)。

## 2. `agent_config.json`

最小可用示例（**值全部换成你自己的**）：

```json
{
  "model": "mimo-v2.6-flash",
  "llm_host": "token-plan-cn.xiaomimimo.com",
  "llm_path": "/v1/chat/completions",
  "llm_port": "443",
  "api_key": "<你的 MiMo API Key>",
  "llm_backend_0": "{\"host\":\"token-plan-cn.xiaomimimo.com\",\"path\":\"/v1/chat/completions\",\"port\":\"443\",\"api_key\":\"<你的 MiMo API Key>\",\"model\":\"mimo-v2.6-flash\",\"priority\":0,\"cost_tier\":1}",

  "asr_model": "mimo-v2.5-asr",
  "asr_language": "zh",
  "volc_api_key": "<火山新控制台 API Key（36 位 UUID）>",
  "volc_asr_resource": "volc.seedasr.sauc.duration",
  "volc_speaker": "zh_female_vv_uranus_bigtts",

  "tavily_key": "<你的 Tavily API Key（tvly…）>",

  "qweather_host": "<你的和风天气 API Host>",
  "qweather_api_key": "<你的和风天气 API Key（32 位）>"
}
```

字段说明：

| 键 | 说明 |
|---|---|
| `model` / `llm_host` / `llm_path` / `llm_port` / `api_key` | 大模型网关（MiMo token plan） |
| `llm_backend_0` | 多后端配置（JSON 字符串）；留一份即可 |
| `asr_model` | 只有 `mimo-v2.5-asr` 是真 ASR；别拿 LLM 模型当 ASR |
| `asr_language` | 网关实测忽略；对 MiMo 没有方言能力 |
| `volc_api_key` | **新控制台** API Key（36 位 UUID）。旧控制台的 appkey/Access Token/Secret Key **没有授权**，不要用 |
| `volc_asr_resource` | 火山 ASR 2.0：`volc.seedasr.sauc.duration` |
| `volc_speaker` | 火山 TTS 2.0 音色；必须属于 `seed-tts-2.0` 资源 |
| `tavily_key` | **键名必须是 `tavily_key`**（`tvly-` 开头，58 位）。配了它，web_search/新闻优先走 Tavily；新浪财经只是免 key 兜底。⚠ 搜索 key 是**每个 app 一份静态变量**：ai_agent 与 hello_app 都要调用 `tool_web_search_init()`（已在 hello_app 启动时补上） |
| `qweather_host` | 和风天气**新版控制台**的专属 API Host（如 `xxxxxxxxxx.re.qweatherapi.com`）。在 [console.qweather.com](https://console.qweather.com) → 设置 → API Host 获取 |
| `qweather_api_key` | 和风天气 API KEY（32 位十六进制）。控制台 → 项目管理 → API KEY 获取。**两个都配齐才走和风**，缺一走 Open-Meteo 免 key |

### 火山 TTS 2.0（2026-11-01 起）

```
POST https://openspeech.bytedance.com/api/v3/tts/unidirectional
X-Api-Key: <volc_api_key>
X-Api-Resource-Id: seed-tts-2.0
Content-Type: application/json
{"req_params":{"text":"…","speaker":"zh_female_vv_uranus_bigtts",
  "audio_params":{"format":"pcm","sample_rate":16000}}}
```

- **不要把资源写成 `volc.service_type.10029`**：那是旧豆包 TTS，本机账号未开通，
  返回 403 `requested resource not granted`。
- **音色不要用 `zh_female_cancan`**：它属于旧资源，会报
  `resource ID is mismatched with speaker related resource`。
- 实测可用音色：`zh_female_vv_uranus_bigtts`、`zh_female_xiaohe_uranus_bigtts`。
- 返回是 JSON + base64 PCM（16k 单声道），响应缓冲 512KB。

### 后端优先级（代码里固定）

1. TTS：配了 `volc_api_key` → **火山 2.0 优先**；单块失败直连 MiMo 兜底。
2. ASR：配了 `volc_api_key` → 火山 v3（方言自动判别）；出错回落 MiMo。
3. LLM：velaclaw（ai_agent ReAct）优先；`-110` 超时后走 `mimo_chat` 直连兜底。

## 3. `push_key.txt`

只有一行 key（Bark 的 device key，或 PushPlus 的 token；同一个文件只能放一种）。
`network_comm.c` 读 `/etc/assets/push_key.txt`；没有文件时 `push_is_enabled()`
为假，报警页会显示"请直接联系家人"，Bark 一定发不出去。

**报警推送节奏**（`voice_emerg.c`）：10 秒确认期没人说「没事」→ 首发 Bark；
之后每 30 秒重发；180 秒自停。串口判据：`[PUSH] OK: HTTP 200`。

## 3.6 和风天气（QWeather）详细配置

和风天气新版控制台给每个账号分配**独立的 API Host**，不再共用 `devapi.qweather.com`：

1. 登录 [console.qweather.com](https://console.qweather.com)
2. 左侧「设置」→「API Host」→ 复制你的专属 Host
3. 「项目管理」→ 新建项目 → 免费订阅 → 在项目详情「KEY」处复制 API KEY
4. 填入 `agent_config.json`：

```json
"qweather_host": "<你的专属 API Host，如 xxxxx.re.qweatherapi.com>",
"qweather_api_key": "<你的 32 位 API KEY>"
```

**判据**（串口）：

```
[web_search] QWeather configured（host/key 长度 xx/xx，值不打）
```

如果看到 `QWeather 未配齐`，说明两个字段有一个没填。

> 和风天气免费额度：每天 1000 次，对家用足够。
> 未配置时自动走 Open-Meteo（免 key），天气照常可用，只是城市精度稍差。

## 3.7 Tavily 详细配置

1. 注册 [app.tavily.com](https://app.tavily.com)
2. 控制台 →「API Keys」→ 新建 → 复制（`tvly-` 开头）
3. 填入 `agent_config.json` 的 `tavily_key`

**判据**（串口）：

```
[web_search] Tavily key configured
[web_search] News via Tavily OK: N bytes
```

> 免费额度：每月 1000 次搜索。未配置时新闻走新浪财经（只有财经要闻），
> 搜索功能不可用，但天气快路不受影响（它走和风/Open-Meteo）。

## 4. 常见配置问题

| 现象 | 原因 | 处理 |
|---|---|---|
| ASR 正常、TTS 没声 | 火山 TTS 资源/音色不对（403 / resource mismatch） | 用 `seed-tts-2.0` + 2.0 音色；或看串口 `[volc_tts] HTTP 403` |
| TTS 突然不出声 | MiMo 429 配额耗尽 + 火山没配 | 看 `[TTS] MiMo 合成失败…降级到火山引擎`；按上面配火山 |
| 报警没推送 | `push_key.txt` 不在 ROMFS | 放文件 → 同步 ROMFS → 重编；串口看 `push_is_enabled` 相关行 |
| 改配置没生效 | ROMFS 没重建 | 删 `romfs.img` 重编，见 BUILD_AND_FLASH |
| 天气默认城市不对 | `MIMO_DEFAULT_CITY` / 语音里没说城市 | 当前默认济南；说「北京天气」这类已知城市会走快速通道 |
| 旧后端凭据想用 | 旧控制台 appkey/Access Token 未授权 | 不要用；用新控制台 API Key（UUID） |

## 5. 安全约定

- 两个密钥文件已在 `.gitignore`；提交前再跑一次
  `git status --short`，确认它们不在列表里。
- 文档/日志/补丁里不要出现明文 key。`patches/verify_upstream_trees.sh`
  会扫描补丁里的凭据形状，提交补丁前跑一遍。
- 泄漏过 key 的话，去控制台吊销重发，别只删文件。
