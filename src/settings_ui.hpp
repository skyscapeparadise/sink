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
    PRESET_BTN_SAVE = 22,
    PRESET_BTN_RENAME = 23,
    PRESET_BTN_DELETE = 24,
    THEME_BTN = 25,           // click to open the theme list
    BG_KIND_BTN = 26,         // click to open the background kinds
    FIT_BTN = 27,             // click to cycle how a video or image fits the window
    // background = "color": its swatch, and putting the theme's color back.
    COLOR_SWATCH = 30,
    COLOR_RESET = 31,
    // background = "gradient": a swatch per stop (GRADIENT_STOP + index),
    // adding and removing stops, and the angle.
    GRADIENT_STOP = 40,       // 40..47
    GRADIENT_REMOVE = 48,
    GRADIENT_ADD = 49,
    GRADIENT_ANGLE = 50,
    // The text's color (colors.foreground): its swatch, and the theme's back.
    TEXT_SWATCH = 51,
    TEXT_RESET = 52,
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
    bool toggle = false;  // drawn as a checkbox with its label, `on` its state
    bool on = false;
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
    // The shader background's .frag file (empty when none has been picked),
    // shown beside the type button when that's the kind.
    void set_shader_file(const std::string& path);
    // Opens the file picker for a shader; the choice comes back as user
    // event 19.
    void pick_shader_file();
    // The same for an image background: shown beside the type button, and a
    // picker whose choice comes back as user event 20.
    void set_image_file(const std::string& path);
    // And for a video background (event 1 brings back the file).
    void set_video_file(const std::string& path);
    void pick_video_file();
    void pick_image_file();
    static std::string select_label(const std::string& kind);
    // How a video or image background meets the window: "stretch", "fill"
    // or "fit". Clicking the button moves to the next; the choice comes back
    // as user event 21.
    void set_background_fit(const std::string& fit);
    // The color background's color, and whether it's the preset's own rather
    // than the theme's. Its swatch opens the color picker (see ColorPicker);
    // "use theme's color" is user event 23.
    void set_color_background(const SDL_FColor& color, bool custom);
    // The gradient's stops (2 to 8) and angle in degrees. A stop's swatch
    // opens the color picker for it; - and + are user event 24 (data1 -1 or
    // +1); the angle, event 25 (data1 the new angle).
    void set_gradient(const std::vector<SDL_FColor>& stops, int angle_degrees);
    // The text's color, and whether it's the preset's own. Its swatch opens
    // the color picker (event 22, tag kTextColorTag); "use theme's color" is
    // user event 26.
    void set_text_color(const SDL_FColor& color, bool custom);
    static constexpr int kTextColorTag = 100;
    void set_animated_typing(bool enabled);
    void set_broadcasting(bool enabled);
    bool get_broadcasting() const { return broadcasting_; }
    
    void set_exposure(float exposure);
    float get_exposure() const { return exposure_; }
    // Frosted glass's roughness (0..1): a slider of its own, shown for it.
    void set_glass_roughness(float roughness);
    float get_glass_roughness() const { return glass_roughness_; }

    void set_hue_shift(float degrees);
    float get_hue_shift() const { return hue_shift_; }

    void set_vibrancy_enabled(bool enabled);
    bool get_vibrancy_enabled() const { return vibrancy_enabled_; }

    void set_crt_effect_enabled(bool enabled);
    bool get_crt_effect_enabled() const { return crt_effect_enabled_; }

    void set_ligatures_enabled(bool enabled);
    bool get_ligatures_enabled() const { return ligatures_enabled_; }

    void set_hdr_console_enabled(bool enabled);
    // [shell] integration: not part of a preset, and only for shells started
    // after it changes.
    void set_shell_integration_enabled(bool enabled);
    bool get_shell_integration_enabled() const { return shell_integration_enabled_; }
    bool get_hdr_console_enabled() const { return hdr_console_enabled_; }

    // Preset list + currently active preset, shown in the preset row.
    // `names` should already be sorted the way it should display ("pool" first).
    void set_preset_names(const std::vector<std::string>& names);
    void set_active_preset(const std::string& name);
    // Whether Settings has changes on top of the active preset (sink.toml's
    // [edits]): shown as "(edited)", and what "save" saves. Picking a preset
    // from the list, even the active one, drops them.
    void set_preset_edited(bool edited);
    // Presets that ship with sink: marked in the list, and not renamable,
    // deletable or savable into.
    void set_builtin_presets(const std::vector<std::string>& names);
    // Presets that come only from an included file: tagged and locked the same way.
    void set_included_presets(const std::vector<std::string>& names);
    // One line at the bottom of the window about sink.toml, such as a syntax
    // error that is stopping changes from being saved. Empty hides it.
    void set_config_notice(const std::string& text);
    void set_toggle(int id, bool on);
    // The bundled themes and the one in use, for the theme button and its list.
    void set_themes(const std::vector<std::string>& names, const std::string& active);

