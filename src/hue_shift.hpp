#pragma once

#include <SDL3/SDL.h>

// Per-pixel video-background-only color effects -- hue rotation and an
// NTSC-composite-style chroma bleed -- applied via a custom GPU fragment
// shader, using SDL3's SDL_GPURenderState API (3.4.0+). This is the one
// draw in the app that needs an actual shader: SDL's 2D renderer can only
// scale RGB channels (SDL_SetTextureColorMod/SDL_SetRenderColorScale,
// which exposure uses), not rotate hue or blur chroma -- those are
// per-pixel/neighboring-pixel operations.
//
// Both effects apply only to this one texture draw (the video background),
// never to the terminal text drawn on top of it afterward.
//
// Requires the renderer to have been created with the "gpu" backend
// (SDL_GPU_RENDERER); init() fails gracefully (returns false, logs why) on
// any other backend, and draw() falls back to a plain, unmodified draw so
// callers don't need to branch on whether the effect is actually available.
class HueShiftEffect {
public:
    HueShiftEffect();
    ~HueShiftEffect();

    bool init(SDL_Renderer* renderer, bool linear_colorspace);
    void cleanup();

    bool is_ready() const { return state_ != nullptr; }

    // Draws `source` (stretched to fill the current render target, same as
    // a plain SDL_RenderTexture(renderer, source, nullptr, nullptr)) with
    // `color_scale` applied (same role as exposure's SDL_SetRenderColorScale),
    // an NTSC-style chroma bleed applied if `ntsc_enabled`, and its hue
    // rotated by `hue_degrees` (0 = no rotation).
    //
    // `source` is commonly a YUV-format streaming video texture, which SDL's
    // *default* draw path knows how to convert to RGB but a custom fragment
    // shader plugged in via SDL_GPURenderState does not (it just samples
    // whatever raw plane bytes the texture holds) -- so this renders
    // `source` normally into an offscreen RGB target first, then applies the
    // shader as a second pass on that already-RGB result.
    void draw(SDL_Renderer* renderer, SDL_Texture* source, float hue_degrees, float color_scale, bool ntsc_enabled);

private:
    SDL_GPUDevice* device_ = nullptr; // borrowed from the renderer, not owned
    SDL_GPUShader* shader_ = nullptr;
    SDL_GPURenderState* state_ = nullptr;
    bool linear_colorspace_ = false;

    // Offscreen RGB target the source is drawn into before the shader pass;
    // (re)created on demand to match the renderer's current output size.
    SDL_Texture* intermediate_ = nullptr;
    int intermediate_w_ = 0;
    int intermediate_h_ = 0;

    void ensure_intermediate(SDL_Renderer* renderer, int w, int h);
};
