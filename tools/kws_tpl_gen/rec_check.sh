#!/usr/bin/env bash
# 录音自检 —— 录 2 秒，当场告诉你「麦到底有没有声音」。
#
# 为什么要有它（2026-10-30 踩的坑）：用户照着录了 4 秒「你好，openvela」，
# 格式全对、时长全对，但**整段是纯静音**（峰值 1/32768）。而 ALSA 那边
# Capture 是 100% 未静音 —— 问题在更上游：VMware 的虚拟声卡压根没接麦克风，
# guest 里怎么调都没用。这种事不先量一下，就会"重录一次还是空的"。
#
# 用法：./rec_check.sh          # 录 2 秒量电平
#       ./rec_check.sh 5        # 录 5 秒
#
# 判据：
#   峰值 ≥ 3000（约 9%）  → ✓ 能录，可以去跑 kws_tpl_gen 了
#   峰值 1~300            → ✗ 只有量化噪声，麦没接上（见下面的排查）
#   峰值 < 1              → ✗ 全零，录音通路根本没动
#
# ⚠ 这只验"有没有声音"，不验"念得对不对"。

set -u
SECS="${1:-2}"
OUT="$(mktemp -u /tmp/rec_check_XXXXXX.wav)"

echo "录 ${SECS} 秒……（对着麦克风正常说话就行）"
arecord -q -f S16_LE -r 16000 -c 1 -d "$SECS" "$OUT" || {
  echo "★ arecord 跑不起来"; exit 1;
}

python3 - "$OUT" <<'PY'
import sys, wave, array
w = wave.open(sys.argv[1])
a = array.array('h'); a.frombytes(w.readframes(w.getnframes()))
pk = max(max(a), -min(a)) if len(a) else 0
nz = sum(1 for x in a if abs(x) > 300)
print(f"  {w.getframerate()}Hz / {w.getnchannels()}声道 / {w.getsampwidth()*8}bit / "
      f"{len(a)/w.getframerate():.2f} 秒")
print(f"  峰值 {pk} /32768 = {pk/32768*100:.1f}%    "
      f"超过 300 的采样点 {nz} 个 = {nz/max(1,len(a))*100:.1f}%")
print()
if pk >= 3000:
    print(f"★ ✓ 有声音（峰值 {pk}），可以录正式的了。")
    sys.exit(0)
if pk <= 1:
    print("★ ✗ 全是零 —— 录音通路根本没动。")
else:
    print(f"★ ✗ 只有本底噪声（峰值 {pk}）—— 麦克风没接上，或接到了没声音的源。")
print()
print("  1) 虚拟机的声卡有没有把麦克风接进来：")
print("     · VMware：虚拟机 → 设置 → 声卡 → 「已连接」「启动时连接」都勾上，")
print("       设备选宿主机的麦克风；改完**重启虚拟机**。")
print("     · 先在宿主机上确认麦克风本身有声音（Windows：设置→系统→声音→输入→测试）。")
print("  2) ALSA 这边：amixer  看 Capture 是不是 100% [on]（2026-10-30 实测是好的）。")
print("  3) 都调不好就**用手机录**，传文件过来 —— 绕开虚拟机最省事。")
sys.exit(2)
PY
rc=$?
echo
echo "（样本留在 $OUT，不满意可以自己听）"
exit $rc
