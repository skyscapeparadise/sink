#!/bin/bash
# build_text_stack.sh - Build libpng, FreeType, HarfBuzz and SDL3_ttf for sink.
#
# Homebrew's harfbuzz links glib (which drags in libpcre2 and libintl) and
# graphite2: ~2.2MB in the app bundle. Neither is needed here.
#   * glib only provides harfbuzz's Unicode property lookups. With it off,
#     harfbuzz uses its own embedded UCD tables (hb_ucd_get_unicode_funcs in
#     src/hb-unicode.cc), which is the upstream default and fully functional.
#   * graphite2 only matters for SIL Graphite fonts. Monaspace and every other
#     font sink ships or is likely to load is OpenType.
#
# HarfBuzz itself is KEPT, and is in use: grapheme clusters (combining marks,
# emoji ZWJ sequences) are shaped through it by TTF_RenderText_Blended (see
# FontManager::get_cluster_glyph). That text is whatever reaches the terminal,
# so HarfBuzz is on the untrusted-input path -- keep it current. Single glyphs
# still render via TTF_RenderGlyph_Blended, FreeType only.
#
# FreeType and libpng (which FreeType needs for colour bitmap glyphs, i.e.
# emoji) used to come from Homebrew. Homebrew builds for the macOS it's
# running on, so after the build machine moved to macOS 27, sink 0.11 shipped
# both with a 27.0 minimum inside an app that promises 26.0. They're built here
# now, for MACOSX_DEPLOYMENT_TARGET like everything else; FreeType without
# brotli (WOFF2 web fonts, which sink never loads) or its own HarfBuzz.
#
# All four libraries go into one prefix, so they are siblings and find each
# other through @loader_path. SDL3_ttf links the vendored SDL3
# (build_sdl3.sh), which must be built first.
#
# Output: vendor/textstack. Picked up by CMakeLists.txt.

set -euo pipefail

PNG_VERSION="1.6.59"
# libpng publishes no signature for 1.6.59. This tarball was checked file for
# file against the v1.6.59 tag in github.com/pnggroup/libpng, which Cosmin
# Truta signed (GitHub verifies it).
PNG_SHA256="d80dd2a38a37f803cb9b6ac7b14bd6e74ddc3b654780a8380bdf93523fdb4389"
FT_VERSION="2.15.0"
# Checked against Werner Lemberg's signature (freetype-2.15.0.tar.xz.sig, key
# E30674707856409FF1948010BE6C3AAC63AD8E3F).
FT_SHA256="93cd47eb719c325107f2d3cf087d06299e944c3b48a40b55537302304dea1dd5"
HB_VERSION="14.6.0"
HB_SHA256="d07a007327277708a2a73ae437887cdbaf282937f6d03ca5467723e9099af586"
TTF_VERSION="3.2.2"
TTF_SHA256="63547d58d0185c833213885b635a2c0548201cc8f301e6587c0be1a67e1e045d"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SDL3_PREFIX="${SCRIPT_DIR}/sdl3"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
# One shared prefix on purpose. HarfBuzz installs with an install_name of
# @rpath/libharfbuzz.dylib, so libSDL3_ttf can only resolve it via an
# @loader_path rpath -- which means the two dylibs must be siblings.
PREFIX="${SCRIPT_DIR}/textstack"
HB_PREFIX="${PREFIX}"
TTF_PREFIX="${PREFIX}"
CACHE_DIR="${SCRIPT_DIR}/.cache"
BUILD_DIR="${SCRIPT_DIR}/.build"

if [ "${1:-}" = "--clean" ]; then
    echo "removing ${PREFIX}..."
    rm -rf "${PREFIX}"
    rm -rf "${BUILD_DIR}/libpng-${PNG_VERSION}" "${BUILD_DIR}/freetype-${FT_VERSION}"
    rm -rf "${BUILD_DIR}/harfbuzz-${HB_VERSION}" "${BUILD_DIR}/SDL3_ttf-${TTF_VERSION}"
    exit 0
fi

STAMP="${TTF_PREFIX}/.sink-text-stack-stamp"
STAMP_VALUE="${PNG_VERSION}-${FT_VERSION}-${HB_VERSION}-${TTF_VERSION}-${MACOSX_DEPLOYMENT_TARGET:-unset}-$(shasum -a 256 "$0" | cut -c1-12)"
if [ -f "${STAMP}" ] && [ "$(cat "${STAMP}")" = "${STAMP_VALUE}" ]; then
    echo "vendored text stack already built (libpng ${PNG_VERSION}, freetype ${FT_VERSION}, harfbuzz ${HB_VERSION}, SDL3_ttf ${TTF_VERSION})"
    echo "(run '$0 --clean' to force a rebuild)"
    exit 0
