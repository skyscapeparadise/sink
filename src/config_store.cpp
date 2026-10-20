#include "config_store.hpp"

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {

uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string dirname_of(const std::string& path) {
    size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string(".") : (slash == 0 ? std::string("/") : path.substr(0, slash));
}

void make_dirs(const std::string& dir) {
    if (dir.empty() || dir == "/" || dir == ".") return;
    struct stat st;
    if (stat(dir.c_str(), &st) == 0) return;
    make_dirs(dirname_of(dir));
    mkdir(dir.c_str(), 0755);
}

std::optional<std::string> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return std::nullopt;
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// A backup beside `path`, never replacing an earlier one: name, name.1, ...
void write_backup(const std::string& path, const std::string& text) {
    std::string name = path;
    struct stat st;
    for (int i = 1; stat(name.c_str(), &st) == 0; ++i) name = path + "." + std::to_string(i);
    std::ofstream f(name, std::ios::binary);
    f << text;
}

// Splits a dotted registry path ("terminal.scrollback_lines") into the
// table it lives in and its key.
std::pair<toml_edit::Path, std::string> split(const std::string& dotted) {
    toml_edit::Path parts;
    size_t start = 0;
    for (size_t dot; (dot = dotted.find('.', start)) != std::string::npos; start = dot + 1) {
        parts.push_back(dotted.substr(start, dot - start));
    }
    return { parts, dotted.substr(start) };
}

bool same_value(const config::Value& a, const config::Value& b) {
    if (std::holds_alternative<double>(a) && std::holds_alternative<double>(b)) {
        return std::abs(std::get<double>(a) - std::get<double>(b)) < 1e-6;
    }
    return a == b;
}

} // namespace

std::string ConfigStore::default_path() {
    // For trying a configuration, or running two copies of sink, without
    // touching the usual one.
    if (const char* override_path = std::getenv("SINK_CONFIG"); override_path && *override_path) {
        return config::expand_home(override_path);
    }
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.config/sink/sink.toml";
}

ConfigStore::ConfigStore(std::string path) : path_(std::move(path)) {}

ConfigStore::FileStamp ConfigStore::stamp_of(const std::string& path) {
    FileStamp s;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return s;
    s.exists = true;
    s.mtime_ns = static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1000000000 + st.st_mtimespec.tv_nsec;
    s.size = static_cast<int64_t>(st.st_size);
    s.inode = static_cast<uint64_t>(st.st_ino);
    return s;
}

void ConfigStore::load() {
    std::optional<std::string> text = read_file(path_);

    if (!text) {
        // No sink.toml. The pre-TOML files, if any, become it; otherwise a
        // fresh one, so there is a file to find and edit.
        const std::string dir = dirname_of(path_);
        std::optional<std::string> config_txt = read_file(dir + "/config.txt");
        std::map<std::string, std::string> preset_files;
        if (DIR* d = opendir((dir + "/presets").c_str())) {
            while (struct dirent* entry = readdir(d)) {
                std::string name = entry->d_name;
                if (name.size() > 4 && name.compare(name.size() - 4, 4, ".txt") == 0) {
                    if (auto contents = read_file(dir + "/presets/" + name)) preset_files[name] = *contents;
                }
            }
            closedir(d);
        }
        if (config_txt || !preset_files.empty()) {
            write(config::migrate_legacy(config_txt.value_or(""), preset_files));
        } else {
            config::Config fresh;
            fresh.global["preset"] = config::Setting{ std::string("pool"), 0 };
            write(config::generate(fresh));
        }
    } else if (config::is_v0(*text)) {
        if (auto migrated = config::migrate_v0(*text)) {
            write_backup(path_ + ".v0.bak", *text);
            write(*migrated);
        }
    }

    read_from_disk();
    known_ = stamp();
    rebuild();
}

void ConfigStore::read_from_disk() {
    base_text_ = read_file(path_).value_or("");
    config::Diagnostic err;
    if (config::read_text(base_text_, err)) {
        error_.reset();
        last_good_text_ = base_text_;
    } else {
        error_ = err;
        std::cerr << "sink.toml: line " << err.line << ": " << err.message
                  << " -- using the last good settings until it's fixed" << std::endl;
    }
}

bool ConfigStore::reload_if_changed() {
    FileStamp now = stamp();
    if (now == known_) {
        // sink.toml is as it was; an included file may not be.
        for (const auto& [file, seen] : included_) {
            if (!(stamp_of(file) == seen)) {
                read_config();
                return true;
            }
        }
        return false;
    }
    known_ = now;
    read_from_disk();
    rebuild();
    return true;
}

void ConfigStore::rebuild() {
    working_text_ = last_good_text_;
    backup_due_ = false;
    for (const toml_edit::Edit& e : pending_) {
        bool regen = false;
        if (auto next = toml_edit::apply_or_regenerate(working_text_, e, regen)) {
            working_text_ = *next;
            if (regen) backup_due_ = regenerated_ = true;
        }
    }
    read_config();
    dirty_ = !pending_.empty();
}

void ConfigStore::read_config() {
    config::Diagnostic err;
    auto c = config::read_with_includes(working_text_, path_, err, [](const std::string& file) {
        return read_file(file);
    });
    if (!c) return;
    config_ = std::move(*c);
    included_.clear();
    for (const std::string& file : config_.included_files) included_.push_back({ file, stamp_of(file) });
}

void ConfigStore::edit(const toml_edit::Edit& e) {
    pending_.push_back(e);
    last_edit_ms_ = now_ms();
    rebuild();
}

void ConfigStore::flush_if_due(uint64_t quiet_ms) {
    if (dirty_ && now_ms() - last_edit_ms_ >= quiet_ms) flush();
}

