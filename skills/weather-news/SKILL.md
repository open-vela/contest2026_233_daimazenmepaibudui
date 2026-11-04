---
name: weather-news
description: 天气与新闻快路（免 LLM，直接查数据源并念结果）
---

# 天气 / 新闻 Skill

## 功能

识别天气/新闻意图后**跳过 LLM**，直接查数据源并交给 TTS 播报。
避免 velaclaw 的 ReAct 超时（-110 可达 45 秒）。

## 天气

```
识别到天气意图 → 提取城市 → QWeather（配置后优先）/ Open-Meteo → 念结果
```

| 后端 | 地址 | 是否需要 key |
|---|---|---|
| 和风天气 | `<qweather_host>/v7/weather/now` | 需要 `qweather_host` + `qweather_api_key` |
| Open-Meteo | `api.open-meteo.com` + `geocoding-api.open-meteo.com` | 免 key |

- 城市表：`g_known_cities[]`（省/市/县 60+ 个）
- 默认城市：济南（`MIMO_DEFAULT_CITY`）
- 不认识的城市的说法 → 回落到大模型

## 新闻

```
识别到新闻意图 → 按类别选查询词 → Tavily topic=news → 念标题+摘要
```

| 类别 | 查询词 |
|---|---|
| 娱乐 | 娱乐新闻 |
| 体育 | 体育新闻 |
| 科技 | 科技新闻 |
| 政治/时事 | 时事新闻 |
| 军事 | 军事新闻 |
| 财经 | 财经新闻 |
| 国际 | 国际新闻 |
| 健康 | 健康新闻 |

## 新闻概括

说完新闻标题后，用户问「详细说说第X条」→ 用 `tool_news_search_execute` 搜正文 → 念前 60 字摘要。

## 密钥配置

见 [README 第 3 节](../../README.md#3-密钥配置) 和 [docs/CONFIGURATION.md](../../docs/CONFIGURATION.md)。

## 相关代码

- `app/hello_app/ai_companion_main.c` —— `weather_fast_try` / `news_fast_try` / `news_detail_try`
- `app/hello_app/mimo_voice.c` —— `mimo_weather_lookup`（Open-Meteo 底层）
- `packages/ai_agent/src/tools/tool_web_search.c` —— Tavily / 和风 / 新浪
