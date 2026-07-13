#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <queue>
#include <thread>
#include <atomic>
#include <sys/types.h>

class PTYBridge {
public:
    PTYBridge();
    ~PTYBridge();

    bool spawn(int cols, int rows);
    void shutdown();

    // Send resize dimensions to the OS pseudo-terminal
    void resize_pty(int cols, int rows);

    // Write input bytes (characters or escapes) to the shell
    bool write_to_pty(const char* data, size_t size);

    // Read all currently accumulated output from the background read thread queue
    std::vector<char> read_pending();

    bool is_running() const { return running_; }

private:
    int master_fd_ = -1;
    pid_t child_pid_ = -1;
    std::atomic<bool> running_{false};
    std::thread read_thread_;

    // Thread-safe read queue
    std::mutex queue_mutex_;
    std::queue<char> read_queue_;

    void read_loop();
};
