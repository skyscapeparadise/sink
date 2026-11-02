// Frees panes mid-flood, over and over, to prove a pane's pty reader thread is
// always stopped before anything it uses is destroyed.
//
// Each round opens a Pane on a shell that never stops writing (`yes`), drains
// and parses its output on this thread the way the frame does, and frees the
// pane at a random moment -- without calling shutdown(), so ~Pane has to.
// Every tenth round stops reading first, so the pane is freed while its reader
// is stalled on the 4MB backpressure cap rather than mid-read.
//
// On its own this only finds crashes. Built with -DSINK_TSAN=ON it finds a
// reader that races what's being destroyed, and under AddressSanitizer one
// that touches freed memory. See docs/threading-plan.md, phase 2.
//
//   pane_lifetime_test [rounds]     (default 1000)
#include "app_state.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <thread>
#include <vector>

namespace {
std::atomic<long> g_notified{0};
void on_output() { g_notified.fetch_add(1, std::memory_order_relaxed); }
} // namespace

int main(int argc, char** argv) {
    const int rounds = argc > 1 ? std::atoi(argv[1]) : 1000;

    PTYBridge::ShellSettings sh;
    sh.command = "/usr/bin/yes";
    sh.args = {"sink pane lifetime test: a line of output, again and again"};
    sh.login = false;
    PTYBridge::set_shell_settings(sh);
    PTYBridge::set_shell_integration_enabled(false);
    PTYBridge::set_output_notify(on_output);

    std::mt19937 rng(20261102);
    using clk = std::chrono::steady_clock;
    long long parsed = 0;
    std::vector<char> buf;

    for (int round = 0; round < rounds; ++round) {
        auto pane = std::make_unique<Pane>();
        pane->lock()->resize(80, 24);
        if (!pane->pty.spawn(80, 24)) {
            std::fprintf(stderr, "round %d: spawn failed\n", round);
            return 1;
        }
        const bool stall = round % 10 == 9;
        const auto until = clk::now() + std::chrono::milliseconds(rng() % (stall ? 60 : 20));
        while (clk::now() < until) {
            if (stall) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            pane->pty.read_pending(buf);
            if (!buf.empty()) {
                pane->lock().parse(buf.data(), buf.size());
                parsed += static_cast<long long>(buf.size());
            }
        }
        pane.reset();  // no shutdown(): ~Pane must stop the reader first
    }

    if (parsed == 0 || g_notified.load() == 0) {
        std::fprintf(stderr, "no output was ever read (%lld bytes, %ld notifications): the test tested nothing\n",
                     parsed, g_notified.load());
        return 1;
    }
    std::printf("%d panes freed mid-flood; %.1f MB parsed, %ld output notifications\n",
                rounds, parsed / 1048576.0, g_notified.load());
    return 0;
}
