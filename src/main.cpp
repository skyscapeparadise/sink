#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <iostream>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <mutex>
#include <thread>
#include <queue>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include "terminal_grid.hpp"
#include "pty_bridge.hpp"
#include "ansi_parser.hpp"
#include "video_engine.hpp"
#include "settings_ui.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#if defined(__APPLE__)
#include "macos_menu.h"
#endif

// Forward declaration of loop iteration handler
SDL_AppResult SDL_AppIterate(void* appstate);

// Global AppState reference for menu timer callbacks
static void* g_app_state = nullptr;

extern "C" void trigger_menu_render_tick() {
    if (g_app_state) {
        SDL_AppIterate(g_app_state);
    }
}

// Forward declaration of lightweight inspect helper to check if a video file has HDR properties
static bool inspect_hdr(const std::string& filepath) {
    bool is_hdr = false;
    AVFormatContext* temp_fmt_ctx = nullptr;

    if (avformat_open_input(&temp_fmt_ctx, filepath.c_str(), nullptr, nullptr) == 0) {
        if (avformat_find_stream_info(temp_fmt_ctx, nullptr) >= 0) {
            for (unsigned int i = 0; i < temp_fmt_ctx->nb_streams; i++) {
                if (temp_fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                    AVCodecParameters* codec_params = temp_fmt_ctx->streams[i]->codecpar;
                    if (codec_params->color_trc == AVCOL_TRC_SMPTE2084 || 
                        codec_params->color_trc == AVCOL_TRC_ARIB_STD_B67 ||
                        codec_params->color_primaries == AVCOL_PRI_BT2020) {
                        is_hdr = true;
                    }
                    break;
                }
            }
        }
        avformat_close_input(&temp_fmt_ctx);
    }
    return is_hdr;
}

// TerminalWindow struct contains all state properties for a single tab/window
struct TerminalWindow {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    FontManager font_manager;
    TerminalGrid terminal;
    PTYBridge pty;
    ANSIParser parser;
    VideoEngine video_engine;
    
    bool has_video = false;
    float exposure = 0.7f;
    bool animated_typing = true;
    std::vector<char> animation_buffer;
    float scroll_accumulator = 0.0f;

    // Dissolve state
    enum FadeState { FADE_HOLD_BLACK, FADE_OUT, FADE_DONE };
    FadeState fade_state = FADE_HOLD_BLACK;
    float fade_opacity = 1.0f;

    float cell_w = 0.0f;
    float cell_h = 0.0f;
    int mouse_down_col = -1;
    int mouse_down_row = -1;
};

// AppState holds the application global coordinates and window pointers
struct AppState {
    std::vector<TerminalWindow*> windows;
    TerminalWindow* active_window = nullptr;
    SettingsUI settings_ui;

    std::string video_path;
    bool video_is_hdr = false;
    std::string font_path;
    float padding = 2.0f;
    float base_font_size = 15.0f;
    float display_scale = 1.0f;
    Uint64 last_tick = 0;
    bool input_broadcasting = false;
    float exposure = 0.7f;
    bool animated_typing = true;
};

static std::string resolve_default_video_path() {
    std::string default_video = "sinkpool.mp4";
    std::string resolved_video = get_bundle_resource_path(default_video);
    FILE* f_vid = fopen(resolved_video.c_str(), "r");
    if (f_vid) {
        fclose(f_vid);
        return resolved_video;
    }
    
    // Check fallback locations
    const char* fallbacks[] = {
        "sinkpool.mp4",
        "../sinkpool.mp4",
        "/Users/kady/Projects/sink/sinkpool.mp4"
    };
    for (const char* fb : fallbacks) {
        FILE* f_fb = fopen(fb, "r");
        if (f_fb) {
            fclose(f_fb);
            return fb;
        }
    }
    return default_video;
}

static std::string resolve_default_font_path() {
    std::string resolved_font = get_bundle_resource_path("MonaspaceNeon-Regular.otf");
    FILE* f_font = fopen(resolved_font.c_str(), "r");
    if (f_font) {
        fclose(f_font);
        return resolved_font;
    }
    const char* fallbacks[] = {
        "fonts/MonaspaceNeon-Regular.otf",
        "../fonts/MonaspaceNeon-Regular.otf",
        "/Users/kady/Projects/sink/fonts/MonaspaceNeon-Regular.otf",
        "/System/Library/Fonts/SFNSMono.ttf",
        "/System/Library/Fonts/Supplemental/Courier New.ttf"
    };
    for (const char* path : fallbacks) {
        FILE* f_fb = fopen(path, "r");
        if (f_fb) {
            fclose(f_fb);
            return path;
        }
    }
    return "Courier New.ttf";
}

