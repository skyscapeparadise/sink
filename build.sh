#!/bin/bash
# build.sh - Configure and compile the sink project

# Exit on any error
set -e

echo "🔨 Setting up and compiling sink..."

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
