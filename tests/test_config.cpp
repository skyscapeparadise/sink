// Unit tests for config.cpp: reading and validating sink.toml version 1,
// resolving presets, generating files, and migrating older formats. Pure
// text in and out -- no files are touched -- except that HOME is set to a
// fixed value so ~ expansion is predictable.
#include "config.hpp"

#include <cstdio>
#include <cstdlib>
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

static config::Config read_ok(const std::string& text) {
    config::Diagnostic err;
    auto c = config::read_text(text, err);
    if (!c) {
        std::printf("unexpected parse error, line %d: %s\n", err.line, err.message.c_str());
        std::exit(1);
    }
    return *c;
}

static bool has_warning(const config::Config& c, const std::string& fragment, int line = -1) {
    for (const auto& w : c.warnings) {
        if (w.message.find(fragment) != std::string::npos && (line < 0 || w.line == line)) return true;
    }
    return false;
}

static void test_empty_file_is_all_defaults() {
    config::Config c = read_ok("");
    CHECK(c.warnings.empty());
    CHECK(config::active_preset(c) == "pool");
    CHECK(config::global_int(c, "terminal.scrollback_lines") == 10000);
    CHECK(config::global_bool(c, "shell.integration"));
    CHECK(config::option_as_meta(c) == "none");
    config::PresetValues p = config::resolve_preset(c, "pool");
    CHECK(p.background == "pool");
    CHECK(p.font == "Monaspace Neon");
    CHECK(p.exposure == 0.7);
    CHECK(p.ligatures && p.title_bar && p.typing_animation);
    CHECK(!p.crt && !p.hdr_text);
}

static void test_reads_v1() {
    config::Config c = read_ok(
        "version = 1\n"
        "preset = \"clouds\"\n"
        "\n"
        "[terminal]\n"
        "scrollback_lines = 20000\n"
        "\n"
        "[shell]\n"
        "integration = false\n"
        "\n"
        "[keyboard]\n"
        "option_as_meta = \"left\"\n"
        "\n"
        "[presets.clouds]\n"
        "background = \"~/Videos/Dry Ice 2.mp4\"\n"
        "exposure = 0.83\n"
        "hue_shift = 70\n"  // an integer is fine for a float key
        "crt = true\n"
        "\n"
        "[presets.\"helvetica pool\"]\n"
        "font = \"/System/Library/Fonts/HelveticaNeue.ttc\"\n");
    CHECK(c.warnings.empty());
    CHECK(config::active_preset(c) == "clouds");
    CHECK(config::global_int(c, "terminal.scrollback_lines") == 20000);
    CHECK(!config::global_bool(c, "shell.integration"));
    CHECK(config::option_as_meta(c) == "left");

    config::PresetValues clouds = config::resolve_preset(c, "clouds");
    CHECK(clouds.background == "/Users/test/Videos/Dry Ice 2.mp4");
    CHECK(clouds.exposure == 0.83);
    CHECK(clouds.hue_shift == 70.0);
    CHECK(clouds.crt);
    CHECK(clouds.font == "Monaspace Neon");  // not overridden: the default

    auto names = config::preset_names(c);
    CHECK(names.size() == 3);
    CHECK(names[0] == "pool" && names[1] == "clouds" && names[2] == "helvetica pool");
    CHECK(config::resolve_preset(c, "helvetica pool").font == "/System/Library/Fonts/HelveticaNeue.ttc");
}

static void test_bad_values_are_reported_not_trusted() {
    config::Config c = read_ok(
        "version = 1\n"
        "[terminal]\n"
        "scrollback_lines = -1\n"            // line 3: clamped, not 18 quintillion
        "[keyboard]\n"
        "option_as_meta = \"sideways\"\n"    // line 5: not a choice
        "[presets.x]\n"
        "exposure = \"bright\"\n"            // line 7: wrong type
        "exposre = 1.0\n"                    // line 8: typo
        "hue_shift = 400\n");                // line 9: clamped
    CHECK(config::global_int(c, "terminal.scrollback_lines") == 100);
    CHECK(has_warning(c, "terminal.scrollback_lines = -1 is outside 100 to 1000000", 3));
    CHECK(config::option_as_meta(c) == "none");
    CHECK(has_warning(c, "sideways", 5));
    CHECK(has_warning(c, "presets.x.exposure should be a number", 7));
    CHECK(has_warning(c, "unknown setting presets.x.exposre", 8));
    config::PresetValues x = config::resolve_preset(c, "x");
    CHECK(x.exposure == 0.7);
    CHECK(x.hue_shift == 360.0);
    CHECK(has_warning(c, "hue_shift = 400", 9));
}

