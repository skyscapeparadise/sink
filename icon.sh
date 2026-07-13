#!/bin/sh
# make_icns.sh — Convert icon.png → icon.icns using Apple's iconutil.
# Usage: sh make_icns.sh

set -eu

SRC="icon.png"
ICONSET="icon.iconset"
OUT="icon.icns"

[ -f "$SRC" ] || { echo "✖ Missing $SRC"; exit 1; }

# Remove old iconset if needed
rm -rf "$ICONSET"
mkdir "$ICONSET"

# Generate each required size
sips -z 16 16     "$SRC" --out "$ICONSET/icon_16x16.png"
sips -z 32 32     "$SRC" --out "$ICONSET/icon_16x16@2x.png"
sips -z 32 32     "$SRC" --out "$ICONSET/icon_32x32.png"
sips -z 64 64     "$SRC" --out "$ICONSET/icon_32x32@2x.png"
sips -z 128 128   "$SRC" --out "$ICONSET/icon_128x128.png"
sips -z 256 256   "$SRC" --out "$ICONSET/icon_128x128@2x.png"
sips -z 256 256   "$SRC" --out "$ICONSET/icon_256x256.png"
sips -z 512 512   "$SRC" --out "$ICONSET/icon_256x256@2x.png"
sips -z 512 512   "$SRC" --out "$ICONSET/icon_512x512.png"

# The @2x 512 size uses full-res source (no downscale)
cp "$SRC" "$ICONSET/icon_512x512@2x.png"

# Build the .icns
iconutil -c icns "$ICONSET" -o "$OUT"

echo "✅ Created $OUT"