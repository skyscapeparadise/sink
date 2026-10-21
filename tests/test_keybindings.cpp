// Tests for keybindings.cpp: reading keys in either spelling, and resolving
// the user's [keybindings] over the defaults.
#include "keybindings.hpp"

#include <cstdio>
#include <string>

static int checks_failed = 0;
static int checks_run = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++checks_run;                                                        \
        if (!(cond)) {                                                       \
            ++checks_failed;                                                 \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)

using namespace keybindings;

static std::string key(const std::string& text) {
    auto c = parse_chord(text);
    return c ? to_string(*c) : std::string("(none)");
}

static void test_parse() {
    CHECK(key("cmd+shift+d") == "shift+cmd+d");
    CHECK(key("Cmd+Shift+D") == "shift+cmd+d");
    CHECK(key("⌘⇧D") == "shift+cmd+d");             // ⌘⇧D
    CHECK(key("⌥⌘←") == "opt+cmd+left");        // ⌥⌘←
    CHECK(key("option+command+left") == "opt+cmd+left");
    CHECK(key("alt+cmd+left") == "opt+cmd+left");
    CHECK(key("cmd+,") == "cmd+,");
    CHECK(key("cmd+-") == "cmd+-");
    CHECK(key("cmd+=") == "cmd+=");
    CHECK(key("cmd++") == "cmd+=");                            // the = key
    CHECK(key("⌘+") == "cmd+=");
    CHECK(key("cmd+[") == "cmd+[");
    CHECK(key("ctrl+shift+enter") == "ctrl+shift+return");
    CHECK(key("cmd-shift-k") == "shift+cmd+k");
    CHECK(key("ctrl+backspace") == "ctrl+delete");
    CHECK(key("f12") == "f12");
    CHECK(key("cmd+pageup") == "cmd+page_up");
    CHECK(key("") == "(none)");
    CHECK(key("cmd+") == "(none)");
    CHECK(key("cmd+banana") == "(none)");
    CHECK(key("hyper+k") == "(none)");
    CHECK(key("f21") == "(none)");
}

static void test_defaults() {
    std::vector<Binding> b = resolve({});
    const Binding* d = find(b, *parse_chord("cmd+d"));
    CHECK(d && d->action == "split_right");
    CHECK(find(b, *parse_chord("cmd+9"))->action == "select_last_tab");
    CHECK(!key_for(b, "broadcast_input"));  // no default key
    for (const Action& a : actions()) {
        if (*a.default_key) CHECK(parse_chord(a.default_key).has_value());
    }
}

static void test_user_bindings() {
    std::vector<Problem> problems;
    std::vector<UserEntry> user = {
        { "cmd+shift+e", "split_right", "", false, 3 },     // moves split_right
        { "cmd+k", "none", "", false, 4 },                  // unbinds clear screen
        { "cmd+u", "", "\x15", true, 5 },                   // sends ^U
        { "cmd+t", "", "tmux\n", true, 6 },                 // takes cmd+t from new_tab
        { "ctrl+b", "broadcast_input", "", false, 7 },
        { "cmd+banana", "copy", "", false, 8 },
        { "cmd+l", "make_coffee", "", false, 9 },
        { "cmd+q", "copy", "", false, 10 },
    };
    std::vector<Binding> b = resolve(user, &problems);

    CHECK(find(b, *parse_chord("cmd+shift+e"))->action == "split_right");
    CHECK(!find(b, *parse_chord("cmd+d")));                 // moved, so freed
    CHECK(!find(b, *parse_chord("cmd+k")));
    CHECK(!key_for(b, "clear_screen"));                     // only key was unbound
    const Binding* u = find(b, *parse_chord("cmd+u"));
    CHECK(u && u->action.empty() && u->text == "\x15");
    CHECK(find(b, *parse_chord("cmd+t"))->text == "tmux\n");
    CHECK(!key_for(b, "new_tab"));
    CHECK(key_for(b, "broadcast_input").has_value());
    CHECK(key_for(b, "copy") && to_string(*key_for(b, "copy")) == "cmd+c");  // bad entries changed nothing

    CHECK(problems.size() == 3);
    CHECK(problems.size() == 3 && problems[0].line == 8 && problems[1].line == 9 && problems[2].line == 10);
    CHECK(problems.size() == 3 && problems[2].message.find("belongs to macOS") != std::string::npos);

    // Keeping both keys: bind the old one too.
    std::vector<Binding> both = resolve({ { "cmd+shift+e", "split_right", "", false, 1 },
                                          { "cmd+d", "split_right", "", false, 2 } });
    CHECK(find(both, *parse_chord("cmd+d")) && find(both, *parse_chord("cmd+shift+e")));
}

int main() {
    test_parse();
    test_defaults();
    test_user_bindings();
    std::printf("%d/%d checks passed\n", checks_run - checks_failed, checks_run);
    return checks_failed == 0 ? 0 : 1;
}
