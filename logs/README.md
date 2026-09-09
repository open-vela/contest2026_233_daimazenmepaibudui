# logs/ — AI Coding 日志目录

存放开发过程中与 AI 工具的对话日志，随作品代码一并提交。

## 目录结构

```text
logs/
└── gaoxiaoying0207/             # 开发者 GitHub 用户名
    ├── manifest.json            # 会话清单
    └── 2026-08-30/              # 日期目录
        ├── claude-code__7b5954e6-f504-4969-a7a0-bd1fe66b1f8c.jsonl
        └── claude-code__3db08d5b-17b6-4d79-b936-826b0b2d2dbc.jsonl
```

## 日志说明

- **工具**: claude-code (Claude Code CLI)
- **格式**: JSONL (每行一个事件)
- **内容**: AI对话、工具调用、代码生成、调试过程

## 提交方式

```bash
# 添加日志
git add logs/

# 提交
git commit -s -m "logs: sync AI sessions"

# 推送
git push
```

## 日志验证

```bash
# 查看日志列表
ls -la logs/gaoxiaoying0207/2026-08-30/

# 验证日志格式
python3 ../.claude/skills/contest-log-collector/tools/validate-log.py logs/
```

详细说明见[《AI Coding 日志归集与提交手册》](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/ai_coding_log_guide.md)。