static void test_syntax_error_reports_line() {
    config::Diagnostic err;
    auto c = config::read_text("version = 1\n[presets.x]\nexposure = 0.5\nfont = \"unterminated\n", err);
    CHECK(!c);
    CHECK(err.line == 4);
    CHECK(!err.message.empty());
}

static void test_builtin_cannot_be_redefined() {
    config::Config c = read_ok("[presets.pool]\nexposure = 1.5\n");
    CHECK(has_warning(c, "\"pool\" is a built-in preset", 1));
    CHECK(config::resolve_preset(c, "pool").exposure == 0.7);
    CHECK(config::preset_names(c).size() == 1);
}

static void test_unknown_preset_falls_back_to_pool() {
    config::Config c = read_ok("preset = \"gone\"\n");
    CHECK(config::active_preset(c) == "pool");
}

static void test_generate_round_trips() {
    config::Config c = read_ok(
        "preset = \"my look\"\n"
        "[terminal]\nscrollback_lines = 5000\n"
        "[presets.\"my look\"]\nexposure = 1.25\ncrt = true\nbackground = \"~/v.mp4\"\n");
    std::string text = config::generate(c);
    config::Config again = read_ok(text);
    CHECK(again.warnings.empty());
    CHECK(config::active_preset(again) == "my look");
    CHECK(config::global_int(again, "terminal.scrollback_lines") == 5000);
    config::PresetValues p = config::resolve_preset(again, "my look");
    CHECK(p.exposure == 1.25 && p.crt && p.background == "/Users/test/v.mp4");
    CHECK(text.find("version = 1") != std::string::npos);
    CHECK(text.find("background = \"~/v.mp4\"") != std::string::npos);  // ~ kept
}

// Shaped exactly like a real version 0 file sink wrote.
static const char* kV0 =
    "# sink terminal configuration\n"
    "\n"
    "active_preset = 'pool'\n"
    "\n"
    "[presets.clouds]\n"
    "animated_typing = true\n"
    "crt_mode_enabled = true\n"
    "exposure = 0.834762\n"
    "font_path = 'default'\n"
    "hdr_console_enabled = false\n"
    "hue_shift = 70.1367\n"
    "ligatures_enabled = true\n"
    "scrollback_lines = 10000\n"
    "vibrancy_enabled = true\n"
    "video_path = '/Users/test/Projects/Demo Reel April 2022/Liquid/Dry Ice 2.mp4'\n"
    "\n"
    "[presets.pool]\n"
    "animated_typing = true\n"
    "crt_mode_enabled = false\n"
    "exposure = 0.834762\n"
    "font_path = '/System/Library/Fonts/HelveticaNeue.ttc'\n"
    "hdr_console_enabled = false\n"
    "hue_shift = 0.0\n"
    "ligatures_enabled = true\n"
    "scrollback_lines = 20000\n"
    "vibrancy_enabled = true\n"
    "video_path = 'default'\n";

static void test_v0_migration() {
    CHECK(config::is_v0(kV0));
    CHECK(!config::is_v0("version = 1\n"));
    CHECK(!config::is_v0(""));
    CHECK(!config::is_v0("[presets.x]\nexposure = 1.0\n"));  // hand-written v1 without version

    auto text = config::migrate_v0(kV0);
    CHECK(text.has_value());
    if (!text) return;
    config::Config c = read_ok(*text);
    CHECK(c.warnings.empty());
    CHECK(!config::is_v0(*text));

    // The customised pool is kept, as an ordinary preset, and stays active.
    CHECK(config::active_preset(c) == "pool copy");
    config::PresetValues mine = config::resolve_preset(c, "pool copy");
    CHECK(mine.font == "/System/Library/Fonts/HelveticaNeue.ttc");
    CHECK(mine.background == "pool");
    CHECK(mine.exposure > 0.834 && mine.exposure < 0.835);

    config::PresetValues clouds = config::resolve_preset(c, "clouds");
    CHECK(clouds.crt);
    CHECK(clouds.hue_shift > 70.13 && clouds.hue_shift < 70.14);
    CHECK(clouds.background == "/Users/test/Projects/Demo Reel April 2022/Liquid/Dry Ice 2.mp4");

    // Defaults are dropped, home is written as ~, scrollback came from the
    // active preset.
    CHECK(text->find("ligatures") == std::string::npos);
    CHECK(text->find("\"~/Projects/Demo Reel") != std::string::npos);
    CHECK(config::global_int(c, "terminal.scrollback_lines") == 20000);
}

