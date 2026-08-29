#!/bin/bash
# build.sh - Configure and compile the sink project

# Exit on any error
set -e

echo "🔨 Setting up and compiling sink..."

# 0. Build the vendored LGPL decode-only FFmpeg if it isn't there yet.
# Without this, CMake falls back to Homebrew's ffmpeg, which is GPLv3 and must
# not be shipped. No-op once built (see the stamp check in the script).
echo "  checking vendored ffmpeg..."
./vendor/build_ffmpeg.sh

echo "  checking vendored SDL3_image..."
./vendor/build_sdl3_image.sh

echo "  checking vendored text stack (harfbuzz + SDL3_ttf)..."
./vendor/build_text_stack.sh

# 1. Configure the build directory if it doesn't exist
if [ ! -d "build" ]; then
    echo "  build/ directory not found. Configuring CMake..."
    cmake -B build
else
    echo "  build/ directory found. Ready to compile."
fi

# 2. Compile the project
echo "  compiling binary..."
cmake --build build

echo "✅ Build complete!"
echo "👉 Run ./releases/package.sh to bundle sink.app, or run ./build/sink directly."
