#include "settings_ui.hpp"
#include <iostream>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <SDL3_image/SDL_image.h>

#if defined(__APPLE__)
#include "macos_menu.h"
#endif

// Static callbacks for file dialogs.
static void background_dialog_callback(void* userdata, const char* const* filelist, int filter) {
    if (filelist && *filelist) {
        SDL_Event ev;
        std::memset(&ev, 0, sizeof(ev));
        ev.type = SDL_EVENT_USER;
        ev.user.code = 1; // Background path selected
        ev.user.data1 = strdup(*filelist);
        SDL_PushEvent(&ev);
    }
}

static void shader_dialog_callback(void* userdata, const char* const* filelist, int filter) {
    if (filelist && *filelist) {
        SDL_Event ev;
        std::memset(&ev, 0, sizeof(ev));
        ev.type = SDL_EVENT_USER;
        ev.user.code = 19; // Shader file selected
        ev.user.data1 = strdup(*filelist);
        SDL_PushEvent(&ev);
    }
}

static void image_dialog_callback(void* userdata, const char* const* filelist, int filter) {
    if (filelist && *filelist) {
        SDL_Event ev;
        std::memset(&ev, 0, sizeof(ev));
        ev.type = SDL_EVENT_USER;
        ev.user.code = 20; // Image file selected
        ev.user.data1 = strdup(*filelist);
        SDL_PushEvent(&ev);
    }
}

static void font_dialog_callback(void* userdata, const char* const* filelist, int filter) {
    if (filelist && *filelist) {
        SDL_Event ev;
        std::memset(&ev, 0, sizeof(ev));
        ev.type = SDL_EVENT_USER;
        ev.user.code = 2; // Font path selected
        ev.user.data1 = strdup(*filelist);
        SDL_PushEvent(&ev);
    }
}

SettingsUI::SettingsUI() {}

SettingsUI::~SettingsUI() {
    close();
}

bool SettingsUI::open(SDL_Window* parent_window) {
    if (window_) return true; // Already open

    parent_ = parent_window;
    
    // Create settings window (500x640, High-DPI enabled)
    window_ = SDL_CreateWindow(
        "sink settings",
        500, static_cast<int>(kWindowH + (has_kind_row() ? 0.0f : -kKindRowH)),
        SDL_WINDOW_HIGH_PIXEL_DENSITY
    );

    if (!window_) {
        std::cerr << "Failed to create Settings window: " << SDL_GetError() << std::endl;
        return false;
    }

    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (!renderer_) {
        std::cerr << "Failed to create Settings renderer: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        return false;
    }

    // Load local font context for text rendering. Mona Sans (bundled under
    // fonts/, SIL OFL licensed -- see fonts/MonaSans-OFL.txt) is the intended
    // settings UI font; fall back to the main terminal's bundled font, then a
    // system font, so this still renders for every user even if it's missing.
    float scale = SDL_GetWindowDisplayScale(window_);
    if (scale <= 0.0f) scale = 1.0f;
    std::string font_path = get_bundle_resource_path("MonaSans-VariableFont.ttf");
    if (!font_manager_.load_font(renderer_, font_path, 13.0f * scale)) {
        font_path = "fonts/MonaSans-VariableFont.ttf";
        if (!font_manager_.load_font(renderer_, font_path, 13.0f * scale)) {
            font_path = get_bundle_resource_path("MonaspaceNeon-Regular.otf");
            if (!font_manager_.load_font(renderer_, font_path, 13.0f * scale)) {
                font_path = "fonts/MonaspaceNeon-Regular.otf";
                if (!font_manager_.load_font(renderer_, font_path, 13.0f * scale)) {
                    font_path = "/System/Library/Fonts/SFNSMono.ttf";
                    font_manager_.load_font(renderer_, font_path, 13.0f * scale);
                }
            }
        }
    }

    // Load SVG logo textures
    std::string sink_logo_path = "logos/sinklogo.svg";
    std::string rain_logo_path = "logos/rainlogo.svg";
#if defined(__APPLE__)
    std::string resolved_sink = get_bundle_resource_path("sinklogo.svg");
    FILE* f_sl = fopen(resolved_sink.c_str(), "r");
    if (f_sl) { fclose(f_sl); sink_logo_path = resolved_sink; }
    std::string resolved_rain = get_bundle_resource_path("rainlogo.svg");
    FILE* f_rl = fopen(resolved_rain.c_str(), "r");
    if (f_rl) { fclose(f_rl); rain_logo_path = resolved_rain; }
#endif

    sink_logo_ = IMG_LoadTexture(renderer_, sink_logo_path.c_str());
    if (sink_logo_) {
        SDL_SetTextureBlendMode(sink_logo_, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(sink_logo_, SDL_SCALEMODE_LINEAR);
    } else {
        std::cerr << "Failed to load sink logo: " << SDL_GetError() << std::endl;
    }

    // Rasterize the rain logo near its on-screen height (with 2x headroom
    // for supersampling) instead of at its native ~1280px viewBox width.
    // Loading it full-size and then shrinking it ~19x via plain bilinear
    // filtering (no mipmaps) aliases badly on the logo's thin strokes.
    {
        int rain_target_h = static_cast<int>(36.0f * scale * 2.0f);
        SDL_IOStream* rain_io = SDL_IOFromFile(rain_logo_path.c_str(), "rb");
        if (rain_io) {
            SDL_Surface* rain_surf = IMG_LoadSizedSVG_IO(rain_io, 0, rain_target_h);
            SDL_CloseIO(rain_io);
            if (rain_surf) {
                rain_logo_ = SDL_CreateTextureFromSurface(renderer_, rain_surf);
                SDL_DestroySurface(rain_surf);
            }
        }
    }
    if (!rain_logo_) {
        rain_logo_ = IMG_LoadTexture(renderer_, rain_logo_path.c_str());
    }
    if (rain_logo_) {
        SDL_SetTextureBlendMode(rain_logo_, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(rain_logo_, SDL_SCALEMODE_LINEAR);
    } else {
        std::cerr << "Failed to load rain logo: " << SDL_GetError() << std::endl;
    }

    init_layout();
    return true;
}

void SettingsUI::close() {
    close_picker();
    if (editing_name_ && window_) {
        SDL_StopTextInput(window_);
    }
    editing_name_ = false;
    preset_dropdown_open_ = false;
    font_manager_.cleanup();
    if (sink_logo_) {
        SDL_DestroyTexture(sink_logo_);
        sink_logo_ = nullptr;
    }
    if (rain_logo_) {
        SDL_DestroyTexture(rain_logo_);
        rain_logo_ = nullptr;
    }
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    dragging_ = false;
}

void SettingsUI::set_paths(const std::string& bg_path, const std::string& font_path) {
    bg_path_ = bg_path.empty() ? "none" : bg_path;
    font_path_ = font_path.empty() ? "default" : font_path;
    const std::string kind = background_kind_label();
    background_list_.active = kind;
    for (auto& btn : buttons_) {
        if (btn.id == BG_KIND_BTN) btn.label = kind;
        if (btn.id == 1) btn.label = select_label(kind);
    }
    // A kind with controls of its own has a row for them; the others don't,
    // and everything below moves up rather than leave a gap.
    const float shift = has_kind_row() ? 0.0f : -kKindRowH;
    if (window_ && shift != shift_) {
        init_layout();
        SDL_SetWindowSize(window_, 500, static_cast<int>(kWindowH + shift_));
    }
}

bool SettingsUI::has_kind_row() const {
    const std::string kind = background_kind_label();
    return kind == "video" || kind == "image" || kind == "color" || kind == "gradient" || kind == "frosted glass";
}

void SettingsUI::set_shader_file(const std::string& path) {
    shader_file_ = path;
}

void SettingsUI::set_video_file(const std::string& path) {
    video_file_ = path;
}

void SettingsUI::pick_video_file() {
    if (!window_) return;
    SDL_DialogFileFilter filters[] = { {"Videos", "mp4;mov;m4v;mkv;webm;avi"} };
    SDL_ShowOpenFileDialog(background_dialog_callback, nullptr, window_, filters, 1, nullptr, false);
}

void SettingsUI::set_image_file(const std::string& path) {
    image_file_ = path;
}

void SettingsUI::set_background_fit(const std::string& fit) {
    background_fit_ = fit;
    for (auto& btn : buttons_) {
        if (btn.id == FIT_BTN) btn.label = "fit: " + background_fit_;
    }
}

void SettingsUI::set_color_background(const SDL_FColor& color, bool custom) {
    bg_color_ = color;
    bg_color_custom_ = custom;
}

void SettingsUI::set_text_color(const SDL_FColor& color, bool custom) {
    text_color_ = color;
    text_color_custom_ = custom;
}

void SettingsUI::set_gradient(const std::vector<SDL_FColor>& stops, int angle_degrees) {
    gradient_stops_ = stops;
    gradient_angle_ = ((angle_degrees % 360) + 360) % 360;
    for (auto& btn : buttons_) {
        if (btn.id == GRADIENT_ANGLE) btn.label = "angle: " + std::to_string(gradient_angle_) + " deg";
    }
}

// The controls beside the background's type button depend on the type: the
// file button for videos, images and shaders, swatches for the color and the
// gradient.
bool SettingsUI::shown(int id) const {
    const std::string kind = background_kind_label();
    const bool color = kind == "color", gradient = kind == "gradient";
    // The file button is for kinds that come from a file: videos (pool being
    // the built-in one), images and shaders.
    if (id == 1) return kind == "video" || kind == "image" || kind == "shader";
    if (id == FIT_BTN) return fit_applies();  // only videos and images have a shape to fit
    if (id == COLOR_SWATCH || id == COLOR_RESET) return color;
    if (id >= GRADIENT_STOP && id < GRADIENT_STOP + 8) {
        return gradient && id - GRADIENT_STOP < static_cast<int>(gradient_stops_.size());
    }
    if (id == GRADIENT_REMOVE || id == GRADIENT_ADD || id == GRADIENT_ANGLE) return gradient;
    return true;
}

void SettingsUI::change_angle(int delta) {
    const int next = (((gradient_angle_ + delta) % 360) + 360) % 360;
    set_gradient(gradient_stops_, next);
    SDL_Event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.type = SDL_EVENT_USER;
    ev.user.code = 25; // Gradient angle
    ev.user.data1 = reinterpret_cast<void*>(static_cast<intptr_t>(next));
    SDL_PushEvent(&ev);
}

bool SettingsUI::look_sliders_apply() const {
    const std::string kind = background_kind_label();
    return kind == "pool" || kind == "video" || kind == "image" || kind == "shader" || kind == "gel";
}

bool SettingsUI::fit_applies() const {
    const std::string kind = background_kind_label();
    return kind == "video" || kind == "image";
}

void SettingsUI::pick_image_file() {
    if (!window_) return;
    SDL_DialogFileFilter filters[] = { {"Pictures", "jpg;jpeg;png;heic;heif;webp;tif;tiff;gif;bmp;avif;jxl"} };
    SDL_ShowOpenFileDialog(image_dialog_callback, nullptr, window_, filters, 1, nullptr, false);
}

// What the file button beside the type button says, for that kind.
std::string SettingsUI::select_label(const std::string& kind) {
    if (kind == "shader") return "select shader...";
    if (kind == "image") return "select image...";
    return "select video...";
}

void SettingsUI::pick_shader_file() {
    if (!window_) return;
    SDL_DialogFileFilter filters[] = { {"GLSL shaders", "frag;glsl;fs"} };
    SDL_ShowOpenFileDialog(shader_dialog_callback, nullptr, window_, filters, 1, nullptr, false);
}

// The background kinds the list offers, grouped by where the picture comes
// from: sink's own scenes ("pool" is the built-in video), your files, colors
// you pick, and the desktop.
const std::vector<std::string> SettingsUI::kBackgroundKinds = {
    "pool", "gel", "video", "image", "shader", "color", "gradient", "clear", "frosted glass",
};

// What the type button says: the kind, "pool" for the built-in video, or
// "video" for one picked from a file.
std::string SettingsUI::background_kind_label() const {
    std::string lower = bg_path_;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.find("sinkpool.mp4") != std::string::npos || lower == "default" || lower == "pool") return "pool";
    for (const std::string& k : kBackgroundKinds) {
        if (lower == k) return k;
    }
    return "video";
}

void SettingsUI::set_toggle(int id, bool on) {
    for (auto& btn : buttons_) {
        if (btn.id == id) btn.on = on;
    }
}

void SettingsUI::set_animated_typing(bool enabled) {
    animated_typing_ = enabled;
    set_toggle(5, enabled);
}

void SettingsUI::set_broadcasting(bool enabled) {
    broadcasting_ = enabled;
    set_toggle(6, enabled);
}

void SettingsUI::set_exposure(float exposure) {
    exposure_ = exposure;
    for (auto& s : sliders_) {
        if (s.id == 1) {
            s.value = std::clamp(exposure_ / 2.0f, 0.0f, 1.0f);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "exposure: %.2f", exposure_);
            s.label = buf;
            break;
        }
    }
}

