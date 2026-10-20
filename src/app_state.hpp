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
#include "background_shader.hpp"
#include "config_store.hpp"
#include "crt_shader.hpp"
#include "font_manager.hpp"
#include "hue_shift.hpp"
#include "pty_bridge.hpp"
#include "settings_ui.hpp"
#include "terminal_grid.hpp"
#include "video_engine.hpp"
#include "key_bindings.hpp"

// One shell session: grid + PTY + parser and all the per-session input and
// pacing state that used to live directly on TerminalWindow. A window shows
// one or more panes arranged by a split tree.
struct Pane {
    TerminalGrid terminal;
    PTYBridge pty;
    ANSIParser parser;
    std::mutex grid_mutex;

    // Reused every frame to receive PTYBridge::read_pending()'s bytes. Held
    // across frames rather than declared at the call site so its storage and
    // the reader thread's trade places instead of being reallocated.
    std::vector<char> pty_output;

    // Bytes read from the pty and not yet parsed. Parsing is capped per frame
    // (kMaxParseBytesPerFrame), so a program dumping output faster than the
    // parser absorbs it leaves a backlog here rather than stretching the
    // frame until it is gone.
    std::vector<char> pty_pending;

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

    // A PTY size held back while the window is still being resized; see
    // layout_pane_node(). Applied once it has stopped changing.
    bool pty_resize_pending = false;
    int pending_pty_cols = 0, pending_pty_rows = 0;
    int pending_pty_px_w = 0, pending_pty_px_h = 0;
    Uint64 pending_pty_since = 0;
    // When the held-back size was last sent; the cursor stays hidden briefly
    // after, until the shell's redraw has put it back on the text.
    Uint64 pty_resize_sent_at = 0;

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

    // The pane zoomed to fill the window (⇧⌘↩), or null. The others keep
    // running, and keep their size, but aren't laid out, drawn or clickable
    // until it's put back. Anything that changes which pane is focused or
    // how the tree is split puts it back first.
    Pane* zoomed = nullptr;

    // The title the focused pane's program asked for (OSC 0/2), and the one
    // last given to the window -- which differs while broadcasting, when a
    // warning goes in front of it. See sync_window_title().
    std::string shell_title = "sink";
    std::string shown_title;

    // Font size in points. Per window, like Terminal's View > Bigger and
    // Smaller; every window starts at its preset's font_size, which Default
    // Size returns to.
    float font_size = 15.0f;
    float base_font_size = 15.0f;

    std::atomic<bool> demo_running{false};
    std::atomic<bool> demo_skip_requested{false};
    std::atomic<bool> demo_abort{false};
    std::thread demo_thread;

    bool has_video = false;
    // The preset this window shows, and its background and font. Usually
    // the active preset, but a window opened with File > New Window with
    // Preset has its own -- kept when its font is reloaded for a new
    // display, and the one it follows when sink.toml changes.
    std::string preset_name;
    std::string video_path;
    std::string font_path;
    // Its preset's theme, given to every pane, including ones split off later.
    ColorScheme color_scheme;

    // What's behind the text (`background` in sink.toml); see apply_background.
    enum class Backdrop { Video, Color, Gradient, Gel, Clear, Frosted, LiquidGlass };
    Backdrop backdrop = Backdrop::Video;
    std::string background;              // the setting, as written
    BackgroundShader background_shader;  // gradient and gel
    BackgroundShader::Params backdrop_params;
    SDL_FColor backdrop_color{0, 0, 0, 1};  // solid, and the tint over glass
    float glass_tint = 0.3f;
    float gel_speed = 1.0f;
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

    // Pixels per point of the display this window is on -- 2 on a Retina
    // screen, 1 on most external ones. Layout works in points and rendering
    // in pixels, so every point is multiplied by this on the way to the
    // screen, and the font is rasterized at base size times this. Per window,
    // because two windows can be on displays with different densities, and
    // kept current (refresh_display_scale) because a window can move between
    // them -- including by going full screen on another display.
    float display_scale = 1.0f;

    // A resize in progress: a window edge being dragged, a zoom animation,
    // or a pane divider being dragged. The grids follow it live, throttled
    // by what reflowing costs -- see the layout step in SDL_AppIterate.
    //
    // This used to wait for the resize to stop before reflowing at all,
    // because reflowing per frame corrupted the shell's prompt redraw. That
    // corruption is fixed at its source now (TerminalGrid::resize keeps the
    // cursor where the shell will look, and a shell editing a line is told
    // the size only once it settles), so the wait went, and with it text that
    // stayed unwrapped until the mouse was released.
    bool has_pending_resize = false;
    int pending_w = 0;
    int pending_h = 0;
    Uint64 last_resize_event_time = 0;
    Uint64 last_live_layout_tick = 0;
    Uint64 live_layout_gap_ms = 0;  // twice what the last layout took, capped

    // Last time this window actually presented. Synchronized output (DECSET
    // 2026) defers presentation, and this is what bounds that: the screen has
    // to refresh even if an application holds updates open back to back.
    Uint64 last_present_ms = 0;

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

    // sink.toml. Read at startup, edited by the Settings window, and saved
    // a moment after the last change (see ConfigStore).
    ConfigStore config;
    // The active preset's theme, for new windows.
    ColorScheme color_scheme;
    std::string theme_name = "sink";
    std::string background = "pool";    // the active preset's, as written

    std::string video_path;
    std::string font_path;
    std::string active_preset_name = "pool";
    float padding = 2.0f;
    float base_font_size = 15.0f;
    Uint64 last_tick = 0;
    bool input_broadcasting = false;
    // Set when a keypad press was already answered with an SS3 sequence, so
    // the text-input event SDL sends for the same key is dropped rather than
    // reaching the shell twice. Armed and cleared within one key press.
    bool suppress_next_text_input = false;
    // sink.toml's option_as_meta (see key_bindings.hpp).
    key_bindings::OptionAsMeta option_as_meta = key_bindings::OptionAsMeta::None;
    float exposure = 0.7f;
    float hue_shift = 0.0f;
    bool animated_typing = true;
    bool vibrancy_enabled = true;
    bool crt_mode_enabled = false;
    bool ligatures_enabled = true;
    bool hdr_console_enabled = false;
    int scrollback_lines = 10000;
};
