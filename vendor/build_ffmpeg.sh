#!/bin/bash
# build_ffmpeg.sh - Build a minimal, LGPL, decode-only ffmpeg for sink.
#
# Why vendor instead of using Homebrew's ffmpeg:
#   * License. Homebrew builds with --enable-gpl --enable-version3 (it wants
#     x264/x265), so its libraries report "GPL version 3 or later". Bundling
#     those into sink.app would relicense the whole app as GPLv3, which
#     conflicts with sink's LGPL-2.1. This build is LGPL-2.1.
#   * Size. Homebrew's ffmpeg tree costs ~33MB in the app bundle, most of it
#     encoders (x265 alone is 7MB) that a terminal background player never
#     calls. sink only ever decodes.
#
# Output: vendor/ffmpeg/{include,lib}. CMakeLists.txt picks this up
# automatically via PKG_CONFIG_PATH.

set -euo pipefail

FFMPEG_VERSION="8.1.2"
FFMPEG_SHA256="464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${SCRIPT_DIR}/ffmpeg"
CACHE_DIR="${SCRIPT_DIR}/.cache"
BUILD_DIR="${SCRIPT_DIR}/.build"
TARBALL="${CACHE_DIR}/ffmpeg-${FFMPEG_VERSION}.tar.xz"
SRC_DIR="${BUILD_DIR}/ffmpeg-${FFMPEG_VERSION}"

if [ "${1:-}" = "--clean" ]; then
    echo "removing ${PREFIX} and ${BUILD_DIR}..."
    rm -rf "${PREFIX}" "${BUILD_DIR}"
    exit 0
fi

# Already built? The stamp records the version so a version bump rebuilds.
# Keyed on the script's own hash as well as the version, so editing the
# configure flags below forces a rebuild instead of silently reusing the old one.
STAMP="${PREFIX}/.sink-ffmpeg-stamp"
STAMP_VALUE="${FFMPEG_VERSION}-$(shasum -a 256 "$0" | cut -c1-12)"
if [ -f "${STAMP}" ] && [ "$(cat "${STAMP}")" = "${STAMP_VALUE}" ]; then
    echo "vendored ffmpeg ${FFMPEG_VERSION} already built at ${PREFIX}"
    echo "(run '$0 --clean' to force a rebuild)"
    exit 0
fi

mkdir -p "${CACHE_DIR}" "${BUILD_DIR}"

if [ ! -f "${TARBALL}" ]; then
    echo "downloading ffmpeg ${FFMPEG_VERSION}..."
    curl -fL --retry 3 -o "${TARBALL}.tmp" \
        "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz"
    mv "${TARBALL}.tmp" "${TARBALL}"
fi

echo "verifying checksum..."
ACTUAL="$(shasum -a 256 "${TARBALL}" | awk '{print $1}')"
if [ "${ACTUAL}" != "${FFMPEG_SHA256}" ]; then
    echo "ERROR: checksum mismatch for ${TARBALL}"
    echo "  expected ${FFMPEG_SHA256}"
    echo "  actual   ${ACTUAL}"
    exit 1
fi

echo "extracting..."
rm -rf "${SRC_DIR}"
tar -xf "${TARBALL}" -C "${BUILD_DIR}"

cd "${SRC_DIR}"