static std::string roughness_label(float r) {
    return "roughness: " + std::to_string(static_cast<int>(std::lround(r * 100.0f))) + "%";
}

void SettingsUI::set_glass_roughness(float roughness) {
    glass_roughness_ = std::clamp(roughness, 0.0f, 1.0f);
    for (auto& s : sliders_) {
        if (s.id == 3) {
            s.value = glass_roughness_;
            s.label = roughness_label(glass_roughness_);
        }
    }
}

void SettingsUI::set_hue_shift(float degrees) {
    hue_shift_ = degrees;
    for (auto& s : sliders_) {
        if (s.id == 2) {
            s.value = std::clamp(hue_shift_ / 360.0f, 0.0f, 1.0f);
            char buf[32];
            // Plain ASCII "deg" rather than the (multi-byte UTF-8) degree
            // sign -- draw_text() walks the string byte-by-byte, not
            // UTF-8-aware, so a multi-byte glyph here would render broken.
            std::snprintf(buf, sizeof(buf), "hue shift: %.0f deg", hue_shift_);
            s.label = buf;
            break;
        }
    }
}

void SettingsUI::set_vibrancy_enabled(bool enabled) {
    vibrancy_enabled_ = enabled;
    set_toggle(7, enabled);
}

void SettingsUI::set_crt_effect_enabled(bool enabled) {
    crt_effect_enabled_ = enabled;
    set_toggle(8, enabled);
}

void SettingsUI::set_ligatures_enabled(bool enabled) {
    ligatures_enabled_ = enabled;
    set_toggle(9, enabled);
}

void SettingsUI::set_hdr_console_enabled(bool enabled) {
    hdr_console_enabled_ = enabled;
    set_toggle(10, enabled);
}

void SettingsUI::set_shell_integration_enabled(bool enabled) {
    shell_integration_enabled_ = enabled;
    set_toggle(11, enabled);
}

void SettingsUI::set_preset_names(const std::vector<std::string>& names) {
    preset_names_ = names.empty() ? std::vector<std::string>{"pool"} : names;
}

void SettingsUI::set_builtin_presets(const std::vector<std::string>& names) {
    builtin_presets_ = names;
}

void SettingsUI::set_config_notice(const std::string& text) {
    config_notice_ = text;
}

void SettingsUI::set_themes(const std::vector<std::string>& names, const std::string& active) {
    theme_list_.items = names;
    theme_list_.active = active;
    const std::string label = "theme: " + active;
    for (auto& btn : buttons_) {
        if (btn.id == THEME_BTN) btn.label = label;
    }
}

float SettingsUI::text_y(float y, float h, float scale) const {
    return (y + h / 2.0f) * scale - font_manager_.get_cell_height() / 2.0f;
}

void SettingsUI::draw_list(const DropList& list, float scale) {
    const int count = static_cast<int>(list.items.size());
    if (count == 0) return;
    SDL_FRect first = list.row(0);
    SDL_FRect list_bg = { first.x * scale, first.y * scale, first.w * scale, (count * 22.0f) * scale };
    draw_rect_filled(list_bg, SDL_FColor{0.06f, 0.08f, 0.10f, 0.98f}, 6.0f * scale);
    draw_rect_outline(list_bg, colors_.border, 6.0f * scale);
    for (int i = 0; i < count; ++i) {
        SDL_FRect row = list.row(i);
        SDL_FRect phys_row = { row.x * scale, row.y * scale, row.w * scale, row.h * scale };
        const bool is_active = list.items[i] == list.active;
        const bool is_hovered = i == list.hover;
        if (is_hovered || is_active) {
            SDL_FRect highlight = { phys_row.x + 3.0f * scale, phys_row.y + 2.0f * scale,
                                    phys_row.w - 6.0f * scale, phys_row.h - 4.0f * scale };
            draw_rect_filled(highlight, is_hovered ? SDL_FColor{1.0f, 1.0f, 1.0f, 0.10f}
                                                   : SDL_FColor{0.0f, 0.8f, 0.8f, 0.15f}, 4.0f * scale);
        }
        draw_text(list.items[i], phys_row.x + 10.0f * scale,
                  phys_row.y + (phys_row.h / 2.0f) - (font_manager_.get_cell_height() / 2.0f),
                  (is_active || is_hovered) ? colors_.text_primary : colors_.text_secondary);
    }
}

void SettingsUI::click_list(DropList& list, float mx, float my) {
    const int i = list.at(mx, my);
    if (i >= 0 && list.items[i] != list.active) {
        SDL_Event ev;
        std::memset(&ev, 0, sizeof(ev));
        ev.type = SDL_EVENT_USER;
        ev.user.code = list.user_code;
        ev.user.data1 = strdup(list.items[i].c_str());
        SDL_PushEvent(&ev);
    }
    list.open = false;
    list.hover = -1;
}

// "Built in" for the purposes of locking: sink's own presets, and ones that
// come only from an included file, which sink can't rename or delete.
bool SettingsUI::is_builtin(const std::string& name) const {
    return std::find(builtin_presets_.begin(), builtin_presets_.end(), name) != builtin_presets_.end() ||
           is_included(name);
}

bool SettingsUI::is_included(const std::string& name) const {
    return std::find(included_presets_.begin(), included_presets_.end(), name) != included_presets_.end();
}

void SettingsUI::set_included_presets(const std::vector<std::string>& names) {
    included_presets_ = names;
}

void SettingsUI::set_active_preset(const std::string& name) {
    active_preset_ = name.empty() ? "pool" : name;
}

void SettingsUI::set_preset_edited(bool edited) {
    preset_edited_ = edited;
}

bool SettingsUI::is_sink_preset(const std::string& name) const {
    return std::find(builtin_presets_.begin(), builtin_presets_.end(), name) != builtin_presets_.end();
}

// "preset: clouds (edited)", the name shortened to fit `max_w` (pixels) if it
// has to be -- never the "(edited)", which is the part that matters.
std::string SettingsUI::preset_label(float max_w) {
    const std::string suffix = preset_edited_ ? " (edited)" : "";
    std::string name = active_preset_;
    if (measure_text_width("preset: " + name + suffix) > max_w) {
        while (!name.empty() && measure_text_width("preset: " + name + "..." + suffix) > max_w) {
            do { name.pop_back(); } while (!name.empty() && (static_cast<unsigned char>(name.back()) & 0xC0) == 0x80);
            if (!name.empty() && (static_cast<unsigned char>(name.back()) & 0xC0) == 0xC0) name.pop_back();
        }
        name += "...";
    }
    return "preset: " + name + suffix;
}

SDL_FRect SettingsUI::dropdown_row_rect(int index) const {
    return { 16.0f, 92.0f + index * 22.0f, 220.0f, 22.0f };
}

// Name-edit row: a text field plus explicit save/cancel buttons, spanning
// the same 16..484 bounds as the rest of the preset row.
SDL_FRect SettingsUI::edit_field_rect() const {
    return { 16.0f, 66.0f, 340.0f, 24.0f };
}

SDL_FRect SettingsUI::edit_save_rect() const {
    return { 364.0f, 66.0f, 48.0f, 24.0f };
}

SDL_FRect SettingsUI::edit_cancel_rect() const {
    return { 420.0f, 66.0f, 64.0f, 24.0f };
}

