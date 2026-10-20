#pragma once

// sink.toml, version 1: the key registry, the in-memory model, and reading a
// document into it.
//
// Every key sink understands is described once, in the registry (see
// config::keys()): its path, type, default, allowed range and documentation.
// Validation, `sink --show-config`, the JSON schema and the README reference
// are all generated from it, so they cannot disagree with each other or with
// what sink actually reads.
//
// Values are kept generically, by key path, rather than in a struct per
// section: a preset stores only the keys it overrides, and anything sink does
// not recognise is left exactly as the user wrote it. resolve_preset() turns a
// preset into the fully-populated PresetValues the rest of sink uses.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace config {

constexpr int kCurrentVersion = 1;

// ---- Values ----------------------------------------------------------------

using StringList = std::vector<std::string>;
using Value = std::variant<bool, int64_t, double, std::string, StringList>;

enum class KeyType {
    Bool,
    Int,
    Float,
    String,
    Path,        // a file path, or a name with no '/' (see is_path())
    BoolOrChoice // true, false, or one of KeyInfo::choices
};

// Which part of the file a key lives in. Global keys are written at their
// path from the top of the document ("terminal.scrollback_lines"); preset keys
// at their path inside [presets.<name>] ("exposure").
enum class Scope { Global, Preset };

struct KeyInfo {
    const char* path;
    Scope scope;
    KeyType type;
    Value default_value;
    double min = 0;      // Int and Float only
    double max = 0;
    StringList choices;  // BoolOrChoice only
    const char* doc;
};

// Every key, in the order the reference documents them.
const std::vector<KeyInfo>& keys();
const KeyInfo* find_key(Scope scope, const std::string& path);

// ---- Model -----------------------------------------------------------------

// A value as found in the file, with where it was, so a problem can be
// reported against the line the user needs to look at.
struct Setting {
    Value value;
    int line = 0;
};

using SettingMap = std::map<std::string, Setting>;  // key path -> setting

struct Diagnostic {
    int line = 0;          // 1-based; 0 when there is no meaningful line
    std::string message;
};

struct Config {
    int version = kCurrentVersion;
    SettingMap global;                          // validated global keys
    std::map<std::string, SettingMap> presets;  // user presets: name -> overrides
    std::vector<std::string> preset_order;      // names as they appear in the file
    std::vector<Diagnostic> warnings;           // unknown keys, out-of-range values
};

// The values a preset resolves to: its overrides on top of the built-in it
// shares a name with (if any) on top of the registry defaults.
struct PresetValues {
    std::string name;
    std::string background;    // a path, or a built-in background's name
    std::string font;          // a path, or a font name
    double exposure = 0;
    double hue_shift = 0;
    bool crt = false;
    bool hdr_text = false;
    bool ligatures = true;
    bool title_bar = true;
    bool typing_animation = true;
};

struct BuiltinPreset {
    const char* name;
    std::vector<std::pair<std::string, Value>> overrides;  // preset-scope paths
};

const std::vector<BuiltinPreset>& builtin_presets();
bool is_builtin(const std::string& preset_name);

// ---- Reading ---------------------------------------------------------------

// Parses TOML text and validates it into a Config.
//
// On a syntax error returns nullopt and fills `error` with the line and
// message; the caller must then leave the file alone rather than overwrite
// what the user is in the middle of editing.
//
// Otherwise never fails: a value of the wrong type is reported in warnings
// and left out (so the default applies), one out of range is clamped and
// reported, and unknown keys are reported but otherwise ignored -- they stay
// in the file untouched.
std::optional<Config> read_text(const std::string& text, Diagnostic& error);

// Typed access to a global key, falling back to its registry default.
bool global_bool(const Config& c, const std::string& path);
int64_t global_int(const Config& c, const std::string& path);
std::string global_string(const Config& c, const std::string& path);

// keyboard.option_as_meta as one of "none", "left", "right", "both".
std::string option_as_meta(const Config& c);

// The active preset's name (top-level `preset`), or "pool" when it names
// nothing that exists.
std::string active_preset(const Config& c);

// Every preset name: built-ins first in their defined order, then the user's
// in file order. A user preset may not take a built-in's name.
std::vector<std::string> preset_names(const Config& c);
bool preset_exists(const Config& c, const std::string& name);

PresetValues resolve_preset(const Config& c, const std::string& name);

// ---- Generating and migrating -------------------------------------------------

// A complete sink.toml for `c`, with the header comment: `preset`, then the
// global keys that differ from their defaults grouped by table, then each
// user preset's overrides. Used for new files and migrations; edits to an
// existing file go through the in-place editor instead, which keeps the
// user's comments and layout.
std::string generate(const Config& c);

// Whether `text` is a version 0 file: the format before this one, which had no
// `version` key, kept `active_preset` at the top and wrote every setting into
// every preset under the old names (video_path, vibrancy_enabled, ...). A file
// with no `version` and none of those is read as current, so a config written
// by hand from the documentation needn't start with `version = 1`.
bool is_v0(const std::string& text);

// Converts a version 0 file to current text. Keys are renamed, values equal
// to their defaults dropped, scrollback_lines moved out of presets (the active
// preset's value is kept), and a customised "pool" -- now a built-in that
// can't be redefined -- becomes "pool copy", staying active if it was.
// nullopt if `text` does not parse.
std::optional<std::string> migrate_v0(const std::string& text);

// Converts the pre-TOML configuration: ~/.config/sink/config.txt and the
// presets/*.txt files beside it, given as file name -> contents, into
// current text.
std::string migrate_legacy(const std::string& config_txt,
                           const std::map<std::string, std::string>& preset_files);

// ---- Reference -----------------------------------------------------------------
//
// All generated from keys(), so none of them can disagree with what sink
// actually reads.

// Where the JSON schema is published. Files sink creates name it on their
// first line (`#:schema ...`), which editors with Taplo-based TOML support
// (VS Code, Zed, Neovim) use for completion and checking.
constexpr const char* kSchemaUrl =
    "https://raw.githubusercontent.com/skyscapeparadise/sink/main/sink.schema.json";

// Every setting at its default, each with its documentation, as a valid
// sink.toml (`sink --show-config --default`).
std::string reference_toml();

// Every setting as sink reads `c`, defaults filled in, every preset in full
// (`sink --show-config`).
std::string effective_toml(const Config& c);

// A JSON Schema (draft-07) for sink.toml. The copy at kSchemaUrl is checked
// against this by the tests.
std::string json_schema();

// ---- Paths -----------------------------------------------------------------

// A value is a path if it contains '/' or starts with '~'; otherwise it names
// something sink ships (a background, a font).
bool is_path(const std::string& value);
std::string expand_home(const std::string& path);    // "~/x" -> "/Users/me/x"
std::string contract_home(const std::string& path);  // "/Users/me/x" -> "~/x"

// ---- Writing values ----------------------------------------------------------

// A value as TOML source text: strings quoted and escaped, floats with no
// binary noise (0.834762, not 0.83476197719573975).
std::string to_toml(const Value& v);

// A key as TOML source text: bare when it can be, quoted otherwise.
std::string toml_key(const std::string& key);

} // namespace config
