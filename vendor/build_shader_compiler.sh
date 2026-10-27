#!/bin/bash
# build_shader_compiler.sh - Build the GLSL -> Metal compiler for shader backgrounds.
#
# The "shader" background runs a .frag file the user picks, so its GLSL can't
# be compiled when sink is built -- sink compiles it when the file is chosen,
# in-process: glslang turns GLSL into SPIR-V, and SPIRV-Cross turns SPIR-V
# into the Metal Shading Language SDL's GPU renderer takes. The same pair, and
# this script, come from understory, which does the same for its shader
# elements.
#
# glslang rather than shaderc: shaderc is glslang plus SPIRV-Tools, and
# SPIRV-Tools is only there for the optimizer. glslang with ENABLE_OPT=OFF is
# just the front end sink needs.
#
# Both are built static, so they link into sink itself: no dylib to bundle,
# and the linker drops what sink never calls.
#
# Output: vendor/shaderc/{include,lib}, plus both projects' license texts.
# Picked up by CMakeLists.txt.

set -euo pipefail

GLSLANG_VERSION="16.5.0"
GLSLANG_SHA256="01af17195fbeb59e39e31e9506de35bb39dfd35807ea0c9a1a99d7d1183ddd45"
CROSS_VERSION="vulkan-sdk-1.4.357.0"
CROSS_SHA256="97c910326afdd44d794ce8561326fa675fd1958b27142f03295403044d639639"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${SCRIPT_DIR}/shaderc"
CACHE_DIR="${SCRIPT_DIR}/.cache"
BUILD_DIR="${SCRIPT_DIR}/.build"

if [ "${1:-}" = "--clean" ]; then
    echo "removing ${PREFIX}..."
    rm -rf "${PREFIX}" "${BUILD_DIR}/glslang-${GLSLANG_VERSION}" \
           "${BUILD_DIR}/SPIRV-Cross-${CROSS_VERSION}"
    exit 0
fi

STAMP="${PREFIX}/.sink-shader-compiler-stamp"
STAMP_VALUE="${GLSLANG_VERSION}-${CROSS_VERSION}-${MACOSX_DEPLOYMENT_TARGET:-unset}-$(shasum -a 256 "$0" | cut -c1-12)"
if [ -f "${STAMP}" ] && [ "$(cat "${STAMP}")" = "${STAMP_VALUE}" ]; then
    echo "vendored shader compiler already built at ${PREFIX}"
    exit 0
fi

if [ -z "${MACOSX_DEPLOYMENT_TARGET:-}" ]; then
    echo "ERROR: MACOSX_DEPLOYMENT_TARGET is not set. Run via ./build.sh, or"
    echo "       'source ./sdk_env.sh' first, so the compiler targets the macOS sink supports."
    exit 1
fi

mkdir -p "${CACHE_DIR}" "${BUILD_DIR}"

fetch() {  # url  filename  sha256
    local tarball="${CACHE_DIR}/$2"
    if [ ! -f "${tarball}" ]; then
        echo "downloading $2..."
        curl -fsSL --retry 3 -o "${tarball}.tmp" "$1"
        mv "${tarball}.tmp" "${tarball}"
    fi
    local actual
    actual="$(shasum -a 256 "${tarball}" | awk '{print $1}')"
    if [ "${actual}" != "$3" ]; then
        echo "ERROR: checksum mismatch for $2"
        echo "  expected $3"
        echo "  actual   ${actual}"
        exit 1
    fi
    tar -xzf "${tarball}" -C "${BUILD_DIR}"
}

rm -rf "${PREFIX}"

# ---- glslang: GLSL -> SPIR-V ------------------------------------------------
GLSLANG_SRC="${BUILD_DIR}/glslang-${GLSLANG_VERSION}"
rm -rf "${GLSLANG_SRC}"
fetch "https://github.com/KhronosGroup/glslang/archive/refs/tags/${GLSLANG_VERSION}.tar.gz" \
      "glslang-${GLSLANG_VERSION}.tar.gz" "${GLSLANG_SHA256}"

echo "configuring glslang for macOS ${MACOSX_DEPLOYMENT_TARGET} (no optimizer, no tools)..."
cmake -S "${GLSLANG_SRC}" -B "${GLSLANG_SRC}/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET}" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DENABLE_OPT=OFF \
    -DENABLE_GLSLANG_BINARIES=OFF \
    -DGLSLANG_TESTS=OFF \
    -DENABLE_CTEST=OFF \
    -DENABLE_HLSL=OFF \
    -DBUILD_EXTERNAL=OFF > /dev/null

echo "compiling glslang..."
cmake --build "${GLSLANG_SRC}/build" -j"$(sysctl -n hw.ncpu)" > /dev/null
cmake --install "${GLSLANG_SRC}/build" > /dev/null
cp "${GLSLANG_SRC}/LICENSE.txt" "${PREFIX}/glslang-LICENSE.txt"