void SettingsUI::begin_edit(PresetEditMode mode, const std::string& initial_text) {
    preset_dropdown_open_ = false;
    editing_name_ = true;
    edit_mode_ = mode;
    edit_buffer_ = initial_text;
    edit_save_hovered_ = false;
    edit_cancel_hovered_ = false;
    if (window_) {
        SDL_StartTextInput(window_);
    }
}

void SettingsUI::cancel_edit() {
    if (editing_name_ && window_) {
        SDL_StopTextInput(window_);
    }
    editing_name_ = false;
    edit_mode_ = PresetEditMode::kNone;
    edit_buffer_.clear();
    edit_save_hovered_ = false;
    edit_cancel_hovered_ = false;
}

void SettingsUI::commit_edit() {
    if (!editing_name_) return;

    std::string trimmed = edit_buffer_;
    size_t start = trimmed.find_first_not_of(' ');
    size_t end = trimmed.find_last_not_of(' ');
    trimmed = (start == std::string::npos) ? "" : trimmed.substr(start, end - start + 1);

    PresetEditMode mode = edit_mode_;
    cancel_edit();

    if (trimmed.empty()) return;

    SDL_Event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.type = SDL_EVENT_USER;
    ev.user.code = (mode == PresetEditMode::kNew) ? 11 : 13; // New preset / Rename preset
    ev.user.data1 = strdup(trimmed.c_str());
    SDL_PushEvent(&ev);
}

float SettingsUI::measure_text_width(const std::string& text) {
    float w = 0.0f;
    for (char c : text) {
        const GlyphInfo* glyph = font_manager_.get_glyph(renderer_, static_cast<char32_t>(c));
        if (glyph) {
            // Some glyphs (wide proportional letters like 'm' in particular)
            // rasterize slightly wider than their reported advance metric.
            // Stepping by the advance alone would then draw the next glyph
            // over the tail of this one -- most visible as garbled-looking
            // doubled letters (e.g. "mm"). Never step by less than the
            // glyph's own rendered width.
            w += std::max(glyph->advance, glyph->src_rect.w);
        }
    }
    return w;
}

