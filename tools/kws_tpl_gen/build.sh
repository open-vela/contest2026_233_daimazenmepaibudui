#!/usr/bin/env bash
# 编 PC 版唤醒词模板生成器。
#
# 关键点：**不复制、不改写 kws_dtw**，直接把 app/hello_app/kws_dtw.c 编进来 ——
# 板上跑的和 PC 上跑的是同一份特征提取代码，模板格式不可能对不上。
#
# 两处让它能脱离 NuttX 编译（都不动 kws_dtw 本身）：
#   1. -I 放在最前面的 ./nuttx/ 是个空壳 config.h —— kws_dtw.h 唯一的 NuttX
#      依赖，而那个头里没有一个宏被 kws_dtw.c 用到；
#   2. -DKWS_DATA_DIR='"kws_out"' 覆盖落盘目录成相对路径，工具靠 chdir 落到
#      你指定的输出目录（板上不带这个宏，展开结果和改动前逐字一样）。
#
# 用法：./build.sh

set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$HERE/../../app/hello_app"
CC="${CC:-cc}"

if [ ! -f "$APP/kws_dtw.c" ]; then
  echo "★ 找不到 $APP/kws_dtw.c —— 目录结构变了？"
  exit 1
fi

echo "编译：kws_tpl_gen（$CC）"
# -I "$HERE" 必须在前：它顶掉 <nuttx/config.h>，换成那个空壳。
# -I "$APP" 是给 #include "kws_dtw.h" 找头。
#
# ⚠ 用 gnu99 而不是 c99：kws_dtw.c 用了 struct timespec / clock_gettime
# （kws_selftest 量耗时）。纯 -std=c99 会把 POSIX 那层藏起来，报
# "storage size of 't0' isn't known"。这是**编译选项的事，不改 kws_dtw.c**
# —— 那份是要进固件的。
"$CC" -O2 -std=gnu99 -Wall \
  -I "$HERE" \
  -I "$APP" \
  -DKWS_DATA_DIR='"kws_out"' \
  -o "$HERE/kws_tpl_gen" \
  "$HERE/kws_tpl_gen.c" \
  "$APP/kws_dtw.c" \
  -lm

echo "好了：$HERE/kws_tpl_gen"
echo "试跑：$HERE/kws_tpl_gen --help"

# ---- kws_dist：量距离（标定阈值用）--------------------------------------
# 同一份 kws_dtw.c、同一个 -DKWS_DATA_DIR —— 量出来的距离和板子完全同口径。
# 单独一个可执行，因为用途不同（生成器是"录→落盘"，这个是"比对→报数"）。
echo "编译：kws_dist"
"$CC" -O2 -std=gnu99 -Wall \
  -I "$HERE" \
  -I "$APP" \
  -DKWS_DATA_DIR='"kws_out"' \
  -o "$HERE/kws_dist" \
  "$HERE/kws_dist.c" \
  "$APP/kws_dtw.c" \
  -lm

echo "好了：$HERE/kws_dist"
echo "试跑：$HERE/kws_dist --help"
