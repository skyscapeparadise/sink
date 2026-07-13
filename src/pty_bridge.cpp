#include "pty_bridge.hpp"
#include <iostream>
#include <unistd.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#include <chrono>

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
    if (read_thread_.joinable()) {
        read_thread_.join();
    }
}

void PTYBridge::resize_pty(int cols, int rows) {
    if (master_fd_ == -1) return;
    struct winsize ws;
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;
    ioctl(master_fd_, TIOCSWINSZ, &ws);
}

bool PTYBridge::write_to_pty(const char* data, size_t size) {
    if (master_fd_ == -1) return false;
    ssize_t written = ::write(master_fd_, data, size);
    return written == static_cast<ssize_t>(size);
}

std::vector<char> PTYBridge::read_pending() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    std::vector<char> data;
    data.reserve(read_queue_.size());
    while (!read_queue_.empty()) {
        data.push_back(read_queue_.front());
        read_queue_.pop();
    }
    return data;
}

void PTYBridge::read_loop() {
    char buffer[1024];
    struct pollfd pfd;
    pfd.fd = master_fd_;
    pfd.events = POLLIN;

    while (running_) {
        // Poll the master file descriptor with a 100ms timeout
        int ret = poll(&pfd, 1, 100);
        if (ret > 0) {
            if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
                ssize_t bytes_read = ::read(master_fd_, buffer, sizeof(buffer));
                if (bytes_read > 0) {
                    std::lock_guard<std::mutex> lock(queue_mutex_);
                    for (ssize_t i = 0; i < bytes_read; ++i) {
                        read_queue_.push(buffer[i]);
                    }
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
