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

    // Whether a submitted command line is one of sink's built-in commands.
    //
    // The line has to *be* the command: "sinkdemo", or "sinksing" optionally
    // followed by a song. These used to match the name anywhere in the line,
    // and the Enter key was swallowed for anything that matched -- so
    // `grep sinkdemo notes.txt`, or a commit message mentioning sinksing,
    // never ran. Inline so they can be tested without the demo itself.
    inline std::string trimmed_command(const std::string& cmd) {
        const char* ws = " \t\r\n";
        size_t first = cmd.find_first_not_of(ws);
        if (first == std::string::npos) return {};
        return cmd.substr(first, cmd.find_last_not_of(ws) - first + 1);
    }
    inline bool is_demo_command(const std::string& cmd) {
        return trimmed_command(cmd) == "sinkdemo";
    }
    inline bool is_sing_command(const std::string& cmd) {
        std::string line = trimmed_command(cmd);
        return line == "sinksing" ||
               (line.compare(0, 9, "sinksing ") == 0 || line.compare(0, 9, "sinksing\t") == 0);
    }
    // The song named after "sinksing", or empty for none.
    inline std::string sing_argument(const std::string& cmd) {
        if (!is_sing_command(cmd)) return {};
        return trimmed_command(trimmed_command(cmd).substr(8));
    }

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
