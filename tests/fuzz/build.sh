#!/bin/bash
# Builds sink's fuzzers into build-fuzz/. See README.md.
#
# Needs Homebrew's LLVM: Xcode's clang has AddressSanitizer and
# UndefinedBehaviorSanitizer but not libFuzzer. Writes only to build-fuzz/.
set -euo pipefail
cd "$(dirname "$0")/../.."

LLVM=/opt/homebrew/opt/llvm
if [ ! -x "$LLVM/bin/clang++" ]; then
    echo "Homebrew LLVM not found ($LLVM). Install it with: brew install llvm" >&2
    exit 1
fi
source ./sdk_env.sh >/dev/null

VERSION="$(sed -nE 's/^project\(SinkTerminal VERSION ([0-9.]+).*/\1/p' CMakeLists.txt)"
IFS=. read -r MAJ MIN PAT <<< "$VERSION"
VERSION_NUM=$((MAJ * 10000 + MIN * 100 + PAT))

OUT=build-fuzz
mkdir -p "$OUT"
CXXFLAGS=(
    -std=gnu++20 -g -O1 -fno-omit-frame-pointer
    -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined
    -isysroot "$SDKROOT" -mmacosx-version-min="$MACOSX_DEPLOYMENT_TARGET"
    "-DSINK_VERSION=\"$VERSION\"" "-DSINK_VERSION_NUM=$VERSION_NUM"
    -Isrc -I/opt/homebrew/include
    -isystem vendor/textstack/include -isystem vendor/sdl3_image/include
)
# Homebrew's clang compiles against its own libc++ headers, so it links its
# own libc++ too; the system one may lack symbols the newer headers expect.
LDFLAGS=(
    -L"$LLVM/lib/c++" -Wl,-rpath,"$LLVM/lib/c++"
    -L"$LLVM/lib/unwind" -lunwind
    -Wl,-rpath,"$PWD/vendor/textstack/lib" -Wl,-rpath,"$PWD/vendor/sdl3_image/lib"
    -Wl,-rpath,/opt/homebrew/lib
    -lz
)
SDL_LIBS=(vendor/sdl3_image/lib/libSDL3_image.0.4.4.dylib /opt/homebrew/lib/libSDL3.0.dylib)
TTF_LIBS=(vendor/textstack/lib/libSDL3_ttf.0.2.2.dylib)

echo "building fuzz_terminal..."
"$LLVM/bin/clang++" "${CXXFLAGS[@]}" tests/fuzz/fuzz_terminal.cpp \
    src/ansi_parser.cpp src/terminal_grid.cpp src/terminal_images.cpp \
    src/prompt_editing.cpp src/font_manager.cpp \
    "${LDFLAGS[@]}" "${SDL_LIBS[@]}" "${TTF_LIBS[@]}" -o "$OUT/fuzz_terminal"

echo "building fuzz_images..."
"$LLVM/bin/clang++" "${CXXFLAGS[@]}" tests/fuzz/fuzz_images.cpp src/terminal_images.cpp \
    "${LDFLAGS[@]}" "${SDL_LIBS[@]}" -o "$OUT/fuzz_images"

echo "built: $OUT/fuzz_terminal $OUT/fuzz_images"
