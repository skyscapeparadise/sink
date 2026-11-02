// Frees panes mid-flood, over and over, to prove a pane's pty reader thread is
// always stopped before anything it uses is destroyed.
//
// Each round opens a Pane on a shell that never stops writing (`yes`), drains
// and parses its output on this thread the way the frame does, and frees the
// pane at a random moment -- without calling shutdown(), so ~Pane has to.
// Every tenth round stops reading first, so the pane is freed while its reader
// is stalled on the 4MB backpressure cap rather than mid-read.
//
// --threaded installs the threaded parser (pane_output::parse_on_reader), so
// the reader thread parses while this thread plays the renderer: locking the
// pane every millisecond and reading the grid, as the frame does.
//
// On its own this only finds crashes. Built with -DSINK_TSAN=ON it finds a
// reader that races what's being destroyed or what the "renderer" reads, and
// under AddressSanitizer one that touches freed memory. See
// docs/threading-plan.md, phases 2 and 3.
//
//   pane_lifetime_test [--threaded] [rounds]     (default 1000)
#include "app_state.hpp"
#include "pane_output.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <thread>
#include <string>
#include <vector>

namespace {
std::atomic<long> g_notified{0};
void on_output() { g_notified.fetch_add(1, std::memory_order_relaxed); }
} // namespace

int main(int argc, char** argv) {
    bool threaded = false;
    int rounds = 1000;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--threaded") threaded = true;
        else rounds = std::atoi(argv[i]);
    }

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
    int rounds_with_output = 0;  // threaded: rounds where the grid showed parsed output
    std::vector<char> buf;

    for (int round = 0; round < rounds; ++round) {
        auto pane = std::make_unique<Pane>();
        pane->lock()->resize(80, 24);
        if (threaded) pane_output::attach_reader_parser(*pane);
        if (!pane->pty.spawn(80, 24)) {
            std::fprintf(stderr, "round %d: spawn failed\n", round);
            return 1;
        }
        const bool stall = round % 10 == 9;
        const auto until = clk::now() + std::chrono::milliseconds(rng() % (stall ? 60 : 20));
        while (clk::now() < until) {
            if (threaded) {
                // The reader parses; this is the renderer reading what it made.
                {
                    auto g = pane->lock();
                    parsed += g->get_cursor_row() + g->get_cell_at(0, 0).codepoint % 2;
                    parsed += static_cast<long long>(g->get_scrollback_size() > 0);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
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
        if (threaded) {
            // `yes` fills 24 rows within a millisecond or two, so most rounds
            // should end with lines in scrollback -- proof the reader parsed.
            auto g = pane->lock();
            if (g->get_scrollback_size() > 0) ++rounds_with_output;
        }
        pane.reset();  // no shutdown(): ~Pane must stop the reader first
    }

    if (parsed == 0 || (!threaded && g_notified.load() == 0)) {
        std::fprintf(stderr, "no output was ever read (%lld bytes, %ld notifications): the test tested nothing\n",
                     parsed, g_notified.load());
        return 1;
    }
    if (threaded) {
        if (rounds_with_output < rounds / 2) {
            std::fprintf(stderr, "only %d of %d rounds showed parsed output: the reader isn't parsing\n",
                         rounds_with_output, rounds);
            return 1;
        }
        std::printf("%d panes freed mid-flood while their readers parsed; %d showed parsed output\n",
                    rounds, rounds_with_output);
        return 0;
    }
    std::printf("%d panes freed mid-flood; %.1f MB parsed, %ld output notifications\n",
                rounds, parsed / 1048576.0, g_notified.load());
    return 0;
}
