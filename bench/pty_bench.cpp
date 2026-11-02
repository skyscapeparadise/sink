// End-to-end benchmark: a real pty, a real child process, the app's parse path.
//
// bench/sink_bench starts from bytes already in memory and bench/render_bench
// from a grid already filled. Neither sees the reader thread pulling bytes off
// the pty and handing them to the main thread, which for a long time cost as
// much as parsing did. This does: it spawns a child through PTYBridge exactly
// as the app spawns a shell, and drains it the way SDL_AppIterate does.
//
// The child is /bin/cat, not the user's login shell. Earlier versions ran
// `cat` from the login shell, which put the user's rc files, prompt and
// plugins into the measurement and made it vary 10-30% run to run. It is
// started through /bin/sh only to turn off output post-processing (onlcr
// would turn every \n into \r\n, changing the byte count) and to wait for a
// go signal, so the clock starts when the first byte can flow, not at fork.
//
// Two modes per workload:
//   pty.<w>         free-running: drain and parse everything as fast as
//                   possible. How much the terminal can swallow.
//   pty_paced.<w>   one drain per 16.7ms frame, parsing through
//                   sync_output::parse with the app's own per-frame budget
//                   (sync_output::kFrameParseBudgetNs). What a user actually
//                   gets, and what one frame costs while output pours in.
//
//   pty_threaded.<w>            the threaded parser (SINK_THREADED_PARSER):
//                               the app's own Pane and
//                               pane_output::parse_on_reader parse on the pty
//                               reader thread, while this thread plays the
//                               renderer, locking the pane and reading the
//                               whole screen every 4ms. A frame comes every
//                               16.7ms, but where it lands against the
//                               parser's work is effectively random either
//                               way, and 4ms samples that four times as often.
//   pty_threaded.<w>.lock_wait  the longest any of those locks waited: what
//                               a frame can lose to the parser holding the
//                               pane during a flood (docs/threading-plan.md,
//                               phase 4). The typewriter is off for this mode
//                               -- no frame tick runs to type anything out.
//
// Checks, failing the run if any is off:
//   - exactly the file's bytes arrive, no more, no fewer
//   - the grid ends with the same fingerprint sink_bench's parser.<w> gets
//     from the same bytes in memory (bench/run.py compares the two)
#include "ansi_parser.hpp"
#include "app_state.hpp"
#include "harness.hpp"
#include "pane_output.hpp"
#include "pty_bridge.hpp"
#include "sync_output.hpp"
#include "terminal_grid.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kCols = 120, kRows = 50;  // sink_bench's size, so fingerprints compare
constexpr auto kFrame = std::chrono::microseconds(16667);

struct Run {
    bool ok = false;
    double seconds = 0;
    size_t bytes = 0;
    double worst_frame_ms = 0;  // paced: the longest any one frame spent parsing
    int frames = 0;
    std::string fingerprint;
};

uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        bench::clock::now().time_since_epoch()).count());
}

Run run_once(const std::string& path, size_t expect, bool paced) {
    Run r;
    PTYBridge::ShellSettings sh;
    sh.command = "/bin/sh";
    sh.login = false;
    sh.args = {"-c", "stty -opost -echo; printf R; read go; exec /bin/cat \"$0\"", path};
    PTYBridge::set_shell_settings(sh);
    PTYBridge::set_shell_integration_enabled(false);

    TerminalGrid grid;
    grid.resize(kCols, kRows);
    ANSIParser parser;
    sync_output::Hold hold;
    PTYBridge pty;
    if (!pty.spawn(kCols, kRows)) { std::fprintf(stderr, "pty spawn failed\n"); return r; }

    // Wait for the child's "R": stty has run, and cat starts on our newline.
    std::vector<char> chunk;
    auto give_up = bench::clock::now() + std::chrono::seconds(10);
    bool ready = false;
    while (!ready && bench::clock::now() < give_up) {
        pty.read_pending(chunk);
        for (char c : chunk) if (c == 'R') ready = true;
        if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!ready) { std::fprintf(stderr, "child never signalled ready\n"); pty.shutdown(); return r; }

    std::vector<char> pending;
    size_t received = 0;
    auto t0 = bench::clock::now();
    pty.write_to_pty("\n", 1);
    auto deadline = t0 + std::chrono::seconds(60);
    auto next_frame = t0;

    while (bench::clock::now() < deadline) {
        if (paced) {
            next_frame += kFrame;
            std::this_thread::sleep_until(next_frame);
        }
        pty.read_pending(chunk);
        received += chunk.size();
        pending.insert(pending.end(), chunk.begin(), chunk.end());

        if (!pending.empty()) {
            const uint64_t start = now_ns();
            size_t used;
            if (paced) {
                // main.cpp's parse_within_budget(), with the same budget.
                used = sync_output::parse(parser, grid, hold, pending.data(), pending.size(), start / 1000000ull,
                                          [start]() { return now_ns() - start >= sync_output::kFrameParseBudgetNs; });
            } else {
                used = sync_output::parse(parser, grid, hold, pending.data(), pending.size(), start / 1000000ull,
                                          []() { return false; });
            }
            r.worst_frame_ms = std::max(r.worst_frame_ms, (now_ns() - start) / 1e6);
            ++r.frames;
            pending.erase(pending.begin(), pending.begin() + static_cast<long>(used));
            if (grid.has_pending_reply()) (void)grid.take_pending_reply();
        } else if (!paced) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        if (received >= expect && pending.empty()) break;
    }
    r.seconds = bench::seconds_since(t0);
    // Anything more would be a bug: give the child a moment to show it.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    pty.read_pending(chunk);
    received += chunk.size();
    pty.shutdown();

    r.bytes = received;
    r.ok = received == expect;
    if (!r.ok) std::fprintf(stderr, "%s: received %zu bytes, expected %zu\n", path.c_str(), received, expect);
    r.fingerprint = bench::grid_fingerprint(grid);
    return r;
}

