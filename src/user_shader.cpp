#include "user_shader.hpp"

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <vector>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <spirv_msl.hpp>

namespace user_shader {

namespace {

static_assert(offsetof(Inputs, time) == 12, "iTime packs after iResolution, as std140 puts it");
static_assert(offsetof(Inputs, mouse) == 16 && offsetof(Inputs, date) == 32, "std140 vec4s");
static_assert(offsetof(Inputs, time_delta) == 48 && offsetof(Inputs, linear_output) == 68, "std140 scalars");
static_assert(sizeof(Inputs) == 80, "must match SinkInputs below");

// Comes before the user's code. The bindings are the ones SDL's GPU renderer
// gives a custom fragment shader: SDL's vertex shader hands over a color at
// location 0 and a texture coordinate at location 1, samplers live in set 2
// and uniform buffers in set 3 (which SPIRV-Cross, with decoration bindings
// on, turns into Metal's texture(0)/sampler(0) and buffer(0)).
const char* kPrelude = R"(#version 450
layout(location = 0) in vec4 sink_vertex_color;
layout(location = 1) in vec2 sink_vertex_uv;
layout(location = 0) out vec4 sink_out;
layout(set = 2, binding = 0) uniform sampler2D sink_white;  // what SDL draws with: 1x1 white
layout(set = 3, binding = 0) uniform SinkInputs {
    vec3 iResolution;
    float iTime;
    vec4 iMouse;
    vec4 iDate;
    float iTimeDelta;
    int iFrame;
    float iFrameRate;
    float sink_exposure;
    float sink_hue;
    float sink_linear;
    float sink_pad0;
    float sink_pad1;
};
const float iSampleRate = 44100.0;
)";

// Comes after it: runs the user's mainImage, then gives the result the
// exposure and hue shift every background gets (the same matrix as
// background_shader.cpp's), and converts it for a linear target.
const char* kEpilogue = R"(
void main() {
    vec4 color = vec4(0.0, 0.0, 0.0, 1.0);
    mainImage(color, vec2(gl_FragCoord.x, iResolution.y - gl_FragCoord.y));
    vec3 col = clamp(color.rgb, 0.0, 1.0);
    float a = sink_hue * 0.0174532925;
    float c = cos(a), s = sin(a);
    mat3 hue = mat3(
        vec3(0.213 + c*0.787 - s*0.213, 0.213 - c*0.213 + s*0.143, 0.213 - c*0.213 - s*0.787),
        vec3(0.715 - c*0.715 - s*0.715, 0.715 + c*0.285 + s*0.140, 0.715 - c*0.715 + s*0.715),
        vec3(0.072 - c*0.072 + s*0.928, 0.072 - c*0.072 - s*0.283, 0.072 + c*0.928 + s*0.072));
    col = clamp(hue * col, 0.0, 1.0) * sink_exposure;
    if (sink_linear > 0.5) col = pow(col, vec3(2.2));
    // Times the white texture SDL draws with, so the sampler it binds is
    // one the shader has.
    sink_out = vec4(col, 1.0) * texture(sink_white, sink_vertex_uv).a;
}
)";

std::once_flag g_init_once;
std::mutex g_compile_mutex;  // glslang's process state isn't safe to share across threads

// The source without comments, so a name mentioned in one isn't taken as code.
std::string strip_comments(const std::string& source) {
    std::string code;
    code.reserve(source.size());
    for (size_t i = 0; i < source.size();) {
        if (source.compare(i, 2, "//") == 0) {
            while (i < source.size() && source[i] != '\n') ++i;
        } else if (source.compare(i, 2, "/*") == 0) {
            i += 2;
            while (i + 1 < source.size() && source.compare(i, 2, "*/") != 0) ++i;
            i = std::min(i + 2, source.size());
        } else {
            code.push_back(source[i++]);
        }
    }
    return code;
}

// The user's code as it goes between the prelude and the epilogue: any
// #version line blanked (the prelude has one; a second is an error), so the
// line numbers in errors still match the file.
std::string body_of(const std::string& source) {
    std::string out;
    out.reserve(source.size());
    size_t start = 0;
    while (start <= source.size()) {
        size_t end = source.find('\n', start);
        if (end == std::string::npos) end = source.size();
        std::string line = source.substr(start, end - start);
        const size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line.compare(first, 8, "#version") == 0) line.clear();
        out += line;
        if (end < source.size()) out += '\n';
        start = end + 1;
    }
    return out;
}

// glslang reports "ERROR: 0:12: 'x' : undeclared identifier" with the
// line as #line set it. Trimmed to its first error, and the "0:" dropped,
// it reads as a line of the user's file.
std::string first_error(const std::string& log) {
    size_t at = log.find("ERROR: ");
    if (at == std::string::npos) return log;
    std::string line = log.substr(at + 7, log.find('\n', at) - (at + 7));
    if (line.compare(0, 2, "0:") == 0) line = "line " + line.substr(2);
    return line;
}

} // namespace

Compiled compile(const std::string& source, const std::string& name) {
    Compiled out;
    const std::string code = strip_comments(source);
    if (code.find("mainImage") == std::string::npos) {
        out.error = name + ": no mainImage function -- sink runs Shadertoy-style shaders, "
                    "void mainImage(out vec4 fragColor, in vec2 fragCoord)";
        return out;
    }
    if (code.find("iChannel") != std::string::npos) {
        out.error = name + ": uses iChannel textures, which sink doesn't provide";
        return out;
    }

    std::call_once(g_init_once, [] { glslang::InitializeProcess(); });
    std::lock_guard<std::mutex> lock(g_compile_mutex);

    const std::string full = std::string(kPrelude) + "#line 1\n" + body_of(source) + "\n" + kEpilogue;
    const char* strings[1] = { full.c_str() };
    glslang::TShader shader(EShLangFragment);
    shader.setStrings(strings, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, EShLangFragment, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    if (!shader.parse(GetDefaultResources(), 100, false, EShMsgDefault)) {
        out.error = name + ": " + first_error(shader.getInfoLog());
        return out;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(EShMsgDefault)) {
        out.error = name + ": " + first_error(program.getInfoLog());
        return out;
    }
    std::vector<unsigned int> spirv;
    glslang::GlslangToSpv(*program.getIntermediate(EShLangFragment), spirv);
    if (spirv.empty()) {
        out.error = name + ": the compiler produced nothing";
        return out;
    }

    try {
        spirv_cross::CompilerMSL msl(std::move(spirv));
        spirv_cross::CompilerMSL::Options opts;
        opts.platform = spirv_cross::CompilerMSL::Options::macOS;
        // Without this SPIRV-Cross numbers Metal resources in order of first
        // use and ignores the bindings above.
        opts.enable_decoration_binding = true;
        msl.set_msl_options(opts);
        // Variables declared without a value start at zero, as they do in a
        // browser -- Shadertoy shaders count on it (`float i, d, z;` then
        // `++i` in a loop), and in Metal they'd start as whatever was there.
        spirv_cross::CompilerGLSL::Options common = msl.get_common_options();
        common.force_zero_initialized_variables = true;
        msl.set_common_options(common);
        out.msl = msl.compile();
        out.ok = !out.msl.empty();
        if (!out.ok) out.error = name + ": the compiler produced nothing";
    } catch (const std::exception& e) {
        out.error = name + ": " + e.what();
    }
    return out;
}

} // namespace user_shader
