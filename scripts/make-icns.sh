#!/bin/zsh
# PNG (1024 px) -> .icns with every size macOS wants.
#   scripts/make-icns.sh <icon-1024.png> <out.icns>
set -euo pipefail
src=$1; out=$2
set=$(mktemp -d)/AppIcon.iconset
mkdir -p "$set"
for s in 16 32 128 256 512; do
    sips -z $s $s "$src" --out "$set/icon_${s}x${s}.png" >/dev/null
    sips -z $((s*2)) $((s*2)) "$src" --out "$set/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$set" -o "$out"
