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
#include "sink_demo.hpp"
#include "preset_manager.hpp"
#include "hue_shift.hpp"
#include "crt_shader.hpp"

#if defined(__APPLE__)
#include "macos_menu.h"
#endif

// Forward declaration of loop iteration handler
SDL_AppResult SDL_AppIterate(void* appstate);

// Global AppState reference for menu timer callbacks
static void* g_app_state = nullptr;

// How much brighter than normal (1.0) "hdr console" text renders. Values
// above 1.0 only actually look brighter than SDR white -- rather than just
// clamping to white -- because every renderer is created with the
// linear/extended colorspace (see the renderer-creation comment in
// create_terminal_window).
static const float kHdrConsoleTextBoost = 1.6f;

extern "C" void trigger_menu_render_tick() {
    if (g_app_state) {
        SDL_AppIterate(g_app_state);
    }
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
    std::mutex grid_mutex;
    std::atomic<bool> demo_running{false};
    std::atomic<bool> demo_skip_requested{false};
    std::atomic<bool> demo_abort{false};
    std::thread demo_thread;

    bool has_video = false;
    float exposure = 0.7f;
    float hue_shift_degrees = 0.0f;
    HueShiftEffect hue_shift;
    CrtShaderEffect crt_shader;
    bool animated_typing = true;
    std::vector<char> animation_buffer;
    Uint64 last_output_chunk_time = 0;
    int rapid_chunk_streak = 0;
    float scroll_accumulator = 0.0f;
    float scroll_velocity = 0.0f;
    Uint64 last_wheel_time = 0;

    // Dissolve state
    enum FadeState { FADE_HOLD_BLACK, FADE_OUT, FADE_DONE };
    FadeState fade_state = FADE_HOLD_BLACK;
    float fade_opacity = 1.0f;

    // Feature states
    bool search_drawer_open = false;
    std::string search_input_text;
    bool crt_mode_enabled = false;
    bool vibrancy_enabled = true;
    bool hdr_console_enabled = false;

    float cell_w = 0.0f;
    float cell_h = 0.0f;
    int mouse_down_col = -1;
    int mouse_down_row = -1;

    // Debounces the (expensive: PTY SIGWINCH + full grid reflow) resize
    // work so a burst of resize events -- a fast native window-zoom
    // animation firing dozens of intermediate frames in ~200ms is the worst
    // case, but a manual edge-drag does this too -- doesn't apply a reflow
    // per intermediate frame (which was visibly corrupting the shell's
    // prompt redraw). Only the latest pending size is kept; it's applied
    // once no new resize event has arrived for a short quiet period.
    bool has_pending_resize = false;
    int pending_cols = 0;
    int pending_rows = 0;
    Uint64 last_resize_event_time = 0;
};

// AppState holds the application global coordinates and window pointers
struct AppState {
    std::vector<TerminalWindow*> windows;
    TerminalWindow* active_window = nullptr;
    SettingsUI settings_ui;

    std::string video_path;
    std::string font_path;
    std::string active_preset_name = "pool";
    float padding = 2.0f;
    float base_font_size = 15.0f;
    float display_scale = 1.0f;
    Uint64 last_tick = 0;
    bool input_broadcasting = false;
    float exposure = 0.7f;
    float hue_shift = 0.0f;
    bool animated_typing = true;
    bool vibrancy_enabled = true;
    bool crt_mode_enabled = false;
    bool ligatures_enabled = true;
    bool hdr_console_enabled = false;
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
        "../sinkpool.mp4"
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

// Persists the live settings as the active preset (presets/<name>.txt), plus
// a small config.txt pointing at which preset is active. This is called
// after every settings tweak, same as the old flat-config version was.
static void save_config(AppState* state) {
    const char* home = getenv("HOME");
    if (!home) return;
    std::string config_dir = std::string(home) + "/.config/sink";
    mkdir(config_dir.c_str(), 0755); // Ensure folder exists

    Preset p;
    p.name = state->active_preset_name;
    p.video_path = state->video_path;
    if (p.video_path.find("sinkpool.mp4") != std::string::npos) {
        p.video_path = "default";
    }
    p.font_path = state->font_path;
    if (p.font_path.find("MonaspaceNeon-Regular.otf") != std::string::npos) {
        p.font_path = "default";
    }
    p.exposure = state->exposure;
    p.hue_shift = state->hue_shift;
    p.animated_typing = state->animated_typing;
    p.vibrancy_enabled = state->vibrancy_enabled;
    p.crt_mode_enabled = state->crt_mode_enabled;
    p.ligatures_enabled = state->ligatures_enabled;
    p.hdr_console_enabled = state->hdr_console_enabled;
    presets::save(p);

    std::ofstream f(config_dir + "/config.txt");
    if (f.is_open()) {
        f << "active_preset=" << state->active_preset_name << "\n";
    }
}

// Loads the active preset (following config.txt's active_preset pointer)
// into `state`. On a pre-presets install, migrates the old flat config.txt
// fields into a "pool" preset instead of discarding them; on a fresh
// install, seeds "pool" with hardcoded baseline defaults. Either way,
// "pool" always ends up present on disk so there's a permanent fallback.
static void load_config(AppState* state) {
    const char* home = getenv("HOME");
    if (!home) return;

    std::string requested_preset;
    bool has_active_preset_key = false;
    Preset legacy;
    bool has_legacy_fields = false;

    std::ifstream f(std::string(home) + "/.config/sink/config.txt");
    if (f.is_open()) {
        std::string line;
        while (std::getline(f, line)) {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);

            if (key == "active_preset") {
                requested_preset = val.empty() ? "pool" : val;
                has_active_preset_key = true;
            } else if (key == "video_path") {
                legacy.video_path = val;
                has_legacy_fields = true;
            } else if (key == "font_path") {
                legacy.font_path = val;
                has_legacy_fields = true;
            } else if (key == "exposure") {
                try { legacy.exposure = std::stof(val); } catch (...) {}
            } else if (key == "animated_typing") {
                legacy.animated_typing = (val == "true");
            } else if (key == "vibrancy_enabled") {
                legacy.vibrancy_enabled = (val == "true");
            } else if (key == "crt_mode_enabled") {
                legacy.crt_mode_enabled = (val == "true");
            } else if (key == "ligatures_enabled") {
                legacy.ligatures_enabled = (val == "true");
            }
        }
        f.close();
    }

    Preset active;
    if (has_active_preset_key && presets::exists(requested_preset)) {
        active = presets::load(requested_preset);
    } else if (has_legacy_fields && !presets::exists("pool")) {
        legacy.name = "pool";
        presets::save(legacy);
        active = legacy;
    } else if (presets::exists("pool")) {
        active = presets::load("pool");
    } else {
        active = Preset{}; // hardcoded baseline: name="pool" with default look
    }

    if (!presets::exists(active.name)) {
        presets::save(active);
    }

    state->active_preset_name = active.name;
    state->video_path = (active.video_path.empty() || active.video_path == "default")
        ? resolve_default_video_path() : active.video_path;
    state->font_path = (active.font_path.empty() || active.font_path == "default")
        ? resolve_default_font_path() : active.font_path;
    state->exposure = active.exposure;
    state->hue_shift = active.hue_shift;
    state->animated_typing = active.animated_typing;
    state->vibrancy_enabled = active.vibrancy_enabled;
    state->crt_mode_enabled = active.crt_mode_enabled;
    state->ligatures_enabled = active.ligatures_enabled;
    state->hdr_console_enabled = active.hdr_console_enabled;

