#include "background_shader.hpp"

#include <cmath>
#include <cstring>
#include <iostream>

namespace {

// Same shader ABI as hue_shift.cpp: SDL's built-in vertex shader feeds
// locn0 (color) and locn1 (texcoord); uniforms at buffer(0); entry "main0".
const char* kBackgroundMSL = R"MSL(
#include <metal_stdlib>
#include <simd/simd.h>
using namespace metal;

struct main0_out { float4 out_var_SV_Target [[color(0)]]; };
struct main0_in {
    float4 in_var_COLOR0 [[user(locn0)]];
    float2 in_var_TEXCOORD0 [[user(locn1)]];
};

struct BgUniforms {
    float2 size;        // window, in points
    float time;         // gel churn, seconds
    float mode;         // 0 gradient, 1 gel
    float angle;        // gradient direction, radians
    float count;        // gradient stops
    float linear_mode;  // 1 when the target is linear (hdr text)
    float exposure;
    float hue;          // degrees
    float pad0, pad1, pad2;
    float4 colors[8];   // sRGB
};

// ---- the gel: photoscoping's nebula ---------------------------------------

float hash12(float2 p) {
    float3 p3 = fract(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float value_noise(float2 p) {
    float2 i = floor(p);
    float2 f = fract(p);
    float2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float a = hash12(i);
    float b = hash12(i + float2(1.0, 0.0));
    float c = hash12(i + float2(0.0, 1.0));
    float d = hash12(i + float2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Rotating between octaves keeps the lattice from lining up into streaks.
constant float2x2 kOctave = float2x2(float2(0.80, 0.60), float2(-0.60, 0.80));

float fbm(float2 p) {
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 6; ++i) {
        sum += amp * value_noise(p);
        p = kOctave * p * 2.03 + float2(17.1, 9.2);
        amp *= 0.5;
    }
    return sum / 0.984;
}

// Sparse stars, each breathing slowly at a rate of its own.
float stars(float2 px, float cell, float density, float t) {
    float2 c = floor(px / cell);
    float h = hash12(c + 71.3);
    if (h > density) return 0.0;
    float2 at = (float2(hash12(c + 3.1), hash12(c + 9.7)) * 0.8 + 0.1) * cell;
    float d = length(px - c * cell - at);
    float twinkle = 0.4 + 0.6 * hash12(c + 23.9);
    float breath = 0.7 + 0.3 * sin(t * (0.4 + 1.2 * hash12(c + 5.5)) + 6.2832 * hash12(c + 41.0));
    return twinkle * breath * exp(-d * d * 1.6);
}

// Measured in points from the window's top-right corner, so resizing
// uncovers more of the same sky rather than stretching it.
float3 gel(float2 uv, constant BgUniforms& u) {
    float2 px = float2(uv.x * u.size.x - u.size.x, uv.y * u.size.y);
    float2 p = px / 300.0 + float2(3.7, 1.9);
    float t = u.time;

    float2 q = float2(fbm(p + float2(0.021, 0.013) * t), fbm(p + float2(5.2, 1.3) - float2(0.017, 0.024) * t));
    float2 r = float2(fbm(p + 3.2 * q + float2(1.7, 9.2) + float2(-0.032, 0.041) * t),
                      fbm(p + 3.2 * q + float2(8.3, 2.8) + float2(0.038, -0.027) * t));
    float f = fbm(p + 3.0 * r + float2(0.009, -0.006) * t);

    // Light gathers near the top and falls away to the bottom.
    float2 n = px / max(u.size.y, 1.0);
    float fall = 1.0 - smoothstep(0.05, 1.05, n.y);
    float2 g = (n - float2(-0.30, 0.28)) * float2(1.5, 2.1);
    float glow = exp(-dot(g, g));

    float3 kVoid = u.colors[0].rgb, kDeep = u.colors[1].rgb, kPlum = u.colors[2].rgb;
    float3 kOrchid = u.colors[3].rgb, kVeil = u.colors[4].rgb;

    float3 col = kVoid;
    col = mix(col, kDeep, clamp(f * f * 2.2, 0.0, 1.0) * (0.35 + 0.65 * fall));
    col = mix(col, kPlum, clamp(length(q) * 0.9 - 0.35, 0.0, 1.0) * fall * (0.4 + 0.6 * glow));
    col = mix(col, kOrchid, smoothstep(0.55, 0.95, f) * glow * fall);
    col += kVeil * pow(smoothstep(0.62, 1.0, f * (0.6 + 0.6 * r.x)), 3.0) * glow * 0.45;

    // Dust lanes where the warp folds over itself.
    float lane = smoothstep(0.30, 0.62, r.y) * 0.65 + 0.35;
    col *= mix(1.0, lane, 0.85 * fall);

    float s = stars(px, 46.0, 0.10, t) + 0.6 * stars(px + 400.0, 23.0, 0.05, t);
    col += float3(0.85, 0.80, 1.0) * s * (1.0 - smoothstep(0.35, 0.8, f)) * 0.55;
    return col;
}

// ---- the gradient -----------------------------------------------------------

float3 gradient(float2 uv, constant BgUniforms& u) {
    float2 dir = float2(cos(u.angle), sin(u.angle));
    float2 pt = (uv - 0.5) * u.size;
    // Scaled so the far corners land exactly on the first and last stop.
    float reach = 0.5 * (abs(u.size.x * dir.x) + abs(u.size.y * dir.y));
    float t = clamp(dot(pt, dir) / max(reach, 1.0) * 0.5 + 0.5, 0.0, 1.0);
    int n = max(int(u.count), 1);
    if (n == 1) return u.colors[0].rgb;
    float seg = t * float(n - 1);
    int i = min(int(seg), n - 2);
    return mix(u.colors[i].rgb, u.colors[i + 1].rgb, seg - float(i));
}

fragment main0_out main0(main0_in in [[stage_in]],
                         constant BgUniforms& u [[buffer(0)]],
                         texture2d<float> u_texture [[texture(0)]],
                         sampler u_sampler [[sampler(0)]],
                         float4 frag [[position]])
{
    main0_out out = {};
    float2 uv = in.in_var_TEXCOORD0;
    float3 col = u.mode > 0.5 ? gel(uv, u) : gradient(uv, u);

    // Hue rotation and exposure, as the video background gets them.
    float a = u.hue * 0.0174532925;
    float cosA = cos(a), sinA = sin(a);
    float3x3 m = float3x3(
        float3(0.213 + cosA*0.787 - sinA*0.213, 0.213 - cosA*0.213 + sinA*0.143, 0.213 - cosA*0.213 - sinA*0.787),
        float3(0.715 - cosA*0.715 - sinA*0.715, 0.715 + cosA*0.285 + sinA*0.140, 0.715 - cosA*0.715 + sinA*0.715),
        float3(0.072 - cosA*0.072 + sinA*0.928, 0.072 - cosA*0.072 - sinA*0.283, 0.072 + cosA*0.928 + sinA*0.072));
    col = clamp(m * col, 0.0, 1.0) * u.exposure;

    // Dither: dark gradients band visibly in 8 bits without it.
    float dither = (hash12(frag.xy) + hash12(frag.yx + 7.0) - 1.0) / 255.0;
    col = clamp(col + dither, 0.0, 1.0);
    if (u.linear_mode > 0.5) col = pow(col, float3(2.2));
    out.out_var_SV_Target = float4(col, 1.0);
    return out;
}
)MSL";

struct Uniforms {
    float size[2];
    float time, mode, angle, count, linear_mode, exposure, hue;
    float pad[3];
    float colors[8][4];
};
static_assert(sizeof(Uniforms) == 48 + 128, "must match BgUniforms in the shader");

} // namespace

BackgroundShader::~BackgroundShader() { cleanup(); }

bool BackgroundShader::init(SDL_Renderer* renderer, bool linear_colorspace) {
    cleanup();
    linear_colorspace_ = linear_colorspace;
    device_ = SDL_GetGPURendererDevice(renderer);
    if (!device_ || !(SDL_GetGPUShaderFormats(device_) & SDL_GPU_SHADERFORMAT_MSL)) {
        std::cerr << "Background shader: needs the GPU renderer with MSL -- gradient and gel backgrounds "
                     "fall back to a solid color" << std::endl;
        device_ = nullptr;
        return false;
    }

    SDL_GPUShaderCreateInfo info;
    SDL_zero(info);
    info.format = SDL_GPU_SHADERFORMAT_MSL;
    info.code = reinterpret_cast<const Uint8*>(kBackgroundMSL);
    info.code_size = std::strlen(kBackgroundMSL);
    info.entrypoint = "main0";
    info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    info.num_samplers = 1;
    info.num_uniform_buffers = 1;
    shader_ = SDL_CreateGPUShader(device_, &info);
    if (!shader_) {
        std::cerr << "Background shader: failed to compile: " << SDL_GetError() << std::endl;
        device_ = nullptr;
        return false;
    }

    SDL_GPURenderStateCreateInfo state_info;
    SDL_zero(state_info);
    state_info.fragment_shader = shader_;
    state_ = SDL_CreateGPURenderState(renderer, &state_info);
    if (!state_) {
        std::cerr << "Background shader: failed to create render state: " << SDL_GetError() << std::endl;
        cleanup();
        return false;
    }

    white_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 1, 1);
    if (white_) {
        const Uint32 pixel = 0xFFFFFFFF;
        SDL_UpdateTexture(white_, nullptr, &pixel, sizeof(pixel));
    } else {
        cleanup();
        return false;
    }
    return true;
}

void BackgroundShader::cleanup() {
    if (white_) {
        SDL_DestroyTexture(white_);
        white_ = nullptr;
    }
    if (state_) {
        SDL_DestroyGPURenderState(state_);
        state_ = nullptr;
    }
    if (shader_ && device_) SDL_ReleaseGPUShader(device_, shader_);
    shader_ = nullptr;
    device_ = nullptr;
}

void BackgroundShader::draw(SDL_Renderer* renderer, const Params& p) {
    if (!state_ || !white_) return;
    Uniforms u;
    std::memset(&u, 0, sizeof(u));
    u.size[0] = p.logical_w;
    u.size[1] = p.logical_h;
    u.time = p.time;
    u.mode = static_cast<float>(p.mode);
    u.angle = p.angle_degrees * 3.14159265f / 180.0f;
    u.count = static_cast<float>(p.count);
    u.linear_mode = linear_colorspace_ ? 1.0f : 0.0f;
    u.exposure = p.exposure;
    u.hue = p.hue_degrees;
    for (int i = 0; i < 8; ++i) {
        u.colors[i][0] = p.colors[i].r;
        u.colors[i][1] = p.colors[i].g;
        u.colors[i][2] = p.colors[i].b;
        u.colors[i][3] = 1.0f;
    }
    SDL_SetGPURenderStateFragmentUniforms(state_, 0, &u, sizeof(u));
    SDL_SetGPURenderState(renderer, state_);
    SDL_RenderTexture(renderer, white_, nullptr, nullptr);
    SDL_SetGPURenderState(renderer, nullptr);
}
