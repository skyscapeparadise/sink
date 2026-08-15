#pragma once

#include <string>
#include <vector>

// A bundle of the terminal customizations a user can save and switch
// between: background media, typeface, exposure, and the various
// look/behavior toggles exposed in the settings UI.
struct Preset {
    std::string name = "pool";
    std::string video_path = "default";
    std::string font_path = "default";
    float exposure = 0.7f;
    float hue_shift = 0.0f; // degrees, 0-360
    bool animated_typing = true;
    bool vibrancy_enabled = true;
    bool crt_mode_enabled = false;
    bool ligatures_enabled = true;
    bool hdr_console_enabled = false;
    int scrollback_lines = 10000;
};

// Presets, and which one is currently active, live together in one
// hand-editable TOML file (see config_path()) -- each preset is its own
// [presets.<name>] table. "pool" is the permanent baseline preset: it
// always exists and the UI does not allow renaming or deleting it, so
// there's always a known-good preset to fall back to.
//
// This file is fully regenerated (not incrementally patched) on every save
// -- including ones the Settings UI triggers on every tweak -- so it's
// meant to be hand-edited freely, but a comment added inside a preset's own
// section won't survive the *next* GUI-triggered save (the file-level
// header comment is always restored either way). A future version may add
// comment-preserving round-trip edits; this one doesn't yet.
namespace presets {

std::string config_path();

// One-time migration from the pre-TOML config (~/.config/sink/config.txt
// pointing at ~/.config/sink/presets/*.txt, or an even older flat
// config.txt from before presets existed at all) into config_path(). A
// no-op if config_path() already exists. The old files are left in place,
// untouched, but never read again once this has run once. Called once at
// startup before any other function in this namespace.
void migrate_legacy_config_if_needed();

// Names of all presets, "pool" first, then alphabetical (case-insensitive).
std::vector<std::string> list_names();

bool exists(const std::string& name);

// Loads a preset by name; returns a default-valued Preset (named `name`)
// if no preset by that name exists.
Preset load(const std::string& name);

// Writes `preset` into its [presets.<preset.name>] table.
void save(const Preset& preset);

// Renames a preset in place, preserving its settings. Returns false (no-op)
// if `old_name` is "pool", `new_name` is empty, or `new_name` is already
// taken by a different preset.
bool rename(const std::string& old_name, const std::string& new_name);

// Removes a preset. Returns false (no-op) for "pool" or unknown names.
bool remove(const std::string& name);

// Returns `base` unchanged if unused, otherwise `base` with " 2", " 3", ...
// appended until an unused name is found.
std::string unique_name(const std::string& base);

// The top-level `active_preset` key.
std::string get_active_preset_name();
void set_active_preset_name(const std::string& name);

} // namespace presets
