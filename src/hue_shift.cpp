#include "hue_shift.hpp"
#include <iostream>
#include <cstring>

namespace {

// Matches the shader ABI SDL's 2D GPU renderer expects for a custom
// fragment shader plugged in via SDL_GPURenderState: the built-in vertex
// shader still runs and hands the fragment stage a per-vertex color
// (already carrying whatever SDL_SetTextureColorMod/SDL_SetRenderColorScale
// would normally apply -- multiplying it in below means exposure keeps
// working unchanged) and a texcoord, at fixed locn0/locn1 bindings; the
// primary texture/sampler are auto-bound at texture(0)/sampler(0); a
// uniform buffor at buffer(0) carries the one parameter this shader needs.
// Entry point must be named "main0" -- that's the convention SDL's shader
// tooling (and this hand-written shader, to match) uses.
const char* kHueShiftMSL = R"MSL(
#include <metal_stdlib>
#include <simd/simd.h>
using namespace metal;

struct main0_out
{
    float4 out_var_SV_Target [[color(0)]];
};

struct main0_in
{
    float4 in_var_COLOR0 [[user(locn0)]];
    float2 in_var_TEXCOORD0 [[user(locn1)]];
};

struct HueUniforms
{
    float hue_degrees;
    float ntsc_enabled; // 0.0 or 1.0 -- avoids bool alignment questions in a Metal constant buffer
    float linear_mode;  // 0.0 (standard colorspace) or 1.0 (linear/extended, hdr console)
};

// See the identical helper in crt_shader.cpp for why this exists: the
// chroma-bleed and hue-rotation math below was tuned against values in a
// perceptual/gamma-like 0..1 range, but when hdr console is on the render
// target is linear/extended colorspace and samples come back as physically
// linear light instead -- same picture, different numbers. Converting into
// one consistent reference space before the math (and back before output)
// keeps the effect looking the same regardless of which colorspace it's
// actually running in.
float3 toStylizeSpace(float3 c, float linear_mode) {
    return mix(c, pow(max(c, 0.0), float3(1.0 / 2.2)), linear_mode);
}
float3 fromStylizeSpace(float3 c, float linear_mode) {
    return mix(c, pow(max(c, 0.0), float3(2.2)), linear_mode);
}

fragment main0_out main0(main0_in in [[stage_in]],
                          constant HueUniforms& u [[buffer(0)]],
                          texture2d<float> u_texture [[texture(0)]],
                          sampler u_sampler [[sampler(0)]])
{
    main0_out out = {};
    float2 uv = in.in_var_TEXCOORD0;
    float4 c = u_texture.sample(u_sampler, uv) * in.in_var_COLOR0;
    float3 base = toStylizeSpace(c.rgb, u.linear_mode);

    if (u.ntsc_enabled > 0.5) {
        // Composite-NTSC-style chroma bleed: blur the *color* horizontally
        // across several taps while keeping *brightness* from the sharp
        // center sample, approximating how an analog composite signal
        // smears chroma sideways while luma stays comparatively crisp.
        float2 texel = 1.0 / float2(u_texture.get_width(), u_texture.get_height());
        float3 lumaWeights = float3(0.299, 0.587, 0.114);
        float centerY = dot(base, lumaWeights);

        const int HALF = 4;
        float3 chromaSum = float3(0.0);
        float weightSum = 0.0;
        for (int i = -HALF; i <= HALF; i++) {
            float w = 1.0 - abs(float(i)) / float(HALF + 1);
            float2 sampleUv = uv + float2(texel.x * float(i) * 1.6, 0.0);
            float3 s = toStylizeSpace(u_texture.sample(u_sampler, sampleUv).rgb, u.linear_mode);
            float sY = dot(s, lumaWeights);
            chromaSum += (s - sY) * w;
            weightSum += w;
        }
        float3 chroma = chromaSum / weightSum;
        float3 ntscColor = float3(centerY) + chroma * 1.3;

        // NTSC's narrower color gamut read as slightly muted colors.
        float ntscLuma = dot(ntscColor, lumaWeights);
        base = mix(ntscColor, float3(ntscLuma), 0.08);
    }

    // Classic hue-rotation matrix: rotates RGB around the gray (luma) axis
    // by `angle`, preserving each pixel's own contrast/saturation instead
    // of washing everything toward a fixed tint. At hue_degrees == 0 this
    // matrix is the identity, so it's always safe to apply.
    float angle = u.hue_degrees * 0.0174532925f; // degrees -> radians
    float cosA = cos(angle);
    float sinA = sin(angle);

    float3x3 m = float3x3(
        float3(0.213 + cosA*0.787 - sinA*0.213, 0.213 - cosA*0.213 + sinA*0.143, 0.213 - cosA*0.213 - sinA*0.787),
        float3(0.715 - cosA*0.715 - sinA*0.715, 0.715 + cosA*0.285 + sinA*0.140, 0.715 - cosA*0.715 + sinA*0.715),
        float3(0.072 - cosA*0.072 + sinA*0.928, 0.072 - cosA*0.072 - sinA*0.283, 0.072 + cosA*0.928 + sinA*0.072)
    );

    float3 rotated = clamp(m * base, 0.0, 1.0);
    float3 outColor = fromStylizeSpace(rotated, u.linear_mode);

    out.out_var_SV_Target = float4(outColor, c.w);
    return out;
}
)MSL";

} // namespace

HueShiftEffect::HueShiftEffect() {}

HueShiftEffect::~HueShiftEffect() {
    cleanup();
}

