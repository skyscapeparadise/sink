#include "keybindings.hpp"

#include <algorithm>
#include <cctype>

namespace keybindings {

const std::vector<Action>& actions() {
    static const std::vector<Action> a = {
        { "new_window",        "cmd+n",            "Open a new window." },
        { "new_tab",           "cmd+t",            "Open a new tab." },
        { "close_window",      "cmd+w",            "Close the window or tab." },
        { "settings",          "cmd+,",            "Open Settings." },
        { "print",             "cmd+p",            "Print the terminal's text." },
        { "cut",               "cmd+x",            "Cut the selection (where it's editable)." },
        { "copy",              "cmd+c",            "Copy the selection." },
        { "paste",             "cmd+v",            "Paste." },
        { "select_all",        "cmd+a",            "Select everything." },
        { "find",              "cmd+f",            "Show or hide the find bar." },
        { "clear_screen",      "cmd+k",            "Clear the screen and scrollback." },
        { "font_bigger",       "cmd+=",            "Make the text bigger in this window." },
        { "font_smaller",      "cmd+-",            "Make the text smaller in this window." },
        { "font_default",      "cmd+0",            "Return to the preset's font_size." },
        { "previous_prompt",   "cmd+up",           "Scroll to the prompt above (needs shell integration)." },
        { "next_prompt",       "cmd+down",         "Scroll to the prompt below." },
        { "split_right",       "cmd+d",            "Split the pane, the new one on the right." },
        { "split_down",        "cmd+shift+d",      "Split the pane, the new one below." },
        { "close_pane",        "cmd+shift+w",      "Close the pane." },
        { "select_pane_left",  "opt+cmd+left",     "Move to the pane on the left." },
        { "select_pane_right", "opt+cmd+right",    "Move to the pane on the right." },
        { "select_pane_up",    "opt+cmd+up",       "Move to the pane above." },
        { "select_pane_down",  "opt+cmd+down",     "Move to the pane below." },
        { "next_pane",         "cmd+]",            "Move to the next pane." },
        { "previous_pane",     "cmd+[",            "Move to the previous pane." },
        { "zoom_pane",         "cmd+shift+return", "Zoom the pane to fill the window, or back." },
        { "broadcast_input",   "",                 "Type into every pane at once, or stop." },
        { "select_tab_1",      "cmd+1",            "Show the first tab." },
        { "select_tab_2",      "cmd+2",            "Show the second tab." },
        { "select_tab_3",      "cmd+3",            "Show the third tab." },
        { "select_tab_4",      "cmd+4",            "Show the fourth tab." },
        { "select_tab_5",      "cmd+5",            "Show the fifth tab." },
        { "select_tab_6",      "cmd+6",            "Show the sixth tab." },
        { "select_tab_7",      "cmd+7",            "Show the seventh tab." },
        { "select_tab_8",      "cmd+8",            "Show the eighth tab." },
        { "select_last_tab",   "cmd+9",            "Show the last tab." },
    };
    return a;
}

const Action* find_action(const std::string& name) {
    for (const Action& a : actions()) {
        if (name == a.name) return &a;
    }
    return nullptr;
}

namespace {

std::string lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// The symbols menus show, as UTF-8, and the word each stands for.
const std::pair<const char*, const char*> kSymbols[] = {
    { "⌘", "cmd" },  { "⇧", "shift" }, { "⌥", "opt" },  { "⌃", "ctrl" },
    { "←", "left" }, { "→", "right" }, { "↑", "up" },   { "↓", "down" },
    { "↩", "return" }, { "⏎", "return" }, { "⇥", "tab" }, { "⎋", "escape" },
    { "⌫", "delete" }, { "⌦", "forward_delete" }, { "↖", "home" }, { "↘", "end" },
    { "⇞", "page_up" }, { "⇟", "page_down" },
};

const char* const kNamedKeys[] = {
    "up", "down", "left", "right", "return", "tab", "space", "escape", "delete",
    "forward_delete", "home", "end", "page_up", "page_down",
};

// Other spellings of a key name.
std::string canonical_key(const std::string& k) {
    if (k == "enter") return "return";
    if (k == "esc") return "escape";
    if (k == "backspace") return "delete";
    if (k == "del") return "forward_delete";
    if (k == "pageup" || k == "pgup") return "page_up";
    if (k == "pagedown" || k == "pgdn") return "page_down";
    if (k == "plus" || k == "+") return "=";  // ⌘+ is ⌘= on the keyboard
    if (k == "minus") return "-";
    if (k == "comma") return ",";
    if (k == "period") return ".";
    return k;
}

bool is_key_name(const std::string& k) {
    if (k.size() == 1) {
        const unsigned char ch = static_cast<unsigned char>(k[0]);
        return std::isalnum(ch) || std::ispunct(ch);
    }
    for (const char* n : kNamedKeys) {
        if (k == n) return true;
    }
    if (k.size() >= 2 && k[0] == 'f') {
        const std::string digits = k.substr(1);
        if (std::all_of(digits.begin(), digits.end(), [](unsigned char d) { return std::isdigit(d); })) {
            const int n = std::stoi(digits);
            return n >= 1 && n <= 20;
        }
    }
    return false;
}

// Splits "⌥⌘←" or "opt+cmd+left" into its parts: words between '+', with any
// symbols broken out as words of their own.
std::vector<std::string> tokens(const std::string& text) {
    std::vector<std::string> out;
    std::string word;
    auto flush = [&]() {
        if (!word.empty()) out.push_back(word);
        word.clear();
    };
    for (size_t i = 0; i < text.size();) {
        bool symbol = false;
        for (const auto& [sym, name] : kSymbols) {
            const std::string s = sym;
            if (text.compare(i, s.size(), s) == 0) {
                flush();
                out.push_back(name);
                i += s.size();
                symbol = true;
                break;
            }
        }
        if (symbol) continue;
        // A '+' joins parts -- unless it's the key itself ("cmd++", "⌘+").
        if (text[i] == '+' && !(word.empty() && (i + 1 == text.size()))) {
            flush();
            ++i;
            continue;
        }
        if (text[i] == ' ' || text[i] == '-' ) {
            // "cmd-shift-d" and "cmd shift d" read the same way, but "cmd+-"
            // is the minus key.
            if (text[i] == '-' && (i + 1 == text.size() || text[i + 1] == '+')) {
                word += text[i++];
                continue;
            }
            flush();
            ++i;
            continue;
        }
        word += text[i++];
    }
    flush();
    return out;
}

} // namespace

std::optional<Chord> parse_chord(const std::string& text) {
    std::vector<std::string> parts = tokens(text);
    if (parts.empty()) return std::nullopt;
    Chord c;
    for (size_t i = 0; i < parts.size(); ++i) {
        const std::string p = lower(parts[i]);
        const bool last = i + 1 == parts.size();
        if (p == "cmd" || p == "command") c.cmd = true;
        else if (p == "shift") c.shift = true;
        else if (p == "opt" || p == "option" || p == "alt") c.opt = true;
        else if (p == "ctrl" || p == "control") c.ctrl = true;
        else if (last && is_key_name(canonical_key(p))) c.key = canonical_key(p);
        else return std::nullopt;
    }
    if (c.key.empty()) return std::nullopt;
    return c;
}

std::string to_string(const Chord& c) {
    std::string s;
    if (c.ctrl) s += "ctrl+";
    if (c.opt) s += "opt+";
    if (c.shift) s += "shift+";
    if (c.cmd) s += "cmd+";
    return s + c.key;
}

bool is_reserved(const Chord& c) {
    if (!c.cmd || c.ctrl) return false;
    if (!c.opt && !c.shift && (c.key == "q" || c.key == "h" || c.key == "m" || c.key == "`" || c.key == "tab")) {
        return true;
    }
    if (c.opt && !c.shift && (c.key == "h" || c.key == "escape")) return true;  // hide others, force quit
    if (c.shift && !c.opt && c.key == "`") return true;
    return false;
}

std::vector<Binding> resolve(const std::vector<UserEntry>& user, std::vector<Problem>* problems) {
    auto report = [&](int line, const std::string& message) {
        if (problems) problems->push_back({ line, message });
    };

    struct Parsed {
        Chord chord;
        const UserEntry* entry;
    };
    std::vector<Parsed> parsed;
    for (const UserEntry& e : user) {
        std::optional<Chord> c = parse_chord(e.key);
        if (!c) {
            report(e.line, "keybindings: \"" + e.key + "\" isn't a key sink knows; skipping it");
            continue;
        }
        if (is_reserved(*c)) {
            report(e.line, "keybindings: " + to_string(*c) + " belongs to macOS and can't be changed; skipping it");
            continue;
        }
        if (!e.has_text && e.action != "none" && !find_action(e.action)) {
            report(e.line, "keybindings: \"" + e.action + "\" isn't an action (see sink --show-config --default); skipping it");
            continue;
        }
        parsed.push_back({ *c, &e });
    }

    // Defaults, minus actions the user moved and keys the user set.
    std::vector<Binding> out;
    for (const Action& a : actions()) {
        if (!*a.default_key) continue;
        const bool moved = std::any_of(parsed.begin(), parsed.end(), [&](const Parsed& p) {
            return !p.entry->has_text && p.entry->action == a.name;
        });
        if (moved) continue;
        const Chord c = *parse_chord(a.default_key);
        const bool taken = std::any_of(parsed.begin(), parsed.end(), [&](const Parsed& p) { return p.chord == c; });
        if (taken) continue;
        out.push_back({ c, a.name, "" });
    }
    // The user's, the last of any repeated key winning.
    for (const Parsed& p : parsed) {
        out.erase(std::remove_if(out.begin(), out.end(), [&](const Binding& b) { return b.chord == p.chord; }), out.end());
        if (p.entry->has_text) out.push_back({ p.chord, "", p.entry->text });
        else if (p.entry->action != "none") out.push_back({ p.chord, p.entry->action, "" });
    }
    return out;
}

const Binding* find(const std::vector<Binding>& bindings, const Chord& c) {
    for (const Binding& b : bindings) {
        if (b.chord == c) return &b;
    }
    return nullptr;
}

std::optional<Chord> key_for(const std::vector<Binding>& bindings, const std::string& action) {
    for (const Binding& b : bindings) {
        if (b.action == action) return b.chord;
    }
    return std::nullopt;
}

} // namespace keybindings