    // Apply loaded settings to any active windows
    for (auto* tw : state->windows) {
        tw->exposure = state->exposure;
        tw->hue_shift_degrees = state->hue_shift;
        tw->animated_typing = state->animated_typing;
        tw->vibrancy_enabled = state->vibrancy_enabled;
        tw->crt_mode_enabled = state->crt_mode_enabled;
        tw->terminal.set_enable_ligatures(state->ligatures_enabled);
        tw->hdr_console_enabled = state->hdr_console_enabled;
    }
}

// Spawn a new terminal window container
// `preset_override`, when non-null, seeds this one new window from that
// preset's saved settings instead of AppState's shared "active preset"
// fields -- used by the File menu's "New Window/Tab with Preset" submenus so
// picking a preset there only affects the window being created, the same
// way opening a new profile in Terminal.app leaves other windows alone.
// Plain "New Window"/"New Tab" (preset_override == nullptr) keep reading
// from AppState as before.
static TerminalWindow* create_terminal_window(AppState* state, SDL_Window* parent_tab_window, const Preset* preset_override = nullptr) {
    TerminalWindow* tw = new TerminalWindow();

    std::string video_path = state->video_path;
    std::string typeface_path = state->font_path;
    float exposure = state->exposure;
    float hue_shift_degrees = state->hue_shift;
    bool animated_typing = state->animated_typing;
    bool vibrancy_enabled = state->vibrancy_enabled;
    bool crt_mode_enabled = state->crt_mode_enabled;
    bool ligatures_enabled = state->ligatures_enabled;
    bool hdr_console_enabled = state->hdr_console_enabled;

    if (preset_override) {
        video_path = (preset_override->video_path.empty() || preset_override->video_path == "default")
            ? resolve_default_video_path() : preset_override->video_path;
        typeface_path = (preset_override->font_path.empty() || preset_override->font_path == "default")
            ? resolve_default_font_path() : preset_override->font_path;
        exposure = preset_override->exposure;
        hue_shift_degrees = preset_override->hue_shift;
        animated_typing = preset_override->animated_typing;
        vibrancy_enabled = preset_override->vibrancy_enabled;
        crt_mode_enabled = preset_override->crt_mode_enabled;
        ligatures_enabled = preset_override->ligatures_enabled;
        hdr_console_enabled = preset_override->hdr_console_enabled;
    }

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

    tw->vibrancy_enabled = vibrancy_enabled;
    tw->crt_mode_enabled = crt_mode_enabled;
    tw->hdr_console_enabled = hdr_console_enabled;
    tw->terminal.set_enable_ligatures(ligatures_enabled);

    // Create Renderer. "gpu" backend (not the default per-platform driver)
    // so the video/hue-shift/CRT effects can plug custom shaders into this
    // renderer's texture draws via SDL_GPURenderState -- the classic 2D
    // renderer backends don't support that. This part is unconditional and
    // already proven stable (it's what hue-shift/CRT-shader have been
    // running on all along).
    //
    // The linear/extended-range colorspace, on the other hand, is only
    // requested when this window actually needs it (hdr console is on for
    // it): that's what lets a color value go above 1.0 and actually come
    // out brighter than SDR white instead of clamping to it, which is how
    // hdr console makes text glow. Making *every* window request it
    // unconditionally (tried first) made the GPU renderer stall on its
    // first present at startup -- a plain window with nothing to show
    // stayed blank until something else (e.g. opening Settings) forced a
    // redraw -- so it's opt-in per window instead. Toggling hdr console
    // live on an *already-open* window is handled separately, by tearing
    // this renderer down and recreating it -- see
    // recreate_renderer_for_hdr_console().
    if (hdr_console_enabled) {
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, tw->window);
        SDL_SetStringProperty(props, SDL_PROP_RENDERER_CREATE_NAME_STRING, SDL_GPU_RENDERER);
        SDL_SetNumberProperty(props, SDL_PROP_RENDERER_CREATE_OUTPUT_COLORSPACE_NUMBER, SDL_COLORSPACE_SRGB_LINEAR);
        tw->renderer = SDL_CreateRendererWithProperties(props);
        SDL_DestroyProperties(props);
    } else {
        tw->renderer = SDL_CreateRenderer(tw->window, SDL_GPU_RENDERER);
    }

    if (!tw->renderer) {
        std::cerr << "SDL_CreateRenderer Error: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(tw->window);
        delete tw;
        return nullptr;
    }

    SDL_SetRenderVSync(tw->renderer, 1);

    // Best-effort: hue shift/CRT-shader just won't be available (falls
    // back to the plain rectangle overlay) if this fails, no need to fail
    // window creation over it.
    tw->hue_shift.init(tw->renderer);
    tw->hue_shift_degrees = hue_shift_degrees;
    tw->crt_shader.init(tw->renderer, hdr_console_enabled);

#if defined(__APPLE__)
    enable_macos_window_vibrancy(tw->window, tw->vibrancy_enabled);
#endif

    float scale = SDL_GetWindowDisplayScale(tw->window);
    if (scale <= 0.0f) scale = 1.0f;
    state->display_scale = scale;

    if (typeface_path.empty()) {
        typeface_path = "/System/Library/Fonts/SFNSMono.ttf";
    }

    float font_size_px = state->base_font_size * scale;
    if (!tw->font_manager.load_font(tw->renderer, typeface_path, font_size_px, false)) {
        typeface_path = "/System/Library/Fonts/Supplemental/Courier New.ttf";
        tw->font_manager.load_font(tw->renderer, typeface_path, font_size_px, false);
    }

    tw->cell_w = (tw->font_manager.get_cell_width() / scale) + 1.0f;
    tw->cell_h = (tw->font_manager.get_cell_height() / scale) - 0.8f;

    // Settle initial grid scale dimensions
    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(tw->window, &win_w, &win_h);
    float top_offset_pts = tw->vibrancy_enabled ? 32.0f : 34.0f;
    int cols = std::max(40, static_cast<int>((win_w - 2 * state->padding) / tw->cell_w));
    int rows = std::max(10, static_cast<int>((win_h - 2 * state->padding - top_offset_pts) / tw->cell_h));

    tw->terminal.resize(cols, rows);
    tw->terminal.clear_screen();

    // Start Pseudo-Terminal process connection
    if (!tw->pty.spawn(cols, rows)) {
        std::cerr << "Failed to initialize PTY shell context" << std::endl;
    }

    SDL_StartTextInput(tw->window);

    // Setup Video Background Engine
    if (!video_path.empty() && video_path != "None") {
        if (tw->video_engine.open_video(tw->renderer, video_path)) {
            tw->video_engine.start();
            tw->has_video = true;
        }
    }

    tw->animated_typing = animated_typing;
    tw->exposure = exposure;
    tw->fade_state = TerminalWindow::FADE_HOLD_BLACK;
    tw->fade_opacity = 1.0f;
    tw->scroll_accumulator = 0.0f;
    tw->scroll_velocity = 0.0f;
    tw->last_wheel_time = 0;

    // Attach as tab if a parent window is present
    if (parent_tab_window) {
        add_window_as_tab(parent_tab_window, tw->window);
    }

    return tw;
}