bool HueShiftEffect::init(SDL_Renderer* renderer, bool linear_colorspace) {
    cleanup();
    linear_colorspace_ = linear_colorspace;

    device_ = SDL_GetGPURendererDevice(renderer);
    if (!device_) {
        std::cerr << "Hue shift: renderer has no GPU device (needs the \"" << SDL_GPU_RENDERER << "\" renderer backend) -- background hue shift disabled" << std::endl;
        return false;
    }

    SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device_);
    if (!(formats & SDL_GPU_SHADERFORMAT_MSL)) {
        std::cerr << "Hue shift: MSL shaders not supported by this GPU device -- background hue shift disabled" << std::endl;
        device_ = nullptr;
        return false;
    }

    SDL_GPUShaderCreateInfo info;
    SDL_zero(info);
    info.format = SDL_GPU_SHADERFORMAT_MSL;
    info.code = reinterpret_cast<const Uint8*>(kHueShiftMSL);
    info.code_size = std::strlen(kHueShiftMSL);
    info.entrypoint = "main0";
    info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    info.num_samplers = 1;
    info.num_uniform_buffers = 1;

    shader_ = SDL_CreateGPUShader(device_, &info);
    if (!shader_) {
        std::cerr << "Hue shift: failed to compile shader: " << SDL_GetError() << std::endl;
        device_ = nullptr;
        return false;
    }

    SDL_GPURenderStateCreateInfo state_info;
    SDL_zero(state_info);
    state_info.fragment_shader = shader_;
    state_ = SDL_CreateGPURenderState(renderer, &state_info);
    if (!state_) {
        std::cerr << "Hue shift: failed to create render state: " << SDL_GetError() << std::endl;
        SDL_ReleaseGPUShader(device_, shader_);
        shader_ = nullptr;
        device_ = nullptr;
        return false;
    }

    return true;
}

void HueShiftEffect::cleanup() {
    if (intermediate_) {
        SDL_DestroyTexture(intermediate_);
        intermediate_ = nullptr;
        intermediate_w_ = 0;
        intermediate_h_ = 0;
    }
    if (state_) {
        SDL_DestroyGPURenderState(state_);
        state_ = nullptr;
    }
    if (shader_ && device_) {
        SDL_ReleaseGPUShader(device_, shader_);
        shader_ = nullptr;
    }
    device_ = nullptr;
}

void HueShiftEffect::ensure_intermediate(SDL_Renderer* renderer, int w, int h) {
    if (intermediate_ && intermediate_w_ == w && intermediate_h_ == h) return;
    if (intermediate_) {
        SDL_DestroyTexture(intermediate_);
        intermediate_ = nullptr;
    }
    if (w <= 0 || h <= 0) return;

    if (linear_colorspace_) {
        // Match the parent renderer's actual colorspace instead of always
        // using a standard 8-bit target -- see crt_shader.cpp's identical
        // ensure_intermediate() for why: a non-float target would silently
        // reinterpret/clamp values, breaking the round trip the shader's
        // toStylizeSpace/fromStylizeSpace conversion depends on.
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_RGBA64_FLOAT);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, SDL_COLORSPACE_SRGB_LINEAR);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_TARGET);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, w);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, h);
        intermediate_ = SDL_CreateTextureWithProperties(renderer, props);
        SDL_DestroyProperties(props);
    } else {
        intermediate_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, w, h);
    }

    if (intermediate_) {
        intermediate_w_ = w;
        intermediate_h_ = h;
    } else {
        intermediate_w_ = 0;
        intermediate_h_ = 0;
    }
}

void HueShiftEffect::draw(SDL_Renderer* renderer, SDL_Texture* source, float hue_degrees, float color_scale, bool ntsc_enabled) {
    bool need_shader = state_ && (hue_degrees != 0.0f || ntsc_enabled);
    if (!need_shader) {
        SDL_SetRenderColorScale(renderer, color_scale);
        SDL_RenderTexture(renderer, source, nullptr, nullptr);
        SDL_SetRenderColorScale(renderer, 1.0f);
        return;
    }

    SDL_Texture* real_target = SDL_GetRenderTarget(renderer);
    int out_w = 0, out_h = 0;
    SDL_GetCurrentRenderOutputSize(renderer, &out_w, &out_h);
    ensure_intermediate(renderer, out_w, out_h);

    if (!intermediate_) {
        // Couldn't allocate the intermediate target this frame -- fall back
        // to a plain draw rather than dropping the frame or (worse) running
        // the shader directly on `source`, which may be a YUV texture.
        SDL_SetRenderColorScale(renderer, color_scale);
        SDL_RenderTexture(renderer, source, nullptr, nullptr);
        SDL_SetRenderColorScale(renderer, 1.0f);
        return;
    }

    // Pass 1: normal draw (SDL's default path handles YUV -> RGB conversion
    // if `source` needs it) into the offscreen RGB intermediate.
    SDL_SetRenderTarget(renderer, intermediate_);
    SDL_SetRenderColorScale(renderer, color_scale);
    SDL_RenderTexture(renderer, source, nullptr, nullptr);
    SDL_SetRenderColorScale(renderer, 1.0f);

    // Pass 2: NTSC chroma bleed (if enabled) then hue-rotate the now-RGB
    // intermediate onto the real target.
    SDL_SetRenderTarget(renderer, real_target);
    float uniforms[3] = { hue_degrees, ntsc_enabled ? 1.0f : 0.0f, linear_colorspace_ ? 1.0f : 0.0f };
    SDL_SetGPURenderStateFragmentUniforms(state_, 0, uniforms, sizeof(uniforms));
    SDL_SetGPURenderState(renderer, state_);
    SDL_RenderTexture(renderer, intermediate_, nullptr, nullptr);
    SDL_SetGPURenderState(renderer, nullptr);
}
