#include "settings_ui.hpp"
#include <iostream>
#include <cstring>
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
    
    // Create settings window (spacious dimensions 500x475, High-DPI enabled)
    window_ = SDL_CreateWindow(
        "sink settings",
        500, 475,
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
}

void SettingsUI::set_animated_typing(bool enabled) {
    animated_typing_ = enabled;
    for (auto& btn : buttons_) {
        if (btn.id == 5) {
            btn.label = std::string("typing: ") + (animated_typing_ ? "on" : "off");
            break;
        }
    }
}

void SettingsUI::set_broadcasting(bool enabled) {
    broadcasting_ = enabled;
    for (auto& btn : buttons_) {
        if (btn.id == 6) {
            btn.label = std::string("broadcast: ") + (broadcasting_ ? "on" : "off");
            break;
        }
    }
}

void SettingsUI::set_exposure(float exposure) {
    exposure_ = exposure;
    for (auto& s : sliders_) {
        if (s.id == 1) {
            s.value = std::clamp(exposure_ / 2.0f, 0.0f, 1.0f);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "Exposure: %.2f", exposure_);
            s.label = buf;
            break;
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
    for (auto& btn : buttons_) {
        if (btn.id == 7) {
            btn.label = std::string("title bar: ") + (vibrancy_enabled_ ? "on" : "off");
            break;
        }
    }
}

void SettingsUI::set_crt_effect_enabled(bool enabled) {
    crt_effect_enabled_ = enabled;
    for (auto& btn : buttons_) {
        if (btn.id == 8) {
            btn.label = std::string("crt shader: ") + (crt_effect_enabled_ ? "on" : "off");
            break;
        }
    }
}

void SettingsUI::set_ligatures_enabled(bool enabled) {
    ligatures_enabled_ = enabled;
    for (auto& btn : buttons_) {
        if (btn.id == 9) {
            btn.label = std::string("ligatures: ") + (ligatures_enabled_ ? "on" : "off");
            break;
        }
    }
}

void SettingsUI::set_hdr_console_enabled(bool enabled) {
    hdr_console_enabled_ = enabled;
    for (auto& btn : buttons_) {
        if (btn.id == 10) {
            btn.label = std::string("hdr console: ") + (hdr_console_enabled_ ? "on" : "off");
            break;
        }
    }
}

void SettingsUI::set_preset_names(const std::vector<std::string>& names) {
    preset_names_ = names.empty() ? std::vector<std::string>{"pool"} : names;
}

void SettingsUI::set_active_preset(const std::string& name) {
    active_preset_ = name.empty() ? "pool" : name;
    for (auto& btn : buttons_) {
        if (btn.id == PRESET_BTN_NAME) {
            btn.label = std::string("preset: ") + active_preset_;
            break;
        }
    }
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
    sliders_.clear();

    // 1. Preset row (not affected by the +44 shift below -- everything else
    // moved down to make room for it under the header). Spans the same
    // 16..484 bounds as the card outlines below it (rather than the 24pt
    // inset used by content *inside* those cards), so its left/right edges
    // line up with the visible card borders.
    UIButton btn_preset_name = { PRESET_BTN_NAME, std::string("preset: ") + active_preset_, {16.0f, 66.0f, 178.0f, 24.0f}, colors_.btn_idle, colors_.btn_hover };
    UIButton btn_preset_new = { PRESET_BTN_NEW, "new", {202.0f, 66.0f, 46.0f, 24.0f}, colors_.btn_idle, colors_.btn_hover };
    UIButton btn_preset_dup = { PRESET_BTN_DUPLICATE, "duplicate", {256.0f, 66.0f, 88.0f, 24.0f}, colors_.btn_idle, colors_.btn_hover };
    UIButton btn_preset_rename = { PRESET_BTN_RENAME, "rename", {352.0f, 66.0f, 64.0f, 24.0f}, colors_.btn_idle, colors_.btn_hover };
    UIButton btn_preset_delete = { PRESET_BTN_DELETE, "delete", {424.0f, 66.0f, 60.0f, 24.0f}, colors_.btn_danger, colors_.btn_danger_hover };

    buttons_.push_back(btn_preset_name);
    buttons_.push_back(btn_preset_new);
    buttons_.push_back(btn_preset_dup);
    buttons_.push_back(btn_preset_rename);
    buttons_.push_back(btn_preset_delete);

    // 2. UI Buttons (shifted down 44pt from their original position to make
    // room for the preset row above, then another 43pt below the sliders
    // to make room for the hue shift slider added alongside exposure)
    UIButton btn_bg_select = { 1, "select file...", {24.0f, 136.0f, 125.0f, 28.0f}, colors_.btn_idle, colors_.btn_hover };
    UIButton btn_bg_clear = { 2, "clear", {159.0f, 136.0f, 75.0f, 28.0f}, colors_.btn_danger, colors_.btn_danger_hover };
    UIButton btn_font_select = { 3, "select font...", {24.0f, 313.0f, 125.0f, 28.0f}, colors_.btn_idle, colors_.btn_hover };

    // Two rows of toggles, packed to their actual label widths instead of
    // one-per-row -- there's plenty of horizontal room in a 468pt-wide card
    // for three (then two) of these side by side.
    UIButton btn_crt_toggle = { 8, std::string("crt shader: ") + (crt_effect_enabled_ ? "on" : "off"), {24.0f, 355.0f, 135.0f, 28.0f}, colors_.btn_idle, colors_.btn_hover };
    UIButton btn_vibrancy_toggle = { 7, std::string("title bar: ") + (vibrancy_enabled_ ? "on" : "off"), {169.0f, 355.0f, 140.0f, 28.0f}, colors_.btn_idle, colors_.btn_hover };
    UIButton btn_ligatures_toggle = { 9, std::string("ligatures: ") + (ligatures_enabled_ ? "on" : "off"), {319.0f, 355.0f, 145.0f, 28.0f}, colors_.btn_idle, colors_.btn_hover };

    UIButton btn_hdr_console_toggle = { 10, std::string("hdr console: ") + (hdr_console_enabled_ ? "on" : "off"), {24.0f, 397.0f, 165.0f, 28.0f}, colors_.btn_idle, colors_.btn_hover };
    UIButton btn_broadcast_toggle = { 6, std::string("broadcast: ") + (broadcasting_ ? "on" : "off"), {199.0f, 397.0f, 135.0f, 28.0f}, colors_.btn_idle, colors_.btn_hover };

    buttons_.push_back(btn_bg_select);
    buttons_.push_back(btn_bg_clear);
    buttons_.push_back(btn_font_select);
    buttons_.push_back(btn_crt_toggle);
    buttons_.push_back(btn_vibrancy_toggle);
    buttons_.push_back(btn_ligatures_toggle);
    buttons_.push_back(btn_hdr_console_toggle);
    buttons_.push_back(btn_broadcast_toggle);

    // 3. UI Sliders
    char exposure_buf[32];
    std::snprintf(exposure_buf, sizeof(exposure_buf), "exposure: %.2f", exposure_);
    UISlider s_exposure = { 1, exposure_buf, {24.0f, 194.0f, 440.0f, 8.0f}, std::clamp(exposure_ / 2.0f, 0.0f, 1.0f), 0.0f, 2.0f };
    sliders_.push_back(s_exposure);

    char hue_buf[32];
    std::snprintf(hue_buf, sizeof(hue_buf), "hue shift: %.0f deg", hue_shift_);
    UISlider s_hue = { 2, hue_buf, {24.0f, 238.0f, 440.0f, 8.0f}, std::clamp(hue_shift_ / 360.0f, 0.0f, 1.0f), 0.0f, 360.0f };
    sliders_.push_back(s_hue);
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
                btn.hovered = (mx >= btn.rect.x && mx <= btn.rect.x + btn.rect.w &&
                               my >= btn.rect.y && my <= btn.rect.y + btn.rect.h);
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

            // While the preset dropdown is open, clicks are scoped to its
            // rows: a hit switches to that preset, a miss just dismisses it.
            if (preset_dropdown_open_) {
                for (size_t i = 0; i < preset_names_.size() && static_cast<int>(i) < kMaxDropdownRows; ++i) {
                    SDL_FRect row = dropdown_row_rect(static_cast<int>(i));
                    if (mx >= row.x && mx <= row.x + row.w && my >= row.y && my <= row.y + row.h) {
                        preset_dropdown_open_ = false;
                        dropdown_hover_index_ = -1;
                        if (preset_names_[i] != active_preset_) {
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
                if (mx >= s.rect.x - 10.0f && mx <= s.rect.x + s.rect.w + 10.0f &&
                    my >= s.rect.y - 8.0f && my <= s.rect.y + s.rect.h + 8.0f) {
                    dragging_ = true;
                    active_slider_id_ = s.id;
                    update_slider_value(s.id, mx);
                    return;
                }
            }

            // Check button clicks
            for (auto& btn : buttons_) {
                if (mx >= btn.rect.x && mx <= btn.rect.x + btn.rect.w &&
                    my >= btn.rect.y && my <= btn.rect.y + btn.rect.h) {
                    
                    if (btn.id == 1) { // Select Background
                        SDL_DialogFileFilter filters[] = { {"Media files", "mp4;mov;mkv;png;jpg;jpeg;bmp"} };
                        SDL_ShowOpenFileDialog(background_dialog_callback, nullptr, window_, filters, 1, nullptr, false);
                    } else if (btn.id == 2) { // Clear Background
                        SDL_Event ev;
                        std::memset(&ev, 0, sizeof(ev));
                        ev.type = SDL_EVENT_USER;
                        ev.user.code = 3; // Clear background
                        SDL_PushEvent(&ev);
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
                    } else if (btn.id == PRESET_BTN_NAME) {
                        preset_dropdown_open_ = true;
                        dropdown_hover_index_ = -1;
                    } else if (btn.id == PRESET_BTN_NEW) {
                        begin_edit(PresetEditMode::kNew, "");
                    } else if (btn.id == PRESET_BTN_DUPLICATE) {
                        SDL_Event ev;
                        std::memset(&ev, 0, sizeof(ev));
                        ev.type = SDL_EVENT_USER;
                        ev.user.code = 12; // Duplicate active preset
                        SDL_PushEvent(&ev);
                    } else if (btn.id == PRESET_BTN_RENAME) {
                        if (active_preset_ != "pool") {
                            begin_edit(PresetEditMode::kRename, active_preset_);
                        }
                    } else if (btn.id == PRESET_BTN_DELETE) {
                        if (active_preset_ != "pool") {
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

    // 2. Draw section cards
    SDL_FRect bg_card = { 16.0f * scale, 104.0f * scale, 468.0f * scale, 158.0f * scale };
    draw_rect_filled(bg_card, colors_.card, 10.0f * scale);
    draw_rect_outline(bg_card, colors_.border, 10.0f * scale);

    SDL_FRect font_card = { 16.0f * scale, 279.0f * scale, 468.0f * scale, 165.0f * scale };
    draw_rect_filled(font_card, colors_.card, 10.0f * scale);
    draw_rect_outline(font_card, colors_.border, 10.0f * scale);

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

    draw_text("background media", 24.0f * scale, 112.0f * scale, colors_.text_secondary);
    std::string bg_disp = bg_path_;
    std::string bg_lower = bg_disp;
    std::transform(bg_lower.begin(), bg_lower.end(), bg_lower.begin(), ::tolower);
    if (bg_lower.find("sinkpool.mp4") != std::string::npos || bg_disp.empty() || bg_lower == "default") {
        bg_disp = "default";
    }
    float bg_value_x = 244.0f * scale;
    bg_disp = truncate_head(bg_disp, value_right_edge - bg_value_x);
    draw_text(bg_disp, bg_value_x, 143.0f * scale, colors_.text_primary);

    draw_text("terminal typeface font", 24.0f * scale, 288.0f * scale, colors_.text_secondary);
    std::string font_disp = font_path_;
    std::string font_lower = font_disp;
    std::transform(font_lower.begin(), font_lower.end(), font_lower.begin(), ::tolower);
    if (font_lower.find("monaspaceneon-regular.otf") != std::string::npos || font_lower == "default") {
        font_disp = "default (monaspace neon)";
    }
    float font_value_x = 159.0f * scale;
    font_disp = truncate_head(font_disp, value_right_edge - font_value_x);
    draw_text(font_disp, font_value_x, 320.0f * scale, colors_.text_primary);

    // 4. Draw Buttons (ghost buttons with thin outline borders)
    bool preset_locked = (active_preset_ == "pool");
    for (const auto& btn : buttons_) {
        // The name field turns into an editable text box (drawn separately,
        // below) while editing; the action buttons next to it don't apply
        // to anything mid-edit, so skip the whole row.
        if (editing_name_ && (btn.id == PRESET_BTN_NAME || btn.id == PRESET_BTN_NEW ||
                               btn.id == PRESET_BTN_DUPLICATE || btn.id == PRESET_BTN_RENAME ||
                               btn.id == PRESET_BTN_DELETE)) {
            continue;
        }

        bool disabled = preset_locked && (btn.id == PRESET_BTN_RENAME || btn.id == PRESET_BTN_DELETE);

        SDL_FRect phys_rect = {
            btn.rect.x * scale,
            btn.rect.y * scale,
            btn.rect.w * scale,
            btn.rect.h * scale
        };

        draw_rect_filled(phys_rect, disabled ? colors_.btn_idle : (btn.hovered ? btn.hover_color : btn.color), 6.0f * scale);

        // Draw thin white border outline (0.2 alpha idle, 0.6 alpha hover)
        SDL_FColor border_color = disabled ? SDL_FColor{1.0f, 1.0f, 1.0f, 0.10f} :
            (btn.hovered ? SDL_FColor{1.0f, 1.0f, 1.0f, 0.60f} : SDL_FColor{1.0f, 1.0f, 1.0f, 0.20f});
        draw_rect_outline(phys_rect, border_color, 6.0f * scale);

        float text_w = measure_text_width(btn.label);

        float tx = (phys_rect.x + phys_rect.w / 2.0f) - (text_w / 2.0f);
        float ty = (phys_rect.y + phys_rect.h / 2.0f) - (font_manager_.get_cell_height() / 2.0f);

        draw_text(btn.label, tx, ty, disabled ? colors_.text_secondary : colors_.text_primary);
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

    // 4c. Preset dropdown overlay, drawn last so it sits above every other card/button
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
            draw_text(preset_names_[i], phys_row.x + 10.0f * scale,
                       phys_row.y + (phys_row.h / 2.0f) - (font_manager_.get_cell_height() / 2.0f),
                       (is_active || is_hovered) ? colors_.text_primary : colors_.text_secondary);
        }
    }

    // 5. Draw Sliders
    for (const auto& s : sliders_) {
        // Draw Slider Label
        draw_text(s.label, s.rect.x * scale, (s.rect.y - 18.0f) * scale, colors_.text_secondary);

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
        SDL_FColor slider_accent = {0.0f, 0.8f, 0.8f, 0.80f};
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
        draw_rect_filled(phys_knob, colors_.text_primary, 4.0f * scale); // White handle knob
        draw_rect_outline(phys_knob, colors_.border, 4.0f * scale);
    }

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
