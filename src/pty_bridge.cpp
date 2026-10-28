#include "pty_bridge.hpp"
#include "private_files.hpp"
#include "shell_integration.hpp"
#include <iostream>
#include <unistd.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#include <chrono>
#include <vector>
#include <sys/stat.h>
#include <cerrno>

#include <climits>
#include <cstdlib>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <util.h>
#else
#include <pty.h>
#endif

namespace {

// The directory of stub executables for sink's built-in commands (sinkdemo,
// sinksing), which spawn() prepends to the child shell's PATH.
//
// It lives under $HOME rather than /tmp. /tmp is world-writable, so the old
// location was a directory any other account -- or any unprivileged process
// on the machine -- could create first and fill with executables named `ls`,
// `git` or `sudo`. sink would then find its mkdir() already satisfied and
// prepend that attacker-owned directory to PATH, so those names shadowed the
// real binaries in every shell sink opened: arbitrary code execution as the
// user, on every launch, persisting until /tmp was cleared.
//
// Prepending to PATH is only safe if nobody but the user can write to what is
// being prepended, so the directory is verified after creation and the whole
// feature is dropped (empty return) if it does not check out. Computed once:
// the result is reused by every later spawn.
// This program's own path, symlinks resolved.
std::string own_executable() {
#if defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return {};
    char real[PATH_MAX];
    return realpath(buf, real) ? std::string(real) : std::string(buf);
#else
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
#endif
}

// `s` as one sh word: single-quoted, any single quote in it closed and
// escaped, so a path with spaces or quotes stays one argument.
std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

const std::string& stub_bin_dir() {
    static const std::string dir = [] () -> std::string {
        std::string root = private_files::config_root();
        if (root.empty()) return {};
        std::string path = root + "/bin";
        if (!private_files::ensure_dir(path)) return {};
        static const char kStub[] = "#!/bin/sh\nexit 0\n";
        if (!private_files::write_file(path + "/sinkdemo", kStub, 0700)) return {};
        if (!private_files::write_file(path + "/sinksing", kStub, 0700)) return {};
        // sinkview runs this sink as `sink --view` (sinkview.hpp). Rewritten
        // every launch, so it follows sink.app when it moves or updates.
        const std::string self = own_executable();
        if (!self.empty()) {
            const std::string view = "#!/bin/sh\nexec " + shell_quote(self) + " --view \"$@\"\n";
            private_files::write_file(path + "/sinkview", view, 0700);
        }
        return path;
    }();
    return dir;
}

// The installed shell integration (see shell_integration.hpp), or empty if it
// couldn't be installed safely, in which case shells start without it.
// Computed once per run, so every shell gets the scripts this build carries.
const std::string& integration_dir() {
    static const std::string dir = shell_integration::install(private_files::config_root());
    return dir;
}

} // namespace

bool PTYBridge::shell_integration_enabled_ = true;
PTYBridge::ShellSettings PTYBridge::shell_settings_;

