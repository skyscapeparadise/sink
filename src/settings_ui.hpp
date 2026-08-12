#pragma once

#include <SDL3/SDL.h>
#include <string>
#include <vector>
#include "font_manager.hpp"

// Button ids used for the preset row. Kept out of the 1-9 range used by the
// background/font/toggle controls above.
enum PresetButtonId {
    PRESET_BTN_NAME = 20,     // click to open the preset dropdown
    PRESET_BTN_NEW = 21,
    PRESET_BTN_DUPLICATE = 22,
    PRESET_BTN_RENAME = 23,
    PRESET_BTN_DELETE = 24,
};

// What a committed name-edit should do once the user presses Enter.
enum class PresetEditMode { kNone, kNew, kRename };

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

    void set_vibrancy_enabled(bool enabled);
    bool get_vibrancy_enabled() const { return vibrancy_enabled_; }

    void set_crt_effect_enabled(bool enabled);
    bool get_crt_effect_enabled() const { return crt_effect_enabled_; }

    void set_ligatures_enabled(bool enabled);
    bool get_ligatures_enabled() const { return ligatures_enabled_; }

    // Preset list + currently active preset, shown in the preset row.
    // `names` should already be sorted the way it should display ("pool" first).
    void set_preset_names(const std::vector<std::string>& names);
    void set_active_preset(const std::string& name);

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
    bool vibrancy_enabled_ = true;
    bool crt_effect_enabled_ = false;
    bool ligatures_enabled_ = true;

    bool dragging_ = false;
    int active_slider_id_ = 0;

    FontManager font_manager_;

    SDL_Texture* sink_logo_ = nullptr;
    SDL_Texture* rain_logo_ = nullptr;

    // Preset row state
    std::vector<std::string> preset_names_ = {"pool"};
    std::string active_preset_ = "pool";
    bool preset_dropdown_open_ = false;
    int dropdown_hover_index_ = -1;
    static constexpr int kMaxDropdownRows = 8;

    // Inline text editing, used for both "new preset" and "rename preset".
    // Committing pushes an SDL_EVENT_USER (see .cpp for codes); the caller
    // (main.cpp) owns actually creating/renaming/switching the preset.
    bool editing_name_ = false;
    PresetEditMode edit_mode_ = PresetEditMode::kNone;
    std::string edit_buffer_;
    bool edit_save_hovered_ = false;
    bool edit_cancel_hovered_ = false;

    void init_layout();
    void update_slider_value(int slider_id, float mouse_x);
    void draw_text(const std::string& text, float x, float y, const SDL_FColor& color, bool monospace = false);
    void draw_rect_filled(const SDL_FRect& rect, const SDL_FColor& color, float radius = 0.0f);
    void draw_rect_outline(const SDL_FRect& rect, const SDL_FColor& color, float radius = 0.0f);

    // Measures rendered text width in the same (physical/scaled) pixel
    // space as glyph advances, since the font is loaded at a size that
    // already bakes in the display scale.
    float measure_text_width(const std::string& text);
    // Truncates from the front (keeping the tail, prefixed with "...") so
    // the result fits within max_width -- used for paths/filenames, where
    // the identifying part is usually at the end.
    std::string truncate_head(const std::string& text, float max_width);

    SDL_FRect dropdown_row_rect(int index) const;
    // Layout for the name-edit row: text field, then explicit "save"/
    // "cancel" buttons (mouse-discoverable alternative to Enter/Escape).
    SDL_FRect edit_field_rect() const;
    SDL_FRect edit_save_rect() const;
    SDL_FRect edit_cancel_rect() const;
    void begin_edit(PresetEditMode mode, const std::string& initial_text);
    void cancel_edit();
    void commit_edit();
};
