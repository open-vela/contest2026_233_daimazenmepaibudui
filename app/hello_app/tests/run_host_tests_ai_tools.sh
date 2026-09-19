#!/bin/sh
#
# run_host_tests_ai_tools.sh —— ai_tools_provider.c（工具 provider）的 host 测试
#
# 为什么要单独一个脚本（而不是塞进 run_host_tests.sh）：那一份是模板工程留下的
# member2 测试，和这份的 fake 头文件（假 nuttx/config.h、假 tools/tool_registry.h）
# 不共用一套 -I；分成两份互不影响，这一份自己就能跑。
#
# 编两遍，两颗二进制：
#   1)  有 ui_post_ask_alarm 的强符号  -> 走"对面已落地"那条分支（弹框 + 回文本）
#   2) -DNO_ASK_STUB                  -> 走"对面还没落地"那条分支（weak 判空）
#
# 用法：sh app/hello_app/tests/run_host_tests_ai_tools.sh
#
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
APP_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
STUBS="$SCRIPT_DIR/ai_tools_stubs"
BUILD_DIR=${TMPDIR:-/tmp}/hello_app_ai_tools_tests

mkdir -p "$BUILD_DIR"

CC=${CC:-cc}
# -I$STUBS 必须在最前面：它提供假的 <nuttx/config.h> 和 tools/tool_registry.h
CFLAGS="-std=gnu11 -Wall -Wextra -Werror -I$STUBS -I$APP_DIR"

echo "== 编译（对面已落地：ui_post_ask_alarm 有强符号） =="
$CC $CFLAGS \
  "$SCRIPT_DIR/test_ai_tools_provider.c" \
  "$STUBS/stub_framework.c" \
  "$APP_DIR/ai_tools_provider.c" \
  -o "$BUILD_DIR/test_ai_tools_provider"

echo "== 编译（对面没落地：-DNO_ASK_STUB） =="
$CC $CFLAGS -DNO_ASK_STUB \
  "$SCRIPT_DIR/test_ai_tools_provider.c" \
  "$STUBS/stub_framework.c" \
  "$APP_DIR/ai_tools_provider.c" \
  -o "$BUILD_DIR/test_ai_tools_provider_noask"

echo "== 运行 1/2 =="
"$BUILD_DIR/test_ai_tools_provider"

echo "== 运行 2/2 =="
"$BUILD_DIR/test_ai_tools_provider_noask"

echo "全部通过"
