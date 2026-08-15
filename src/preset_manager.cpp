#include "preset_manager.hpp"

// toml++ v3.4.0 still uses the pre-C++23 (deprecated-but-not-removed)
// whitespace-before-literal-operator-suffix syntax internally; harmless,
// but silence it rather than let a third-party header's own deprecation
// notice show up in sink's build output.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-literal-operator"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-literal-operator"
#endif
#include <toml.hpp>
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/stat.h>

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string config_dir() {
    const char* home = getenv("HOME");
    if (!home) return "./.sink_config";
    return std::string(home) + "/.config/sink";
}

void ensure_config_dir() {
    const char* home = getenv("HOME");
    if (!home) return;
    mkdir((std::string(home) + "/.config").c_str(), 0755);
    mkdir(config_dir().c_str(), 0755);
}

// A plain float->double cast preserves the float's exact binary value,
// which almost never has a short decimal representation (e.g. 0.834762f
// becomes 0.83476197719573975) -- toml++'s round-trip-safe float
// serializer then has to print that whole tail. Rounding to a fixed
// *decimal place count* doesn't reliably fix this either: a float only
// carries ~7 significant digits total, so for a larger value like
// 70.1367 (3 integer digits), rounding to 6 decimal *places* rounds well
// past where the float actually has any precision left, and the noise
// comes right back. Routing through ostream's default float formatting
// (6 significant digits, matching what the pre-TOML flat-file format
// already used) and reparsing that gives the same clean value a human
// would have typed, regardless of magnitude.
double clean_double(float v) {
    std::ostringstream ss;
    ss << v;
    return std::stod(ss.str());
}

toml::table preset_to_table(const Preset& p) {
    toml::table t;
    t.insert("video_path", p.video_path);
    t.insert("font_path", p.font_path);
    t.insert("exposure", clean_double(p.exposure));
    t.insert("hue_shift", clean_double(p.hue_shift));
    t.insert("animated_typing", p.animated_typing);
    t.insert("vibrancy_enabled", p.vibrancy_enabled);
    t.insert("crt_mode_enabled", p.crt_mode_enabled);
    t.insert("ligatures_enabled", p.ligatures_enabled);
    t.insert("hdr_console_enabled", p.hdr_console_enabled);
    t.insert("scrollback_lines", static_cast<int64_t>(p.scrollback_lines));
    return t;
}

// `name` comes from the [presets.<name>] table key, not a field inside the
// table, so it's supplied separately rather than read back out of `t`.
Preset table_to_preset(const std::string& name, const toml::table& t) {
    Preset p;
    p.name = name;
    p.video_path = t["video_path"].value_or(p.video_path);
    p.font_path = t["font_path"].value_or(p.font_path);
    p.exposure = static_cast<float>(t["exposure"].value_or(static_cast<double>(p.exposure)));
    p.hue_shift = static_cast<float>(t["hue_shift"].value_or(static_cast<double>(p.hue_shift)));
    p.animated_typing = t["animated_typing"].value_or(p.animated_typing);
    p.vibrancy_enabled = t["vibrancy_enabled"].value_or(p.vibrancy_enabled);
    p.crt_mode_enabled = t["crt_mode_enabled"].value_or(p.crt_mode_enabled);
    p.ligatures_enabled = t["ligatures_enabled"].value_or(p.ligatures_enabled);
    p.hdr_console_enabled = t["hdr_console_enabled"].value_or(p.hdr_console_enabled);
    p.scrollback_lines = t["scrollback_lines"].value_or(p.scrollback_lines);
    return p;
}

toml::table load_document() {
    std::ifstream f(presets::config_path());
    if (!f.is_open()) return toml::table{};
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        return toml::parse(ss.str());
    } catch (const toml::parse_error& e) {
        std::cerr << "sink.toml: parse error, ignoring existing file: " << e.what() << std::endl;
        return toml::table{};
    }
}