private:
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Window* parent_ = nullptr;
    
    UIColors colors_;
    std::vector<UIButton> buttons_;
    std::vector<UISlider> sliders_;
    
    std::string bg_path_ = "None";
    std::string shader_file_;
    std::string image_file_;
    std::string video_file_;
    std::string background_fit_ = "stretch";
    SDL_FColor bg_color_{0, 0, 0, 1};
    bool bg_color_custom_ = false;
    std::vector<SDL_FColor> gradient_stops_;
    SDL_FColor text_color_{1, 1, 1, 1};
    bool text_color_custom_ = false;
    int gradient_angle_ = 45;
    bool shown(int id) const;        // whether a button belongs to the background kind showing
    void change_angle(int delta);

    // sink's own color picker, drawn over Settings beside the swatch that
    // opened it -- the same on every platform, rather than each one's own.
    // A saturation/brightness square, a hue strip and a hex field; every
    // change is user event 22, with `tag` (0 the color background, 1 + n a
    // gradient's stop n) in data1 and the color as 0xRRGGBB in data2.
    struct ColorPicker {
        bool open = false;
        int tag = 0;
        float h = 0, s = 0, v = 0;   // hue 0..360, saturation and value 0..1
        float x = 0, y = 0;          // top left, in points
        bool drag_sv = false, drag_hue = false;
        bool editing_hex = false;
        std::string hex;             // the field's text while it's being typed in
    };
    ColorPicker picker_;
    static constexpr float kPickerW = 236.0f, kPickerH = 208.0f;
    // Opens beside `anchor` (a swatch): below it, or above when below would
    // run off the window.
    void open_picker(int tag, const SDL_FColor& color, const SDL_FRect& anchor);
    void close_picker();
    SDL_FRect picker_rect() const { return { picker_.x, picker_.y, kPickerW, kPickerH }; }
    SDL_FRect picker_sv_rect() const { return { picker_.x + 10, picker_.y + 10, 182, 150 }; }
    SDL_FRect picker_hue_rect() const { return { picker_.x + 200, picker_.y + 10, 26, 150 }; }
    SDL_FRect picker_hex_rect() const { return { picker_.x + 10, picker_.y + 170, 118, 28 }; }
    SDL_FRect picker_done_rect() const { return { picker_.x + 164, picker_.y + 170, 62, 28 }; }
    SDL_FColor picker_color() const;
    void picker_drag(float mx, float my);  // follows a drag in the square or the strip
    void picker_send();                    // the color as it stands, as event 22
    bool picker_event(const SDL_Event& event);  // true when the picker took it
    void draw_picker(float scale);
    bool fit_applies() const;  // the background is a video or an image
    // Exposure and hue shift: for videos, images, shaders and the gel -- not
    // for colors picked outright (color, gradient) or the desktop (glass).
    bool look_sliders_apply() const;
    std::string font_path_ = "Default";
    bool animated_typing_ = true;
    bool broadcasting_ = false;
    float exposure_ = 1.0f;
    float glass_roughness_ = 0.5f;
    float hue_shift_ = 0.0f;
    bool vibrancy_enabled_ = true;
    bool crt_effect_enabled_ = false;
    bool ligatures_enabled_ = true;
    bool hdr_console_enabled_ = false;
    bool shell_integration_enabled_ = true;

    bool dragging_ = false;
    int active_slider_id_ = 0;

    FontManager font_manager_;

    SDL_Texture* sink_logo_ = nullptr;
    SDL_Texture* rain_logo_ = nullptr;

    // Preset row state
    std::vector<std::string> preset_names_ = {"pool"};
    std::string active_preset_ = "pool";
    bool preset_edited_ = false;
    bool is_sink_preset(const std::string& name) const;  // ships with sink
    std::string preset_label(float max_w);
    std::vector<std::string> builtin_presets_ = {"pool"};
    std::vector<std::string> included_presets_;
    bool is_included(const std::string& name) const;
    std::string config_notice_;
    // A list that drops open from a button; picking a row sends `user_code`
    // with the row's text (strdup'd) as data1.
    struct DropList {
        std::vector<std::string> items;
        std::string active;
        bool open = false;
        int hover = -1;
        float x = 0, y = 0, w = 220;  // the edge it opens from
        bool upward = false;
        int user_code = 0;
        SDL_FRect row(int i) const {
            const float n = static_cast<float>(items.size());
            return { x, upward ? y - (n - i) * 22.0f : y + i * 22.0f, w, 22.0f };
        }
        int at(float mx, float my) const {
            for (int i = 0; i < static_cast<int>(items.size()); ++i) {
                SDL_FRect r = row(i);
                if (mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h) return i;
            }
            return -1;
        }
    };
    DropList theme_list_;       // opens upward from the theme button: no room below
    DropList background_list_;  // opens down from the background type button
    static const std::vector<std::string> kBackgroundKinds;
    std::string background_kind_label() const;
    void draw_list(const DropList& list, float scale);
    // Where text goes so it sits centered in a row that starts at `y` and is
    // `h` tall, both in points; returns pixels.
    float text_y(float y, float h, float scale) const;
    // Handles a click while `list` is open: picks a row, or just closes it.
    void click_list(DropList& list, float mx, float my);
    bool is_builtin(const std::string& name) const;
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
    // The background card's second row -- fit, angle, roughness, "use
    // theme's color" -- is there only for kinds with one; without it,
    // everything below moves up by kKindRowH and the window is that much
    // shorter. Y() places something laid out as if the row were there.
    static constexpr float kKindRowH = 36.0f;
    static constexpr float kWindowH = 640.0f;
    float shift_ = 0.0f;
    bool has_kind_row() const;
    float Y(float y) const { return y >= 186.0f ? y + shift_ : y; }
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
