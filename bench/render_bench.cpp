// Frame-time benchmark for sink's render path, composed the way main.cpp
// composes a frame.
//
// Each scene is a grid's contents plus the window around it: background kind
// (color, gradient, gel, shader, picture, video), the CRT effect, HDR text,
// split panes. A frame is drawn in the same order SDL_AppIterate draws one --
// CRT scene begin, clear, background, panes (clip, HDR boost, render, dim),
// dividers, CRT composite, present -- using the same classes. If that order
// changes in main.cpp, change it here.
//
// Two numbers per scene:
//   render.<scene>.cpu    CPU time to build the frame (everything up to
//                         present). Decides the main thread's headroom.
//   render.<scene>.frame  Time to finished pixels: the frame is built,
//                         submitted, and waited for, through a 1x1 readback
//                         (SDL submits the command buffer and waits on its
//                         fence). CPU and GPU in series, so an upper bound on
//                         what the frame costs. A shader background can be
//                         nearly free on the CPU and expensive here; the old
//                         render_bench measured only the CPU and could not
//                         have seen it.
//
// Frames are never presented while timing. Presenting waits for the display
// even on a hidden window with vsync off -- the first version of this
// measured 8.33ms for every scene, which was this machine's 120Hz refresh,
// not sink. The readback's own cost is in every .frame number; the blank
// scene shows how much it is.
// Each sample is the mean over one batch of frames, not one frame: single
// frames are too short to time against the driver's scheduling.
//
// The renderer is the app's: SDL's "gpu" backend, SDR unless the scene has
// HDR text, which gets a linear-colorspace renderer as main.cpp makes one.
// The previous version used SDL's default backend (Metal 2D), which sink does
// not draw with.
//
// Evidence that the frame was drawn: after timing, each scene is drawn once
// more at a fixed time and read back. The pixels' hash goes in the results
// (bench/run.py warns if it changes), the frame is saved as a PNG if
// --snapshots DIR is given, and a scene whose readback is a single flat
// color fails the run -- a renderer that silently dropped the work would
// otherwise look very fast.
//
//   ./build/render_bench [--json] [--snapshots DIR] [--samples N] [--only S] [workloads]
#include "ansi_parser.hpp"
#include "background_shader.hpp"
#include "crt_shader.hpp"
#include "font_manager.hpp"
#include "harness.hpp"
#include "hue_shift.hpp"
#include "shader_background.hpp"
#include "terminal_grid.hpp"
#include "video_engine.hpp"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr int kCols = 200, kRows = 50;  // one pane's grid, as in earlier results
constexpr float kFontPt = 15.0f;        // sink's default font_size
constexpr float kPaddingPt = 2.0f;      // sink's default padding
constexpr float kHdrBoost = 1.6f;       // kHdrConsoleTextBoost in main.cpp
constexpr float kDt = 1.0f / 60.0f;     // what a 60Hz frame advances animations by
constexpr int kBatch = 20;              // frames per sample, at most
constexpr double kBatchMs = 60.0;       // ...and fewer when frames are slow

enum class Backdrop { Color, Gradient, Gel, Shader, Image, Video };

struct Scene {
    const char* id;
    const char* label;
    const char* content;  // workload file, "@painted", or nullptr for blank
    Backdrop backdrop = Backdrop::Color;
    bool crt = false;
    bool hdr = false;
    int panes = 1;         // 1, or 4 in a 2x2 split
    int scroll_back = 0;
    const char* search = nullptr;
    bool deterministic = true;  // false: content depends on timing (video)
};

const Scene kScenes[] = {
    // Text over the plain theme color: the grid render on its own.
    {"blank",       "blank grid",              nullptr},
    {"plain",       "plain text",              "plain_text.bin"},
    {"sgr",         "SGR-heavy",               "colored_text.bin"},
    {"unicode",     "UTF-8 heavy",             "unicode_heavy.bin"},
    {"graphemes",   "grapheme clusters",       "graphemes.bin"},
    {"cjk",         "distinct CJK screenful",  "distinct_cjk.bin"},
    {"painted",     "painted backgrounds",     "@painted"},
    {"images",      "inline images",           "images.bin"},
    {"scrollback",  "scrollback (deep)",       "plain_text.bin", Backdrop::Color, false, false, 1, 4000},
    {"search",      "scrollback + search",     "plain_text.bin", Backdrop::Color, false, false, 1, 4000, "e"},
    {"panes4",      "four panes",              "colored_text.bin", Backdrop::Color, false, false, 4},
    // Backgrounds, each with plain text over it.
    {"gradient",    "gradient background",     "plain_text.bin", Backdrop::Gradient},
    {"gel",         "gel background",          "plain_text.bin", Backdrop::Gel},
    {"shader",      "shader background",       "plain_text.bin", Backdrop::Shader},
    {"picture",     "4K picture background",   "plain_text.bin", Backdrop::Image},
    {"video",       "video background",        "plain_text.bin", Backdrop::Video, false, false, 1, 0, nullptr, false},
    // Effects.
    {"crt",         "CRT effect",              "plain_text.bin", Backdrop::Color, true},
    {"hdr",         "HDR text",                "plain_text.bin", Backdrop::Color, false, true},
    {"everything",  "gel + CRT + HDR + 4 panes", "colored_text.bin", Backdrop::Gel, true, true, 4},
};

