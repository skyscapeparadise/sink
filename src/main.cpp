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
#include <optional>
#include <strings.h>
#include <sys/stat.h>
#include "terminal_grid.hpp"
#include "pty_bridge.hpp"
#include "prompt_editing.hpp"
#include "ansi_parser.hpp"
#include "video_engine.hpp"
#include "settings_ui.hpp"
#include "sink_demo.hpp"
#include "app_state.hpp"
#include "config_store.hpp"
#include "system_fonts.hpp"
#include "themes.hpp"
#include "background_shader.hpp"
#include "keybindings.hpp"
#include "hue_shift.hpp"
#include "crt_shader.hpp"

#if defined(__APPLE__)
#include "macos_menu.h"
#endif

// Forward declaration of loop iteration handler
SDL_AppResult SDL_AppIterate(void* appstate);

// Global AppState reference for menu timer callbacks
static void* g_app_state = nullptr;

// How long the black dissolve overlay will wait on the video background's
// first decoded frame before giving up and revealing the terminal anyway.
// See the FADE_HOLD_BLACK handling in SDL_AppIterate for why this deadline
// exists at all.
static constexpr Uint64 kFadeHoldTimeoutMs = 3000;

// How much brighter than normal (1.0) "hdr console" text renders. Values
// above 1.0 only actually look brighter than SDR white -- rather than just
// clamping to white -- because every renderer is created with the
// linear/extended colorspace (see the renderer-creation comment in
// create_terminal_window).
static const float kHdrConsoleTextBoost = 1.6f;

// Ceiling on the time one frame may spend parsing shell output, per pane, and
// the granularity the ceiling is checked at.
//
// A time budget rather than a byte budget because the parser's throughput is
// not a constant to predict against: it holds 180-196 MB/s on sink_bench's
// 120-column grid but roughly 120 MB/s at 200 columns, since a wider grid
// moves more per scrolled line. A byte cap chosen from the first figure
// overshoots by 3x at the second. This measures instead.
//
// 8ms is half a 60Hz frame: enough left over for rendering, presenting and
// event handling with room for variance, and measured to hold end-to-end
// throughput at 57 MB/s against 100 MB/s uncapped. The trade is linear across
// the range (5ms buys 32 MB/s, 14ms buys 89 MB/s) and this is the midpoint
// where a frame still has as much time outside the parser as inside it.
//
// The parse slice (sync_output::kSliceBytes) is large enough that the clock
// read is noise and small enough that overshoot past the budget is bounded to
// one slice -- measured at 0.65ms over, on an 8ms budget.
static constexpr Uint64 kParseBudgetNs = 8ull * 1000000ull;

// Parses as much of `data` as the budget allows and returns how much that was;
// the rest waits for the next frame. Splitting a burst across frames is safe
// because ANSIParser carries its state between calls -- a half-built CSI, a
// partial UTF-8 sequence and a pending charset designation all survive the
// boundary. Output inside a synchronized update is held until the update
// closes (see sync_output.hpp).
static size_t parse_within_budget(Pane& pane, const char* data, size_t size) {
    const Uint64 start = SDL_GetTicksNS();
    return sync_output::parse(pane.parser, pane.terminal, pane.sync_hold, data, size, start / 1000000ull,
                              [start]() { return SDL_GetTicksNS() - start >= kParseBudgetNs; });
}

// --- Kitty keyboard protocol encoding -------------------------------------
//
// Only the keys the protocol actually disambiguates are re-encoded. Ordinary
// typing keeps arriving as SDL_EVENT_TEXT_INPUT and is written through
// untouched, which is what keeps dead keys, IME and international layouts
// working -- reporting every key as an escape code (the protocol's flag 8)
// would mean taking that path over, and is deliberately not supported.

// The protocol's modifier bitfield, before the +1 it carries on the wire.
static int kitty_modifier_bits(SDL_Keymod mod) {
    int m = 0;
    if (mod & SDL_KMOD_SHIFT) m |= 1;
    if (mod & SDL_KMOD_ALT)   m |= 2;
    if (mod & SDL_KMOD_CTRL)  m |= 4;
    if (mod & SDL_KMOD_GUI)   m |= 8;
    return m;
}

// Keys with an established CSI form: the modifiers ride in that form rather
// than in a CSI u sequence, which is what keeps them working for readers that
// only understand the legacy encoding.
struct KittyFunctionalKey {
    SDL_Keycode sym;
    char final_byte; // 0 when the key uses the CSI <number> ~ form instead
    int number;
};
static const KittyFunctionalKey kKittyFunctionalKeys[] = {
    { SDLK_UP,       'A', 1 },  { SDLK_DOWN,     'B', 1 },
    { SDLK_RIGHT,    'C', 1 },  { SDLK_LEFT,     'D', 1 },
    { SDLK_HOME,     'H', 1 },  { SDLK_END,      'F', 1 },
    { SDLK_F1,       'P', 1 },  { SDLK_F2,       'Q', 1 },
    { SDLK_F3,       'R', 1 },  { SDLK_F4,       'S', 1 },
    { SDLK_INSERT,    0,  2 },  { SDLK_DELETE,    0,  3 },
    { SDLK_PAGEUP,    0,  5 },  { SDLK_PAGEDOWN,  0,  6 },
    { SDLK_F5,        0, 15 },  { SDLK_F6,        0, 17 },
    { SDLK_F7,        0, 18 },  { SDLK_F8,        0, 19 },
    { SDLK_F9,        0, 20 },  { SDLK_F10,       0, 21 },
    { SDLK_F11,       0, 23 },  { SDLK_F12,       0, 24 },
};

// Builds the sequence for one key event, or returns false to mean "this key
// is not one the protocol changes here -- use the existing handling".
//
// `event_type` is 1 press, 2 repeat, 3 release, and is only ever emitted when
// the app asked for event reporting.
static bool encode_kitty_key(SDL_Keycode sym, SDL_Keymod mod, bool is_repeat,
                             bool is_press, int flags, std::string& out) {
    if (!(flags & TerminalGrid::kKbdDisambiguate)) return false;
    const bool report_events = (flags & TerminalGrid::kKbdReportEvents) != 0;
    if (!is_press && !report_events) return false; // releases are invisible otherwise

    const int mods = kitty_modifier_bits(mod);
    const int event_type = is_press ? (is_repeat ? 2 : 1) : 3;

    // The modifier field is omitted when there is nothing to report, unless an
    // event type has to be carried -- the two share one parameter.
    auto suffix = [&](int leading) {
        std::string t;
        if (mods != 0 || report_events) {
            t += std::to_string(leading);
            t += ';';
            t += std::to_string(mods + 1);
            if (report_events) {
                t += ':';
                t += std::to_string(event_type);
            }
        } else if (leading != 1) {
            t += std::to_string(leading);
        }
        return t;
    };

    for (const KittyFunctionalKey& k : kKittyFunctionalKeys) {
        if (k.sym != sym) continue;
        // Unmodified arrows with nothing to report are left alone so DECCKM
        // still decides between the CSI and SS3 forms for them.
        if (mods == 0 && !report_events) return false;
        out = "\x1b[";
        if (k.final_byte) {
            out += suffix(k.number);
            out += k.final_byte;
        } else {
            out += suffix(k.number);
            out += '~';
        }
        return true;
    }

    // Keys the protocol gives a codepoint of their own.
    char32_t code = 0;
    switch (sym) {
        case SDLK_ESCAPE:    code = 27;  break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:  code = 13;  break;
        case SDLK_TAB:       code = 9;   break;
        case SDLK_BACKSPACE: code = 127; break;
        default:
            // Printable keys carry their unshifted Unicode codepoint, which is
            // what an SDL keycode already is once the scancode-namespace bit
            // is ruled out.
            if (sym > 0 && sym < 0x110000 && !(sym & SDLK_SCANCODE_MASK)) {
                code = static_cast<char32_t>(sym);
            }
            break;
    }
    if (code == 0) return false;

    // Escape is re-encoded even unmodified -- telling it apart from the ESC
    // that introduces every other sequence is the whole point of the flag.
    // Everything else keeps its legacy byte until a modifier makes it
    // ambiguous, so plain typing, Enter, Tab and Backspace are untouched.
    if (mods == 0 && !report_events && sym != SDLK_ESCAPE) return false;

    out = "\x1b[";
    out += std::to_string(static_cast<unsigned>(code));
    if (mods != 0 || report_events) {
        out += ';';
        out += std::to_string(mods + 1);
        if (report_events) {
            out += ':';
            out += std::to_string(event_type);
        }
    }
    out += 'u';
    return true;
}

// Application keypad mode (DECKPAM). The final byte each keypad key sends as
// an SS3 sequence, which is how a full-screen app tells a keypad 1 from the 1
// on the number row. Numeric mode sends the plain character instead, which is
// what SDL_EVENT_TEXT_INPUT already delivers.
static char app_keypad_final(SDL_Keycode sym) {
    switch (sym) {
        case SDLK_KP_0: return 'p';
        case SDLK_KP_1: return 'q';
        case SDLK_KP_2: return 'r';
        case SDLK_KP_3: return 's';
        case SDLK_KP_4: return 't';
        case SDLK_KP_5: return 'u';
        case SDLK_KP_6: return 'v';
        case SDLK_KP_7: return 'w';
        case SDLK_KP_8: return 'x';
        case SDLK_KP_9: return 'y';
        case SDLK_KP_PERIOD:   return 'n';
        case SDLK_KP_ENTER:    return 'M';
        case SDLK_KP_PLUS:     return 'k';
        case SDLK_KP_MINUS:    return 'm';
        case SDLK_KP_MULTIPLY: return 'j';
        case SDLK_KP_DIVIDE:   return 'o';
        case SDLK_KP_COMMA:    return 'l';
        case SDLK_KP_EQUALS:   return 'X';
        default: return 0;
    }
}

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
        "backgrounds/sinkpool.mp4",
        "../backgrounds/sinkpool.mp4"
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

// The bundled Monaspace Neon: the app bundle's copy, else the source tree's
// when run from a build directory. Empty if neither is there.
static std::string resolve_bundled_font_path() {
    const std::string candidates[] = {
        get_bundle_resource_path("MonaspaceNeon-Regular.otf"),
        "fonts/MonaspaceNeon-Regular.otf",
        "../fonts/MonaspaceNeon-Regular.otf",
    };
    for (const std::string& path : candidates) {
        FILE* f = fopen(path.c_str(), "r");
        if (f) {
            fclose(f);
            return path;
        }
    }
    return "";
}