fi

if [ -z "${MACOSX_DEPLOYMENT_TARGET:-}" ]; then
    echo "ERROR: MACOSX_DEPLOYMENT_TARGET is not set. Run via ./build.sh, or"
    echo "       'source ./sdk_env.sh' first."
    exit 1
fi
if [ ! -f "${SDL3_PREFIX}/lib/cmake/SDL3/SDL3Config.cmake" ]; then
    echo "ERROR: vendored SDL3 not found. Run vendor/build_sdl3.sh first."
    exit 1
fi

mkdir -p "${CACHE_DIR}" "${BUILD_DIR}"

fetch() {
    url="$1"; out="$2"; want="$3"
    if [ ! -f "${out}" ]; then
        echo "downloading $(basename "${out}")..."
        curl -fL --retry 3 -o "${out}.tmp" "${url}"
        mv "${out}.tmp" "${out}"
    fi
    got="$(shasum -a 256 "${out}" | awk '{print $1}')"
    if [ "${got}" != "${want}" ]; then
        echo "ERROR: checksum mismatch for ${out}"
        echo "  expected ${want}"
        echo "  actual   ${got}"
        exit 1
    fi
}

fetch "https://download.sourceforge.net/libpng/libpng-${PNG_VERSION}.tar.xz" \
      "${CACHE_DIR}/libpng-${PNG_VERSION}.tar.xz" "${PNG_SHA256}"
fetch "https://download.savannah.gnu.org/releases/freetype/freetype-${FT_VERSION}.tar.xz" \
      "${CACHE_DIR}/freetype-${FT_VERSION}.tar.xz" "${FT_SHA256}"
fetch "https://github.com/harfbuzz/harfbuzz/releases/download/${HB_VERSION}/harfbuzz-${HB_VERSION}.tar.xz" \
      "${CACHE_DIR}/harfbuzz-${HB_VERSION}.tar.xz" "${HB_SHA256}"
fetch "https://github.com/libsdl-org/SDL_ttf/releases/download/release-${TTF_VERSION}/SDL3_ttf-${TTF_VERSION}.tar.gz" \
      "${CACHE_DIR}/SDL3_ttf-${TTF_VERSION}.tar.gz" "${TTF_SHA256}"

rm -rf "${PREFIX}"

# ------------------------------------------------------------------ libpng
PNG_SRC="${BUILD_DIR}/libpng-${PNG_VERSION}"
echo "extracting libpng..."
rm -rf "${PNG_SRC}"
tar -xf "${CACHE_DIR}/libpng-${PNG_VERSION}.tar.xz" -C "${BUILD_DIR}"

echo "configuring libpng (shared library only, system zlib)..."
cmake -S "${PNG_SRC}" -B "${PNG_SRC}/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET}" \
    -DPNG_SHARED=ON \
    -DPNG_STATIC=OFF \
    -DPNG_FRAMEWORK=OFF \
    -DPNG_TESTS=OFF \
    -DPNG_TOOLS=OFF

echo "compiling libpng..."
cmake --build "${PNG_SRC}/build" -j"$(sysctl -n hw.ncpu)"
cmake --install "${PNG_SRC}/build"
cp "${PNG_SRC}/LICENSE" "${PREFIX}/libpng-LICENSE"

# ---------------------------------------------------------------- freetype
FT_SRC="${BUILD_DIR}/freetype-${FT_VERSION}"
echo "extracting freetype..."
rm -rf "${FT_SRC}"
tar -xf "${CACHE_DIR}/freetype-${FT_VERSION}.tar.xz" -C "${BUILD_DIR}"

# HVF is Apple's experimental vector font format, read through
# /usr/lib/libhvf.dylib. That library is in the macOS 26.5 SDK, but nothing
# says 26.0-26.4 have it, and linking it would stop FreeType loading on any
# that don't. sink has no use for HVF fonts.
echo "configuring freetype (libpng and system zlib; no brotli, harfbuzz or HVF)..."
cmake -S "${FT_SRC}" -B "${FT_SRC}/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_PREFIX_PATH="${PREFIX}" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET}" \
    -DBUILD_SHARED_LIBS=ON \
    -DFT_REQUIRE_PNG=ON \
    -DFT_REQUIRE_ZLIB=ON \
    -DFT_DISABLE_BROTLI=ON \
    -DFT_DISABLE_HARFBUZZ=ON \
    -DFT_DISABLE_HVF=ON

