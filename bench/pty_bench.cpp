// End-to-end throughput benchmark: real pty, real shell, real parser.
//
// bench/sink_bench measures the parser starting from a buffer that is already
// in memory, and bench/render_bench measures frame building from a grid that
// is already populated. Neither can see the step in between -- the reader
// thread pulling bytes off the pty master and handing them to the main thread
// -- which is the only part of the pipeline that involves two threads and a
// lock, and which for a long time cost about as much as parsing did.
//
// This spawns the user's login shell through PTYBridge exactly as the app
// does, has it cat a workload file, and drains it the way SDL_AppIterate does:
// read_pending() once per simulated frame, then parse everything it returned.
// The number reported is what the terminal can actually swallow.
//
// It depends on the user's shell and machine, so treat it as a before/after
// measure of this program rather than a cross-terminal comparison; Alacritty's
// vtebench (see bench/run_vtebench.sh) is the tool for that.
#include "ansi_parser.hpp"
#include "terminal_grid.hpp"
#include "pty_bridge.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using clk = std::chrono::steady_clock;

// Drains and discards whatever the shell emits until it goes quiet for
// `quiet_ms`, so login-shell startup and prompt painting stay out of the
// measurement.
static void settle(PTYBridge& pty, std::vector<char>& buf, int quiet_ms) {
    auto last = clk::now();
    while (std::chrono::duration<double, std::milli>(clk::now() - last).count() < quiet_ms) {
        pty.read_pending(buf);
        if (!buf.empty()) last = clk::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

int main(int argc, char** argv) {
    std::string workload = (argc > 1) ? argv[1] : "bench/workloads/plain_text.bin";
    const int cols = 200, rows = 50;

    std::FILE* f = std::fopen(workload.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s (run bench/gen_workloads.py)\n", workload.c_str()); return 1; }
    std::fseek(f, 0, SEEK_END);
    long workload_bytes = std::ftell(f);
    std::fclose(f);

    TerminalGrid grid;
    grid.resize(cols, rows);
    ANSIParser parser;
    PTYBridge pty;
    if (!pty.spawn(cols, rows)) { std::fprintf(stderr, "pty spawn failed\n"); return 1; }

    std::vector<char> chunk;
    settle(pty, chunk, 400);
    // Echo would double the command line back at us and, more importantly,
    // put the completion marker in the stream before the workload even starts.
    pty.write_to_pty("stty -echo\n", 11);
    settle(pty, chunk, 300);

    // The marker is split so that the *command* containing it does not itself
    // match, in case the shell echoes despite the above.
    const char* kMarker = "DONE_SINK_PTY_BENCH";
    std::string cmd = "cat '" + workload + "'; echo D\"\"ONE_SINK_PTY_BENCH\n";

    size_t total = 0;
    bool done = false;
    std::string tail; // carries the last few bytes so a split marker still matches

    auto t0 = clk::now();
    pty.write_to_pty(cmd.data(), cmd.size());

    // One drain per simulated 60fps frame, matching SDL_AppIterate.
    auto deadline = t0 + std::chrono::seconds(120);
    while (!done && clk::now() < deadline) {
        pty.read_pending(chunk);
        if (chunk.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        total += chunk.size();
        parser.parse(grid, chunk.data(), chunk.size());

        tail.append(chunk.data(), chunk.size());
        if (tail.find(kMarker) != std::string::npos) done = true;
        if (tail.size() > 64) tail.erase(0, tail.size() - 64);
    }
    auto t1 = clk::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();

    pty.shutdown();

    if (!done) { std::fprintf(stderr, "timed out after %zu bytes\n", total); return 1; }

    std::printf("workload    : %s (%.1f MB)\n", workload.c_str(), workload_bytes / 1048576.0);
    std::printf("received    : %.1f MB\n", total / 1048576.0);
    std::printf("elapsed     : %.3f s\n", secs);
    std::printf("throughput  : %.1f MB/s   (pty read -> hand-off -> parse)\n",
                (total / 1048576.0) / secs);
    return 0;
}