std::string SettingsUI::truncate_head(const std::string& text, float max_width) {
    if (measure_text_width(text) <= max_width) return text;

    const std::string ellipsis = "...";
    if (measure_text_width(ellipsis) > max_width) return ellipsis;

    // Longest tail (by character count) that fits alongside the ellipsis;
    // width shrinks monotonically as `cut` grows, so binary search works.
    size_t lo = 0, hi = text.size();
    std::string best = ellipsis;
    while (lo <= hi) {
        size_t mid = lo + (hi - lo) / 2;
        std::string candidate = ellipsis + text.substr(mid);
        if (measure_text_width(candidate) <= max_width) {
            best = candidate;
            if (mid == 0) break;
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    return best;
}

void SettingsUI::init_layout() {
    buttons_.clear();
    shift_ = has_kind_row() ? 0.0f : -kKindRowH;

    theme_list_.x = 24.0f;
    theme_list_.y = Y(356.0f);
    theme_list_.w = 250.0f;
    theme_list_.upward = false;
    theme_list_.user_code = 17;  // theme picked
    background_list_.items = kBackgroundKinds;
    background_list_.x = 24.0f;
    background_list_.y = 158.0f;
    background_list_.w = 180.0f;
    background_list_.user_code = 18;  // background kind picked
    sliders_.clear();

    // 1. Preset row (not affected by the +44 shift below -- everything else
    // moved down to make room for it under the header). Spans the same
    // 16..484 bounds as the card outlines below it (rather than the 24pt
    // inset used by content *inside* those cards), so its left/right edges
    // line up with the visible card borders.
    // The name's label is built as it's drawn (preset_label), to fit.
    UIButton btn_preset_name = { PRESET_BTN_NAME, "", {16.0f, 66.0f, 220.0f, 24.0f}, colors_.btn_idle, colors_.btn_hover };
    UIButton btn_preset_new = { PRESET_BTN_NEW, "new", {244.0f, 66.0f, 46.0f, 24.0f}, colors_.btn_idle, colors_.btn_hover };
    UIButton btn_preset_dup = { PRESET_BTN_SAVE, "save", {298.0f, 66.0f, 46.0f, 24.0f}, colors_.btn_idle, colors_.btn_hover };
    UIButton btn_preset_rename = { PRESET_BTN_RENAME, "rename", {352.0f, 66.0f, 64.0f, 24.0f}, colors_.btn_idle, colors_.btn_hover };
    UIButton btn_preset_delete = { PRESET_BTN_DELETE, "delete", {424.0f, 66.0f, 60.0f, 24.0f}, colors_.btn_danger, colors_.btn_danger_hover };

    buttons_.push_back(btn_preset_name);
    buttons_.push_back(btn_preset_new);
    buttons_.push_back(btn_preset_dup);
    buttons_.push_back(btn_preset_rename);
    buttons_.push_back(btn_preset_delete);

    // 2. The cards: the background, the theme (colors and the window's look),
    // the text, and what applies to every window.
    // "text": the typeface and how it's drawn. "all windows": settings that
    // aren't part of a preset. Positions are in points, matching the cards
    // render() draws; checkboxes sit in three columns, at 24, 184 and 344.
    auto button = [&](int id, const std::string& label, float x, float y, float w) {
        buttons_.push_back({ id, label, {x, y, w, 28.0f}, colors_.btn_idle, colors_.btn_hover });
    };
    auto toggle = [&](int id, const std::string& label, bool on, float x, float y, float w) {
        UIButton b{ id, label, {x, y, w, 24.0f}, colors_.btn_idle, colors_.btn_hover };
        b.toggle = true;
        b.on = on;
        buttons_.push_back(b);
    };
    button(BG_KIND_BTN, background_kind_label(), 24.0f, 128.0f, 125.0f);
    button(1, select_label(background_kind_label()), 159.0f, 128.0f, 115.0f);
    // The background card's second row: what the kind has to set.
    button(FIT_BTN, "fit: " + background_fit_, 24.0f, 164.0f, 250.0f);
    button(COLOR_SWATCH, "", 159.0f, 128.0f, 28.0f);
    button(COLOR_RESET, "use theme's color", 24.0f, 164.0f, 250.0f);
    for (int i = 0; i < 8; ++i) button(GRADIENT_STOP + i, "", 159.0f + i * 34.0f, 128.0f, 28.0f);
    button(GRADIENT_REMOVE, "-", 431.0f, 128.0f, 20.0f);
    button(GRADIENT_ADD, "+", 456.0f, 128.0f, 20.0f);
    button(GRADIENT_ANGLE, "angle: " + std::to_string(gradient_angle_) + " deg", 24.0f, 164.0f, 250.0f);

    // The theme card: the theme, the text's color, and the window's look.
    button(THEME_BTN, "theme: " + theme_list_.active, 24.0f, Y(326.0f), 250.0f);
    button(TEXT_SWATCH, "", 108.0f, Y(362.0f), 28.0f);
    button(TEXT_RESET, "use theme's color", 324.0f, Y(362.0f), 152.0f);
    toggle(8, "crt effect", crt_effect_enabled_, 24.0f, Y(400.0f), 150.0f);
    toggle(7, "title bar", vibrancy_enabled_, 184.0f, Y(400.0f), 150.0f);

    button(3, "select font...", 24.0f, Y(470.0f), 125.0f);
    toggle(9, "ligatures", ligatures_enabled_, 24.0f, Y(508.0f), 150.0f);
    toggle(5, "typing animation", animated_typing_, 184.0f, Y(508.0f), 150.0f);
    toggle(10, "hdr text", hdr_console_enabled_, 344.0f, Y(508.0f), 120.0f);

    toggle(11, "shell integration", shell_integration_enabled_, 24.0f, Y(578.0f), 150.0f);
    toggle(6, "broadcast input", broadcasting_, 184.0f, Y(578.0f), 150.0f);

    // 3. UI Sliders
    char exposure_buf[32];
    std::snprintf(exposure_buf, sizeof(exposure_buf), "exposure: %.2f", exposure_);
    UISlider s_exposure = { 1, exposure_buf, {24.0f, Y(222.0f), 440.0f, 8.0f}, std::clamp(exposure_ / 2.0f, 0.0f, 1.0f), 0.0f, 2.0f };
    sliders_.push_back(s_exposure);

    char hue_buf[32];
    std::snprintf(hue_buf, sizeof(hue_buf), "hue shift: %.0f deg", hue_shift_);
    UISlider s_hue = { 2, hue_buf, {24.0f, Y(260.0f), 440.0f, 8.0f}, std::clamp(hue_shift_ / 360.0f, 0.0f, 1.0f), 0.0f, 360.0f };
    sliders_.push_back(s_hue);

    // Frosted glass's roughness, where the fit button is for videos.
    UISlider s_rough = { 3, roughness_label(glass_roughness_), {24.0f, 186.0f, 250.0f, 8.0f}, glass_roughness_, 0.0f, 1.0f };
    sliders_.push_back(s_rough);
}

void SettingsUI::update_slider_value(int slider_id, float mouse_x) {
    for (auto& s : sliders_) {
        if (s.id == slider_id) {
            float pct = (mouse_x - s.rect.x) / s.rect.w;
            pct = std::clamp(pct, 0.0f, 1.0f);
            s.value = pct;
            
            if (s.id == 1) { // Exposure
                exposure_ = pct * 2.0f;
                char buf[32];
                std::snprintf(buf, sizeof(buf), "exposure: %.2f", exposure_);
                s.label = buf;
            } else if (s.id == 3) { // Frosted glass's roughness
                glass_roughness_ = pct;
                s.label = roughness_label(pct);
            } else if (s.id == 2) { // Hue shift
                hue_shift_ = pct * 360.0f;
                char buf[32];
                std::snprintf(buf, sizeof(buf), "hue shift: %.0f deg", hue_shift_);
                s.label = buf;
            }
            break;
        }
    }
}

void SettingsUI::process_event(const SDL_Event& event) {
    if (!window_) return;

    Uint32 our_win_id = SDL_GetWindowID(window_);
    if (picker_.open && picker_event(event)) return;

    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == our_win_id) {
        close();
    } else if (event.type == SDL_EVENT_MOUSE_MOTION && event.motion.windowID == our_win_id) {
        float mx = event.motion.x;
        float my = event.motion.y;
        
        if (dragging_) {
            update_slider_value(active_slider_id_, mx);
        } else if (editing_name_) {
            SDL_FRect save = edit_save_rect();
            SDL_FRect cancel = edit_cancel_rect();
            edit_save_hovered_ = (mx >= save.x && mx <= save.x + save.w && my >= save.y && my <= save.y + save.h);
            edit_cancel_hovered_ = (mx >= cancel.x && mx <= cancel.x + cancel.w && my >= cancel.y && my <= cancel.y + cancel.h);
        } else if (theme_list_.open) {
            theme_list_.hover = theme_list_.at(mx, my);
        } else if (background_list_.open) {
            background_list_.hover = background_list_.at(mx, my);
        } else if (preset_dropdown_open_) {
            dropdown_hover_index_ = -1;
            for (size_t i = 0; i < preset_names_.size() && static_cast<int>(i) < kMaxDropdownRows; ++i) {
                SDL_FRect row = dropdown_row_rect(static_cast<int>(i));
                if (mx >= row.x && mx <= row.x + row.w && my >= row.y && my <= row.y + row.h) {
                    dropdown_hover_index_ = static_cast<int>(i);
                    break;
                }
            }
        } else {
            // Hover checking
            for (auto& btn : buttons_) {
                btn.hovered = shown(btn.id) && (mx >= btn.rect.x && mx <= btn.rect.x + btn.rect.w &&
                                                my >= btn.rect.y && my <= btn.rect.y + btn.rect.h);
            }
        }
    } else if (event.type == SDL_EVENT_MOUSE_WHEEL && event.wheel.windowID == our_win_id) {
        for (const auto& btn : buttons_) {
            if (btn.id == GRADIENT_ANGLE && shown(btn.id) && event.wheel.mouse_x >= btn.rect.x &&
                event.wheel.mouse_x <= btn.rect.x + btn.rect.w && event.wheel.mouse_y >= btn.rect.y &&
                event.wheel.mouse_y <= btn.rect.y + btn.rect.h && event.wheel.y != 0.0f) {
                change_angle(event.wheel.y > 0 ? 5 : -5);
            }
        }
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.windowID == our_win_id) {
        if (event.button.button == SDL_BUTTON_LEFT) {
            float mx = event.button.x;
            float my = event.button.y;

            // While the name field is being edited, clicks are scoped to the
            // save/cancel buttons; anything else (including the field itself)
            // just abandons the edit, so a stray click can't both dismiss a
            // rename AND toggle a nearby control in the same gesture.
            if (editing_name_) {
                SDL_FRect save = edit_save_rect();
                bool hit_save = (mx >= save.x && mx <= save.x + save.w && my >= save.y && my <= save.y + save.h);
                // Clicking "cancel" or anywhere else both just abandon the edit.
                hit_save ? commit_edit() : cancel_edit();
                return;
            }

            // Likewise the theme and background lists: a row picks it, a
            // miss closes the list.
            if (theme_list_.open) {
                click_list(theme_list_, mx, my);
                return;
            }
            if (background_list_.open) {
                click_list(background_list_, mx, my);
                return;
            }

            // While the preset dropdown is open, clicks are scoped to its
            // rows: a hit switches to that preset, a miss just dismisses it.
            if (preset_dropdown_open_) {
                for (size_t i = 0; i < preset_names_.size() && static_cast<int>(i) < kMaxDropdownRows; ++i) {
                    SDL_FRect row = dropdown_row_rect(static_cast<int>(i));
                    if (mx >= row.x && mx <= row.x + row.w && my >= row.y && my <= row.y + row.h) {
                        preset_dropdown_open_ = false;
                        dropdown_hover_index_ = -1;
                        // The active preset too, when it's been edited: that
                        // puts it back as saved.
                        if (preset_names_[i] != active_preset_ || preset_edited_) {
                            SDL_Event ev;
                            std::memset(&ev, 0, sizeof(ev));
                            ev.type = SDL_EVENT_USER;
                            ev.user.code = 10; // Switch preset
                            ev.user.data1 = strdup(preset_names_[i].c_str());
                            SDL_PushEvent(&ev);
                        }
                        return;
                    }
                }
                preset_dropdown_open_ = false;
                dropdown_hover_index_ = -1;
                return;
            }

            // Check slider clicks (with target padding height for ease of use)
            for (const auto& s : sliders_) {
                const bool usable = s.id == 3 ? background_kind_label() == "frosted glass" : look_sliders_apply();
                if (usable && mx >= s.rect.x - 10.0f && mx <= s.rect.x + s.rect.w + 10.0f &&
                    my >= s.rect.y - 8.0f && my <= s.rect.y + s.rect.h + 8.0f) {
                    dragging_ = true;
                    active_slider_id_ = s.id;
                    update_slider_value(s.id, mx);
                    return;
                }
            }

            // Check button clicks
            for (auto& btn : buttons_) {
                if (shown(btn.id) && mx >= btn.rect.x && mx <= btn.rect.x + btn.rect.w &&
                    my >= btn.rect.y && my <= btn.rect.y + btn.rect.h) {

                    if (btn.id == COLOR_SWATCH) {
                        open_picker(0, bg_color_, btn.rect);
                    } else if (btn.id >= GRADIENT_STOP && btn.id < GRADIENT_STOP + 8) {
                        open_picker(1 + btn.id - GRADIENT_STOP, gradient_stops_[btn.id - GRADIENT_STOP], btn.rect);
                    } else if (btn.id == TEXT_SWATCH) {
                        open_picker(kTextColorTag, text_color_, btn.rect);
                    } else if (btn.id == TEXT_RESET) {
                        if (text_color_custom_) {
                            SDL_Event ev;
                            std::memset(&ev, 0, sizeof(ev));
                            ev.type = SDL_EVENT_USER;
                            ev.user.code = 26; // Text color back to the theme's
                            SDL_PushEvent(&ev);
                        }
                    } else if (btn.id == COLOR_RESET || btn.id == GRADIENT_REMOVE || btn.id == GRADIENT_ADD) {
                        const bool can = btn.id == COLOR_RESET ? bg_color_custom_
                                       : btn.id == GRADIENT_REMOVE ? gradient_stops_.size() > 2
                                                                   : gradient_stops_.size() < 8;
                        if (can) {
                            SDL_Event ev;
                            std::memset(&ev, 0, sizeof(ev));
                            ev.type = SDL_EVENT_USER;
                            ev.user.code = btn.id == COLOR_RESET ? 23 : 24;
                            ev.user.data1 = reinterpret_cast<void*>(static_cast<intptr_t>(btn.id == GRADIENT_ADD ? 1 : -1));
                            SDL_PushEvent(&ev);
                        }
                    } else if (btn.id == GRADIENT_ANGLE) {
                        // A click turns it by 45 degrees (shift-click back);
                        // the scroll wheel over it, by 5.
                        change_angle((SDL_GetModState() & SDL_KMOD_SHIFT) ? -45 : 45);
                    } else if (btn.id == 1 && background_kind_label() == "shader") {
                        pick_shader_file();
                    } else if (btn.id == 1 && background_kind_label() == "image") {
                        pick_image_file();
                    } else if (btn.id == 1) { // Select a video
                        pick_video_file();
                    } else if (btn.id == 3) { // Select Font
                        SDL_DialogFileFilter filters[] = { {"Font files", "ttf;otf;ttc"} };
                        SDL_ShowOpenFileDialog(font_dialog_callback, nullptr, window_, filters, 1, nullptr, false);
                    } else if (btn.id == 5) { // Toggle Animated Typing
                        animated_typing_ = !animated_typing_;
                        set_animated_typing(animated_typing_);
                        
                        SDL_Event ev;
                        std::memset(&ev, 0, sizeof(ev));
                        ev.type = SDL_EVENT_USER;
                        ev.user.code = 4; // Toggle animation
                        ev.user.data1 = (void*)(intptr_t)animated_typing_;
                        SDL_PushEvent(&ev);
                    } else if (btn.id == 6) { // Toggle Input Broadcasting
                        broadcasting_ = !broadcasting_;
                        set_broadcasting(broadcasting_);
                        
                        SDL_Event ev;
                        std::memset(&ev, 0, sizeof(ev));
                        ev.type = SDL_EVENT_USER;
                        ev.user.code = 5; // Toggle input broadcasting
                        ev.user.data1 = (void*)(intptr_t)broadcasting_;
                        SDL_PushEvent(&ev);
                    } else if (btn.id == 7) { // Toggle Vibrancy
                        set_vibrancy_enabled(!vibrancy_enabled_);
                    } else if (btn.id == 8) { // Toggle CRT Mode
                        set_crt_effect_enabled(!crt_effect_enabled_);
                    } else if (btn.id == 9) { // Toggle Ligatures
                        set_ligatures_enabled(!ligatures_enabled_);
                    } else if (btn.id == 10) { // Toggle HDR Console
                        set_hdr_console_enabled(!hdr_console_enabled_);
                    } else if (btn.id == 11) { // Toggle shell integration
                        set_shell_integration_enabled(!shell_integration_enabled_);
                    } else if (btn.id == THEME_BTN) {
                        theme_list_.open = true;
                        theme_list_.hover = -1;
                    } else if (btn.id == FIT_BTN) {
                        if (fit_applies()) {
                            const std::string next = background_fit_ == "stretch" ? "fill"
                                                   : background_fit_ == "fill"    ? "fit"
                                                                                  : "stretch";
                            set_background_fit(next);
                            SDL_Event ev;
                            std::memset(&ev, 0, sizeof(ev));
                            ev.type = SDL_EVENT_USER;
                            ev.user.code = 21; // Background fit changed
                            ev.user.data1 = strdup(next.c_str());
                            SDL_PushEvent(&ev);
                        }
                    } else if (btn.id == BG_KIND_BTN) {
                        background_list_.open = true;
                        background_list_.hover = -1;
                    } else if (btn.id == PRESET_BTN_NAME) {
                        preset_dropdown_open_ = true;
                        dropdown_hover_index_ = -1;
                    } else if (btn.id == PRESET_BTN_NEW) {
                        begin_edit(PresetEditMode::kNew, "");
                    } else if (btn.id == PRESET_BTN_SAVE) {
                        if (preset_edited_ && !is_sink_preset(active_preset_)) {
                            SDL_Event ev;
                            std::memset(&ev, 0, sizeof(ev));
                            ev.type = SDL_EVENT_USER;
                            ev.user.code = 12; // Save edits into the active preset
                            SDL_PushEvent(&ev);
                        }
                    } else if (btn.id == PRESET_BTN_RENAME) {
                        if (!is_builtin(active_preset_)) {
                            begin_edit(PresetEditMode::kRename, active_preset_);
                        }
                    } else if (btn.id == PRESET_BTN_DELETE) {
                        if (!is_builtin(active_preset_)) {
                            SDL_Event ev;
                            std::memset(&ev, 0, sizeof(ev));
                            ev.type = SDL_EVENT_USER;
                            ev.user.code = 14; // Delete active preset
                            SDL_PushEvent(&ev);
                        }
                    }
                    break;
                }
            }
        }
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.windowID == our_win_id) {
        dragging_ = false;
        active_slider_id_ = 0;
    } else if (event.type == SDL_EVENT_TEXT_INPUT && event.text.windowID == our_win_id) {
        if (editing_name_) {
            // Keep preset names short enough to fit their UI everywhere they're
            // rendered (dropdown rows, the "current preset" button label, etc).
            if (edit_buffer_.size() < 40) {
                edit_buffer_ += event.text.text;
            }
        }
    } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.windowID == our_win_id) {
        if (editing_name_) {
            if (event.key.key == SDLK_BACKSPACE) {
                if (!edit_buffer_.empty()) {
                    // Drop one UTF-8 code point, not just one byte.
                    size_t cut = edit_buffer_.size() - 1;
                    while (cut > 0 && (static_cast<unsigned char>(edit_buffer_[cut]) & 0xC0) == 0x80) {
                        --cut;
                    }
                    edit_buffer_.erase(cut);
                }
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
                commit_edit();
            } else if (event.key.key == SDLK_ESCAPE) {
                cancel_edit();
            }
        } else if (event.key.key == SDLK_ESCAPE && preset_dropdown_open_) {
            preset_dropdown_open_ = false;
            dropdown_hover_index_ = -1;
        } else if (event.key.key == SDLK_ESCAPE && (theme_list_.open || background_list_.open)) {
            theme_list_.open = background_list_.open = false;
            theme_list_.hover = background_list_.hover = -1;
        }
    }
}

