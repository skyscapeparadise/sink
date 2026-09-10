#include "pty_bridge.hpp"
#include <iostream>
#include <unistd.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#include <chrono>
#include <sys/stat.h>
#include <cerrno>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

namespace {

// Writes one stub executable, replacing whatever is at `path`. Opened with
// O_NOFOLLOW and O_EXCL after an unlink, so a symlink planted at the target
// can never redirect the write somewhere else.
bool write_stub(const std::string& path) {
    ::unlink(path.c_str());
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0700);
    if (fd < 0) return false;
    static const char kBody[] = "#!/bin/sh\nexit 0\n";
    const size_t len = sizeof(kBody) - 1;
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::write(fd, kBody + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        off += static_cast<size_t>(n);
    }
    bool ok = (::fchmod(fd, 0700) == 0);
    ::close(fd);
    return ok;
}

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
const std::string& stub_bin_dir() {
    static const std::string dir = [] () -> std::string {
        const char* home = getenv("HOME");
        if (!home || !*home) return {};
        std::string base = std::string(home) + "/.config/sink";
        ::mkdir((std::string(home) + "/.config").c_str(), 0755);
        ::mkdir(base.c_str(), 0700);
        std::string path = base + "/bin";
        if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) return {};

        // Vet what we ended up with rather than assuming mkdir made it. It may
        // have already existed as something else entirely, and EEXIST above is
        // deliberately tolerated so a second launch reuses the first's work.
        struct stat st;
        if (::lstat(path.c_str(), &st) != 0) return {};
        if (!S_ISDIR(st.st_mode)) return {};           // a file, or a symlink to one
        if (st.st_uid != ::getuid()) return {};        // someone else owns it
        if (st.st_mode & (S_IWGRP | S_IWOTH)) return {}; // others can drop binaries in

        if (!write_stub(path + "/sinkdemo")) return {};
        if (!write_stub(path + "/sinksing")) return {};
        return path;
    }();
    return dir;
}

} // namespace

PTYBridge::PTYBridge() {}

PTYBridge::~PTYBridge() {
    shutdown();
}

bool PTYBridge::spawn(int cols, int rows, const std::string& cwd) {
    struct winsize ws;
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;

    // Resolved before the fork: this touches the filesystem and allocates,
    // neither of which is safe to do between fork() and exec() in a process
    // that has other threads running (sink's reader threads do).
    const std::string& stub_bin = stub_bin_dir();

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

        // Determine user shell
        const char* shell = getenv("SHELL");
        if (!shell) {
#if defined(__APPLE__)
            shell = "/bin/zsh";
#else
            shell = "/bin/bash";
#endif
        }

        // Replace process with a login shell
        execl(shell, shell, "-l", (char*)nullptr);
        
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
