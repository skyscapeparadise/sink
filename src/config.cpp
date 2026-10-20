#include "config.hpp"

// toml++ v3.4.0 still uses the pre-C++23 whitespace-before-literal-suffix
// syntax internally; silence that rather than let a third-party header's
// deprecation notice show up in sink's build. Same in toml_edit.cpp.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-literal-operator"
#endif
#include <toml.hpp>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace config {

// ---- Registry ----------------------------------------------------------------

const std::vector<KeyInfo>& keys() {
    static const std::vector<KeyInfo> k = {
        // Global
        { "preset", Scope::Global, KeyType::String, std::string("pool"), 0, 0, {},
          "The preset in use: a built-in such as \"pool\", or the name of one of "
          "your [presets.<name>] tables." },
        { "terminal.scrollback_lines", Scope::Global, KeyType::Int, int64_t{10000}, 100, 1000000, {},
          "Lines of output kept above the screen, per pane." },
        { "shell.integration", Scope::Global, KeyType::Bool, true, 0, 0, {},
          "Load sink's shell integration into zsh and bash: prompt marks "
          "(OSC 133) and the working directory (OSC 7), which make Cmd+Up/Down "
          "prompt jumping and new tabs in the current directory work." },
        { "keyboard.option_as_meta", Scope::Global, KeyType::BoolOrChoice, false, 0, 0, {"left", "right"},
          "Make Option act as Meta, sending ESC before the key (for emacs and "
          "readline shortcuts) instead of typing special characters. true for "
          "both Option keys, or \"left\" or \"right\" for just one." },

        // Preset
        { "background", Scope::Preset, KeyType::Path, std::string("pool"), 0, 0, {},
          "The video behind the terminal: a built-in background's name, or a "
          "path to a video file." },
        { "font", Scope::Preset, KeyType::Path, std::string("Monaspace Neon"), 0, 0, {},
          "The typeface: \"Monaspace Neon\" (bundled), or a path to a font "
          "file. Characters the font lacks come from Monaspace Neon, then the "
          "system's fonts." },
        { "exposure", Scope::Preset, KeyType::Float, 0.7, 0, 2, {},
          "Brightness of the background video." },
        { "hue_shift", Scope::Preset, KeyType::Float, 0.0, 0, 360, {},
          "Rotates the background video's colors, in degrees." },
        { "crt", Scope::Preset, KeyType::Bool, false, 0, 0, {},
          "The CRT effect: scanlines, curvature and glow." },
        { "hdr_text", Scope::Preset, KeyType::Bool, false, 0, 0, {},
          "Draw text brighter than SDR white on HDR displays." },
        { "ligatures", Scope::Preset, KeyType::Bool, true, 0, 0, {},
          "Draw pairs like -> and != as single symbols." },
        { "title_bar", Scope::Preset, KeyType::Bool, true, 0, 0, {},
          "Show the translucent title bar." },
        { "typing_animation", Scope::Preset, KeyType::Bool, true, 0, 0, {},
          "Animate output as it is typed onto the screen." },
    };
    return k;
}

const KeyInfo* find_key(Scope scope, const std::string& path) {
    for (const KeyInfo& k : keys()) {
        if (k.scope == scope && path == k.path) return &k;
    }
    return nullptr;
}

const std::vector<BuiltinPreset>& builtin_presets() {
    // pool is the registry defaults exactly; later built-ins (one per bundled
    // background) list what makes them different.
    static const std::vector<BuiltinPreset> b = {
        { "pool", {} },
    };
    return b;
}

bool is_builtin(const std::string& name) {
    for (const BuiltinPreset& b : builtin_presets()) {
        if (name == b.name) return true;
    }
    return false;
}

// ---- Values ------------------------------------------------------------------