void SettingsUI::render() {
    if (!renderer_) return;

    // 1. Clear window
    SDL_SetRenderDrawColor(renderer_, 
        static_cast<Uint8>(colors_.bg.r * 255), 
        static_cast<Uint8>(colors_.bg.g * 255), 
        static_cast<Uint8>(colors_.bg.b * 255), 255);
    SDL_RenderClear(renderer_);

    float scale = SDL_GetWindowDisplayScale(window_);
    if (scale <= 0.0f) scale = 1.0f;

    // Draw underwater sunlight glow gradient at the top of the settings window
    {
        SDL_Vertex vertices[4];
        SDL_FColor c_top_left = {0.0f, 0.8f, 0.8f, 0.20f};  // Bright cyan glow
        SDL_FColor c_top_right = {0.0f, 0.4f, 0.6f, 0.15f}; // Teal-blue glow
        SDL_FColor c_bottom = {0.0f, 0.0f, 0.0f, 0.0f};      // Fades to transparent

        vertices[0] = { {0.0f, 0.0f}, c_top_left, {0.0f, 0.0f} };
        vertices[1] = { {static_cast<float>(500.0f * scale), 0.0f}, c_top_right, {0.0f, 0.0f} };
        vertices[2] = { {0.0f, 160.0f * scale}, c_bottom, {0.0f, 0.0f} };
        vertices[3] = { {static_cast<float>(500.0f * scale), 160.0f * scale}, c_bottom, {0.0f, 0.0f} };

        int indices[6] = { 0, 1, 2, 2, 1, 3 };
        SDL_RenderGeometry(renderer_, nullptr, vertices, 4, indices, 6);
    }

    // 2. Section cards, each with its title.
    auto card = [&](float y, float h, const char* title) {
        SDL_FRect r = { 16.0f * scale, y * scale, 468.0f * scale, h * scale };
        draw_rect_filled(r, colors_.card, 10.0f * scale);
        draw_rect_outline(r, colors_.border, 10.0f * scale);
        draw_text(title, 24.0f * scale, text_y(y + 4.0f, 20.0f, scale), colors_.text_secondary);
    };
    card(100.0f, 186.0f + shift_, "background");
    card(Y(298.0f), 132.0f, "theme");
    card(Y(442.0f), 96.0f, "text");
    card(Y(550.0f), 62.0f, "all windows - not part of the preset");

    // 3. Draw Logos in Header Bar
    if (sink_logo_) {
        float w = 0.0f, h = 0.0f;
        SDL_GetTextureSize(sink_logo_, &w, &h);
        float aspect = (h > 0.0f) ? (w / h) : 1.0f;
        float draw_h = 36.0f * scale;
        float draw_w = draw_h * aspect;
        SDL_FRect sink_dst = { 16.0f * scale, 12.0f * scale, draw_w, draw_h };
        SDL_RenderTexture(renderer_, sink_logo_, nullptr, &sink_dst);
    } else {
        draw_text("sink", 16.0f * scale, 20.0f * scale, colors_.text_primary);
    }

    if (rain_logo_) {
        float w = 0.0f, h = 0.0f;
        SDL_GetTextureSize(rain_logo_, &w, &h);
        float aspect = (h > 0.0f) ? (w / h) : 1.0f;
        float draw_h = 36.0f * scale;
        float draw_w = draw_h * aspect;
        float draw_x = (500.0f * scale) - draw_w - 16.0f * scale;
        float draw_y = 12.0f * scale;
        SDL_FRect rain_dst = { draw_x, draw_y, draw_w, draw_h };
        SDL_RenderTexture(renderer_, rain_logo_, nullptr, &rain_dst);
    } else {
        draw_text("rain", (500.0f * scale) - 60.0f * scale, 20.0f * scale, colors_.text_secondary);
    }
    
    // Value text is truncated to whatever room is actually left before the
    // card's right edge (measured in glyph widths, not a guessed char
    // count), so it uses the full width of the card instead of an
    // arbitrarily short prefix.
    float value_right_edge = 476.0f * scale; // card right edge (484) minus a small margin

    // Beside the buttons: the video file, or what the kind is.
    const std::string kind = background_kind_label();
    std::string bg_disp = kind == "video"        ? (video_file_.empty() ? std::string("no video picked yet") : video_file_)
                        : kind == "pool"         ? std::string("drifting underwater")
                        : kind == "color"        ? std::string("a solid color")
                        : kind == "gradient"     ? std::string("a gradient")
                        : kind == "gel"          ? std::string("a churning nebula")
                        : kind == "clear"        ? std::string("the desktop, clear")
                        : kind == "frosted glass" ? std::string("the desktop, frosted")
                        : kind == "shader"       ? (shader_file_.empty() ? std::string("no shader picked yet") : shader_file_)
                        : kind == "image"        ? (image_file_.empty() ? std::string("no image picked yet") : image_file_)
                        : std::string("the desktop, through glass");
    // Beside the file button when there is one, else in its place.
    float bg_value_x = (shown(1) ? 284.0f : 159.0f) * scale;
    if (kind == "color") {
        // Beside the swatch: its hex, and whose color it is.
        char hex[8];
        auto byte = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
        std::snprintf(hex, sizeof(hex), "#%02x%02x%02x", byte(bg_color_.r), byte(bg_color_.g), byte(bg_color_.b));
        bg_disp = std::string(hex) + (bg_color_custom_ ? "" : "  (the theme's)");
        bg_value_x = 197.0f * scale;
    }
    if (kind != "gradient") {
        bg_disp = truncate_head(bg_disp, value_right_edge - bg_value_x);
        draw_text(bg_disp, bg_value_x, text_y(128.0f, 28.0f, scale), colors_.text_primary);
    }

    // The text's color: label, swatch (a button), and its hex.
    draw_text("text color", 24.0f * scale, text_y(Y(362.0f), 28.0f, scale), colors_.text_secondary);
    {
        char hex[8];
        auto byte = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
        std::snprintf(hex, sizeof(hex), "#%02x%02x%02x", byte(text_color_.r), byte(text_color_.g), byte(text_color_.b));
        draw_text(std::string(hex) + (text_color_custom_ ? "" : "  (the theme's)"), 146.0f * scale,
                  text_y(Y(362.0f), 28.0f, scale), colors_.text_primary);
    }

    std::string font_disp = font_path_;
    std::string font_lower = font_disp;
    std::transform(font_lower.begin(), font_lower.end(), font_lower.begin(), ::tolower);
    if (font_lower.find("monaspaceneon-regular.otf") != std::string::npos || font_lower == "default") {
        font_disp = "default (monaspace neon)";
    }
    float font_value_x = 159.0f * scale;
    font_disp = truncate_head(font_disp, value_right_edge - font_value_x);
    draw_text(font_disp, font_value_x, text_y(Y(470.0f), 28.0f, scale), colors_.text_primary);

    // 4. Draw Buttons (ghost buttons with thin outline borders)
    bool preset_locked = is_builtin(active_preset_);
    for (const auto& btn : buttons_) {
        // The name field turns into an editable text box (drawn separately,
        // below) while editing; the action buttons next to it don't apply
        // to anything mid-edit, so skip the whole row.
        if (editing_name_ && (btn.id == PRESET_BTN_NAME || btn.id == PRESET_BTN_NEW ||
                               btn.id == PRESET_BTN_SAVE || btn.id == PRESET_BTN_RENAME ||
                               btn.id == PRESET_BTN_DELETE)) {
            continue;
        }

        if (!shown(btn.id)) continue;
        bool disabled = (preset_locked && (btn.id == PRESET_BTN_RENAME || btn.id == PRESET_BTN_DELETE)) ||
                        (btn.id == PRESET_BTN_SAVE && (!preset_edited_ || is_sink_preset(active_preset_))) ||
                        (btn.id == FIT_BTN && !fit_applies()) ||
                        (btn.id == COLOR_RESET && !bg_color_custom_) ||
                        (btn.id == TEXT_RESET && !text_color_custom_) ||
                        (btn.id == GRADIENT_REMOVE && gradient_stops_.size() <= 2) ||
                        (btn.id == GRADIENT_ADD && gradient_stops_.size() >= 8);

        // A swatch: the color itself, outlined, brighter when hovered.
        if (btn.id == COLOR_SWATCH || btn.id == TEXT_SWATCH || (btn.id >= GRADIENT_STOP && btn.id < GRADIENT_STOP + 8)) {
            SDL_FRect r = { btn.rect.x * scale, btn.rect.y * scale, btn.rect.w * scale, btn.rect.h * scale };
            SDL_FColor c = btn.id == COLOR_SWATCH ? bg_color_
                         : btn.id == TEXT_SWATCH  ? text_color_
                                                  : gradient_stops_[btn.id - GRADIENT_STOP];
            c.a = 1.0f;
            draw_rect_filled(r, c, 6.0f * scale);
            draw_rect_outline(r, btn.hovered ? SDL_FColor{1.0f, 1.0f, 1.0f, 0.85f} : SDL_FColor{1.0f, 1.0f, 1.0f, 0.30f},
                              6.0f * scale);
            continue;
        }

        SDL_FRect phys_rect = {
            btn.rect.x * scale,
            btn.rect.y * scale,
            btn.rect.w * scale,
            btn.rect.h * scale
        };

        if (btn.toggle) {
            // A checkbox: filled in the accent color when on, its label beside it.
            const float box = 14.0f * scale;
            SDL_FRect b = { phys_rect.x, phys_rect.y + (phys_rect.h - box) / 2.0f, box, box };
            if (btn.on) {
                draw_rect_filled(b, SDL_FColor{0.0f, 0.8f, 0.8f, 0.85f}, 3.0f * scale);
            } else {
                draw_rect_filled(b, colors_.card, 3.0f * scale);
            }
            draw_rect_outline(b, btn.hovered ? SDL_FColor{1.0f, 1.0f, 1.0f, 0.70f}
                                             : SDL_FColor{1.0f, 1.0f, 1.0f, 0.30f}, 3.0f * scale);
            draw_text(btn.label, b.x + box + 8.0f * scale, text_y(btn.rect.y, btn.rect.h, scale),
                      btn.hovered || btn.on ? colors_.text_primary : colors_.text_secondary);
            continue;
        }

        draw_rect_filled(phys_rect, disabled ? colors_.btn_idle : (btn.hovered ? btn.hover_color : btn.color), 6.0f * scale);

        // Draw thin white border outline (0.2 alpha idle, 0.6 alpha hover)
        SDL_FColor border_color = disabled ? SDL_FColor{1.0f, 1.0f, 1.0f, 0.10f} :
            (btn.hovered ? SDL_FColor{1.0f, 1.0f, 1.0f, 0.60f} : SDL_FColor{1.0f, 1.0f, 1.0f, 0.20f});
        draw_rect_outline(phys_rect, border_color, 6.0f * scale);

        const std::string label = btn.id == PRESET_BTN_NAME ? preset_label(phys_rect.w - 16.0f * scale) : btn.label;
        float text_w = measure_text_width(label);

        float tx = (phys_rect.x + phys_rect.w / 2.0f) - (text_w / 2.0f);
        draw_text(label, tx, text_y(btn.rect.y, btn.rect.h, scale), disabled ? colors_.text_secondary : colors_.text_primary);
    }

    // 4b. Preset name edit row (new/rename): text field + explicit save/
    // cancel buttons, in place of the name button and its neighbors.
    // Enter/Escape still work (see process_event), but the buttons make the
    // action mouse-discoverable and keep everything on one line -- nothing
    // to collide with the card below.
    if (editing_name_) {
        SDL_FRect field = edit_field_rect();
        SDL_FRect phys_field = { field.x * scale, field.y * scale, field.w * scale, field.h * scale };
        draw_rect_filled(phys_field, colors_.card, 6.0f * scale);
        draw_rect_outline(phys_field, SDL_FColor{0.0f, 0.8f, 0.8f, 0.6f}, 6.0f * scale);

        std::string shown = edit_buffer_.empty() ? "preset name..." : edit_buffer_;
        SDL_FColor text_color = edit_buffer_.empty() ? colors_.text_secondary : colors_.text_primary;
        float tx = phys_field.x + 10.0f * scale;
        float ty = phys_field.y + (phys_field.h / 2.0f) - (font_manager_.get_cell_height() / 2.0f);
        draw_text(shown, tx, ty, text_color);

        // Blinking caret at the insertion point, so the field reads as
        // focused/typeable even before anything's been typed into it.
        bool cursor_visible = ((SDL_GetTicks() / 500) % 2) == 0;
        if (cursor_visible) {
            float cursor_w = edit_buffer_.empty() ? 0.0f : measure_text_width(edit_buffer_);
            draw_text("|", tx + cursor_w, ty, SDL_FColor{0.0f, 0.8f, 0.8f, 0.9f});
        }

        SDL_FRect save = edit_save_rect();
        SDL_FRect phys_save = { save.x * scale, save.y * scale, save.w * scale, save.h * scale };
        draw_rect_filled(phys_save, edit_save_hovered_ ? colors_.btn_hover : colors_.btn_idle, 6.0f * scale);
        draw_rect_outline(phys_save, SDL_FColor{0.0f, 0.8f, 0.8f, edit_save_hovered_ ? 0.8f : 0.5f}, 6.0f * scale);
        float save_text_w = measure_text_width("save");
        draw_text("save", phys_save.x + phys_save.w / 2.0f - save_text_w / 2.0f, ty, colors_.text_primary);

        SDL_FRect cancel = edit_cancel_rect();
        SDL_FRect phys_cancel = { cancel.x * scale, cancel.y * scale, cancel.w * scale, cancel.h * scale };
        draw_rect_filled(phys_cancel, edit_cancel_hovered_ ? colors_.btn_hover : colors_.btn_idle, 6.0f * scale);
        SDL_FColor cancel_border = edit_cancel_hovered_ ? SDL_FColor{1.0f, 1.0f, 1.0f, 0.60f} : SDL_FColor{1.0f, 1.0f, 1.0f, 0.20f};
        draw_rect_outline(phys_cancel, cancel_border, 6.0f * scale);
        float cancel_text_w = measure_text_width("cancel");
        draw_text("cancel", phys_cancel.x + phys_cancel.w / 2.0f - cancel_text_w / 2.0f, ty, colors_.text_secondary);
    }

    // sink.toml notice, e.g. a syntax error holding back saves.
    // The start says what's wrong, so a long one loses its end, not its start.
    if (!config_notice_.empty()) {
        std::string shown = config_notice_;
        const float max_w = (500.0f - 48.0f) * scale;
        if (measure_text_width(shown) > max_w) {
            while (!shown.empty() && measure_text_width(shown + "...") > max_w) {
                do { shown.pop_back(); } while (!shown.empty() && (static_cast<unsigned char>(shown.back()) & 0xC0) == 0x80);
                if (!shown.empty() && (static_cast<unsigned char>(shown.back()) & 0xC0) == 0xC0) shown.pop_back();
            }
            shown += "...";
        }
        draw_text(shown, 24.0f * scale, text_y(Y(616.0f), 22.0f, scale), SDL_FColor{1.0f, 0.55f, 0.45f, 1.0f});
    }

    // 5. Draw Sliders. Greyed out, with a note why, for backgrounds they
    // don't change; left in place so nothing moves when the kind changes.
    const bool look_on = look_sliders_apply();
    const bool frosted = background_kind_label() == "frosted glass";
    for (const auto& s : sliders_) {
        if (s.id == 3 && !frosted) continue;  // roughness is frosted glass's alone
        const bool sliders_on = s.id == 3 || look_on;
        // Draw Slider Label
        SDL_FColor label_color = colors_.text_secondary;
        if (!sliders_on) label_color.a *= 0.5f;
        draw_text(s.label, s.rect.x * scale, (s.rect.y - 18.0f) * scale, label_color);
        if (!sliders_on && s.id == 1) {
            const std::string note = "for pool, gel, videos, images and shaders";
            draw_text(note, (s.rect.x + s.rect.w) * scale - measure_text_width(note), (s.rect.y - 18.0f) * scale,
                      label_color);
        }

        // Draw Slider Track
        SDL_FRect phys_track = {
            s.rect.x * scale,
            s.rect.y * scale,
            s.rect.w * scale,
            s.rect.h * scale
        };
        draw_rect_filled(phys_track, colors_.card, 4.0f * scale); // Translucent background
        draw_rect_outline(phys_track, colors_.border, 4.0f * scale);

        // Draw Active Track Fill (in matching cyan accent)
        SDL_FRect phys_active = {
            s.rect.x * scale,
            s.rect.y * scale,
            s.rect.w * scale * s.value,
            s.rect.h * scale
        };
        SDL_FColor slider_accent = sliders_on ? SDL_FColor{0.0f, 0.8f, 0.8f, 0.80f} : SDL_FColor{0.5f, 0.5f, 0.5f, 0.25f};
        draw_rect_filled(phys_active, slider_accent, 4.0f * scale); // Accent cyan fill

        // Draw Handle/Knob
        float knob_cx = s.rect.x + s.rect.w * s.value;
        float knob_w = 8.0f;
        float knob_h = 16.0f;
        SDL_FRect phys_knob = {
            (knob_cx - knob_w / 2.0f) * scale,
            (s.rect.y + s.rect.h / 2.0f - knob_h / 2.0f) * scale,
            knob_w * scale,
            knob_h * scale
        };
        SDL_FColor knob_color = colors_.text_primary;
        if (!sliders_on) knob_color.a *= 0.35f;
        draw_rect_filled(phys_knob, knob_color, 4.0f * scale); // White handle knob
        draw_rect_outline(phys_knob, colors_.border, 4.0f * scale);
    }

    // The preset list, after everything else (sliders included) so it sits over
    // every control it opens across.
    if (preset_dropdown_open_) {
        int count = std::min(static_cast<int>(preset_names_.size()), kMaxDropdownRows);
        SDL_FRect list_bg = { 16.0f * scale, 92.0f * scale, 220.0f * scale, (count * 22.0f) * scale };
        draw_rect_filled(list_bg, SDL_FColor{0.06f, 0.08f, 0.10f, 0.98f}, 6.0f * scale);
        draw_rect_outline(list_bg, colors_.border, 6.0f * scale);

        for (int i = 0; i < count; ++i) {
            SDL_FRect row = dropdown_row_rect(i);
            SDL_FRect phys_row = { row.x * scale, row.y * scale, row.w * scale, row.h * scale };
            bool is_active = (preset_names_[i] == active_preset_);
            bool is_hovered = (i == dropdown_hover_index_);
            if (is_hovered || is_active) {
                // Inset a bit from the row's full bounds so the highlight's
                // own rounded corners never have to line up with the list's
                // (a flush, square-cornered fill on the first/last row would
                // poke out past the container's rounded corners).
                SDL_FRect highlight = {
                    phys_row.x + 3.0f * scale,
                    phys_row.y + 2.0f * scale,
                    phys_row.w - 6.0f * scale,
                    phys_row.h - 4.0f * scale
                };
                SDL_FColor highlight_color = is_hovered ? SDL_FColor{1.0f, 1.0f, 1.0f, 0.10f} : SDL_FColor{0.0f, 0.8f, 0.8f, 0.15f};
                draw_rect_filled(highlight, highlight_color, 4.0f * scale);
            }
            const float row_text_y = phys_row.y + (phys_row.h / 2.0f) - (font_manager_.get_cell_height() / 2.0f);
            draw_text(preset_names_[i], phys_row.x + 10.0f * scale, row_text_y,
                       (is_active || is_hovered) ? colors_.text_primary : colors_.text_secondary);
            if (is_builtin(preset_names_[i])) {
                const std::string tag = is_included(preset_names_[i]) ? "included" : "built-in";
                draw_text(tag, phys_row.x + phys_row.w - measure_text_width(tag) - 10.0f * scale, row_text_y,
                          colors_.text_secondary);
            }
        }
    }

    // Last, so it sits over every other control.
    if (theme_list_.open) draw_list(theme_list_, scale);
    if (background_list_.open) draw_list(background_list_, scale);
    if (picker_.open) draw_picker(scale);

    SDL_RenderPresent(renderer_);
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void SettingsUI::draw_rect_filled(const SDL_FRect& rect, const SDL_FColor& color, float radius) {
    if (radius <= 0.0f) {
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, 
            static_cast<Uint8>(color.r * 255), 
            static_cast<Uint8>(color.g * 255), 
            static_cast<Uint8>(color.b * 255), 
            static_cast<Uint8>(color.a * 255));
        SDL_RenderFillRect(renderer_, &rect);
        return;
    }

    float r = std::min(radius, std::min(rect.w / 2.0f, rect.h / 2.0f));

    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;

    SDL_FPoint corners[4] = {
        { rect.x + r, rect.y + r },                  // Top-Left
        { rect.x + rect.w - r, rect.y + r },         // Top-Right
        { rect.x + rect.w - r, rect.y + rect.h - r },// Bottom-Right
        { rect.x + r, rect.y + rect.h - r }          // Bottom-Left
    };

    float angles[4] = {
        (float)M_PI,
        1.5f * (float)M_PI,
        0.0f,
        0.5f * (float)M_PI
    };

    SDL_FPoint center = { rect.x + rect.w / 2.0f, rect.y + rect.h / 2.0f };
    vertices.push_back({ center, color, {0.0f, 0.0f} });

    const int segments = 6;
    for (int c = 0; c < 4; ++c) {
        for (int s = 0; s <= segments; ++s) {
            float theta = angles[c] + (static_cast<float>(s) / segments) * (0.5f * (float)M_PI);
            float vx = corners[c].x + r * std::cos(theta);
            float vy = corners[c].y + r * std::sin(theta);
            vertices.push_back({ {vx, vy}, color, {0.0f, 0.0f} });
        }
    }

    int num_boundary_verts = static_cast<int>(vertices.size()) - 1;
    for (int i = 1; i <= num_boundary_verts; ++i) {
        int next = (i == num_boundary_verts) ? 1 : (i + 1);
        indices.push_back(0);
        indices.push_back(i);
        indices.push_back(next);
    }

    SDL_RenderGeometry(renderer_, nullptr, vertices.data(), static_cast<int>(vertices.size()), indices.data(), static_cast<int>(indices.size()));
}

