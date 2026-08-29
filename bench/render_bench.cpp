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
    SDL_Renderer* ren = SDL_CreateRenderer(win, nullptr);
    if (!ren) { std::fprintf(stderr, "CreateRenderer: %s\n", SDL_GetError()); return 1; }
    SDL_SetRenderVSync(ren, 0);
    std::printf("renderer: %s\n", SDL_GetRendererName(ren));

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
    };

    std::printf("\n%-26s %10s %10s %12s\n", "scene", "ms/frame", "fps", "cells/frame");
    std::printf("%-26s %10s %10s %12s\n", "-----", "--------", "---", "-----------");

    for (const Scene& s : scenes) {
        TerminalGrid grid;
        grid.resize(cols, rows);
        if (s.workload) {
            std::vector<char> data = read_file(dir + "/" + s.workload);
            if (data.empty()) { std::fprintf(stderr, "missing %s\n", s.workload); continue; }
            // Enough to fill the visible grid several times over, so what's on
            // screen is representative rather than a mostly-blank buffer.
            ANSIParser p;
            size_t take = std::min<size_t>(data.size(), 400000);
            p.parse(grid, data.data(), take);
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
        double avg = total / kFrames;
        std::printf("%-26s %10.3f %10.0f %12d\n", s.label, avg, 1000.0 / avg, cols * rows);
        (void)best;
    }

    fm.cleanup();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