namespace {

int line_of(const toml::node& n) {
    return static_cast<int>(n.source().begin.line);
}

std::string type_name(KeyType t) {
    switch (t) {
        case KeyType::Bool: return "true or false";
        case KeyType::Int: return "a whole number";
        case KeyType::Float: return "a number";
        case KeyType::String: return "a string";
        case KeyType::Path: return "a string";
        case KeyType::BoolOrChoice: return "true, false or a string";
    }
    return "a value";
}

std::string format_number(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

// Converts and validates one node against its registry entry. Returns nullopt
// (with a warning) when the value can't be used, so the default applies.
std::optional<Value> convert(const KeyInfo& k, const std::string& where, const toml::node& n,
                             std::vector<Diagnostic>& warnings) {
    const int line = line_of(n);
    auto wrong_type = [&]() -> std::optional<Value> {
        warnings.push_back({ line, where + " should be " + type_name(k.type) + "; using the default" });
        return std::nullopt;
    };
    auto clamp = [&](double v) {
        if (v < k.min || v > k.max) {
            double c = std::clamp(v, k.min, k.max);
            warnings.push_back({ line, where + " = " + format_number(v) + " is outside " +
                                       format_number(k.min) + " to " + format_number(k.max) +
                                       "; using " + format_number(c) });
            return c;
        }
        return v;
    };

    switch (k.type) {
        case KeyType::Bool:
            if (const auto* b = n.as_boolean()) return Value(b->get());
            return wrong_type();
        case KeyType::Int: {
            if (const auto* i = n.as_integer()) {
                return Value(static_cast<int64_t>(clamp(static_cast<double>(i->get()))));
            }
            if (const auto* f = n.as_floating_point()) {
                double v = f->get();
                if (std::isfinite(v) && v == std::floor(v)) {
                    return Value(static_cast<int64_t>(clamp(v)));
                }
            }
            return wrong_type();
        }
        case KeyType::Float: {
            double v;
            if (const auto* f = n.as_floating_point()) v = f->get();
            else if (const auto* i = n.as_integer()) v = static_cast<double>(i->get());
            else return wrong_type();
            if (!std::isfinite(v)) return wrong_type();
            return Value(clamp(v));
        }
        case KeyType::String:
            if (const auto* s = n.as_string()) return Value(s->get());
            return wrong_type();
        case KeyType::Path:
            if (const auto* s = n.as_string()) {
                if (s->get().empty()) return wrong_type();
                return Value(s->get());
            }
            return wrong_type();
        case KeyType::BoolOrChoice:
            if (const auto* b = n.as_boolean()) return Value(b->get());
            if (const auto* s = n.as_string()) {
                if (std::find(k.choices.begin(), k.choices.end(), s->get()) != k.choices.end()) {
                    return Value(s->get());
                }
                std::string allowed = "true, false";
                for (const std::string& c : k.choices) allowed += ", \"" + c + "\"";
                warnings.push_back({ line, where + " = \"" + s->get() + "\" isn't one of " +
                                           allowed + "; using the default" });
                return std::nullopt;
            }
            return wrong_type();
    }
    return wrong_type();
}

// Walks a table, validating every key it knows and reporting the rest.
// `prefix` is the path within the scope ("" or "terminal"); `where_prefix`
// is how the location reads in a message ("presets.clouds.").
void walk(const toml::table& t, Scope scope, const std::string& prefix, const std::string& where_prefix,
          SettingMap& out, std::vector<Diagnostic>& warnings) {
    for (auto&& [key, node] : t) {
        const std::string path = prefix.empty() ? std::string(key.str()) : prefix + "." + std::string(key.str());
        const std::string where = where_prefix + path;
        if (const KeyInfo* k = find_key(scope, path)) {
            if (auto v = convert(*k, where, node, warnings)) {
                out[path] = Setting{ *v, line_of(node) };
            }
        } else if (const auto* sub = node.as_table()) {
            walk(*sub, scope, path, where_prefix, out, warnings);
        } else {
            warnings.push_back({ static_cast<int>(key.source().begin.line),
                                 "unknown setting " + where + " (left as is)" });
        }
    }
}

const Setting* find(const SettingMap& m, const std::string& path) {
    auto it = m.find(path);
    return it == m.end() ? nullptr : &it->second;
}

Value global_value(const Config& c, const std::string& path) {
    if (const Setting* s = find(c.global, path)) return s->value;
    if (const KeyInfo* k = find_key(Scope::Global, path)) return k->default_value;
    return Value(false);
}

} // namespace

// ---- Reading -----------------------------------------------------------------

std::optional<Config> read_text(const std::string& text, Diagnostic& error) {
    toml::table doc;
    try {
        doc = toml::parse(text);
    } catch (const toml::parse_error& e) {
        error.line = static_cast<int>(e.source().begin.line);
        error.message = std::string(e.description());
        return std::nullopt;
    }

    Config c;
    c.version = static_cast<int>(doc["version"].value_or(int64_t{kCurrentVersion}));

    for (auto&& [key, node] : doc) {
        const std::string name(key.str());
        if (name == "version") continue;
        if (name == "presets") {
            const auto* presets = node.as_table();
            if (!presets) {
                c.warnings.push_back({ line_of(node), "presets should be a table; ignoring it" });
                continue;
            }
            for (auto&& [pkey, pnode] : *presets) {
                const std::string pname(pkey.str());
                const auto* ptable = pnode.as_table();
                if (!ptable) {
                    c.warnings.push_back({ line_of(pnode), "presets." + pname + " should be a table; ignoring it" });
                    continue;
                }
                if (is_builtin(pname)) {
                    c.warnings.push_back({ static_cast<int>(pkey.source().begin.line),
                                           "\"" + pname + "\" is a built-in preset and can't be redefined; "
                                           "rename this one to use it" });
                    continue;
                }
                SettingMap overrides;
                walk(*ptable, Scope::Preset, "", "presets." + toml_key(pname) + ".", overrides, c.warnings);
                c.presets[pname] = std::move(overrides);
                c.preset_order.push_back(pname);
            }
            continue;
        }
        if (const KeyInfo* k = find_key(Scope::Global, name)) {
            if (auto v = convert(*k, name, node, c.warnings)) c.global[name] = Setting{ *v, line_of(node) };
        } else if (const auto* sub = node.as_table()) {
            walk(*sub, Scope::Global, name, "", c.global, c.warnings);
        } else {
            c.warnings.push_back({ static_cast<int>(key.source().begin.line),
                                   "unknown setting " + name + " (left as is)" });
        }
    }
    return c;
}

bool global_bool(const Config& c, const std::string& path) {
    Value v = global_value(c, path);
    return std::holds_alternative<bool>(v) && std::get<bool>(v);
}

int64_t global_int(const Config& c, const std::string& path) {
    Value v = global_value(c, path);
    return std::holds_alternative<int64_t>(v) ? std::get<int64_t>(v) : 0;
}

std::string global_string(const Config& c, const std::string& path) {
    Value v = global_value(c, path);
    return std::holds_alternative<std::string>(v) ? std::get<std::string>(v) : std::string();
}

std::string option_as_meta(const Config& c) {
    Value v = global_value(c, "keyboard.option_as_meta");
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "both" : "none";
    if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
    return "none";
}

std::vector<std::string> preset_names(const Config& c) {
    std::vector<std::string> names;
    for (const BuiltinPreset& b : builtin_presets()) names.push_back(b.name);
    for (const std::string& n : c.preset_order) names.push_back(n);
    return names;
}

bool preset_exists(const Config& c, const std::string& name) {
    return is_builtin(name) || c.presets.count(name) > 0;
}

std::string active_preset(const Config& c) {
    std::string name = global_string(c, "preset");
    return preset_exists(c, name) ? name : std::string(builtin_presets().front().name);
}

PresetValues resolve_preset(const Config& c, const std::string& name) {
    std::map<std::string, Value> v;
    for (const KeyInfo& k : keys()) {
        if (k.scope == Scope::Preset) v[k.path] = k.default_value;
    }
    for (const BuiltinPreset& b : builtin_presets()) {
        if (name == b.name) {
            for (const auto& [path, value] : b.overrides) v[path] = value;
        }
    }
    if (auto it = c.presets.find(name); it != c.presets.end()) {
        for (const auto& [path, setting] : it->second) v[path] = setting.value;
    }

    auto str = [&](const char* p) { return std::get<std::string>(v[p]); };
    auto num = [&](const char* p) { return std::get<double>(v[p]); };
    auto flag = [&](const char* p) { return std::get<bool>(v[p]); };

    PresetValues r;
    r.name = name;
    r.background = is_path(str("background")) ? expand_home(str("background")) : str("background");
    r.font = is_path(str("font")) ? expand_home(str("font")) : str("font");
    r.exposure = num("exposure");
    r.hue_shift = num("hue_shift");
    r.crt = flag("crt");
    r.hdr_text = flag("hdr_text");
    r.ligatures = flag("ligatures");
    r.title_bar = flag("title_bar");
    r.typing_animation = flag("typing_animation");
    return r;
}

// ---- Generating and migrating -----------------------------------------------------

namespace {

const char* kHeader =
    "# sink configuration\n"
    "#\n"
    "# Edit freely: sink keeps your comments and layout, and only rewrites the\n"
    "# lines for settings you change in the Settings window. Every setting, with\n"
    "# its default, is listed by `sink --show-config --default` and at\n"
    "# https://github.com/skyscapeparadise/sink#config\n"
    "\n";

bool same_value(const Value& a, const Value& b) {
    if (std::holds_alternative<double>(a) && std::holds_alternative<double>(b)) {
        return std::fabs(std::get<double>(a) - std::get<double>(b)) < 1e-6;
    }
    return a == b;
}

// Writes the settings of one scope, keys without a table first, then one
// [header] per table. `header_prefix` is "" for globals, "presets.<name>."
// for a preset. Keys come out in registry order.
void emit_scope(std::ostringstream& o, Scope scope, const SettingMap& settings,
                const std::string& header_prefix, bool header_written) {
    std::vector<std::string> tables;
    std::map<std::string, std::vector<std::pair<std::string, const Value*>>> by_table;
    for (const KeyInfo& k : keys()) {
        if (k.scope != scope) continue;
        auto it = settings.find(k.path);
        if (it == settings.end()) continue;
        const std::string path = k.path;
        const size_t dot = path.rfind('.');
        const std::string table = dot == std::string::npos ? "" : path.substr(0, dot);
        const std::string key = dot == std::string::npos ? path : path.substr(dot + 1);
        if (!by_table.count(table)) tables.push_back(table);
        by_table[table].push_back({ key, &it->second.value });
    }
    // Keys outside any table must come first: after a [header], a bare key
    // belongs to that table.
    std::stable_partition(tables.begin(), tables.end(), [](const std::string& t) { return t.empty(); });
    for (const std::string& table : tables) {
        if (!table.empty() || !header_written) {
            std::string header = header_prefix + table;
            if (!header.empty() && header.back() == '.') header.pop_back();
            if (!header.empty()) o << "\n[" << header << "]\n";
        }
        for (const auto& [key, value] : by_table[table]) {
            o << toml_key(key) << " = " << to_toml(*value) << "\n";
        }
    }
}

// The renames from version 0, for keys inside a preset.
const std::pair<const char*, const char*> kV0PresetKeys[] = {
    { "video_path", "background" },
    { "font_path", "font" },
    { "exposure", "exposure" },
    { "hue_shift", "hue_shift" },
    { "animated_typing", "typing_animation" },
    { "vibrancy_enabled", "title_bar" },
    { "crt_mode_enabled", "crt" },
    { "ligatures_enabled", "ligatures" },
    { "hdr_console_enabled", "hdr_text" },
};

std::string unique_preset_name(const Config& c, const std::string& base) {
    if (!preset_exists(c, base)) return base;
    for (int i = 2;; ++i) {
        std::string candidate = base + " " + std::to_string(i);
        if (!preset_exists(c, candidate)) return candidate;
    }
}

// The shared half of both migrations: a version 0 document, as a toml++
// table, into a current Config.
Config convert_v0(const toml::table& doc) {
    Config out;
    std::vector<Diagnostic> ignored;

    std::string active = doc["active_preset"].value_or(std::string("pool"));
    if (auto si = doc["shell_integration"].as_boolean(); si && !si->get()) {
        out.global["shell.integration"] = Setting{ false, 0 };
    }
    if (auto b = doc["option_as_meta"].as_boolean(); b && b->get()) {
        out.global["keyboard.option_as_meta"] = Setting{ true, 0 };
    } else if (auto side = doc["option_as_meta"].as_string()) {
        const std::string v = side->get();
        if (v == "both") out.global["keyboard.option_as_meta"] = Setting{ true, 0 };
        else if (v == "left" || v == "right") out.global["keyboard.option_as_meta"] = Setting{ v, 0 };
    }

    // Converted with names as they were, then "pool" is handled once every
    // other name is known, so its new name can't collide with one of them.
    std::map<std::string, SettingMap> converted;
    std::vector<std::string> order;
    if (const toml::table* presets = doc["presets"].as_table()) {
        for (auto&& [pkey, pnode] : *presets) {
            const toml::table* t = pnode.as_table();
            if (!t) continue;
            const std::string name(pkey.str());
            SettingMap overrides;
            for (const auto& [old_key, new_key] : kV0PresetKeys) {
                const toml::node* n = t->get(old_key);
                const KeyInfo* k = find_key(Scope::Preset, new_key);
                if (!n || !k) continue;
                std::optional<Value> v = convert(*k, new_key, *n, ignored);
                if (!v) continue;
                if (k->type == KeyType::Path && std::holds_alternative<std::string>(*v)) {
                    std::string s = std::get<std::string>(*v);
                    if (s == "default" || s.empty()) continue;
                    // The bundled files were saved by path before "default"
                    // existed for them.
                    if (s.find("sinkpool.mp4") != std::string::npos) continue;
                    if (s.find("MonaspaceNeon-Regular.otf") != std::string::npos) continue;
                    v = Value(contract_home(s));
                }
                if (same_value(*v, k->default_value)) continue;
                overrides[new_key] = Setting{ *v, 0 };
            }
            if (name == active) {
                if (auto sb = t->get("scrollback_lines"); sb && sb->as_integer()) {
                    const KeyInfo* k = find_key(Scope::Global, "terminal.scrollback_lines");
                    if (auto v = convert(*k, "scrollback_lines", *sb, ignored);
                        v && !same_value(*v, k->default_value)) {
                        out.global["terminal.scrollback_lines"] = Setting{ *v, 0 };
                    }
                }
            }
            converted[name] = std::move(overrides);
            order.push_back(name);
        }
    }

    for (const std::string& name : order) {
        if (is_builtin(name)) continue;
        out.presets[name] = converted[name];
        out.preset_order.push_back(name);
    }
    for (const std::string& name : order) {
        if (!is_builtin(name)) continue;
        // A built-in's old table that changes nothing is simply the built-in
        // now. One that does becomes an ordinary preset, so nothing is lost.
        if (converted[name].empty()) continue;
        std::string renamed = unique_preset_name(out, name + " copy");
        out.presets[renamed] = converted[name];
        out.preset_order.push_back(renamed);
        if (active == name) active = renamed;
    }
    if (!preset_exists(out, active)) active = "pool";
    out.global["preset"] = Setting{ active, 0 };
    return out;
}

// One legacy key=value file, as a version 0 preset table plus its name.
std::pair<std::string, toml::table> parse_legacy_preset(const std::string& contents,
                                                        const std::string& fallback_name) {
    std::string name = fallback_name;
    toml::table t;
    std::istringstream in(contents);
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        if (key == "name") {
            if (!val.empty()) name = val;
        } else if (key == "video_path" || key == "font_path") {
            t.insert_or_assign(key, val);
        } else if (key == "exposure" || key == "hue_shift") {
            try { t.insert_or_assign(key, std::stod(val)); } catch (...) {}
        } else if (key == "scrollback_lines") {
            try { t.insert_or_assign(key, static_cast<int64_t>(std::stoll(val))); } catch (...) {}
        } else if (key == "animated_typing" || key == "vibrancy_enabled" || key == "crt_mode_enabled" ||
                   key == "ligatures_enabled" || key == "hdr_console_enabled") {
            t.insert_or_assign(key, val == "true");
        }
    }
    return { name, std::move(t) };
}

} // namespace