void SettingsUI::draw_rect_outline(const SDL_FRect& rect, const SDL_FColor& color, float radius) {
    if (radius <= 0.0f) {
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, 
            static_cast<Uint8>(color.r * 255), 
            static_cast<Uint8>(color.g * 255), 
            static_cast<Uint8>(color.b * 255), 
            static_cast<Uint8>(color.a * 255));
        SDL_RenderRect(renderer_, &rect);
        return;
    }

    float r = std::min(radius, std::min(rect.w / 2.0f, rect.h / 2.0f));

    std::vector<SDL_FPoint> points;

    SDL_FPoint corners[4] = {
        { rect.x + r, rect.y + r },
        { rect.x + rect.w - r, rect.y + r },
        { rect.x + rect.w - r, rect.y + rect.h - r },
        { rect.x + r, rect.y + rect.h - r }
    };

    float angles[4] = {
        (float)M_PI,
        1.5f * (float)M_PI,
        0.0f,
        0.5f * (float)M_PI
    };

    const int segments = 8;
    for (int c = 0; c < 4; ++c) {
        for (int s = 0; s <= segments; ++s) {
            float theta = angles[c] + (static_cast<float>(s) / segments) * (0.5f * (float)M_PI);
            float vx = corners[c].x + r * std::cos(theta);
            float vy = corners[c].y + r * std::sin(theta);
            points.push_back({ vx, vy });
        }
    }
    points.push_back(points[0]);

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, 
        static_cast<Uint8>(color.r * 255), 
        static_cast<Uint8>(color.g * 255), 
        static_cast<Uint8>(color.b * 255), 
        static_cast<Uint8>(color.a * 255));
    SDL_RenderLines(renderer_, points.data(), static_cast<int>(points.size()));
}

