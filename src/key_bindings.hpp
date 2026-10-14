#pragma once

// What the keyboard sends besides typed text: the macOS editing keys other
// terminals have trained everyone's fingers on, the control characters that
// live on symbol keys, and Option as Meta. Kept free of window and pty state
// so they can be tested without either; main.cpp decides when they apply.

#include <SDL3/SDL_keycode.h>
#include <string>

namespace key_bindings {

// The macOS text-editing keys, as the bytes zsh and bash understand (their
// default, emacs-style bindings): ⌥← and ⌥→ move by word (ESC b, ESC f), ⌘←
// and ⌘→ go to the start and end of the line (^A, ^E), ⌥⌫ deletes the word
// behind the cursor (ESC DEL) and ⌥⌦ the one ahead (ESC d), and ⌘⌫ deletes
// back to the start of the line (^U). The same bytes Terminal.app and Ghostty
// send. Null for anything else -- including with Control held, so ⌃⌥ and ⌃⌘
// combinations stay free for whatever else wants them.
inline const char* editing_sequence(SDL_Keycode sym, SDL_Keymod mod) {
    if (mod & SDL_KMOD_CTRL) return nullptr;
    const bool alt = (mod & SDL_KMOD_ALT) != 0;
    const bool cmd = (mod & SDL_KMOD_GUI) != 0;
    if (alt == cmd) return nullptr; // neither, or both
    switch (sym) {
        case SDLK_LEFT:      return alt ? "\x1b" "b" : "\x01";
        case SDLK_RIGHT:     return alt ? "\x1b" "f" : "\x05";
        case SDLK_BACKSPACE: return alt ? "\x1b\x7f" : "\x15";
        case SDLK_DELETE:    return alt ? "\x1b" "d" : nullptr;
        default:             return nullptr;
    }
}

// The control character Ctrl sends with a symbol key, or -1 if it has none.
// Ctrl+letter was always handled; these never were, and went nowhere:
// ⌃Space and ⌃2 (NUL -- set-mark in emacs), ⌃[ (ESC), ⌃\ (quits a program
// that ignores ⌃C), ⌃], ⌃6 (RS), and ⌃-, ⌃_ and ⌃/ (US -- undo in zsh and
// readline). `sym` is the key's unshifted character on the current layout,
// which is what SDL reports.
inline int control_symbol_byte(SDL_Keycode sym) {
    switch (sym) {
        case SDLK_SPACE: case SDLK_2: case SDLK_AT:            return 0x00;
        case SDLK_LEFTBRACKET:                                  return 0x1b;
        case SDLK_BACKSLASH:                                    return 0x1c;
        case SDLK_RIGHTBRACKET:                                 return 0x1d;
        case SDLK_6: case SDLK_CARET:                           return 0x1e;
        case SDLK_MINUS: case SDLK_UNDERSCORE: case SDLK_SLASH: return 0x1f;
        default:                                                return -1;
    }
}

// sink.toml's `option_as_meta`: which Option keys act as Meta, sending ESC
// ahead of the key, instead of typing the special characters macOS puts on
// them (⌥F is ƒ). Off by default, as in every Mac terminal; emacs and
// readline users turn it on.
enum class OptionAsMeta { None, Left, Right, Both };

inline OptionAsMeta parse_option_as_meta(const std::string& value) {
    if (value == "left") return OptionAsMeta::Left;
    if (value == "right") return OptionAsMeta::Right;
    if (value == "both") return OptionAsMeta::Both;
    return OptionAsMeta::None;
}

// The matching value for SDL_HINT_MAC_OPTION_AS_ALT, which keeps those
// Option keys from starting macOS's accent compositions (⌥E then E is é).
inline const char* option_as_alt_hint(OptionAsMeta setting) {
    switch (setting) {
        case OptionAsMeta::Left:  return "only_left";
        case OptionAsMeta::Right: return "only_right";
        case OptionAsMeta::Both:  return "both";
        default:                  return "none";
    }
}

// Whether the Option held in `mod` counts as Meta under the setting.
inline bool option_is_meta(OptionAsMeta setting, SDL_Keymod mod) {
    switch (setting) {
        case OptionAsMeta::Left:  return (mod & SDL_KMOD_LALT) != 0;
        case OptionAsMeta::Right: return (mod & SDL_KMOD_RALT) != 0;
        case OptionAsMeta::Both:  return (mod & SDL_KMOD_ALT) != 0;
        default:                  return false;
    }
}

// Meta plus a key: ESC, then the character the key types without Option
// (with Shift, if held -- which is why `typed` comes from the layout and not
// the key event). False for keys that type nothing.
inline bool meta_sequence(SDL_Keycode typed, std::string& out) {
    if (typed & SDLK_SCANCODE_MASK) return false;
    if (typed < 0x20 || typed == 0x7f || typed >= 0x110000) return false;
    out = "\x1b";
    if (typed < 0x80) {
        out += static_cast<char>(typed);
    } else if (typed < 0x800) {
        out += static_cast<char>(0xC0 | (typed >> 6));
        out += static_cast<char>(0x80 | (typed & 0x3F));
    } else if (typed < 0x10000) {
        out += static_cast<char>(0xE0 | (typed >> 12));
        out += static_cast<char>(0x80 | ((typed >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (typed & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (typed >> 18));
        out += static_cast<char>(0x80 | ((typed >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((typed >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (typed & 0x3F));
    }
    return true;
}

} // namespace key_bindings
