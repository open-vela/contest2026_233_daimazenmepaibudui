#!/usr/bin/env bash
# 核对「三棵上游树的改动有没有完整跟着 contest 仓库走」。
#
# 为什么要这个脚本（2026-10-30 踩的坑，两个）：
#   1. patches/README.md 一直写着「补丁都能 git apply --check --reverse 通过」，
#      那天实测 nuttx 4 个补丁里 3 个过期、另有 3 个改动的文件压根没补丁。
#      没有任何东西会在补丁失修时报警，于是「上传 contest 仓库」和「板子上
#      跑的固件」悄悄脱钩了。
#   2. 第一版核对脚本只跑 `git apply --check --reverse` 就宣布全绿 —— 那条
#      **只证明「补丁和它碰的那几个文件对得上」，不证明没漏文件**。实测
#      packages/ai_agent 有 41 个文件改动，而 per-fix 补丁只覆盖 3 个，
#      38 个文件（含 include/agent_config.h、src/voice/volc_asr.c）会丢，
#      脚本照样打「全绿」。所以这一版多加了**完备性**这一条判据。
#
# 两条判据，缺一不可：
#   ① 一致性：git apply --check --reverse 能干净反打
#              = 补丁内容和当前工作树逐字对应（既没漏也没多）；
#   ② 完备性：树里**每个**在补丁范围内的改动文件，都出现在补丁里。
#              —— 这条才管「有没有整文件被漏掉」。
#   ③ 安全性：补丁新增行里不能出现凭据形状（这些补丁是要传 GitHub 的）。
#
# 只读：不改任何文件、不打任何补丁。
# 用法：bash patches/verify_upstream_trees.sh

set -u

OPENVELA_ROOT="${OPENVELA_ROOT:-/home/xiaoyingyu/openvela}"
HERE="$(cd "$(dirname "$0")" && pwd)"

# ── 权威补丁：只认这三份，都是从当前工作树 `git diff` 直接生成的，覆盖各自
#    树**全部**补丁范围内的改动。**打补丁时也只打这三份**，不要和下面 README
#    表格里那些 per-fix 历史补丁叠加。
#
#    补丁范围：
#      nuttx          = 全部改动（6 个文件）
#      vendor/sifli   = chips/sf32lb52/** + boards/sf32lb52/drivers/lcd/**
#        （boards/sf32lb52/sf32lb52_devkit_lcd/src 是指向 contest 仓库
#          board/contest_board/src 的**软链接**，板级源码本来就跟着 contest
#          仓库走，不进补丁 —— 详见 README「板级改动不走补丁了」一节）
#      packages/ai_agent = 全部改动（去掉 .built / .depend 两个构建产物）
#
# 格式：树相对路径 | 补丁文件名 | 补丁范围的 git pathspec（可多个，空格分隔）
TREE_PATCHES=(
  "nuttx|nuttx-working-tree.patch|."
  "vendor/sifli|vendor-sifli-working-tree.patch|chips/sf32lb52 boards/sf32lb52/drivers/lcd"
  "packages/ai_agent|apps-ai-agent-working-tree.patch|. :(exclude).built :(exclude).depend"
)

bad=0

echo "=== ① 一致性：补丁 ↔ 工作树 逐字对应 ==="
for entry in "${TREE_PATCHES[@]}"; do
  tree="${entry%%|*}"
  rest="${entry#*|}"
  patch="${rest%%|*}"
  dir="$OPENVELA_ROOT/$tree"
  file="$HERE/$patch"

  if [ ! -d "$dir" ]; then
    printf '  ✗ %-40s 找不到树 %s\n' "$patch" "$tree"; bad=$((bad + 1)); continue
  fi
  if [ ! -f "$file" ]; then
    printf '  ✗ %-40s 补丁文件不在\n' "$patch"; bad=$((bad + 1)); continue
  fi

  if err=$(git -C "$dir" apply --check --reverse "$file" 2>&1); then
    printf '  ✓ %-40s %s\n' "$patch" "$tree"
  else
    printf '  ✗ %-40s %s\n' "$patch" "$tree"
    printf '        %s\n' "$(printf '%s' "$err" | head -2)"
    bad=$((bad + 1))
  fi
done

