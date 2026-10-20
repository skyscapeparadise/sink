#pragma once

// The sink.toml file itself: loading it (migrating older formats first),
// editing it in place, saving it safely, and noticing when it changes on disk.
//
// Edits are applied to the in-memory text at once, so config() always
// reflects them, and written out once they stop coming (see flush_if_due), so
// dragging a slider is one write rather than one per frame. While the file on
// disk has a syntax error nothing is written -- the user is in the middle of
// editing it -- and edits wait, to be applied on top of the file once it
// parses again.

#include "config.hpp"
#include "toml_edit.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class ConfigStore {
public:
    // $SINK_CONFIG if set, else ~/.config/sink/sink.toml
    static std::string default_path();

    explicit ConfigStore(std::string path = default_path());

    // Reads the file, creating it if there is none (from the pre-TOML files
    // when those exist) and migrating it if it's an older format. The
    // original of a migrated file is kept beside it as sink.toml.v0.bak.
    void load();

    // Re-reads the file if something other than this store changed it since
    // the last read or write. True if the configuration may have changed.
    bool reload_if_changed();

    const config::Config& config() const { return config_; }

    // The file's syntax error, while it has one. config() is then the last
    // configuration that did parse (the defaults, if none has), and nothing is
    // written until the file is fixed.
    const std::optional<config::Diagnostic>& error() const { return error_; }

    // Set when a change couldn't be made in place and the file was rewritten
    // whole, losing comments. The previous text is in sink.toml.bak.
    bool regenerated() const { return regenerated_; }

    // Whether Settings can rename or delete this preset: only presets with a
    // table in sink.toml itself, since sink never writes included files.
    bool can_rename_or_delete(const std::string& preset) const {
        return !config::is_builtin(preset) && config::is_own_preset(config_, preset);
    }

    // ---- Changes --------------------------------------------------------------
    //
    // Values equal to the default are removed rather than written, so a preset
    // only ever lists what it changes.

    void set_global(const std::string& path, const config::Value& value);
    void set_preset(const std::string& preset, const std::string& path, const config::Value& value);

    // A new preset with `overrides` (preset-scope paths), named `base` or
    // `base 2`, `base 3`... whichever is free. Returns the name used.
    std::string create_preset(const std::string& base,
                              const std::vector<std::pair<std::string, config::Value>>& overrides = {});

    // A copy of any preset, built-in or not, as a new preset named
    // "<name> copy" (or "<name> copy 2"...). Returns the new name.
    std::string duplicate_preset(const std::string& name);

    // False (and nothing changes) for built-ins, unknown names, and taken or
    // empty new names.
    bool rename_preset(const std::string& from, const std::string& to);
    bool remove_preset(const std::string& name);

    // A free preset name: `base`, else `base 2`, `base 3`...
    std::string unique_name(const std::string& base) const;

    // ---- Saving -----------------------------------------------------------------

    // Writes pending changes once `quiet_ms` have passed since the last one.
    void flush_if_due(uint64_t quiet_ms = 400);
    // Writes pending changes now (at quit).
    void flush();
    bool dirty() const { return dirty_; }

    const std::string& path() const { return path_; }

private:
    struct FileStamp {
        bool exists = false;
        int64_t mtime_ns = 0;
        int64_t size = 0;
        uint64_t inode = 0;
        bool operator==(const FileStamp& o) const {
            return exists == o.exists && mtime_ns == o.mtime_ns && size == o.size && inode == o.inode;
        }
    };

    void edit(const toml_edit::Edit& e);
    void rebuild();                      // (last good) text + pending_ -> working_text_, config_
    void read_from_disk();
    bool write(const std::string& text); // atomic, through symlinks
    FileStamp stamp() const { return stamp_of(path_); }
    static FileStamp stamp_of(const std::string& path);
    // Reads the working text with its includes into config_, noting each
    // included file's stamp so live reload sees them change too.
    void read_config();

    std::string path_;
    std::string base_text_;              // the file as last read or written
    std::string last_good_text_;         // the last version of it that parsed
    std::string working_text_;           // base_text_ with pending_ applied
    std::vector<toml_edit::Edit> pending_;
    config::Config config_;
    std::optional<config::Diagnostic> error_;
    bool regenerated_ = false;
    bool backup_due_ = false;            // pending_ needed a regeneration: back up first
    bool dirty_ = false;
    uint64_t last_edit_ms_ = 0;          // steady clock
    FileStamp known_;                    // the file as this store last saw or left it
    std::vector<std::pair<std::string, FileStamp>> included_;  // ...and the files it includes
};
