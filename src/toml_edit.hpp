#pragma once

// Edits TOML text in place: changes the text of one key, or one table, and
// leaves every other byte -- comments, blank lines, ordering, quoting, the
// user's own formatting -- exactly as it was.
//
// toml++ can parse and serialize but not preserve formatting, so the edits
// work on the text itself, located with the source positions toml++ reports
// for each node. Only the simple shapes sink writes are edited this way:
// `key = value` lines inside [table] headers. Anything else (an inline table,
// a table built from dotted keys, a key on the same line as another) makes
// the edit decline, and every edit that does go ahead is re-parsed and
// checked to have done exactly what was asked. apply() returns nullopt in
// both cases; apply_or_regenerate() then falls back to rewriting the whole
// document with toml++, which keeps every key but not comments.

#include <optional>
#include <string>
#include <vector>

namespace toml_edit {

using Path = std::vector<std::string>;  // {"presets", "pool copy"}; {} is the top level

struct Edit {
    enum class Kind {
        Set,          // table.key = value_text, adding the key (and table) if missing
        AddTable,     // an empty [table], if there isn't one already
        Remove,       // delete table.key; nothing to do if it isn't there
        RemoveTable,  // delete the table and everything under it
        RenameTable   // give the table (and everything under it) the last segment `new_name`
    };
    Kind kind = Kind::Set;
    Path table;
    std::string key;         // Set, Remove
    std::string value_text;  // Set: the value as TOML source, e.g. "0.5" or "\"~/a.mp4\""
    std::string new_name;    // RenameTable
};

// The edited text, or nullopt if the edit can't be made in place (see above)
// or the text doesn't parse.
std::optional<std::string> apply(const std::string& text, const Edit& edit);

// apply(), falling back to regenerating the document when an in-place edit
// isn't possible. `regenerated` says which happened, so the caller can keep a
// backup of what the user wrote before the comments go. nullopt only when
// `text` itself doesn't parse.
std::optional<std::string> apply_or_regenerate(const std::string& text, const Edit& edit, bool& regenerated);

} // namespace toml_edit