static void test_v0_untouched_pool_disappears() {
    auto text = config::migrate_v0(
        "active_preset = 'pool'\n"
        "[presets.pool]\nexposure = 0.7\nfont_path = 'default'\nvideo_path = 'default'\n"
        "shell_integration = false\n");
    CHECK(text.has_value());
    if (!text) return;
    config::Config c = read_ok(*text);
    CHECK(config::active_preset(c) == "pool");
    CHECK(config::preset_names(c).size() == 1);
    CHECK(text->find("[presets") == std::string::npos);
}

static void test_v0_top_level_settings() {
    auto text = config::migrate_v0(
        "active_preset = 'pool'\nshell_integration = false\noption_as_meta = \"right\"\n");
    CHECK(text.has_value());
    if (!text) return;
    config::Config c = read_ok(*text);
    CHECK(!config::global_bool(c, "shell.integration"));
    CHECK(config::option_as_meta(c) == "right");
}

static void test_legacy_migration() {
    std::string text = config::migrate_legacy(
        "active_preset=warm\n",
        { { "warm.txt", "name=warm\nexposure=1.1\ncrt_mode_enabled=true\n" },
          { "pool.txt", "name=pool\nexposure=0.7\n" } });
    config::Config c = read_ok(text);
    CHECK(config::active_preset(c) == "warm");
    CHECK(config::resolve_preset(c, "warm").crt);
    CHECK(config::preset_names(c).size() == 2);  // pool unchanged -> just the built-in

    // The oldest format: one flat config.txt, no presets folder.
    std::string flat = config::migrate_legacy("exposure=1.4\nvideo_path=/movies/sea.mp4\n", {});
    config::Config f = read_ok(flat);
    CHECK(config::active_preset(f) == "pool copy");
    CHECK(config::resolve_preset(f, "pool copy").background == "/movies/sea.mp4");
}

static void test_value_and_key_text() {
    CHECK(config::to_toml(config::Value(std::string("a \"b\" \\ c\n"))) == "\"a \\\"b\\\" \\\\ c\\n\"");
    CHECK(config::to_toml(config::Value(0.834762)) == "0.834762");
    CHECK(config::to_toml(config::Value(70.1367f * 1.0)) == "70.1367");
    CHECK(config::to_toml(config::Value(int64_t{10000})) == "10000");
    CHECK(config::to_toml(config::Value(true)) == "true");
    CHECK(config::toml_key("clouds") == "clouds");
    CHECK(config::toml_key("pool copy") == "\"pool copy\"");
    CHECK(config::toml_key("") == "\"\"");
}

static void test_paths() {
    CHECK(config::is_path("~/a.mp4"));
    CHECK(config::is_path("/a.mp4"));
    CHECK(config::is_path("videos/a.mp4"));
    CHECK(!config::is_path("pool"));
    CHECK(!config::is_path("Monaspace Neon"));
    CHECK(config::expand_home("~/a") == "/Users/test/a");
    CHECK(config::expand_home("~") == "/Users/test");
    CHECK(config::expand_home("/x") == "/x");
    CHECK(config::contract_home("/Users/test/a") == "~/a");
    CHECK(config::contract_home("/Users/tester/a") == "/Users/tester/a");
}

static void test_background_kinds() {
    config::Config c = read_ok(
        "[presets.a]\nbackground = \"Frosted Glass\"\n"   // any case
        "[presets.b]\nbackground = \"~/v.mp4\"\n"          // a path
        "[presets.c]\nbackground = \"sparkles\"\n"         // line 6: neither
        "[presets.d]\nbackground = \"gel\"\n[presets.d.gel]\nspeed = 9\ncolors = [\"#000\", \"#111\"]\n");
    CHECK(config::resolve_preset(c, "a").background == "frosted glass");
    CHECK(config::resolve_preset(c, "b").background == "/Users/test/v.mp4");
    CHECK(config::resolve_preset(c, "c").background == "pool");
    CHECK(has_warning(c, "presets.c.background = \"sparkles\" isn't a path", 6));
    config::PresetValues d = config::resolve_preset(c, "d");
    CHECK(d.background == "gel");
    CHECK(d.gel_speed == 4.0);                   // clamped
    CHECK(d.gel_colors.size() == 2);
    CHECK(d.gradient_angle == 45.0);
    CHECK(d.glass_tint > 0.29 && d.glass_tint < 0.31);
}