std::string generate(const Config& c) {
    std::ostringstream o;
    o << kHeader;
    o << "version = " << kCurrentVersion << "\n";
    o << "preset = " << to_toml(Value(global_string(c, "preset"))) << "\n";

    SettingMap globals;
    for (const auto& [path, setting] : c.global) {
        const KeyInfo* k = find_key(Scope::Global, path);
        if (path == "preset" || !k || same_value(setting.value, k->default_value)) continue;
        globals[path] = setting;
    }
    emit_scope(o, Scope::Global, globals, "", true);

    for (const std::string& name : c.preset_order) {
        auto it = c.presets.find(name);
        if (it == c.presets.end()) continue;
        const std::string prefix = "presets." + toml_key(name) + ".";
        o << "\n[presets." << toml_key(name) << "]\n";
        emit_scope(o, Scope::Preset, it->second, prefix, true);
    }
    return o.str();
}

bool is_v0(const std::string& text) {
    toml::table doc;
    try {
        doc = toml::parse(text);
    } catch (const toml::parse_error&) {
        return false;
    }
    if (doc.contains("version")) return false;
    if (doc.contains("active_preset") || doc.contains("shell_integration") ||
        doc.contains("option_as_meta")) {
        return true;
    }
    if (const toml::table* presets = doc["presets"].as_table()) {
        for (auto&& [name, node] : *presets) {
            (void)name;
            if (const toml::table* t = node.as_table()) {
                for (const auto& [old_key, new_key] : kV0PresetKeys) {
                    if (std::string(old_key) != new_key && t->contains(old_key)) return true;
                }
                if (t->contains("scrollback_lines")) return true;
            }
        }
    }
    return false;
}

