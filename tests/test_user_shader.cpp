// Tests for user_shader.cpp: Shadertoy-style .frag files compiling to Metal,
// and the messages for ones that don't.
#include "user_shader.hpp"

#include <cstdio>
#include <string>

static int checks_failed = 0;
static int checks_run = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++checks_run;                                                        \
        if (!(cond)) {                                                       \
            ++checks_failed;                                                 \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)

static bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// The shape most Shadertoy shaders have, #version line and all.
static void test_shadertoy_shader_compiles() {
    const auto r = user_shader::compile(
        "#version 300 es\n"
        "precision highp float;\n"
        "// mainImage is what Shadertoy calls\n"
        "void mainImage(out vec4 fragColor, in vec2 fragCoord) {\n"
        "    vec2 uv = fragCoord / iResolution.xy;\n"
        "    vec3 col = 0.5 + 0.5 * cos(iTime + uv.xyx + vec3(0, 2, 4));\n"
        "    col *= 1.0 + 0.0 * (iMouse.x + iDate.w + iTimeDelta + iFrameRate + float(iFrame));\n"
        "    fragColor = vec4(col, 1.0);\n"
        "}\n", "rainbow.frag");
    if (!r.ok) std::printf("  %s\n", r.error.c_str());
    CHECK(r.ok);
    CHECK(contains(r.msl, "fragment") && contains(r.msl, "main0"));
    CHECK(contains(r.msl, "[[buffer(0)]]") && contains(r.msl, "[[texture(0)]]"));
    CHECK(contains(r.msl, "user(locn1)"));
}

// Errors name the file and the line as it is in the file.
static void test_errors_point_at_the_line() {
    const auto r = user_shader::compile(
        "#version 300 es\n"
        "void mainImage(out vec4 fragColor, in vec2 fragCoord) {\n"
        "    fragColor = vec4(undefined_thing, 1.0);\n"
        "}\n", "broken.frag");
    CHECK(!r.ok);
    if (!contains(r.error, "broken.frag: line 3")) std::printf("  got: %s\n", r.error.c_str());
    CHECK(contains(r.error, "broken.frag: line 3"));
    CHECK(contains(r.error, "undefined_thing"));
}

static void test_wrong_kinds_of_shader() {
    auto r = user_shader::compile("void main() { gl_FragColor = vec4(1.0); }\n", "plain.frag");
    CHECK(!r.ok && contains(r.error, "mainImage"));
    r = user_shader::compile("// void mainImage() would go here\nvoid main() {}\n", "comment.frag");
    CHECK(!r.ok && contains(r.error, "mainImage"));
    r = user_shader::compile(
        "void mainImage(out vec4 c, in vec2 p) { c = texture(iChannel0, p); }\n", "tex.frag");
    CHECK(!r.ok && contains(r.error, "iChannel"));
}

// Variables declared without a value start at zero, as in a browser.
// Shadertoy shaders lean on it -- mrange's glowmarcher declares
// `float i, d, z;` and counts up from there -- and without it they drew black.
static void test_unset_variables_start_at_zero() {
    const auto r = user_shader::compile(
        "void mainImage(out vec4 o, vec2 c) {\n"
        "    float z;\n"
        "    for (float i; ++i < 5.; ) z += i;\n"
        "    o = vec4(z);\n"
        "}\n", "zero.frag");
    CHECK(r.ok);
    CHECK(contains(r.msl, "float i = {};") && contains(r.msl, "float z = {};"));
}

int main() {
    test_shadertoy_shader_compiles();
    test_errors_point_at_the_line();
    test_wrong_kinds_of_shader();
    test_unset_variables_start_at_zero();
    std::printf("%d/%d checks passed\n", checks_run - checks_failed, checks_run);
    return checks_failed == 0 ? 0 : 1;
}