// Safely terminate and destroy a terminal window context
static void destroy_terminal_window(TerminalWindow* tw) {
    if (!tw) return;
    // Any running sinkdemo/sinksing thread still holds tw and touches
    // tw->terminal/tw->pty/tw->grid_mutex on its own schedule. Signal it to
    // unwind and join before freeing tw, otherwise it dereferences freed
    // memory on its next iteration.
    SinkDemo::request_abort(tw);
    if (tw->demo_thread.joinable()) {
        tw->demo_thread.join();
    }
    tw->pty.shutdown();
    tw->video_engine.stop();
    tw->video_engine.close_video();
    tw->hue_shift.cleanup(); // must run before destroying the renderer it was created against
    tw->crt_shader.cleanup();
    if (tw->renderer) {
        SDL_DestroyRenderer(tw->renderer);
    }
    if (tw->window) {
        SDL_DestroyWindow(tw->window);
    }
    delete tw;
}

// Tears down `tw`'s renderer and everything built against it (font glyph
// atlas, video texture/decode, hue-shift/CRT-shader GPU shader state --
// all invalid once their owning renderer/GPU device is gone), then
// recreates all of it against a fresh renderer with `want_linear_colorspace`.
// A renderer's output colorspace can't be changed after creation, so this
// is what makes toggling "hdr console" live on an *already-open* window
// possible at all -- see the colorspace comment in create_terminal_window.
//
// The video background has no partial-rebind API (open_video() creates its
// FFmpeg decode context and its SDL_Texture together), so it's closed and
// reopened from the same path/position-0 rather than migrated -- a brief
// restart to frame 0, traded for the toggle actually being live.
static bool recreate_renderer_for_hdr_console(AppState* state, TerminalWindow* tw, bool want_linear_colorspace) {
    bool had_video = tw->has_video;
    std::string video_path_to_reopen = state->video_path;

    // Order matters: everything that owns a texture/shader-state against
    // the current renderer must be torn down *before* that renderer is
    // destroyed, or they're left holding pointers into a dead GPU device.
    tw->video_engine.close_video(); // also stops decode; see close_video()
    tw->has_video = false;
    tw->hue_shift.cleanup();
    tw->crt_shader.cleanup();
    tw->font_manager.cleanup();

    if (tw->renderer) {
        SDL_DestroyRenderer(tw->renderer);
        tw->renderer = nullptr;
    }

    if (want_linear_colorspace) {
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, tw->window);
        SDL_SetStringProperty(props, SDL_PROP_RENDERER_CREATE_NAME_STRING, SDL_GPU_RENDERER);
        SDL_SetNumberProperty(props, SDL_PROP_RENDERER_CREATE_OUTPUT_COLORSPACE_NUMBER, SDL_COLORSPACE_SRGB_LINEAR);
        tw->renderer = SDL_CreateRendererWithProperties(props);
        SDL_DestroyProperties(props);
    } else {
        tw->renderer = SDL_CreateRenderer(tw->window, SDL_GPU_RENDERER);
    }

    if (!tw->renderer) {
        std::cerr << "recreate_renderer_for_hdr_console: SDL_CreateRenderer failed: " << SDL_GetError() << std::endl;
        return false;
    }

    SDL_SetRenderVSync(tw->renderer, 1);
    tw->hue_shift.init(tw->renderer);
    tw->crt_shader.init(tw->renderer, want_linear_colorspace);

    float font_size_px = state->base_font_size * state->display_scale;
    if (!tw->font_manager.load_font(tw->renderer, state->font_path, font_size_px, false)) {
        tw->font_manager.load_font(tw->renderer, "/System/Library/Fonts/Supplemental/Courier New.ttf", font_size_px, false);
    }
    tw->cell_w = (tw->font_manager.get_cell_width() / state->display_scale) + 1.0f;
    tw->cell_h = (tw->font_manager.get_cell_height() / state->display_scale) - 0.8f;

    if (had_video && !video_path_to_reopen.empty() && video_path_to_reopen != "None") {
        if (tw->video_engine.open_video(tw->renderer, video_path_to_reopen)) {
            tw->video_engine.start();
            tw->has_video = true;
            tw->fade_state = TerminalWindow::FADE_HOLD_BLACK;
            tw->fade_opacity = 1.0f;
        }
    }

#if defined(__APPLE__)
    enable_macos_window_vibrancy(tw->window, tw->vibrancy_enabled);
#endif

    return true;
}

// Applies a preset's background/font/exposure/toggles to `state` and every
// live TerminalWindow, mirroring what the individual background/font/toggle
// settings-UI handlers do, but bundled together for a one-shot preset switch.
static void apply_preset_to_state_and_windows(AppState* state, const Preset& p) {
    state->active_preset_name = p.name;
    state->video_path = (p.video_path.empty() || p.video_path == "default")
        ? resolve_default_video_path() : p.video_path;
    state->font_path = (p.font_path.empty() || p.font_path == "default")
        ? resolve_default_font_path() : p.font_path;
    state->exposure = p.exposure;
    state->hue_shift = p.hue_shift;
    state->animated_typing = p.animated_typing;
    state->vibrancy_enabled = p.vibrancy_enabled;
    state->crt_mode_enabled = p.crt_mode_enabled;
    state->ligatures_enabled = p.ligatures_enabled;
    state->hdr_console_enabled = p.hdr_console_enabled;

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

        if (tw->font_manager.load_font(tw->renderer, state->font_path, state->base_font_size * state->display_scale, false)) {
            tw->cell_w = (tw->font_manager.get_cell_width() / state->display_scale) + 1.0f;
            tw->cell_h = (tw->font_manager.get_cell_height() / state->display_scale) - 0.8f;

            int w, h;
            SDL_GetWindowSize(tw->window, &w, &h);
            int new_cols = std::max(40, static_cast<int>((w - 2 * state->padding) / tw->cell_w));
            int new_rows = std::max(10, static_cast<int>((h - 2 * state->padding) / tw->cell_h));
            tw->terminal.resize(new_cols, new_rows);
            tw->pty.resize_pty(new_cols, new_rows);
        }

        tw->exposure = state->exposure;
        tw->hue_shift_degrees = state->hue_shift;
        tw->animated_typing = state->animated_typing;
        tw->vibrancy_enabled = state->vibrancy_enabled;
        tw->crt_mode_enabled = state->crt_mode_enabled;
        tw->terminal.set_enable_ligatures(state->ligatures_enabled);
        if (tw->hdr_console_enabled != state->hdr_console_enabled) {
            tw->hdr_console_enabled = state->hdr_console_enabled;
            // This window's renderer colorspace no longer matches the
            // incoming preset's hdr_console setting -- rebuild it (which
            // also re-opens the video/font this loop just set, but against
            // the correct renderer this time).
            recreate_renderer_for_hdr_console(state, tw, tw->hdr_console_enabled);
        }
#if defined(__APPLE__)
        enable_macos_window_vibrancy(tw->window, state->vibrancy_enabled);
#endif
    }
}

// Pushes the current preset list/selection into the (already-open) settings
// window so its preset row and dropdown reflect what's on disk.
static void refresh_settings_ui_presets(AppState* state) {
    state->settings_ui.set_preset_names(presets::list_names());
    state->settings_ui.set_active_preset(state->active_preset_name);
}

