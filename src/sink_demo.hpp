#pragma once

#include <string>
#include <vector>

struct TerminalWindow;
struct AppState;

namespace SinkDemo {
    // Embedded song lyrics
    extern const char* SONG_COELACANTH;
    extern const char* SONG_SNAKE;
    extern const char* SONG_SINK;
    extern const char* SONG_YOU;

    // Get song lyrics by name
    std::string get_song_lyrics(const std::string& song_name);

    // Command check helpers
    bool is_demo_command(const std::string& cmd);
    bool is_sing_command(const std::string& cmd);

    // Demo state and skip control per window
    bool is_demo_running(TerminalWindow* tw);
    void request_skip(TerminalWindow* tw);

    // Signal a running demo/sing thread to abort entirely (e.g. window is closing).
    // Sticky for the lifetime of tw; caller must join the thread that ran
    // run_demo/run_sing before deleting tw.
    void request_abort(TerminalWindow* tw);

    // Execute built-in demo sequence
    void run_demo(TerminalWindow* tw, AppState* state);

    // Execute built-in sinksing command
    void run_sing(TerminalWindow* tw, const std::string& song_name);
}
