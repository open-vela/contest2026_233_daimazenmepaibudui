#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
APP_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
BUILD_DIR=${TMPDIR:-/tmp}/hello_app_member2_tests

mkdir -p "$BUILD_DIR/nuttx"
printf '%s\n' '#define OK 0' > "$BUILD_DIR/nuttx/config.h"

${CC:-cc} -std=gnu11 -Wall -Wextra -Werror \
  -I"$BUILD_DIR" -I"$APP_DIR" \
  "$SCRIPT_DIR/test_member2.c" \
  "$APP_DIR/ai_state_machine.c" \
  "$APP_DIR/ai_llm.c" \
  "$APP_DIR/ai_sound_detect.c" \
  "$APP_DIR/ai_care.c" \
  -pthread -lm -o "$BUILD_DIR/test_member2"

"$BUILD_DIR/test_member2"