static void save_config(AppState* state) {
    const char* home = getenv("HOME");
    if (!home) return;
    std::string config_dir = std::string(home) + "/.config/sink";
    mkdir(config_dir.c_str(), 0755); // Ensure folder exists
    
    std::ofstream f(config_dir + "/config.txt");
    if (f.is_open()) {
        std::string v_path = state->video_path;
        if (v_path.find("sinkpool.mp4") != std::string::npos) {
            v_path = "default";
        }
        
        std::string f_path = state->font_path;
        if (f_path.find("MonaspaceNeon-Regular.otf") != std::string::npos) {
            f_path = "default";
        }
        
        f << "video_path=" << v_path << "\n";
        f << "font_path=" << f_path << "\n";
        f << "exposure=" << state->exposure << "\n";
        f << "animated_typing=" << (state->animated_typing ? "true" : "false") << "\n";
        f.close();
    }
}

static void load_config(AppState* state) {
    const char* home = getenv("HOME");
    if (!home) return;
    std::ifstream f(std::string(home) + "/.config/sink/config.txt");
    if (f.is_open()) {
        std::string line;
        while (std::getline(f, line)) {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            
            if (key == "video_path") {
                if (val == "default" || val.empty()) {
                    state->video_path = resolve_default_video_path();
                } else {
                    state->video_path = val;
                }
            } else if (key == "font_path") {
                if (val == "default" || val.empty()) {
                    state->font_path = resolve_default_font_path();
                } else {
                    state->font_path = val;
                }
            } else if (key == "exposure") {
                try {
                    state->exposure = std::stof(val);
                } catch (...) {}
            } else if (key == "animated_typing") {
                state->animated_typing = (val == "true");
            }
        }
        f.close();
        
        // Apply loaded settings to any active windows
        for (auto* tw : state->windows) {
            tw->exposure = state->exposure;
            tw->animated_typing = state->animated_typing;
        }
    }
}

// Spawn a new terminal window container
static TerminalWindow* create_terminal_window(AppState* state, SDL_Window* parent_tab_window) {
    TerminalWindow* tw = new TerminalWindow();

    // Create Window
    tw->window = SDL_CreateWindow(
        "sink",
        1000, 480,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
    );
    if (!tw->window) {
        std::cerr << "SDL_CreateWindow Error: " << SDL_GetError() << std::endl;
        delete tw;
        return nullptr;
    }

    // Create Renderer. Linear colorspace if background video is HDR.
    if (state->video_is_hdr) {
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, tw->window);
        SDL_SetNumberProperty(props, SDL_PROP_RENDERER_CREATE_OUTPUT_COLORSPACE_NUMBER, SDL_COLORSPACE_SRGB_LINEAR);
        tw->renderer = SDL_CreateRendererWithProperties(props);
        SDL_DestroyProperties(props);
    } else {
        tw->renderer = SDL_CreateRenderer(tw->window, nullptr);
    }

    if (!tw->renderer) {
        std::cerr << "SDL_CreateRenderer Error: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(tw->window);
        delete tw;
        return nullptr;
    }

    float scale = SDL_GetWindowDisplayScale(tw->window);
    if (scale <= 0.0f) scale = 1.0f;
    state->display_scale = scale;

    std::string font_path = state->font_path;
    if (font_path.empty()) {
        font_path = "/System/Library/Fonts/SFNSMono.ttf";
    }

    float font_size_px = state->base_font_size * scale;
    if (!tw->font_manager.load_font(tw->renderer, font_path, font_size_px, false)) {
        font_path = "/System/Library/Fonts/Supplemental/Courier New.ttf";
        tw->font_manager.load_font(tw->renderer, font_path, font_size_px, false);
    }

    tw->cell_w = (tw->font_manager.get_cell_width() / scale) + 1.0f;
    tw->cell_h = (tw->font_manager.get_cell_height() / scale) - 0.8f;

    // Settle initial grid scale dimensions
    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(tw->window, &win_w, &win_h);
    int cols = std::max(40, static_cast<int>((win_w - 2 * state->padding) / tw->cell_w));
    int rows = std::max(10, static_cast<int>((win_h - 2 * state->padding) / tw->cell_h));

    tw->terminal.resize(cols, rows);
    tw->terminal.clear_screen();

    // Start Pseudo-Terminal process connection
    if (!tw->pty.spawn(cols, rows)) {
        std::cerr << "Failed to initialize PTY shell context" << std::endl;
    }

    SDL_StartTextInput(tw->window);

    // Setup Video Background Engine
    if (!state->video_path.empty() && state->video_path != "None") {
        if (tw->video_engine.open_video(tw->renderer, state->video_path)) {
            tw->video_engine.start();
            tw->has_video = true;
        }
    }

    tw->animated_typing = state->animated_typing;
    tw->exposure = state->exposure;
    tw->fade_state = TerminalWindow::FADE_HOLD_BLACK;
    tw->fade_opacity = 1.0f;
    tw->scroll_accumulator = 0.0f;

    // Attach as tab if a parent window is present
    if (parent_tab_window) {
        add_window_as_tab(parent_tab_window, tw->window);
    }

    return tw;
}

