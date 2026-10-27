#include "shader_background.hpp"

#include "user_shader.hpp"

#include <sys/stat.h>

#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {

int64_t mtime_of(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1000000000 + st.st_mtimespec.tv_nsec;
}

} // namespace

ShaderBackground::~ShaderBackground() { cleanup(); }

void ShaderBackground::release_gpu() {
    if (state_) SDL_DestroyGPURenderState(state_);
    state_ = nullptr;
    if (shader_ && device_) SDL_ReleaseGPUShader(device_, shader_);
    shader_ = nullptr;
}

void ShaderBackground::cleanup() {
    release_gpu();
    if (white_) SDL_DestroyTexture(white_);
    white_ = nullptr;
    device_ = nullptr;
}

bool ShaderBackground::load(SDL_Renderer* renderer, const std::string& path, bool linear_colorspace) {
    release_gpu();
    path_ = path;
    linear_colorspace_ = linear_colorspace;
    mtime_ns_ = mtime_of(path);
    last_check_ms_ = SDL_GetTicks();
    const std::string name = path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1);

    device_ = SDL_GetGPURendererDevice(renderer);
    if (!device_ || !(SDL_GetGPUShaderFormats(device_) & SDL_GPU_SHADERFORMAT_MSL)) {
        error_ = "shader backgrounds need the GPU renderer";
        device_ = nullptr;
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error_ = "can't read " + name;
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    const user_shader::Compiled c = user_shader::compile(text.str(), name);
    if (!c.ok) {
        error_ = c.error;
        return false;
    }

    SDL_GPUShaderCreateInfo info;
    SDL_zero(info);
    info.format = SDL_GPU_SHADERFORMAT_MSL;
    info.code = reinterpret_cast<const Uint8*>(c.msl.c_str());
    info.code_size = c.msl.size();
    info.entrypoint = "main0";
    info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    info.num_samplers = 1;
    info.num_uniform_buffers = 1;
    shader_ = SDL_CreateGPUShader(device_, &info);
    if (!shader_) {
        error_ = name + ": Metal didn't accept it: " + SDL_GetError();
        return false;
    }
    SDL_GPURenderStateCreateInfo state_info;
    SDL_zero(state_info);
    state_info.fragment_shader = shader_;
    state_ = SDL_CreateGPURenderState(renderer, &state_info);
    if (!state_) {
        error_ = name + ": " + SDL_GetError();
        release_gpu();
        return false;
    }
    if (!white_) {
        white_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 1, 1);
        if (white_) {
            const Uint32 pixel = 0xFFFFFFFF;
            SDL_UpdateTexture(white_, nullptr, &pixel, sizeof(pixel));
        }
    }
    error_.clear();
    return white_ != nullptr;
}

bool ShaderBackground::reload_if_changed(SDL_Renderer* renderer) {
    if (path_.empty()) return false;
    const uint64_t now = SDL_GetTicks();
    if (now - last_check_ms_ < 1000) return false;
    last_check_ms_ = now;
    const int64_t m = mtime_of(path_);
    if (m == mtime_ns_) return false;
    load(renderer, path_, linear_colorspace_);
    if (!error_.empty()) std::cerr << "Shader background: " << error_ << std::endl;
    return true;
}

void ShaderBackground::draw(SDL_Renderer* renderer, const Frame& f) {
    if (!state_ || !white_) return;
    int w = 0, h = 0;
    SDL_GetCurrentRenderOutputSize(renderer, &w, &h);

    user_shader::Inputs in;
    std::memset(&in, 0, sizeof(in));
    in.resolution[0] = static_cast<float>(w);
    in.resolution[1] = static_cast<float>(h);
    in.resolution[2] = 1.0f;
    in.time = f.time;
    in.time_delta = f.time_delta;
    in.frame = f.frame;
    in.frame_rate = f.time_delta > 0 ? 1.0f / f.time_delta : 60.0f;
    in.mouse[0] = f.mouse_x;
    in.mouse[1] = f.mouse_y;
    const std::time_t now = std::time(nullptr);
    std::tm local;
    localtime_r(&now, &local);
    in.date[0] = static_cast<float>(local.tm_year + 1900);
    in.date[1] = static_cast<float>(local.tm_mon);
    in.date[2] = static_cast<float>(local.tm_mday);
    in.date[3] = static_cast<float>(local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec);
    in.exposure = f.exposure;
    in.hue_degrees = f.hue_degrees;
    in.linear_output = linear_colorspace_ ? 1.0f : 0.0f;

    SDL_SetGPURenderStateFragmentUniforms(state_, 0, &in, sizeof(in));
    SDL_SetGPURenderState(renderer, state_);
    SDL_RenderTexture(renderer, white_, nullptr, nullptr);
    SDL_SetGPURenderState(renderer, nullptr);
}
