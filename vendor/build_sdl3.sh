#!/bin/bash
# build_sdl3.sh - Build SDL3 for sink, for the macOS version sink supports.
#
# SDL3 used to come from Homebrew. Homebrew builds for the macOS it's running
# on, so after the build machine moved to macOS 27, sink 0.11 shipped an SDL3
# whose minimum was 27.0 inside an app that promises 26.0. Building it here
# picks up MACOSX_DEPLOYMENT_TARGET from sdk_env.sh like every other vendored
# library, and releases/package.sh refuses a bundle with anything newer.
#
# SDL3 is also what decodes kitty-protocol PNGs from terminal output (see
# build_sdl3_image.sh), so keep it current.
#
# Output: vendor/sdl3/{include,lib}. Picked up by CMakeLists.txt, and by the
# SDL3_image and SDL3_ttf builds, which must link this one rather than
# Homebrew's.

set -euo pipefail

SDL_VERSION="3.4.18"
# Checked against Sam Lantinga's signature (SDL3-3.4.18.tar.gz.sig, keys
# 1528635D8053A57F77D1E08630A59377A7763BE6 and
# 0900104363B4C9D4223DE149D913FE7D4B61D39B) and the digest GitHub lists for
# the release asset.
SDL_SHA256="9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${SCRIPT_DIR}/sdl3"
CACHE_DIR="${SCRIPT_DIR}/.cache"
BUILD_DIR="${SCRIPT_DIR}/.build"
TARBALL="${CACHE_DIR}/SDL3-${SDL_VERSION}.tar.gz"
SRC_DIR="${BUILD_DIR}/SDL3-${SDL_VERSION}"

if [ "${1:-}" = "--clean" ]; then
    echo "removing ${PREFIX}..."
    rm -rf "${PREFIX}" "${SRC_DIR}"
    exit 0
fi

STAMP="${PREFIX}/.sink-sdl3-stamp"
STAMP_VALUE="${SDL_VERSION}-${MACOSX_DEPLOYMENT_TARGET:-unset}-$(shasum -a 256 "$0" | cut -c1-12)"
if [ -f "${STAMP}" ] && [ "$(cat "${STAMP}")" = "${STAMP_VALUE}" ]; then
    echo "vendored SDL3 ${SDL_VERSION} already built at ${PREFIX}"
    exit 0
fi

if [ -z "${MACOSX_DEPLOYMENT_TARGET:-}" ]; then
    echo "ERROR: MACOSX_DEPLOYMENT_TARGET is not set. Run via ./build.sh, or"
    echo "       'source ./sdk_env.sh' first, so SDL3 targets the macOS sink supports."
    exit 1
fi

mkdir -p "${CACHE_DIR}" "${BUILD_DIR}"
if [ ! -f "${TARBALL}" ]; then
    echo "downloading SDL3 ${SDL_VERSION}..."
    curl -fL --retry 3 -o "${TARBALL}.tmp" \
        "https://github.com/libsdl-org/SDL/releases/download/release-${SDL_VERSION}/SDL3-${SDL_VERSION}.tar.gz"
    mv "${TARBALL}.tmp" "${TARBALL}"
fi
echo "verifying checksum..."
ACTUAL="$(shasum -a 256 "${TARBALL}" | awk '{print $1}')"
if [ "${ACTUAL}" != "${SDL_SHA256}" ]; then
    echo "ERROR: checksum mismatch for ${TARBALL}"
    echo "  expected ${SDL_SHA256}"
    echo "  actual   ${ACTUAL}"
    exit 1
fi

echo "extracting..."
rm -rf "${SRC_DIR}"
tar -xzf "${TARBALL}" -C "${BUILD_DIR}"

echo "configuring SDL3 for macOS ${MACOSX_DEPLOYMENT_TARGET}..."
cmake -S "${SRC_DIR}" -B "${SRC_DIR}/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET}" \
    -DSDL_SHARED=ON \
    -DSDL_STATIC=OFF \
    -DSDL_TEST_LIBRARY=OFF \
    -DSDL_TESTS=OFF \
    -DSDL_EXAMPLES=OFF

echo "compiling SDL3..."
cmake --build "${SRC_DIR}/build" -j"$(sysctl -n hw.ncpu)"

echo "installing to ${PREFIX}..."
rm -rf "${PREFIX}"
cmake --install "${SRC_DIR}/build"
cp "${SRC_DIR}/LICENSE.txt" "${PREFIX}/LICENSE.txt"

LIB="${PREFIX}/lib/libSDL3.0.dylib"
MINOS="$(otool -l "${LIB}" | awk '/LC_BUILD_VERSION/{f=1} f && /minos/{print $2; exit}')"
if [ "${MINOS}" != "${MACOSX_DEPLOYMENT_TARGET}" ]; then
    echo "ERROR: ${LIB} targets macOS ${MINOS}, not ${MACOSX_DEPLOYMENT_TARGET}"
    exit 1
fi

echo "${STAMP_VALUE}" > "${STAMP}"
echo
echo "vendored SDL3 ${SDL_VERSION} built at ${PREFIX} (macOS ${MINOS}+)"
