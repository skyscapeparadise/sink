// Unit tests for preset_manager's TOML-backed config store. Runs against a
// scratch HOME (set via setenv before any presets:: call, since
// preset_manager.cpp resolves paths from $HOME on every call rather than
// caching it) so this never touches a real ~/.config/sink.
#include "preset_manager.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>

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

static void write_file(const std::string& path, const std::string& contents) {
    std::ofstream f(path);
    f << contents;
}

static void test_fresh_install_defaults_to_pool() {
    setenv("HOME", "/tmp/sink_test_fresh", 1);
    system("rm -rf /tmp/sink_test_fresh && mkdir -p /tmp/sink_test_fresh");

    presets::migrate_legacy_config_if_needed();
    CHECK(presets::exists("pool"));
    CHECK(presets::get_active_preset_name() == "pool");
    Preset p = presets::load("pool");
    CHECK(p.name == "pool");
    CHECK(p.scrollback_lines == 10000); // struct default
}

static void test_save_load_roundtrip() {
    setenv("HOME", "/tmp/sink_test_roundtrip", 1);
    system("rm -rf /tmp/sink_test_roundtrip && mkdir -p /tmp/sink_test_roundtrip");
    presets::migrate_legacy_config_if_needed();

    Preset p;
    p.name = "dark";
    p.video_path = "/some/path with spaces/video.mp4";
    p.font_path = "default";
    p.exposure = 0.834762f;
    p.hue_shift = 70.1367f;
    p.crt_mode_enabled = true;
    p.scrollback_lines = 25000;
    presets::save(p);

    CHECK(presets::exists("dark"));
    Preset loaded = presets::load("dark");
    CHECK(loaded.video_path == p.video_path);
    CHECK(loaded.exposure == p.exposure); // exact float round-trip, not just "close"
    CHECK(loaded.hue_shift == p.hue_shift);
    CHECK(loaded.crt_mode_enabled == true);
    CHECK(loaded.scrollback_lines == 25000);

    // The float should serialize cleanly, not as a long binary-noise tail
    std::ifstream f(presets::config_path());
    std::string contents((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(contents.find("0.834762") != std::string::npos);
    CHECK(contents.find("70.1367") != std::string::npos);
    CHECK(contents.find("0.83476197719573975") == std::string::npos);
}

static void test_pool_is_protected() {
    setenv("HOME", "/tmp/sink_test_protected", 1);
    system("rm -rf /tmp/sink_test_protected && mkdir -p /tmp/sink_test_protected");
    presets::migrate_legacy_config_if_needed();

    CHECK(presets::rename("pool", "renamed") == false);
    CHECK(presets::remove("pool") == false);
    CHECK(presets::exists("pool"));
}

static void test_rename_and_remove() {
    setenv("HOME", "/tmp/sink_test_rename", 1);
    system("rm -rf /tmp/sink_test_rename && mkdir -p /tmp/sink_test_rename");
    presets::migrate_legacy_config_if_needed();

    Preset p; p.name = "temp";
    presets::save(p);
    CHECK(presets::rename("temp", "renamed") == true);
    CHECK(!presets::exists("temp"));
    CHECK(presets::exists("renamed"));

    // Can't rename onto an existing name
    Preset other; other.name = "other";
    presets::save(other);
    CHECK(presets::rename("renamed", "other") == false);

    CHECK(presets::remove("renamed") == true);
    CHECK(!presets::exists("renamed"));
    CHECK(presets::remove("nonexistent") == false);
}

static void test_unique_name() {
    setenv("HOME", "/tmp/sink_test_unique", 1);
    system("rm -rf /tmp/sink_test_unique && mkdir -p /tmp/sink_test_unique");
    presets::migrate_legacy_config_if_needed();

    CHECK(presets::unique_name("brandnew") == "brandnew");
    Preset p; p.name = "taken";
    presets::save(p);
    CHECK(presets::unique_name("taken") == "taken 2");
    Preset p2; p2.name = "taken 2";
    presets::save(p2);
    CHECK(presets::unique_name("taken") == "taken 3");
}

static void test_active_preset_pointer() {
    setenv("HOME", "/tmp/sink_test_active", 1);
    system("rm -rf /tmp/sink_test_active && mkdir -p /tmp/sink_test_active");
    presets::migrate_legacy_config_if_needed();

    Preset p; p.name = "focused";
    presets::save(p);
    presets::set_active_preset_name("focused");
    CHECK(presets::get_active_preset_name() == "focused");
}

static void test_legacy_migration_preserves_all_presets() {
    setenv("HOME", "/tmp/sink_test_legacy", 1);
    system("rm -rf /tmp/sink_test_legacy && mkdir -p /tmp/sink_test_legacy/.config/sink/presets");

    write_file("/tmp/sink_test_legacy/.config/sink/config.txt", "active_preset=clouds\n");
    write_file("/tmp/sink_test_legacy/.config/sink/presets/pool.txt",
              "name=pool\nvideo_path=default\nfont_path=default\nexposure=0.7\n"
              "hue_shift=0\nanimated_typing=true\nvibrancy_enabled=true\n"
              "crt_mode_enabled=false\nligatures_enabled=true\nhdr_console_enabled=false\n");
    write_file("/tmp/sink_test_legacy/.config/sink/presets/clouds.txt",
              "name=clouds\nvideo_path=/x/y.mp4\nfont_path=default\nexposure=0.834762\n"
              "hue_shift=70.1367\nanimated_typing=true\nvibrancy_enabled=true\n"
              "crt_mode_enabled=true\nligatures_enabled=true\nhdr_console_enabled=false\n");

    presets::migrate_legacy_config_if_needed();

    struct stat st;
    CHECK(stat(presets::config_path().c_str(), &st) == 0); // sink.toml was created
    CHECK(presets::get_active_preset_name() == "clouds");
    CHECK(presets::exists("pool"));
    CHECK(presets::exists("clouds"));
    Preset clouds = presets::load("clouds");
    CHECK(clouds.video_path == "/x/y.mp4");
    CHECK(clouds.crt_mode_enabled == true);
    CHECK(clouds.scrollback_lines == 10000); // wasn't in the old file; struct default applies

    // Old files must survive untouched, not be deleted
    CHECK(stat("/tmp/sink_test_legacy/.config/sink/config.txt", &st) == 0);
    CHECK(stat("/tmp/sink_test_legacy/.config/sink/presets/clouds.txt", &st) == 0);

    // Running migration again must be a no-op (must not, say, revert a
    // since-changed active_preset back to the legacy value)
    presets::set_active_preset_name("pool");
    presets::migrate_legacy_config_if_needed();
    CHECK(presets::get_active_preset_name() == "pool");
}

static void test_malformed_toml_does_not_crash() {
    setenv("HOME", "/tmp/sink_test_malformed", 1);
    system("rm -rf /tmp/sink_test_malformed && mkdir -p /tmp/sink_test_malformed/.config/sink");
    write_file("/tmp/sink_test_malformed/.config/sink/sink.toml", "this is not [ valid toml {{{\n");

    // Should degrade to empty config rather than crash/throw uncaught
    CHECK(presets::get_active_preset_name() == "pool");
    CHECK(!presets::exists("anything"));
}

int main() {
    test_fresh_install_defaults_to_pool();
    test_save_load_roundtrip();
    test_pool_is_protected();
    test_rename_and_remove();
    test_unique_name();
    test_active_preset_pointer();
    test_legacy_migration_preserves_all_presets();
    test_malformed_toml_does_not_crash();

    system("rm -rf /tmp/sink_test_fresh /tmp/sink_test_roundtrip /tmp/sink_test_protected "
          "/tmp/sink_test_rename /tmp/sink_test_unique /tmp/sink_test_active "
          "/tmp/sink_test_legacy /tmp/sink_test_malformed");

    std::printf("%d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
