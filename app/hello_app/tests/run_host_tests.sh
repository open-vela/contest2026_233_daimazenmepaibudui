#!/bin/sh
#
# run_host_tests.sh —— hello_app 的 host 测试（member2 那几个模块）
#
# 在 PC 上直接编 app/hello_app 的 ai_state_machine / ai_llm / ai_sound_detect /
# ai_care 并跑 test_member2.c：**不需要板子、不需要 NuttX 构建**。
#
# 假头文件在 host_stubs/：那几个 .c 引了 NuttX 的头（<nuttx/config.h>、
# <nuttx/wdog.h>、<nuttx/semaphore.h>），PC 上编没有它们 —— 这里补一份最小的。
# 以后哪个 .c 新引了某个 NuttX 头，就往 host_stubs/nuttx/ 里补一份假头，
# 别改被测源码。这份 -I 只作用于本脚本；真机固件编的是 NuttX 真的那些头，
# 两边互不影响（道理和 ai_tools_stubs/ 一样）。
#
# member2 那几个模块走本脚本；ai_tools_provider 那份走
# run_host_tests_ai_tools.sh（它自带一套 ai_tools_stubs/）。
#
# 用法：sh app/hello_app/tests/run_host_tests.sh
#
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
APP_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
STUBS="$SCRIPT_DIR/host_stubs"
BUILD_DIR=${TMPDIR:-/tmp}/hello_app_member2_tests

mkdir -p "$BUILD_DIR"

CC=${CC:-cc}
# -I$STUBS 必须在最前面：它提供假的 <nuttx/config.h> 等（理由见文件头）
CFLAGS="-std=gnu11 -Wall -Wextra -Werror -I$STUBS -I$APP_DIR"

$CC $CFLAGS \
  "$SCRIPT_DIR/test_member2.c" \
  "$APP_DIR/ai_state_machine.c" \
  "$APP_DIR/ai_llm.c" \
  "$APP_DIR/ai_sound_detect.c" \
  "$APP_DIR/ai_care.c" \
  -pthread -lm -o "$BUILD_DIR/test_member2"

"$BUILD_DIR/test_member2"
