// Unit tests for toml_edit: in-place edits must change exactly the text they
// are about and leave every other byte alone, and decline (so the caller
// regenerates) whenever they can't.
#include "toml_edit.hpp"

#include <cstdio>
#include <string>

static int checks_failed = 0;
static int checks_run = 0;

static void check_eq(const char* file, int line, const std::string& got, const std::string& want) {
    ++checks_run;
    if (got != want) {
        ++checks_failed;
        std::printf("FAIL %s:%d\n--- got:\n%s--- want:\n%s---\n", file, line, got.c_str(), want.c_str());
    }
}
#define CHECK_EQ(got, want) check_eq(__FILE__, __LINE__, (got), (want))
#define CHECK(cond)                                                          \
    do {                                                                     \
        ++checks_run;                                                        \
        if (!(cond)) {                                                       \
            ++checks_failed;                                                 \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)

using toml_edit::Edit;

static Edit set(toml_edit::Path table, std::string key, std::string value) {
    Edit e;
    e.kind = Edit::Kind::Set;
    e.table = std::move(table);
    e.key = std::move(key);
    e.value_text = std::move(value);
    return e;
}

static Edit remove(toml_edit::Path table, std::string key) {
    Edit e;
    e.kind = Edit::Kind::Remove;
    e.table = std::move(table);
    e.key = std::move(key);
    return e;
}

static Edit remove_table(toml_edit::Path table) {
    Edit e;
    e.kind = Edit::Kind::RemoveTable;
    e.table = std::move(table);
    return e;
}

static Edit rename_table(toml_edit::Path table, std::string to) {
    Edit e;
    e.kind = Edit::Kind::RenameTable;
    e.table = std::move(table);
    e.new_name = std::move(to);
    return e;
}

static std::string must(const std::string& text, const Edit& e) {
    auto out = toml_edit::apply(text, e);
    if (!out) {
        ++checks_failed;
        std::printf("FAIL: edit declined on:\n%s---\n", text.c_str());
        return std::string();
    }
    return *out;
}

static const std::string kFile =
    "# my sink setup\n"
    "version = 1\n"
    "preset = \"clouds\"   # the one I use\n"
    "\n"
    "[terminal]\n"
    "scrollback_lines = 20000\n"
    "\n"
    "# moody\n"
    "[presets.clouds]\n"
    "  background = \"~/v.mp4\"\n"
    "  exposure = 0.8 # dim\n"
    "\n"
    "# for presentations\n"
    "[presets.\"big text\"]\n"
    "crt = true\n";

static void test_replace_keeps_everything_else() {
    CHECK_EQ(must(kFile, set({ "presets", "clouds" }, "exposure", "1.2")),
             "# my sink setup\n"
             "version = 1\n"
             "preset = \"clouds\"   # the one I use\n"
             "\n"
             "[terminal]\n"
             "scrollback_lines = 20000\n"
             "\n"
             "# moody\n"
             "[presets.clouds]\n"
             "  background = \"~/v.mp4\"\n"
             "  exposure = 1.2 # dim\n"
             "\n"
             "# for presentations\n"
             "[presets.\"big text\"]\n"
             "crt = true\n");
    CHECK(must(kFile, set({}, "preset", "\"big text\"")).find("version = 1\npreset = \"big text\"   # the one I use\n") !=
          std::string::npos);
}

static void test_insert_into_table_matches_indent() {
    std::string out = must(kFile, set({ "presets", "clouds" }, "crt", "true"));
    CHECK(out.find("  exposure = 0.8 # dim\n  crt = true\n\n# for presentations\n") != std::string::npos);
}

static void test_insert_into_last_table() {
    std::string out = must(kFile, set({ "presets", "big text" }, "hue_shift", "90"));
    CHECK(out.size() > 0 && out.substr(out.size() - 26) == "crt = true\nhue_shift = 90\n");
}

static void test_insert_new_table_at_end() {
    std::string out = must(kFile, set({ "presets", "new one" }, "exposure", "1.5"));
    CHECK(out.substr(0, kFile.size()) == kFile);
    CHECK_EQ(out.substr(kFile.size()), "\n[presets.\"new one\"]\nexposure = 1.5\n");
}

static void test_insert_top_level_before_first_header() {
    std::string out = must(kFile, set({}, "include", "[\"~/a.toml\"]"));
    CHECK(out.find("preset = \"clouds\"   # the one I use\ninclude = [\"~/a.toml\"]\n\n[terminal]") !=
          std::string::npos);
    // A file with only tables still gets the key above them.
    CHECK_EQ(must("[terminal]\nscrollback_lines = 5\n", set({}, "version", "1")),
             "version = 1\n\n[terminal]\nscrollback_lines = 5\n");
    CHECK_EQ(must("", set({}, "version", "1")), "version = 1\n");
}

static void test_remove_key() {
    std::string out = must(kFile, remove({ "presets", "clouds" }, "exposure"));
    CHECK(out.find("  background = \"~/v.mp4\"\n\n# for presentations") != std::string::npos);
    CHECK(out.find("exposure") == std::string::npos);
    CHECK_EQ(must(kFile, remove({ "presets", "clouds" }, "missing")), kFile);
}

static void test_remove_table_keeps_next_tables_comment() {
    CHECK_EQ(must(kFile, remove_table({ "presets", "clouds" })),
             "# my sink setup\n"
             "version = 1\n"
             "preset = \"clouds\"   # the one I use\n"
             "\n"
             "[terminal]\n"
             "scrollback_lines = 20000\n"
             "\n"
             "# for presentations\n"
             "[presets.\"big text\"]\n"
             "crt = true\n");
    // The last table, with its subtable.
    std::string with_sub = kFile + "\n[presets.\"big text\".colors]\nforeground = \"#ffffff\"\n";
    CHECK_EQ(must(with_sub, remove_table({ "presets", "big text" })),
             "# my sink setup\n"
             "version = 1\n"
             "preset = \"clouds\"   # the one I use\n"
             "\n"
             "[terminal]\n"
             "scrollback_lines = 20000\n"
             "\n"
             "# moody\n"
             "[presets.clouds]\n"
             "  background = \"~/v.mp4\"\n"
             "  exposure = 0.8 # dim\n");
}

static void test_rename_table_and_subtables() {
    std::string with_sub = kFile + "\n[presets.clouds . colors] # theirs\nforeground = \"#ffffff\"\n";
    std::string out = must(with_sub, rename_table({ "presets", "clouds" }, "storm front"));
    CHECK(out.find("# moody\n[presets.\"storm front\"]\n  background") != std::string::npos);
    CHECK(out.find("[presets.\"storm front\".colors] # theirs\n") != std::string::npos);
}

static void test_non_ascii_before_value() {
    std::string text = "\"café\" = \"é\" # ok\n";
    CHECK_EQ(must(text, set({}, "café", "\"crème\"")), "\"café\" = \"crème\" # ok\n");
}

static void test_dotted_and_multiline_values() {
    CHECK_EQ(must("terminal.scrollback_lines = 5\n", set({ "terminal" }, "scrollback_lines", "9")),
             "terminal.scrollback_lines = 9\n");
    CHECK_EQ(must("[shell]\nargs = [\n  \"-l\",\n]\nx = 1\n", set({ "shell" }, "args", "[\"-i\"]")),
             "[shell]\nargs = [\"-i\"]\nx = 1\n");
    // Inserting into a table defined by dotted keys has nowhere safe to go.
    CHECK(!toml_edit::apply("terminal.scrollback_lines = 5\n", set({ "terminal" }, "other", "1")));
}

static void test_declines_then_regenerates() {
    std::string inline_table = "# note\nterminal = { scrollback_lines = 5 }\nunknown = \"kept\"\n";
    CHECK(!toml_edit::apply(inline_table, set({ "terminal" }, "other", "1")));
    bool regenerated = false;
    auto out = toml_edit::apply_or_regenerate(inline_table, set({ "terminal" }, "other", "1"), regenerated);
    CHECK(out && regenerated);
    CHECK(out && out->find("unknown") != std::string::npos);
    CHECK(out && out->find("other = 1") != std::string::npos);
    CHECK(out && out->find("scrollback_lines = 5") != std::string::npos);

    regenerated = true;
    out = toml_edit::apply_or_regenerate(kFile, set({ "terminal" }, "scrollback_lines", "1"), regenerated);
    CHECK(out && !regenerated);
    CHECK(!toml_edit::apply_or_regenerate("x = [", set({}, "y", "1"), regenerated));
}

static void test_add_table() {
    Edit e;
    e.kind = Edit::Kind::AddTable;
    e.table = { "presets", "pool copy" };
    CHECK_EQ(must("preset = \"pool\"\n", e), "preset = \"pool\"\n\n[presets.\"pool copy\"]\n");
    std::string twice = must(must("", e), e);
    CHECK_EQ(twice, "[presets.\"pool copy\"]\n");
}

static void test_crlf() {
    std::string text = "[a]\r\nx = 1\r\n";
    CHECK_EQ(must(text, set({ "a" }, "x", "2")), "[a]\r\nx = 2\r\n");
}

int main() {
    test_replace_keeps_everything_else();
    test_insert_into_table_matches_indent();
    test_insert_into_last_table();
    test_insert_new_table_at_end();
    test_insert_top_level_before_first_header();
    test_remove_key();
    test_remove_table_keeps_next_tables_comment();
    test_rename_table_and_subtables();
    test_non_ascii_before_value();
    test_dotted_and_multiline_values();
    test_declines_then_regenerates();
    test_add_table();
    test_crlf();
    std::printf("%d/%d checks passed\n", checks_run - checks_failed, checks_run);
    return checks_failed == 0 ? 0 : 1;
}