static std::string resolve_default_font_path() {
    std::string bundled = resolve_bundled_font_path();
    if (!bundled.empty()) return bundled;
    const char* fallbacks[] = {
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

// The bundled background and font are "pool" and "Monaspace Neon" in
// sink.toml; anything else is a path. A name sink doesn't know (from a newer
// config, or a typo) falls back to the bundled file rather than nothing.
static std::string resolve_background(const std::string& value) {
    return config::is_path(value) ? value : resolve_default_video_path();
}

// The bundled font's name in sink.toml.
static bool is_bundled_font_name(const std::string& value) {
    return strcasecmp(value.c_str(), "Monaspace Neon") == 0;
}

// A font as FontManager takes it: a path, or the name of an installed font.
// One that isn't installed falls back to the bundled font, and Settings says
// so (update_config_notice).
static std::string resolve_font(const std::string& value) {
    if (config::is_path(value)) return value;
    if (is_bundled_font_name(value) || !FontManager::can_open(value)) return resolve_default_font_path();
    return value;
}

static std::string font_setting(const std::string& resolved_path) {
    return resolved_path.find("MonaspaceNeon-Regular.otf") != std::string::npos ? std::string("Monaspace Neon")
                                                                                : resolved_path;
}

static void refresh_settings_ui_presets(AppState* state);

// A preset's colors: its theme (sink's own if the name isn't one), with any
// [presets.<name>.colors] overrides on top.
static ColorScheme scheme_for(const config::PresetValues& p) {
    const themes::Theme* t = themes::find(p.theme);
    if (!t) t = &themes::all().front();
    auto color = [](const themes::Rgb& c, float alpha = 1.0f) {
        return SDL_FColor{ c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, alpha };
    };
    auto pick = [&](const std::string& override_hex, const themes::Rgb& theme_color) {
        themes::Rgb rgb;
        return (!override_hex.empty() && themes::parse_hex(override_hex, rgb)) ? rgb : theme_color;
    };
    ColorScheme s;
    s.foreground = color(pick(p.foreground, t->foreground));
    s.background = color(pick(p.background_color, t->background));
    s.cursor = color(pick(p.cursor, t->cursor));
    s.selection = color(pick(p.selection, t->selection), t->selection_alpha);
    for (int i = 0; i < 16; ++i) {
        s.palette[i] = color(pick(i < static_cast<int>(p.palette.size()) ? p.palette[i] : std::string(),
                                  t->palette[i]));
    }
    return s;
}

using Backdrop = TerminalWindow::Backdrop;

// `background` as a kind. Anything not named here is a video: a path, or a
// built-in one such as "pool".
static Backdrop backdrop_of(const std::string& background) {
    static const std::pair<const char*, Backdrop> kinds[] = {
        { "color", Backdrop::Color },           { "gradient", Backdrop::Gradient },
        { "gel", Backdrop::Gel },               { "clear", Backdrop::Clear },
        { "frosted glass", Backdrop::Frosted }, { "liquid glass", Backdrop::LiquidGlass },
    };
    if (config::is_path(background)) return Backdrop::Video;
    for (const auto& [name, kind] : kinds) {
        if (strcasecmp(name, background.c_str()) == 0) return kind;
    }
    return Backdrop::Video;
}

// What Settings shows for the background: the kind's name, or the video.
static std::string background_display(const AppState* state) {
    return backdrop_of(state->background) == Backdrop::Video ? state->video_path : state->background;
}

static SDL_FColor mix_color(const SDL_FColor& a, const SDL_FColor& b, float t) {
    return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1.0f };
}

static SDL_FColor hex_color(const std::string& hex) {
    themes::Rgb rgb;
    themes::parse_hex(hex, rgb);
    return { rgb.r / 255.0f, rgb.g / 255.0f, rgb.b / 255.0f, 1.0f };
}

// Gives a window its preset's background: opens or closes the video (only
// when it changes), works out the gradient's and gel's colors -- the
// preset's own, else built from the theme -- and shows the right macOS view
// behind the window. tw->color_scheme must already be the preset's.
static void apply_background(AppState* state, TerminalWindow* tw, const config::PresetValues& p) {
    (void)state;
    tw->background = p.background;
    tw->backdrop = backdrop_of(p.background);

    const std::string video = tw->backdrop == Backdrop::Video ? resolve_background(p.background) : std::string();
    if (tw->video_path != video) {
        tw->video_engine.stop();
        tw->video_engine.close_video();
        tw->has_video = false;
        tw->video_path = video;
        if (!video.empty() && tw->video_engine.open_video(tw->renderer, video)) {
            tw->video_engine.start();
            tw->has_video = true;
            tw->fade_state = TerminalWindow::FADE_HOLD_BLACK;
            tw->fade_opacity = 1.0f;
        }
    }

    const ColorScheme& scheme = tw->color_scheme;
    tw->backdrop_color = scheme.background;
    tw->glass_tint = static_cast<float>(p.glass_tint);
    tw->gel_speed = static_cast<float>(p.gel_speed);

    BackgroundShader::Params& bp = tw->backdrop_params;
    if (tw->backdrop == Backdrop::Gradient) {
        bp.mode = BackgroundShader::Mode::Gradient;
        bp.angle_degrees = static_cast<float>(p.gradient_angle);
        if (p.gradient_colors.size() >= 2) {
            bp.count = static_cast<int>(std::min<size_t>(p.gradient_colors.size(), 8));
            for (int i = 0; i < bp.count; ++i) bp.colors[i] = hex_color(p.gradient_colors[i]);
        } else {
            // The theme's background, deepening toward its blue.
            bp.count = 2;
            bp.colors[0] = scheme.background;
            bp.colors[1] = mix_color(scheme.background, scheme.palette[4], 0.5f);
        }
    } else if (tw->backdrop == Backdrop::Gel) {
        bp.mode = BackgroundShader::Mode::Gel;
        if (p.gel_colors.size() == 5) {
            for (int i = 0; i < 5; ++i) bp.colors[i] = hex_color(p.gel_colors[i]);
        } else if (strcasecmp(p.theme.c_str(), "sink") == 0) {
            // sink's own theme: the original nebula, purple falling to black.
            const SDL_FColor original[5] = {
                {0.012f, 0.006f, 0.020f, 1}, {0.110f, 0.035f, 0.170f, 1}, {0.300f, 0.075f, 0.380f, 1},
                {0.560f, 0.200f, 0.640f, 1}, {0.820f, 0.560f, 0.900f, 1},
            };
            for (int i = 0; i < 5; ++i) bp.colors[i] = original[i];
        } else {
            // The same shape in the theme's colors: its background, darkened,
            // for the depths, its magenta for the cloud, its text for the
            // wisps. The steps between keep the original's proportions -- the
            // deep layer about a fifth of the way to the cloud's color, the
            // next about half -- or the nebula loses its depth.
            const SDL_FColor& accent = scheme.palette[5];
            const SDL_FColor depths = mix_color(scheme.background, SDL_FColor{0, 0, 0, 1}, 0.75f);
            bp.colors[0] = depths;
            bp.colors[1] = mix_color(depths, accent, 0.2f);
            bp.colors[2] = mix_color(depths, accent, 0.5f);
            bp.colors[3] = accent;
            bp.colors[4] = mix_color(accent, scheme.foreground, 0.5f);
        }
    }

#if defined(__APPLE__)
    int behind = 0;
    if (tw->backdrop == Backdrop::Frosted) behind = 1;
    if (tw->backdrop == Backdrop::LiquidGlass) behind = 2;
    set_macos_window_backdrop(tw->window, behind);
#endif
}

// For the File menu's "New Window/Tab with Preset" submenus (macos_menu.mm).
std::vector<std::string> current_preset_names() {
    if (!g_app_state) return {};
    return config::preset_names(static_cast<AppState*>(g_app_state)->config.config());
}

// Saves what the Settings window changed: each setting that differs from what
// sink.toml says goes into [edits], on top of the active preset. The preset
// itself only changes when "save" is clicked, and "new" makes a preset of the
// lot. Written to disk once the changes stop (ConfigStore).
static void save_config(AppState* state) {
    ConfigStore& store = state->config;
    const config::PresetValues saved = config::resolve_active(store.config());

    std::vector<std::pair<std::string, config::Value>> changes;
    auto changed_str = [&](const char* key, const std::string& now, const std::string& before) {
        if (now != before) changes.push_back({ key, config::Value(now) });
    };
    auto changed_num = [&](const char* key, double now, double before) {
        if (std::fabs(now - before) > 1e-6) changes.push_back({ key, config::Value(now) });
    };
    auto changed_bool = [&](const char* key, bool now, bool before) {
        if (now != before) changes.push_back({ key, config::Value(now) });
    };
    changed_str("background", state->background, saved.background);
    changed_str("font", font_setting(state->font_path),
                config::is_path(saved.font) ? saved.font : font_setting(resolve_font(saved.font)));
    changed_num("exposure", state->exposure, saved.exposure);
    changed_num("hue_shift", state->hue_shift, saved.hue_shift);
    changed_bool("typing_animation", state->animated_typing, saved.typing_animation);
    changed_bool("title_bar", state->vibrancy_enabled, saved.title_bar);
    changed_bool("crt", state->crt_mode_enabled, saved.crt);
    changed_bool("ligatures", state->ligatures_enabled, saved.ligatures);
    changed_bool("hdr_text", state->hdr_console_enabled, saved.hdr_text);
    changed_str("theme", state->theme_name, saved.theme);
    if (changes.empty()) return;

    for (const auto& [key, value] : changes) store.set_edit(key, value);
    refresh_settings_ui_presets(state);  // "(edited)", and whether "save" can be clicked
}

// Copies a resolved preset into AppState's live settings (windows are
// updated separately, by apply_preset_to_state_and_windows).
static void set_state_from_preset(AppState* state, const config::PresetValues& p) {
    state->active_preset_name = p.name;
    state->background = p.background;
    state->video_path = backdrop_of(p.background) == Backdrop::Video ? resolve_background(p.background) : std::string();
    state->font_path = resolve_font(p.font);
    state->base_font_size = static_cast<float>(p.font_size);
    state->line_height = static_cast<float>(p.line_height);
    state->color_scheme = scheme_for(p);
    state->theme_name = p.theme;
    state->exposure = static_cast<float>(p.exposure);
    state->hue_shift = static_cast<float>(p.hue_shift);
    state->animated_typing = p.typing_animation;
    state->vibrancy_enabled = p.title_bar;
    state->crt_mode_enabled = p.crt;
    state->ligatures_enabled = p.ligatures;
    state->hdr_console_enabled = p.hdr_text;
}

// Puts each action's key from [keybindings] on its menu item, so the menus
// show what the keys actually do. Before the menus exist this does nothing;
// SDL_AppInit calls it again once they do.
static void apply_menu_shortcuts(AppState* state) {
#if defined(__APPLE__)
    if (!state->menus_ready) return;
    for (const keybindings::Action& a : keybindings::actions()) {
        std::optional<keybindings::Chord> c = keybindings::key_for(state->bindings, a.name);
        if (c) set_menu_shortcut(a.name, c->key.c_str(), c->cmd, c->shift, c->opt, c->ctrl);
        else   set_menu_shortcut(a.name, "", false, false, false, false);
    }
#else
    (void)state;
#endif
}

// The settings that aren't part of a preset.
static void apply_global_settings(AppState* state) {
    const config::Config& c = state->config.config();
    PTYBridge::set_shell_integration_enabled(config::global_bool(c, "shell.integration"));
    PTYBridge::ShellSettings shell;
    shell.command = config::expand_home(config::global_string(c, "shell.command"));
    shell.args = config::global_strings(c, "shell.args");
    shell.login = config::global_bool(c, "shell.login");
    shell.env = config::global_table(c, "shell.env");
    shell.working_directory = config::expand_home(config::global_string(c, "shell.working_directory"));
    PTYBridge::set_shell_settings(shell);

    // [keybindings], resolved over the defaults (problems were reported when
    // the file was read). The menus show them once they exist.
    std::vector<keybindings::UserEntry> user;
    for (const auto& [key, value] : config::global_table(c, "keybindings")) {
        keybindings::UserEntry e;
        e.key = key;
        if (!value.empty() && value[0] == config::kKeybindingText) {
            e.text = value.substr(1);
            e.has_text = true;
        } else {
            e.action = value;
        }
        user.push_back(e);
    }
    state->bindings = keybindings::resolve(user);
    apply_menu_shortcuts(state);
    state->option_as_meta = key_bindings::parse_option_as_meta(config::option_as_meta(c));
    SDL_SetHint(SDL_HINT_MAC_OPTION_AS_ALT, key_bindings::option_as_alt_hint(state->option_as_meta));
    state->scrollback_lines = static_cast<int>(config::global_int(c, "terminal.scrollback_lines"));
}

// Reads sink.toml (creating or migrating it as needed) into AppState, before
// any window exists.
static void load_config(AppState* state) {
    state->config.load();
    for (const config::Diagnostic& w : state->config.config().warnings) {
        std::cerr << (w.file.empty() ? std::string("sink.toml") : w.file) << ": line " << w.line << ": " << w.message << std::endl;
    }
    apply_global_settings(state);
    const config::Config& c = state->config.config();
    set_state_from_preset(state, config::resolve_active(c));
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
//
// Except in full screen, where there is no title bar to make room for: macOS
// hides it and slides it down over the content only while the pointer is at
// the top edge, as Terminal.app does. The base used to apply there too,
// leaving a 34pt strip of background above the grid in every full-screen
// window. In full screen the native inset is used alone -- normally 0, and
// anything macOS does keep on screen, like a tab bar, still respected.
static float get_top_offset_pts(TerminalWindow* tw) {
    float base = tw->vibrancy_enabled ? 32.0f : 34.0f;
#if defined(__APPLE__)
    float native = get_native_content_top_inset(tw->window);
    if (is_native_fullscreen(tw->window)) return std::max(0.0f, native);
    if (native > base) return native;
#endif
    return base;
}

// Pixels per point for a window's backing store: what layout's points have to
// be multiplied by to land on the right pixels.
static float window_pixel_density(SDL_Window* window) {
    float density = window ? SDL_GetWindowPixelDensity(window) : 0.0f;
    return density > 0.0f ? density : 1.0f;
}

// The window's font at its current scale, and the cell size that follows.
static void load_window_font(AppState* state, TerminalWindow* tw) {
    const std::string& font = tw->font_path.empty() ? state->font_path : tw->font_path;
    std::string path = font.empty() ? "/System/Library/Fonts/SFNSMono.ttf" : font;
    float font_size_px = tw->font_size * tw->display_scale;
    if (!tw->font_manager.load_font(tw->renderer, path, font_size_px, false) &&
        !tw->font_manager.load_font(tw->renderer, resolve_default_font_path(), font_size_px, false)) {
        tw->font_manager.load_font(tw->renderer, "/System/Library/Fonts/Supplemental/Courier New.ttf", font_size_px, false);
    }
    tw->cell_w = (tw->font_manager.get_cell_width() / tw->display_scale) + 1.0f;
    tw->cell_h = (tw->font_manager.get_cell_height() / tw->display_scale) - 0.8f;
}

static void layout_panes(AppState* state, TerminalWindow* tw, bool apply_grids);

// Follows the window to a display of another density. The scale used to be
// read once, when the window opened, and kept for the whole app: a window
// opened on a Retina screen (2) and then shown on a 1x display -- moved
// there, or put full screen on it -- went on drawing everything at twice the
// size, so only the top-left quarter of the grid fit in the window. Normal
// shell use hid it, since a prompt sits top-left; the demo's full-screen video
// was the first thing to fill the grid.
//
// Checked every frame -- one cheap call -- rather than trusting a particular
// window event to arrive for every way a window can change displays. On a
// change: the font is rasterized again at the new size and the panes are laid
// out again, which resizes the grids and tells the shells.
static void refresh_display_scale(AppState* state, TerminalWindow* tw) {
    float density = window_pixel_density(tw->window);
    if (std::fabs(density - tw->display_scale) < 0.01f) return;
    tw->display_scale = density;
    load_window_font(state, tw);
    layout_panes(state, tw, true);
}

static TerminalWindow* create_terminal_window(AppState* state, SDL_Window* parent_tab_window, const config::PresetValues* preset_override = nullptr) {
    TerminalWindow* tw = new TerminalWindow();
    tw->base_font_size = preset_override ? static_cast<float>(preset_override->font_size) : state->base_font_size;
    tw->font_manager.set_line_height(preset_override ? static_cast<float>(preset_override->line_height)
                                                     : state->line_height);
    tw->font_size = tw->base_font_size;

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

    tw->preset_name = preset_override ? preset_override->name : state->active_preset_name;
    if (preset_override) {
        video_path = resolve_background(preset_override->background);
        typeface_path = resolve_font(preset_override->font);
        exposure = static_cast<float>(preset_override->exposure);
        hue_shift_degrees = static_cast<float>(preset_override->hue_shift);
        animated_typing = preset_override->typing_animation;
        vibrancy_enabled = preset_override->title_bar;
        crt_mode_enabled = preset_override->crt;
        ligatures_enabled = preset_override->ligatures;
        hdr_console_enabled = preset_override->hdr_text;
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
    tw->color_scheme = preset_override ? scheme_for(*preset_override) : state->color_scheme;
    tw->fpane().terminal.set_color_scheme(tw->color_scheme);

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
    tw->background_shader.init(tw->renderer, hdr_console_enabled);
    tw->hue_shift_degrees = hue_shift_degrees;
    tw->crt_shader.init(tw->renderer, hdr_console_enabled);

#if defined(__APPLE__)
    enable_macos_window_vibrancy(tw->window, tw->vibrancy_enabled);
#endif

    float scale = window_pixel_density(tw->window);
    tw->display_scale = scale;

    if (typeface_path.empty()) {
        typeface_path = "/System/Library/Fonts/SFNSMono.ttf";
    }

    float font_size_px = tw->font_size * scale;
    if (!tw->font_manager.load_font(tw->renderer, typeface_path, font_size_px, false) &&
        !tw->font_manager.load_font(tw->renderer, resolve_default_font_path(), font_size_px, false)) {
        tw->font_manager.load_font(tw->renderer, "/System/Library/Fonts/Supplemental/Courier New.ttf", font_size_px, false);
    }
    tw->font_path = typeface_path;

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

    // A new window or tab starts where the focused pane of the window it was
    // opened from is. Empty when there is no such pane -- the first window --
    // or when that shell never reported a directory, in which case the child
    // inherits sink's own and the existing home-directory fallback applies.
    std::string inherit_cwd;
    if (state->active_window) {
        inherit_cwd = state->active_window->fpane().terminal.get_working_directory();
    }

    // Start Pseudo-Terminal process connection
    if (!tw->fpane().pty.spawn(cols, rows, inherit_cwd)) {
        std::cerr << "Failed to initialize PTY shell context" << std::endl;
    }

    SDL_StartTextInput(tw->window);

    // The background: a video, or one of the other kinds.
    {
        config::PresetValues p = preset_override
            ? *preset_override
            : config::resolve_active(state->config.config());
        if (!preset_override) {
            // A video given on the command line overrides the preset's.
            p.background = state->video_path.empty() || backdrop_of(state->background) != Backdrop::Video
                ? state->background : state->video_path;
        }
        apply_background(state, tw, p);
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
    tw->background_shader.cleanup();
    // Same rule for the panes' image textures. Their destructors run below as
    // part of `delete tw`, which is after the renderer is gone -- they would
    // otherwise be destroying textures against a dead device.
    for (Pane* pane : all_panes(tw)) {
        pane->terminal.images().release_textures();
    }
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
        // cell_w/cell_h are in points; the pty and the geometry reports both
        // want real pixels, so scale back up.
        int cell_px_w = static_cast<int>(std::lround(tw->cell_w * tw->display_scale));
        int cell_px_h = static_cast<int>(std::lround(tw->cell_h * tw->display_scale));
        pane->terminal.set_cell_pixel_size(cell_px_w, cell_px_h);

        // How many cells the display this window is on would hold, for the
        // XTWINOPS screen-size report. Display bounds come back in the same
        // points cell_w/cell_h are in, so no scaling is involved. A program
        // asking wants to know how large it could grow, so this is the whole
        // display rather than the usable area -- and it is the display the
        // window is actually on, not the primary one.
        SDL_Rect display_bounds;
        if (tw->cell_w > 0.0f && tw->cell_h > 0.0f &&
            SDL_GetDisplayBounds(SDL_GetDisplayForWindow(tw->window), &display_bounds)) {
            pane->terminal.set_screen_size_chars(
                static_cast<int>(display_bounds.w / tw->cell_w),
                static_cast<int>(display_bounds.h / tw->cell_h));
        }
        if (cols != pane->terminal.get_cols() || rows != pane->terminal.get_rows()) {
            std::lock_guard<std::mutex> lock(pane->grid_mutex);
            // While the shell is editing a line, it is told about the new
            // size only once the size stops changing. It redraws by climbing
            // the height its line had at the size it last drew, which the
            // grid's reflow preserves (see TerminalGrid::resize) -- but a
            // drag delivers a size a frame, and a shell answering one of
            // them after the grid has moved on to the next draws for a width
            // the screen no longer has. Its idea of the line's height then
            // disagrees with the screen, and the next redraw climbs into the
            // output above and clears it. Full-screen apps and running
            // commands repaint from scratch and get every size at once.
            int start_row = 0, start_col = 0;
            bool at_command_line = pane->terminal.input_start(start_row, start_col);
            pane->terminal.resize(cols, rows);
            if (at_command_line) {
                pane->pty_resize_pending = true;
                pane->pending_pty_cols = cols;
                pane->pending_pty_rows = rows;
                pane->pending_pty_px_w = cell_px_w;
                pane->pending_pty_px_h = cell_px_h;
                pane->pending_pty_since = SDL_GetTicks();
            } else {
                pane->pty_resize_pending = false;
                pane->pty.resize_pty(cols, rows, cell_px_w, cell_px_h);
            }
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

static PaneNode* find_leaf(PaneNode* node, Pane* pane);

static void layout_panes(AppState* state, TerminalWindow* tw, bool apply_grids = true) {
    int w = 0, h = 0;
    SDL_GetWindowSize(tw->window, &w, &h);
    float top = get_top_offset_pts(tw);
    SDL_FRect whole = { 0.0f, top, static_cast<float>(w), static_cast<float>(h) - top };
    PaneNode* zoomed_leaf = tw->zoomed ? find_leaf(tw->root.get(), tw->zoomed) : nullptr;
    if (tw->zoomed && !zoomed_leaf) tw->zoomed = nullptr; // closed out from under the zoom
    layout_pane_node(state, tw, zoomed_leaf ? zoomed_leaf : tw->root.get(), whole, apply_grids);
}

// Whether a pane is on screen: all of them, unless one is zoomed.
static bool pane_visible(const TerminalWindow* tw, const Pane* pane) {
    return !tw->zoomed || tw->zoomed == pane;
}

static Pane* pane_at(TerminalWindow* tw, float x, float y) {
    for (Pane* pane : all_panes(tw)) {
        if (!pane_visible(tw, pane)) continue;
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
    tw->zoomed = nullptr;
    PaneNode* leaf = find_leaf(tw->root.get(), tw->focused);
    if (!leaf) return;

    // Start the new shell where the one being split is, if that shell
    // reported it (OSC 7). Splitting a pane to run something alongside what
    // is already there is the case this exists for.
    std::string inherit_cwd = leaf->pane->terminal.get_working_directory();

    auto new_pane = std::make_unique<Pane>();
    new_pane->terminal.set_enable_ligatures(state->ligatures_enabled);
    new_pane->terminal.set_max_scrollback(static_cast<size_t>(state->scrollback_lines));
    new_pane->terminal.set_color_scheme(tw->color_scheme);

    // Seed the new grid/PTY at roughly half the old pane; layout_panes
    // recomputes the exact geometry right after
    int cols = std::max(20, vertical ? leaf->pane->terminal.get_cols() / 2
                                     : leaf->pane->terminal.get_cols());
    int rows = std::max(5, vertical ? leaf->pane->terminal.get_rows()
                                    : leaf->pane->terminal.get_rows() / 2);
    new_pane->terminal.resize(cols, rows);
    new_pane->terminal.clear_screen();
    if (!new_pane->pty.spawn(cols, rows, inherit_cwd)) {
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
    tw->zoomed = nullptr;

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

// Puts a zoomed pane back among the others.
static void unzoom_pane(AppState* state, TerminalWindow* tw) {
    if (!tw->zoomed) return;
    tw->zoomed = nullptr;
    layout_panes(state, tw);
}

// ⇧⌘↩: the focused pane fills the window, or goes back to its place. A
// window with one pane has nothing to zoom.
static void toggle_zoom_pane(AppState* state, TerminalWindow* tw) {
    if (tw->zoomed) {
        unzoom_pane(state, tw);
    } else if (!tw->root->is_leaf()) {
        tw->zoomed = tw->focused;
        layout_panes(state, tw);
    }
}

// ⌘] and ⌘[: focus the next or previous pane in reading order (the split
// tree's order), wrapping at either end.
static void cycle_pane_focus(AppState* state, TerminalWindow* tw, int step) {
    unzoom_pane(state, tw);
    std::vector<Pane*> panes = all_panes(tw);
    if (panes.size() < 2) return;
    auto it = std::find(panes.begin(), panes.end(), tw->focused);
    int at = (it == panes.end()) ? 0 : static_cast<int>(it - panes.begin());
    int n = static_cast<int>(panes.size());
    tw->focused = panes[((at + step) % n + n) % n];
}

// View > Bigger, Smaller, Default Size. Steps a point at a time, within
// sizes that still make sense, and reflows the window's panes to match.
static constexpr float kMinFontSize = 6.0f;
static constexpr float kMaxFontSize = 72.0f;
static void set_window_font_size(AppState* state, TerminalWindow* tw, float size) {
    size = std::clamp(size, kMinFontSize, kMaxFontSize);
    if (size == tw->font_size) return;
    tw->font_size = size;
    load_window_font(state, tw);
    layout_panes(state, tw);
}

// ⌘K: clear the focused pane's screen and scrollback. At a shell prompt, the
// shell is then sent ^L, which redraws the prompt -- and whatever had been
// typed -- at the top; with a command running nothing is sent, since ^L would
// be input to the command. A full-screen program's screen is its own to
// clear, so on the alternate screen this does nothing.
static void clear_screen_and_scrollback(TerminalWindow* tw) {
    Pane& pane = tw->fpane();
    bool at_prompt = false;
    {
        std::lock_guard<std::mutex> lock(pane.grid_mutex);
        if (pane.terminal.is_alt_screen_active()) return;
        int row = 0, col = 0;
        at_prompt = pane.terminal.input_start(row, col);
        pane.terminal.clear_screen();
        pane.terminal.clear_scrollback();
    }
    if (at_prompt) {
        const char form_feed = '\x0c';
        pane.pty.write_to_pty(&form_feed, 1);
    }
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
    bool degraded = false; // set if the requested colorspace had to be dropped
    bool had_video = tw->has_video;
    std::string video_path_to_reopen = tw->video_path.empty() ? state->video_path : tw->video_path;

    // Order matters: everything that owns a texture/shader-state against
    // the current renderer must be torn down *before* that renderer is
    // destroyed, or they're left holding pointers into a dead GPU device.
    tw->video_engine.close_video(); // also stops decode; see close_video()
    tw->has_video = false;
    tw->hue_shift.cleanup();
    tw->crt_shader.cleanup();
    tw->background_shader.cleanup();
    tw->font_manager.cleanup();
    // Inline images hold textures too. Only the textures go: the decoded
    // pixels stay, so every image re-uploads against the new renderer the
    // next time it is drawn and nothing on screen is lost to the toggle.
    for (Pane* pane : all_panes(tw)) {
        pane->terminal.images().release_textures();
    }

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

    // Returning here with tw->renderer left null would be fatal in a way
    // that doesn't look fatal: every SDL_Render* call in the frame loop
    // silently no-ops on a null renderer, so the app keeps running, keeps
    // reading the shell, and keeps handling input behind a window that
    // never draws again. The extended-colorspace renderer is the one that
    // can plausibly fail (see create_terminal_window's colorspace
    // comment), so fall back to the plain one and report the toggle as
    // failed rather than leave the window unable to present at all.
    if (!tw->renderer && want_linear_colorspace) {
        std::cerr << "recreate_renderer_for_hdr_console: extended-colorspace SDL_CreateRenderer failed: "
                  << SDL_GetError() << " -- falling back to the standard colorspace (hdr console off)" << std::endl;
        want_linear_colorspace = false;
        tw->hdr_console_enabled = false;
        degraded = true;
        tw->renderer = SDL_CreateRenderer(tw->window, SDL_GPU_RENDERER);
    }

    if (!tw->renderer) {
        std::cerr << "recreate_renderer_for_hdr_console: SDL_CreateRenderer failed: " << SDL_GetError() << std::endl;
        return false;
    }

    SDL_SetRenderVSync(tw->renderer, 1);
    tw->hue_shift.init(tw->renderer, want_linear_colorspace);
    tw->background_shader.init(tw->renderer, want_linear_colorspace);
    tw->crt_shader.init(tw->renderer, want_linear_colorspace);

    float font_size_px = tw->font_size * tw->display_scale;
    const std::string& font = tw->font_path.empty() ? state->font_path : tw->font_path;
    if (!tw->font_manager.load_font(tw->renderer, font, font_size_px, false)) {
        tw->font_manager.load_font(tw->renderer, "/System/Library/Fonts/Supplemental/Courier New.ttf", font_size_px, false);
    }
    tw->cell_w = (tw->font_manager.get_cell_width() / tw->display_scale) + 1.0f;
    tw->cell_h = (tw->font_manager.get_cell_height() / tw->display_scale) - 0.8f;

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

    // False if the fallback above had to drop the requested colorspace --
    // the window is drawable, but hdr console is not actually on.
    return !degraded;
}

// Gives one window a preset's look. The video and font are reopened only
// when they change: a live reload that touched exposure shouldn't restart
// the video or relayout the panes.
static void apply_preset_to_window(AppState* state, TerminalWindow* tw, const config::PresetValues& p) {
    tw->preset_name = p.name;
    const std::string font = resolve_font(p.font);

    tw->color_scheme = scheme_for(p);
    apply_background(state, tw, p);

    // A changed font_size is the new base for this window, replacing any
    // View > Bigger/Smaller.
    const bool size_changed = tw->base_font_size != static_cast<float>(p.font_size);
    if (size_changed) tw->base_font_size = tw->font_size = static_cast<float>(p.font_size);
    const bool spacing_changed = tw->font_manager.line_height() != static_cast<float>(p.line_height);
    tw->font_manager.set_line_height(static_cast<float>(p.line_height));
    if (tw->font_path != font || size_changed || spacing_changed) {
        tw->font_path = font;
        if (tw->font_manager.load_font(tw->renderer, font, tw->font_size * tw->display_scale, false)) {
            tw->cell_w = (tw->font_manager.get_cell_width() / tw->display_scale) + 1.0f;
            tw->cell_h = (tw->font_manager.get_cell_height() / tw->display_scale) - 0.8f;
            layout_panes(state, tw);
        }
    }

    tw->exposure = static_cast<float>(p.exposure);
    tw->hue_shift_degrees = static_cast<float>(p.hue_shift);
    tw->animated_typing = p.typing_animation;
    tw->vibrancy_enabled = p.title_bar;
    tw->crt_mode_enabled = p.crt;
    for (Pane* pane : all_panes(tw)) {
        pane->terminal.set_enable_ligatures(p.ligatures);
        pane->terminal.set_max_scrollback(static_cast<size_t>(state->scrollback_lines));
        pane->terminal.set_color_scheme(tw->color_scheme);
    }
    if (tw->hdr_console_enabled != p.hdr_text) {
        tw->hdr_console_enabled = p.hdr_text;
        // This window's renderer colorspace no longer matches -- rebuild it
        // (which also reopens its video and font, against the new renderer).
        recreate_renderer_for_hdr_console(state, tw, tw->hdr_console_enabled);
    }
#if defined(__APPLE__)
    enable_macos_window_vibrancy(tw->window, p.title_bar);
#endif
}

// Switching presets in Settings: the preset becomes the active one and every
// window takes it on, as before.
static void apply_preset_to_state_and_windows(AppState* state, const config::PresetValues& p) {
    set_state_from_preset(state, p);
    for (auto* tw : state->windows) apply_preset_to_window(state, tw, p);
}

// Pushes the current preset list/selection into the (already-open) settings
// window so its preset row and dropdown reflect what's on disk.
static void refresh_settings_ui_presets(AppState* state) {
    state->settings_ui.set_preset_names(config::preset_names(state->config.config()));
    std::vector<std::string> builtins;
    for (const config::BuiltinPreset& b : config::builtin_presets()) builtins.push_back(b.name);
    state->settings_ui.set_builtin_presets(builtins);
    // Presets only an included file has: Settings can change them (into
    // sink.toml) but not rename or delete them.
    std::vector<std::string> included;
    for (const std::string& name : config::preset_names(state->config.config())) {
        if (!config::is_builtin(name) && !state->config.can_rename_or_delete(name)) included.push_back(name);
    }
    state->settings_ui.set_included_presets(included);
    state->settings_ui.set_active_preset(state->active_preset_name);
    state->settings_ui.set_preset_edited(!state->config.config().edits.empty());
}

// Re-syncs every settings-window control from AppState, used after a preset
// switch, new, save or delete replaces the live settings wholesale.
static void sync_settings_ui_from_state(AppState* state) {
    state->settings_ui.set_paths(background_display(state), state->font_path);
    state->settings_ui.set_animated_typing(state->animated_typing);
    state->settings_ui.set_exposure(state->exposure);
    state->settings_ui.set_hue_shift(state->hue_shift);
    state->settings_ui.set_vibrancy_enabled(state->vibrancy_enabled);
    state->settings_ui.set_crt_effect_enabled(state->crt_mode_enabled);
    state->settings_ui.set_ligatures_enabled(state->ligatures_enabled);
    state->settings_ui.set_hdr_console_enabled(state->hdr_console_enabled);
    state->settings_ui.set_themes(themes::names(), state->theme_name);
    state->settings_ui.set_shell_integration_enabled(config::global_bool(state->config.config(), "shell.integration"));
    refresh_settings_ui_presets(state);
}

// One line in Settings about sink.toml: a syntax error (which also stops
// saving), else the first warning, else a note that it had to be rewritten.
static void update_config_notice(AppState* state) {
    const ConfigStore& store = state->config;
    auto clean = [](std::string text) {
        for (char& ch : text) {
            if (static_cast<unsigned char>(ch) < 0x20) ch = ' ';
        }
        return text;
    };
    std::string notice;
    if (const auto& err = store.error()) {
        notice = "sink.toml line " + std::to_string(err->line) + ": " + clean(err->message) +
                 " -- changes won't be saved until it's fixed";
    } else if (const std::string font = config::resolve_active(store.config()).font;
               !config::is_path(font) && !is_bundled_font_name(font) && !FontManager::can_open(font)) {
        notice = "font \"" + font + "\" isn't installed; using Monaspace Neon";
    } else if (const auto& warnings = store.config().warnings; !warnings.empty()) {
        const config::Diagnostic& w = warnings.front();
        const std::string file = w.file.empty() ? std::string("sink.toml") : w.file.substr(w.file.rfind('/') + 1);
        notice = file + " line " + std::to_string(w.line) + ": " + clean(w.message);
        if (warnings.size() > 1) notice += " (+" + std::to_string(warnings.size() - 1) + " more)";
    } else if (store.regenerated()) {
        notice = "sink.toml was rewritten in full; your previous version is in sink.toml.bak";
    }
    state->settings_ui.set_config_notice(notice);
}

// Live reload: about once a second, picks up edits made to sink.toml outside
// sink. Each window follows its own preset; windows that were on the active
// one follow the file's `preset` if that changed.
static void reload_config_if_changed(AppState* state) {
    static Uint64 last_check = 0;
    const Uint64 now = SDL_GetTicks();
    if (now - last_check < 1000) return;
    last_check = now;

    if (state->config.reload_if_changed()) {
        const config::Config& c = state->config.config();
        for (const config::Diagnostic& w : c.warnings) {
            std::cerr << (w.file.empty() ? std::string("sink.toml") : w.file) << ": line " << w.line << ": " << w.message << std::endl;
        }
        const std::string previous_active = state->active_preset_name;
        const std::string active = config::active_preset(c);
        apply_global_settings(state);
        set_state_from_preset(state, config::resolve_active(c));
        for (auto* tw : state->windows) {
            std::string name = tw->preset_name;
            if (name == previous_active || !config::preset_exists(c, name)) name = active;
            apply_preset_to_window(state, tw, name == active ? config::resolve_active(c) : config::resolve_preset(c, name));
        }
        sync_settings_ui_from_state(state);
    }
    update_config_notice(state);
}

// ---- Command line -------------------------------------------------------------
//
// Options for working with sink.toml, answered on stdout without opening a
// window. They read the file but never write it: a file in the old format is
// converted in memory only.

static void print_usage(std::ostream& out) {
    out << "usage: sink [options] [video]\n"
           "\n"
           "  video                   play this file behind the terminal\n"
           "\n"
           "  --show-config           print every setting as sink reads your sink.toml\n"
           "  --show-config --default print every setting at its default, documented\n"
           "  --check-config          report problems in sink.toml; exits 1 if there are any\n"
           "  --print-schema          print the JSON schema for sink.toml\n"
           "  --version               print sink's version\n"
           "  --help                  print this\n"
           "\n"
           "sink.toml is " << ConfigStore::default_path() << " (set SINK_CONFIG to use another file).\n";
}

// The config file as sink would read it, or nullopt (after reporting why) on
// a syntax error.
static std::optional<config::Config> read_config_for_command_line(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::string text;
    if (f.is_open()) {
        std::stringstream ss;
        ss << f.rdbuf();
        text = ss.str();
    } else {
        std::cerr << "no file at " << path << "; showing the defaults" << std::endl;
    }
    if (config::is_v0(text)) {
        std::cerr << path << " is in the format sink 0.11 and earlier used; it's converted when sink next starts"
                  << std::endl;
        if (auto converted = config::migrate_v0(text)) text = *converted;
    }
    config::Diagnostic err;
    std::optional<config::Config> c = config::read_with_includes(text, path, err, [](const std::string& file) {
        std::ifstream in(file, std::ios::binary);
        if (!in.is_open()) return std::optional<std::string>();
        std::stringstream contents;
        contents << in.rdbuf();
        return std::optional<std::string>(contents.str());
    });
    if (!c) std::cerr << path << ":" << err.line << ": " << err.message << std::endl;
    return c;
}

// Handles command-line options. Returns the exit code when sink should exit
// instead of starting, nullopt to start normally.
static std::optional<int> run_command_line(int argc, char* argv[]) {
    std::vector<std::string> args(argv + 1, argv + argc);
    auto has = [&](const char* flag) { return std::find(args.begin(), args.end(), flag) != args.end(); };
    for (const std::string& a : args) {
        static const char* known[] = { "--show-config", "--default", "--check-config", "--print-schema",
                                       "--version", "--help", "-h" };
        if (a.size() > 1 && a[0] == '-' &&
            std::find_if(std::begin(known), std::end(known), [&](const char* k) { return a == k; }) ==
                std::end(known)) {
            std::cerr << "sink: unknown option " << a << "\n\n";
            print_usage(std::cerr);
            return 2;
        }
    }

    if (has("--help") || has("-h")) {
        print_usage(std::cout);
        return 0;
    }
    if (has("--version")) {
        std::cout << "sink " << SINK_VERSION << std::endl;
        return 0;
    }
    if (has("--print-schema")) {
        std::cout << config::json_schema();
        return 0;
    }
    if (has("--show-config") && has("--default")) {
        std::cout << config::reference_toml();
        return 0;
    }
    const std::string path = ConfigStore::default_path();
    if (has("--show-config")) {
        std::optional<config::Config> c = read_config_for_command_line(path);
        if (!c) return 1;
        std::cout << config::effective_toml(*c);
        return 0;
    }
    if (has("--check-config")) {
        std::optional<config::Config> c = read_config_for_command_line(path);
        if (!c) return 1;
        for (const config::Diagnostic& w : c->warnings) {
            std::cout << (w.file.empty() ? path : w.file) << ":" << w.line << ": " << w.message << "\n";
        }
        if (c->warnings.empty()) std::cout << path << ": OK" << std::endl;
        return c->warnings.empty() ? 0 : 1;
    }

    // Anything else is the video to play behind the terminal, handled once
    // the app has started.
    return std::nullopt;
}

// ---- Keyboard shortcuts ------------------------------------------------------

// A key press as [keybindings] names keys. The key is the unshifted one, so
// shift+cmd+d is "d" with shift, not "D".
static keybindings::Chord chord_of(const SDL_KeyboardEvent& key) {
    keybindings::Chord c;
    c.cmd = (key.mod & SDL_KMOD_GUI) != 0;
    c.shift = (key.mod & SDL_KMOD_SHIFT) != 0;
    c.opt = (key.mod & SDL_KMOD_ALT) != 0;
    c.ctrl = (key.mod & SDL_KMOD_CTRL) != 0;
    const SDL_Keycode k = SDL_GetKeyFromScancode(key.scancode, SDL_KMOD_NONE, false);
    switch (k) {
        case SDLK_UP: c.key = "up"; break;
        case SDLK_DOWN: c.key = "down"; break;
        case SDLK_LEFT: c.key = "left"; break;
        case SDLK_RIGHT: c.key = "right"; break;
        case SDLK_RETURN: case SDLK_KP_ENTER: c.key = "return"; break;
        case SDLK_TAB: c.key = "tab"; break;
        case SDLK_SPACE: c.key = "space"; break;
        case SDLK_ESCAPE: c.key = "escape"; break;
        case SDLK_BACKSPACE: c.key = "delete"; break;
        case SDLK_DELETE: c.key = "forward_delete"; break;
        case SDLK_HOME: c.key = "home"; break;
        case SDLK_END: c.key = "end"; break;
        case SDLK_PAGEUP: c.key = "page_up"; break;
        case SDLK_PAGEDOWN: c.key = "page_down"; break;
        default:
            if (k >= SDLK_F1 && k <= SDLK_F12) c.key = "f" + std::to_string(k - SDLK_F1 + 1);
            else if (k >= SDLK_F13 && k <= SDLK_F20) c.key = "f" + std::to_string(k - SDLK_F13 + 13);
            else if (k > 32 && k < 127) c.key = std::string(1, static_cast<char>(std::tolower(static_cast<int>(k))));
            break;
    }
    return c;
}

static void open_settings_window(AppState* state, TerminalWindow* tw) {
    if (!tw || !state->settings_ui.open(tw->window)) return;
    state->settings_ui.set_paths(background_display(state), state->font_path);
    state->settings_ui.set_animated_typing(tw->animated_typing);
    state->settings_ui.set_exposure(tw->exposure);
    state->settings_ui.set_hue_shift(tw->hue_shift_degrees);
    state->settings_ui.set_broadcasting(state->input_broadcasting);
    state->settings_ui.set_vibrancy_enabled(tw->vibrancy_enabled);
    state->settings_ui.set_crt_effect_enabled(tw->crt_mode_enabled);
    state->settings_ui.set_hdr_console_enabled(tw->hdr_console_enabled);
    state->settings_ui.set_themes(themes::names(), state->theme_name);
    state->settings_ui.set_shell_integration_enabled(config::global_bool(state->config.config(), "shell.integration"));
    refresh_settings_ui_presets(state);
}

static void toggle_search(TerminalWindow* tw) {
    tw->search_drawer_open = !tw->search_drawer_open;
    tw->fpane().terminal.set_search_active(tw->search_drawer_open);
    if (tw->search_drawer_open) tw->fpane().terminal.set_search_query(tw->search_input_text);
}

// Runs what a key is bound to: text goes to the shell (to every pane while
// broadcasting); an action does what its menu item does, through the same
// requests where there is one.
static void run_binding(AppState* state, TerminalWindow* tw, const keybindings::Binding& b) {
    if (!tw) return;
    if (b.action.empty()) {
        std::vector<Pane*> targets;
        if (state->input_broadcasting) {
            for (auto* w : state->windows) for (Pane* p : all_panes(w)) targets.push_back(p);
        } else {
            targets.push_back(&tw->fpane());
        }
        for (Pane* p : targets) {
            p->terminal.note_user_edit();
            p->pty.write_to_pty(b.text.data(), b.text.size());
        }
        return;
    }
    const std::string& a = b.action;
    if (a == "settings") {
        if (state->settings_ui.is_open()) state->settings_ui.close();
        else open_settings_window(state, tw);
    }
    else if (a == "new_window") set_new_window_requested(true);
    else if (a == "new_tab") set_new_tab_requested(true);
    else if (a == "close_tab") set_close_window_requested(true);
    else if (a == "print") set_print_requested(true);
    else if (a == "cut") set_cut_requested(true);
    else if (a == "copy") set_copy_requested(true);
    else if (a == "paste") set_paste_requested(true);
    else if (a == "select_all") set_select_all_requested(true);
    else if (a == "find") toggle_search(tw);
    else if (a == "clear_screen") set_clear_screen_requested(true);
    else if (a == "font_bigger") set_font_size_requested(1);
    else if (a == "font_smaller") set_font_size_requested(-1);
    else if (a == "font_default") set_font_size_requested(2);
    else if (a == "previous_prompt" || a == "next_prompt") {
        std::lock_guard<std::mutex> lock(tw->fpane().grid_mutex);
        if (a == "previous_prompt") tw->fpane().terminal.scroll_to_prev_prompt();
        else tw->fpane().terminal.scroll_to_next_prompt();
    }
    else if (a == "split_right") set_split_pane_right_requested(true);
    else if (a == "split_down") set_split_pane_down_requested(true);
    else if (a == "close_pane") set_close_pane_requested(true);
    else if (a == "select_pane_left") set_pane_focus_requested(1);
    else if (a == "select_pane_right") set_pane_focus_requested(2);
    else if (a == "select_pane_up") set_pane_focus_requested(3);
    else if (a == "select_pane_down") set_pane_focus_requested(4);
    else if (a == "next_pane") set_pane_cycle_requested(1);
    else if (a == "previous_pane") set_pane_cycle_requested(-1);
    else if (a == "zoom_pane") set_zoom_pane_requested(true);
    else if (a == "broadcast_input") set_toggle_broadcast_requested(true);
    else if (a == "select_last_tab") select_native_tab(tw->window, 9);
    else if (a.compare(0, 11, "select_tab_") == 0) select_native_tab(tw->window, std::atoi(a.c_str() + 11));
}

// SDL3 Application initialization entry point
SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[]) {
    // Before SDL starts, so `sink --show-config` opens no window and never
    // touches sink.toml.
    if (std::optional<int> code = run_command_line(argc, argv)) {
        std::cout.flush();
        std::exit(*code);
    }

    SDL_SetAppMetadata("sink", SINK_VERSION, "com.rainmultimedia.sink");
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
    FontManager::set_bundled_fallback_path(resolve_bundled_font_path());
    FontManager::set_font_lookup([](const std::string& name) -> std::optional<FontManager::NamedFont> {
        std::optional<system_fonts::Found> found = system_fonts::find(name);
        if (!found) return std::nullopt;
        return FontManager::NamedFont{ found->path, found->family, found->style };
    });

    // Load config if present (may override the defaults above with the
    // active preset's saved background/font/etc)
    load_config(state);

    if (argc > 1) {
        state->video_path = argv[1];
    }

    // Spawn first window
    TerminalWindow* tw = create_terminal_window(state, nullptr);
    if (!tw) {
        // SDL_AppQuit frees the state: SDL passes it *appstate either way,
        // and freeing it here too crashed.
        return SDL_APP_FAILURE;
    }
    state->windows.push_back(tw);
    state->active_window = tw;
    state->last_tick = SDL_GetTicks();

#if defined(__APPLE__)
    setup_macos_menu();
    state->menus_ready = true;
    apply_menu_shortcuts(state);
#endif

    return SDL_APP_CONTINUE;
}

// Keys that only modify other keys, and send nothing on their own.
static bool is_modifier_key(SDL_Keycode sym) {
    switch (sym) {
        case SDLK_LSHIFT: case SDLK_RSHIFT:
        case SDLK_LCTRL:  case SDLK_RCTRL:
        case SDLK_LALT:   case SDLK_RALT:
        case SDLK_LGUI:   case SDLK_RGUI:
        case SDLK_CAPSLOCK: case SDLK_MODE:
            return true;
        default:
            return false;
    }
}

static bool delete_selection_in_prompt(TerminalWindow* tw) {
    if (!tw) return false;
    std::string payload;
    if (!prompt_edit::delete_selection_payload(tw->fpane().terminal, payload)) {
        return false;
    }
    if (!payload.empty()) {
        tw->fpane().pty.write_to_pty(payload.data(), payload.size());
        tw->fpane().terminal.note_user_edit();
    }
    tw->fpane().terminal.clear_selection();
    return true;
}

static void set_input_broadcasting(AppState* state, bool on);

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
        open_settings_window(state, state->active_window);
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

    if (event->type == SDL_EVENT_WINDOW_FOCUS_GAINED ||
        event->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        bool gained = (event->type == SDL_EVENT_WINDOW_FOCUS_GAINED);
        SDL_WindowID focus_win_id = event->window.windowID;
        for (auto* tw : state->windows) {
            if (!tw->window || SDL_GetWindowID(tw->window) != focus_win_id) continue;
            if (gained) state->active_window = tw;
            // Focus reporting (DECSET 1004): CSI I on gain, CSI O on loss.
            // Sent per pane, since each runs its own program and enables the
            // mode independently.
            for (Pane* pane : all_panes(tw)) {
                if (pane && pane->terminal.is_focus_reporting()) {
                    const char* seq = gained ? "\x1b[I" : "\x1b[O";
                    pane->pty.write_to_pty(seq, 3);
                }
            }
            break;
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
            if (PaneNode* node = divider_at(target_tw->zoomed ? nullptr : target_tw->root.get(), event->button.x, event->button.y)) {
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
                // Rects (divider position, clipping) track the mouse at once;
                // the grids follow through the same throttled live layout a
                // window resize uses (see has_pending_resize).
                //
                // Grid reflow used to wait for mouse-up, because each pause
                // in a drag sent the shell another SIGWINCH before its last
                // redraw had settled and the fragments piled up. The shell is
                // now told the size only once it holds (layout_pane_node), and
                // the reflow keeps the cursor where its redraw will look, so
                // the panes can rewrap as the divider moves.
                layout_panes(state, target_tw, false);
                target_tw->has_pending_resize = true;
                target_tw->last_resize_event_time = SDL_GetTicks();
                return SDL_APP_CONTINUE;
            }

            PaneNode* hover = divider_at(target_tw->zoomed ? nullptr : target_tw->root.get(), event->motion.x, event->motion.y);
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
            // The row this click resolves to is computed from scroll_offset_,
            // so land the view on it before doing so -- otherwise a click that
            // interrupts a fling selects the row the scroll was heading for
            // rather than the one under the pointer.
            target_tw->fpane().terminal.snap_scroll_view();
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
                    std::string move_payload =
                        prompt_edit::click_payload(target_tw->fpane().terminal, row, col);
                    if (!move_payload.empty()) {
                        target_tw->fpane().pty.write_to_pty(move_payload.data(), move_payload.size());
                        target_tw->fpane().terminal.note_user_edit();
                        target_tw->fpane().terminal.clear_selection();
                    }
                }
                target_tw->fpane().terminal.end_selection();
            }
        }
    } else if (event->type == SDL_EVENT_TEXT_INPUT) {
        if (state->suppress_next_text_input) {
            // The key that produced this was already sent as an SS3 keypad
            // sequence; see app_keypad_final().
            state->suppress_next_text_input = false;
            return SDL_APP_CONTINUE;
        }
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

        // Text input belongs to the key press before it. Suppression armed
        // for a key that then typed nothing must not swallow the next key's.
        state->suppress_next_text_input = false;

        // [keybindings]: every shortcut sink has, as the user has set them.
        // Menu items do nothing when fired by their own key (macos_menu.mm),
        // so this is the only place a shortcut runs.
        if (const keybindings::Binding* b = keybindings::find(state->bindings, chord_of(event->key))) {
            const bool settings_focused =
                state->settings_ui.is_open() && SDL_GetKeyboardFocus() == state->settings_ui.get_window();
            if (b->action == "settings" || !settings_focused) {
                run_binding(state, target_tw, *b);
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
                    // Drop a whole character, not a byte. SDL delivers text
                    // input as UTF-8, so pop_back() on anything non-ASCII left
                    // a dangling continuation byte in the query -- the search
                    // then matched nothing and kept doing so for every further
                    // keystroke, since every later query still carried the
                    // broken prefix.
                    std::string& q = target_tw->search_input_text;
                    q.pop_back();
                    while (!q.empty() &&
                           (static_cast<unsigned char>(q.back()) & 0xC0) == 0x80) {
                        q.pop_back();
                    }
                    target_tw->fpane().terminal.set_search_query(q);
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

        // Ignore input if settings has focus
        if (state->settings_ui.is_open() && SDL_GetKeyboardFocus() == state->settings_ui.get_window()) {
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
            // Tell the grid what this key does to the line being edited (see
            // prompt_editing.hpp). Modifier keys and Cmd shortcuts send
            // nothing to the shell, so they don't count -- pressing Shift
            // while a command runs must not pin the input start mid-output.
            //
            // Ctrl-D only ends the line when the line is empty -- that is EOF,
            // which exits the shell or ends input to cat or python. With text
            // on the line it is delete-char in zsh and readline, an edit like
            // any other, and treating it as the end would let the next pause
            // in output re-anchor the line mid-command.
            //
            // The submitted line's text is kept: the built-in commands below
            // check it, and once the line is submitted the grid no longer
            // knows where it was.
            std::string submitted_line;
            if (!is_modifier_key(sym) && !(mod & SDL_KMOD_GUI)) {
                bool ctrl = (mod & SDL_KMOD_CTRL) != 0;
                bool submit = sym == SDLK_RETURN || sym == SDLK_KP_ENTER ||
                              (ctrl && sym == SDLK_C) ||
                              (ctrl && sym == SDLK_D &&
                               prompt_edit::current_line_text(tw->fpane().terminal).empty());
                if (submit) submitted_line = prompt_edit::submit_line(tw->fpane().terminal);
                else        tw->fpane().terminal.note_user_edit();
            }
            // Cmd+X only. Ctrl+X goes to the shell: it is how nano exits, and
            // the start of emacs-style chords like zsh and bash's ^X^E.
            if ((mod & SDL_KMOD_GUI) && sym == SDLK_X) {
                perform_cut_action(tw);
                return SDL_APP_CONTINUE;
            }
            if (!(mod & SDL_KMOD_GUI) && sym != SDLK_BACKSPACE && sym != SDLK_DELETE) {
                tw->fpane().terminal.clear_selection();
            }
            tw->fpane().terminal.reset_scroll();
            tw->fpane().scroll_velocity = 0.0f;
            tw->fpane().scroll_accumulator = 0.0f;

            // Application keypad. Only reached when an app has actually asked
            // for it, so ordinary typing never comes near this.
            if (tw->fpane().terminal.is_app_keypad()) {
                if (char final_byte = app_keypad_final(sym)) {
                    char seq[3] = { '\x1b', 'O', final_byte };
                    tw->fpane().pty.write_to_pty(seq, 3);
                    // SDL follows a keypad press with a text-input event
                    // carrying the digit. Both would reach the shell, so the
                    // text half is dropped -- scoped to the very next event,
                    // and only ever armed on a keypad key in this mode.
                    state->suppress_next_text_input = true;
                    continue;
                }
            }

            // Kitty keyboard protocol, if this pane's app negotiated it.
            // `continue` rather than `return`, so broadcasting still reaches
            // the remaining windows -- each pane carries its own flags and one
            // may be running a full-screen app while another is at a shell.
            std::string kitty_seq;
            if (encode_kitty_key(sym, mod, event->key.repeat, true,
                                 tw->fpane().terminal.kbd_flags(), kitty_seq)) {
                tw->fpane().pty.write_to_pty(kitty_seq.data(), kitty_seq.size());
                continue;
            }

            // The macOS editing keys: Option and Command with the arrows and
            // delete keys (see key_bindings.hpp).
            if (const char* seq = key_bindings::editing_sequence(sym, mod)) {
                // A selection in the command line is what a delete key
                // removes, the same as plain Delete.
                if ((sym == SDLK_BACKSPACE || sym == SDLK_DELETE) && delete_selection_in_prompt(tw)) continue;
                // Command keys aren't counted as edits above, since most send
                // nothing; these do.
                if (mod & SDL_KMOD_GUI) tw->fpane().terminal.note_user_edit();
                tw->fpane().pty.write_to_pty(seq, std::strlen(seq));
                continue;
            }

            // Option as Meta: ESC, then what the key types without Option.
            // The text-input event macOS follows it with is dropped, or the
            // key would arrive twice.
            if (key_bindings::option_is_meta(state->option_as_meta, mod) &&
                !(mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL))) {
                SDL_Keycode typed = SDL_GetKeyFromScancode(event->key.scancode,
                                                           mod & (SDL_KMOD_SHIFT | SDL_KMOD_CAPS), false);
                std::string meta_seq;
                if (key_bindings::meta_sequence(typed, meta_seq)) {
                    tw->fpane().pty.write_to_pty(meta_seq.data(), meta_seq.size());
                    state->suppress_next_text_input = true;
                    continue;
                }
            }

            if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
                const std::string& typed_line = submitted_line;
                
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
                    std::string song_name = SinkDemo::sing_argument(typed_line);
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
                } else if (int byte = key_bindings::control_symbol_byte(sym); byte >= 0) {
                    const char control_char = static_cast<char>(byte);
                    tw->fpane().pty.write_to_pty(&control_char, 1);
                }
            }
        }
    } else if (event->type == SDL_EVENT_KEY_UP) {
        // Releases exist only for apps that asked for event reporting; with
        // the legacy encoding there is nothing to send and this is skipped
        // entirely, so ordinary typing is unaffected.
        if (!target_tw) return SDL_APP_CONTINUE;
        if (state->settings_ui.is_open() && SDL_GetKeyboardFocus() == state->settings_ui.get_window()) {
            return SDL_APP_CONTINUE;
        }
        if (target_tw->search_drawer_open) return SDL_APP_CONTINUE;

        std::vector<TerminalWindow*> release_targets;
        if (state->input_broadcasting) release_targets = state->windows;
        else release_targets.push_back(target_tw);

        for (auto* tw : release_targets) {
            std::string kitty_seq;
            if (encode_kitty_key(event->key.key, event->key.mod, false, false,
                                 tw->fpane().terminal.kbd_flags(), kitty_seq)) {
                tw->fpane().pty.write_to_pty(kitty_seq.data(), kitty_seq.size());
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
        if (event->user.code == 1 || event->user.code == 3 || event->user.code == 18) {
            // 1: a video file picked in Settings. 18: a kind picked from the
            // background list ("pool" being the built-in video). 3: back to
            // the built-in video.
            char* path = event->user.code == 3 ? nullptr : (char*)event->user.data1;
            state->background = path ? std::string(path) : std::string("pool");
            config::PresetValues p = config::resolve_active(state->config.config());
            p.background = state->background;
            p.theme = state->theme_name;
            state->video_path = resolve_background(state->background);
            for (auto* tw : state->windows) apply_background(state, tw, p);
            state->settings_ui.set_paths(background_display(state), state->font_path);
            save_config(state);
            free(path);
        } else if (event->user.code == 2) { // Font selected
            char* path = (char*)event->user.data1;
            std::cout << "Settings Event: font changed: " << path << std::endl;

            std::string new_font_path = path;
            state->font_path = new_font_path;

            for (auto* tw : state->windows) {
                tw->font_path = new_font_path;
                if (tw->font_manager.load_font(tw->renderer, new_font_path, tw->font_size * tw->display_scale, false)) {
                    tw->cell_w = (tw->font_manager.get_cell_width() / tw->display_scale) + 1.0f;
                    tw->cell_h = (tw->font_manager.get_cell_height() / tw->display_scale) - 0.8f;

                    layout_panes(state, tw);
                }
            }

            state->settings_ui.set_paths(background_display(state), state->font_path);
            save_config(state);
            free(path);
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
            set_input_broadcasting(state, broadcast);
        } else if (event->user.code == 10) { // Preset picked: it, as saved, dropping any edits
            char* name = (char*)event->user.data1;
            std::cout << "Settings Event: switched to preset '" << name << "'" << std::endl;
            state->config.clear_edits();
            state->config.set_global("preset", config::Value(std::string(name)));
            apply_preset_to_state_and_windows(state, config::resolve_active(state->config.config()));
            sync_settings_ui_from_state(state);
            free(name);
        } else if (event->user.code == 11) { // New preset: the current look, saved under a name
            char* name = (char*)event->user.data1;
            std::cout << "Settings Event: new preset '" << name << "'" << std::endl;
            const std::string created = state->config.save_as_preset(name);
            for (auto* tw : state->windows) {
                if (tw->preset_name == state->active_preset_name) tw->preset_name = created;
            }
            state->active_preset_name = created;  // the look itself is unchanged
            sync_settings_ui_from_state(state);
            free(name);
        } else if (event->user.code == 12) { // Save edits into the active preset
            std::cout << "Settings Event: saving edits into preset '" << state->active_preset_name << "'" << std::endl;
            state->config.save_edits();  // built-ins refuse
            sync_settings_ui_from_state(state);
        } else if (event->user.code == 13) { // Rename active preset
            char* name = (char*)event->user.data1;
            std::cout << "Settings Event: renaming preset '" << state->active_preset_name << "' to '" << name << "'" << std::endl;
            if (state->config.rename_preset(state->active_preset_name, name)) {
                for (auto* tw : state->windows) {
                    if (tw->preset_name == state->active_preset_name) tw->preset_name = name;
                }
                state->active_preset_name = name;  // look and feel are unchanged
            }
            sync_settings_ui_from_state(state);
            free(name);
        } else if (event->user.code == 14) { // Delete active preset
            std::cout << "Settings Event: deleting preset '" << state->active_preset_name << "'" << std::endl;
            if (state->config.remove_preset(state->active_preset_name)) {  // built-ins refuse
                const config::Config& c = state->config.config();
                apply_preset_to_state_and_windows(state, config::resolve_active(c));
            }
            sync_settings_ui_from_state(state);
        } else if (event->user.code == 17) { // Theme picked in Settings
            char* name = (char*)event->user.data1;
            state->theme_name = name;
            // The active preset's colors under the new theme, keeping its
            // own color overrides, for every window -- as the other Settings
            // controls do.
            config::PresetValues p = config::resolve_active(state->config.config());
            p.theme = state->theme_name;
            state->color_scheme = scheme_for(p);
            p.background = state->background;
            for (auto* tw : state->windows) {
                tw->color_scheme = state->color_scheme;
                for (Pane* pane : all_panes(tw)) pane->terminal.set_color_scheme(tw->color_scheme);
                apply_background(state, tw, p);  // the gradient and gel follow the theme
            }
            save_config(state);
            state->settings_ui.set_themes(themes::names(), state->theme_name);
            free(name);
        } else if (event->user.code == 15) { // File menu: New Window with Preset
            char* name = (char*)event->user.data1;
            std::cout << "Menu Event: new window with preset '" << name << "'" << std::endl;
            config::PresetValues p = config::resolve_preset(state->config.config(), name);
            TerminalWindow* tw = create_terminal_window(state, nullptr, &p);
            if (tw) {
                state->windows.push_back(tw);
                state->active_window = tw;
            }
            free(name);
        } else if (event->user.code == 16) { // File menu: New Tab with Preset
            char* name = (char*)event->user.data1;
            std::cout << "Menu Event: new tab with preset '" << name << "'" << std::endl;
            config::PresetValues p = config::resolve_preset(state->config.config(), name);
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

// Broadcast mode sends typing to the focused pane of every window, so it
// has to be impossible to forget it's on. Every sign of it is switched here,
// together, whether from the settings window or Window > Broadcast Input:
// the settings button, the menu checkmark, the Dock badge, and -- through
// sync_window_title() and render_broadcast_marker() each frame -- a warning
// in front of every window title and an outline round every receiving pane.
static void set_input_broadcasting(AppState* state, bool on) {
    state->input_broadcasting = on;
    state->settings_ui.set_broadcasting(on);
    set_broadcast_menu_checked(on);
    set_dock_badge(on ? "\u26A0\uFE0E" : nullptr);
}

// The window's title, with the broadcast warning in front of it while
// typing goes everywhere. Checked each frame rather than at each change, so
// windows opened while broadcasting get it too.
static void sync_window_title(AppState* state, TerminalWindow* tw) {
    std::string want = state->input_broadcasting ? "\u26A0\uFE0E broadcasting \u00B7 " + tw->shell_title
                                                 : tw->shell_title;
    if (want != tw->shown_title) {
        SDL_SetWindowTitle(tw->window, want.c_str());
        tw->shown_title = want;
    }
}

// The outline and "broadcasting" tag on a pane receiving broadcast input,
// in coral: warm enough to stand out from the water colours and whatever
// video is behind the text, without reading as an error. Drawn after the
// CRT pass so it stays crisp.
static void render_broadcast_marker(TerminalWindow* tw, const Pane& pane) {
    SDL_Renderer* r = tw->renderer;
    const float s = tw->display_scale;
    const SDL_FRect area = { pane.rect.x * s, pane.rect.y * s, pane.rect.w * s, pane.rect.h * s };
    const float edge = 2.0f * s;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 255, 122, 89, 235);
    const SDL_FRect sides[4] = {
        { area.x, area.y, area.w, edge },
        { area.x, area.y + area.h - edge, area.w, edge },
        { area.x, area.y, edge, area.h },
        { area.x + area.w - edge, area.y, edge, area.h },
    };
    SDL_RenderFillRects(r, sides, 4);

    // The tag, in the top right corner
    const std::string label = "broadcasting";
    const float cell_w = tw->font_manager.get_cell_width();
    const float pad = 6.0f * s;
    const float tag_w = label.size() * cell_w + 2.0f * pad;
    const float tag_h = tw->font_manager.get_cell_height() + pad;
    const SDL_FRect tag = { area.x + area.w - tag_w - edge, area.y + edge, tag_w, tag_h };
    SDL_RenderFillRect(r, &tag);

    float x = tag.x + pad;
    const float y = tag.y + pad / 2.0f;
    for (char ch : label) {
        const GlyphInfo* g = tw->font_manager.get_glyph(r, static_cast<char32_t>(ch));
        if (g) {
            SDL_FRect src = g->src_rect;
            SDL_FRect dst = { x, y, g->src_rect.w, g->src_rect.h };
            SDL_SetTextureColorMod(tw->font_manager.get_atlas_texture(), 30, 14, 10);
            SDL_RenderTexture(r, tw->font_manager.get_atlas_texture(), &src, &dst);
        }
        x += cell_w;
    }
    SDL_SetTextureColorMod(tw->font_manager.get_atlas_texture(), 255, 255, 255);
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

// Removes the bracketed-paste markers from text about to be sent *inside* a
// bracketed paste.
//
// Bracketed paste is what stops pasted newlines from being run as commands:
// the shell sees ESC[200~ ... ESC[201~ and treats everything between as
// literal text. That guarantee only holds if the payload cannot contain the
// terminator itself. Pasting content that embeds ESC[201~ would otherwise end
// the paste early, and every byte after it -- newlines included -- would be
// handled as if the user had typed it, so the tail of the clipboard runs as
// commands.
//
// That is reachable without the user pasting anything unusual: OSC 52 lets any
// output sink displays set the clipboard, so a cat'd file or a compromised
// host on the far end of an ssh session can plant the payload and wait for the
// next Cmd+V. ESC[200~ goes too -- it cannot end the paste, but leaving it
// lets a payload nest a second start marker and desynchronise shells that
// count them.
static std::string strip_paste_markers(const char* text, size_t len) {
    static const std::string kMarkers[] = { "\x1b[200~", "\x1b[201~" };
    std::string out(text, len);
    // Removing a marker splices its neighbours together, and those halves can
    // spell a *new* marker that was not in the original text
    // ("ESC[2" + "ESC[201~" + "01~" leaves "ESC[201~" behind). So this runs to
    // a fixed point rather than in one pass. It terminates: every iteration
    // that changes anything makes the string strictly shorter.
    bool changed = true;
    while (changed) {
        changed = false;
        for (const std::string& marker : kMarkers) {
            size_t at;
            while ((at = out.find(marker)) != std::string::npos) {
                out.erase(at, marker.size());
                changed = true;
            }
        }
    }
    return out;
}


// How long a resize has to stop before the final layout is taken as done.
static constexpr Uint64 kResizeSettleMs = 100;

// How long a window size has to hold before a shell at its command line is
// told about it. See layout_pane_node(). Long enough to span the gap between
// frames of a drag, short enough that the prompt reflows as soon as the
// mouse stops.
static constexpr Uint64 kPtyResizeSettleMs = 120;

// After that size is sent, how long the cursor stays hidden while the shell
// redraws its line. A redraw takes a millisecond or two; this only has to
// outlast a frame or so of it.
static constexpr Uint64 kPtyRedrawGraceMs = 60;

SDL_AppResult SDL_AppIterate(void* appstate) {
    AppState* state = static_cast<AppState*>(appstate);
    if (!state) return SDL_APP_FAILURE;

    // Settings changes reach sink.toml once they stop coming; changes made
    // to it by hand are picked up.
    state->config.flush_if_due();
    reload_config_if_changed(state);

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
                    tw->fpane().terminal.note_user_edit();
                    if (tw->fpane().terminal.is_bracketed_paste_active()) {
                        // Wrap in bracketed-paste markers so the shell treats the
                        // content as literal text instead of executing embedded
                        // newlines. The payload has its own markers stripped first,
                        // or it could close the bracket early and escape that --
                        // see strip_paste_markers.
                        std::string body = strip_paste_markers(text, strlen(text));
                        tw->fpane().pty.write_to_pty("\x1b[200~", 6);
                        tw->fpane().pty.write_to_pty(body.data(), body.size());
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
        if (get_find_requested()) toggle_search(tw);
        if (get_crt_mode_requested()) {
            // Route this through the same path the Settings toggle uses
            // rather than flipping the active window's flag directly.
            // crt mode is a global, persisted (preset-level) setting --
            // create_terminal_window seeds every new window/tab from
            // state->crt_mode_enabled, so a value living only on one
            // window can't survive anyway. Skipping the state/UI update
            // also left the settings panel's own toggle comparing against
            // a stale value, so its next click could compute a no-op and
            // read as a dead button.
            bool crt = !state->crt_mode_enabled;
            state->crt_mode_enabled = crt;
            for (auto* w : state->windows) {
                w->crt_mode_enabled = crt;
            }
            state->settings_ui.set_crt_effect_enabled(crt);
            save_config(state);
        }
    }

    if (get_toggle_broadcast_requested()) {
        set_input_broadcasting(state, !state->input_broadcasting);
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
            // Directions are worked out from the panes' places on screen,
            // which a zoom has taken away
            unzoom_pane(state, state->active_window);
            focus_pane_directional(state->active_window, dx, dy);
        }
        if (int step = get_pane_cycle_requested()) {
            cycle_pane_focus(state, state->active_window, step);
        }
        if (get_zoom_pane_requested()) {
            toggle_zoom_pane(state, state->active_window);
        }
        if (get_clear_screen_requested()) {
            clear_screen_and_scrollback(state->active_window);
        }
        if (int change = get_font_size_requested()) {
            TerminalWindow* tw = state->active_window;
            float size = (change == 2) ? tw->base_font_size : tw->font_size + change;
            set_window_font_size(state, tw, size);
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

        // Shell integration lives in [shell], not in a preset, and changes
        // only the shells started from now on.
        bool shell_integration = state->settings_ui.get_shell_integration_enabled();
        if (config::global_bool(state->config.config(), "shell.integration") != shell_integration) {
            state->config.set_global("shell.integration", config::Value(shell_integration));
            PTYBridge::set_shell_integration_enabled(shell_integration);
        }

        bool hdr_console = state->settings_ui.get_hdr_console_enabled();
        if (state->hdr_console_enabled != hdr_console) {
            state->hdr_console_enabled = hdr_console;
            bool all_applied = true;
            for (auto* tw : state->windows) {
                tw->hdr_console_enabled = hdr_console;
                if (!recreate_renderer_for_hdr_console(state, tw, hdr_console)) {
                    all_applied = false;
                }
            }
            // A window that couldn't get the extended colorspace fell back
            // to the standard one, so hdr console isn't actually on for it.
            // Snap the toggle back to what's really in effect instead of
            // leaving the UI claiming a state no window is in -- otherwise
            // the next toggle compares against a lie and skips the work.
            if (!all_applied && hdr_console) {
                state->hdr_console_enabled = false;
                state->settings_ui.set_hdr_console_enabled(false);
            }
            save_config(state);
        }
    }

    // Iterate all active windows
    for (auto* tw : state->windows) {
        refresh_display_scale(state, tw);

        // Follow a resize in progress live (see has_pending_resize). Each
        // layout is timed, and the next waits twice as long as it took --
        // every frame while reflowing is cheap, which it is unless
        // scrollback is very long, and no more than a third of the time when
        // it isn't, so a drag stays smooth either way. A last layout once the
        // size has held for kResizeSettleMs lands the exact final size.
        if (tw->has_pending_resize) {
            bool settled = current_tick - tw->last_resize_event_time >= kResizeSettleMs;
            bool due = current_tick - tw->last_live_layout_tick >= tw->live_layout_gap_ms;
            if (settled || due) {
                Uint64 started_ns = SDL_GetTicksNS();
                // layout_panes no-ops any pane whose cols/rows come out unchanged
                layout_panes(state, tw);
                Uint64 cost_ms = (SDL_GetTicksNS() - started_ns) / 1000000;
                tw->last_live_layout_tick = current_tick;
                tw->live_layout_gap_ms = std::min<Uint64>(kResizeSettleMs, 2 * cost_ms);
                if (settled) tw->has_pending_resize = false;
            }
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

            // A size held back during a resize, now that it has settled.
            if (pane->pty_resize_pending &&
                current_tick - pane->pending_pty_since >= kPtyResizeSettleMs) {
                pane->pty_resize_pending = false;
                pane->pty_resize_sent_at = current_tick;
                pane->pty.resize_pty(pane->pending_pty_cols, pane->pending_pty_rows,
                                     pane->pending_pty_px_w, pane->pending_pty_px_h);
            }
            // While the shell hasn't redrawn for the new size, the cursor is
            // parked where the shell will look for it rather than on the
            // text (TerminalGrid::resize). Hidden until the redraw lands,
            // rather than seen hopping about during the drag.
            pane->terminal.set_cursor_suppressed(
                pane->pty_resize_pending ||
                (pane->pty_resize_sent_at && current_tick - pane->pty_resize_sent_at < kPtyRedrawGraceMs));

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
                if (tw->fade_hold_start_time == 0) {
                    tw->fade_hold_start_time = current_tick;
                }
                // Normally the hold ends the moment the video has something
                // to show. But this overlay covers the *entire window*, so a
                // first frame that never arrives (decode failure, an
                // unreadable file, a decode thread that stopped) hides a
                // perfectly live terminal behind solid black indefinitely --
                // with no way out but restarting. Give up after a deadline
                // and reveal the terminal without the background instead.
                bool ready = tw->video_engine.has_rendered_first_frame();
                if (!ready && (current_tick - tw->fade_hold_start_time) >= kFadeHoldTimeoutMs) {
                    std::cerr << "Video background: no frame decoded within "
                              << kFadeHoldTimeoutMs << "ms -- revealing the terminal without it" << std::endl;
                    tw->has_video = false;
                    ready = true;
                }
                if (ready) {
                    tw->fade_hold_start_time = 0;
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
            tw->fade_hold_start_time = 0;
        }

        // Process incoming shell data per pane (suppressed for the pane a
        // running sinkdemo is streaming into; the other panes stay live)
        for (Pane* pane_ptr : all_panes(tw)) {
        Pane& pane = *pane_ptr;
        pane.pty.read_pending(pane.pty_output);
        // Carry anything last frame's budget did not reach. The swap keeps the
        // common no-backlog case free of a copy.
        if (pane.pty_pending.empty()) {
            pane.pty_pending.swap(pane.pty_output);
        } else if (!pane.pty_output.empty()) {
            pane.pty_pending.insert(pane.pty_pending.end(),
                                    pane.pty_output.begin(), pane.pty_output.end());
        }

        // Parse only as much as fits in this frame's budget; the rest waits.
        // Under a full-tilt `cat` a single drain reached 3MB and took 20.8ms
        // to parse -- 125% of a 60Hz frame spent inside the parser, before
        // rendering and before input was so much as looked at, which is why a
        // runaway command was so hard to interrupt.
        const char* out_data = pane.pty_pending.data();
        size_t out_size = pane.pty_pending.size();
        size_t consumed = 0;

        bool demo_owns_pane = SinkDemo::is_demo_running(tw) && pane_ptr == &tw->demo_target();
        if (out_size > 0 && !demo_owns_pane) {
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

                // Output inside a synchronized update is held, not typed out.
                if (out_size > 5 || fast_turnover || pane.terminal.is_synchronized_output()) {
                    // Large chunk / fast turnover (command or program output): bypass typing effect
                    if (!pane.animation_buffer.empty()) {
                        pane.parser.parse(pane.terminal, pane.animation_buffer.data(), pane.animation_buffer.size());
                        pane.animation_buffer.clear();
                    }
                    consumed = parse_within_budget(pane, out_data, out_size);
                } else {
                    // Small, unhurried chunk (user typing): queue for animated
                    // typing, which does its own pacing from there.
                    pane.animation_buffer.insert(pane.animation_buffer.end(), out_data, out_data + out_size);
                    consumed = out_size;
                }
            } else {
                consumed = parse_within_budget(pane, out_data, out_size);
            }
            pane.terminal.note_output_idle();
        }

        // Retire what was handled. A pane a demo has taken over drops its
        // shell output entirely rather than accumulating it, which is what
        // happened implicitly when this buffer was a per-frame local.
        if (demo_owns_pane) {
            pane.pty_pending.clear();
        } else if (consumed > 0) {
            pane.pty_pending.erase(pane.pty_pending.begin(),
                                   pane.pty_pending.begin() + static_cast<long>(consumed));
        }

        // Apply any OSC 0/2 title change from the data just parsed (only
        // the focused pane owns the window title)
        if (pane_ptr == tw->focused && pane.terminal.has_pending_title()) {
            std::string title = pane.terminal.take_window_title();
            tw->shell_title = title.empty() ? "sink" : title;
        }

        // Send anything the parser owes the shell (DSR cursor reports, DA
        // capability answers). Not gated to the focused pane either, and for
        // a stronger reason than the clipboard: the program that asked is
        // most likely blocked waiting for this, focused or not.
        if (pane.terminal.has_pending_reply()) {
            std::string reply = pane.terminal.take_pending_reply();
            pane.pty.write_to_pty(reply.data(), reply.size());
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
                pane.terminal.note_output_idle();
            }
        }
        } // end per-pane read/parse loop

        // Defence in depth against a window whose renderer teardown/recreate
        // (see recreate_renderer_for_hdr_console) left it without one: every
        // SDL_Render* call below would silently no-op, presenting nothing and
        // leaving a black window that looks like a hang.
        if (!tw->renderer) continue;

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

        // A. The background. Opaque kinds cover the window; the clear and
        // glass ones lay a tint of the theme's background over the desktop
        // (or the glass macOS draws behind the window).
        const SDL_FColor& bc = tw->backdrop_color;
        const float lit = tw->exposure;
        switch (tw->backdrop) {
            case Backdrop::Color:
                SDL_SetRenderDrawColorFloat(tw->renderer, bc.r * lit, bc.g * lit, bc.b * lit, 1.0f);
                break;
            case Backdrop::Clear:
            case Backdrop::Frosted:
            case Backdrop::LiquidGlass:
                SDL_SetRenderDrawColorFloat(tw->renderer, bc.r, bc.g, bc.b, tw->glass_tint);
                break;
            case Backdrop::Gradient:
            case Backdrop::Gel:
            case Backdrop::Video:
                SDL_SetRenderDrawColor(tw->renderer, 0, 0, 0, 255);
                break;
        }
        SDL_RenderClear(tw->renderer);
        if (tw->backdrop == Backdrop::Gradient || tw->backdrop == Backdrop::Gel) {
            if (tw->backdrop == Backdrop::Gel) tw->backdrop_params.time += dt * tw->gel_speed;
            BackgroundShader::Params params = tw->backdrop_params;
            params.exposure = tw->exposure;
            params.hue_degrees = tw->hue_shift_degrees;
            params.logical_w = draw_w / tw->display_scale;
            params.logical_h = draw_h / tw->display_scale;
            if (tw->background_shader.is_ready()) {
                tw->background_shader.draw(tw->renderer, params);
            } else {
                SDL_SetRenderDrawColorFloat(tw->renderer, bc.r * lit, bc.g * lit, bc.b * lit, 1.0f);
                SDL_RenderClear(tw->renderer);
            }
        }

        // The video, when that's the background.
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
            if (!pane_visible(tw, pane_ptr)) continue;
            Pane& pane = *pane_ptr;
            std::lock_guard<std::mutex> lock(pane.grid_mutex);
            pane.terminal.set_enable_ligatures(state->ligatures_enabled);
            float start_x = (pane.rect.x + state->padding) * tw->display_scale;
            float start_y = (pane.rect.y + state->padding) * tw->display_scale;

            // Clip each pane so its margin fill and glyphs can't bleed
            // across the divider into a neighbor
            SDL_Rect clip = {
                static_cast<int>(pane.rect.x * tw->display_scale),
                static_cast<int>(pane.rect.y * tw->display_scale),
                static_cast<int>(pane.rect.w * tw->display_scale),
                static_cast<int>(pane.rect.h * tw->display_scale)
            };
            SDL_SetRenderClipRect(tw->renderer, &clip);

            if (tw->hdr_console_enabled) {
                SDL_SetRenderColorScale(tw->renderer, kHdrConsoleTextBoost);
            }
            pane.terminal.render(tw->renderer, tw->font_manager, start_x, start_y, tw->display_scale, dt, tw->animated_typing);
            if (tw->hdr_console_enabled) {
                SDL_SetRenderColorScale(tw->renderer, 1.0f);
            }

            // Dim unfocused panes so the active one reads at a glance
            if (pane_ptr != tw->focused && !tw->root->is_leaf() && !tw->zoomed) {
                SDL_FRect dim = {
                    pane.rect.x * tw->display_scale,
                    pane.rect.y * tw->display_scale,
                    pane.rect.w * tw->display_scale,
                    pane.rect.h * tw->display_scale
                };
                SDL_SetRenderDrawBlendMode(tw->renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(tw->renderer, 0, 0, 0, 64);
                SDL_RenderFillRect(tw->renderer, &dim);
            }
            SDL_SetRenderClipRect(tw->renderer, nullptr);
        }

        // Divider lines between panes
        if (!tw->root->is_leaf() && !tw->zoomed) {
            render_pane_dividers(tw, tw->root.get(), tw->display_scale);
        }

        // C. Composite with the CRT shader, or fall back to the plain
        // rectangle-overlay version if the shader isn't available.
        if (use_crt_shader) {
            tw->crt_shader.end_scene(tw->renderer);
        } else if (tw->crt_mode_enabled) {
            render_crt_effects(tw->renderer, draw_w, draw_h, tw->display_scale);
        }

        // D. Draw Search Bar Drawer UI if open
        if (tw->search_drawer_open) {
            render_search_drawer(tw, draw_w, draw_h);
        }

        // Broadcasting: mark the pane this window's typing goes to
        if (state->input_broadcasting && tw->focused && pane_visible(tw, tw->focused)) {
            render_broadcast_marker(tw, *tw->focused);
        }
        sync_window_title(state, tw);

        // E. Draw black dissolve overlay mask
        if (tw->fade_opacity > 0.0f) {
            SDL_FRect full_rect = { 0.0f, 0.0f, static_cast<float>(draw_w), static_cast<float>(draw_h) };
            SDL_SetRenderDrawBlendMode(tw->renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(tw->renderer, 0, 0, 0, static_cast<Uint8>(tw->fade_opacity * 255.0f));
            SDL_RenderFillRect(tw->renderer, &full_rect);
        }

        // Every frame, even while a program has a synchronized update open:
        // its output is what's held (see kSyncBegin), so the text on screen
        // is always a finished update and the background keeps moving.
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
    // The menu timer runs a frame through g_app_state, and it can still fire
    // after this returns: SDL_Quit's Cocoa teardown spins the run loop. A
    // frame on the state deleted below crashed every quit from SDL 3.4.18 on.
#if defined(__APPLE__)
    stop_menu_render_timer();
#endif
    g_app_state = nullptr;

    AppState* state = static_cast<AppState*>(appstate);
    if (state) {
        state->config.flush();
        state->settings_ui.close();
        for (auto* tw : state->windows) {
            destroy_terminal_window(tw);
        }
        delete state;
    }
}
