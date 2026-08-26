#!/usr/bin/env bash
# ============================================================
#  Openvela flash helper for SF32LB52-DevKit-LCD (Linux)
#
#  KEY: the SFBL bootrom REQUIRES the partition table (ftab)
#       at 0x12000000, otherwise it will not boot the image.
#
#  Usage:  ./flash_openvela.sh [nuttx.bin] [serial-device]
#          (defaults: ./nuttx.bin, /dev/ttyUSB0)
# ============================================================
set -e
cd "$(dirname "$0")"

IMG="${1:-nuttx.bin}"
DEV="${2:-/dev/ttyUSB0}"

if [ ! -f "$IMG" ]; then
    echo "ERROR: firmware image not found: $IMG"
    echo "Put nuttx.bin here or pass it as the first argument."
    exit 1
fi

if ! command -v sftool >/dev/null 2>&1; then
    echo "ERROR: sftool not found. Install from"
    echo "  https://github.com/OpenSiFli/sftool/releases  (0.2.5 recommended)"
    exit 1
fi

sftool -p "$DEV" -c SF32LB52 -m nor --before default_reset --after soft_reset \
    write_flash "ftab.bin@0x12000000" "$IMG@0x12010000"

echo "Done. Open serial at 1000000 8N1, expect: SFBL / ABCD / NuttShell (NSH)"
