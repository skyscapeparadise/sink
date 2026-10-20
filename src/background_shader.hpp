#pragma once

#include <SDL3/SDL.h>

// The procedural backgrounds -- a gradient, and the gel -- drawn by one GPU
// fragment shader filling the window, through SDL3's SDL_GPURenderState as
// hue_shift.cpp does. The gel is the nebula from photoscoping's inspector
// panel (src/gpu/shaders/nebula.frag there): domain-warped fractal noise in
// five colors, dust lanes and a few breathing stars, slowly churning.
//
// Needs the "gpu" renderer backend; without it init() returns false and the
// caller draws a solid color instead.
class BackgroundShader {
public:
    enum class Mode { Gradient = 0, Gel = 1 };

    struct Params {
        Mode mode = Mode::Gradient;
        SDL_FColor colors[8] = {};  // sRGB; gradient stops, or the gel's five
        int count = 2;              // gradient stops in use
        float angle_degrees = 45;   // gradient direction: 0 left to right, 90 top to bottom
        float time = 0;             // gel: seconds of churn so far
        float exposure = 1;         // brightness scale, as for video
        float hue_degrees = 0;      // hue rotation, as for video
        float logical_w = 0, logical_h = 0;  // window size in points
    };

    BackgroundShader() = default;
    ~BackgroundShader();
    BackgroundShader(const BackgroundShader&) = delete;
    BackgroundShader& operator=(const BackgroundShader&) = delete;

    bool init(SDL_Renderer* renderer, bool linear_colorspace);
    void cleanup();
    bool is_ready() const { return state_ != nullptr; }

    // Fills the current render target.
    void draw(SDL_Renderer* renderer, const Params& params);

private:
    SDL_GPUDevice* device_ = nullptr;  // borrowed from the renderer
    SDL_GPUShader* shader_ = nullptr;
    SDL_GPURenderState* state_ = nullptr;
    SDL_Texture* white_ = nullptr;     // what the full-window draw samples (ignored)
    bool linear_colorspace_ = false;
};