echo "compiling freetype..."
cmake --build "${FT_SRC}/build" -j"$(sysctl -n hw.ncpu)"
cmake --install "${FT_SRC}/build"
cp "${FT_SRC}/LICENSE.TXT" "${PREFIX}/FreeType-LICENSE.txt"

# ---------------------------------------------------------------- harfbuzz
HB_SRC="${BUILD_DIR}/harfbuzz-${HB_VERSION}"
echo "extracting harfbuzz..."
rm -rf "${HB_SRC}"
tar -xf "${CACHE_DIR}/harfbuzz-${HB_VERSION}.tar.xz" -C "${BUILD_DIR}"

echo "configuring harfbuzz (freetype only, no glib/graphite2/icu/cairo)..."
cmake -S "${HB_SRC}" -B "${HB_SRC}/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${HB_PREFIX}" \
    -DCMAKE_PREFIX_PATH="${PREFIX}" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET}" \
    -DBUILD_SHARED_LIBS=ON \
    -DHB_HAVE_FREETYPE=ON \
    -DHB_HAVE_GLIB=OFF \
    -DHB_HAVE_GRAPHITE2=OFF \
    -DHB_HAVE_ICU=OFF \
    -DHB_HAVE_CAIRO=OFF \
    -DHB_HAVE_GOBJECT=OFF \
    -DHB_HAVE_INTROSPECTION=OFF \
    -DHB_BUILD_UTILS=OFF \
    -DHB_BUILD_SUBSET=OFF \
    -DHB_BUILD_RASTER=OFF \
    -DHB_BUILD_VECTOR=OFF \
    -DHB_BUILD_GPU=OFF

echo "compiling harfbuzz..."
cmake --build "${HB_SRC}/build" -j"$(sysctl -n hw.ncpu)"
cmake --install "${HB_SRC}/build"
cp "${HB_SRC}/COPYING" "${HB_PREFIX}/COPYING" 2>/dev/null || true

# --------------------------------------------------------------- SDL3_ttf
TTF_SRC="${BUILD_DIR}/SDL3_ttf-${TTF_VERSION}"
echo "extracting SDL3_ttf..."
rm -rf "${TTF_SRC}"
tar -xzf "${CACHE_DIR}/SDL3_ttf-${TTF_VERSION}.tar.gz" -C "${BUILD_DIR}"

echo "configuring SDL3_ttf against the vendored SDL3, freetype and harfbuzz..."
cmake -S "${TTF_SRC}" -B "${TTF_SRC}/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${TTF_PREFIX}" \
    -DCMAKE_PREFIX_PATH="${PREFIX};${SDL3_PREFIX}" \
    -DSDL3_DIR="${SDL3_PREFIX}/lib/cmake/SDL3" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET}" \
    -DBUILD_SHARED_LIBS=ON \
    -DSDLTTF_VENDORED=OFF \
    -DSDLTTF_HARFBUZZ=ON \
    -DSDLTTF_PLUTOSVG=OFF \
    -DSDLTTF_SAMPLES=OFF \
    -DSDLTTF_INSTALL=ON

echo "compiling SDL3_ttf..."
cmake --build "${TTF_SRC}/build" -j"$(sysctl -n hw.ncpu)"
cmake --install "${TTF_SRC}/build"

# CMake strips build-tree rpaths on install, leaving @rpath load commands
# (SDL3_ttf -> harfbuzz, harfbuzz -> freetype, freetype -> libpng) with no
# rpath to satisfy them. They are all siblings in ${PREFIX}/lib, so
# @loader_path is all that is needed. SDL3 itself is found through the
# executable's rpath, as in the app bundle.
for lib in "${PREFIX}"/lib/libSDL3_ttf.0.dylib "${PREFIX}"/lib/libharfbuzz.dylib "${PREFIX}"/lib/libfreetype.6.dylib; do
    install_name_tool -add_rpath "@loader_path" "$(cd "$(dirname "${lib}")" && pwd)/$(basename "$(readlink -f "${lib}")")" 2>/dev/null || true
    codesign --force --sign - "$(readlink -f "${lib}")" 2>/dev/null || true
done
cp "${TTF_SRC}/LICENSE.txt" "${TTF_PREFIX}/LICENSE.txt" 2>/dev/null || true

