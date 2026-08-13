#include "preset_manager.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <sys/stat.h>

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string sanitize_slug(const std::string& name) {
    std::string slug;
    slug.reserve(name.size());
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') {
            slug += c;
        } else if (c == ' ') {
            slug += '_';
        }
    }
    if (slug.empty()) slug = "preset";
    return slug;
}

std::string preset_path(const std::string& name) {
    return presets::presets_dir() + "/" + sanitize_slug(name) + ".txt";
}

void ensure_dirs() {
    const char* home = getenv("HOME");
    if (!home) return;
    mkdir((std::string(home) + "/.config").c_str(), 0755);
    mkdir((std::string(home) + "/.config/sink").c_str(), 0755);
    mkdir(presets::presets_dir().c_str(), 0755);
}

} // namespace

namespace presets {

std::string presets_dir() {
    const char* home = getenv("HOME");
    if (!home) return "./.sink_presets";
    return std::string(home) + "/.config/sink/presets";
}

bool exists(const std::string& name) {
    struct stat st;
    return stat(preset_path(name).c_str(), &st) == 0;
}

Preset load(const std::string& name) {
    Preset p;
    p.name = name;

    std::ifstream f(preset_path(name));
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
        }
    }
    return p;
}

void save(const Preset& preset) {
    ensure_dirs();
    std::ofstream f(preset_path(preset.name));
    if (!f.is_open()) return;

    f << "name=" << preset.name << "\n";
    f << "video_path=" << preset.video_path << "\n";
    f << "font_path=" << preset.font_path << "\n";
    f << "exposure=" << preset.exposure << "\n";
    f << "hue_shift=" << preset.hue_shift << "\n";
    f << "animated_typing=" << (preset.animated_typing ? "true" : "false") << "\n";
    f << "vibrancy_enabled=" << (preset.vibrancy_enabled ? "true" : "false") << "\n";
    f << "crt_mode_enabled=" << (preset.crt_mode_enabled ? "true" : "false") << "\n";
    f << "ligatures_enabled=" << (preset.ligatures_enabled ? "true" : "false") << "\n";
    f << "hdr_console_enabled=" << (preset.hdr_console_enabled ? "true" : "false") << "\n";
}

std::vector<std::string> list_names() {
    ensure_dirs();
    std::vector<std::string> names;

    DIR* d = opendir(presets_dir().c_str());
    if (d) {
        struct dirent* entry;
        while ((entry = readdir(d)) != nullptr) {
            std::string fname = entry->d_name;
            if (fname.size() > 4 && fname.compare(fname.size() - 4, 4, ".txt") == 0) {
                // Slugs are idempotent under sanitize_slug, so re-deriving
                // the path from the bare filename loads the same file.
                Preset p = load(fname.substr(0, fname.size() - 4));
                names.push_back(p.name);
            }
        }
        closedir(d);
    }

    if (!exists("pool")) {
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

bool rename(const std::string& old_name, const std::string& new_name) {
    if (to_lower(old_name) == "pool") return false;
    if (new_name.empty()) return false;
    if (!exists(old_name)) return false;

    bool same_slug = sanitize_slug(new_name) == sanitize_slug(old_name);
    if (!same_slug && exists(new_name)) return false;

    Preset p = load(old_name);
    std::string old_path = preset_path(old_name);
    p.name = new_name;
    save(p);
    if (!same_slug) {
        ::remove(old_path.c_str());
    }
    return true;
}

bool remove(const std::string& name) {
    if (to_lower(name) == "pool") return false;
    if (!exists(name)) return false;
    return ::remove(preset_path(name).c_str()) == 0;
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

} // namespace presets