// Safely terminate and destroy a terminal window context
static void destroy_terminal_window(TerminalWindow* tw) {
    if (!tw) return;
    tw->pty.shutdown();
    tw->video_engine.stop();
    tw->video_engine.close_video();
    if (tw->renderer) {
        SDL_DestroyRenderer(tw->renderer);
    }
    if (tw->window) {
        SDL_DestroyWindow(tw->window);
    }
    delete tw;
}

// SDL3 Application initialization entry point
SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[]) {
    SDL_SetAppMetadata("sink", "0.5.0", "com.rainmultimedia.sink");
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_CREATOR_STRING, "rain multimedia");
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_COPYRIGHT_STRING, "copyright © 2026 rain multimedia. all rights reserved.");

    // Initialize SDL3 Video
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL_Init Error: " << SDL_GetError() << std::endl;
        return SDL_APP_FAILURE;
    }

    AppState* state = new AppState();
    *appstate = state;
    g_app_state = state;

    state->video_path = resolve_default_video_path();

    // Load config if present
    load_config(state);

    if (argc > 1) {
        state->video_path = argv[1];
        state->video_is_hdr = inspect_hdr(state->video_path);
    }

    state->font_path = resolve_default_font_path();

    // Spawn first window
    TerminalWindow* tw = create_terminal_window(state, nullptr);
    if (!tw) {
        delete state;
        return SDL_APP_FAILURE;
    }
    state->windows.push_back(tw);
    state->active_window = tw;
    state->last_tick = SDL_GetTicks();

#if defined(__APPLE__)
    setup_macos_menu();
#endif

    return SDL_APP_CONTINUE;
}

