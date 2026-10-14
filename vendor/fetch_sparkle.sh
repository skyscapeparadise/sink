#!/bin/bash
# fetch_sparkle.sh - Fetch the prebuilt Sparkle framework sink updates itself with.
#
# Sparkle checks an appcast attached to sink's latest GitHub release, verifies
# the EdDSA signature on the feed and on the DMG it points at, and swaps the
# running app for the new one. Nothing is compiled here: Sparkle ships a
# framework already built for both architectures, plus the command-line tools
# (sign_update, generate_keys) that releases/publish_update.sh signs with.
#
# The framework is only ever loaded from a packaged sink.app (see
# setup_updater() in src/macos_menu.mm), so build.sh does not need this --
# releases/package.sh runs it.
#
# Output: vendor/sparkle/{Sparkle.framework,bin,LICENSE}.

set -euo pipefail

SPARKLE_VERSION="2.10.0"
SPARKLE_SHA256="c2bf58aa8387266ac179357b1415d6f2635f044da8be41042af32425dae6da0c"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${SCRIPT_DIR}/sparkle"
CACHE_DIR="${SCRIPT_DIR}/.cache"
BUILD_DIR="${SCRIPT_DIR}/.build"
TARBALL="${CACHE_DIR}/Sparkle-${SPARKLE_VERSION}.tar.xz"
SRC_DIR="${BUILD_DIR}/Sparkle-${SPARKLE_VERSION}"

if [ "${1:-}" = "--clean" ]; then
    echo "removing ${PREFIX}..."
    rm -rf "${PREFIX}" "${SRC_DIR}"
    exit 0
fi

STAMP="${PREFIX}/.sink-sparkle-stamp"
STAMP_VALUE="${SPARKLE_VERSION}-$(shasum -a 256 "$0" | cut -c1-12)"
if [ -f "${STAMP}" ] && [ "$(cat "${STAMP}")" = "${STAMP_VALUE}" ]; then
    echo "vendored Sparkle ${SPARKLE_VERSION} already at ${PREFIX}"
    echo "(run '$0 --clean' to force a fresh copy)"
    exit 0
fi

mkdir -p "${CACHE_DIR}" "${BUILD_DIR}"

if [ ! -f "${TARBALL}" ]; then
    echo "downloading Sparkle ${SPARKLE_VERSION}..."
    curl -fL --retry 3 -o "${TARBALL}.tmp" \
        "https://github.com/sparkle-project/Sparkle/releases/download/${SPARKLE_VERSION}/Sparkle-${SPARKLE_VERSION}.tar.xz"
    mv "${TARBALL}.tmp" "${TARBALL}"
fi

echo "verifying checksum..."
ACTUAL="$(shasum -a 256 "${TARBALL}" | awk '{print $1}')"
if [ "${ACTUAL}" != "${SPARKLE_SHA256}" ]; then
    echo "ERROR: checksum mismatch for ${TARBALL}"
    echo "  expected ${SPARKLE_SHA256}"
    echo "  actual   ${ACTUAL}"
    exit 1
fi

echo "extracting..."
rm -rf "${SRC_DIR}"
mkdir -p "${SRC_DIR}"
tar -xJf "${TARBALL}" -C "${SRC_DIR}"

# ditto, not cp: the framework is a versioned bundle (Versions/Current and the
# top-level entries are symlinks), and flattening those breaks its signature.
rm -rf "${PREFIX}"
mkdir -p "${PREFIX}/bin"
ditto "${SRC_DIR}/Sparkle.framework" "${PREFIX}/Sparkle.framework"
for tool in sign_update generate_keys; do
    cp "${SRC_DIR}/bin/${tool}" "${PREFIX}/bin/${tool}"
done
cp "${SRC_DIR}/LICENSE" "${PREFIX}/LICENSE"
echo "${STAMP_VALUE}" > "${STAMP}"

echo
echo "vendored Sparkle ${SPARKLE_VERSION} at ${PREFIX}"