void ConfigStore::flush() {
    if (!dirty_ || error_) return;
    // Something else may have written the file since it was read; apply the
    // pending changes to that rather than overwrite it.
    if (!(stamp() == known_)) {
        known_ = stamp();
        read_from_disk();
        rebuild();
        if (error_) return;
    }
    if (backup_due_) write_backup(path_ + ".bak", base_text_);
    if (write(working_text_)) {
        base_text_ = last_good_text_ = working_text_;
        pending_.clear();
        dirty_ = false;
        known_ = stamp();
    }
}

bool ConfigStore::write(const std::string& text) {
    // Through a symlink to the real file, so a sink.toml kept in a dotfiles
    // repo and linked into place stays linked.
    std::string target = path_;
    char resolved[PATH_MAX];
    if (realpath(path_.c_str(), resolved)) target = resolved;
    const std::string dir = dirname_of(target);
    make_dirs(dir);

    mode_t mode = 0644;
    struct stat st;
    if (stat(target.c_str(), &st) == 0) mode = st.st_mode & 07777;

    // Written beside the file and renamed over it: a crash or a full disk
    // leaves the old file whole rather than half of the new one.
    std::string tmp = dir + "/.sink.toml.XXXXXX";
    std::vector<char> tmpl(tmp.begin(), tmp.end());
    tmpl.push_back('\0');
    int fd = mkstemp(tmpl.data());
    if (fd < 0) {
        std::cerr << "sink.toml: can't save: " << std::strerror(errno) << std::endl;
        return false;
    }
    bool ok = true;
    for (size_t done = 0; ok && done < text.size();) {
        ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) ok = false;
        else done += static_cast<size_t>(n);
    }
    ok = ok && fchmod(fd, mode) == 0 && fsync(fd) == 0;
    ok = (close(fd) == 0) && ok;
    if (!ok || rename(tmpl.data(), target.c_str()) != 0) {
        std::cerr << "sink.toml: can't save: " << std::strerror(errno) << std::endl;
        unlink(tmpl.data());
        return false;
    }
    return true;
}

// ---- Changes -------------------------------------------------------------------

void ConfigStore::set_global(const std::string& path, const config::Value& value) {
    const config::KeyInfo* k = config::find_key(config::Scope::Global, path);
    if (!k) return;
    auto [table, key] = split(path);
    toml_edit::Edit e;
    e.table = table;
    e.key = key;
    if (path != "preset" && same_value(value, k->default_value)) {
        e.kind = toml_edit::Edit::Kind::Remove;
    } else {
        e.kind = toml_edit::Edit::Kind::Set;
        e.value_text = config::to_toml(value);
    }
    edit(e);
}

void ConfigStore::set_preset(const std::string& preset, const std::string& path, const config::Value& value) {
    const config::KeyInfo* k = config::find_key(config::Scope::Preset, path);
    if (!k || config::is_builtin(preset)) return;
    config::Value v = value;
    if (k->type == config::KeyType::Path && std::holds_alternative<std::string>(v) &&
        config::is_path(std::get<std::string>(v))) {
        v = config::contract_home(std::get<std::string>(v));
    }
    auto [table, key] = split(path);
    toml_edit::Edit e;
    e.table = { "presets", preset };
    e.table.insert(e.table.end(), table.begin(), table.end());
    e.key = key;
    if (same_value(v, k->default_value)) {
        e.kind = toml_edit::Edit::Kind::Remove;
    } else {
        e.kind = toml_edit::Edit::Kind::Set;
        e.value_text = config::to_toml(v);
    }
    edit(e);
}

std::string ConfigStore::unique_name(const std::string& base) const {
    const std::string b = base.empty() ? std::string("preset") : base;
    if (!config::preset_exists(config_, b)) return b;
    for (int i = 2;; ++i) {
        std::string candidate = b + " " + std::to_string(i);
        if (!config::preset_exists(config_, candidate)) return candidate;
    }
}

std::string ConfigStore::create_preset(const std::string& base,
                                       const std::vector<std::pair<std::string, config::Value>>& overrides) {
    const std::string name = unique_name(base);
    toml_edit::Edit e;
    e.kind = toml_edit::Edit::Kind::AddTable;
    e.table = { "presets", name };
    edit(e);
    for (const auto& [path, value] : overrides) set_preset(name, path, value);
    return name;
}

std::string ConfigStore::duplicate_preset(const std::string& name) {
    std::vector<std::pair<std::string, config::Value>> overrides;
    for (const config::BuiltinPreset& b : config::builtin_presets()) {
        if (name == b.name) overrides = b.overrides;
    }
    if (auto it = config_.presets.find(name); it != config_.presets.end()) {
        for (const auto& [path, setting] : it->second) overrides.push_back({ path, setting.value });
    }
    return create_preset(name + " copy", overrides);
}

bool ConfigStore::rename_preset(const std::string& from, const std::string& to) {
    if (!can_rename_or_delete(from)) return false;
    if (to.empty() || config::preset_exists(config_, to)) return false;
    const bool active = config::active_preset(config_) == from;
    toml_edit::Edit e;
    e.kind = toml_edit::Edit::Kind::RenameTable;
    e.table = { "presets", from };
    e.new_name = to;
    edit(e);
    if (active) set_global("preset", config::Value(to));
    return true;
}

bool ConfigStore::remove_preset(const std::string& name) {
    if (!can_rename_or_delete(name)) return false;
    const bool active = config::active_preset(config_) == name;
    toml_edit::Edit e;
    e.kind = toml_edit::Edit::Kind::RemoveTable;
    e.table = { "presets", name };
    edit(e);
    if (active) set_global("preset", config::Value(std::string(config::builtin_presets().front().name)));
    return true;
}