# --disable-gpl AND --disable-version3: with version3 alone the result is
# LGPLv3, which still isn't compatible with sink's LGPL-2.1. Both off gives
# LGPL-2.1, verified by the avcodec_license() check at the end of this script.
#
# No --enable-lib* at all. Every external codec library Homebrew links (x264,
# x265, svt-av1, dav1d, vpx, opus, lame, openssl) is dropped: they're all
# encoders, alternative decoders for codecs ffmpeg decodes natively, or TLS.
# That takes the ffmpeg dylibs in the bundle from 14 to 5.
#
# --disable-everything plus an explicit decode-only allowlist. Enabling every
# built-in decoder instead costs ~7.7MB more for codecs nobody sets as a
# terminal background (VC-1/WMV, RealVideo, Cinepak, Indeo). The list below
# covers anything a user plausibly points video_path at. To widen it, add to
# --enable-decoder/parser/demuxer; the stamp hash above forces a rebuild.
#
# xlib/libxcb are [autodetect] and WILL turn themselves on if X11 headers are
# present on the build machine (Homebrew installs them as a transitive dep of
# plenty of things), silently adding libX11/libxcb/libXau/libXdmcp -- ~1.3MB of
# X11 -- to a macOS-only app bundle. Forced off.
#
# Note the bitstream filters: h264/hevc_mp4toannexb and extract_extradata are
# what let the VideoToolbox hwaccel path work on MP4-contained H.264/HEVC.
# Without them --disable-everything produces a build that opens files but
# decodes nothing.
echo "configuring (LGPL, decode-only)..."
./configure \
    --prefix="${PREFIX}" \
    --disable-gpl \
    --disable-version3 \
    --disable-nonfree \
    --enable-shared \
    --disable-static \
    --disable-programs \
    --disable-doc \
    --disable-avdevice \
    --disable-avfilter \
    --disable-devices \
    --disable-network \
    --disable-debug \
    --disable-xlib \
    --disable-libxcb \
    --disable-libxcb-shm \
    --disable-libxcb-xfixes \
    --disable-libxcb-shape \
    --enable-videotoolbox \
    --enable-pthreads \
    --disable-everything \
    --enable-decoder=h264,hevc,prores,mpeg4,mpeg2video,mpeg1video,mjpeg,vp8,vp9,av1,theora,rawvideo,qtrle,png \
    --enable-parser=h264,hevc,mpeg4video,mpegvideo,mjpeg,vp8,vp9,av1,png \
    --enable-demuxer=mov,matroska,avi,mpegts,mpegps,flv,image2 \
    --enable-protocol=file \
    --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,extract_extradata,vp9_superframe,av1_frame_split,prores_metadata

echo "compiling (this takes a few minutes)..."
make -j"$(sysctl -n hw.ncpu)"

echo "installing to ${PREFIX}..."
rm -rf "${PREFIX}"
make install

# Ship the license text next to the libraries; LGPL redistribution requires
# conveying it, and releases/package.sh copies it into the app bundle.
cp COPYING.LGPLv2.1 "${PREFIX}/COPYING.LGPLv2.1"
cat > "${PREFIX}/README.sink" <<EOF
FFmpeg ${FFMPEG_VERSION}, built for sink under LGPL-2.1.

Unmodified upstream source: https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz
sha256: ${FFMPEG_SHA256}

Built by vendor/build_ffmpeg.sh; see that script for the exact configure line.
These libraries are dynamically linked, so they can be replaced with a
modified build by substituting the dylibs in sink.app/Contents/Frameworks/.
EOF

echo "${STAMP_VALUE}" > "${STAMP}"

echo
echo "verifying the build is actually LGPL..."
cat > "${BUILD_DIR}/licchk.c" <<'EOF'
#include <stdio.h>
#include <string.h>
const char *avcodec_license(void);
const char *avformat_license(void);
const char *avutil_license(void);
const char *swscale_license(void);
int main(void) {
    const char *l[4] = { avcodec_license(), avformat_license(),
                         avutil_license(), swscale_license() };
    const char *n[4] = { "libavcodec", "libavformat", "libavutil", "libswscale" };
    int bad = 0;
    for (int i = 0; i < 4; i++) {
        printf("  %-12s %s\n", n[i], l[i]);
        if (strncmp(l[i], "LGPL version 2.1", 16) != 0) bad = 1;
    }
    return bad;
}
EOF
clang "${BUILD_DIR}/licchk.c" -o "${BUILD_DIR}/licchk" \
    -I"${PREFIX}/include" \
    "${PREFIX}/lib/libavcodec.dylib" "${PREFIX}/lib/libavformat.dylib" \
    "${PREFIX}/lib/libavutil.dylib" "${PREFIX}/lib/libswscale.dylib"
if ! "${BUILD_DIR}/licchk"; then
    echo "ERROR: vendored ffmpeg is not LGPL-2.1. Refusing to continue."
    exit 1
fi

echo
echo "verifying no X11 dependency leaked in..."
X11LEAK="$(otool -L "${PREFIX}/lib/libavcodec.62.dylib" "${PREFIX}/lib/libavutil.60.dylib" \
    | grep -oE "lib(X11|xcb|Xau|Xdmcp)\.[0-9.]*dylib" | sort -u || true)"
if [ -n "${X11LEAK}" ]; then
    echo "ERROR: vendored ffmpeg links X11:"
    echo "${X11LEAK}" | sed 's/^/    /'
    exit 1
fi
echo "  clean -- no X11"

echo
echo "vendored ffmpeg ${FFMPEG_VERSION} built at ${PREFIX}"
du -sh "${PREFIX}/lib"
