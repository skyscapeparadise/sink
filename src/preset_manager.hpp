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

// Presets are stored as individual key=value text files under
// ~/.config/sink/presets/, one per preset, named after a filesystem-safe
// slug of the preset's display name (the exact display name is also stored
// inside the file, so casing/punctuation survives slug collisions).
//
// "pool" is the permanent baseline preset: it always exists and the UI does
// not allow renaming or deleting it, so there's always a known-good preset
// to fall back to.
namespace presets {

std::string presets_dir();

// Names of all presets on disk, "pool" first, then alphabetical
// (case-insensitive). Creates the presets directory if it doesn't exist yet.
std::vector<std::string> list_names();

bool exists(const std::string& name);

// Loads a preset by name; returns a default-valued Preset (named `name`)
// if no matching file exists.
Preset load(const std::string& name);

// Writes `preset` to disk under a slug derived from preset.name.
void save(const Preset& preset);

// Renames a preset in place, preserving its settings. Returns false (no-op)
// if `old_name` is "pool", `new_name` is empty, or `new_name` is already
// taken by a different preset.
bool rename(const std::string& old_name, const std::string& new_name);

// Deletes a preset's file. Returns false (no-op) for "pool" or unknown names.
bool remove(const std::string& name);

// Returns `base` unchanged if unused, otherwise `base` with " 2", " 3", ...
// appended until an unused name is found.
std::string unique_name(const std::string& base);

} // namespace presets
