#include "crt_shader.hpp"
#include <iostream>
#include <cstring>

namespace {

// Same shader ABI as hue_shift.cpp's shader -- see the comment there for
// why the bindings/entry-point name are fixed the way they are.
const char* kCrtMSL = R"MSL(
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

struct CRTUniforms
{
    float2 resolution; // the window, in pixels
    float linear_mode; // 0.0 (standard colorspace) or 1.0 (linear/extended, hdr console)
    float scale;       // pixels per point: 2 on Retina displays, 1 otherwise
};

// All of this shader's stylized math (glow threshold, scanline/vignette
// depth, warm-tint amount) was tuned by eye once, against values in a
// perceptual/gamma-like 0..1 range. When the render target is the
// linear/extended colorspace (hdr console on), texture samples come back
// as *physically linear* light instead -- the same visual brightness reads
// as a much smaller number there (e.g. midtone ~0.5 gamma is ~0.21 linear),
// so identical constants would land completely differently depending on
// which colorspace happened to be in use, independent of the actual
// picture on screen. These convert samples into that one consistent
// reference space before the stylized math, then back before output, so
// the effect looks the same regardless of which colorspace it's running
// in -- only elsewhere (the per-tap clamp) does anything need to know it's
// specifically in HDR range.
float3 toStylizeSpace(float3 c, float linear_mode) {
    return mix(c, pow(max(c, 0.0), float3(1.0 / 2.2)), linear_mode);
}
float3 fromStylizeSpace(float3 c, float linear_mode) {
    return mix(c, pow(max(c, 0.0), float3(2.2)), linear_mode);
}

// Sizes are in points, not pixels, so the tube looks the same on a Retina
// display as on any other: scanlines measured in pixels were half as tall
// on Retina, and too fine to see.
constant float kScanlinePitch = 3.0;  // points from one scanline to the next
constant float kCurvature = 0.04;     // how much the glass bulges

fragment main0_out main0(main0_in in [[stage_in]],
                          constant CRTUniforms& u [[buffer(0)]],
                          texture2d<float> u_texture [[texture(0)]],
                          sampler u_sampler [[sampler(0)]])
{
    main0_out out = {};
    const float2 texel = 1.0 / u.resolution;
    const float scale = max(u.scale, 1.0);

    // Curved glass: the picture bulges out a little from the middle. The
    // bulge is scaled so the middle of each edge stays put -- nothing at the
    // window's edges is pushed off it. Only the corners reach past the
    // picture, and they show it mirrored back at the edge rather than black.
    float2 cc = in.in_var_TEXCOORD0 - 0.5;
    float bulge = (1.0 + kCurvature * dot(cc, cc)) / (1.0 + kCurvature * 0.25);
    float2 uv = 1.0 - abs(1.0 - abs(0.5 + cc * bulge));

    // Convergence: red and blue land slightly apart toward the sides of the
    // tube, as the beams do on a real one.
    float spread = 0.6 * scale * (cc.x * 2.0);
    float4 mid = u_texture.sample(u_sampler, uv) * in.in_var_COLOR0;
    float red = u_texture.sample(u_sampler, uv + float2(spread, 0.0) * texel).r;
    float blue = u_texture.sample(u_sampler, uv - float2(spread, 0.0) * texel).b;
    float4 rawBase = float4(red, mid.g, blue, mid.a);
    float4 base = float4(toStylizeSpace(rawBase.rgb, u.linear_mode), rawBase.a);

    // Halation/bloom: a ring of taps around this pixel, weighted toward
    // whichever of them are bright, added back as a soft glow. A cheap
    // single-pass stand-in for a real blur+threshold bloom pipeline --
    // good enough to read as phosphor bleed around bright content.
    float3 glow = float3(0.0);
    const int TAPS = 12;
    for (int i = 0; i < TAPS; i++) {
        float angle = (float(i) / float(TAPS)) * 6.28318530718;
        float radius = (1.5 + 2.0 * fract(float(i) * 0.61803399)) * scale;
        float2 offset = float2(cos(angle), sin(angle)) * texel * radius;
        float3 s = toStylizeSpace(u_texture.sample(u_sampler, uv + offset).rgb, u.linear_mode);
        // Clamp before thresholding: with hdr console on, `s` can exceed
        // 1.0 (that's the point -- true HDR brightness). Left unclamped
        // here, those pixels would blow the smoothstep well past "bright,"
        // making glow react far more strongly than the same visual
        // brightness does in SDR and reading as a wash. The glow effect
        // itself should read the same regardless of colorspace; only the
        // base color at the very end is allowed to actually go over 1.0.
        float lum = dot(clamp(s, 0.0, 1.0), float3(0.299, 0.587, 0.114));
        glow += clamp(s, 0.0, 1.0) * smoothstep(0.5, 1.0, lum);
    }
    glow /= float(TAPS);

    float3 color = base.rgb;
    float lum = clamp(dot(color, float3(0.299, 0.587, 0.114)), 0.0, 1.0);
    float2 pt = uv * u.resolution / scale;  // position in points

    // Scanlines: dark gaps between the beam's lines, kScanlinePitch apart.
    // A bright beam spreads into the gap, as phosphor does, so they're
    // deepest over dark areas and shallower over text.
    float beam = sin(pt.y * (6.28318530718 / kScanlinePitch)) * 0.5 + 0.5;
    float depth = mix(0.55, 0.30, lum);
    float scan = mix(1.0 - depth, 1.0, pow(beam, 0.6));
    color *= scan;

    // Aperture grille: columns of red, green and blue phosphor, a point
    // each.
    int stripe = int(fmod(floor(pt.x), 3.0));
    float3 grille = float3(0.86);
    grille[stripe] = 1.14;
    color *= grille;

    // Make up the brightness the scanlines and grille take away, so text
    // reads as bright as it does without CRT.
    color *= 1.18;

    // Warm phosphor tint, scaled by how bright this pixel already is so
    // true black stays black instead of being lifted into a visible warm
    // brown (a flat additive offset here doesn't know the difference
    // between "black" and "dark" -- it brightens both equally).
    float warmthAmount = dot(color, float3(0.299, 0.587, 0.114));
    color += float3(0.036, 0.020, -0.016) * warmthAmount;
    float vignette = 1.0 - dot(cc, cc) * 0.38;
    color *= vignette;

    // Glow is added last, *after* the scanline/vignette darkening above,
    // so it actually reads as extra brightness bleeding through instead of
    // being multiplied back down by them.
    color += glow * 0.8;

    // With the clear and glass backgrounds the window is partly see-through:
    // macOS lays these pixels over the desktop as premultiplied color, with
    // alpha saying how much of the desktop they cover. The scanlines and
    // vignette darken the whole picture on a real tube, so they darken the
    // desktop showing through as well -- by covering a little more of it.
    // (Opaque backgrounds have alpha 1 already, which this leaves alone.)
    float through = (1.0 - base.a) * scan * vignette;

    float3 outColor = fromStylizeSpace(clamp(color, 0.0, 1.0), u.linear_mode);
    out.out_var_SV_Target = float4(outColor, 1.0 - through);
    return out;
}
)MSL";

} // namespace

