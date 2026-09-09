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

PTYBridge::PTYBridge() {}

PTYBridge::~PTYBridge() {
    shutdown();
}

bool PTYBridge::spawn(int cols, int rows) {
    struct winsize ws;
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;

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

        // Create stub PATH entries for built-in sink commands (sinkdemo and sinksing)
        mkdir("/tmp/.sink_bin", 0755);
        FILE* f1 = fopen("/tmp/.sink_bin/sinkdemo", "w");
        if (f1) { fprintf(f1, "#!/bin/sh\nexit 0\n"); fclose(f1); chmod("/tmp/.sink_bin/sinkdemo", 0755); }
        FILE* f2 = fopen("/tmp/.sink_bin/sinksing", "w");
        if (f2) { fprintf(f2, "#!/bin/sh\nexit 0\n"); fclose(f2); chmod("/tmp/.sink_bin/sinksing", 0755); }

        const char* old_path = getenv("PATH");
        std::string new_path = "/tmp/.sink_bin";
        if (old_path) new_path += ":" + std::string(old_path);
        setenv("PATH", new_path.c_str(), 1);

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