// sink's default theme colors are not the point here; any fixed values do.
constexpr SDL_FColor kThemeBg = {0.06f, 0.06f, 0.08f, 1.0f};

// One renderer and everything that hangs off it: an SDR one and, for HDR
// scenes, a linear one, as main.cpp creates per window.
struct Gfx {
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    FontManager fm;
    HueShiftEffect hue;
    CrtShaderEffect crt;
    BackgroundShader backdrop;
    ShaderBackground user_shader;
    VideoEngine video;
    SDL_Texture* picture = nullptr;
    float density = 1.0f;
    int out_w = 0, out_h = 0;
    bool linear = false;
    std::string error;

    bool open(bool hdr, const std::string& font, const std::string& shader_path, const std::string& video_path) {
        linear = hdr;
        // Size the window to hold 200x50 cells at the default font. The cell
        // size is only known once a font is loaded, which needs a renderer,
        // and a hidden window's backbuffer never changes size after the
        // renderer is made (SDL resizes it on a swapchain mismatch, and a
        // hidden window has no swapchain) -- so measure first, then make the
        // real window at the right size.
        int w_pt = 1600, h_pt = 900;
        for (int pass = 0; pass < 2; ++pass) {
            win = SDL_CreateWindow("sink render bench", w_pt, h_pt, SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
            if (!win) { error = SDL_GetError(); return false; }
            if (hdr) {
                SDL_PropertiesID props = SDL_CreateProperties();
                SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, win);
                SDL_SetStringProperty(props, SDL_PROP_RENDERER_CREATE_NAME_STRING, SDL_GPU_RENDERER);
                SDL_SetNumberProperty(props, SDL_PROP_RENDERER_CREATE_OUTPUT_COLORSPACE_NUMBER, SDL_COLORSPACE_SRGB_LINEAR);
                ren = SDL_CreateRendererWithProperties(props);
                SDL_DestroyProperties(props);
            } else {
                ren = SDL_CreateRenderer(win, SDL_GPU_RENDERER);
            }
            if (!ren) { error = SDL_GetError(); return false; }
            SDL_SetRenderVSync(ren, 0);
            density = SDL_GetWindowPixelDensity(win);
            if (density <= 0.0f) density = 1.0f;
            if (!fm.initialize() || !fm.load_font(ren, font, kFontPt * density)) {
                error = "font load failed: " + font;
                return false;
            }
            // main.cpp's load_window_font() cell size, in points.
            const float cell_w = fm.get_cell_width() / density + 1.0f;
            const float cell_h = fm.get_cell_height() / density - 0.8f;
            const int want_w = static_cast<int>(kCols * cell_w + 2 * kPaddingPt + 0.999f);
            const int want_h = static_cast<int>(kRows * cell_h + 2 * kPaddingPt + 0.999f);
            if (pass == 0) {
                fm.cleanup();
                SDL_DestroyRenderer(ren);
                SDL_DestroyWindow(win);
                ren = nullptr; win = nullptr;
                w_pt = want_w; h_pt = want_h;
            }
        }
        SDL_GetRenderOutputSize(ren, &out_w, &out_h);
        hue.init(ren, linear);
        crt.init(ren, linear);
        backdrop.init(ren, linear);
        if (!user_shader.load(ren, shader_path, linear)) {
            error = "shader: " + user_shader.error();
            return false;
        }
        // A 4K picture, made here rather than decoded from a file so the
        // benchmark needs no image asset: drawing it is the cost being
        // measured, and that doesn't depend on what the pixels are.
        SDL_Surface* s = SDL_CreateSurface(3840, 2160, SDL_PIXELFORMAT_RGBA32);
        for (int y = 0; y < s->h; ++y) {
            Uint8* row = static_cast<Uint8*>(s->pixels) + y * s->pitch;
            for (int x = 0; x < s->w; ++x) {
                row[x * 4 + 0] = static_cast<Uint8>(x * 255 / s->w);
                row[x * 4 + 1] = static_cast<Uint8>(y * 255 / s->h);
                row[x * 4 + 2] = static_cast<Uint8>((x ^ y) & 0xff);
                row[x * 4 + 3] = 255;
            }
        }
        picture = SDL_CreateTextureFromSurface(ren, s);
        SDL_DestroySurface(s);
        if (!video_path.empty() && video.open_video(ren, video_path)) video.start();
        return true;
    }

