#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

// The "shader" background: a user's .frag file (see user_shader.hpp for what
// it can be), drawn filling the window through SDL's GPU renderer, as
// background_shader.cpp draws the gradient and the gel.
//
// The file is watched: saving it recompiles it, so a shader can be worked on
// with sink open beside the editor. When it doesn't compile, error() says why
// and the caller draws something plain instead.
class ShaderBackground {
public:
    ShaderBackground() = default;
    ~ShaderBackground();
    ShaderBackground(const ShaderBackground&) = delete;
    ShaderBackground& operator=(const ShaderBackground&) = delete;

    // Compiles `path` for `renderer`. False, with error() set, if it can't.
    bool load(SDL_Renderer* renderer, const std::string& path, bool linear_colorspace);
    void cleanup();  // before the renderer is destroyed

    // Recompiles the file if it's changed since it was loaded, at most about
    // once a second. True when it did (whether or not that worked).
    bool reload_if_changed(SDL_Renderer* renderer);

    bool is_ready() const { return state_ != nullptr; }
    const std::string& path() const { return path_; }
    const std::string& error() const { return error_; }

    struct Frame {
        float time = 0;        // seconds since the background started
        float time_delta = 0;  // since the last frame
        int frame = 0;
        float mouse_x = 0, mouse_y = 0;  // pixels, from the bottom left
        float exposure = 1;
        float hue_degrees = 0;
    };
    // Fills the current render target.
    void draw(SDL_Renderer* renderer, const Frame& f);

private:
    SDL_GPUDevice* device_ = nullptr;  // borrowed from the renderer
    SDL_GPUShader* shader_ = nullptr;
    SDL_GPURenderState* state_ = nullptr;
    SDL_Texture* white_ = nullptr;
    std::string path_;
    std::string error_;
    bool linear_colorspace_ = false;
    int64_t mtime_ns_ = 0;
    uint64_t last_check_ms_ = 0;

    void release_gpu();
};
