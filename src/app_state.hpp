#pragma once

// Shared definitions of the app's core aggregates. Previously TerminalWindow
// and AppState were defined in main.cpp and *mirrored by hand* in
// sink_demo.cpp (a prefix copy that relied on layout compatibility); both
// files now include this single definition.

#include <SDL3/SDL.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ansi_parser.hpp"
#include "crt_shader.hpp"
#include "font_manager.hpp"
#include "hue_shift.hpp"
#include "pty_bridge.hpp"
#include "settings_ui.hpp"
#include "terminal_grid.hpp"
#include "video_engine.hpp"

// One shell session: grid + PTY + parser and all the per-session input and
// pacing state that used to live directly on TerminalWindow. A window shows
// one or more panes arranged by a split tree.
struct Pane {
    TerminalGrid terminal;
    PTYBridge pty;
    ANSIParser parser;
    std::mutex grid_mutex;

    // Typewriter-animation pacing
    std::vector<char> animation_buffer;
    Uint64 last_output_chunk_time = 0;
    int rapid_chunk_streak = 0;

    // Inertial scrollback state
    float scroll_accumulator = 0.0f;
    float scroll_velocity = 0.0f;
    Uint64 last_wheel_time = 0;

    // Native selection anchor
    int mouse_down_col = -1;
    int mouse_down_row = -1;

    // Last cell sent as a mouse motion report; motion inside one cell is
    // pixel-level noise the app never wants repeated
    int last_mouse_report_col = -1;
    int last_mouse_report_row = -1;

    // Fractional wheel ticks carried between events while translating
    // alt-screen scrolling into arrow keys (trackpads emit tiny deltas)
    float alt_scroll_accum = 0.0f;

    // Content area in window points (excludes title bar), assigned by
    // layout_panes() whenever the window geometry or the split tree changes
    SDL_FRect rect = {0.0f, 0.0f, 0.0f, 0.0f};
};

// Binary split tree. A node is either a leaf (pane != nullptr) or an
// internal split with two children. `vertical` means the children sit side
// by side (the divider line runs vertically).
struct PaneNode {
    std::unique_ptr<Pane> pane;
    bool vertical = false;
    float ratio = 0.5f;
    std::unique_ptr<PaneNode> a;
    std::unique_ptr<PaneNode> b;

    // Layout rect this node occupied last layout pass (window points);
    // divider dragging derives the new split ratio from it
    SDL_FRect rect = {0.0f, 0.0f, 0.0f, 0.0f};

    bool is_leaf() const { return pane != nullptr; }
};

struct TerminalWindow {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    FontManager font_manager;
    VideoEngine video_engine;

    // Split tree; `focused` always points at a live leaf pane of `root`.
    // `demo_pane` pins which pane a running sinkdemo streams into, so focus
    // changes mid-demo can't redirect it.
    std::unique_ptr<PaneNode> root;
    Pane* focused = nullptr;
    Pane* demo_pane = nullptr;

    // Internal node whose divider is being dragged (null when not dragging)
    PaneNode* dragging_divider = nullptr;

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

    // Dissolve state
    enum FadeState { FADE_HOLD_BLACK, FADE_OUT, FADE_DONE };
    FadeState fade_state = FADE_HOLD_BLACK;
    float fade_opacity = 1.0f;
    // When the current FADE_HOLD_BLACK started (0 = not yet stamped). The
    // hold waits on the video's first decoded frame, and that frame can
    // legitimately never arrive (decode failure, a file that stopped being
    // readable) -- without a deadline the full-window black overlay in
    // stage E just stays up forever over an otherwise live terminal.
    Uint64 fade_hold_start_time = 0;

    // Feature states
    bool search_drawer_open = false;
    std::string search_input_text;
    bool crt_mode_enabled = false;
    bool vibrancy_enabled = true;
    bool hdr_console_enabled = false;

    float cell_w = 0.0f;
    float cell_h = 0.0f;

    // Debounces the (expensive: PTY SIGWINCH + full grid reflow) resize
    // work so a burst of resize events -- a fast native window-zoom
    // animation firing dozens of intermediate frames in ~200ms is the worst
    // case, but a manual edge-drag does this too -- doesn't apply a reflow
    // per intermediate frame (which was visibly corrupting the shell's
    // prompt redraw). Only the latest pending size is kept; it's applied
    // once no new resize event has arrived for a short quiet period.
    bool has_pending_resize = false;
    int pending_w = 0;
    int pending_h = 0;
    Uint64 last_resize_event_time = 0;

    // Detects a native tab bar appearing/disappearing (see
    // get_top_offset_pts): unlike an actual window resize, that doesn't
    // fire SDL_EVENT_WINDOW_RESIZED, so it's polled once a frame instead.
    float last_top_offset_pts = -1.0f;

    // Focused-pane accessor. The vast majority of call sites (keyboard
    // input, clipboard, prompt editing) act on the focused pane; mouse and
    // per-pane frame work resolve their pane explicitly instead.
    Pane& fpane() { return *focused; }

    // The pane a running demo streams into (falls back to focused so the
    // accessor is always safe to call).
    Pane& demo_target() { return demo_pane ? *demo_pane : *focused; }
};

inline void collect_panes(PaneNode* node, std::vector<Pane*>& out) {
    if (!node) return;
    if (node->is_leaf()) {
        out.push_back(node->pane.get());
        return;
    }
    collect_panes(node->a.get(), out);
    collect_panes(node->b.get(), out);
}

inline std::vector<Pane*> all_panes(TerminalWindow* tw) {
    std::vector<Pane*> panes;
    collect_panes(tw->root.get(), panes);
    return panes;
}

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
    int scrollback_lines = 10000;
};