    void close() {
        video.stop();
        video.close_video();
        if (picture) SDL_DestroyTexture(picture);
        user_shader.cleanup();
        backdrop.cleanup();
        crt.cleanup();
        hue.cleanup();
        fm.cleanup();
        if (ren) SDL_DestroyRenderer(ren);
        if (win) SDL_DestroyWindow(win);
    }
};

struct Pane {
    std::unique_ptr<TerminalGrid> grid;
    SDL_FRect rect;  // pixels
};

std::string painted_screen(int cols, int rows) {
    // What a full-screen program (vim, htop, tmux) draws: every row on a
    // painted background, a few accent cells, a reversed status line.
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
    return screen;
}

std::vector<Pane> make_panes(const Scene& s, Gfx& g, const std::string& dir) {
    std::vector<Pane> panes;
    const int split = s.panes == 4 ? 2 : 1;
    const float pw = static_cast<float>(g.out_w) / split, ph = static_cast<float>(g.out_h) / split;
    // Pixel size of a cell, which the image protocols place by.
    const int cell_px_w = static_cast<int>(g.fm.get_cell_width() + g.density);
    const int cell_px_h = static_cast<int>(g.fm.get_cell_height() - 0.8f * g.density);
    std::vector<char> data;
    if (s.content && s.content[0] != '@') data = bench::read_file(dir + "/" + s.content);
    for (int i = 0; i < s.panes; ++i) {
        Pane p;
        p.grid = std::make_unique<TerminalGrid>();
        p.rect = {(i % split) * pw, (i / split) * ph, pw, ph};
        p.grid->resize(kCols / split, kRows / split);
        p.grid->set_cell_pixel_size(cell_px_w, cell_px_h);
        p.grid->set_enable_ligatures(true);
        ANSIParser parser;
        if (s.content && std::strcmp(s.content, "@painted") == 0) {
            std::string screen = painted_screen(kCols / split, kRows / split);
            parser.parse(*p.grid, screen.data(), screen.size());
        } else if (!data.empty()) {
            // Enough to fill the screen several times over; a scrollback scene
            // feeds more, so there is history that far back.
            size_t take = std::min<size_t>(data.size(), 400000);
            int passes = s.scroll_back > 0 ? 40 : 1;
            for (int k = 0; k < passes; ++k) parser.parse(*p.grid, data.data(), take);
        }
        if (s.scroll_back > 0) p.grid->scroll_view(s.scroll_back);
        if (s.search) {
            p.grid->set_search_query(s.search);
            p.grid->set_search_active(true);
        }
        if (p.grid->has_pending_reply()) (void)p.grid->take_pending_reply();
        panes.push_back(std::move(p));
    }
    return panes;
}