std::string PTYBridge::find_program(const std::string& command) {
    if (command.empty()) return std::string();
    if (command.find('/') != std::string::npos) {
        return access(command.c_str(), X_OK) == 0 ? command : std::string();
    }
    std::vector<std::string> dirs;
    if (const char* path = getenv("PATH")) {
        std::string p = path;
        for (size_t start = 0, end; start <= p.size(); start = end + 1) {
            end = p.find(':', start);
            if (end == std::string::npos) end = p.size();
            if (end > start) dirs.push_back(p.substr(start, end - start));
        }
    }
    dirs.push_back("/opt/homebrew/bin");
    dirs.push_back("/usr/local/bin");
    for (const std::string& dir : dirs) {
        const std::string candidate = dir + "/" + command;
        if (access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    // /etc/shells lists the shells installed for login use, by full path.
    if (FILE* f = fopen("/etc/shells", "r")) {
        char line[1024];
        std::string found;
        while (found.empty() && fgets(line, sizeof(line), f)) {
            std::string entry = line;
            while (!entry.empty() && (entry.back() == '\n' || entry.back() == '\r' || entry.back() == ' ')) entry.pop_back();
            if (entry.empty() || entry[0] == '#') continue;
            const size_t slash = entry.rfind('/');
            if (slash != std::string::npos && entry.substr(slash + 1) == command &&
                access(entry.c_str(), X_OK) == 0) {
                found = entry;
            }
        }
        fclose(f);
        if (!found.empty()) return found;
    }
    return std::string();
}

PTYBridge::PTYBridge() {}

PTYBridge::~PTYBridge() {
    shutdown();
}

bool PTYBridge::spawn(int cols, int rows, const std::string& cwd, int cell_px_w, int cell_px_h) {
    struct winsize ws;
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_xpixel = static_cast<unsigned short>(cols * cell_px_w);
    ws.ws_ypixel = static_cast<unsigned short>(rows * cell_px_h);

    // Resolved before the fork: this touches the filesystem and allocates,
    // neither of which is safe to do between fork() and exec() in a process
    // that has other threads running (sink's reader threads do).
    const std::string& stub_bin = stub_bin_dir();

    // The shell and how to start it. Also before the fork, for the same
    // reason: installing the integration scripts writes files.
    const char* shell_env = getenv("SHELL");
#if defined(__APPLE__)
    std::string shell = shell_env ? shell_env : "/bin/zsh";
#else
    std::string shell = shell_env ? shell_env : "/bin/bash";
#endif
    // [shell] command, when it names something that's there. One that isn't
    // falls back to the login shell rather than leaving a dead terminal.
    const ShellSettings settings = shell_settings_;
    if (!settings.command.empty()) {
        const std::string program = find_program(settings.command);
        if (!program.empty()) {
            shell = program;
        } else {
            fprintf(stderr, "sink: [shell] command \"%s\" not found; starting %s\n",
                    settings.command.c_str(), shell.c_str());
        }
    }
    const shell_integration::Launch launch =
        shell_integration::plan(shell, shell_integration_enabled_ ? integration_dir() : std::string(),
                                settings.login, settings.args, settings.env);

    running_ = true;
    child_pid_ = forkpty(&master_fd_, nullptr, nullptr, &ws);

    if (child_pid_ < 0) {
        perror("forkpty");
        running_ = false;
        return false;
    }

    if (child_pid_ == 0) {
        // --- Child Process ---
        // Establish its own process group so we can kill it and all sub-processes together
        setpgid(0, 0);

        // Configure standard terminal environment variables
        setenv("TERM", "xterm-256color", 1);
        setenv("LANG", "en_US.UTF-8", 1);
        setenv("LC_ALL", "en_US.UTF-8", 1);

        // Start where the caller asked, if it asked. A failure here is not
        // fatal -- the directory may have been deleted since the shell that
        // reported it last ran -- so fall through to the checks below and
        // start somewhere sane instead.
        if (!cwd.empty()) {
            chdir(cwd.c_str());
        } else if (!settings.working_directory.empty()) {
            chdir(settings.working_directory.c_str());
        }

        // Change working directory to user home if launched with root directory (Finder launch default)
        char cwd_buf[1024];
        if (getcwd(cwd_buf, sizeof(cwd_buf))) {
            if (std::string(cwd_buf) == "/") {
                const char* home = getenv("HOME");
                if (home) {
                    chdir(home);
                }
            }
        }

        // Prepend the stub directory holding sink's built-in commands. It is
        // built and vetted by the parent before the fork (see stub_bin_dir);
        // an empty string means it could not be trusted, in which case PATH is
        // left exactly as it was.
        if (!stub_bin.empty()) {
            const char* old_path = getenv("PATH");
            std::string new_path = stub_bin;
            if (old_path) new_path += ":" + std::string(old_path);
            setenv("PATH", new_path.c_str(), 1);
        }

        // Ensure child shell runs with UTF-8 locale support and xterm capabilities
        setenv("LANG", "en_US.UTF-8", 1);
        setenv("LC_ALL", "en_US.UTF-8", 1);
        setenv("TERM", "xterm-256color", 1);

        // TERM says what escape sequences work; TERM_PROGRAM says who is
        // reading them. Plenty of tools branch on it -- to pick a cursor
        // shape, to decide whether OSC 8 links are worth emitting, to work
        // around known quirks -- and with it unset sink was lumped in with
        // whatever they assume for an unknown terminal.
        setenv("TERM_PROGRAM", "sink", 1);
        setenv("TERM_PROGRAM_VERSION", SINK_VERSION, 1);

        // [shell.env], then what the integration needs on top (it already
        // took these into account; see shell_integration::plan).
        for (const auto& kv : settings.env) setenv(kv.first.c_str(), kv.second.c_str(), 1);

        // The shell, with shell integration if it applies -- worked out
        // before the fork (see launch above); only applied here.
        for (const auto& kv : launch.set) setenv(kv.first.c_str(), kv.second.c_str(), 1);
        for (const auto& key : launch.unset) unsetenv(key.c_str());
        std::vector<char*> argv;
        for (const std::string& arg : launch.argv) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execv(argv[0], argv.data());

        // Fallback to basic /bin/sh
        execl("/bin/sh", "/bin/sh", (char*)nullptr);
        _exit(1);
    }

    // --- Parent Process ---
    read_thread_ = std::thread(&PTYBridge::read_loop, this);
    return true;
}

void PTYBridge::shutdown() {
    running_ = false;
    // Join the read thread before closing master_fd_. read_loop() may be
    // blocked in poll()/read() on that fd right now; closing it out from
    // under another thread that's using it is a race (the fd number can be
    // reused by an unrelated open() elsewhere before the in-flight
    // poll()/read() call returns, causing it to observe the wrong file).
    // read_loop() rechecks running_ at least every 100ms (its poll timeout),
    // so this join returns promptly without needing the fd closed to wake it.
    if (read_thread_.joinable()) {
        read_thread_.join();
    }
    if (master_fd_ != -1) {
        ::close(master_fd_);
        master_fd_ = -1;
    }
    if (child_pid_ != -1) {
        int status;
        // Send terminate signal to the entire process group (indicated by negative PID)
        kill(-child_pid_, SIGTERM);

        // Give processes a moment to shut down gracefully before sending SIGKILL
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        kill(-child_pid_, SIGKILL);

        waitpid(child_pid_, &status, 0);
        child_pid_ = -1;
    }
}

void PTYBridge::resize_pty(int cols, int rows, int cell_px_w, int cell_px_h) {
    if (master_fd_ == -1) return;
    struct winsize ws;
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_xpixel = static_cast<unsigned short>(cols * cell_px_w);
    ws.ws_ypixel = static_cast<unsigned short>(rows * cell_px_h);
    ioctl(master_fd_, TIOCSWINSZ, &ws);
}

bool PTYBridge::write_to_pty(const char* data, size_t size) {
    if (master_fd_ == -1) return false;
    // A single write() call is not guaranteed to consume the whole buffer
    // (e.g. a large clipboard paste can exceed the pty's internal buffer
    // space); retry until every byte is written or a real error occurs.
    size_t total_written = 0;
    while (total_written < size) {
        ssize_t written = ::write(master_fd_, data + total_written, size - total_written);
        if (written < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            return false;
        }
        total_written += static_cast<size_t>(written);
    }
    return true;
}

void PTYBridge::read_pending(std::vector<char>& out) {
    out.clear();
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    // Swap rather than copy: `out` comes back holding the accumulated bytes,
    // and read_buffer_ takes over out's now-empty storage for the next frame.
    // Both capacities survive, so at steady state neither side allocates.
    out.swap(read_buffer_);
}

void PTYBridge::read_loop() {
    // 64K rather than 1K. A program dumping output at full tilt otherwise
    // costs one poll() and one read() per kilobyte, which is the bulk of what
    // this thread does once the byte-at-a-time queue is gone.
    std::vector<char> buffer(64 * 1024);
    struct pollfd pfd;
    pfd.fd = master_fd_;
    pfd.events = POLLIN;

    while (running_) {
        // Backpressure. If the main thread has fallen behind, leave the bytes
        // in the pty instead of buffering them here without limit: the
        // kernel's buffer fills, the child blocks in write(), and it resumes
        // as soon as read_pending() drains. Rechecks often enough that
        // shutdown() still joins promptly.
        size_t pending = 0;
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            pending = read_buffer_.size();
        }
        if (pending >= kMaxPendingBytes) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        // Poll the master file descriptor with a 100ms timeout
        int ret = poll(&pfd, 1, 100);
        if (ret > 0) {
            if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
                ssize_t bytes_read = ::read(master_fd_, buffer.data(), buffer.size());
                if (bytes_read > 0) {
                    std::lock_guard<std::mutex> lock(buffer_mutex_);
                    read_buffer_.insert(read_buffer_.end(), buffer.data(), buffer.data() + bytes_read);
                } else {
                    // EOF or descriptor closed
                    running_ = false;
                    break;
                }
            }
        } else if (ret < 0) {
            if (errno == EINTR) continue;
            running_ = false;
            break;
        }
    }
}