std::optional<std::string> migrate_v0(const std::string& text) {
    toml::table doc;
    try {
        doc = toml::parse(text);
    } catch (const toml::parse_error&) {
        return std::nullopt;
    }
    return generate(convert_v0(doc));
}

std::string migrate_legacy(const std::string& config_txt,
                           const std::map<std::string, std::string>& preset_files) {
    toml::table presets;
    for (const auto& [file_name, contents] : preset_files) {
        std::string fallback = file_name;
        if (fallback.size() > 4 && fallback.compare(fallback.size() - 4, 4, ".txt") == 0) {
            fallback.resize(fallback.size() - 4);
        }
        auto [name, table] = parse_legacy_preset(contents, fallback);
        presets.insert_or_assign(name, std::move(table));
    }

    // config.txt was either a pointer (active_preset=) once per-file presets
    // existed, or on an older install the whole configuration, with no
    // presets/ directory at all.
    std::string requested;
    bool flat = false;
    {
        std::istringstream in(config_txt);
        std::string line;
        while (std::getline(in, line)) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = line.substr(0, eq);
            if (key == "active_preset") requested = line.substr(eq + 1);
            else if (key == "video_path" || key == "font_path" || key == "exposure" ||
                     key == "animated_typing" || key == "vibrancy_enabled" ||
                     key == "crt_mode_enabled" || key == "ligatures_enabled") flat = true;
        }
    }
    std::string active = "pool";
    if (!requested.empty() && presets.contains(requested)) {
        active = requested;
    } else if (flat && !presets.contains("pool")) {
        presets.insert_or_assign("pool", parse_legacy_preset(config_txt, "pool").second);
    }

    toml::table doc;
    doc.insert("active_preset", active);
    doc.insert("presets", std::move(presets));
    return generate(convert_v0(doc));
}

