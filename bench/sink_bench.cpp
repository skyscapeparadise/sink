// Headless throughput benchmark for sink's actual ANSI parser + terminal
// grid -- the same two classes (ANSIParser, TerminalGrid) the real app
// feeds every byte the PTY produces through. No window, renderer, or GPU
// work is involved: this measures parsing + cell/cursor/scrollback
// bookkeeping only, which is the CPU-bound half of "how fast is this
// terminal" -- actual glyph rasterization and compositing is a separate,
// GPU/display-bound cost this doesn't (and can't, headlessly) measure.
#include "ansi_parser.hpp"
#include "terminal_grid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

static std::vector<char> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "could not open %s\n", path.c_str());
        std::exit(1);
    }
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Feeds `data` through the parser in ~8KB chunks, matching the rough
// granularity of a real PTY read() rather than one giant call -- so any
// per-parse-call overhead in ANSIParser is represented proportionally to
// how it'd actually be invoked.
static double run_once(const std::vector<char>& data, int grid_cols, int grid_rows) {
    TerminalGrid grid;
    grid.resize(grid_cols, grid_rows);
    ANSIParser parser;

    constexpr size_t kChunk = 8192;
    auto start = std::chrono::steady_clock::now();
    for (size_t off = 0; off < data.size(); off += kChunk) {
        size_t len = std::min(kChunk, data.size() - off);
        parser.parse(grid, data.data() + off, len);
    }
    auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(end - start).count();
}

struct Workload {
    const char* file;
    const char* label;
};

int main(int argc, char** argv) {
    std::string dir = (argc > 1) ? argv[1] : "bench/workloads";
    const int kRuns = 5; // best-of-N to smooth out scheduler noise

    Workload workloads[] = {
        {"plain_text.bin", "plain text (cat-like)"},
        {"colored_text.bin", "SGR-heavy (colorized ls-like)"},
        {"cursor_heavy.bin", "cursor-heavy (TUI redraw-like)"},
        {"unicode_heavy.bin", "UTF-8 heavy (emoji/CJK/box-drawing)"},
    };

    std::printf("%-32s %10s %12s\n", "workload", "MB/s", "best time (s)");
    std::printf("%-32s %10s %12s\n", "--------", "----", "-------------");

    for (const auto& w : workloads) {
        std::vector<char> data = read_file(dir + "/" + w.file);
        double mb = static_cast<double>(data.size()) / (1024.0 * 1024.0);

        double best = 1e9;
        for (int i = 0; i < kRuns; ++i) {
            double t = run_once(data, 120, 50);
            best = std::min(best, t);
        }

        std::printf("%-32s %10.1f %12.4f\n", w.label, mb / best, best);
    }

    // Find-bar cost. set_search_query() rescans the whole buffer, and the
    // find bar calls it on every keystroke, so this is what one keypress
    // costs -- not a one-off. It scales with scrollback depth, which is
    // user-configurable, so measure across the range people actually set.
    std::vector<char> corpus = read_file(dir + "/plain_text.bin");
    if (!corpus.empty()) {
        std::printf("\n%-32s %10s %12s\n", "find bar (per keystroke)", "ms", "matches");
        std::printf("%-32s %10s %12s\n", "------------------------", "--", "-------");
        for (int lines : {1000, 10000, 100000}) {
            TerminalGrid grid;
            grid.resize(200, 50);
            grid.set_max_scrollback(static_cast<size_t>(lines));
            ANSIParser parser;
            size_t take = std::min<size_t>(corpus.size(), static_cast<size_t>(lines) * 90 + 400000);
            parser.parse(grid, corpus.data(), take);

            double best = 1e9;
            for (int i = 0; i < kRuns; ++i) {
                // A fresh query each time: set_search_query does no caching,
                // but varying it keeps this honest if that ever changes.
                grid.set_search_query(i % 2 ? "0" : "1");
                auto t0 = std::chrono::steady_clock::now();
                grid.set_search_query("0");
                auto t1 = std::chrono::steady_clock::now();
                best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
            }
            char label[64];
            std::snprintf(label, sizeof(label), "%d lines of scrollback", lines);
            std::printf("%-32s %10.2f %12d\n", label, best, grid.get_search_match_count());
        }
    }

    return 0;
}
