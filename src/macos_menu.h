#pragma once

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

// Add "File", "Edit", and "Settings..." menus to the macOS application menu bar
void setup_macos_menu();

// Groups a newly created child window as a tab inside a parent window
void add_window_as_tab(SDL_Window* parent_sdl_win, SDL_Window* child_sdl_win);

// Thread-safe flags to request menu commands
void set_settings_requested(bool requested);
bool get_settings_requested();

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

#ifdef __cplusplus
}
#endif