// ---- Paths -------------------------------------------------------------------

bool is_path(const std::string& value) {
    return value.find('/') != std::string::npos || (!value.empty() && value[0] == '~');
}

std::string expand_home(const std::string& path) {
    const char* home = std::getenv("HOME");
    if (!home || path.empty() || path[0] != '~') return path;
    if (path.size() == 1) return home;
    if (path[1] == '/') return std::string(home) + path.substr(1);
    return path;  // ~otheruser/... is left alone
}

std::string contract_home(const std::string& path) {
    const char* home = std::getenv("HOME");
    if (!home || !*home) return path;
    const std::string h(home);
    if (path == h) return "~";
    if (path.size() > h.size() && path.compare(0, h.size(), h) == 0 && path[h.size()] == '/') {
        return "~" + path.substr(h.size());
    }
    return path;
}

// ---- Writing values ------------------------------------------------------------

namespace {

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char ch : s) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if (ch < 0x20 || ch == 0x7F) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04X", ch);
                    out += buf;
                } else {
                    out += static_cast<char>(ch);
                }
        }
    }
    return out + "\"";
}

} // namespace

std::string to_toml(const Value& v) {
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "true" : "false";
    if (std::holds_alternative<int64_t>(v)) return std::to_string(std::get<int64_t>(v));
    if (std::holds_alternative<double>(v)) return format_number(std::get<double>(v));
    if (std::holds_alternative<std::string>(v)) return quote(std::get<std::string>(v));
    const StringList& list = std::get<StringList>(v);
    std::string out = "[";
    for (size_t i = 0; i < list.size(); ++i) {
        if (i) out += ", ";
        out += quote(list[i]);
    }
    return out + "]";
}

std::string toml_key(const std::string& key) {
    bool bare = !key.empty() && std::all_of(key.begin(), key.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '_' || ch == '-';
    });
    return bare ? key : quote(key);
}

} // namespace config