# -------------------------------------------------------------- assertions
HB_LIB="${HB_PREFIX}/lib/libharfbuzz.dylib"
TTF_LIB="${TTF_PREFIX}/lib/libSDL3_ttf.0.dylib"

echo
echo "verifying glib/graphite2 are gone..."
# Check the files exist first: otool failing on a missing path would otherwise
# be swallowed and read as "no leaks found".
for f in "${HB_LIB}" "${TTF_LIB}"; do
    if [ ! -f "${f}" ]; then
        echo "ERROR: expected build output missing: ${f}"
        exit 1
    fi
done
LEAK="$(otool -L "${HB_LIB}" "${TTF_LIB}" \
    | grep -oE "lib(glib-2\.0|gobject-2\.0|pcre2-8|intl|graphite2|icuuc)\.[0-9.]*dylib" | sort -u || true)"
if [ -n "${LEAK}" ]; then
    echo "ERROR: the vendored text stack still links:"
    echo "${LEAK}" | sed 's/^/    /'
    exit 1
fi
echo "  clean -- harfbuzz links only freetype"

# Dropping glib swaps harfbuzz's Unicode backend for its built-in UCD tables.
# That is upstream's default, but it is exactly the kind of change that could
# silently degrade shaping, so assert the real behaviour rather than the config.
#
# Note what is NOT asserted: that ligatures fire by default. Monaspace keeps its
# code ligatures in stylistic sets ss01-ss09, which are off unless explicitly
# requested, so "!=" shapes to two unchanged glyphs with default features in
# both this build and Homebrew's. The test enables ss01-ss09 to exercise GSUB.
echo
echo "verifying Unicode backend and OpenType shaping..."
cat > "${BUILD_DIR}/shapechk.c" <<'EOF'
#include <stdio.h>
#include <hb.h>

/* 1. The Unicode property lookups glib used to provide, now served by
      harfbuzz's embedded UCD tables. If this regressed, shaping of anything
      non-trivial (scripts, marks, bidi mirroring) would quietly break. */
static int check_unicode(void) {
    hb_unicode_funcs_t *u = hb_unicode_funcs_get_default();
    int bad = 0;
    if (hb_unicode_general_category(u, 'A') != HB_UNICODE_GENERAL_CATEGORY_UPPERCASE_LETTER) {
        printf("  FAIL: general_category('A')\n"); bad = 1; }
    if (hb_unicode_script(u, 0x05D0) != HB_SCRIPT_HEBREW) {
        printf("  FAIL: script(U+05D0) should be Hebrew\n"); bad = 1; }
    if (hb_unicode_script(u, 0x3042) != HB_SCRIPT_HIRAGANA) {
        printf("  FAIL: script(U+3042) should be Hiragana\n"); bad = 1; }
    if (hb_unicode_mirroring(u, '(') != ')') {
        printf("  FAIL: mirroring('(')\n"); bad = 1; }
    if (hb_unicode_combining_class(u, 0x0301) != 230) {
        printf("  FAIL: combining_class(U+0301) should be 230\n"); bad = 1; }
    if (!bad) printf("  unicode backend (built-in UCD): OK\n");
    return bad;
}

/* 2. Plain ASCII must map one glyph per codepoint -- sink's terminal grid
      depends on that, and it is what TTF_RenderGlyph_Blended assumes. */
static int check_ascii(hb_face_t *face) {
    hb_font_t *f = hb_font_create(face); hb_font_set_scale(f, 1000, 1000);
    hb_buffer_t *b = hb_buffer_create();
    hb_buffer_add_utf8(b, "abcdef", -1, 0, -1);
    hb_buffer_guess_segment_properties(b);
    hb_shape(f, b, NULL, 0);
    unsigned n = hb_buffer_get_length(b);
    hb_buffer_destroy(b); hb_font_destroy(f);
    printf(n == 6 ? "  ascii 1:1 shaping: OK\n" : "  FAIL: ascii shaped to %u glyphs\n", n);
    return n != 6;
}

