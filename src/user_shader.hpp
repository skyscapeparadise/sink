#pragma once

// Shader backgrounds: a .frag file the user picks, compiled to Metal while
// sink runs (glslang turns the GLSL into SPIR-V, SPIRV-Cross that into the
// Metal Shading Language SDL's GPU renderer takes -- the path understory uses
// for its shader elements).
//
// The file is written the way Shadertoy shaders are, so most of what's out
// there runs as is:
//
//     void mainImage(out vec4 fragColor, in vec2 fragCoord) { ... }
//
// with fragCoord in pixels from the bottom left, and these inputs:
//
//     vec3  iResolution   the window, in pixels (z is 1)
//     float iTime         seconds since the background started
//     float iTimeDelta    seconds since the last frame
//     float iFrameRate    frames per second
//     int   iFrame        frames drawn so far
//     vec4  iMouse        xy: the mouse, in pixels from the bottom left
//     vec4  iDate         year, month (0-11), day, seconds into the day
//
// Shadertoy's texture inputs (iChannel0-3) aren't provided. The output is
// opaque, and the background's exposure and hue shift apply to it as to
// every other kind.
//
// Pure C++: nothing here touches SDL or the GPU, so it can be tested alone.
// shader_background.cpp hands the result to SDL.

#include <string>

namespace user_shader {

struct Compiled {
    bool ok = false;
    std::string msl;    // Metal source; its fragment function is "main0"
    std::string error;  // why not, when !ok: glslang's message, lines as in the file
};

// `source` is the .frag file's text; `name` goes in front of error messages.
Compiled compile(const std::string& source, const std::string& name);

// The uniform block compile() gives the shader, as C++ lays it out. Its
// std140 layout is fixed by the order below; static_asserts in the .cpp
// check the two agree.
struct Inputs {
    float resolution[3];  // iResolution
    float time;           // iTime
    float mouse[4];       // iMouse
    float date[4];        // iDate
    float time_delta;     // iTimeDelta
    int frame;            // iFrame
    float frame_rate;     // iFrameRate
    float exposure;       // sink's: brightness scale, as for video
    float hue_degrees;    // sink's: hue rotation, as for video
    float linear_output;  // sink's: 1 when the target is linear (hdr text)
    float pad[2];
};

} // namespace user_shader