void save_document(const toml::table& doc) {
    ensure_config_dir();
    std::ofstream f(presets::config_path());
    if (!f.is_open()) return;
    f << "# sink terminal configuration\n"
      << "#\n"
      << "# Hand-editable. Each [presets.<name>] section below is a switchable\n"
      << "# visual profile (background, font, exposure, and behavior toggles);\n"
      << "# `active_preset` selects which one is currently applied. \"pool\" is\n"
      << "# the permanent default and can't be renamed or removed.\n"
      << "#\n"
      << "# Note: sink regenerates this whole file on every save (including ones\n"
      << "# triggered by the Settings panel), so this header comment always comes\n"
      << "# back, but a comment you add inside a [presets.*] section won't survive\n"
      << "# the next GUI-triggered save.\n\n"
      << toml::toml_formatter{
             doc, toml::toml_formatter::default_flags | toml::format_flags::relaxed_float_precision
         }
      << "\n";
}

toml::table* presets_subtable(toml::table& doc, bool create_if_missing) {
    toml::table* sub = doc["presets"].as_table();
    if (!sub && create_if_missing) {
        doc.insert("presets", toml::table{});
        sub = doc["presets"].as_table();
    }
    return sub;
}

// ---- Legacy (pre-TOML) format, read-only, migration use only -------------

std::string legacy_presets_dir() { return config_dir() + "/presets"; }
std::string legacy_config_txt_path() { return config_dir() + "/config.txt"; }

// Parses one legacy key=value file. Used for both the per-preset files
// under presets/*.txt (which include `name`) and the even-older flat
// config.txt (which doesn't -- pre-preset installs kept everything in one
// file with no `name` key at all, hence the fallback_name parameter).
Preset parse_legacy_kv_file(const std::string& path, const std::string& fallback_name) {
    Preset p;
    p.name = fallback_name;
    std::ifstream f(path);
    if (!f.is_open()) return p;
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        if (key == "name") {
            if (!val.empty()) p.name = val;
        } else if (key == "video_path") {
            p.video_path = val;
        } else if (key == "font_path") {
            p.font_path = val;
        } else if (key == "exposure") {
            try { p.exposure = std::stof(val); } catch (...) {}
        } else if (key == "hue_shift") {
            try { p.hue_shift = std::stof(val); } catch (...) {}
        } else if (key == "animated_typing") {
            p.animated_typing = (val == "true");
        } else if (key == "vibrancy_enabled") {
            p.vibrancy_enabled = (val == "true");
        } else if (key == "crt_mode_enabled") {
            p.crt_mode_enabled = (val == "true");
        } else if (key == "ligatures_enabled") {
            p.ligatures_enabled = (val == "true");
        } else if (key == "hdr_console_enabled") {
            p.hdr_console_enabled = (val == "true");
        } else if (key == "scrollback_lines") {
            try { p.scrollback_lines = std::clamp(std::stoi(val), 100, 1000000); } catch (...) {}
        }
    }
    return p;
}

} // namespace