CrtShaderEffect::CrtShaderEffect() {}

CrtShaderEffect::~CrtShaderEffect() {
    cleanup();
}

bool CrtShaderEffect::init(SDL_Renderer* renderer, bool linear_colorspace) {
    cleanup();
    linear_colorspace_ = linear_colorspace;

    device_ = SDL_GetGPURendererDevice(renderer);
    if (!device_) {
        std::cerr << "CRT shader: renderer has no GPU device (needs the \"" << SDL_GPU_RENDERER << "\" renderer backend) -- falling back to the plain overlay" << std::endl;
        return false;
    }

    SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device_);
    if (!(formats & SDL_GPU_SHADERFORMAT_MSL)) {
        std::cerr << "CRT shader: MSL shaders not supported by this GPU device -- falling back to the plain overlay" << std::endl;
        device_ = nullptr;
        return false;
    }

    SDL_GPUShaderCreateInfo info;
    SDL_zero(info);
    info.format = SDL_GPU_SHADERFORMAT_MSL;
    info.code = reinterpret_cast<const Uint8*>(kCrtMSL);
    info.code_size = std::strlen(kCrtMSL);
    info.entrypoint = "main0";
    info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    info.num_samplers = 1;
    info.num_uniform_buffers = 1;

    shader_ = SDL_CreateGPUShader(device_, &info);
    if (!shader_) {
        std::cerr << "CRT shader: failed to compile shader: " << SDL_GetError() << std::endl;
        device_ = nullptr;
        return false;
    }

    SDL_GPURenderStateCreateInfo state_info;
    SDL_zero(state_info);
    state_info.fragment_shader = shader_;
    state_ = SDL_CreateGPURenderState(renderer, &state_info);
    if (!state_) {
        std::cerr << "CRT shader: failed to create render state: " << SDL_GetError() << std::endl;
        SDL_ReleaseGPUShader(device_, shader_);
        shader_ = nullptr;
        device_ = nullptr;
        return false;
    }

    return true;
}

