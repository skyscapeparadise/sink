// Headless throughput benchmark for sink's actual ANSI parser + terminal
// grid -- the same two classes (ANSIParser, TerminalGrid) the real app
// feeds every byte the PTY produces through. No window, renderer, or GPU
// work is involved: this measures parsing + cell/cursor/scrollback
// bookkeeping only, which is the CPU-bound half of "how fast is this
// terminal" -- actual glyph rasterization and compositing is a separate,
// GPU/display-bound cost this doesn't (and can't, headlessly) measure.
#include "ansi_parser.hpp"
#include "terminal_grid.hpp"

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

    return 0;
}