// One frame, in main.cpp's order. `t` is the animation clock.
void draw_frame(Gfx& g, const Scene& s, std::vector<Pane>& panes, float t) {
    SDL_Renderer* ren = g.ren;
    SDL_SetRenderViewport(ren, nullptr);
    const bool use_crt = s.crt && g.crt.is_ready();
    if (use_crt) g.crt.begin_scene(ren);

    const float exposure = 1.0f, hue_degrees = 0.0f;
    if (s.backdrop == Backdrop::Gradient || s.backdrop == Backdrop::Gel) SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    else SDL_SetRenderDrawColorFloat(ren, kThemeBg.r, kThemeBg.g, kThemeBg.b, 1.0f);
    SDL_RenderClear(ren);

    if (s.backdrop == Backdrop::Gradient || s.backdrop == Backdrop::Gel) {
        BackgroundShader::Params p;
        const bool gel = s.backdrop == Backdrop::Gel;
        p.mode = gel ? BackgroundShader::Mode::Gel : BackgroundShader::Mode::Gradient;
        // sink's own gel colors (main.cpp), and a two-stop gradient.
        const SDL_FColor gel_colors[5] = {
            {0.012f, 0.006f, 0.020f, 1}, {0.110f, 0.035f, 0.170f, 1}, {0.300f, 0.075f, 0.380f, 1},
            {0.560f, 0.200f, 0.640f, 1}, {0.820f, 0.560f, 0.900f, 1},
        };
        if (gel) for (int i = 0; i < 5; ++i) p.colors[i] = gel_colors[i];
        else { p.colors[0] = kThemeBg; p.colors[1] = {0.1f, 0.1f, 0.3f, 1}; p.count = 2; }
        p.time = t;
        p.logical_w = g.out_w / g.density;
        p.logical_h = g.out_h / g.density;
        g.backdrop.draw(ren, p);
    } else if (s.backdrop == Backdrop::Image && g.picture) {
        g.hue.draw(ren, g.picture, hue_degrees, exposure, s.crt, nullptr, nullptr);
    } else if (s.backdrop == Backdrop::Shader) {
        ShaderBackground::Frame f;
        f.time = t;
        f.time_delta = kDt;
        f.frame = static_cast<int>(t / kDt);
        g.user_shader.draw(ren, f);
    } else if (s.backdrop == Backdrop::Video) {
        g.video.update_frame(ren, kDt);
        if (SDL_Texture* tex = g.video.get_texture()) g.hue.draw(ren, tex, hue_degrees, exposure, s.crt, nullptr, nullptr);
    }

    for (size_t i = 0; i < panes.size(); ++i) {
        Pane& p = panes[i];
        SDL_Rect clip = {static_cast<int>(p.rect.x), static_cast<int>(p.rect.y),
                         static_cast<int>(p.rect.w), static_cast<int>(p.rect.h)};
        SDL_SetRenderClipRect(ren, &clip);
        if (s.hdr) SDL_SetRenderColorScale(ren, kHdrBoost);
        p.grid->render(ren, g.fm, p.rect.x + kPaddingPt * g.density, p.rect.y + kPaddingPt * g.density,
                       g.density, kDt, false);
        if (s.hdr) SDL_SetRenderColorScale(ren, 1.0f);
        if (panes.size() > 1 && i != 0) {  // pane 0 has focus; the rest are dimmed
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 64);
            SDL_RenderFillRect(ren, &p.rect);
        }
        SDL_SetRenderClipRect(ren, nullptr);
    }
    if (panes.size() > 1) {  // dividers
        SDL_SetRenderDrawColor(ren, 80, 80, 90, 255);
        SDL_FRect v = {g.out_w / 2.0f - g.density / 2, 0, g.density, static_cast<float>(g.out_h)};
        SDL_FRect h = {0, g.out_h / 2.0f - g.density / 2, static_cast<float>(g.out_w), g.density};
        SDL_RenderFillRect(ren, &v);
        SDL_RenderFillRect(ren, &h);
    }
    if (use_crt) g.crt.end_scene(ren);
}

// Waits for the GPU to finish everything submitted so far. A readback has to.
void gpu_sync(SDL_Renderer* ren) {
    SDL_Rect one = {0, 0, 1, 1};
    if (SDL_Surface* s = SDL_RenderReadPixels(ren, &one)) SDL_DestroySurface(s);
}

struct Readback {
    std::string hash;
    int distinct = 0;  // distinct colors in a sample of pixels, capped
    bool ok = false;
};

Readback read_frame(SDL_Renderer* ren, const std::string& png_path) {
    Readback rb;
    SDL_Surface* raw = SDL_RenderReadPixels(ren, nullptr);
    if (!raw) return rb;
    SDL_Surface* s = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(raw);
    if (!s) return rb;
    bench::Hash h;
    std::set<uint32_t> colors;
    for (int y = 0; y < s->h; ++y) {
        const Uint8* row = static_cast<const Uint8*>(s->pixels) + y * s->pitch;
        h.bytes(row, static_cast<size_t>(s->w) * 4);
        if (colors.size() < 64 && y % 7 == 0) {
            for (int x = 0; x < s->w; x += 13) {
                uint32_t c;
                std::memcpy(&c, row + x * 4, 4);
                colors.insert(c);
            }
        }
    }
    rb.hash = h.hex();
    rb.distinct = static_cast<int>(colors.size());
    rb.ok = rb.distinct > 1;
    if (!png_path.empty()) IMG_SavePNG(s, png_path.c_str());
    SDL_DestroySurface(s);
    return rb;
}

} // namespace

