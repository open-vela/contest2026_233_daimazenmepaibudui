#!/bin/bash
#
# 把工作区里的板级实现（board/contest_board）暴露给 openvela 构建树。
#
# 为什么不是整目录软链：
#   nuttx/CMakeLists.txt 里用
#       nuttx_create_symlink(${NUTTX_BOARD_ABS_DIR}/../drivers .../drivers/platform)
#   去找芯片侧驱动目录。如果 sf32lb52_devkit_lcd 整个是软链，
#   那么 ".../sf32lb52_devkit_lcd/../drivers" 里的 ".." 会按 POSIX 规则
#   解析到**软链目标的父目录**（判题：board/），于是 drivers 找不到、
#   构建报 "drivers/platform/input/ft6146.c missing"。
#   所以 sf32lb52_devkit_lcd 必须保持是「真目录」，只有里面的**每一项**是软链。
#
# 什么时候要重跑：
#   `repo sync` 之后，vendor/sifli 这个子仓库可能把原始目录签回来，
#   覆盖掉这里的软链 —— 那就重跑一次本脚本。
#
# 用法：  bash board/contest_board/scripts/link_board_impl.sh
#
set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # .../board/contest_board/scripts
BOARD_DIR="$(cd "$HERE/.." && pwd)"                     # .../board/contest_board

# openvela 工作树：优先用环境变量，其次按常见布局猜
OPENVELA_DIR="${OPENVELA_DIR:-}"
if [ -z "$OPENVELA_DIR" ]; then
  for c in "$BOARD_DIR/../../../.." "$HOME/openvela"; do
    if [ -d "$c/vendor/sifli/boards/sf32lb52" ]; then OPENVELA_DIR="$(cd "$c" && pwd)"; break; fi
  done
fi

if [ -z "$OPENVELA_DIR" ] || [ ! -d "$OPENVELA_DIR/vendor/sifli/boards/sf32lb52" ]; then
  echo "找不到 openvela 工作树；请用 OPENVELA_DIR=/path/to/openvela 指定" >&2
  exit 1
fi

DEST="$OPENVELA_DIR/vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd"
echo "board dir : $BOARD_DIR"
echo "openvela  : $OPENVELA_DIR"
echo "dest      : $DEST"

# 如果整目录本身是个软链，先拆掉（这是会破坏 ../drivers 的那个错误形态）
if [ -L "$DEST" ]; then
  echo "注意：$DEST 原本是整目录软链，先删除"
  rm -f "$DEST"
fi

mkdir -p "$DEST"

for e in CMakeLists.txt Kconfig README.md README_zh-cn.md configs include scripts src; do
  if [ -e "$BOARD_DIR/$e" ]; then
    ln -sfn "$BOARD_DIR/$e" "$DEST/$e"
    echo "  link $e"
  else
    echo "  !! 缺失 $e（跳过）" >&2
  fi
done

echo
echo "检查芯片驱动软链是否还能解析："
echo "  $(readlink "$OPENVELA_DIR/nuttx/drivers/platform" 2>/dev/null || echo '(无)')"
if [ -r "$OPENVELA_DIR/nuttx/drivers/platform/input/ft6146.c" ] || \
   [ -r "$OPENVELA_DIR/vendor/sifli/boards/sf32lb52/drivers/input/ft6146.c" ]; then
  echo "  OK"
else
  echo "  !! drivers/platform 解析不到，构建会失败" >&2
  exit 1
fi