// SDL3 Application event processor entry point
SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) {
    AppState* state = static_cast<AppState*>(appstate);
    if (!state) return SDL_APP_FAILURE;

    // Handle macOS menu settings trigger
    if (get_settings_requested() && !state->settings_ui.is_open()) {
        SDL_Window* parent_win = state->active_window ? state->active_window->window : nullptr;
        if (parent_win && state->settings_ui.open(parent_win)) {
            state->settings_ui.set_paths(state->video_path, state->font_path);
            state->settings_ui.set_animated_typing(state->active_window ? state->active_window->animated_typing : true);
            state->settings_ui.set_exposure(state->active_window ? state->active_window->exposure : 1.0f);
        }
    }

    // Forward events to Settings UI window if active
    if (state->settings_ui.is_open()) {
        state->settings_ui.process_event(*event);
    }

    if (event->type == SDL_EVENT_QUIT) {
        return SDL_APP_SUCCESS;
    }

    // Resolve correct window ID matching the current event union state
    SDL_WindowID win_id = 0;
    if (event->type >= SDL_EVENT_WINDOW_FIRST && event->type <= SDL_EVENT_WINDOW_LAST) {
        win_id = event->window.windowID;
    } else if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) {
        win_id = event->key.windowID;
    } else if (event->type == SDL_EVENT_TEXT_INPUT) {
        win_id = event->text.windowID;
    } else if (event->type == SDL_EVENT_MOUSE_MOTION) {
        win_id = event->motion.windowID;
    } else if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN || event->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        win_id = event->button.windowID;
    } else if (event->type == SDL_EVENT_MOUSE_WHEEL) {
        win_id = event->wheel.windowID;
    }

    // Identify target Terminal Window
    TerminalWindow* target_tw = nullptr;
    if (win_id != 0) {
        for (auto* tw : state->windows) {
            if (tw->window && SDL_GetWindowID(tw->window) == win_id) {
                target_tw = tw;
                break;
            }
        }
    }

    if (!target_tw) {
        target_tw = state->active_window;
    }

    if (!target_tw) {
        return SDL_APP_CONTINUE;
    }

    if (event->type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
        SDL_WindowID focus_win_id = event->window.windowID;
        for (auto* tw : state->windows) {
            if (tw->window && SDL_GetWindowID(tw->window) == focus_win_id) {
                state->active_window = tw;
                break;
            }
        }
    }

    if (event->type == SDL_EVENT_MOUSE_WHEEL) {
        if (state->settings_ui.is_open() && SDL_GetKeyboardFocus() == state->settings_ui.get_window()) {
            return SDL_APP_CONTINUE;
        }
        float wheel_y = -event->wheel.y;
        if (event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            wheel_y = -wheel_y;
        }
        
        target_tw->scroll_accumulator += wheel_y;
        int lines = 0;
        if (target_tw->scroll_accumulator >= 1.0f) {
            lines = static_cast<int>(std::floor(target_tw->scroll_accumulator));
            target_tw->scroll_accumulator -= lines;
        } else if (target_tw->scroll_accumulator <= -1.0f) {
            lines = static_cast<int>(std::ceil(target_tw->scroll_accumulator));
            target_tw->scroll_accumulator -= lines;
        }
        
        if (lines != 0) {
            target_tw->terminal.scroll_view(lines);
        }
    } else if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if (event->button.windowID == SDL_GetWindowID(target_tw->window)) {
            if (event->button.button == SDL_BUTTON_LEFT) {
                float mx = event->button.x;
                float my = event->button.y;
                
                int col = static_cast<int>((mx - state->padding) / target_tw->cell_w);
                int row = static_cast<int>((my - state->padding) / target_tw->cell_h);
                
                int clicks = event->button.clicks;
                if (clicks == 1) {
                    target_tw->mouse_down_col = col;
                    target_tw->mouse_down_row = row;
                    target_tw->terminal.start_selection(col, row);
                } else if (clicks == 2) {
                    // Double click: select word
                    target_tw->terminal.select_word_at(col, row);
                    std::string selected_text = target_tw->terminal.get_selected_text();
                    if (!selected_text.empty()) {
                        SDL_SetClipboardText(selected_text.c_str());
                    }
                } else if (clicks == 3) {
                    // Triple click: select line
                    target_tw->terminal.select_line_at(row);
                    std::string selected_text = target_tw->terminal.get_selected_text();
                    if (!selected_text.empty()) {
                        SDL_SetClipboardText(selected_text.c_str());
                    }
                }
            }
        }
    } else if (event->type == SDL_EVENT_MOUSE_MOTION) {
        if (event->motion.windowID == SDL_GetWindowID(target_tw->window)) {
            float mx = event->motion.x;
            float my = event->motion.y;
            
            int col = static_cast<int>((mx - state->padding) / target_tw->cell_w);
            int row = static_cast<int>((my - state->padding) / target_tw->cell_h);
            
            if (target_tw->terminal.is_selecting()) {
                target_tw->terminal.update_selection(col, row);
            }
        }
    } else if (event->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        if (event->button.windowID == SDL_GetWindowID(target_tw->window)) {
            if (event->button.button == SDL_BUTTON_LEFT) {
                float mx = event->button.x;
                float my = event->button.y;
                
                int col = static_cast<int>((mx - state->padding) / target_tw->cell_w);
                int row = static_cast<int>((my - state->padding) / target_tw->cell_h);
                
                int clicks = event->button.clicks;
                if (clicks == 1 && col == target_tw->mouse_down_col && row == target_tw->mouse_down_row) {
                    // Snapping cursor on mouse release
                    if (!target_tw->terminal.is_alt_screen_active() && row == target_tw->terminal.get_cursor_row()) {
                        if (target_tw->terminal.get_prompt_boundary() == -1) {
                            target_tw->terminal.set_prompt_boundary(target_tw->terminal.get_cursor_col());
                        }
                        
                        if (col >= target_tw->terminal.get_prompt_boundary()) {
                            int current_col = target_tw->terminal.get_cursor_col();
                            int offset = col - current_col;
                            
                            std::string move_payload;
                            if (offset > 0) {
                                for (int o = 0; o < offset; ++o) {
                                    move_payload += "\x1b[C"; // Standard Right Arrow
                                }
                            } else if (offset < 0) {
                                for (int o = 0; o < -offset; ++o) {
                                    move_payload += "\x1b[D"; // Standard Left Arrow
                                }
                            }
                            if (!move_payload.empty()) {
                                target_tw->pty.write_to_pty(move_payload.data(), move_payload.size());
                            }
                            target_tw->terminal.clear_selection();
                        }
                    }
                }
                target_tw->terminal.end_selection();
            }
        }
    } else if (event->type == SDL_EVENT_TEXT_INPUT) {
        if (state->settings_ui.is_open() && SDL_GetKeyboardFocus() == state->settings_ui.get_window()) {
            return SDL_APP_CONTINUE;
        }
        if (state->input_broadcasting) {
            for (auto* tw : state->windows) {
                tw->terminal.clear_selection();
                tw->terminal.reset_scroll();
                tw->pty.write_to_pty(event->text.text, std::strlen(event->text.text));
            }
        } else {
            target_tw->terminal.clear_selection();
            target_tw->terminal.reset_scroll();
            target_tw->pty.write_to_pty(event->text.text, std::strlen(event->text.text));
        }
    } else if (event->type == SDL_EVENT_KEY_DOWN) {
        SDL_Keycode sym = event->key.key;
        SDL_Keymod mod = event->key.mod;

        // Cmd+Comma settings window trigger
        if ((mod & SDL_KMOD_GUI) && sym == SDLK_COMMA) {
            if (!state->settings_ui.is_open()) {
                if (state->settings_ui.open(target_tw->window)) {
                    state->settings_ui.set_paths(state->video_path, state->font_path);
                    state->settings_ui.set_animated_typing(target_tw->animated_typing);
                    state->settings_ui.set_exposure(target_tw->exposure);
                    state->settings_ui.set_broadcasting(state->input_broadcasting);
                }
            } else {
                state->settings_ui.close();
            }
            return SDL_APP_CONTINUE;
        }

        // Ignore input if settings has focus
        if (state->settings_ui.is_open() && SDL_GetKeyboardFocus() == state->settings_ui.get_window()) {
            return SDL_APP_CONTINUE;
        }

        std::vector<TerminalWindow*> target_windows;
        if (state->input_broadcasting) {
            target_windows = state->windows;
        } else {
            target_windows.push_back(target_tw);
        }

        for (auto* tw : target_windows) {
            if (tw->terminal.get_prompt_boundary() == -1) {
                tw->terminal.set_prompt_boundary(tw->terminal.get_cursor_col());
            }
            if (!(mod & SDL_KMOD_GUI) && sym != SDLK_BACKSPACE && sym != SDLK_DELETE) {
                tw->terminal.clear_selection();
            }
            tw->terminal.reset_scroll();

            if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
                const char c = '\r';
                tw->pty.write_to_pty(&c, 1);
            } else if (sym == SDLK_BACKSPACE || sym == SDLK_DELETE) {
                bool handled = false;
                std::cerr << "[BACKSPACE] has_selection=" << tw->terminal.has_selection()
                          << " is_alt=" << tw->terminal.is_alt_screen_active() << std::endl;
                if (!tw->terminal.is_alt_screen_active() && tw->terminal.has_selection()) {
                    int r0 = tw->terminal.get_select_start_row();
                    int r1 = tw->terminal.get_select_end_row();
                    int total_history = static_cast<int>(tw->terminal.get_scrollback_size());
                    int cursor_row_grid = tw->terminal.get_cursor_row() + total_history;
                    int prompt_boundary = tw->terminal.get_prompt_boundary();
                    int c0 = tw->terminal.get_select_start_col();
                    int c1 = tw->terminal.get_select_end_col();
                    int start_col = std::min(c0, c1);
                    int end_col = std::max(c0, c1);
                    
                    std::cerr << "[BACKSPACE] r0=" << r0 << " r1=" << r1
                              << " cursor_row_grid=" << cursor_row_grid
                              << " prompt_boundary=" << prompt_boundary
                              << " c0=" << c0 << " c1=" << c1
                              << " start_col=" << start_col << " end_col=" << end_col << std::endl;

                    if (r0 == r1 && r0 == cursor_row_grid) {
                        std::cerr << "[BACKSPACE] Row match! Checking prompt boundary..." << std::endl;
                        if (prompt_boundary != -1 && start_col >= prompt_boundary) {
                            int cursor_col = tw->terminal.get_cursor_col();
                            int len = end_col - start_col + 1;
                            
                            std::cerr << "[BACKSPACE] Boundary match! cursor_col=" << cursor_col
                                      << " len=" << len << std::endl;

                            // 1. Move cursor to start_col
                            std::string payload;
                            int target_pos = start_col;
                            if (cursor_col < target_pos) {
                                for (int i = 0; i < (target_pos - cursor_col); ++i) {
                                    payload += "\x1b[C"; // Standard Right Arrow
                                }
                            } else if (cursor_col > target_pos) {
                                for (int i = 0; i < (cursor_col - target_pos); ++i) {
                                    payload += "\x1b[D"; // Standard Left Arrow
                                }
                            }
                            
                            // 2. Send Delete keys to delete the selection forward
                            for (int i = 0; i < len; ++i) {
                                payload += "\x1b[3~"; // vt100 delete key sequence
                            }
                            
                            // 3. Move cursor back to its target position
                            if (cursor_col > start_col) {
                                if (cursor_col >= start_col + len) {
                                    int final_target = cursor_col - len;
                                    for (int i = 0; i < (final_target - start_col); ++i) {
                                        payload += "\x1b[C"; // Standard Right Arrow
                                    }
                                }
                            } else if (cursor_col < start_col) {
                                for (int i = 0; i < (start_col - cursor_col); ++i) {
                                    payload += "\x1b[D"; // Standard Left Arrow
                                }
                            }
                            
                            std::cerr << "[BACKSPACE] payload size=" << payload.size() << " payload=";
                            for (char c : payload) {
                                if (c == '\x1b') std::cerr << "\\e";
                                else if (c == '\x7f') std::cerr << "\\x7f";
                                else std::cerr << c;
                            }
                            std::cerr << std::endl;

                            tw->pty.write_to_pty(payload.data(), payload.size());
                            
                            tw->terminal.clear_selection();
                            handled = true;
                        }
                    }
                }
                
                if (!handled) {
                    if (sym == SDLK_DELETE) {
                        tw->pty.write_to_pty("\x1b[3~", 4); // Standard vt100 delete key sequence
                    } else {
                        const char c = '\x7f';
                        tw->pty.write_to_pty(&c, 1);
                    }
                }
            } else if (sym == SDLK_TAB) {
                const char c = '\t';
                tw->pty.write_to_pty(&c, 1);
            } else if (sym == SDLK_ESCAPE) {
                const char c = '\x1b';
                tw->pty.write_to_pty(&c, 1);
            } else if (sym == SDLK_UP) {
                tw->pty.write_to_pty("\x1b[A", 3);
            } else if (sym == SDLK_DOWN) {
                tw->pty.write_to_pty("\x1b[B", 3);
            } else if (sym == SDLK_RIGHT) {
                tw->pty.write_to_pty("\x1b[C", 3);
            } else if (sym == SDLK_LEFT) {
                tw->pty.write_to_pty("\x1b[D", 3);
            }
        }
    } else if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        if (event->window.windowID == SDL_GetWindowID(target_tw->window)) {
            auto it = std::find(state->windows.begin(), state->windows.end(), target_tw);
            if (it != state->windows.end()) {
                state->windows.erase(it);
            }
            destroy_terminal_window(target_tw);
            if (state->windows.empty()) {
                return SDL_APP_SUCCESS;
            }
            state->active_window = state->windows[0];
        }
    } else if (event->type == SDL_EVENT_WINDOW_RESIZED) {
        if (event->window.windowID == SDL_GetWindowID(target_tw->window)) {
            int w = event->window.data1;
            int h = event->window.data2;
            if (w > 0 && h > 0) {
                int new_cols = std::max(40, static_cast<int>((w - 2 * state->padding) / target_tw->cell_w));
                int new_rows = std::max(10, static_cast<int>((h - 2 * state->padding) / target_tw->cell_h));
                
                target_tw->terminal.resize(new_cols, new_rows);
                target_tw->pty.resize_pty(new_cols, new_rows);
            }
        }
    } else if (event->type == SDL_EVENT_USER) {
        if (event->user.code == 1) { // Media file selected
            char* path = (char*)event->user.data1;
            std::cout << "Settings Event: background changed: " << path << std::endl;

            state->video_path = path;
            state->video_is_hdr = inspect_hdr(state->video_path);

            for (auto* tw : state->windows) {
                tw->video_engine.stop();
                tw->video_engine.close_video();
                tw->has_video = false;
                if (!state->video_path.empty() && state->video_path != "None") {
                    if (tw->video_engine.open_video(tw->renderer, state->video_path)) {
                        tw->video_engine.start();
                        tw->has_video = true;
                        tw->fade_state = TerminalWindow::FADE_HOLD_BLACK;
                        tw->fade_opacity = 1.0f;
                    }
                }
            }

            state->settings_ui.set_paths(state->video_path, state->font_path);
            save_config(state);
            free(path);
        } else if (event->user.code == 2) { // Font selected
            char* path = (char*)event->user.data1;
            std::cout << "Settings Event: font changed: " << path << std::endl;

            std::string new_font_path = path;
            state->font_path = new_font_path;

            for (auto* tw : state->windows) {
                if (tw->font_manager.load_font(tw->renderer, new_font_path, state->base_font_size * state->display_scale, false)) {
                    tw->cell_w = (tw->font_manager.get_cell_width() / state->display_scale) + 1.0f;
                    tw->cell_h = (tw->font_manager.get_cell_height() / state->display_scale) - 0.8f;

                    int w, h;
                    SDL_GetWindowSize(tw->window, &w, &h);
                    int new_cols = std::max(40, static_cast<int>((w - 2 * state->padding) / tw->cell_w));
                    int new_rows = std::max(10, static_cast<int>((h - 2 * state->padding) / tw->cell_h));
                    
                    tw->terminal.resize(new_cols, new_rows);
                    tw->pty.resize_pty(new_cols, new_rows);
                }
            }

            state->settings_ui.set_paths(state->video_path, state->font_path);
            save_config(state);
            free(path);
        } else if (event->user.code == 3) { // Clear media -> reset to default pool video background
            std::cout << "Settings Event: background cleared to default." << std::endl;
            state->video_path = resolve_default_video_path();
            state->video_is_hdr = inspect_hdr(state->video_path);
            for (auto* tw : state->windows) {
                tw->video_engine.stop();
                tw->video_engine.close_video();
                tw->has_video = false;
                if (tw->video_engine.open_video(tw->renderer, state->video_path)) {
                    tw->video_engine.start();
                    tw->has_video = true;
                    tw->fade_state = TerminalWindow::FADE_HOLD_BLACK;
                    tw->fade_opacity = 1.0f;
                }
            }
            state->settings_ui.set_paths(state->video_path, state->font_path);
            save_config(state);
        } else if (event->user.code == 4) { // Typing effect toggle
            bool anim = (bool)(intptr_t)event->user.data1;
            std::cout << "Settings Event: typing effect toggled: " << (anim ? "ON" : "OFF") << std::endl;
            state->animated_typing = anim;
            for (auto* tw : state->windows) {
                tw->animated_typing = anim;
                if (!tw->animated_typing) {
                    if (!tw->animation_buffer.empty()) {
                        tw->parser.parse(tw->terminal, tw->animation_buffer.data(), tw->animation_buffer.size());
                        tw->animation_buffer.clear();
                    }
                }
            }
            save_config(state);
        } else if (event->user.code == 5) { // Input broadcasting toggle
            bool broadcast = (bool)(intptr_t)event->user.data1;
            std::cout << "Settings Event: input broadcasting toggled: " << (broadcast ? "ON" : "OFF") << std::endl;
            state->input_broadcasting = broadcast;
        }
    }

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate) {
    AppState* state = static_cast<AppState*>(appstate);
    if (!state) return SDL_APP_FAILURE;

    // Process native macOS menu clipboard actions
    if (state->active_window) {
        TerminalWindow* tw = state->active_window;
        if (get_copy_requested()) {
            std::string selected_text = tw->terminal.get_selected_text();
            if (!selected_text.empty()) {
                SDL_SetClipboardText(selected_text.c_str());
            }
        }
        if (get_paste_requested()) {
            if (SDL_HasClipboardText()) {
                char* text = SDL_GetClipboardText();
                if (text) {
                    if (tw->terminal.is_bracketed_paste_active()) {
                        // Wrap in bracketed-paste markers so the shell treats the
                        // content as literal text instead of executing embedded newlines.
                        tw->pty.write_to_pty("\x1b[200~", 6);
                        tw->pty.write_to_pty(text, strlen(text));
                        tw->pty.write_to_pty("\x1b[201~", 6);
                    } else {
                        tw->pty.write_to_pty(text, strlen(text));
                    }
                    SDL_free(text);
                }
            }
        }
        if (get_select_all_requested()) {
            tw->terminal.select_all();
        }
        if (get_print_requested()) {
            std::string print_text = tw->terminal.get_selected_text();
            if (print_text.empty()) {
                print_text = tw->terminal.get_all_text();
            }
            trigger_print_dialog(print_text.c_str());
        }
    }

    // Process Window spawning menu actions
    if (get_new_window_requested()) {
        TerminalWindow* tw = create_terminal_window(state, nullptr);
        if (tw) {
            state->windows.push_back(tw);
            state->active_window = tw;
        }
    }
    if (get_new_tab_requested()) {
        SDL_Window* parent_win = state->active_window ? state->active_window->window : nullptr;
        TerminalWindow* tw = create_terminal_window(state, parent_win);
        if (tw) {
            state->windows.push_back(tw);
            state->active_window = tw;
        }
    }
    if (get_close_window_requested()) {
        if (state->active_window) {
            TerminalWindow* tw = state->active_window;
            auto it = std::find(state->windows.begin(), state->windows.end(), tw);
            if (it != state->windows.end()) {
                state->windows.erase(it);
            }
            destroy_terminal_window(tw);
            if (state->windows.empty()) {
                return SDL_APP_SUCCESS;
            }
            state->active_window = state->windows[0];
        }
    }

    // Delta time calculation
    Uint64 current_tick = SDL_GetTicks();
    Uint64 elapsed = current_tick - state->last_tick;
    if (elapsed < 10) { // Throttle duplicate updates if called too quickly by Cocoa's common modes timer
        return SDL_APP_CONTINUE;
    }
    float dt = elapsed / 1000.0f;
    state->last_tick = current_tick;

    // Sync exposure setting from UI in real-time
    if (state->settings_ui.is_open()) {
        float exp = state->settings_ui.get_exposure();
        if (state->exposure != exp) {
            state->exposure = exp;
            for (auto* tw : state->windows) {
                tw->exposure = exp;
            }
            save_config(state);
        }
    }

    // Iterate all active windows
    for (auto* tw : state->windows) {
        tw->terminal.update_timers(dt);
        if (tw->has_video) {
            if (tw->fade_state == TerminalWindow::FADE_HOLD_BLACK) {
                if (tw->video_engine.has_rendered_first_frame()) {
                    tw->fade_state = TerminalWindow::FADE_OUT;
                }
            } else if (tw->fade_state == TerminalWindow::FADE_OUT) {
                tw->fade_opacity -= dt * 4.0f;
                if (tw->fade_opacity <= 0.0f) {
                    tw->fade_opacity = 0.0f;
                    tw->fade_state = TerminalWindow::FADE_DONE;
                }
            }
        } else {
            tw->fade_state = TerminalWindow::FADE_DONE;
            tw->fade_opacity = 0.0f;
        }

        // Process incoming shell data
        std::vector<char> output = tw->pty.read_pending();
        if (!output.empty()) {
            std::cerr << "[PTY_READ] size=" << output.size() << " content=";
            for (char c : output) {
                if (c == '\x1b') std::cerr << "\\e";
                else if (c == '\x7f') std::cerr << "\\x7f";
                else if (c == '\x08') std::cerr << "\\b";
                else if (c < 32 || c > 126) std::cerr << "\\x" << std::hex << (int)(unsigned char)c << std::dec;
                else std::cerr << c;
            }
            std::cerr << std::endl;

            if (tw->animated_typing) {
                if (output.size() > 5) {
                    // Large chunk / burst output (command output): bypass typing effect to maintain performance
                    if (!tw->animation_buffer.empty()) {
                        tw->parser.parse(tw->terminal, tw->animation_buffer.data(), tw->animation_buffer.size());
                        tw->animation_buffer.clear();
                    }
                    tw->parser.parse(tw->terminal, output.data(), output.size());
                } else {
                    // Small chunk (user typing): queue for animated typing
                    tw->animation_buffer.insert(tw->animation_buffer.end(), output.begin(), output.end());
                }
            } else {
                tw->parser.parse(tw->terminal, output.data(), output.size());
            }
            tw->terminal.lock_prompt_boundary_if_unset();
        }

        // Process retro animated typing ticks
        if (tw->animated_typing && !tw->animation_buffer.empty()) {
            size_t total_pending = tw->animation_buffer.size();
            size_t chars_to_process = 0;
            
            if (total_pending > 2000) {
                chars_to_process = total_pending;
            } else if (total_pending > 500) {
                chars_to_process = std::min(total_pending, (size_t)16);
            } else {
                chars_to_process = std::min(total_pending, (size_t)4);
            }
            
            tw->parser.parse(tw->terminal, tw->animation_buffer.data(), chars_to_process);
            tw->animation_buffer.erase(tw->animation_buffer.begin(), tw->animation_buffer.begin() + chars_to_process);
            
            if (tw->animation_buffer.empty()) {
                tw->terminal.lock_prompt_boundary_if_unset();
            }
        }

        // Render Active Window Context
        SDL_SetRenderDrawColor(tw->renderer, 0, 0, 0, 255);
        SDL_RenderClear(tw->renderer);

        int draw_w = 0, draw_h = 0;
        SDL_GetRenderOutputSize(tw->renderer, &draw_w, &draw_h);

        // A. Render video background YUV frame if active
        if (tw->has_video) {
            tw->video_engine.update_frame(tw->renderer, dt);
            SDL_Texture* frame_tex = tw->video_engine.get_texture();
            if (frame_tex) {
                SDL_SetRenderColorScale(tw->renderer, tw->exposure);
                SDL_RenderTexture(tw->renderer, frame_tex, nullptr, nullptr);
                SDL_SetRenderColorScale(tw->renderer, 1.0f); // Reset color scale
            }
        }

        // B. Render grid cells
        tw->terminal.render(tw->renderer, tw->font_manager, state->padding, state->padding, state->display_scale, dt, tw->animated_typing);

        // C. Draw black dissolve overlay mask
        if (tw->fade_opacity > 0.0f) {
            SDL_FRect full_rect = { 0.0f, 0.0f, static_cast<float>(draw_w), static_cast<float>(draw_h) };
            SDL_SetRenderDrawBlendMode(tw->renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(tw->renderer, 0, 0, 0, static_cast<Uint8>(tw->fade_opacity * 255.0f));
            SDL_RenderFillRect(tw->renderer, &full_rect);
        }

        SDL_RenderPresent(tw->renderer);
    }

    // Draw Settings UI if open
    if (state->settings_ui.is_open()) {
        state->settings_ui.render();
    }

    return SDL_APP_CONTINUE;
}

// SDL3 Application shutdown clean-up entry point
void SDL_AppQuit(void* appstate, SDL_AppResult result) {
    AppState* state = static_cast<AppState*>(appstate);
    if (state) {
        state->settings_ui.close();
        for (auto* tw : state->windows) {
            destroy_terminal_window(tw);
        }
        delete state;
    }
}
