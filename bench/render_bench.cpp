// Frame-time benchmark for sink's render path.
//
// bench/sink_bench.cpp is headless by design and measures parsing plus grid
// bookkeeping. It cannot see TerminalGrid::render(), which is where the other
// half of a frame goes: walking every cell, resolving SGR attributes into
// colours, looking up glyphs, and building the vertex/index arrays handed to
// SDL. That work is CPU-side and is what this measures.
//
// A hidden window and a real renderer are created because render() needs a
// live SDL_Renderer and a glyph atlas; vsync is disabled so the loop is not
// pinned to the display refresh. GPU execution is asynchronous, so the number
// reported is CPU time spent building a frame, not time to pixels -- which is
// the part that is worth optimizing here and the part that determines how much
// headroom the main thread has.
#include "ansi_parser.hpp"
#include "terminal_grid.hpp"
#include "font_manager.hpp"

#include <SDL3/SDL.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdlib>

static std::vector<char> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

struct Scene {
    const char* label;
    const char* workload;   // nullptr = leave the grid blank
    // Lines to scroll back before timing. The live view reads a contiguous
    // block of cells_; scrollback reads a deque of separately-allocated rows,
    // so the two are worth measuring apart.
    int scroll_back = 0;
    // Query to leave active in the search box while rendering.
    const char* search = nullptr;
};

int main(int argc, char** argv) {
    std::string dir = (argc > 1) ? argv[1] : "bench/workloads";
    std::string font = (argc > 2) ? argv[2] : "fonts/MonaspaceNeon-Regular.otf";
    // Frames per scene. Raise it to give a sampling profiler something to
    // attach to -- the default run is under a second.
    const int kFrames = (argc > 3) ? std::atoi(argv[3]) : 300;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("sink render bench", 1600, 900, SDL_WINDOW_HIDDEN);
    if (!win) { std::fprintf(stderr, "CreateWindow: %s\n", SDL_GetError()); return 1; }
    // SINK_BENCH_GPU=1 uses the renderer the app does (SDL's "gpu" backend);
    // SINK_BENCH_HDR=1 that one with the linear output HDR text uses, where SDL
    // converts every vertex color to linear. Neither set: SDL's default, as
    // the numbers in bench/README.md were measured.
    const bool hdr = std::getenv("SINK_BENCH_HDR") != nullptr;
    const bool gpu = hdr || std::getenv("SINK_BENCH_GPU") != nullptr;
    SDL_Renderer* ren = nullptr;
    if (gpu) {
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, win);
        SDL_SetStringProperty(props, SDL_PROP_RENDERER_CREATE_NAME_STRING, SDL_GPU_RENDERER);
        if (hdr) SDL_SetNumberProperty(props, SDL_PROP_RENDERER_CREATE_OUTPUT_COLORSPACE_NUMBER, SDL_COLORSPACE_SRGB_LINEAR);
        ren = SDL_CreateRendererWithProperties(props);
        SDL_DestroyProperties(props);
    } else {
        ren = SDL_CreateRenderer(win, nullptr);
    }
    if (!ren) { std::fprintf(stderr, "CreateRenderer: %s\n", SDL_GetError()); return 1; }
    SDL_SetRenderVSync(ren, 0);
    std::printf("renderer: %s%s\n", SDL_GetRendererName(ren), hdr ? " (linear output, as with HDR text)" : "");

    FontManager fm;
    if (!fm.initialize() || !fm.load_font(ren, font, 16.0f)) {
        std::fprintf(stderr, "font load failed (%s): %s\n", font.c_str(), SDL_GetError());
        return 1;
    }

    const int cols = 200, rows = 50;

    Scene scenes[] = {
        { "blank grid",              nullptr },
        { "plain text",              "plain_text.bin" },
        { "SGR-heavy (colorized)",   "colored_text.bin" },
        { "UTF-8 heavy",             "unicode_heavy.bin" },
        { "distinct CJK screenful",  "distinct_cjk.bin" },
        { "scrollback (deep)",       "plain_text.bin", 4000 },
        { "scrollback + search",     "plain_text.bin", 4000, "e" },
        // Generated here rather than a workload file: what a full-screen
        // program (vim, htop, tmux) draws -- every row on a painted background,
        // a few accent cells, a reversed status line. Cell backgrounds are
        // drawn as one rectangle per run of a color, which this measures.
        { "painted backgrounds",     "@painted" },
    };

    std::printf("\n%-26s %10s %10s %10s %12s\n",
                "scene", "ms/frame", "fps", "avg ms", "cells/frame");
    std::printf("%-26s %10s %10s %10s %12s\n",
                "-----", "--------", "---", "------", "-----------");

    for (const Scene& s : scenes) {
        TerminalGrid grid;
        grid.resize(cols, rows);
        if (s.workload && std::string(s.workload) == "@painted") {
            std::string screen;
            for (int row = 0; row < rows; ++row) {
                const std::string bg = row == rows - 1 ? "\x1b[7m" : (row % 3 == 0 ? "\x1b[44m" : "\x1b[48;5;236m");
                screen += "\x1b[" + std::to_string(row + 1) + ";1H" + bg;
                for (int c = 0; c < cols; ++c) {
                    if (c % 17 == 0) screen += "\x1b[41m" + std::string(1, static_cast<char>('a' + c % 26)) + bg;
                    else screen += static_cast<char>('a' + c % 26);
                }
                screen += "\x1b[0m";
            }
            ANSIParser p;
            p.parse(grid, screen.data(), screen.size());
        } else if (s.workload) {
            std::vector<char> data = read_file(dir + "/" + s.workload);
            if (data.empty()) { std::fprintf(stderr, "missing %s\n", s.workload); continue; }
            // Enough to fill the visible grid several times over, so what's on
            // screen is representative rather than a mostly-blank buffer.
            ANSIParser p;
            // A scrollback scene needs enough output behind it to actually
            // have scrolled that far back, so it feeds the workload repeatedly.
            size_t take = std::min<size_t>(data.size(), 400000);
            int passes = s.scroll_back > 0 ? 40 : 1;
            for (int i = 0; i < passes; ++i) p.parse(grid, data.data(), take);
        }
        if (s.scroll_back > 0) grid.scroll_view(s.scroll_back);
        if (s.search) {
            grid.set_search_query(s.search);
            grid.set_search_active(true);
        }

        // Warm the glyph atlas and let the driver settle before timing.
        for (int i = 0; i < 30; ++i) {
            SDL_RenderClear(ren);
            grid.render(ren, fm, 0.0f, 0.0f, 1.0f, 0.016f, false);
            SDL_RenderPresent(ren);
        }

        double best = 1e9, total = 0.0;
        for (int i = 0; i < kFrames; ++i) {
            SDL_RenderClear(ren);
            auto t0 = std::chrono::steady_clock::now();
            grid.render(ren, fm, 0.0f, 0.0f, 1.0f, 0.016f, false);
            auto t1 = std::chrono::steady_clock::now();
            SDL_RenderPresent(ren);
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            best = std::min(best, ms);
            total += ms;
        }
        // Best-of, like sink_bench: this contends with the GPU driver and the
        // compositor, and averaging folds their scheduling into the number.
        // Back-to-back averages here varied by 2x; the minimum is stable.
        double avg = total / kFrames;
        std::printf("%-26s %10.3f %10.0f %10.3f %12d\n",
                    s.label, best, 1000.0 / best, avg, cols * rows);
    }

    fm.cleanup();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