static void test_shell_settings() {
    config::Config c = read_ok(
        "[shell]\n"
        "command = \"fish\"\n"
        "args = [\"--private\"]\n"
        "login = false\n"
        "working_directory = \"~/code\"\n"
        "[shell.env]\n"
        "EDITOR = \"nvim\"\n"
        "PAGER = \"less -R\"\n"
        "COUNT = 3\n");                                  // line 9: not a string
    CHECK(config::global_string(c, "shell.command") == "fish");
    CHECK((config::global_strings(c, "shell.args") == config::StringList{ "--private" }));
    CHECK(!config::global_bool(c, "shell.login"));
    CHECK(config::global_string(c, "shell.working_directory") == "~/code");
    config::StringMap env = config::global_table(c, "shell.env");
    CHECK(env.size() == 2 && env[0].first == "EDITOR" && env[1].second == "less -R");
    CHECK(has_warning(c, "shell.env.COUNT should be a string", 9));

    config::Config defaults = read_ok("");
    CHECK(config::global_bool(defaults, "shell.login"));
    CHECK(config::global_strings(defaults, "shell.args").empty());
    CHECK(config::global_table(defaults, "shell.env").empty());
}

static void test_keybindings_config() {
    config::Config c = read_ok(
        "[keybindings]\n"
        "\"cmd+shift+e\" = \"split_right\"\n"
        "\"cmd+k\" = \"none\"\n"
        "\"cmd+u\" = { text = \"\\u0015\" }\n"
        "\"cmd+l\" = \"make_coffee\"\n"       // line 5
        "\"cmd+j\" = 3\n");                     // line 6
    config::StringMap kb = config::global_table(c, "keybindings");
    CHECK(kb.size() == 3);
    CHECK(kb.size() == 3 && kb[2].first == "cmd+u" && kb[2].second == std::string(1, config::kKeybindingText) + "\x15");
    CHECK(has_warning(c, "\"make_coffee\" isn't an action", 5));
    CHECK(has_warning(c, "should be an action's name", 6));
    CHECK(config::effective_toml(c).find("\"cmd+u\" = { text = \"\\u0015\" }") != std::string::npos);
}

static std::string read_source_file(const std::string& relative) {
    std::string path = std::string(SINK_SOURCE_DIR) + "/" + relative;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return std::string();
    std::string text;
    char buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
    std::fclose(f);
    return text;
}

static void test_reference_is_a_valid_config() {
    // Every key in the reference is one sink knows, at a value it accepts.
    config::Config c = read_ok(config::reference_toml());
    CHECK(c.warnings.size() == 0);
    CHECK(config::active_preset(c) == "pool");
    CHECK(config::reference_toml().find("#:schema ") == 0);
    // And the effective view of a real config parses too.
    config::Config mine = read_ok("[presets.clouds]\ncrt = true\n");
    config::Diagnostic err;
    CHECK(config::read_text(config::effective_toml(mine), err).has_value());
    CHECK(config::effective_toml(mine).find("[presets.pool]  # built-in") != std::string::npos);
}

static void test_published_schema_and_readme_are_current() {
    // sink.schema.json is what editors download; regenerate it with
    // `sink --print-schema > sink.schema.json` when the keys change.
    CHECK(read_source_file("sink.schema.json") == config::json_schema());
    const std::string readme = read_source_file("README.md");
    for (const config::KeyInfo& k : config::keys()) {
        std::string path = k.path;
        std::string leaf = path.substr(path.rfind('.') == std::string::npos ? 0 : path.rfind('.') + 1);
        if (readme.find(leaf) == std::string::npos) std::printf("README doesn't mention %s\n", k.path);
        CHECK(readme.find(leaf) != std::string::npos);
    }
}

int main() {
    setenv("HOME", "/Users/test", 1);
    test_empty_file_is_all_defaults();
    test_reads_v1();
    test_bad_values_are_reported_not_trusted();
    test_syntax_error_reports_line();
    test_builtin_cannot_be_redefined();
    test_unknown_preset_falls_back_to_pool();
    test_generate_round_trips();
    test_v0_migration();
    test_v0_untouched_pool_disappears();
    test_v0_top_level_settings();
    test_legacy_migration();
    test_value_and_key_text();
    test_paths();
    test_reference_is_a_valid_config();
    test_background_kinds();
    test_shell_settings();
    test_keybindings_config();
    test_published_schema_and_readme_are_current();
    std::printf("%d/%d checks passed\n", checks_run - checks_failed, checks_run);
    return checks_failed == 0 ? 0 : 1;
}
