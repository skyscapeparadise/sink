#pragma once

#include <SDL3/SDL.h>

// A real CRT post-process pass (soft scanlines, halation/bloom glow, a
// gentle RGB fringe, warm phosphor tint, vignette) via a custom GPU
// fragment shader, replacing the old hard-edged-rectangle overlay -- which
// read as a rigid grid rather than a glowing tube. Same SDL_GPURenderState
// mechanism as HueShiftEffect; see hue_shift.hpp for why that's necessary.
//
// Unlike hue-shift (which reshades one texture draw), this wraps a whole
// scene: call begin_scene() before drawing the video background + terminal
// grid, then end_scene() right after, which composites everything drawn in
// between onto the real target with the CRT shader applied. Requires the
// renderer to have been created with the "gpu" backend; is_ready() reports
// whether that held and the shader compiled, so callers can fall back to
// something else (or nothing) when it didn't.
class CrtShaderEffect {
public:
    CrtShaderEffect();
    ~CrtShaderEffect();

    // `linear_colorspace` must match what the renderer was actually created
    // with (see create_terminal_window's colorspace comment in main.cpp).
    // It decides the offscreen buffer's precision: an 8-bit-per-channel
    // buffer silently clamps anything above 1.0 (e.g. hdr console's
    // brightness-boosted text) to plain white *before* this shader ever
    // sees it, which is what made CRT mode look blown-out/washed with hdr
    // console on -- a float buffer preserves those values through the
    // glow/scanline/vignette math instead.
    bool init(SDL_Renderer* renderer, bool linear_colorspace);
    void cleanup();

    bool is_ready() const { return state_ != nullptr; }

    // Redirects subsequent draws on `renderer` into an offscreen buffer.
    void begin_scene(SDL_Renderer* renderer);
    // Restores the real render target and composites the offscreen buffer
    // onto it through the CRT shader. No-op if begin_scene() didn't
    // actually redirect anything (e.g. this frame's texture allocation
    // failed) or wasn't called.
    void end_scene(SDL_Renderer* renderer);

private:
    SDL_GPUDevice* device_ = nullptr; // borrowed from the renderer, not owned
    SDL_GPUShader* shader_ = nullptr;
    SDL_GPURenderState* state_ = nullptr;

    SDL_Texture* intermediate_ = nullptr;
    int intermediate_w_ = 0;
    int intermediate_h_ = 0;
    SDL_Texture* saved_target_ = nullptr;
    bool scene_active_ = false;
    bool linear_colorspace_ = false;

    void ensure_intermediate(SDL_Renderer* renderer, int w, int h);
};
