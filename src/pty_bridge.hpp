#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <sys/types.h>

class PTYBridge {
public:
    PTYBridge();
    ~PTYBridge();

    // `cwd` is where the shell should start. Empty means inherit sink's own,
    // which is what a first window gets; a new tab or split passes the
    // directory the pane it came from reported through OSC 7.
    // The cell size in pixels goes in the size the shell is told too, so a
    // program can tell how big a picture will look (see sinkview).
    bool spawn(int cols, int rows, const std::string& cwd = std::string(), int cell_px_w = 0, int cell_px_h = 0);
    void shutdown();

    // Send resize dimensions to the OS pseudo-terminal. The pixel sizes fill
    // in ws_xpixel/ws_ypixel, which were left at 0 before: programs that ask
    // the kernel how big the window is in pixels (via TIOCGWINSZ) got zero and
    // had to guess.
    void resize_pty(int cols, int rows, int cell_px_w = 0, int cell_px_h = 0);

    // Write input bytes (characters or escapes) to the shell
    bool write_to_pty(const char* data, size_t size);

    // Hands over everything the reader thread has accumulated since the last
    // call. `out` is cleared and then *swapped* with the internal buffer, so
    // passing the same vector back every frame lets the two sides trade one
    // pair of allocations indefinitely and never grow either again.
    void read_pending(std::vector<char>& out);

    bool is_running() const { return running_; }

    // Whether shells start with sink's shell integration (shell_integration.hpp).
    // Set from the config before spawning; applies to shells started after.
    static void set_shell_integration_enabled(bool enabled) { shell_integration_enabled_ = enabled; }

    // sink.toml's [shell]: what to run in new terminals and how. Applies to
    // shells started after it's set.
    struct ShellSettings {
        std::string command;             // a path, a name to look up, or "" for the login shell
        std::vector<std::string> args;
        bool login = true;
        std::vector<std::pair<std::string, std::string>> env;
        std::string working_directory;   // absolute; "" for home
    };
    static void set_shell_settings(const ShellSettings& settings) { shell_settings_ = settings; }

    // `command` as an executable path: as given if it has a '/', else the
    // first match on PATH, in Homebrew's folders, or among /etc/shells.
    // Empty if there's none -- a Mac app started from Finder has a short
    // PATH, which is why the extra places are searched.
    static std::string find_program(const std::string& command);

private:
    static bool shell_integration_enabled_;
    static ShellSettings shell_settings_;
    int master_fd_ = -1;
    pid_t child_pid_ = -1;
    std::atomic<bool> running_{false};
    std::thread read_thread_;

    // Output accumulated by read_loop(), drained once a frame by read_pending().
    //
    // A vector appended to in bulk, not a std::queue<char> pushed and popped a
    // byte at a time as this used to be: that form measured 208 MB/s for the
    // hand-off *alone*, against a parser that runs at ~190 MB/s. Simply moving
    // bytes between the two threads cost about as much as parsing them, and
    // neither sink_bench (headless, starts after the pty) nor render_bench
    // could see it.
    std::mutex buffer_mutex_;
    std::vector<char> read_buffer_;

    // Stop reading once this much is waiting to be parsed. Nothing bounded the
    // buffer before: a command that outpaces the main thread (`yes`, a huge
    // `cat`) grew it for as long as it ran. Leaving the bytes in the pty
    // instead makes the kernel buffer fill and the child block in write(),
    // which is the backpressure a terminal is supposed to apply. At ~190 MB/s
    // this is a few frames' worth of backlog.
    static constexpr size_t kMaxPendingBytes = 4u << 20;

    void read_loop();
};
