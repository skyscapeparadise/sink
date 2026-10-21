#pragma once

// sink.toml's [keybindings]: which keys do what.
//
// Every shortcut sink has is an *action* with a default key. The user's
// table can move an action to another key, unbind a key ("none"), or make a
// key send text instead. The result is one map from key to binding, which
// drives both the menus (what they show) and sink's key handler (what
// happens) -- see main.cpp.
//
// Pure C++, no SDL or AppKit, so it can be tested on its own.

#include <optional>
#include <string>
#include <vector>

namespace keybindings {

// A key with its modifiers. `key` is normalized: a lowercase letter or digit,
// a punctuation character as typed (",", "=", "["), or a name -- up, down,
// left, right, return, tab, space, escape, delete (backspace),
// forward_delete, home, end, page_up, page_down, f1..f20.
struct Chord {
    bool cmd = false, shift = false, opt = false, ctrl = false;
    std::string key;
    bool operator==(const Chord&) const = default;
};

// "cmd+shift+d", "opt+cmd+left", "cmd+,", or the symbols menus use:
// "⌘⇧D", "⌥⌘←". Modifier words: cmd/command/⌘, shift/⇧, opt/option/alt/⌥,
// ctrl/control/⌃. Case doesn't matter. nullopt if it isn't a key.
std::optional<Chord> parse_chord(const std::string& text);

// "opt+cmd+left": the canonical spelling, as the reference documents it.
std::string to_string(const Chord& c);

// Shortcuts macOS keeps for itself (⌘Q, ⌘H, ⌘M, ⌘`, ⌘Tab...), which sink
// can't take over.
bool is_reserved(const Chord& c);

struct Action {
    const char* name;        // "split_right"
    const char* default_key; // "cmd+d", or "" for none
    const char* doc;
};

// Every action, in the order the reference lists them.
const std::vector<Action>& actions();
const Action* find_action(const std::string& name);

struct Binding {
    Chord chord;
    std::string action;  // an action's name, or "" when it sends text
    std::string text;    // what to send, when action is ""
};

// One entry of the user's [keybindings]: a key and what it's set to -- an
// action name, "none", or text (has_text).
struct UserEntry {
    std::string key;
    std::string action;
    std::string text;
    bool has_text = false;
    int line = 0;
};

struct Problem {
    int line = 0;
    std::string message;
};

// The bindings in effect: the defaults, with the user's entries on top. A key
// the user sets drops whatever it did before. An action the user binds to a
// new key *moves* there -- its default key is freed -- since a menu item can
// show only one shortcut (bind the old key too to keep both). Problems --
// keys that don't parse, unknown actions, reserved keys -- are reported and
// those entries skipped.
std::vector<Binding> resolve(const std::vector<UserEntry>& user, std::vector<Problem>* problems = nullptr);

// The binding for a key, if any.
const Binding* find(const std::vector<Binding>& bindings, const Chord& c);

// The key an action is bound to, if any (for showing it in the menus).
std::optional<Chord> key_for(const std::vector<Binding>& bindings, const std::string& action);

} // namespace keybindings