namespace presets {

std::string config_path() {
    return config_dir() + "/sink.toml";
}

void migrate_legacy_config_if_needed() {
    struct stat st;
    if (stat(config_path().c_str(), &st) == 0) return; // already migrated (or a fresh install writes its own)

    toml::table presets_tbl;

    // Every preset that was ever saved as its own presets/<slug>.txt file --
    // migrated wholesale, not just whichever one happens to be active, so a
    // long-time user doesn't lose their other saved presets in the move.
    DIR* d = opendir(legacy_presets_dir().c_str());
    if (d) {
        struct dirent* entry;
        while ((entry = readdir(d)) != nullptr) {
            std::string fname = entry->d_name;
            if (fname.size() > 4 && fname.compare(fname.size() - 4, 4, ".txt") == 0) {
                std::string fallback_name = fname.substr(0, fname.size() - 4);
                Preset p = parse_legacy_kv_file(legacy_presets_dir() + "/" + fname, fallback_name);
                presets_tbl.insert_or_assign(p.name, preset_to_table(p));
            }
        }
        closedir(d);
    }

    // config.txt historically served two different roles depending on
    // install age: a pointer (`active_preset=`) once per-file presets
    // existed, or -- on an even older install -- the terminal's *entire*
    // flat configuration, with no presets/ directory at all. Both are
    // handled by reading the same file for both purposes at once, exactly
    // mirroring what the pre-TOML load_config() used to do.
    std::string requested_preset;
    bool has_active_preset_key = false;
    bool has_legacy_flat_fields = false;
    {
        std::ifstream f(legacy_config_txt_path());
        std::string line;
        while (std::getline(f, line)) {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            if (key == "active_preset") {
                requested_preset = line.substr(eq + 1);
                has_active_preset_key = true;
            } else if (key == "video_path" || key == "font_path" || key == "exposure" ||
                       key == "animated_typing" || key == "vibrancy_enabled" ||
                       key == "crt_mode_enabled" || key == "ligatures_enabled") {
                has_legacy_flat_fields = true;
            }
        }
    }

    std::string active_name;
    if (has_active_preset_key && !requested_preset.empty() && presets_tbl.contains(requested_preset)) {
        active_name = requested_preset;
    } else if (has_legacy_flat_fields && !presets_tbl.contains("pool")) {
        Preset legacy = parse_legacy_kv_file(legacy_config_txt_path(), "pool");
        presets_tbl.insert_or_assign("pool", preset_to_table(legacy));
        active_name = "pool";
    } else if (presets_tbl.contains("pool")) {
        active_name = "pool";
    } else {
        active_name = "pool"; // fresh install: seeded with defaults below
    }

    if (!presets_tbl.contains("pool")) {
        presets_tbl.insert("pool", preset_to_table(Preset{}));
    }

    toml::table doc;
    doc.insert("active_preset", active_name);
    doc.insert("presets", std::move(presets_tbl));
    save_document(doc);
}

std::vector<std::string> list_names() {
    toml::table doc = load_document();
    std::vector<std::string> names;
    if (toml::table* sub = presets_subtable(doc, false)) {
        for (auto&& [key, val] : *sub) {
            (void)val;
            names.emplace_back(key.str());
        }
    }
    if (std::find_if(names.begin(), names.end(),
                     [](const std::string& n) { return to_lower(n) == "pool"; }) == names.end()) {
        names.push_back("pool");
    }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        bool a_pool = to_lower(a) == "pool";
        bool b_pool = to_lower(b) == "pool";
        if (a_pool != b_pool) return a_pool;
        return to_lower(a) < to_lower(b);
    });
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

bool exists(const std::string& name) {
    toml::table doc = load_document();
    toml::table* sub = presets_subtable(doc, false);
    return sub && sub->contains(name);
}

Preset load(const std::string& name) {
    toml::table doc = load_document();
    if (toml::table* sub = presets_subtable(doc, false)) {
        if (toml::table* preset_tbl = (*sub)[name].as_table()) {
            return table_to_preset(name, *preset_tbl);
        }
    }
    Preset p;
    p.name = name;
    return p;
}

void save(const Preset& preset) {
    toml::table doc = load_document();
    toml::table* sub = presets_subtable(doc, true);
    sub->insert_or_assign(preset.name, preset_to_table(preset));
    save_document(doc);
}

bool rename(const std::string& old_name, const std::string& new_name) {
    if (to_lower(old_name) == "pool") return false;
    if (new_name.empty()) return false;
    if (old_name == new_name) return true;

    toml::table doc = load_document();
    toml::table* sub = presets_subtable(doc, false);
    if (!sub || !sub->contains(old_name)) return false;
    if (sub->contains(new_name)) return false;

    toml::table moved = *sub->get(old_name)->as_table();
    sub->erase(old_name);
    sub->insert(new_name, std::move(moved));
    save_document(doc);
    return true;
}

bool remove(const std::string& name) {
    if (to_lower(name) == "pool") return false;
    toml::table doc = load_document();
    toml::table* sub = presets_subtable(doc, false);
    if (!sub || !sub->contains(name)) return false;
    sub->erase(name);
    save_document(doc);
    return true;
}

std::string unique_name(const std::string& base) {
    std::string candidate = base.empty() ? "preset" : base;
    if (!exists(candidate)) return candidate;
    for (int i = 2; i < 1000; ++i) {
        candidate = base + " " + std::to_string(i);
        if (!exists(candidate)) return candidate;
    }
    return base + " " + std::to_string(static_cast<long>(std::time(nullptr)));
}

std::string get_active_preset_name() {
    toml::table doc = load_document();
    return doc["active_preset"].value_or(std::string("pool"));
}

void set_active_preset_name(const std::string& name) {
    toml::table doc = load_document();
    doc.insert_or_assign("active_preset", name);
    save_document(doc);
}

} // namespace presets