void SettingsUI::draw_text(const std::string& text, float x, float y, const SDL_FColor& color, bool monospace) {
    SDL_Texture* atlas = font_manager_.get_atlas_texture();
    if (!atlas) return;

    float atlas_w = 0.0f;
    float atlas_h = 0.0f;
    if (!SDL_GetTextureSize(atlas, &atlas_w, &atlas_h)) return;

    SDL_SetTextureColorMod(atlas, static_cast<Uint8>(color.r * 255), static_cast<Uint8>(color.g * 255), static_cast<Uint8>(color.b * 255));
    SDL_SetTextureAlphaMod(atlas, static_cast<Uint8>(color.a * 255));

    float current_x = x;

    for (char c : text) {
        if (c == '\n' || c == '\r') continue;
        const GlyphInfo* glyph = font_manager_.get_glyph(renderer_, static_cast<char32_t>(c));
        if (glyph) {
            SDL_FRect src = glyph->src_rect;
            SDL_FRect dst = {
                current_x,
                y,
                src.w,
                src.h
            };

            SDL_RenderTexture(renderer_, atlas, &src, &dst);
            
            if (monospace) {
                current_x += font_manager_.get_cell_width();
            } else {
                // See measure_text_width(): never step by less than the
                // glyph's own rendered width, or the next glyph gets drawn
                // over this one's tail.
                current_x += std::max(glyph->advance, src.w);
            }
        }
    }

    SDL_SetTextureColorMod(atlas, 255, 255, 255);
    SDL_SetTextureAlphaMod(atlas, 255);
}

// ---- The color picker --------------------------------------------------------

namespace {

void hsv_to_rgb(float h, float s, float v, float& r, float& g, float& b) {
    h = std::fmod(std::fmod(h, 360.0f) + 360.0f, 360.0f) / 60.0f;
    const float c = v * s, x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f)), m = v - c;
    float rr = 0, gg = 0, bb = 0;
    switch (static_cast<int>(h)) {
        case 0: rr = c; gg = x; break;
        case 1: rr = x; gg = c; break;
        case 2: gg = c; bb = x; break;
        case 3: gg = x; bb = c; break;
        case 4: rr = x; bb = c; break;
        default: rr = c; bb = x; break;
    }
    r = rr + m; g = gg + m; b = bb + m;
}

void rgb_to_hsv(float r, float g, float b, float& h, float& s, float& v) {
    const float mx = std::max({ r, g, b }), mn = std::min({ r, g, b }), d = mx - mn;
    v = mx;
    s = mx > 0 ? d / mx : 0;
    if (d <= 0) { h = 0; return; }
    if (mx == r) h = 60.0f * std::fmod((g - b) / d, 6.0f);
    else if (mx == g) h = 60.0f * ((b - r) / d + 2.0f);
    else h = 60.0f * ((r - g) / d + 4.0f);
    if (h < 0) h += 360.0f;
}

std::string hex_text(const SDL_FColor& c) {
    auto byte = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", byte(c.r), byte(c.g), byte(c.b));
    return buf;
}