struct ThreadedRun {
    bool ok = false;
    double seconds = 0;
    double wait_max_ms = 0;
    std::string fingerprint;
};

// `path` empty: the control. Same pane, same probes, a child that prints
// nothing for half a second -- so its lock waits are the probe's own noise
// (the scheduler, the machine), the floor the real waits are judged against.
ThreadedRun run_threaded(const std::string& path) {
    ThreadedRun r;
    PTYBridge::ShellSettings sh;
    sh.command = "/bin/sh";
    sh.login = false;
    if (path.empty()) sh.args = {"-c", "stty -opost -echo; read go; exec /bin/sleep 0.5"};
    else sh.args = {"-c", "stty -opost -echo; read go; exec /bin/cat \"$0\"", path};
    PTYBridge::set_shell_settings(sh);
    PTYBridge::set_shell_integration_enabled(false);

    auto pane = std::make_unique<Pane>();
    pane->lock()->resize(kCols, kRows);
    pane->typing_animation.store(false);
    pane_output::attach_reader_parser(*pane);
    if (!pane->pty.spawn(kCols, kRows)) { std::fprintf(stderr, "pty spawn failed\n"); return r; }
    // stty runs, then the child waits on `read`; it prints nothing, so give
    // it time rather than waiting for a signal through the parser.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    auto t0 = bench::clock::now();
    pane->pty.write_to_pty("\n", 1);
    constexpr auto kProbe = std::chrono::milliseconds(4);
    auto next_frame = t0 + kProbe;
    const auto deadline = t0 + std::chrono::seconds(60);
    // The reader exits at EOF, after parsing everything cat wrote. Checked
    // every half millisecond so the throughput isn't rounded to frames.
    while (pane->pty.is_running() && bench::clock::now() < deadline) {
        if (bench::clock::now() >= next_frame) {
            next_frame += kProbe;
            auto w0 = bench::clock::now();
            auto g = pane->lock();
            r.wait_max_ms = std::max(r.wait_max_ms, bench::seconds_since(w0) * 1000.0);
            (void)bench::screen_text_hash(g.grid());  // a frame's worth of reading the grid
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    r.seconds = bench::seconds_since(t0);
    r.ok = !pane->pty.is_running();
    if (!r.ok) std::fprintf(stderr, "%s: timed out\n", path.empty() ? "control" : path.c_str());
    r.fingerprint = bench::grid_fingerprint(pane->lock().grid());
    return r;
}

} // namespace

int main(int argc, char** argv) {
    bench::Options opt = bench::parse_options(argc, argv);
    std::FILE* json_out = opt.json ? bench::json_stream() : stdout;
    const std::string dir = opt.positional.empty() ? "bench/workloads" : opt.positional[0];
    const int samples = opt.samples > 0 ? opt.samples : 10;
    const int warmup = opt.warmup >= 0 ? opt.warmup : 1;

    bench::Report report("pty_bench");
    report.config("cols", kCols);
    report.config("rows", kRows);
    report.config("frame_ms", 16.667);
    report.config("parse_budget_ms", sync_output::kFrameParseBudgetNs / 1e6);
    report.config("samples", samples);
    report.config("child", "/bin/sh -c 'stty -opost -echo; exec /bin/cat FILE'");

    int failures = 0;
    const char* files[][3] = {
        {"plain_text.bin",   "plain_text",   "plain text"},
        {"cursor_heavy.bin", "cursor_heavy", "cursor-heavy"},
        {"sync_tui.bin",     "sync_tui",     "synchronized-output TUI"},
    };
    for (int paced = 0; paced < 2; ++paced) {
        for (auto& f : files) {
            const std::string base = std::string(paced ? "pty_paced." : "pty.") + f[1];
            if (!opt.wants(base)) continue;
            const std::string path = dir + "/" + f[0];
            const size_t size = bench::read_file(path).size();
            const double mb = size / (1024.0 * 1024.0);

            bench::Metric tp{base, std::string(paced ? "paced: " : "pty: ") + f[2], "MB/s", "higher", {}, {}, {}};
            bench::Metric worst{base + ".worst_frame", std::string("paced: ") + f[2] + ", worst frame", "ms", "lower", {}, {}, {}};
            std::string fp;
            for (int i = 0; i < warmup + samples; ++i) {
                Run r = run_once(path, size, paced);
                if (!r.ok) { ++failures; break; }
                if (fp.empty()) fp = r.fingerprint;
                else if (fp != r.fingerprint) {
                    std::fprintf(stderr, "%s: grid differs between runs\n", base.c_str());
                    ++failures;
                }
                if (i < warmup) continue;
                tp.samples.push_back(mb / r.seconds);
                worst.samples.push_back(r.worst_frame_ms);
            }
            tp.fingerprint = fp;
            tp.add_info("bytes", static_cast<double>(size));
            tp.add_info("same_as", std::string("parser.") + f[1]);
            report.add(std::move(tp));
            if (paced) {
                worst.add_info("frame_ms", 16.667);
                report.add(std::move(worst));
            }
        }
    }

    if (opt.wants("pty_threaded.idle")) {
        bench::Metric wait{"pty_threaded.idle.lock_wait", "threaded: no output (control), worst lock wait",
                           "ms", "lower", {}, {}, {}};
        for (int i = 0; i < warmup + samples; ++i) {
            ThreadedRun r = run_threaded("");
            if (!r.ok) { ++failures; break; }
            if (i >= warmup) wait.samples.push_back(r.wait_max_ms);
        }
        wait.add_info("note", "the probe's own noise: compare pty_threaded.*.lock_wait against this");
        report.add(std::move(wait));
    }
    // Images too, for the threaded parser: decoding one happens inside the
    // parse, so it's where a long hold of the pane's lock would come from.
    const char* threaded_files[][3] = {
        {"plain_text.bin",   "plain_text",   "plain text"},
        {"cursor_heavy.bin", "cursor_heavy", "cursor-heavy"},
        {"sync_tui.bin",     "sync_tui",     "synchronized-output TUI"},
        {"images.bin",       "images",       "sixel + kitty images"},
    };
    for (auto& f : threaded_files) {
        const std::string base = std::string("pty_threaded.") + f[1];
        if (!opt.wants(base)) continue;
        const std::string path = dir + "/" + f[0];
        const size_t size = bench::read_file(path).size();
        const double mb = size / (1024.0 * 1024.0);
        bench::Metric tp{base, std::string("threaded: ") + f[2], "MB/s", "higher", {}, {}, {}};
        bench::Metric wait{base + ".lock_wait", std::string("threaded: ") + f[2] + ", worst lock wait", "ms", "lower", {}, {}, {}};
        std::string fp;
        for (int i = 0; i < warmup + samples; ++i) {
            ThreadedRun r = run_threaded(path);
            if (!r.ok) { ++failures; break; }
            if (fp.empty()) fp = r.fingerprint;
            else if (fp != r.fingerprint) {
                std::fprintf(stderr, "%s: grid differs between runs\n", base.c_str());
                ++failures;
            }
            if (i < warmup) continue;
            tp.samples.push_back(mb / r.seconds);
            wait.samples.push_back(r.wait_max_ms);
        }
        tp.fingerprint = fp;
        tp.add_info("bytes", static_cast<double>(size));
        tp.add_info("same_as", std::string("parser.") + f[1]);
        report.add(std::move(tp));
        wait.add_info("frame_ms", 16.667);
        report.add(std::move(wait));
    }

    if (opt.json) report.write(json_out);
    else report.print_table(stdout);
    return failures ? 1 : 0;
}