int main(int argc, char** argv) {
    // --snapshots DIR is ours; strip it before the common flags.
    std::string snapshots;
    std::vector<char*> rest;
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--snapshots") == 0 && i + 1 < argc) snapshots = argv[++i];
        else rest.push_back(argv[i]);
    }
    bench::Options opt = bench::parse_options(static_cast<int>(rest.size()), rest.data());
    std::FILE* json_out = opt.json ? bench::json_stream() : stdout;
    const std::string dir = opt.positional.size() > 0 ? opt.positional[0] : "bench/workloads";
    const std::string font = opt.positional.size() > 1 ? opt.positional[1] : "fonts/MonaspaceNeon-Regular.otf";
    const std::string shader = "bench/shaders/bench.frag";
    const std::string video = "backgrounds/sinkpool.mp4";
    const int samples = opt.samples > 0 ? opt.samples : 15;
    const int warmup = opt.warmup >= 0 ? opt.warmup : 2;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 2;
    }

    bench::Report report("render_bench");
    report.config("cols", kCols);
    report.config("rows", kRows);
    report.config("font_pt", kFontPt);
    report.config("max_batch_frames", kBatch);
    report.config("samples", samples);
    report.config("renderer", SDL_GPU_RENDERER);

    int failures = 0;
    for (int hdr = 0; hdr < 2; ++hdr) {
        std::vector<const Scene*> todo;
        for (const Scene& s : kScenes)
            if (s.hdr == (hdr == 1) && (opt.wants(std::string("render.") + s.id + ".cpu") ||
                                         opt.wants(std::string("render.") + s.id + ".frame")))
                todo.push_back(&s);
        if (todo.empty()) continue;

        Gfx g;
        if (!g.open(hdr == 1, font, shader, video)) {
            std::fprintf(stderr, "renderer setup failed: %s\n", g.error.c_str());
            return 2;
        }
        if (hdr == 0) {
            report.config("output_px", std::to_string(g.out_w) + "x" + std::to_string(g.out_h));
            report.config("pixel_density", g.density);
            report.config("crt_shader", g.crt.is_ready() ? "yes" : "no (fallback overlay)");
        }

        for (const Scene* sp : todo) {
            const Scene& s = *sp;
            std::vector<Pane> panes = make_panes(s, g, dir);
            float t = 0.0f;
            // Warm the glyph atlas, the shaders and the driver.
            auto w0 = bench::clock::now();
            for (int i = 0; i < 30; ++i) {
                draw_frame(g, s, panes, t += kDt);
                gpu_sync(g.ren);
            }
            // A scene at 80ms a frame would take half a minute at 20 frames a
            // sample; batch fewer so every scene costs about the same.
            const double warm_ms = bench::seconds_since(w0) * 1000.0 / 30;
            const int batch = std::clamp(static_cast<int>(kBatchMs / std::max(warm_ms, 0.001)), 1, kBatch);

            bench::Metric cpu{std::string("render.") + s.id + ".cpu", std::string(s.label) + " (cpu)", "ms", "lower", {}, {}, {}};
            bench::Metric frame{std::string("render.") + s.id + ".frame", std::string(s.label) + " (frame)", "ms", "lower", {}, {}, {}};
            for (int b = 0; b < warmup + samples; ++b) {
                double cpu_s = 0.0;
                auto t0 = bench::clock::now();
                for (int i = 0; i < batch; ++i) {
                    auto f0 = bench::clock::now();
                    draw_frame(g, s, panes, t += kDt);
                    cpu_s += bench::seconds_since(f0);
                    gpu_sync(g.ren);
                }
                const double total_s = bench::seconds_since(t0);
                if (b < warmup) continue;
                cpu.samples.push_back(cpu_s * 1000.0 / batch);
                frame.samples.push_back(total_s * 1000.0 / batch);
            }

            // The evidence frame, at a fixed time so it's reproducible.
            draw_frame(g, s, panes, 1.0f);
            std::string png = snapshots.empty() ? "" : snapshots + "/" + s.id + ".png";
            Readback rb = read_frame(g.ren, png);
            if (!rb.ok) {
                std::fprintf(stderr, "render.%s: the frame read back as one flat color -- nothing was drawn\n", s.id);
                ++failures;
            }
            // The grid's state is the deterministic part of a scene; the
            // pixels are evidence, checked more loosely (GPU output can move
            // with a driver update without anything in sink changing).
            std::string fp;
            for (Pane& p : panes) fp += bench::grid_fingerprint(*p.grid).substr(0, 8);
            for (bench::Metric* m : {&cpu, &frame}) {
                if (!opt.wants(m->id)) continue;
                m->fingerprint = fp;
                m->add_info("pixels", s.deterministic ? rb.hash : std::string("nondeterministic"));
                m->add_info("distinct_colors", rb.distinct);
                m->add_info("output_px", std::to_string(g.out_w) + "x" + std::to_string(g.out_h));
                m->add_info("batch_frames", batch);
                report.add(std::move(*m));
            }
        }
        g.close();
    }

    SDL_Quit();
    if (opt.json) report.write(json_out);
    else report.print_table(stdout);
    return failures ? 1 : 0;
}