# ---- SPIRV-Cross: SPIR-V -> MSL ---------------------------------------------
CROSS_SRC="${BUILD_DIR}/SPIRV-Cross-${CROSS_VERSION}"
rm -rf "${CROSS_SRC}"
fetch "https://github.com/KhronosGroup/SPIRV-Cross/archive/refs/tags/${CROSS_VERSION}.tar.gz" \
      "SPIRV-Cross-${CROSS_VERSION}.tar.gz" "${CROSS_SHA256}"

# Only the MSL backend is wanted. (The MSL emitter derives from the GLSL one,
# so spirv-cross-glsl is still built, as a dependency.)
echo "configuring SPIRV-Cross for macOS ${MACOSX_DEPLOYMENT_TARGET} (MSL only)..."
cmake -S "${CROSS_SRC}" -B "${CROSS_SRC}/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET}" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DSPIRV_CROSS_SHARED=OFF \
    -DSPIRV_CROSS_STATIC=ON \
    -DSPIRV_CROSS_CLI=OFF \
    -DSPIRV_CROSS_ENABLE_TESTS=OFF \
    -DSPIRV_CROSS_ENABLE_HLSL=OFF \
    -DSPIRV_CROSS_ENABLE_CPP=OFF \
    -DSPIRV_CROSS_ENABLE_REFLECT=OFF \
    -DSPIRV_CROSS_ENABLE_C_API=OFF \
    -DSPIRV_CROSS_ENABLE_UTIL=OFF > /dev/null

echo "compiling SPIRV-Cross..."
cmake --build "${CROSS_SRC}/build" -j"$(sysctl -n hw.ncpu)" > /dev/null
cmake --install "${CROSS_SRC}/build" > /dev/null
cp "${CROSS_SRC}/LICENSE" "${PREFIX}/SPIRV-Cross-LICENSE"

# ---- check the whole path works, not just that it linked --------------------
#
# The failure worth guarding against is a compiler that builds, links, and
# returns something that isn't MSL. So compile a real shader end to end.
echo
echo "checking GLSL -> SPIR-V -> MSL end to end..."
cat > "${BUILD_DIR}/shaderchk.cpp" <<'EOF'
#include <cstdio>
#include <string>
#include <vector>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <spirv_msl.hpp>

static const char* kSource = R"(#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(set = 3, binding = 0) uniform UBO { float time; float scale; } ubo;
void main() { out_color = vec4(v_uv * ubo.scale, ubo.time, 1.0); }
)";

int main() {
    glslang::InitializeProcess();
    glslang::TShader shader(EShLangFragment);
    shader.setStrings(&kSource, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, EShLangFragment, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    if (!shader.parse(GetDefaultResources(), 100, false, EShMsgDefault)) {
        printf("  FAIL: glslang parse: %s\n", shader.getInfoLog());
        return 1;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(EShMsgDefault)) {
        printf("  FAIL: glslang link: %s\n", program.getInfoLog());
        return 1;
    }
    std::vector<unsigned int> spirv;
    glslang::GlslangToSpv(*program.getIntermediate(EShLangFragment), spirv);
    glslang::FinalizeProcess();
    if (spirv.empty()) { printf("  FAIL: empty SPIR-V\n"); return 1; }
    spirv_cross::CompilerMSL msl(std::move(spirv));
    spirv_cross::CompilerMSL::Options opts;
    opts.platform = spirv_cross::CompilerMSL::Options::macOS;
    msl.set_msl_options(opts);
    const std::string out = msl.compile();
    if (out.find("#include <metal_stdlib>") == std::string::npos ||
        out.find("fragment") == std::string::npos) {
        printf("  FAIL: output doesn't look like MSL:\n%s\n", out.substr(0, 200).c_str());
        return 1;
    }
    printf("  ok: %zu bytes of MSL\n", out.size());
    return 0;
}
EOF
GLSLANG_LIBS=""
for lib in glslang MachineIndependent GenericCodeGen glslang-default-resource-limits SPIRV SPVRemapper OSDependent OGLCompiler; do
    [ -f "${PREFIX}/lib/lib${lib}.a" ] && GLSLANG_LIBS="${GLSLANG_LIBS} -l${lib}"
done
CROSS_LIBS=""
for lib in spirv-cross-msl spirv-cross-glsl spirv-cross-core; do
    [ -f "${PREFIX}/lib/lib${lib}.a" ] && CROSS_LIBS="${CROSS_LIBS} -l${lib}"
done
c++ -std=c++17 "${BUILD_DIR}/shaderchk.cpp" -o "${BUILD_DIR}/shaderchk" \
    -mmacosx-version-min="${MACOSX_DEPLOYMENT_TARGET}" \
    -I"${PREFIX}/include" -I"${PREFIX}/include/spirv_cross" \
    -L"${PREFIX}/lib" ${GLSLANG_LIBS} ${CROSS_LIBS}
if ! "${BUILD_DIR}/shaderchk"; then
    echo "ERROR: the vendored shader compiler doesn't work end to end."
    exit 1
fi

echo "${STAMP_VALUE}" > "${STAMP}"
echo
echo "vendored shader compiler built at ${PREFIX}"