// "#rgb", "#rrggbb", with or without the #; false for anything else.
bool parse_hex(std::string t, SDL_FColor& out) {
    if (!t.empty() && t[0] == '#') t.erase(0, 1);
    if (t.size() == 3) t = std::string{ t[0], t[0], t[1], t[1], t[2], t[2] };
    if (t.size() != 6 || t.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
    const unsigned long v = std::strtoul(t.c_str(), nullptr, 16);
    out = { ((v >> 16) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f, (v & 0xFF) / 255.0f, 1.0f };
    return true;
}

bool inside(const SDL_FRect& r, float x, float y) {
    return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h;
}

} // namespace

void SettingsUI::open_picker(int tag, const SDL_FColor& color, const SDL_FRect& anchor) {
    close_picker();
    picker_.open = true;
    picker_.tag = tag;
    rgb_to_hsv(color.r, color.g, color.b, picker_.h, picker_.s, picker_.v);
    picker_.x = std::clamp(anchor.x, 16.0f, 484.0f - kPickerW);
    const float below = anchor.y + anchor.h + 4.0f;
    picker_.y = below + kPickerH <= Y(634.0f) ? below : anchor.y - 4.0f - kPickerH;
}

void SettingsUI::close_picker() {
    if (picker_.editing_hex && window_) SDL_StopTextInput(window_);
    picker_ = ColorPicker{};
}

SDL_FColor SettingsUI::picker_color() const {
    SDL_FColor c{ 0, 0, 0, 1 };
    hsv_to_rgb(picker_.h, picker_.s, picker_.v, c.r, c.g, c.b);
    return c;
}

void SettingsUI::picker_send() {
    const SDL_FColor c = picker_color();
    // The swatch follows at once; Settings is told the same again by sink.
    if (picker_.tag == kTextColorTag) text_color_ = c;
    else if (picker_.tag == 0) bg_color_ = c;
    else if (picker_.tag - 1 < static_cast<int>(gradient_stops_.size())) gradient_stops_[picker_.tag - 1] = c;
    auto byte = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    SDL_Event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.type = SDL_EVENT_USER;
    ev.user.code = 22;  // A color picked
    ev.user.data1 = reinterpret_cast<void*>(static_cast<intptr_t>(picker_.tag));
    ev.user.data2 = reinterpret_cast<void*>(static_cast<uintptr_t>((byte(c.r) << 16) | (byte(c.g) << 8) | byte(c.b)));
    SDL_PushEvent(&ev);
}

void SettingsUI::picker_drag(float mx, float my) {
    if (picker_.drag_sv) {
        const SDL_FRect r = picker_sv_rect();
        picker_.s = std::clamp((mx - r.x) / r.w, 0.0f, 1.0f);
        picker_.v = 1.0f - std::clamp((my - r.y) / r.h, 0.0f, 1.0f);
    } else if (picker_.drag_hue) {
        const SDL_FRect r = picker_hue_rect();
        picker_.h = std::clamp((my - r.y) / r.h, 0.0f, 1.0f) * 360.0f;
    } else {
        return;
    }
    picker_send();
}

bool SettingsUI::picker_event(const SDL_Event& event) {
    const Uint32 win = SDL_GetWindowID(window_);
    auto stop_hex = [&]() {
        if (picker_.editing_hex) SDL_StopTextInput(window_);
        picker_.editing_hex = false;
    };
    auto apply_hex = [&]() {
        SDL_FColor c;
        if (parse_hex(picker_.hex, c)) {
            rgb_to_hsv(c.r, c.g, c.b, picker_.h, picker_.s, picker_.v);
            picker_send();
        }
        stop_hex();
    };
    switch (event.type) {
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            if (event.button.windowID != win || event.button.button != SDL_BUTTON_LEFT) return false;
            const float mx = event.button.x, my = event.button.y;
            if (!inside(picker_rect(), mx, my)) {
                // A click away closes it, and does nothing else.
                if (picker_.editing_hex) apply_hex();
                close_picker();
                return true;
            }
            if (inside(picker_hex_rect(), mx, my)) {
                if (!picker_.editing_hex) {
                    picker_.editing_hex = true;
                    picker_.hex = hex_text(picker_color());
                    SDL_StartTextInput(window_);
                }
                return true;
            }
            if (picker_.editing_hex) apply_hex();
            if (inside(picker_done_rect(), mx, my)) {
                close_picker();
                return true;
            }
            const SDL_FRect sv = picker_sv_rect(), hue = picker_hue_rect();
            if (inside(SDL_FRect{ sv.x - 4, sv.y - 4, sv.w + 8, sv.h + 8 }, mx, my)) picker_.drag_sv = true;
            else if (inside(SDL_FRect{ hue.x - 4, hue.y - 4, hue.w + 8, hue.h + 8 }, mx, my)) picker_.drag_hue = true;
            picker_drag(mx, my);
            return true;
        }
        case SDL_EVENT_MOUSE_MOTION:
            if (event.motion.windowID != win) return false;
            if (picker_.drag_sv || picker_.drag_hue) {
                picker_drag(event.motion.x, event.motion.y);
                return true;
            }
            return inside(picker_rect(), event.motion.x, event.motion.y);
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.windowID != win) return false;
            picker_.drag_sv = picker_.drag_hue = false;
            return true;
        case SDL_EVENT_MOUSE_WHEEL:
            return event.wheel.windowID == win;
        case SDL_EVENT_TEXT_INPUT:
            if (event.text.windowID != win || !picker_.editing_hex) return false;
            for (const char* p = event.text.text; *p; ++p) {
                if (std::isxdigit(static_cast<unsigned char>(*p)) || *p == '#') {
                    if (picker_.hex.size() < 7) picker_.hex += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
                }
            }
            return true;
        case SDL_EVENT_KEY_DOWN: {
            if (event.key.windowID != win) return false;
            const SDL_Keycode k = event.key.key;
            if (picker_.editing_hex) {
                if (k == SDLK_BACKSPACE && !picker_.hex.empty()) picker_.hex.pop_back();
                else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) apply_hex();
                else if (k == SDLK_ESCAPE) stop_hex();
                else if (k == SDLK_V && (event.key.mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL))) {
                    if (char* clip = SDL_GetClipboardText()) {
                        std::string t = clip;
                        SDL_free(clip);
                        t.erase(std::remove_if(t.begin(), t.end(), [](unsigned char c) { return std::isspace(c); }), t.end());
                        picker_.hex = t.substr(0, 7);
                    }
                }
                return true;
            }
            if (k == SDLK_ESCAPE || k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                close_picker();
                return true;
            }
            return false;
        }
        default:
            return false;
    }
}

void SettingsUI::draw_picker(float scale) {
    auto phys = [&](const SDL_FRect& r) { return SDL_FRect{ r.x * scale, r.y * scale, r.w * scale, r.h * scale }; };
    const SDL_FRect box = phys(picker_rect());
    draw_rect_filled(box, SDL_FColor{ 0.06f, 0.08f, 0.10f, 1.0f }, 8.0f * scale);
    draw_rect_outline(box, colors_.border, 8.0f * scale);

    // The square: white to the hue across, everything to black down. Two
    // quads of vertex colors, which the GPU blends between.
    const SDL_FRect sv = phys(picker_sv_rect());
    SDL_FColor pure{ 0, 0, 0, 1 };
    hsv_to_rgb(picker_.h, 1.0f, 1.0f, pure.r, pure.g, pure.b);
    const SDL_FColor white{ 1, 1, 1, 1 }, clear{ 0, 0, 0, 0 }, black{ 0, 0, 0, 1 };
    auto quad = [&](const SDL_FRect& r, SDL_FColor tl, SDL_FColor tr, SDL_FColor br, SDL_FColor bl) {
        const SDL_Vertex v[4] = {
            { { r.x, r.y }, tl, { 0, 0 } }, { { r.x + r.w, r.y }, tr, { 0, 0 } },
            { { r.x + r.w, r.y + r.h }, br, { 0, 0 } }, { { r.x, r.y + r.h }, bl, { 0, 0 } },
        };
        const int idx[6] = { 0, 1, 2, 0, 2, 3 };
        SDL_RenderGeometry(renderer_, nullptr, v, 4, idx, 6);
    };
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    quad(sv, white, pure, pure, white);
    quad(sv, clear, clear, black, black);
    draw_rect_outline(sv, colors_.border, 0.0f);

    // The hue strip, top to bottom: red, yellow, green, cyan, blue, magenta, red.
    const SDL_FRect hue = phys(picker_hue_rect());
    for (int i = 0; i < 6; ++i) {
        SDL_FColor a{ 0, 0, 0, 1 }, b{ 0, 0, 0, 1 };
        hsv_to_rgb(i * 60.0f, 1, 1, a.r, a.g, a.b);
        hsv_to_rgb((i + 1) * 60.0f, 1, 1, b.r, b.g, b.b);
        const SDL_FRect seg = { hue.x, hue.y + hue.h * i / 6.0f, hue.w, hue.h / 6.0f };
        quad(seg, a, a, b, b);
    }
    draw_rect_outline(hue, colors_.border, 0.0f);

    // Where the color is, in each: a ring in the square, a bar across the strip.
    const float ring = 10.0f * scale;
    const SDL_FRect at = { sv.x + picker_.s * sv.w - ring / 2, sv.y + (1.0f - picker_.v) * sv.h - ring / 2, ring, ring };
    draw_rect_outline(at, picker_.v > 0.5f ? SDL_FColor{ 0, 0, 0, 0.9f } : SDL_FColor{ 1, 1, 1, 0.9f }, ring / 2);
    const SDL_FRect bar = { hue.x - 2 * scale, hue.y + picker_.h / 360.0f * hue.h - 3 * scale, hue.w + 4 * scale, 6 * scale };
    draw_rect_outline(bar, SDL_FColor{ 1, 1, 1, 0.95f }, 3.0f * scale);

    // The hex field (typing in it when it's outlined in the accent), the
    // color as it stands, and done.
    const SDL_FRect field = phys(picker_hex_rect());
    draw_rect_filled(field, colors_.card, 6.0f * scale);
    draw_rect_outline(field, picker_.editing_hex ? SDL_FColor{ 0.0f, 0.8f, 0.8f, 0.8f } : colors_.border, 6.0f * scale);
    std::string shown = picker_.editing_hex ? picker_.hex : hex_text(picker_color());
    if (picker_.editing_hex && (SDL_GetTicks() / 500) % 2 == 0) shown += "|";
    draw_text(shown, field.x + 10.0f * scale, text_y(picker_hex_rect().y, picker_hex_rect().h, scale), colors_.text_primary);
    const SDL_FRect preview = phys(SDL_FRect{ picker_.x + 134, picker_.y + 170, 24, 28 });
    draw_rect_filled(preview, picker_color(), 6.0f * scale);
    draw_rect_outline(preview, colors_.border, 6.0f * scale);
    const SDL_FRect done = phys(picker_done_rect());
    draw_rect_filled(done, colors_.btn_idle, 6.0f * scale);
    draw_rect_outline(done, SDL_FColor{ 1, 1, 1, 0.2f }, 6.0f * scale);
    const float tw = measure_text_width("done");
    draw_text("done", done.x + done.w / 2 - tw / 2, text_y(picker_done_rect().y, picker_done_rect().h, scale), colors_.text_primary);
}

