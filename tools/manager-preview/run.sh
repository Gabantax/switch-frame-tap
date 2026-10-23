#!/bin/bash
# The Switch app's screens on a PC: build tools/manager-preview, run it in a
# made-up SD card, write PNGs to OUT (default: tools/manager-preview/out).
# Needs SDL2 (dev headers), g++, python3 with PIL, and ref/libtesla (for
# stb_truetype.h - the overlay build needs it too). FONT picks the TTF.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT="${1:-$HERE/out}"
FONT="${FONT:-/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

g++ -std=c++20 -O1 -w -I"$HERE/stub" -I"$REPO/ref/libtesla/include" $(pkg-config --cflags sdl2) \
    "$HERE/preview.cpp" -o "$WORK/preview" $(pkg-config --libs sdl2)

# the SD card of a full install ("sdmc:/..." is a relative path on a PC)
SD="$WORK/sdmc:"
mkdir -p "$SD/atmosphere/contents/0100000000000C20" "$SD/atmosphere/contents/0100000000000C21/flags" \
         "$SD/switch/.overlays" "$SD/config/switch-frame-tap/dvr-patches" "$SD/switch/switch-frame-tap/screenshots"
touch "$SD/atmosphere/contents/0100000000000C20/exefs.nsp" "$SD/atmosphere/contents/0100000000000C21/exefs.nsp" \
      "$SD/atmosphere/contents/0100000000000C21/flags/boot2.flag" "$SD/switch/.overlays/switch-frame-tap.ovl"
printf 'connection = network\nhome_menu = 1\noverlays = 1\nscreenshot = 1\nquality = medium\n' > "$SD/config/switch-frame-tap/config.ini"
printf 'result=started system memory 3482 KB\nothers=sys-clk 1024 KB; MissionControl 812 KB\n' > "$SD/config/switch-frame-tap/launcher.txt"

(cd "$WORK" && ./preview "$OUT" "$FONT" >/dev/null)
python3 - "$OUT" <<'EOF'
import glob, os, sys
from PIL import Image
for b in sorted(glob.glob(os.path.join(sys.argv[1], '*.bmp'))):
    Image.open(b).convert('RGB').save(b[:-4] + '.png')
    os.remove(b)
EOF
ls "$OUT"/*.png
