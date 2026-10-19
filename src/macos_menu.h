#pragma once

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

// Add "File", "Edit", and "Settings..." menus to the macOS application menu bar
void setup_macos_menu();

// Stops the 60Hz timer that drives frames during AppKit's blocking loops.
// Called at quit, before the app state the timer's frames use is freed.
void stop_menu_render_timer();

// Groups a newly created child window as a tab inside a parent window
void add_window_as_tab(SDL_Window* parent_sdl_win, SDL_Window* child_sdl_win);

// Height in points of whatever AppKit is currently drawing above the
// content view (title bar strip, plus the native tab bar when the window
// has more than one tab) -- derived from NSWindow.contentLayoutRect, the
// documented API for exactly this. Returns -1 if unavailable (window gone,
// non-Apple build wouldn't call this at all).
float get_native_content_top_inset(SDL_Window* sdl_win);

// Whether the window is in macOS full screen (its own Space), however it got
// there -- the green button, the menu, or the keyboard shortcut.
bool is_native_fullscreen(SDL_Window* sdl_win);

// Opens a URI via NSWorkspace (no shell involved -- terminal output is
// attacker-controlled, e.g. `cat`'d files or a compromised remote SSH
// session, so this must never go through system()/popen()). Only
// http/https/mailto are opened; anything else (including file:, which
// would expose arbitrary local file access to a hyperlink in untrusted
// output) is silently ignored.
void open_url_if_safe(const char* uri);

// Thread-safe flags to request menu commands
void set_settings_requested(bool requested);
bool get_settings_requested();

void set_cut_requested(bool requested);
bool get_cut_requested();

void set_copy_requested(bool requested);
bool get_copy_requested();

void set_paste_requested(bool requested);
bool get_paste_requested();

void set_select_all_requested(bool requested);
bool get_select_all_requested();

void set_new_window_requested(bool requested);
bool get_new_window_requested();

void set_new_tab_requested(bool requested);
bool get_new_tab_requested();

void set_close_window_requested(bool requested);
bool get_close_window_requested();

void set_print_requested(bool requested);
bool get_print_requested();
void trigger_print_dialog(const char* text_utf8);

void set_find_requested(bool requested);
bool get_find_requested();

void set_crt_mode_requested(bool requested);
bool get_crt_mode_requested();

// Split-pane commands (Shell menu)
void set_split_pane_right_requested(bool requested);
bool get_split_pane_right_requested();

void set_split_pane_down_requested(bool requested);
bool get_split_pane_down_requested();

void set_close_pane_requested(bool requested);
bool get_close_pane_requested();

// Directional pane-focus request: 0 = none, 1 = left, 2 = right, 3 = up,
// 4 = down. The getter consumes the pending request.
void set_pane_focus_requested(int direction);
int get_pane_focus_requested();
// Step pane focus through the window's panes in order: 1 = next, -1 =
// previous, 0 = none. The getter consumes the pending request.
void set_pane_cycle_requested(int step);
int get_pane_cycle_requested();
// Zoom the focused pane to fill its window, or put it back (Window menu).
void set_zoom_pane_requested(bool requested);
bool get_zoom_pane_requested();
// Clear the screen and scrollback (Edit menu).
void set_clear_screen_requested(bool requested);
bool get_clear_screen_requested();
// Font size for the active window (View menu): 1 = bigger, -1 = smaller,
// 2 = back to the default, 0 = none. The getter consumes the pending request.
void set_font_size_requested(int change);
int get_font_size_requested();
// Window > Broadcast Input: a request to flip broadcasting, and the item's
// checkmark, which follows the setting however it was changed.
void set_toggle_broadcast_requested(bool requested);
bool get_toggle_broadcast_requested();
void set_broadcast_menu_checked(bool checked);
// The badge on sink's Dock icon; null clears it.
void set_dock_badge(const char* label_utf8);
// Brings the window's index-th native tab (1-based) to the front; 9 or more
// means the last tab, as in Safari and Terminal. No-op without tabs.
void select_native_tab(SDL_Window* sdl_win, int index);

// Enable or disable macOS native translucent window vibrancy / acrylic blur
void enable_macos_window_vibrancy(SDL_Window* sdl_win, bool enable);

// Classic macOS "zoom" (grow to fill the screen, or restore) with AppKit's
// own native animation -- the same toggle a double-click on a native title
// bar performs. Distinct from the green-button Spaces-based fullscreen.
void zoom_macos_window(SDL_Window* sdl_win);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <string>
std::string get_bundle_resource_path(const std::string& filename);
#endif