// Re-syncs every settings-window control from AppState, used after a preset
// switch/create/duplicate/delete replaces the live settings wholesale.
static void sync_settings_ui_from_state(AppState* state) {
    state->settings_ui.set_paths(state->video_path, state->font_path);
    state->settings_ui.set_animated_typing(state->animated_typing);
    state->settings_ui.set_exposure(state->exposure);
    state->settings_ui.set_hue_shift(state->hue_shift);
    state->settings_ui.set_vibrancy_enabled(state->vibrancy_enabled);
    state->settings_ui.set_crt_effect_enabled(state->crt_mode_enabled);
    state->settings_ui.set_ligatures_enabled(state->ligatures_enabled);
    state->settings_ui.set_hdr_console_enabled(state->hdr_console_enabled);
    refresh_settings_ui_presets(state);
}

// SDL3 Application initialization entry point
SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[]) {
    SDL_SetAppMetadata("sink", "0.8.0", "com.rainmultimedia.sink");
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
    state->font_path = resolve_default_font_path();

    // Load config if present (may override the defaults above with the
    // active preset's saved background/font/etc)
    load_config(state);

    if (argc > 1) {
        state->video_path = argv[1];
    }

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

static bool delete_selection_in_prompt(TerminalWindow* tw) {
    if (!tw || tw->terminal.is_alt_screen_active() || !tw->terminal.has_selection()) {
        return false;
    }
    int cols = tw->terminal.get_cols();
    int r0 = tw->terminal.get_select_start_row();
    int r1 = tw->terminal.get_select_end_row();
    int c0 = tw->terminal.get_select_start_col();
    int c1 = tw->terminal.get_select_end_col();

    int start_r, start_c, end_r, end_c;
    if ((r0 < r1) || (r0 == r1 && c0 <= c1)) {
        start_r = r0; start_c = c0;
        end_r = r1; end_c = c1;
    } else {
        start_r = r1; start_c = c1;
        end_r = r0; end_c = c0;
    }

    int total_history = static_cast<int>(tw->terminal.get_scrollback_size());
    int cursor_row_active = tw->terminal.get_cursor_row();
    int cursor_row_grid = cursor_row_active + total_history;
    int cursor_col = tw->terminal.get_cursor_col();
    int prompt_boundary = tw->terminal.get_prompt_boundary();
    const auto& row_wrapped = tw->terminal.get_row_wrapped();

    // Trace prompt start row in grid coordinates
    int p_start_row_active = cursor_row_active;
    while (p_start_row_active > 0 && (p_start_row_active - 1) < static_cast<int>(row_wrapped.size()) && row_wrapped[p_start_row_active - 1]) {
        p_start_row_active--;
    }
    int p_start_row_grid = p_start_row_active + total_history;

    // Trace prompt end row in grid coordinates
    int p_end_row_active = cursor_row_active;
    while (p_end_row_active < tw->terminal.get_rows() - 1 && p_end_row_active < static_cast<int>(row_wrapped.size()) && row_wrapped[p_end_row_active]) {
        p_end_row_active++;
    }
    int p_end_row_grid = p_end_row_active + total_history;

    // Check if selection is within the prompt rows
    if (start_r >= p_start_row_grid && end_r <= p_end_row_grid) {
        if (start_r == p_start_row_grid && prompt_boundary != -1 && start_c < prompt_boundary) {
            return false;
        }

        int start_idx = (start_r - p_start_row_grid) * cols + start_c;
        int end_idx = (end_r - p_start_row_grid) * cols + end_c;
        int len = end_idx - start_idx + 1;
        int cursor_idx = (cursor_row_grid - p_start_row_grid) * cols + cursor_col;

        std::string payload;

        // 1. Move cursor to start_idx
        int move_offset = start_idx - cursor_idx;
        if (move_offset > 0) {
            for (int i = 0; i < move_offset; ++i) {
                payload += "\x1b[C"; // Standard Right Arrow
            }
        } else if (move_offset < 0) {
            for (int i = 0; i < -move_offset; ++i) {
                payload += "\x1b[D"; // Standard Left Arrow
            }
        }

        // 2. Send Delete keys forward
        for (int i = 0; i < len; ++i) {
            payload += "\x1b[3~"; // vt100 delete key sequence
        }

        // 3. Move cursor back to target position
        if (cursor_idx > start_idx) {
            int final_target = (cursor_idx >= start_idx + len) ? (cursor_idx - len) : start_idx;
            int return_offset = final_target - start_idx;
            for (int i = 0; i < return_offset; ++i) {
                payload += "\x1b[C"; // Standard Right Arrow
            }
        } else if (cursor_idx < start_idx) {
            int return_offset = start_idx - cursor_idx;
            for (int i = 0; i < return_offset; ++i) {
                payload += "\x1b[D"; // Standard Left Arrow
            }
        }

        tw->pty.write_to_pty(payload.data(), payload.size());
        tw->terminal.clear_selection();
        return true;
    }
    return false;
}

static void perform_cut_action(TerminalWindow* tw) {
    if (!tw || !tw->terminal.has_selection()) return;
    std::string selected_text = tw->terminal.get_selected_text();
    if (!selected_text.empty()) {
        SDL_SetClipboardText(selected_text.c_str());
    }
    bool handled = delete_selection_in_prompt(tw);
    if (!handled) {
        tw->terminal.clear_selection();
    }
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
            state->settings_ui.set_hue_shift(state->active_window ? state->active_window->hue_shift_degrees : 0.0f);
            state->settings_ui.set_hdr_console_enabled(state->active_window ? state->active_window->hdr_console_enabled : false);
            refresh_settings_ui_presets(state);
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
        
        Uint64 now = SDL_GetTicks();
        float delta_sec = (target_tw->last_wheel_time > 0) ? static_cast<float>(now - target_tw->last_wheel_time) / 1000.0f : 0.1f;
        target_tw->last_wheel_time = now;
        
        if ((wheel_y > 0.0f && target_tw->scroll_velocity < 0.0f) || (wheel_y < 0.0f && target_tw->scroll_velocity > 0.0f)) {
            target_tw->scroll_velocity = 0.0f;
        }
        
        float target_v = 0.0f;
        if (delta_sec > 0.001f && delta_sec < 0.2f) {
            target_v = (wheel_y * 3.75f) / delta_sec;
        } else {
            target_v = wheel_y * 10.0f;
        }
        
        target_tw->scroll_velocity = target_tw->scroll_velocity * 0.2f + target_v * 0.8f;
        target_tw->scroll_accumulator += wheel_y * 0.3f;
    } else if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if (event->button.windowID == SDL_GetWindowID(target_tw->window)) {
            target_tw->scroll_velocity = 0.0f;
            if (event->button.button == SDL_BUTTON_LEFT) {
                float mx = event->button.x;
                float my = event->button.y;
                float top_offset_pts = target_tw->vibrancy_enabled ? 32.0f : 34.0f;

                // Clicks land in the title bar strip above the terminal
                // grid (SDL_WINDOW_HIGH_PIXEL_DENSITY + full-size content
                // view means this app's own view -- not a native AppKit
                // title bar -- receives them). Double-clicking there zooms
                // the window, matching the classic macOS title-bar gesture;
                // other click counts up here are just ignored rather than
                // falling through into text selection with a garbage
                // negative row.
                if (my < top_offset_pts) {
                    if (event->button.clicks == 2) {
#if defined(__APPLE__)
                        zoom_macos_window(target_tw->window);
#endif
                    }
                    return SDL_APP_CONTINUE;
                }

                int col = static_cast<int>((mx - state->padding) / target_tw->cell_w);
                int row = static_cast<int>((my - (state->padding + top_offset_pts)) / target_tw->cell_h);

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
            float top_offset_pts = target_tw->vibrancy_enabled ? 32.0f : 34.0f;
            
            int col = static_cast<int>((mx - state->padding) / target_tw->cell_w);
            int row = static_cast<int>((my - (state->padding + top_offset_pts)) / target_tw->cell_h);
            
            if (target_tw->terminal.is_selecting()) {
                target_tw->terminal.update_selection(col, row);
            }
        }
    } else if (event->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        if (event->button.windowID == SDL_GetWindowID(target_tw->window)) {
            if (event->button.button == SDL_BUTTON_LEFT) {
                float mx = event->button.x;
                float my = event->button.y;
                float top_offset_pts = target_tw->vibrancy_enabled ? 32.0f : 34.0f;
                
                int col = static_cast<int>((mx - state->padding) / target_tw->cell_w);
                int row = static_cast<int>((my - (state->padding + top_offset_pts)) / target_tw->cell_h);
                
                int clicks = event->button.clicks;
                if (clicks == 1 && col == target_tw->mouse_down_col && row == target_tw->mouse_down_row) {
                    // Snapping cursor on mouse release
                    if (!target_tw->terminal.is_alt_screen_active()) {
                        int cols = target_tw->terminal.get_cols();
                        int cursor_row = target_tw->terminal.get_cursor_row();
                        int cursor_col = target_tw->terminal.get_cursor_col();
                        const auto& row_wrapped = target_tw->terminal.get_row_wrapped();

                        int p_start_row = cursor_row;
                        while (p_start_row > 0 && (p_start_row - 1) < static_cast<int>(row_wrapped.size()) && row_wrapped[p_start_row - 1]) {
                            p_start_row--;
                        }

                        int p_end_row = cursor_row;
                        while (p_end_row < target_tw->terminal.get_rows() - 1 && p_end_row < static_cast<int>(row_wrapped.size()) && row_wrapped[p_end_row]) {
                            p_end_row++;
                        }

                        if (target_tw->terminal.get_prompt_boundary() == -1) {
                            target_tw->terminal.set_prompt_boundary(cursor_col);
                        }

                        if (row >= p_start_row && row <= p_end_row) {
                            int prompt_boundary = target_tw->terminal.get_prompt_boundary();
                            if (!(row == p_start_row && prompt_boundary != -1 && col < prompt_boundary)) {
                                int cursor_idx = (cursor_row - p_start_row) * cols + cursor_col;
                                int click_idx = (row - p_start_row) * cols + col;
                                int offset = click_idx - cursor_idx;

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
                }
                target_tw->terminal.end_selection();
            }
        }
    } else if (event->type == SDL_EVENT_TEXT_INPUT) {
        if (state->settings_ui.is_open() && SDL_GetKeyboardFocus() == state->settings_ui.get_window()) {
            return SDL_APP_CONTINUE;
        }
        if (target_tw && target_tw->search_drawer_open) {
            target_tw->search_input_text += event->text.text;
            target_tw->terminal.set_search_query(target_tw->search_input_text);
            return SDL_APP_CONTINUE;
        }
        if (state->input_broadcasting) {
            for (auto* tw : state->windows) {
                tw->terminal.clear_selection();
                tw->terminal.reset_scroll();
                tw->scroll_velocity = 0.0f;
                tw->scroll_accumulator = 0.0f;
                tw->pty.write_to_pty(event->text.text, std::strlen(event->text.text));
            }
        } else {
            target_tw->terminal.clear_selection();
            target_tw->terminal.reset_scroll();
            target_tw->scroll_velocity = 0.0f;
            target_tw->scroll_accumulator = 0.0f;
            target_tw->pty.write_to_pty(event->text.text, std::strlen(event->text.text));
        }
    } else if (event->type == SDL_EVENT_KEY_DOWN) {
        SDL_Keycode sym = event->key.key;
        SDL_Keymod mod = event->key.mod;

        // Cmd+F search bar toggle shortcut
        if ((mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL)) && sym == SDLK_F) {
            target_tw->search_drawer_open = !target_tw->search_drawer_open;
            target_tw->terminal.set_search_active(target_tw->search_drawer_open);
            if (target_tw->search_drawer_open) {
                target_tw->terminal.set_search_query(target_tw->search_input_text);
            }
            return SDL_APP_CONTINUE;
        }

        // Handle Search Drawer active keyboard inputs
        if (target_tw && target_tw->search_drawer_open) {
            if (sym == SDLK_ESCAPE) {
                target_tw->search_drawer_open = false;
                target_tw->terminal.set_search_active(false);
                return SDL_APP_CONTINUE;
            } else if (sym == SDLK_BACKSPACE || sym == SDLK_DELETE) {
                if (!target_tw->search_input_text.empty()) {
                    target_tw->search_input_text.pop_back();
                    target_tw->terminal.set_search_query(target_tw->search_input_text);
                }
                return SDL_APP_CONTINUE;
            } else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
                if (mod & SDL_KMOD_SHIFT) {
                    target_tw->terminal.search_prev();
                } else {
                    target_tw->terminal.search_next();
                }
                return SDL_APP_CONTINUE;
            } else if (sym == SDLK_UP) {
                target_tw->terminal.search_prev();
                return SDL_APP_CONTINUE;
            } else if (sym == SDLK_DOWN) {
                target_tw->terminal.search_next();
                return SDL_APP_CONTINUE;
            }
        }

        // Cmd+Comma settings window trigger
        if ((mod & SDL_KMOD_GUI) && sym == SDLK_COMMA) {
            if (!state->settings_ui.is_open()) {
                if (state->settings_ui.open(target_tw->window)) {
                    state->settings_ui.set_paths(state->video_path, state->font_path);
                    state->settings_ui.set_animated_typing(target_tw->animated_typing);
                    state->settings_ui.set_exposure(target_tw->exposure);
                    state->settings_ui.set_hue_shift(target_tw->hue_shift_degrees);
                    state->settings_ui.set_broadcasting(state->input_broadcasting);
                    state->settings_ui.set_vibrancy_enabled(target_tw->vibrancy_enabled);
                    state->settings_ui.set_crt_effect_enabled(target_tw->crt_mode_enabled);
                    state->settings_ui.set_hdr_console_enabled(target_tw->hdr_console_enabled);
                    refresh_settings_ui_presets(state);
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

        // Cmd+N / Cmd+T: New Window / New Tab. Handled directly here rather
        // than via the Cocoa menu's keyEquivalent, since the "New Window"/
        // "New Tab" items now carry a preset submenu -- once an NSMenuItem
        // has a submenu, AppKit stops treating it as an actionable item, so
        // its keyEquivalent is display-only. Both go through the same
        // set_new_window_requested()/set_new_tab_requested() flags the menu
        // items themselves used to set, so they land on the same "with
        // whatever preset is currently active" path as before.
        if ((mod & SDL_KMOD_GUI) && sym == SDLK_N) {
            set_new_window_requested(true);
            return SDL_APP_CONTINUE;
        }
        if ((mod & SDL_KMOD_GUI) && sym == SDLK_T) {
            set_new_tab_requested(true);
            return SDL_APP_CONTINUE;
        }

        // Tab key demo skip trigger
        if (sym == SDLK_TAB && SinkDemo::is_demo_running(target_tw)) {
            SinkDemo::request_skip(target_tw);
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
            if ((mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL)) && sym == SDLK_X) {
                perform_cut_action(tw);
                return SDL_APP_CONTINUE;
            }
            if (!(mod & SDL_KMOD_GUI) && sym != SDLK_BACKSPACE && sym != SDLK_DELETE) {
                tw->terminal.clear_selection();
            }
            tw->terminal.reset_scroll();
            tw->scroll_velocity = 0.0f;
            tw->scroll_accumulator = 0.0f;

            if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
                std::string typed_line = tw->terminal.get_current_line_text();
                
                if (SinkDemo::is_demo_command(typed_line) && !SinkDemo::is_demo_running(tw)) {
                    const char cancel_cmd[] = "\x15\x03";
                    tw->pty.write_to_pty(cancel_cmd, 2);
                    if (tw->demo_thread.joinable()) tw->demo_thread.join();
                    tw->demo_thread = std::thread([tw, state]() {
                        SinkDemo::run_demo(tw, state);
                    });
                } else if (SinkDemo::is_sing_command(typed_line) && !SinkDemo::is_demo_running(tw)) {
                    const char cancel_cmd[] = "\x15\x03";
                    tw->pty.write_to_pty(cancel_cmd, 2);
                    std::string song_name;
                    size_t pos = typed_line.find("sinksing");
                    if (pos != std::string::npos) {
                        song_name = typed_line.substr(pos + 8);
                        size_t first = song_name.find_first_not_of(" \t\r\n");
                        if (first != std::string::npos) {
                            size_t last = song_name.find_last_not_of(" \t\r\n");
                            song_name = song_name.substr(first, (last - first + 1));
                        } else {
                            song_name = "";
                        }
                    }
                    if (tw->demo_thread.joinable()) tw->demo_thread.join();
                    tw->demo_thread = std::thread([tw, song_name]() {
                        SinkDemo::run_sing(tw, song_name);
                    });
                } else if (!SinkDemo::is_demo_command(typed_line) && !SinkDemo::is_sing_command(typed_line)) {
                    const char c = '\r';
                    tw->pty.write_to_pty(&c, 1);
                }
            } else if (sym == SDLK_BACKSPACE || sym == SDLK_DELETE) {
                bool handled = delete_selection_in_prompt(tw);
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
            } else if (mod & SDL_KMOD_CTRL) {
                if (sym >= SDLK_A && sym <= SDLK_Z) {
                    char control_char = static_cast<char>(sym - SDLK_A + 1);
                    tw->pty.write_to_pty(&control_char, 1);
                }
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
                float top_offset_pts = target_tw->vibrancy_enabled ? 32.0f : 34.0f;
                int new_cols = std::max(40, static_cast<int>((w - 2 * state->padding) / target_tw->cell_w));
                int new_rows = std::max(10, static_cast<int>((h - 2 * state->padding - top_offset_pts) / target_tw->cell_h));

                target_tw->pending_cols = new_cols;
                target_tw->pending_rows = new_rows;
                target_tw->has_pending_resize = true;
                target_tw->last_resize_event_time = SDL_GetTicks();
            }
        }
    } else if (event->type == SDL_EVENT_USER) {
        if (event->user.code == 1) { // Media file selected
            char* path = (char*)event->user.data1;
            std::cout << "Settings Event: background changed: " << path << std::endl;

            state->video_path = path;

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
        } else if (event->user.code == 10) { // Switch preset
            char* name = (char*)event->user.data1;
            std::cout << "Settings Event: switched to preset '" << name << "'" << std::endl;
            apply_preset_to_state_and_windows(state, presets::load(name));
            save_config(state);
            sync_settings_ui_from_state(state);
            free(name);
        } else if (event->user.code == 11) { // New preset, seeded with baseline defaults
            char* name = (char*)event->user.data1;
            std::cout << "Settings Event: new preset '" << name << "'" << std::endl;
            Preset p; // Preset{} defaults = the same baseline "pool" ships with
            p.name = presets::unique_name(name);
            presets::save(p);
            apply_preset_to_state_and_windows(state, p);
            save_config(state);
            sync_settings_ui_from_state(state);
            free(name);
        } else if (event->user.code == 12) { // Duplicate active preset
            std::cout << "Settings Event: duplicating preset '" << state->active_preset_name << "'" << std::endl;
            Preset dup = presets::load(state->active_preset_name);
            dup.name = presets::unique_name(state->active_preset_name + " copy");
            presets::save(dup);
            apply_preset_to_state_and_windows(state, dup);
            save_config(state);
            sync_settings_ui_from_state(state);
        } else if (event->user.code == 13) { // Rename active preset
            char* name = (char*)event->user.data1;
            std::cout << "Settings Event: renaming preset '" << state->active_preset_name << "' to '" << name << "'" << std::endl;
            if (presets::rename(state->active_preset_name, name)) {
                state->active_preset_name = name;
                save_config(state); // rewrites the active_preset pointer only; look/feel is unchanged
            }
            sync_settings_ui_from_state(state);
            free(name);
        } else if (event->user.code == 14) { // Delete active preset
            std::cout << "Settings Event: deleting preset '" << state->active_preset_name << "'" << std::endl;
            presets::remove(state->active_preset_name); // no-op if it's "pool"
            apply_preset_to_state_and_windows(state, presets::load("pool"));
            save_config(state);
            sync_settings_ui_from_state(state);
        } else if (event->user.code == 15) { // File menu: New Window with Preset
            char* name = (char*)event->user.data1;
            std::cout << "Menu Event: new window with preset '" << name << "'" << std::endl;
            Preset p = presets::load(name);
            TerminalWindow* tw = create_terminal_window(state, nullptr, &p);
            if (tw) {
                state->windows.push_back(tw);
                state->active_window = tw;
            }
            free(name);
        } else if (event->user.code == 16) { // File menu: New Tab with Preset
            char* name = (char*)event->user.data1;
            std::cout << "Menu Event: new tab with preset '" << name << "'" << std::endl;
            Preset p = presets::load(name);
            SDL_Window* parent_win = state->active_window ? state->active_window->window : nullptr;
            TerminalWindow* tw = create_terminal_window(state, parent_win, &p);
            if (tw) {
                state->windows.push_back(tw);
                state->active_window = tw;
            }
            free(name);
        }
    }

    return SDL_APP_CONTINUE;
}

static void render_crt_effects(SDL_Renderer* renderer, int width, int height, float scale) {
    if (scale <= 0.0f) scale = 1.0f;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // 1. EasyMode Authentic Retro Scanlines (240 TV line density)
    float scanline_pitch = 6.0f * scale; // 6pt pitch = ~240 scanlines across screen
    float scanline_h = 2.2f * scale;     // 2.2pt dark scanline width
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 75); // 30% dark scanline opacity
    for (float y = 0.0f; y < height; y += scanline_pitch) {
        SDL_FRect line_rect = { 0.0f, y, static_cast<float>(width), scanline_h };
        SDL_RenderFillRect(renderer, &line_rect);
    }

    // 2. EasyMode RGB Aperture Grille Triads (7.5pt triad width)
    float stripe_w = 2.5f * scale;
    float triad_w = stripe_w * 3.0f;
    for (float x = 0.0f; x < width; x += triad_w) {
        // Red subpixel column
        SDL_SetRenderDrawColor(renderer, 255, 60, 60, 10);
        SDL_FRect r_rect = { x, 0.0f, stripe_w, static_cast<float>(height) };
        SDL_RenderFillRect(renderer, &r_rect);

        // Green subpixel column
        SDL_SetRenderDrawColor(renderer, 60, 255, 60, 10);
        SDL_FRect g_rect = { x + stripe_w, 0.0f, stripe_w, static_cast<float>(height) };
        SDL_RenderFillRect(renderer, &g_rect);

        // Blue subpixel column
        SDL_SetRenderDrawColor(renderer, 60, 120, 255, 10);
        SDL_FRect b_rect = { x + stripe_w * 2.0f, 0.0f, stripe_w, static_cast<float>(height) };
        SDL_RenderFillRect(renderer, &b_rect);
    }

    // 3. EasyMode Phosphor Luminescence & Gamma Warmth
    SDL_SetRenderDrawColor(renderer, 255, 240, 210, 8); // Soft warm phosphor glow
    SDL_FRect screen_rect = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
    SDL_RenderFillRect(renderer, &screen_rect);
}

static void render_search_drawer(TerminalWindow* tw, int width, int height) {
    if (!tw->search_drawer_open) return;

    SDL_Renderer* r = tw->renderer;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    float panel_w = std::min(450.0f, width - 40.0f);
    float panel_h = 38.0f;
    float panel_x = width - panel_w - 20.0f;
    float panel_y = 12.0f;

    // Background Card
    SDL_FRect bg_rect = { panel_x, panel_y, panel_w, panel_h };
    SDL_SetRenderDrawColor(r, 12, 16, 24, 235);
    SDL_RenderFillRect(r, &bg_rect);

    // Cyan Border
    SDL_SetRenderDrawColor(r, 0, 200, 255, 180);
    SDL_RenderRect(r, &bg_rect);

    // Label & Input text
    std::string text = "Find: " + tw->search_input_text + "_";
    int count = tw->terminal.get_search_match_count();
    int idx = tw->terminal.get_current_search_index();

    std::string match_info;
    if (tw->search_input_text.empty()) {
        match_info = "";
    } else if (count == 0) {
        match_info = "[0 matches]";
    } else {
        match_info = "[" + std::to_string(idx + 1) + "/" + std::to_string(count) + "]";
    }

    float cell_w = tw->font_manager.get_cell_width();
    float text_x = panel_x + 12.0f;
    float text_y = panel_y + 10.0f;

    for (char ch : text) {
        const GlyphInfo* g = tw->font_manager.get_glyph(r, static_cast<char32_t>(ch));
        if (g) {
            SDL_FRect src = g->src_rect;
            SDL_FRect dst = { text_x, text_y, g->src_rect.w, g->src_rect.h };
            SDL_SetTextureColorMod(tw->font_manager.get_atlas_texture(), 255, 255, 255);
            SDL_RenderTexture(r, tw->font_manager.get_atlas_texture(), &src, &dst);
        }
        text_x += cell_w;
    }

    if (!match_info.empty()) {
        float info_x = panel_x + panel_w - (match_info.length() * cell_w) - 14.0f;
        for (char ch : match_info) {
            const GlyphInfo* g = tw->font_manager.get_glyph(r, static_cast<char32_t>(ch));
            if (g) {
                SDL_FRect src = g->src_rect;
                SDL_FRect dst = { info_x, text_y, g->src_rect.w, g->src_rect.h };
                SDL_SetTextureColorMod(tw->font_manager.get_atlas_texture(), 0, 230, 255);
                SDL_RenderTexture(r, tw->font_manager.get_atlas_texture(), &src, &dst);
            }
            info_x += cell_w;
        }
    }
}

SDL_AppResult SDL_AppIterate(void* appstate) {
    AppState* state = static_cast<AppState*>(appstate);
    if (!state) return SDL_APP_FAILURE;

    // Process native macOS menu clipboard actions
    if (state->active_window) {
        TerminalWindow* tw = state->active_window;
        if (get_cut_requested()) {
            perform_cut_action(tw);
        }
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
        if (get_find_requested()) {
            tw->search_drawer_open = !tw->search_drawer_open;
            tw->terminal.set_search_active(tw->search_drawer_open);
            if (tw->search_drawer_open) {
                tw->terminal.set_search_query(tw->search_input_text);
            }
        }
        if (get_crt_mode_requested()) {
            tw->crt_mode_enabled = !tw->crt_mode_enabled;
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

    // Delta time calculation and CPU thread throttling
    Uint64 current_tick = SDL_GetTicks();
    Uint64 elapsed = current_tick - state->last_tick;
    if (elapsed < 16) {
        SDL_Delay(16 - elapsed);
        current_tick = SDL_GetTicks();
        elapsed = current_tick - state->last_tick;
    }
    float dt = elapsed / 1000.0f;
    state->last_tick = current_tick;

    // Sync settings from UI in real-time
    if (state->settings_ui.is_open()) {
        float exp = state->settings_ui.get_exposure();
        if (state->exposure != exp) {
            state->exposure = exp;
            for (auto* tw : state->windows) {
                tw->exposure = exp;
            }
            save_config(state);
        }

        float hue = state->settings_ui.get_hue_shift();
        if (state->hue_shift != hue) {
            state->hue_shift = hue;
            for (auto* tw : state->windows) {
                tw->hue_shift_degrees = hue;
            }
            save_config(state);
        }

        bool vib = state->settings_ui.get_vibrancy_enabled();
        if (state->vibrancy_enabled != vib) {
            state->vibrancy_enabled = vib;
            for (auto* tw : state->windows) {
                tw->vibrancy_enabled = vib;
#if defined(__APPLE__)
                enable_macos_window_vibrancy(tw->window, vib);
#endif
            }
            save_config(state);
        }

        bool crt = state->settings_ui.get_crt_effect_enabled();
        if (state->crt_mode_enabled != crt) {
            state->crt_mode_enabled = crt;
            for (auto* tw : state->windows) {
                tw->crt_mode_enabled = crt;
            }
            save_config(state);
        }

        bool lig = state->settings_ui.get_ligatures_enabled();
        if (state->ligatures_enabled != lig) {
            state->ligatures_enabled = lig;
            for (auto* tw : state->windows) {
                tw->terminal.set_enable_ligatures(lig);
            }
            save_config(state);
        }

        bool hdr_console = state->settings_ui.get_hdr_console_enabled();
        if (state->hdr_console_enabled != hdr_console) {
            state->hdr_console_enabled = hdr_console;
            for (auto* tw : state->windows) {
                tw->hdr_console_enabled = hdr_console;
                recreate_renderer_for_hdr_console(state, tw, hdr_console);
            }
            save_config(state);
        }
    }

    // Iterate all active windows
    for (auto* tw : state->windows) {
        // Apply a debounced resize once the window has been settled at its
        // current size for a short quiet period (see has_pending_resize),
        // so a burst of resize events (a live drag, or an animated
        // window-zoom firing many intermediate frames) doesn't reflow the
        // grid and SIGWINCH the shell on every single one of them.
        if (tw->has_pending_resize && (current_tick - tw->last_resize_event_time) >= 100) {
            if (tw->pending_cols != tw->terminal.get_cols() || tw->pending_rows != tw->terminal.get_rows()) {
                tw->terminal.resize(tw->pending_cols, tw->pending_rows);
                tw->pty.resize_pty(tw->pending_cols, tw->pending_rows);
            }
            tw->has_pending_resize = false;
        }

        tw->terminal.update_timers(dt);

        // Process inertial scrolling physics
        if (std::abs(tw->scroll_velocity) > 0.01f) {
            tw->scroll_accumulator += tw->scroll_velocity * dt;
            float friction = 6.0f;
            tw->scroll_velocity *= std::exp(-friction * dt);
            if (std::abs(tw->scroll_velocity) < 0.05f) {
                tw->scroll_velocity = 0.0f;
            }
        }
        
        if (std::abs(tw->scroll_accumulator) >= 1.0f) {
            int lines = 0;
            if (tw->scroll_accumulator >= 1.0f) {
                lines = static_cast<int>(std::floor(tw->scroll_accumulator));
                tw->scroll_accumulator -= static_cast<float>(lines);
            } else if (tw->scroll_accumulator <= -1.0f) {
                lines = static_cast<int>(std::ceil(tw->scroll_accumulator));
                tw->scroll_accumulator -= static_cast<float>(lines);
            }
            
            if (lines != 0) {
                int current_offset = tw->terminal.get_scroll_offset();
                int max_offset = static_cast<int>(tw->terminal.get_scrollback_size());
                
                if ((lines > 0 && current_offset >= max_offset) || (lines < 0 && current_offset <= 0)) {
                    tw->scroll_velocity = 0.0f;
                    tw->scroll_accumulator = 0.0f;
                } else {
                    tw->terminal.scroll_view(lines);
                    int new_offset = tw->terminal.get_scroll_offset();
                    if (new_offset == 0 || new_offset == max_offset) {
                        tw->scroll_velocity = 0.0f;
                        tw->scroll_accumulator = 0.0f;
                    }
                }
            }
        }
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

        // Process incoming shell data (suppressed while sinkdemo is running for this window)
        std::vector<char> output = tw->pty.read_pending();
        if (!output.empty() && !SinkDemo::is_demo_running(tw)) {
            std::lock_guard<std::mutex> lock(tw->grid_mutex);
            if (tw->animated_typing) {
                // A human at the keyboard produces isolated chunks (a keystroke's
                // echo) tens-to-hundreds of ms apart. A program streaming
                // continuous screen updates (cacademo, htop, vim, ...) produces
                // many chunks back-to-back with almost no gap between them, even
                // when each individual chunk happens to be small -- a single
                // chunk-size check can't tell those apart. Track the gap between
                // consecutive chunks and once several land faster than typing
                // plausibly could, treat the whole burst as program output so
                // the typewriter pacing doesn't make rendering fall behind it.
                Uint64 now = SDL_GetTicks();
                Uint64 gap = now - tw->last_output_chunk_time;
                tw->last_output_chunk_time = now;
                if (gap < 40) {
                    tw->rapid_chunk_streak++;
                } else {
                    tw->rapid_chunk_streak = 0;
                }
                bool fast_turnover = tw->rapid_chunk_streak >= 3;

                if (output.size() > 5 || fast_turnover) {
                    // Large chunk / fast turnover (command or program output): bypass typing effect
                    if (!tw->animation_buffer.empty()) {
                        tw->parser.parse(tw->terminal, tw->animation_buffer.data(), tw->animation_buffer.size());
                        tw->animation_buffer.clear();
                    }
                    tw->parser.parse(tw->terminal, output.data(), output.size());
                } else {
                    // Small, unhurried chunk (user typing): queue for animated typing
                    tw->animation_buffer.insert(tw->animation_buffer.end(), output.begin(), output.end());
                }
            } else {
                tw->parser.parse(tw->terminal, output.data(), output.size());
            }
            tw->terminal.lock_prompt_boundary_if_unset();
        }

        // Process retro animated typing ticks
        if (tw->animated_typing && !tw->animation_buffer.empty()) {
            std::lock_guard<std::mutex> lock(tw->grid_mutex);
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

        int draw_w = 0, draw_h = 0;
        SDL_GetRenderOutputSize(tw->renderer, &draw_w, &draw_h);

        // Ensure full window render viewport
        SDL_SetRenderViewport(tw->renderer, nullptr);

        // If the real CRT shader (scanlines/halation/vignette, see
        // crt_shader.hpp) is available, everything from here through the
        // end of "B." below renders into an offscreen buffer instead of
        // the window, so the shader pass in "C." can react to the actual
        // composited video+text brightness. Falls back to the older
        // rectangle-overlay version (drawn straight onto the window) if
        // the GPU shader isn't available.
        bool use_crt_shader = tw->crt_mode_enabled && tw->crt_shader.is_ready();
        if (use_crt_shader) {
            tw->crt_shader.begin_scene(tw->renderer);
        }

        // Render Active Window Context
        if (tw->vibrancy_enabled && !tw->has_video) {
            SDL_SetRenderDrawColor(tw->renderer, 0, 0, 0, 80);
        } else {
            SDL_SetRenderDrawColor(tw->renderer, 0, 0, 0, 255);
        }
        SDL_RenderClear(tw->renderer);

        // A. Render video background YUV frame if active
        if (tw->has_video) {
            tw->video_engine.update_frame(tw->renderer, dt);
            SDL_Texture* frame_tex = tw->video_engine.get_texture();
            if (frame_tex) {
                tw->hue_shift.draw(tw->renderer, frame_tex, tw->hue_shift_degrees, tw->exposure, tw->crt_mode_enabled);
            }
        }

        // B. Render grid cells. "hdr console" boosts text brightness past
        // 1.0 (true HDR: on an HDR-capable display this comes out brighter
        // than SDR white, not just clamped/clipped to it, since the
        // renderer is always created with the linear/extended colorspace --
        // see the renderer-creation comment above) via the same
        // SDL_SetRenderColorScale mechanism exposure already uses for the
        // video, bracketed around just this text draw so the video
        // background's own luminance is untouched.
        {
            std::lock_guard<std::mutex> lock(tw->grid_mutex);
            tw->terminal.set_enable_ligatures(state->ligatures_enabled);
            float top_pts = tw->vibrancy_enabled ? 32.0f : 34.0f;
            float start_y = (state->padding + top_pts) * state->display_scale;
            float start_x = state->padding * state->display_scale;
            if (tw->hdr_console_enabled) {
                SDL_SetRenderColorScale(tw->renderer, kHdrConsoleTextBoost);
            }
            tw->terminal.render(tw->renderer, tw->font_manager, start_x, start_y, state->display_scale, dt, tw->animated_typing);
            if (tw->hdr_console_enabled) {
                SDL_SetRenderColorScale(tw->renderer, 1.0f);
            }
        }

        // C. Composite with the CRT shader, or fall back to the plain
        // rectangle-overlay version if the shader isn't available.
        if (use_crt_shader) {
            tw->crt_shader.end_scene(tw->renderer);
        } else if (tw->crt_mode_enabled) {
            render_crt_effects(tw->renderer, draw_w, draw_h, state->display_scale);
        }

        // D. Draw Search Bar Drawer UI if open
        if (tw->search_drawer_open) {
            render_search_drawer(tw, draw_w, draw_h);
        }

        // E. Draw black dissolve overlay mask
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
