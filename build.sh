#!/bin/bash
# build.sh - Configure and compile the sink project

# Exit on any error
set -e

echo "🔨 Setting up and compiling sink..."

# 0. Choose the SDK and deployment target (see sdk_env.sh for why this is not
# just whatever xcrun reports). Exports SDKROOT and MACOSX_DEPLOYMENT_TARGET,
# which the vendored builds below inherit.
source ./sdk_env.sh

# 1. Build the vendored LGPL decode-only FFmpeg if it isn't there yet.
# Without this, CMake falls back to Homebrew's ffmpeg, which is GPLv3 and must
# not be shipped. No-op once built (see the stamp check in the script).
echo "  checking vendored ffmpeg..."
./vendor/build_ffmpeg.sh

echo "  checking vendored SDL3_image..."
./vendor/build_sdl3_image.sh

echo "  checking vendored text stack (harfbuzz + SDL3_ttf)..."
./vendor/build_text_stack.sh

# 2. Configure. This runs every time rather than only when build/ is missing.
# CMake is a no-op when nothing has changed, and skipping it meant an existing
# build/ kept whatever sysroot it was first configured with -- so the one case
# that most needs reconfiguring, a toolchain that moved under an existing build
# directory, was the one case that silently did not get it.
echo "  configuring CMake..."
cmake -B build -DCMAKE_OSX_SYSROOT="${SDKROOT}"

# 3. Compile the project
echo "  compiling binary..."
cmake --build build

echo "✅ Build complete!"
echo "👉 Run ./releases/package.sh to bundle sink.app, or run ./build/sink directly."