echo
echo "=== ② 完备性：范围内每个改动文件都要在补丁里 ==="
for entry in "${TREE_PATCHES[@]}"; do
  tree="${entry%%|*}"
  rest="${entry#*|}"
  patch="${rest%%|*}"
  spec="${rest#*|}"
  dir="$OPENVELA_ROOT/$tree"
  file="$HERE/$patch"
  [ -d "$dir" ] && [ -f "$file" ] || continue

  # shellcheck disable=SC2086  # pathspec 是刻意要分词的
  # ⚠ comm 只认 **LC_ALL=C 的字节序**，用本地 locale 排序它会报
  #   "file is not in sorted order" 并且**照样输出结果**（结果是错的）。
  #   2026-10-30 第一版就栽在这儿：一边喊 comm 报错一边打 ✓。
  changed=$(git -C "$dir" diff --name-only -- $spec | LC_ALL=C sort -u)
  covered=$(grep -E '^\+\+\+ b/' "$file" | sed 's|^+++ b/||' | LC_ALL=C sort -u)

  missing=$(comm -23 <(printf '%s\n' "$changed") <(printf '%s\n' "$covered") | grep -v '^$' || true)
  extra=$(comm -13 <(printf '%s\n' "$changed") <(printf '%s\n' "$covered") | grep -v '^$' || true)

  n=$(printf '%s\n' "$changed" | grep -c . || true)
  if [ -z "$missing" ]; then
    printf '  ✓ %-40s %s（%s 个文件全在补丁里）\n' "$patch" "$tree" "$n"
  else
    printf '  ✗ %-40s %s（%s 个改动文件，补丁漏了 %s 个）\n' \
           "$patch" "$tree" "$n" "$(printf '%s\n' "$missing" | grep -c .)"
    printf '%s\n' "$missing" | sed 's/^/        漏：/'
    bad=$((bad + 1))
  fi
  if [ -n "$extra" ]; then
    printf '        （补丁里有、工作树里已无的文件 %s 个）\n' \
           "$(printf '%s\n' "$extra" | grep -c .)"
  fi
done

echo
echo "=== ③ 安全性：补丁里不能带凭据（这些补丁是要传 GitHub 的）==="
python3 - "$HERE" <<'PY'
import glob, hashlib, re, sys
pats = {
    "tp- 密钥":      r'\btp-[A-Za-z0-9_\-]{20,}',
    "sk- 密钥":      r'\bsk-[A-Za-z0-9_\-]{20,}',
    "tvly- 密钥":    r'\btvly-[A-Za-z0-9_\-]{20,}',
    "64 位 hex":     r'\b[0-9a-f]{64}\b',
    "36 位 UUID":    r'\b[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\b',
    "和风 host":     r'\b[a-z0-9]{6,}\.re\.qweatherapi\.com\b',
    "Bearer":        r'Bearer\s+[A-Za-z0-9_\.\-]{16,}',
}
bad = 0
for f in sorted(glob.glob(sys.argv[1] + "/*working-tree.patch")):
    adds = [l[1:] for l in open(f, encoding="utf-8", errors="replace")
            if l.startswith("+") and not l.startswith("+++")]
    blob = "\n".join(adds)
    hits = []
    for name, pat in pats.items():
        for m in sorted(set(re.findall(pat, blob)))[:3]:
            hits.append((name, len(m), m[:4], m[-4:],
                         hashlib.sha256(m.encode()).hexdigest()[:8]))
    if hits:
        bad += 1
        print(f"  ✗ {f.split('/')[-1]}")
        for name, ln, a, b, h in hits:
            print(f"        ★ {name}: 长度 {ln} 首4 {a} 尾4 {b} sha256前8 {h}")
    else:
        print(f"  ✓ {f.split('/')[-1]}  新增 {len(adds)} 行，未见凭据形状")
sys.exit(1 if bad else 0)
PY
[ $? -ne 0 ] && bad=$((bad + 1))

echo
if [ "$bad" -eq 0 ]; then
  echo "★ 全绿：三棵树的改动都完整跟着 contest 仓库走，上传不会丢。"
  exit 0
fi
echo "★ $bad 条没达标 —— 上游树的改动**没有**完整跟着 contest 仓库走。"
echo "  重新生成那一份补丁（以 nuttx 为例）："
echo "    cd $OPENVELA_ROOT/nuttx"
echo "    git diff > $HERE/nuttx-working-tree.patch"
echo "  生成后先跑一遍上面 ③ 的凭据扫描，确认干净再提交。"
exit 1