void CrtShaderEffect::cleanup() {
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
    scene_active_ = false;
}

void CrtShaderEffect::ensure_intermediate(SDL_Renderer* renderer, int w, int h) {
    if (intermediate_ && intermediate_w_ == w && intermediate_h_ == h) return;
    if (intermediate_) {
        SDL_DestroyTexture(intermediate_);
        intermediate_ = nullptr;
    }
    if (w <= 0 || h <= 0) return;

    if (linear_colorspace_) {
        // A plain 8-bit-per-channel target silently clamps anything past
        // 1.0 to white -- fine for SDR, but it means hdr console's
        // brightness-boosted text loses its actual value the moment it's
        // drawn in here, before this shader even runs. A float format has
        // the range to carry it through untouched.
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
        // Drawn over the window replacing what's there rather than blended
        // with it: the window isn't cleared this frame (the scene was cleared
        // into this texture instead), so with a see-through background,
        // blending piled each frame on top of the last.
        SDL_SetTextureBlendMode(intermediate_, SDL_BLENDMODE_NONE);
        intermediate_w_ = w;
        intermediate_h_ = h;
    } else {
        intermediate_w_ = 0;
        intermediate_h_ = 0;
    }
}

void CrtShaderEffect::begin_scene(SDL_Renderer* renderer) {
    scene_active_ = false;
    if (!state_) return;

    saved_target_ = SDL_GetRenderTarget(renderer);
    int w = 0, h = 0;
    SDL_GetCurrentRenderOutputSize(renderer, &w, &h);
    ensure_intermediate(renderer, w, h);
    if (!intermediate_) return;

    SDL_SetRenderTarget(renderer, intermediate_);
    scene_active_ = true;
}

void CrtShaderEffect::end_scene(SDL_Renderer* renderer) {
    if (!scene_active_) return;
    scene_active_ = false;

    SDL_SetRenderTarget(renderer, saved_target_);

    SDL_Window* window = SDL_GetRenderWindow(renderer);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    struct { float w, h, linear_mode, scale; } uniforms = {
        static_cast<float>(intermediate_w_),
        static_cast<float>(intermediate_h_),
        linear_colorspace_ ? 1.0f : 0.0f,
        density > 0.0f ? density : 1.0f
    };
    SDL_SetGPURenderStateFragmentUniforms(state_, 0, &uniforms, sizeof(uniforms));
    SDL_SetGPURenderState(renderer, state_);
    SDL_RenderTexture(renderer, intermediate_, nullptr, nullptr);
    SDL_SetGPURenderState(renderer, nullptr);
}
