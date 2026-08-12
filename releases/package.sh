#!/bin/bash
# package.sh - Packages the sink terminal into a standalone macOS App Bundle (sink.app)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "${SCRIPT_DIR}/.."

echo "packaging sink.app..."

# 1. Compile the latest binary
echo "  compiling project..."
cmake -B build -S .
cmake --build build

# 2. Setup the App Bundle folders
APP_NAME="sink"
APP_DIR="${APP_NAME}.app"
CONTENTS_DIR="${APP_DIR}/Contents"
MACOS_DIR="${CONTENTS_DIR}/MacOS"
RESOURCES_DIR="${CONTENTS_DIR}/Resources"

echo "  creating bundle directory tree..."
rm -rf "${APP_DIR}"
mkdir -p "${MACOS_DIR}"
mkdir -p "${RESOURCES_DIR}"

# 3. Copy the compiled executable and assets
echo "  copying binary..."
cp build/sink "${MACOS_DIR}/${APP_NAME}"

echo "  copying app icon..."
if [ -f "icon.icns" ]; then
    cp icon.icns "${RESOURCES_DIR}/icon.icns"
else
    echo "  warning: icon.icns not found! app bundle will use default fallback icon."
fi

echo "  copying background video..."
if [ -f "sinkpool.mp4" ]; then
    cp sinkpool.mp4 "${RESOURCES_DIR}/sinkpool.mp4"
fi

echo "  copying default monaspace neon font..."
if [ -f "fonts/MonaspaceNeon-Regular.otf" ]; then
    cp fonts/MonaspaceNeon-Regular.otf "${RESOURCES_DIR}/MonaspaceNeon-Regular.otf"
fi

echo "  copying mona sans font (settings UI)..."
if [ -f "fonts/MonaSans-VariableFont.ttf" ]; then
    cp "fonts/MonaSans-VariableFont.ttf" "${RESOURCES_DIR}/MonaSans-VariableFont.ttf"
fi

echo "  copying logo assets..."
if [ -f "logos/sinklogo.svg" ]; then
    cp logos/sinklogo.svg "${RESOURCES_DIR}/sinklogo.svg"
fi
if [ -f "logos/rainlogo.svg" ]; then
    cp logos/rainlogo.svg "${RESOURCES_DIR}/rainlogo.svg"
fi

echo "  copying chunked binary demo archive..."
mkdir -p "${RESOURCES_DIR}/demo"
if [ -f "demo/splash.dat" ]; then
    cp demo/splash.dat "${RESOURCES_DIR}/demo/splash.dat"
fi

# 4. Generate the Info.plist file
echo "  generating Info.plist..."
cat <<EOF > "${CONTENTS_DIR}/Info.plist"
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>English</string>
    <key>CFBundleExecutable</key>
    <string>${APP_NAME}</string>
    <key>CFBundleIconFile</key>
    <string>icon.icns</string>
    <key>CFBundleIdentifier</key>
    <string>com.rainmultimedia.${APP_NAME}</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>${APP_NAME}</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>0.8</string>
    <key>CFBundleSignature</key>
    <string>????</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSHumanReadableCopyright</key>
    <string>copyright © 2026 rain multimedia. all rights reserved.</string>
</dict>
</plist>
EOF

echo "successfully packaged ${APP_DIR}!"
echo "you can now launch it by running: open ${APP_DIR}"
