#!/bin/bash
# build_sdl3_image.sh - Build a minimal SDL3_image for sink.
#
# sink calls exactly three SDL3_image functions (src/settings_ui.cpp), all of
# them loading the two bundled SVG logos. Still-image backgrounds do NOT go
# through here -- video_path is handed to VideoEngine, so ffmpeg's png/mjpeg
# decoders and image2 demuxer handle those.
#
# Homebrew's SDL3_image enables every format, dragging ~12MB of AVIF/JXL/TIFF/
# WebP decoders and their transitive deps (libaom, libhwy, brotli, lcms2,
# lzma, zstd, ...) into the app bundle to render two vector logos.
#
# This build enables SVG (vendored nanosvg, no external deps) plus the
# header-only formats that cost nothing, and uses SDL_image's built-in stb
# backend for PNG/JPEG so libpng/libjpeg-turbo aren't needed either. Result:
# libSDL3_image with zero non-system dependencies beyond SDL3 itself.
#
# Output: vendor/sdl3_image/{include,lib}. Picked up by CMakeLists.txt.

set -euo pipefail

SDLIMAGE_VERSION="3.4.4"
SDLIMAGE_SHA256="29751304a13d25ac513f24305fa25b06a6edd9607718c90129b8350d35fc5573"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${SCRIPT_DIR}/sdl3_image"
CACHE_DIR="${SCRIPT_DIR}/.cache"
BUILD_DIR="${SCRIPT_DIR}/.build"
TARBALL="${CACHE_DIR}/SDL3_image-${SDLIMAGE_VERSION}.tar.gz"
SRC_DIR="${BUILD_DIR}/SDL3_image-${SDLIMAGE_VERSION}"

if [ "${1:-}" = "--clean" ]; then
    echo "removing ${PREFIX}..."
    rm -rf "${PREFIX}" "${SRC_DIR}"
    exit 0
fi

STAMP="${PREFIX}/.sink-sdl3-image-stamp"
STAMP_VALUE="${SDLIMAGE_VERSION}-$(shasum -a 256 "$0" | cut -c1-12)"
if [ -f "${STAMP}" ] && [ "$(cat "${STAMP}")" = "${STAMP_VALUE}" ]; then
    echo "vendored SDL3_image ${SDLIMAGE_VERSION} already built at ${PREFIX}"
    echo "(run '$0 --clean' to force a rebuild)"
    exit 0
fi

mkdir -p "${CACHE_DIR}" "${BUILD_DIR}"

if [ ! -f "${TARBALL}" ]; then
    echo "downloading SDL3_image ${SDLIMAGE_VERSION}..."
    curl -fL --retry 3 -o "${TARBALL}.tmp" \
        "https://github.com/libsdl-org/SDL_image/releases/download/release-${SDLIMAGE_VERSION}/SDL3_image-${SDLIMAGE_VERSION}.tar.gz"
    mv "${TARBALL}.tmp" "${TARBALL}"
fi

echo "verifying checksum..."
ACTUAL="$(shasum -a 256 "${TARBALL}" | awk '{print $1}')"
if [ "${ACTUAL}" != "${SDLIMAGE_SHA256}" ]; then
    echo "ERROR: checksum mismatch for ${TARBALL}"
    echo "  expected ${SDLIMAGE_SHA256}"
    echo "  actual   ${ACTUAL}"
    exit 1
fi

echo "extracting..."
rm -rf "${SRC_DIR}"
tar -xzf "${TARBALL}" -C "${BUILD_DIR}"

# The four formats below are the only ones with external dependencies, and
# each pulls in a subtree: AVIF -> libavif, libaom, libdav1d; JXL -> libjxl,
# libhwy, brotli, lcms2; TIF -> libtiff, liblzma, libzstd; WEBP -> libwebp,
# libsharpyuv. All off. BACKEND_STB=ON keeps PNG/JPG working via SDL_image's
# bundled stb_image rather than linking libpng/libjpeg-turbo.
echo "configuring (SVG + dependency-free formats only)..."
cmake -S "${SRC_DIR}" -B "${SRC_DIR}/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DBUILD_SHARED_LIBS=ON \
    -DSDLIMAGE_SAMPLES=OFF \
    -DSDLIMAGE_TESTS=OFF \
    -DSDLIMAGE_VENDORED=OFF \
    -DSDLIMAGE_BACKEND_STB=ON \
    -DSDLIMAGE_AVIF=OFF \
    -DSDLIMAGE_JXL=OFF \
    -DSDLIMAGE_TIF=OFF \
    -DSDLIMAGE_WEBP=OFF \
    -DSDLIMAGE_SVG=ON \
    -DSDLIMAGE_PNG=ON \
    -DSDLIMAGE_JPG=ON

echo "compiling..."
cmake --build "${SRC_DIR}/build" -j"$(sysctl -n hw.ncpu)"

echo "installing to ${PREFIX}..."
rm -rf "${PREFIX}"
cmake --install "${SRC_DIR}/build"

cp "${SRC_DIR}/LICENSE.txt" "${PREFIX}/LICENSE.txt" 2>/dev/null || true
echo "${STAMP_VALUE}" > "${STAMP}"

# The whole point is dropping the dependency subtree, so fail if any of it
# survived rather than discovering it in the app bundle later.
echo
echo "verifying no heavy image dependencies leaked in..."
LEAKED="$(otool -L "${PREFIX}/lib/libSDL3_image.0.dylib" \
    | grep -oE "lib(avif|aom|jxl|hwy|brotli[a-z]*|lcms2|tiff|lzma|zstd|webp[a-z]*|sharpyuv|png16|jpeg)\.[0-9.]*dylib" \
    | sort -u || true)"
if [ -n "${LEAKED}" ]; then
    echo "ERROR: libSDL3_image still links:"
    echo "${LEAKED}" | sed 's/^/    /'
    exit 1
fi
echo "  clean -- links only SDL3 and system libraries"

echo
echo "vendored SDL3_image ${SDLIMAGE_VERSION} built at ${PREFIX}"
du -sh "${PREFIX}/lib"