/* 3. Real GSUB substitution, proving the shaping engine itself works. */
static int check_gsub(hb_face_t *face) {
    const char *tags[9] = {"ss01","ss02","ss03","ss04","ss05","ss06","ss07","ss08","ss09"};
    hb_feature_t ft[9];
    for (int i = 0; i < 9; i++) {
        hb_feature_from_string(tags[i], -1, &ft[i]);
        ft[i].value = 1; ft[i].start = HB_FEATURE_GLOBAL_START; ft[i].end = HB_FEATURE_GLOBAL_END;
    }
    const char *cases[4] = { "!=", "==", "->", "=>" };
    int hits = 0;
    for (int t = 0; t < 4; t++) {
        hb_font_t *f = hb_font_create(face); hb_font_set_scale(f, 1000, 1000);
        hb_buffer_t *b = hb_buffer_create();
        hb_buffer_add_utf8(b, cases[t], -1, 0, -1);
        hb_buffer_guess_segment_properties(b);
        hb_shape(f, b, ft, 9);
        unsigned n = hb_buffer_get_length(b);
        hb_glyph_info_t *g = hb_buffer_get_glyph_infos(b, NULL);
        for (unsigned i = 0; i < n; i++) {
            hb_codepoint_t nominal = 0;
            hb_font_get_nominal_glyph(f, (hb_codepoint_t)cases[t][i], &nominal);
            if (g[i].codepoint != nominal) { hits++; break; }
        }
        hb_buffer_destroy(b); hb_font_destroy(f);
    }
    printf(hits == 4 ? "  gsub ligature substitution (ss01-09): %d/4 OK\n"
                     : "  FAIL: gsub substitution only %d/4\n", hits);
    return hits != 4;
}

int main(int argc, char **argv) {
    hb_blob_t *blob = hb_blob_create_from_file(argv[1]);
    if (!blob || hb_blob_get_length(blob) == 0) { printf("  cannot read %s\n", argv[1]); return 1; }
    hb_face_t *face = hb_face_create(blob, 0);
    if (hb_face_get_glyph_count(face) == 0) { printf("  no glyphs in face\n"); return 1; }
    return check_unicode() | check_ascii(face) | check_gsub(face);
}
EOF
clang "${BUILD_DIR}/shapechk.c" -o "${BUILD_DIR}/shapechk" \
    -I"${HB_PREFIX}/include/harfbuzz" \
    "${HB_LIB}" -Wl,-rpath,"${HB_PREFIX}/lib"
if ! "${BUILD_DIR}/shapechk" "${REPO_ROOT}/fonts/MonaspaceNeon-Regular.otf"; then
    echo "ERROR: vendored harfbuzz does not shape correctly. Refusing to continue."
    exit 1
fi

# The failure this catches is a silently unloadable library: an @rpath load
# command with no matching LC_RPATH resolves fine at link time and dies at
# launch. dlopen exercises the real resolution path.
echo
echo "verifying the text stack actually loads..."
cat > "${BUILD_DIR}/loadchk.c" <<'EOF'
#include <stdio.h>
#include <dlfcn.h>
int main(int argc, char **argv) {
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { printf("  FAIL: %s\n", dlerror()); return 1; }
    if (!dlsym(h, "TTF_Init")) { printf("  FAIL: TTF_Init not found\n"); return 1; }
    printf("  dlopen(libSDL3_ttf) + TTF_Init resolved: OK\n");
    return 0;
}
EOF
clang "${BUILD_DIR}/loadchk.c" -o "${BUILD_DIR}/loadchk" -Wl,-rpath,"${SDL3_PREFIX}/lib"
if ! "${BUILD_DIR}/loadchk" "${TTF_PREFIX}/lib/libSDL3_ttf.0.dylib"; then
    echo "ERROR: vendored SDL3_ttf cannot be loaded. Refusing to continue."
    exit 1
fi

# Nothing from Homebrew, and nothing newer than the macOS sink supports.
echo
echo "verifying every library is self-built and targets macOS ${MACOSX_DEPLOYMENT_TARGET}..."
for f in "${PREFIX}"/lib/*.dylib; do
    [ -L "${f}" ] && continue
    if otool -L "${f}" | tail -n +2 | grep -q "/opt/homebrew"; then
        echo "ERROR: ${f} links a Homebrew library:"
        otool -L "${f}" | grep "/opt/homebrew" | sed 's/^/    /'
        exit 1
    fi
    minos="$(otool -l "${f}" | awk '/LC_BUILD_VERSION/{f=1} f && /minos/{print $2; exit}')"
    if [ "${minos}" != "${MACOSX_DEPLOYMENT_TARGET}" ]; then
        echo "ERROR: ${f} targets macOS ${minos}, not ${MACOSX_DEPLOYMENT_TARGET}"
        exit 1
    fi
done
echo "  clean"

echo "${STAMP_VALUE}" > "${STAMP}"
echo
echo "vendored text stack built:"
du -sh "${PREFIX}/lib"

