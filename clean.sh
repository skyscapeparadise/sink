#!/bin/bash
# clean.sh - Clean build artifacts for the sink project

echo "cleaning build artifacts..."

# Remove CMake build directory
if [ -d "build" ]; then
    rm -rf build
    echo "  removed build/ directory."
fi

# Remove macOS system metadata files
find . -name ".DS_Store" -type f -delete
echo "  removed all .DS_Store files."

echo "clean complete!"
