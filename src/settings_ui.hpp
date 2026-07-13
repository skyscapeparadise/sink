#pragma once

#include <SDL3/SDL.h>
#include <string>
#include <vector>
#include "font_manager.hpp"

struct UIColors {
    SDL_FColor bg = {0.04f, 0.05f, 0.08f, 1.00f};          // Deep black-slate background
    SDL_FColor card = {1.00f, 1.00f, 1.00f, 0.02f};        // Translucent card container
    SDL_FColor border = {1.00f, 1.00f, 1.00f, 0.06f};      // Card border
    
    SDL_FColor text_primary = {0.95f, 0.95f, 0.95f, 1.0f};  // Bright text
    SDL_FColor text_secondary = {0.50f, 0.52f, 0.55f, 1.0f};// Dim text
    
    SDL_FColor btn_idle = {1.00f, 1.00f, 1.00f, 0.00f};     // Transparent idle background
    SDL_FColor btn_hover = {1.00f, 1.00f, 1.00f, 0.08f};    // Subtly lit white hover
    SDL_FColor btn_danger = {1.00f, 0.20f, 0.25f, 0.00f};   // Transparent red idle
    SDL_FColor btn_danger_hover = {1.00f, 0.20f, 0.25f, 0.10f}; // Translucent red hover
};

struct UIButton {
    int id;
    std::string label;
    SDL_FRect rect;
    SDL_FColor color;
    SDL_FColor hover_color;
    bool hovered = false;
};

struct UISlider {
    int id;             // 1 = Exposure
    std::string label;
    SDL_FRect rect;
    float value;        // 0.0f to 1.0f (slider knob percent)
    float min_val;
    float max_val;
};

class SettingsUI {
public:
    SettingsUI();
    ~SettingsUI();

    bool open(SDL_Window* parent_window);
    void close();
    bool is_open() const { return window_ != nullptr; }

    void process_event(const SDL_Event& event);
    void render();

    SDL_Window* get_window() const { return window_; }
    SDL_Renderer* get_renderer() const { return renderer_; }

    void set_paths(const std::string& bg_path, const std::string& font_path);
    void set_animated_typing(bool enabled);
    void set_broadcasting(bool enabled);
    bool get_broadcasting() const { return broadcasting_; }
    
    void set_exposure(float exposure);
    float get_exposure() const { return exposure_; }

private:
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Window* parent_ = nullptr;
    
    UIColors colors_;
    std::vector<UIButton> buttons_;
    std::vector<UISlider> sliders_;
    
    std::string bg_path_ = "None";
    std::string font_path_ = "Default";
    bool animated_typing_ = true;
    bool broadcasting_ = false;
    float exposure_ = 1.0f;

    bool dragging_ = false;
    int active_slider_id_ = 0;

    FontManager font_manager_;

    SDL_Texture* sink_logo_ = nullptr;
    SDL_Texture* rain_logo_ = nullptr;

    void init_layout();
    void update_slider_value(int slider_id, float mouse_x);
    void draw_text(const std::string& text, float x, float y, const SDL_FColor& color, bool monospace = false);
    void draw_rect_filled(const SDL_FRect& rect, const SDL_FColor& color, float radius = 0.0f);
    void draw_rect_outline(const SDL_FRect& rect, const SDL_FColor& color, float radius = 0.0f);
};
