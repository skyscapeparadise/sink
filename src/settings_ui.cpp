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
    
    // Create settings window (compact dimensions 480x360, High-DPI enabled)
    window_ = SDL_CreateWindow(
        "sink settings",
        480, 360,
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

    // Load local font context for text rendering
    float scale = SDL_GetWindowDisplayScale(window_);
    if (scale <= 0.0f) scale = 1.0f;
    std::string font_path = "/Users/kady/Library/Fonts/MonaSans-VariableFont_wdth,wght.ttf";
    if (!font_manager_.load_font(renderer_, font_path, 13.0f * scale)) {
        font_path = "/System/Library/Fonts/SFNSMono.ttf";
        font_manager_.load_font(renderer_, font_path, 13.0f * scale);
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

    rain_logo_ = IMG_LoadTexture(renderer_, rain_logo_path.c_str());
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

void SettingsUI::init_layout() {
    buttons_.clear();
    sliders_.clear();

    // 1. UI Buttons
    UIButton btn_bg_select = { 1, "select file...", {24.0f, 94.0f, 130.0f, 28.0f}, colors_.btn_idle, colors_.btn_hover };
    UIButton btn_bg_clear = { 2, "clear", {164.0f, 94.0f, 80.0f, 28.0f}, colors_.btn_danger, colors_.btn_danger_hover };
    UIButton btn_font_select = { 3, "select font...", {24.0f, 232.0f, 130.0f, 28.0f}, colors_.btn_idle, colors_.btn_hover };
    
    UIButton btn_anim_toggle = { 5, std::string("typing: ") + (animated_typing_ ? "on" : "off"), {24.0f, 305.0f, 140.0f, 32.0f}, colors_.btn_idle, colors_.btn_hover };
    UIButton btn_broadcast_toggle = { 6, std::string("broadcast: ") + (broadcasting_ ? "on" : "off"), {180.0f, 305.0f, 140.0f, 32.0f}, colors_.btn_idle, colors_.btn_hover };
    UIButton btn_done = { 4, "done", {366.0f, 305.0f, 90.0f, 32.0f}, colors_.btn_idle, colors_.btn_hover };

    buttons_.push_back(btn_bg_select);
    buttons_.push_back(btn_bg_clear);
    buttons_.push_back(btn_font_select);
    buttons_.push_back(btn_anim_toggle);
    buttons_.push_back(btn_broadcast_toggle);
    buttons_.push_back(btn_done);

    // 2. UI Sliders
    char exposure_buf[32];
    std::snprintf(exposure_buf, sizeof(exposure_buf), "exposure: %.2f", exposure_);
    UISlider s_exposure = { 1, exposure_buf, {24.0f, 154.0f, 432.0f, 8.0f}, std::clamp(exposure_ / 2.0f, 0.0f, 1.0f), 0.0f, 2.0f };
    sliders_.push_back(s_exposure);
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
                    } else if (btn.id == 4) { // Done
                        close();
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
                    }
                    break;
                }
            }
        }
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.windowID == our_win_id) {
        dragging_ = false;
        active_slider_id_ = 0;
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
        vertices[1] = { {static_cast<float>(480.0f * scale), 0.0f}, c_top_right, {0.0f, 0.0f} };
        vertices[2] = { {0.0f, 160.0f * scale}, c_bottom, {0.0f, 0.0f} };
        vertices[3] = { {static_cast<float>(480.0f * scale), 160.0f * scale}, c_bottom, {0.0f, 0.0f} };

        int indices[6] = { 0, 1, 2, 2, 1, 3 };
        SDL_RenderGeometry(renderer_, nullptr, vertices, 4, indices, 6);
    }

    // 2. Draw section cards
    SDL_FRect bg_card = { 16.0f * scale, 60.0f * scale, 448.0f * scale, 120.0f * scale };
    draw_rect_filled(bg_card, colors_.card, 10.0f * scale);
    draw_rect_outline(bg_card, colors_.border, 10.0f * scale);

    SDL_FRect font_card = { 16.0f * scale, 195.0f * scale, 448.0f * scale, 80.0f * scale };
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
        float draw_x = (480.0f * scale) - draw_w - 16.0f * scale;
        float draw_y = 12.0f * scale;
        SDL_FRect rain_dst = { draw_x, draw_y, draw_w, draw_h };
        SDL_RenderTexture(renderer_, rain_logo_, nullptr, &rain_dst);
    } else {
        draw_text("rain", (480.0f * scale) - 60.0f * scale, 20.0f * scale, colors_.text_secondary);
    }
    
    draw_text("background media", 24.0f * scale, 70.0f * scale, colors_.text_secondary);
    std::string bg_disp = bg_path_;
    std::transform(bg_disp.begin(), bg_disp.end(), bg_disp.begin(), ::tolower);
    if (bg_disp.find("sinkpool.mp4") != std::string::npos || bg_disp.empty() || bg_disp == "default") {
        bg_disp = "default";
    } else {
        if (bg_disp.length() > 18) {
            bg_disp = "..." + bg_disp.substr(bg_disp.length() - 15);
        }
    }
    draw_text(bg_disp, 260.0f * scale, 70.0f * scale, colors_.text_primary);

    draw_text("terminal typeface font", 24.0f * scale, 205.0f * scale, colors_.text_secondary);
    std::string font_disp = font_path_;
    std::transform(font_disp.begin(), font_disp.end(), font_disp.begin(), ::tolower);
    if (font_disp.find("monaspaceneon-regular.otf") != std::string::npos || font_disp == "default") {
        font_disp = "default (monaspace neon)";
    } else {
        if (font_disp.length() > 28) {
            font_disp = "..." + font_disp.substr(font_disp.length() - 25);
        }
    }
    draw_text(font_disp, 164.0f * scale, 239.0f * scale, colors_.text_primary);

    // 4. Draw Buttons (ghost buttons with thin outline borders)
    for (const auto& btn : buttons_) {
        SDL_FRect phys_rect = {
            btn.rect.x * scale,
            btn.rect.y * scale,
            btn.rect.w * scale,
            btn.rect.h * scale
        };

        draw_rect_filled(phys_rect, btn.hovered ? btn.hover_color : btn.color, 6.0f * scale);
        
        // Draw thin white border outline (0.2 alpha idle, 0.6 alpha hover)
        SDL_FColor border_color = btn.hovered ? SDL_FColor{1.0f, 1.0f, 1.0f, 0.60f} : SDL_FColor{1.0f, 1.0f, 1.0f, 0.20f};
        draw_rect_outline(phys_rect, border_color, 6.0f * scale);

        float text_w = 0.0f;
        for (char c : btn.label) {
            const GlyphInfo* glyph = font_manager_.get_glyph(static_cast<char32_t>(c));
            if (glyph) text_w += glyph->advance;
        }
        
        float tx = (phys_rect.x + phys_rect.w / 2.0f) - (text_w / 2.0f);
        float ty = (phys_rect.y + phys_rect.h / 2.0f) - (font_manager_.get_cell_height() / 2.0f);
        
        draw_text(btn.label, tx, ty, colors_.text_primary);
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
        const GlyphInfo* glyph = font_manager_.get_glyph(static_cast<char32_t>(c));
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
                current_x += glyph->advance;
            }
        }
    }

    SDL_SetTextureColorMod(atlas, 255, 255, 255);
    SDL_SetTextureAlphaMod(atlas, 255);
}
