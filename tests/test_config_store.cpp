// Tests for ConfigStore: the sink.toml file on disk. Every test works in its
// own scratch folder under /tmp, and HOME points there too, so a real
// ~/.config/sink is never read or written.
#include "config_store.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

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

static std::string scratch() {
    char tmpl[] = "/tmp/sink_config_store.XXXXXX";
    std::string dir = mkdtemp(tmpl);
    setenv("HOME", dir.c_str(), 1);
    mkdir((dir + "/.config").c_str(), 0755);
    mkdir((dir + "/.config/sink").c_str(), 0755);
    return dir + "/.config/sink";
}

static std::string read(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void write(const std::string& path, const std::string& text) {
    std::ofstream f(path);
    f << text;
}

static bool exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

static void test_fresh_install_creates_file() {
    std::string dir = scratch();
    ConfigStore store(dir + "/sink.toml");
    store.load();
    std::string text = read(dir + "/sink.toml");
    CHECK(contains(text, "version = 1\npreset = \"pool\"\n"));
    CHECK(config::active_preset(store.config()) == "pool");
    CHECK(!store.error());
}

static void test_v0_file_is_migrated_with_backup() {
    std::string dir = scratch();
    const std::string v0 =
        "active_preset = 'pool'\n"
        "[presets.pool]\nexposure = 1.5\nfont_path = 'default'\nvideo_path = 'default'\n";
    write(dir + "/sink.toml", v0);
    ConfigStore store(dir + "/sink.toml");
    store.load();
    CHECK(read(dir + "/sink.toml.v0.bak") == v0);
    CHECK(contains(read(dir + "/sink.toml"), "version = 1"));
    CHECK(config::active_preset(store.config()) == "pool copy");
    CHECK(config::resolve_preset(store.config(), "pool copy").exposure == 1.5);

    // Loading again changes nothing and makes no second backup.
    ConfigStore again(dir + "/sink.toml");
    again.load();
    CHECK(!exists(dir + "/sink.toml.v0.bak.1"));
}

static void test_legacy_files_are_migrated() {
    std::string dir = scratch();
    mkdir((dir + "/presets").c_str(), 0755);
    write(dir + "/config.txt", "active_preset=warm\n");
    write(dir + "/presets/warm.txt", "name=warm\ncrt_mode_enabled=true\n");
    ConfigStore store(dir + "/sink.toml");
    store.load();
    CHECK(config::active_preset(store.config()) == "warm");
    CHECK(config::resolve_preset(store.config(), "warm").crt);
}

static void test_edits_keep_comments_and_wait_for_quiet() {
    std::string dir = scratch();
    write(dir + "/sink.toml",
          "# mine\nversion = 1\npreset = \"clouds\"\n\n[presets.clouds] # moody\nexposure = 0.8 # dim\n");
    ConfigStore store(dir + "/sink.toml");
    store.load();

    store.set_preset("clouds", "exposure", config::Value(1.1));
    store.set_preset("clouds", "exposure", config::Value(1.2));
    CHECK(config::resolve_preset(store.config(), "clouds").exposure == 1.2);  // live at once
    store.flush_if_due(60000);
    CHECK(store.dirty());
    CHECK(contains(read(dir + "/sink.toml"), "exposure = 0.8"));  // not yet written
    store.flush_if_due(0);
    CHECK(!store.dirty());
    CHECK(read(dir + "/sink.toml") ==
          "# mine\nversion = 1\npreset = \"clouds\"\n\n[presets.clouds] # moody\nexposure = 1.2 # dim\n");

    // Back to the default: the key goes, the comment on the table stays.
    store.set_preset("clouds", "exposure", config::Value(0.7));
    store.set_preset("clouds", "background", config::Value(std::string(getenv("HOME")) + "/v.mp4"));
    store.flush();
    CHECK(read(dir + "/sink.toml") ==
          "# mine\nversion = 1\npreset = \"clouds\"\n\n[presets.clouds] # moody\nbackground = \"~/v.mp4\"\n");
}

static void test_syntax_error_blocks_writes_until_fixed() {
    std::string dir = scratch();
    write(dir + "/sink.toml", "version = 1\npreset = \"x\"\n[presets.x]\ncrt = true\n");
    ConfigStore store(dir + "/sink.toml");
    store.load();
    CHECK(config::resolve_preset(store.config(), "x").crt);

    const std::string broken = "version = 1\npreset = \"x\"\n[presets.x]\ncrt = tru\n";
    write(dir + "/sink.toml", broken);
    CHECK(store.reload_if_changed());
    CHECK(store.error() && store.error()->line == 4);
    CHECK(config::resolve_preset(store.config(), "x").crt);  // last good settings kept

    store.set_preset("x", "exposure", config::Value(1.5));
    CHECK(config::resolve_preset(store.config(), "x").exposure == 1.5);
    store.flush();
    CHECK(read(dir + "/sink.toml") == broken);  // untouched while broken

    write(dir + "/sink.toml", "version = 1\npreset = \"x\"\n[presets.x]\ncrt = false\n");
    CHECK(store.reload_if_changed());
    CHECK(!store.error());
    store.flush();
    CHECK(read(dir + "/sink.toml") == "version = 1\npreset = \"x\"\n[presets.x]\ncrt = false\nexposure = 1.5\n");
}

static void test_outside_changes_are_noticed() {
    std::string dir = scratch();
    ConfigStore store(dir + "/sink.toml");
    store.load();
    CHECK(!store.reload_if_changed());
    write(dir + "/sink.toml", "version = 1\npreset = \"a\"\n[presets.a]\nhue_shift = 45\n");
    CHECK(store.reload_if_changed());
    CHECK(config::active_preset(store.config()) == "a");
    CHECK(!store.reload_if_changed());

    // A change made while one of ours waits to be written isn't lost.
    store.set_preset("a", "crt", config::Value(true));
    write(dir + "/sink.toml", "version = 1\npreset = \"a\"\n[presets.a]\nhue_shift = 90\n");
    store.flush();
    std::string text = read(dir + "/sink.toml");
    CHECK(contains(text, "hue_shift = 90") && contains(text, "crt = true"));
}

static void test_symlink_and_mode_survive() {
    std::string dir = scratch();
    std::string real = dir + "/../dotfiles-sink.toml";
    write(real, "version = 1\npreset = \"pool\"\n");
    chmod(real.c_str(), 0600);
    symlink(real.c_str(), (dir + "/sink.toml").c_str());
    ConfigStore store(dir + "/sink.toml");
    store.load();
    store.set_global("terminal.scrollback_lines", config::Value(int64_t{5000}));
    store.flush();
    struct stat st;
    CHECK(lstat((dir + "/sink.toml").c_str(), &st) == 0 && S_ISLNK(st.st_mode));
    CHECK(contains(read(real), "[terminal]\nscrollback_lines = 5000\n"));
    CHECK(stat(real.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
}

static void test_preset_management() {
    std::string dir = scratch();
    ConfigStore store(dir + "/sink.toml");
    store.load();

    // "new" saves the current look -- here pool with crt on -- as a preset.
    store.set_edit("crt", config::Value(true));
    std::string mine = store.save_as_preset("mine");
    CHECK(mine == "mine");
    CHECK(config::active_preset(store.config()) == "mine");
    CHECK(store.config().edits.empty());
    CHECK(config::resolve_preset(store.config(), "mine").crt);
    CHECK(store.save_as_preset("mine") == "mine 2");
    store.set_global("preset", config::Value(mine));
    store.set_preset("pool", "crt", config::Value(true));  // built-ins can't be edited
    CHECK(!config::resolve_preset(store.config(), "pool").crt);

    CHECK(!store.rename_preset("pool", "retro"));
    CHECK(!store.rename_preset(mine, "mine 2"));  // taken
    CHECK(store.rename_preset(mine, "retro"));
    CHECK(config::active_preset(store.config()) == "retro");
    CHECK(config::resolve_preset(store.config(), "retro").crt);

    CHECK(!store.remove_preset("pool"));
    store.set_edit("exposure", config::Value(1.5));
    CHECK(store.remove_preset("retro"));
    CHECK(config::active_preset(store.config()) == "pool");
    CHECK(store.config().edits.empty());  // they were edits to retro
    store.flush();
    std::string text = read(dir + "/sink.toml");
    CHECK(!contains(text, "retro") && contains(text, "[presets.\"mine 2\"]") && !contains(text, "[edits]"));
}

// Settings' changes go into [edits] on top of the active preset; "save" moves
// them into it, and picking a preset drops them.
static void test_edits() {
    std::string dir = scratch();
    write(dir + "/sink.toml", "version = 1\npreset = \"clouds\"\n\n# mine\n[presets.clouds]\nexposure = 1.2\n");
    ConfigStore store(dir + "/sink.toml");
    store.load();

    store.set_edit("hue_shift", config::Value(90.0));
    store.set_edit("exposure", config::Value(1.2));  // what clouds has: no edit
    CHECK(store.config().edits.size() == 1);
    CHECK(config::resolve_active(store.config()).hue_shift == 90.0);
    CHECK(config::resolve_preset(store.config(), "clouds").hue_shift == 0.0);
    store.flush();
    CHECK(contains(read(dir + "/sink.toml"), "[edits]\nhue_shift = 90\n"));
    CHECK(contains(read(dir + "/sink.toml"), "# mine\n"));

    // Moving a control back to the preset's value clears its edit, and the
    // table with it.
    store.set_edit("hue_shift", config::Value(0.0));
    CHECK(store.config().edits.empty());
    store.flush();
    CHECK(!contains(read(dir + "/sink.toml"), "edits"));

    // Saving moves them into the preset.
    store.set_edit("crt", config::Value(true));
    CHECK(store.save_edits());
    CHECK(store.config().edits.empty());
    CHECK(config::resolve_preset(store.config(), "clouds").crt);

    // A built-in can't take them.
    store.set_global("preset", config::Value(std::string("pool")));
    store.set_edit("crt", config::Value(true));
    CHECK(!store.save_edits());
    CHECK(config::resolve_active(store.config()).crt);
    CHECK(!config::resolve_preset(store.config(), "pool").crt);

    // Picking a preset is clear_edits() and then `preset`.
    store.clear_edits();
    CHECK(!config::resolve_active(store.config()).crt);
    store.flush();
    const std::string text = read(dir + "/sink.toml");
    CHECK(!contains(text, "edits") && contains(text, "# mine\n"));
}

static void test_regenerating_keeps_a_backup() {
    std::string dir = scratch();
    const std::string original = "# note\nversion = 1\npresets = { x = { crt = true } }\n";
    write(dir + "/sink.toml", original);
    ConfigStore store(dir + "/sink.toml");
    store.load();
    store.set_preset("x", "exposure", config::Value(1.5));
    store.flush();
    CHECK(store.regenerated());
    CHECK(read(dir + "/sink.toml.bak") == original);
    config::Diagnostic err;
    auto c = config::read_text(read(dir + "/sink.toml"), err);
    CHECK(c && config::resolve_preset(*c, "x").crt && config::resolve_preset(*c, "x").exposure == 1.5);
}

static bool warned(const config::Config& c, const std::string& fragment, const std::string& file_fragment) {
    for (const auto& w : c.warnings) {
        if (w.message.find(fragment) != std::string::npos && w.file.find(file_fragment) != std::string::npos) return true;
    }
    return false;
}

static void test_includes() {
    std::string dir = scratch();
    mkdir((dir + "/parts").c_str(), 0755);
    write(dir + "/parts/shared.toml",
          "include = [\"themes.toml\"]\n"            // relative to parts/
          "[terminal]\nscrollback_lines = 3000\n"
          "[presets.work]\nexposure = 1.5\ncrt = true\n");
    write(dir + "/parts/themes.toml", "[presets.work]\ntheme = \"nord\"\nhue_shift = 10\n");
    write(dir + "/broken.toml", "[presets.x]\ncrt = tru\n");
    write(dir + "/loop.toml", "include = [\"loop.toml\"]\n");
    write(dir + "/sink.toml",
          "version = 1\n"
          "include = [\"parts/shared.toml\", \"missing.toml\", \"broken.toml\", \"loop.toml\"]\n"
          "preset = \"work\"\n"
          "[presets.work]\nexposure = 0.9\n"
          "[presets.mine]\ncrt = true\n");
    ConfigStore store(dir + "/sink.toml");
    store.load();
    const config::Config& c = store.config();

    // Includes are bases, deepest first; sink.toml wins.
    config::PresetValues work = config::resolve_preset(c, "work");
    CHECK(work.exposure == 0.9);           // sink.toml over shared.toml
    CHECK(work.crt);                       // from shared.toml
    CHECK(work.theme == "nord");           // from themes.toml, included by shared.toml
    CHECK(config::global_int(c, "terminal.scrollback_lines") == 3000);

    // Problems with includes are warnings about that file, not failures.
    CHECK(!store.error());
    CHECK(warned(c, "can't read include \"missing.toml\"", ""));
    CHECK(warned(c, "skipping this file", "broken.toml"));
    CHECK(warned(c, "includes itself", "loop.toml"));

    // Presets only in an include can't be renamed or deleted; ones in
    // sink.toml can, and changes land in sink.toml.
    write(dir + "/parts/shared.toml", read(dir + "/parts/shared.toml") + "[presets.team]\ncrt = true\n");
    CHECK(store.reload_if_changed());      // an include changed
    CHECK(config::preset_exists(store.config(), "team"));
    CHECK(!store.can_rename_or_delete("team"));
    CHECK(!store.remove_preset("team"));
    CHECK(store.can_rename_or_delete("mine"));
    store.set_preset("team", "hue_shift", config::Value(200.0));
    store.flush();
    CHECK(contains(read(dir + "/sink.toml"), "[presets.team]\nhue_shift = 200\n"));
    CHECK(!contains(read(dir + "/parts/shared.toml"), "hue_shift = 200"));
    CHECK(config::resolve_preset(store.config(), "team").crt);
    CHECK(config::resolve_preset(store.config(), "team").hue_shift == 200.0);
    CHECK(!store.reload_if_changed());
}

int main() {
    test_fresh_install_creates_file();
    test_v0_file_is_migrated_with_backup();
    test_legacy_files_are_migrated();
    test_edits_keep_comments_and_wait_for_quiet();
    test_syntax_error_blocks_writes_until_fixed();
    test_outside_changes_are_noticed();
    test_symlink_and_mode_survive();
    test_preset_management();
    test_edits();
    test_regenerating_keeps_a_backup();
    test_includes();
    std::printf("%d/%d checks passed\n", checks_run - checks_failed, checks_run);
    return checks_failed == 0 ? 0 : 1;
}
