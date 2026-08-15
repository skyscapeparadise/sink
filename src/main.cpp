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
#include "app_state.hpp"
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

// Persists the live settings into the active preset's [presets.<name>]
// table in sink.toml, and updates the top-level active_preset pointer to
// match. Called after every settings tweak, same as before the config
// format changed.
static void save_config(AppState* state) {
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
    p.scrollback_lines = state->scrollback_lines;
    presets::save(p);
    presets::set_active_preset_name(state->active_preset_name);
}

// Loads the active preset (following sink.toml's active_preset pointer)
// into `state`. Migrates a pre-TOML install once, on first run after
// upgrading, so nothing a user had saved is lost -- see
// presets::migrate_legacy_config_if_needed(). "pool" always ends up present
// either way, so there's always a known-good fallback.
static void load_config(AppState* state) {
    presets::migrate_legacy_config_if_needed();

    std::string requested_preset = presets::get_active_preset_name();

    Preset active;
    if (presets::exists(requested_preset)) {
        active = presets::load(requested_preset);
    } else if (presets::exists("pool")) {
        active = presets::load("pool");
    } else {
        active = Preset{}; // hardcoded baseline: name="pool" with default look
        presets::save(active);
    }

    presets::set_active_preset_name(active.name);
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
    state->scrollback_lines = active.scrollback_lines;

    // Apply loaded settings to any active windows
    for (auto* tw : state->windows) {
        tw->exposure = state->exposure;
        tw->hue_shift_degrees = state->hue_shift;
        tw->animated_typing = state->animated_typing;
        tw->vibrancy_enabled = state->vibrancy_enabled;
        tw->crt_mode_enabled = state->crt_mode_enabled;
        for (Pane* pane : all_panes(tw)) {
            pane->terminal.set_enable_ligatures(state->ligatures_enabled);
            pane->terminal.set_max_scrollback(static_cast<size_t>(state->scrollback_lines));
        }
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
// Top inset (points) above the content area: sink's own custom-painted
// title bar strip, plus -- when this window has more than one tab -- the
// native tab bar that AppKit draws above it without resizing the content
// view (sink uses NSWindowStyleMaskFullSizeContentView, so that resize
// never happens on its own; without this the tab strip just overlaps the
// first row of terminal content). Only ever *adds* to the tuned 32/34pt
// base, so a single-tab window's already pixel-matched layout is untouched.
static float get_top_offset_pts(TerminalWindow* tw) {
    float base = tw->vibrancy_enabled ? 32.0f : 34.0f;
#if defined(__APPLE__)
    float native = get_native_content_top_inset(tw->window);
    if (native > base) return native;
#endif
    return base;
}

static TerminalWindow* create_terminal_window(AppState* state, SDL_Window* parent_tab_window, const Preset* preset_override = nullptr) {
    TerminalWindow* tw = new TerminalWindow();

    // Every window starts as a single-pane split tree
    tw->root = std::make_unique<PaneNode>();
    tw->root->pane = std::make_unique<Pane>();
    tw->focused = tw->root->pane.get();

    std::string video_path = state->video_path;
    std::string typeface_path = state->font_path;
    float exposure = state->exposure;
    float hue_shift_degrees = state->hue_shift;
    bool animated_typing = state->animated_typing;
    bool vibrancy_enabled = state->vibrancy_enabled;
    bool crt_mode_enabled = state->crt_mode_enabled;
    bool ligatures_enabled = state->ligatures_enabled;
    bool hdr_console_enabled = state->hdr_console_enabled;
    int scrollback_lines = state->scrollback_lines;

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
        scrollback_lines = preset_override->scrollback_lines;
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
    tw->fpane().terminal.set_enable_ligatures(ligatures_enabled);
    tw->fpane().terminal.set_max_scrollback(static_cast<size_t>(scrollback_lines));

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
    tw->hue_shift.init(tw->renderer, hdr_console_enabled);
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
    float top_offset_pts = get_top_offset_pts(tw);
    int cols = std::max(40, static_cast<int>((win_w - 2 * state->padding) / tw->cell_w));
    int rows = std::max(10, static_cast<int>((win_h - 2 * state->padding - top_offset_pts) / tw->cell_h));

    tw->fpane().terminal.resize(cols, rows);
    tw->fpane().terminal.clear_screen();

    // Start Pseudo-Terminal process connection
    if (!tw->fpane().pty.spawn(cols, rows)) {
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
    tw->fpane().scroll_accumulator = 0.0f;
    tw->fpane().scroll_velocity = 0.0f;
    tw->fpane().last_wheel_time = 0;

    // Attach as tab if a parent window is present
    if (parent_tab_window) {
        add_window_as_tab(parent_tab_window, tw->window);
    }

    // Assign the root pane its content rect (single pane = whole content
    // area); later splits re-run the layout
    tw->fpane().rect = { 0.0f, top_offset_pts,
                         static_cast<float>(win_w),
                         static_cast<float>(win_h) - top_offset_pts };

    return tw;
}

// Safely terminate and destroy a terminal window context
static void destroy_terminal_window(TerminalWindow* tw) {
    if (!tw) return;
    // Any running sinkdemo/sinksing thread still holds tw and touches
    // tw->fpane().terminal/tw->fpane().pty/tw->fpane().grid_mutex on its own schedule. Signal it to
    // unwind and join before freeing tw, otherwise it dereferences freed
    // memory on its next iteration.
    SinkDemo::request_abort(tw);
    if (tw->demo_thread.joinable()) {
        tw->demo_thread.join();
    }
    for (Pane* pane : all_panes(tw)) {
        pane->pty.shutdown();
    }
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

// ---- Split-pane management -------------------------------------------------

static constexpr float kPaneGutterPts = 1.0f; // divider line thickness

// Recursively assigns pane rects for `node` within `rect` (window points).
// With `apply_grids` set it also applies the resulting grid/PTY geometry to
// each leaf; without it only the rects move -- used mid-divider-drag so the
// expensive (and shell-visible: reflow + SIGWINCH) part runs debounced
// instead of once per mouse-motion event, which corrupts the shell's
// in-flight prompt redraw (the same pathology the window-resize debounce
// exists to avoid).
static void layout_pane_node(AppState* state, TerminalWindow* tw, PaneNode* node, SDL_FRect rect, bool apply_grids) {
    node->rect = rect;
    if (node->is_leaf()) {
        Pane* pane = node->pane.get();
        pane->rect = rect;
        if (!apply_grids) return;
        int cols = std::max(20, static_cast<int>((rect.w - 2 * state->padding) / tw->cell_w));
        int rows = std::max(5, static_cast<int>((rect.h - 2 * state->padding) / tw->cell_h));
        if (cols != pane->terminal.get_cols() || rows != pane->terminal.get_rows()) {
            std::lock_guard<std::mutex> lock(pane->grid_mutex);
            pane->terminal.resize(cols, rows);
            pane->pty.resize_pty(cols, rows);
        }
        return;
    }
    if (node->vertical) {
        float aw = std::floor((rect.w - kPaneGutterPts) * node->ratio);
        layout_pane_node(state, tw, node->a.get(), { rect.x, rect.y, aw, rect.h }, apply_grids);
        layout_pane_node(state, tw, node->b.get(),
                         { rect.x + aw + kPaneGutterPts, rect.y,
                           rect.w - aw - kPaneGutterPts, rect.h }, apply_grids);
    } else {
        float ah = std::floor((rect.h - kPaneGutterPts) * node->ratio);
        layout_pane_node(state, tw, node->a.get(), { rect.x, rect.y, rect.w, ah }, apply_grids);
        layout_pane_node(state, tw, node->b.get(),
                         { rect.x, rect.y + ah + kPaneGutterPts,
                           rect.w, rect.h - ah - kPaneGutterPts }, apply_grids);
    }
}

static void layout_panes(AppState* state, TerminalWindow* tw, bool apply_grids = true) {
    int w = 0, h = 0;
    SDL_GetWindowSize(tw->window, &w, &h);
    float top = get_top_offset_pts(tw);
    layout_pane_node(state, tw, tw->root.get(),
                     { 0.0f, top, static_cast<float>(w), static_cast<float>(h) - top },
                     apply_grids);
}

static Pane* pane_at(TerminalWindow* tw, float x, float y) {
    for (Pane* pane : all_panes(tw)) {
        if (x >= pane->rect.x && x < pane->rect.x + pane->rect.w &&
            y >= pane->rect.y && y < pane->rect.y + pane->rect.h) {
            return pane;
        }
    }
    return tw->focused;
}

static PaneNode* find_leaf(PaneNode* node, Pane* pane) {
    if (!node) return nullptr;
    if (node->is_leaf()) return node->pane.get() == pane ? node : nullptr;
    if (PaneNode* found = find_leaf(node->a.get(), pane)) return found;
    return find_leaf(node->b.get(), pane);
}

static PaneNode* find_parent(PaneNode* node, PaneNode* child) {
    if (!node || node->is_leaf()) return nullptr;
    if (node->a.get() == child || node->b.get() == child) return node;
    if (PaneNode* found = find_parent(node->a.get(), child)) return found;
    return find_parent(node->b.get(), child);
}

// Splits the focused pane in two; the new pane spawns its own shell and
// takes focus (matching iTerm2/Ghostty).
static void split_focused_pane(AppState* state, TerminalWindow* tw, bool vertical) {
    PaneNode* leaf = find_leaf(tw->root.get(), tw->focused);
    if (!leaf) return;

    auto new_pane = std::make_unique<Pane>();
    new_pane->terminal.set_enable_ligatures(state->ligatures_enabled);
    new_pane->terminal.set_max_scrollback(static_cast<size_t>(state->scrollback_lines));

    // Seed the new grid/PTY at roughly half the old pane; layout_panes
    // recomputes the exact geometry right after
    int cols = std::max(20, vertical ? leaf->pane->terminal.get_cols() / 2
                                     : leaf->pane->terminal.get_cols());
    int rows = std::max(5, vertical ? leaf->pane->terminal.get_rows()
                                    : leaf->pane->terminal.get_rows() / 2);
    new_pane->terminal.resize(cols, rows);
    new_pane->terminal.clear_screen();
    if (!new_pane->pty.spawn(cols, rows)) {
        std::cerr << "split: failed to spawn PTY for new pane" << std::endl;
        return;
    }

    leaf->a = std::make_unique<PaneNode>();
    leaf->a->pane = std::move(leaf->pane);
    leaf->b = std::make_unique<PaneNode>();
    leaf->b->pane = std::move(new_pane);
    leaf->vertical = vertical;
    leaf->ratio = 0.5f;

    tw->focused = leaf->b->pane.get();
    layout_panes(state, tw);
}

// Closes the focused pane, promoting its sibling subtree. Returns false if
// this is the window's last pane (caller should close the window instead).
static bool close_focused_pane(AppState* state, TerminalWindow* tw) {
    if (tw->root->is_leaf()) return false;

    PaneNode* leaf = find_leaf(tw->root.get(), tw->focused);
    if (!leaf) return false;
    PaneNode* parent = find_parent(tw->root.get(), leaf);
    if (!parent) return false;

    // A demo streaming into this pane holds pointers into it; unwind first
    if (tw->demo_pane == tw->focused && tw->demo_running.load()) {
        SinkDemo::request_abort(tw);
        if (tw->demo_thread.joinable()) tw->demo_thread.join();
        tw->demo_abort.store(false); // abort is sticky per-launch, not per-window
    }
    if (tw->demo_pane == tw->focused) tw->demo_pane = nullptr;

    tw->focused->pty.shutdown();

    // Promote the sibling subtree into the parent slot
    std::unique_ptr<PaneNode> sibling =
        (parent->a.get() == leaf) ? std::move(parent->b) : std::move(parent->a);
    parent->pane = std::move(sibling->pane);
    parent->vertical = sibling->vertical;
    parent->ratio = sibling->ratio;
    parent->a = std::move(sibling->a);
    parent->b = std::move(sibling->b);

    std::vector<Pane*> remaining = all_panes(tw);
    tw->focused = remaining.empty() ? nullptr : remaining.front();
    tw->dragging_divider = nullptr; // tree just changed under any active drag
    layout_panes(state, tw);
    return true;
}

// Returns the gutter rect of an internal node's divider, inflated by a few
// points on each side so it's actually grabbable at 1pt visual thickness.
static SDL_FRect divider_grab_rect(const PaneNode* node) {
    const float grab = 3.0f;
    if (node->vertical) {
        float aw = std::floor((node->rect.w - kPaneGutterPts) * node->ratio);
        return { node->rect.x + aw - grab, node->rect.y,
                 kPaneGutterPts + 2 * grab, node->rect.h };
    }
    float ah = std::floor((node->rect.h - kPaneGutterPts) * node->ratio);
    return { node->rect.x, node->rect.y + ah - grab,
             node->rect.w, kPaneGutterPts + 2 * grab };
}

// Finds the internal node whose (inflated) divider gutter contains (x, y).
static PaneNode* divider_at(PaneNode* node, float x, float y) {
    if (!node || node->is_leaf()) return nullptr;
    SDL_FRect g = divider_grab_rect(node);
    if (x >= g.x && x < g.x + g.w && y >= g.y && y < g.y + g.h) {
        return node;
    }
    if (PaneNode* found = divider_at(node->a.get(), x, y)) return found;
    return divider_at(node->b.get(), x, y);
}

// Directional focus move: nearest pane center strictly in the requested
// direction from the focused pane's center.
static void focus_pane_directional(TerminalWindow* tw, int dx, int dy) {
    Pane* from = tw->focused;
    float fcx = from->rect.x + from->rect.w / 2.0f;
    float fcy = from->rect.y + from->rect.h / 2.0f;
    Pane* best = nullptr;
    float best_dist = 0.0f;
    for (Pane* pane : all_panes(tw)) {
        if (pane == from) continue;
        float cx = pane->rect.x + pane->rect.w / 2.0f;
        float cy = pane->rect.y + pane->rect.h / 2.0f;
        if ((dx < 0 && cx >= fcx) || (dx > 0 && cx <= fcx) ||
            (dy < 0 && cy >= fcy) || (dy > 0 && cy <= fcy)) {
            continue;
        }
        float dist = (cx - fcx) * (cx - fcx) + (cy - fcy) * (cy - fcy);
        if (!best || dist < best_dist) {
            best = pane;
            best_dist = dist;
        }
    }
    if (best) tw->focused = best;
}

// Draws divider lines in the gutters between panes (in pixels).
static void render_pane_dividers(TerminalWindow* tw, PaneNode* node, float scale) {
    if (!node || node->is_leaf()) return;
    // The gutter sits between child a's far edge and child b's near edge;
    // derive it from the first pane of each subtree along the split axis.
    std::vector<Pane*> a_panes, b_panes;
    collect_panes(node->a.get(), a_panes);
    collect_panes(node->b.get(), b_panes);
    if (!a_panes.empty() && !b_panes.empty()) {
        SDL_FRect gutter;
        if (node->vertical) {
            float gx = b_panes.front()->rect.x - kPaneGutterPts;
            gutter = { gx, a_panes.front()->rect.y, kPaneGutterPts,
                       a_panes.front()->rect.h };
        } else {
            float gy = b_panes.front()->rect.y - kPaneGutterPts;
            gutter = { a_panes.front()->rect.x, gy,
                       a_panes.front()->rect.w, kPaneGutterPts };
        }
        SDL_FRect px = { gutter.x * scale, gutter.y * scale,
                         gutter.w * scale, gutter.h * scale };
        SDL_SetRenderDrawBlendMode(tw->renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(tw->renderer, 255, 255, 255, 48);
        SDL_RenderFillRect(tw->renderer, &px);
    }
    render_pane_dividers(tw, node->a.get(), scale);
    render_pane_dividers(tw, node->b.get(), scale);
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
    tw->hue_shift.init(tw->renderer, want_linear_colorspace);
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
    state->scrollback_lines = p.scrollback_lines;

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

            layout_panes(state, tw);
        }

        tw->exposure = state->exposure;
        tw->hue_shift_degrees = state->hue_shift;
        tw->animated_typing = state->animated_typing;
        tw->vibrancy_enabled = state->vibrancy_enabled;
        tw->crt_mode_enabled = state->crt_mode_enabled;
        for (Pane* pane : all_panes(tw)) {
            pane->terminal.set_enable_ligatures(state->ligatures_enabled);
            pane->terminal.set_max_scrollback(static_cast<size_t>(state->scrollback_lines));
        }
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
    if (!tw || tw->fpane().terminal.is_alt_screen_active() || !tw->fpane().terminal.has_selection()) {
        return false;
    }
    int cols = tw->fpane().terminal.get_cols();
    int r0 = tw->fpane().terminal.get_select_start_row();
    int r1 = tw->fpane().terminal.get_select_end_row();
    int c0 = tw->fpane().terminal.get_select_start_col();
    int c1 = tw->fpane().terminal.get_select_end_col();

    int start_r, start_c, end_r, end_c;
    if ((r0 < r1) || (r0 == r1 && c0 <= c1)) {
        start_r = r0; start_c = c0;
        end_r = r1; end_c = c1;
    } else {
        start_r = r1; start_c = c1;
        end_r = r0; end_c = c0;
    }

    int total_history = static_cast<int>(tw->fpane().terminal.get_scrollback_size());
    int cursor_row_active = tw->fpane().terminal.get_cursor_row();
    int cursor_row_grid = cursor_row_active + total_history;
    int cursor_col = tw->fpane().terminal.get_cursor_col();
    int prompt_boundary = tw->fpane().terminal.get_prompt_boundary();
    const auto& row_wrapped = tw->fpane().terminal.get_row_wrapped();

    // Trace prompt start row in grid coordinates
    int p_start_row_active = cursor_row_active;
    while (p_start_row_active > 0 && (p_start_row_active - 1) < static_cast<int>(row_wrapped.size()) && row_wrapped[p_start_row_active - 1]) {
        p_start_row_active--;
    }
    int p_start_row_grid = p_start_row_active + total_history;

    // Trace prompt end row in grid coordinates
    int p_end_row_active = cursor_row_active;
    while (p_end_row_active < tw->fpane().terminal.get_rows() - 1 && p_end_row_active < static_cast<int>(row_wrapped.size()) && row_wrapped[p_end_row_active]) {
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

        tw->fpane().pty.write_to_pty(payload.data(), payload.size());
        tw->fpane().terminal.clear_selection();
        return true;
    }
    return false;
}

static void perform_cut_action(TerminalWindow* tw) {
    if (!tw || !tw->fpane().terminal.has_selection()) return;
    std::string selected_text = tw->fpane().terminal.get_selected_text();
    if (!selected_text.empty()) {
        SDL_SetClipboardText(selected_text.c_str());
    }
    bool handled = delete_selection_in_prompt(tw);
    if (!handled) {
        tw->fpane().terminal.clear_selection();
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

    // ---- Divider drag-resize ----
    // Grabbing the gutter between panes adjusts that split's ratio live;
    // hovering it shows a resize cursor. Handled before focus-on-click so a
    // divider grab never starts a selection in the pane underneath.
    if (!target_tw->root->is_leaf()) {
        if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
            event->button.button == SDL_BUTTON_LEFT &&
            event->button.windowID == SDL_GetWindowID(target_tw->window)) {
            if (PaneNode* node = divider_at(target_tw->root.get(), event->button.x, event->button.y)) {
                target_tw->dragging_divider = node;
                return SDL_APP_CONTINUE;
            }
        } else if (event->type == SDL_EVENT_MOUSE_MOTION &&
                   event->motion.windowID == SDL_GetWindowID(target_tw->window)) {
            static SDL_Cursor* cursor_default = nullptr;
            static SDL_Cursor* cursor_ew = nullptr;
            static SDL_Cursor* cursor_ns = nullptr;
            if (!cursor_default) {
                cursor_default = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
                cursor_ew = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE);
                cursor_ns = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NS_RESIZE);
            }

            if (PaneNode* node = target_tw->dragging_divider) {
                // Ratio from the mouse position within the node's rect,
                // clamped so neither side collapses below usability
                float r;
                if (node->vertical) {
                    r = (event->motion.x - node->rect.x) / std::max(1.0f, node->rect.w - kPaneGutterPts);
                } else {
                    r = (event->motion.y - node->rect.y) / std::max(1.0f, node->rect.h - kPaneGutterPts);
                }
                node->ratio = std::clamp(r, 0.1f, 0.9f);
                // Rects (divider position, clipping) track the mouse live;
                // the grid reflow + SIGWINCH are deferred to mouse-up, not
                // debounced. A debounce still fires mid-drag whenever the
                // gesture pauses for >100ms (people pause while dragging),
                // and each such pause was sending the shell another SIGWINCH
                // before the previous prompt redraw had settled -- the more
                // hesitant the drag, the more corrupted/duplicated fragments
                // piled up. Deferring unconditionally to release guarantees
                // exactly one reflow per drag, no matter how it's paced.
                layout_panes(state, target_tw, false);
                return SDL_APP_CONTINUE;
            }

            PaneNode* hover = divider_at(target_tw->root.get(), event->motion.x, event->motion.y);
            SDL_SetCursor(hover ? (hover->vertical ? cursor_ew : cursor_ns) : cursor_default);
            if (hover) return SDL_APP_CONTINUE;
        } else if (event->type == SDL_EVENT_MOUSE_BUTTON_UP &&
                   event->button.button == SDL_BUTTON_LEFT &&
                   target_tw->dragging_divider) {
            target_tw->dragging_divider = nullptr;
            // Drag finished: apply the final geometry right away (one
            // reflow + one SIGWINCH) instead of waiting out the debounce
            layout_panes(state, target_tw);
            target_tw->has_pending_resize = false;
            return SDL_APP_CONTINUE;
        }
    }

    // Clicking anywhere inside a pane focuses it before any further mouse
    // routing (reports, selection) resolves against the focused pane
    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event->button.windowID == SDL_GetWindowID(target_tw->window)) {
        float top_pts = get_top_offset_pts(target_tw);
        if (event->button.y >= top_pts) {
            target_tw->focused = pane_at(target_tw, event->button.x, event->button.y);
        }
    }

    // ---- Mouse reporting (DECSET 9/1000/1002/1003, SGR 1006 encoding) ----
    // When the running app has requested mouse events (vim, htop, tmux, ...),
    // clicks, drags and wheel ticks inside the grid are encoded and written
    // to the PTY instead of driving sink's native selection. Holding Shift
    // bypasses reporting so native selection/clipboard stay reachable, the
    // same escape hatch xterm and every GPU terminal provide.
    {
        int mouse_mode = target_tw->fpane().terminal.get_mouse_mode();
        // Wheel events are handled by the unified hovered-pane wheel block
        // below, not here
        bool is_mouse_event = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                              event->type == SDL_EVENT_MOUSE_BUTTON_UP ||
                              event->type == SDL_EVENT_MOUSE_MOTION;
        if (mouse_mode != 0 && is_mouse_event &&
            !(SDL_GetModState() & SDL_KMOD_SHIFT) &&
            !(state->settings_ui.is_open() && SDL_GetKeyboardFocus() == state->settings_ui.get_window())) {

            float top_offset_pts = get_top_offset_pts(target_tw);
            auto cell_of = [&](float mx, float my, int& col, int& row) {
                const SDL_FRect& pr = target_tw->fpane().rect;
                col = std::clamp(static_cast<int>((mx - pr.x - state->padding) / target_tw->cell_w),
                                 0, target_tw->fpane().terminal.get_cols() - 1);
                row = std::clamp(static_cast<int>((my - pr.y - state->padding) / target_tw->cell_h),
                                 0, target_tw->fpane().terminal.get_rows() - 1);
            };
            auto send_report = [&](int button_code, int col, int row, bool release) {
                char buf[40];
                int len;
                if (target_tw->fpane().terminal.is_mouse_sgr()) {
                    len = std::snprintf(buf, sizeof(buf), "\x1b[<%d;%d;%d%c",
                                        button_code, col + 1, row + 1, release ? 'm' : 'M');
                } else {
                    // Legacy X10 byte encoding: coordinates saturate at 223
                    if (release) button_code = 3;
                    len = std::snprintf(buf, sizeof(buf), "\x1b[M%c%c%c",
                                        static_cast<char>(32 + button_code),
                                        static_cast<char>(32 + std::min(col + 1, 223)),
                                        static_cast<char>(32 + std::min(row + 1, 223)));
                }
                if (len > 0) target_tw->fpane().pty.write_to_pty(buf, static_cast<size_t>(len));
            };
            SDL_Keymod mods = SDL_GetModState();
            // X10 mode (9) is press-only and predates modifier reporting
            int mod_bits = (mouse_mode == 9) ? 0
                : ((mods & SDL_KMOD_ALT) ? 8 : 0) | ((mods & SDL_KMOD_CTRL) ? 16 : 0);
            auto sdl_button_code = [](Uint8 b) {
                return b == SDL_BUTTON_MIDDLE ? 1 : (b == SDL_BUTTON_RIGHT ? 2 : 0);
            };

            if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                event->button.windowID == SDL_GetWindowID(target_tw->window)) {
                if (event->button.y >= top_offset_pts) { // title bar clicks stay native
                    int col, row;
                    cell_of(event->button.x, event->button.y, col, row);
                    send_report(sdl_button_code(event->button.button) + mod_bits, col, row, false);
                    target_tw->fpane().last_mouse_report_col = col;
                    target_tw->fpane().last_mouse_report_row = row;
                    return SDL_APP_CONTINUE;
                }
            } else if (event->type == SDL_EVENT_MOUSE_BUTTON_UP &&
                       event->button.windowID == SDL_GetWindowID(target_tw->window)) {
                if (mouse_mode != 9 && event->button.y >= top_offset_pts) {
                    int col, row;
                    cell_of(event->button.x, event->button.y, col, row);
                    send_report(sdl_button_code(event->button.button) + mod_bits, col, row, true);
                    return SDL_APP_CONTINUE;
                }
            } else if (event->type == SDL_EVENT_MOUSE_MOTION &&
                       event->motion.windowID == SDL_GetWindowID(target_tw->window)) {
                Uint32 held = event->motion.state;
                bool report_motion = (mouse_mode == 1003) ||
                                     (mouse_mode == 1002 && held != 0);
                if (report_motion && event->motion.y >= top_offset_pts) {
                    int col, row;
                    cell_of(event->motion.x, event->motion.y, col, row);
                    if (col != target_tw->fpane().last_mouse_report_col ||
                        row != target_tw->fpane().last_mouse_report_row) {
                        int base = 3; // "no button" for pure hover in 1003
                        if (held & SDL_BUTTON_LMASK)      base = 0;
                        else if (held & SDL_BUTTON_MMASK) base = 1;
                        else if (held & SDL_BUTTON_RMASK) base = 2;
                        send_report(base + 32 + mod_bits, col, row, false);
                        target_tw->fpane().last_mouse_report_col = col;
                        target_tw->fpane().last_mouse_report_row = row;
                    }
                }
                // Motion never falls through to native drag-selection while
                // reporting is active
                if (mouse_mode == 1002 || mouse_mode == 1003) return SDL_APP_CONTINUE;
            }
        }
    }

    // Alternate scroll: a pager or editor on the alt screen without mouse
    // reporting still expects the wheel to work -- every macOS terminal
    // translates wheel ticks into arrow keys there, so sink does too.
    // (Scrollback is invisible on the alt screen anyway; inertial-scrolling
    // it would just drag stale shell history over the app's UI.)
    // ---- Unified wheel routing: every wheel behavior (mouse reports,
    // alternate scroll, inertial scrollback) targets the pane under the
    // cursor, matching iTerm2 -- hovering a pane is enough to scroll it.
    if (event->type == SDL_EVENT_MOUSE_WHEEL &&
        event->wheel.windowID == SDL_GetWindowID(target_tw->window)) {
        if (state->settings_ui.is_open() && SDL_GetKeyboardFocus() == state->settings_ui.get_window()) {
            return SDL_APP_CONTINUE;
        }
        Pane& hp = *pane_at(target_tw, event->wheel.mouse_x, event->wheel.mouse_y);
        float top_pts = get_top_offset_pts(target_tw);
        // Positive wy = view up (wheel-up button 64 / arrow up)
        float wy = -event->wheel.y;
        if (event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) wy = -wy;

        int hp_mouse_mode = hp.terminal.get_mouse_mode();
        bool shift_bypass = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;

        if (hp_mouse_mode != 0 && hp_mouse_mode != 9 && !shift_bypass) {
            // The app in the hovered pane asked for wheel events
            if (wy != 0.0f && event->wheel.mouse_y >= top_pts) {
                SDL_Keymod mods = SDL_GetModState();
                int mod_bits = ((mods & SDL_KMOD_ALT) ? 8 : 0) | ((mods & SDL_KMOD_CTRL) ? 16 : 0);
                int button_code = (wy > 0.0f ? 64 : 65) + mod_bits;
                int col = std::clamp(static_cast<int>((event->wheel.mouse_x - hp.rect.x - state->padding) / target_tw->cell_w),
                                     0, hp.terminal.get_cols() - 1);
                int row = std::clamp(static_cast<int>((event->wheel.mouse_y - hp.rect.y - state->padding) / target_tw->cell_h),
                                     0, hp.terminal.get_rows() - 1);
                char buf[40];
                int len;
                if (hp.terminal.is_mouse_sgr()) {
                    len = std::snprintf(buf, sizeof(buf), "\x1b[<%d;%d;%dM", button_code, col + 1, row + 1);
                } else {
                    len = std::snprintf(buf, sizeof(buf), "\x1b[M%c%c%c",
                                        static_cast<char>(32 + button_code),
                                        static_cast<char>(32 + std::min(col + 1, 223)),
                                        static_cast<char>(32 + std::min(row + 1, 223)));
                }
                if (len > 0) hp.pty.write_to_pty(buf, static_cast<size_t>(len));
            }
            return SDL_APP_CONTINUE;
        }

        if (hp.terminal.is_alt_screen_active() && hp_mouse_mode == 0 &&
            hp.terminal.is_alternate_scroll()) {
            // Pager/editor without mouse mode: wheel becomes arrow keys
            hp.alt_scroll_accum += wy * 3.0f; // ~3 lines per wheel notch
            int lines = static_cast<int>(hp.alt_scroll_accum);
            if (lines != 0) {
                hp.alt_scroll_accum -= static_cast<float>(lines);
                bool app_keys = hp.terminal.is_app_cursor_keys();
                const char* seq = (lines > 0) ? (app_keys ? "\x1bOA" : "\x1b[A")
                                              : (app_keys ? "\x1bOB" : "\x1b[B");
                for (int n = std::abs(lines); n > 0; --n) {
                    hp.pty.write_to_pty(seq, 3);
                }
            }
            return SDL_APP_CONTINUE;
        }

        // Inertial scrollback on the hovered pane
        Uint64 now = SDL_GetTicks();
        float delta_sec = (hp.last_wheel_time > 0) ? static_cast<float>(now - hp.last_wheel_time) / 1000.0f : 0.1f;
        hp.last_wheel_time = now;

        if ((wy > 0.0f && hp.scroll_velocity < 0.0f) || (wy < 0.0f && hp.scroll_velocity > 0.0f)) {
            hp.scroll_velocity = 0.0f;
        }

        float target_v = 0.0f;
        if (delta_sec > 0.001f && delta_sec < 0.2f) {
            target_v = (wy * 3.75f) / delta_sec;
        } else {
            target_v = wy * 10.0f;
        }

        hp.scroll_velocity = hp.scroll_velocity * 0.2f + target_v * 0.8f;
        hp.scroll_accumulator += wy * 0.3f;
        return SDL_APP_CONTINUE;
    }

    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if (event->button.windowID == SDL_GetWindowID(target_tw->window)) {
            target_tw->fpane().scroll_velocity = 0.0f;
            if (event->button.button == SDL_BUTTON_LEFT) {
                float mx = event->button.x;
                float my = event->button.y;
                float top_offset_pts = get_top_offset_pts(target_tw);

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

                int col = static_cast<int>((mx - target_tw->fpane().rect.x - state->padding) / target_tw->cell_w);
                int row = static_cast<int>((my - target_tw->fpane().rect.y - state->padding) / target_tw->cell_h);

                // Cmd+Click opens an OSC 8 hyperlink instead of starting a
                // selection -- the same modifier-gated affordance VS Code,
                // iTerm2, and Kitty all use so a plain click still just
                // places the cursor/selects.
                if ((SDL_GetModState() & SDL_KMOD_GUI) && event->button.clicks == 1) {
                    uint32_t link_id = target_tw->fpane().terminal.get_cell_at(col, row).hyperlink_id;
                    if (link_id != 0) {
                        const std::string& uri = target_tw->fpane().terminal.get_hyperlink_uri(link_id);
#if defined(__APPLE__)
                        open_url_if_safe(uri.c_str());
#endif
                        return SDL_APP_CONTINUE;
                    }
                }

                int clicks = event->button.clicks;
                if (clicks == 1) {
                    target_tw->fpane().mouse_down_col = col;
                    target_tw->fpane().mouse_down_row = row;
                    target_tw->fpane().terminal.start_selection(col, row);
                } else if (clicks == 2) {
                    // Double click: select word
                    target_tw->fpane().terminal.select_word_at(col, row);
                    std::string selected_text = target_tw->fpane().terminal.get_selected_text();
                    if (!selected_text.empty()) {
                        SDL_SetClipboardText(selected_text.c_str());
                    }
                } else if (clicks == 3) {
                    // Triple click: select line
                    target_tw->fpane().terminal.select_line_at(row);
                    std::string selected_text = target_tw->fpane().terminal.get_selected_text();
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
            float top_offset_pts = get_top_offset_pts(target_tw);
            
            int col = static_cast<int>((mx - target_tw->fpane().rect.x - state->padding) / target_tw->cell_w);
            int row = static_cast<int>((my - target_tw->fpane().rect.y - state->padding) / target_tw->cell_h);
            
            if (target_tw->fpane().terminal.is_selecting()) {
                target_tw->fpane().terminal.update_selection(col, row);
            }
        }
    } else if (event->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        if (event->button.windowID == SDL_GetWindowID(target_tw->window)) {
            if (event->button.button == SDL_BUTTON_LEFT) {
                float mx = event->button.x;
                float my = event->button.y;
                float top_offset_pts = get_top_offset_pts(target_tw);
                
                int col = static_cast<int>((mx - target_tw->fpane().rect.x - state->padding) / target_tw->cell_w);
                int row = static_cast<int>((my - target_tw->fpane().rect.y - state->padding) / target_tw->cell_h);
                
                int clicks = event->button.clicks;
                if (clicks == 1 && col == target_tw->fpane().mouse_down_col && row == target_tw->fpane().mouse_down_row) {
                    // Snapping cursor on mouse release
                    if (!target_tw->fpane().terminal.is_alt_screen_active()) {
                        int cols = target_tw->fpane().terminal.get_cols();
                        int cursor_row = target_tw->fpane().terminal.get_cursor_row();
                        int cursor_col = target_tw->fpane().terminal.get_cursor_col();
                        const auto& row_wrapped = target_tw->fpane().terminal.get_row_wrapped();

                        int p_start_row = cursor_row;
                        while (p_start_row > 0 && (p_start_row - 1) < static_cast<int>(row_wrapped.size()) && row_wrapped[p_start_row - 1]) {
                            p_start_row--;
                        }

                        int p_end_row = cursor_row;
                        while (p_end_row < target_tw->fpane().terminal.get_rows() - 1 && p_end_row < static_cast<int>(row_wrapped.size()) && row_wrapped[p_end_row]) {
                            p_end_row++;
                        }

                        if (target_tw->fpane().terminal.get_prompt_boundary() == -1) {
                            target_tw->fpane().terminal.set_prompt_boundary(cursor_col);
                        }

                        if (row >= p_start_row && row <= p_end_row) {
                            int prompt_boundary = target_tw->fpane().terminal.get_prompt_boundary();
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
                                    target_tw->fpane().pty.write_to_pty(move_payload.data(), move_payload.size());
                                }
                                target_tw->fpane().terminal.clear_selection();
                            }
                        }
                    }
                }
                target_tw->fpane().terminal.end_selection();
            }
        }
    } else if (event->type == SDL_EVENT_TEXT_INPUT) {
        if (state->settings_ui.is_open() && SDL_GetKeyboardFocus() == state->settings_ui.get_window()) {
            return SDL_APP_CONTINUE;
        }
        if (target_tw && target_tw->search_drawer_open) {
            target_tw->search_input_text += event->text.text;
            target_tw->fpane().terminal.set_search_query(target_tw->search_input_text);
            return SDL_APP_CONTINUE;
        }
        if (state->input_broadcasting) {
            for (auto* tw : state->windows) {
                tw->fpane().terminal.clear_selection();
                tw->fpane().terminal.reset_scroll();
                tw->fpane().scroll_velocity = 0.0f;
                tw->fpane().scroll_accumulator = 0.0f;
                tw->fpane().pty.write_to_pty(event->text.text, std::strlen(event->text.text));
            }
        } else {
            target_tw->fpane().terminal.clear_selection();
            target_tw->fpane().terminal.reset_scroll();
            target_tw->fpane().scroll_velocity = 0.0f;
            target_tw->fpane().scroll_accumulator = 0.0f;
            target_tw->fpane().pty.write_to_pty(event->text.text, std::strlen(event->text.text));
        }
    } else if (event->type == SDL_EVENT_KEY_DOWN) {
        SDL_Keycode sym = event->key.key;
        SDL_Keymod mod = event->key.mod;

        // Cmd+F search bar toggle shortcut
        if ((mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL)) && sym == SDLK_F) {
            target_tw->search_drawer_open = !target_tw->search_drawer_open;
            target_tw->fpane().terminal.set_search_active(target_tw->search_drawer_open);
            if (target_tw->search_drawer_open) {
                target_tw->fpane().terminal.set_search_query(target_tw->search_input_text);
            }
            return SDL_APP_CONTINUE;
        }

        // Handle Search Drawer active keyboard inputs
        if (target_tw && target_tw->search_drawer_open) {
            if (sym == SDLK_ESCAPE) {
                target_tw->search_drawer_open = false;
                target_tw->fpane().terminal.set_search_active(false);
                return SDL_APP_CONTINUE;
            } else if (sym == SDLK_BACKSPACE || sym == SDLK_DELETE) {
                if (!target_tw->search_input_text.empty()) {
                    target_tw->search_input_text.pop_back();
                    target_tw->fpane().terminal.set_search_query(target_tw->search_input_text);
                }
                return SDL_APP_CONTINUE;
            } else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
                if (mod & SDL_KMOD_SHIFT) {
                    target_tw->fpane().terminal.search_prev();
                } else {
                    target_tw->fpane().terminal.search_next();
                }
                return SDL_APP_CONTINUE;
            } else if (sym == SDLK_UP) {
                target_tw->fpane().terminal.search_prev();
                return SDL_APP_CONTINUE;
            } else if (sym == SDLK_DOWN) {
                target_tw->fpane().terminal.search_next();
                return SDL_APP_CONTINUE;
            }
        }

        // Cmd+Up / Cmd+Down: jump between OSC 133 shell prompts (needs shell
        // integration emitting 133;A marks; without them these do nothing)
        if ((mod & SDL_KMOD_GUI) && (sym == SDLK_UP || sym == SDLK_DOWN)) {
            std::lock_guard<std::mutex> lock(target_tw->fpane().grid_mutex);
            if (sym == SDLK_UP) target_tw->fpane().terminal.scroll_to_prev_prompt();
            else                target_tw->fpane().terminal.scroll_to_next_prompt();
            return SDL_APP_CONTINUE;
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

        // Split-pane commands (Cmd+D, Cmd+Shift+D, Cmd+Shift+W, Cmd+Opt+arrows)
        // are handled exclusively via the Shell menu's key equivalents +
        // request-flag polling below -- same pattern as Cut/Copy/Paste/Find/
        // Close Window, none of which have an SDL-side handler either.
        // AppKit consumes the keystroke for its own menu dispatch, so an SDL
        // handler here would double-fire alongside it (this used to be a bug:
        // Cmd+D created two panes, one from each path).

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
            if (tw->fpane().terminal.get_prompt_boundary() == -1) {
                tw->fpane().terminal.set_prompt_boundary(tw->fpane().terminal.get_cursor_col());
            }
            if ((mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL)) && sym == SDLK_X) {
                perform_cut_action(tw);
                return SDL_APP_CONTINUE;
            }
            if (!(mod & SDL_KMOD_GUI) && sym != SDLK_BACKSPACE && sym != SDLK_DELETE) {
                tw->fpane().terminal.clear_selection();
            }
            tw->fpane().terminal.reset_scroll();
            tw->fpane().scroll_velocity = 0.0f;
            tw->fpane().scroll_accumulator = 0.0f;

            if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
                std::string typed_line = tw->fpane().terminal.get_current_line_text();
                
                if (SinkDemo::is_demo_command(typed_line) && !SinkDemo::is_demo_running(tw)) {
                    const char cancel_cmd[] = "\x15\x03";
                    tw->fpane().pty.write_to_pty(cancel_cmd, 2);
                    if (tw->demo_thread.joinable()) tw->demo_thread.join();
                    tw->demo_pane = tw->focused;
                    tw->demo_thread = std::thread([tw, state]() {
                        SinkDemo::run_demo(tw, state);
                    });
                } else if (SinkDemo::is_sing_command(typed_line) && !SinkDemo::is_demo_running(tw)) {
                    const char cancel_cmd[] = "\x15\x03";
                    tw->fpane().pty.write_to_pty(cancel_cmd, 2);
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
                    tw->demo_pane = tw->focused;
                    tw->demo_thread = std::thread([tw, song_name]() {
                        SinkDemo::run_sing(tw, song_name);
                    });
                } else if (!SinkDemo::is_demo_command(typed_line) && !SinkDemo::is_sing_command(typed_line)) {
                    const char c = '\r';
                    tw->fpane().pty.write_to_pty(&c, 1);
                }
            } else if (sym == SDLK_BACKSPACE || sym == SDLK_DELETE) {
                bool handled = delete_selection_in_prompt(tw);
                if (!handled) {
                    if (sym == SDLK_DELETE) {
                        tw->fpane().pty.write_to_pty("\x1b[3~", 4); // Standard vt100 delete key sequence
                    } else {
                        const char c = '\x7f';
                        tw->fpane().pty.write_to_pty(&c, 1);
                    }
                }
            } else if (sym == SDLK_TAB) {
                const char c = '\t';
                tw->fpane().pty.write_to_pty(&c, 1);
            } else if (sym == SDLK_ESCAPE) {
                const char c = '\x1b';
                tw->fpane().pty.write_to_pty(&c, 1);
            } else if (sym == SDLK_UP || sym == SDLK_DOWN ||
                       sym == SDLK_RIGHT || sym == SDLK_LEFT) {
                // DECCKM: full-screen apps ask for the SS3 (ESC O x) form
                bool app_keys = tw->fpane().terminal.is_app_cursor_keys();
                char final_byte = (sym == SDLK_UP)    ? 'A'
                                : (sym == SDLK_DOWN)  ? 'B'
                                : (sym == SDLK_RIGHT) ? 'C' : 'D';
                char seq[3] = { '\x1b', app_keys ? 'O' : '[', final_byte };
                tw->fpane().pty.write_to_pty(seq, 3);
            } else if (mod & SDL_KMOD_CTRL) {
                if (sym >= SDLK_A && sym <= SDLK_Z) {
                    char control_char = static_cast<char>(sym - SDLK_A + 1);
                    tw->fpane().pty.write_to_pty(&control_char, 1);
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
                target_tw->pending_w = w;
                target_tw->pending_h = h;
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

                    layout_panes(state, tw);
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
                    if (!tw->fpane().animation_buffer.empty()) {
                        tw->fpane().parser.parse(tw->fpane().terminal, tw->fpane().animation_buffer.data(), tw->fpane().animation_buffer.size());
                        tw->fpane().animation_buffer.clear();
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
    int count = tw->fpane().terminal.get_search_match_count();
    int idx = tw->fpane().terminal.get_current_search_index();

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
            std::string selected_text = tw->fpane().terminal.get_selected_text();
            if (!selected_text.empty()) {
                SDL_SetClipboardText(selected_text.c_str());
            }
        }
        if (get_paste_requested()) {
            if (SDL_HasClipboardText()) {
                char* text = SDL_GetClipboardText();
                if (text) {
                    if (tw->fpane().terminal.is_bracketed_paste_active()) {
                        // Wrap in bracketed-paste markers so the shell treats the
                        // content as literal text instead of executing embedded newlines.
                        tw->fpane().pty.write_to_pty("\x1b[200~", 6);
                        tw->fpane().pty.write_to_pty(text, strlen(text));
                        tw->fpane().pty.write_to_pty("\x1b[201~", 6);
                    } else {
                        tw->fpane().pty.write_to_pty(text, strlen(text));
                    }
                    SDL_free(text);
                }
            }
        }
        if (get_select_all_requested()) {
            tw->fpane().terminal.select_all();
        }
        if (get_print_requested()) {
            std::string print_text = tw->fpane().terminal.get_selected_text();
            if (print_text.empty()) {
                print_text = tw->fpane().terminal.get_all_text();
            }
            trigger_print_dialog(print_text.c_str());
        }
        if (get_find_requested()) {
            tw->search_drawer_open = !tw->search_drawer_open;
            tw->fpane().terminal.set_search_active(tw->search_drawer_open);
            if (tw->search_drawer_open) {
                tw->fpane().terminal.set_search_query(tw->search_input_text);
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
    // Split-pane menu actions (Shell menu). The menu's key equivalents
    // consume Cmd+D & co. before SDL sees them, so this is the live code
    // path for those shortcuts; the SDL key handlers remain as fallback for
    // builds without the native menu.
    if (state->active_window) {
        if (get_split_pane_right_requested()) {
            split_focused_pane(state, state->active_window, true);
        }
        if (get_split_pane_down_requested()) {
            split_focused_pane(state, state->active_window, false);
        }
        if (get_close_pane_requested()) {
            // Closing the last pane closes the window (iTerm2 semantics);
            // the close-window flag is polled right below this block
            if (!close_focused_pane(state, state->active_window)) {
                set_close_window_requested(true);
            }
        }
        if (int dir = get_pane_focus_requested()) {
            int dx = (dir == 1) ? -1 : (dir == 2) ? 1 : 0;
            int dy = (dir == 3) ? -1 : (dir == 4) ? 1 : 0;
            focus_pane_directional(state->active_window, dx, dy);
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
                for (Pane* pane : all_panes(tw)) {
                    pane->terminal.set_enable_ligatures(lig);
                }
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
            // layout_panes no-ops any pane whose cols/rows come out unchanged
            layout_panes(state, tw);
            tw->has_pending_resize = false;
        }

        // A native tab bar appearing/disappearing changes the usable content
        // height without the window's own frame changing size, so it never
        // fires SDL_EVENT_WINDOW_RESIZED -- poll for it here instead. Rare
        // and discrete (only on tab add/remove), so no debounce needed.
        float current_top_offset = get_top_offset_pts(tw);
        if (current_top_offset != tw->last_top_offset_pts) {
            tw->last_top_offset_pts = current_top_offset;
            if (!tw->has_pending_resize) { // avoid fighting an in-flight debounce
                layout_panes(state, tw);
            }
        }

        for (Pane* pane : all_panes(tw)) {
            pane->terminal.update_timers(dt);

            // Process inertial scrolling physics
            if (std::abs(pane->scroll_velocity) > 0.01f) {
                pane->scroll_accumulator += pane->scroll_velocity * dt;
                float friction = 6.0f;
                pane->scroll_velocity *= std::exp(-friction * dt);
                if (std::abs(pane->scroll_velocity) < 0.05f) {
                    pane->scroll_velocity = 0.0f;
                }
            }

            if (std::abs(pane->scroll_accumulator) >= 1.0f) {
                int lines = 0;
                if (pane->scroll_accumulator >= 1.0f) {
                    lines = static_cast<int>(std::floor(pane->scroll_accumulator));
                    pane->scroll_accumulator -= static_cast<float>(lines);
                } else if (pane->scroll_accumulator <= -1.0f) {
                    lines = static_cast<int>(std::ceil(pane->scroll_accumulator));
                    pane->scroll_accumulator -= static_cast<float>(lines);
                }

                if (lines != 0) {
                    int current_offset = pane->terminal.get_scroll_offset();
                    int max_offset = static_cast<int>(pane->terminal.get_scrollback_size());

                    if ((lines > 0 && current_offset >= max_offset) || (lines < 0 && current_offset <= 0)) {
                        pane->scroll_velocity = 0.0f;
                        pane->scroll_accumulator = 0.0f;
                    } else {
                        pane->terminal.scroll_view(lines);
                        int new_offset = pane->terminal.get_scroll_offset();
                        if (new_offset == 0 || new_offset == max_offset) {
                            pane->scroll_velocity = 0.0f;
                            pane->scroll_accumulator = 0.0f;
                        }
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

        // Process incoming shell data per pane (suppressed for the pane a
        // running sinkdemo is streaming into; the other panes stay live)
        for (Pane* pane_ptr : all_panes(tw)) {
        Pane& pane = *pane_ptr;
        std::vector<char> output = pane.pty.read_pending();
        bool demo_owns_pane = SinkDemo::is_demo_running(tw) && pane_ptr == &tw->demo_target();
        if (!output.empty() && !demo_owns_pane) {
            std::lock_guard<std::mutex> lock(pane.grid_mutex);
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
                Uint64 gap = now - pane.last_output_chunk_time;
                pane.last_output_chunk_time = now;
                if (gap < 40) {
                    pane.rapid_chunk_streak++;
                } else {
                    pane.rapid_chunk_streak = 0;
                }
                bool fast_turnover = pane.rapid_chunk_streak >= 3;

                if (output.size() > 5 || fast_turnover) {
                    // Large chunk / fast turnover (command or program output): bypass typing effect
                    if (!pane.animation_buffer.empty()) {
                        pane.parser.parse(pane.terminal, pane.animation_buffer.data(), pane.animation_buffer.size());
                        pane.animation_buffer.clear();
                    }
                    pane.parser.parse(pane.terminal, output.data(), output.size());
                } else {
                    // Small, unhurried chunk (user typing): queue for animated typing
                    pane.animation_buffer.insert(pane.animation_buffer.end(), output.begin(), output.end());
                }
            } else {
                pane.parser.parse(pane.terminal, output.data(), output.size());
            }
            pane.terminal.lock_prompt_boundary_if_unset();
        }

        // Apply any OSC 0/2 title change from the data just parsed (only
        // the focused pane owns the window title)
        if (pane_ptr == tw->focused && pane.terminal.has_pending_title()) {
            std::string title = pane.terminal.take_window_title();
            SDL_SetWindowTitle(tw->window, title.empty() ? "sink" : title.c_str());
        }

        // Apply any OSC 52 clipboard write -- unlike the title, this isn't
        // gated to the focused pane: a background pane's tmux/script
        // finishing a copy is a legitimate, common case
        if (pane.terminal.has_pending_clipboard_text()) {
            std::string text = pane.terminal.take_clipboard_text();
            SDL_SetClipboardText(text.c_str());
        }

        // Process retro animated typing ticks
        if (tw->animated_typing && !pane.animation_buffer.empty()) {
            std::lock_guard<std::mutex> lock(pane.grid_mutex);
            size_t total_pending = pane.animation_buffer.size();
            size_t chars_to_process = 0;
            
            if (total_pending > 2000) {
                chars_to_process = total_pending;
            } else if (total_pending > 500) {
                chars_to_process = std::min(total_pending, (size_t)16);
            } else {
                chars_to_process = std::min(total_pending, (size_t)4);
            }
            
            pane.parser.parse(pane.terminal, pane.animation_buffer.data(), chars_to_process);
            pane.animation_buffer.erase(pane.animation_buffer.begin(), pane.animation_buffer.begin() + chars_to_process);
            
            if (pane.animation_buffer.empty()) {
                pane.terminal.lock_prompt_boundary_if_unset();
            }
        }
        } // end per-pane read/parse loop

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
        for (Pane* pane_ptr : all_panes(tw)) {
            Pane& pane = *pane_ptr;
            std::lock_guard<std::mutex> lock(pane.grid_mutex);
            pane.terminal.set_enable_ligatures(state->ligatures_enabled);
            float start_x = (pane.rect.x + state->padding) * state->display_scale;
            float start_y = (pane.rect.y + state->padding) * state->display_scale;

            // Clip each pane so its margin fill and glyphs can't bleed
            // across the divider into a neighbor
            SDL_Rect clip = {
                static_cast<int>(pane.rect.x * state->display_scale),
                static_cast<int>(pane.rect.y * state->display_scale),
                static_cast<int>(pane.rect.w * state->display_scale),
                static_cast<int>(pane.rect.h * state->display_scale)
            };
            SDL_SetRenderClipRect(tw->renderer, &clip);

            if (tw->hdr_console_enabled) {
                SDL_SetRenderColorScale(tw->renderer, kHdrConsoleTextBoost);
            }
            pane.terminal.render(tw->renderer, tw->font_manager, start_x, start_y, state->display_scale, dt, tw->animated_typing);
            if (tw->hdr_console_enabled) {
                SDL_SetRenderColorScale(tw->renderer, 1.0f);
            }

            // Dim unfocused panes so the active one reads at a glance
            if (pane_ptr != tw->focused && !tw->root->is_leaf()) {
                SDL_FRect dim = {
                    pane.rect.x * state->display_scale,
                    pane.rect.y * state->display_scale,
                    pane.rect.w * state->display_scale,
                    pane.rect.h * state->display_scale
                };
                SDL_SetRenderDrawBlendMode(tw->renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(tw->renderer, 0, 0, 0, 64);
                SDL_RenderFillRect(tw->renderer, &dim);
            }
            SDL_SetRenderClipRect(tw->renderer, nullptr);
        }

        // Divider lines between panes
        if (!tw->root->is_leaf()) {
            render_pane_dividers(tw, tw->root.get(), state->display_scale);
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
