// Several panes parsing at once, each on its own pty reader thread, while this
// thread does to them what the main thread does. For ThreadSanitizer: built
// with -DSINK_TSAN=ON, it reports any state two threads touch without a lock
// between them. See docs/threading-plan.md, phase 5.
//
// What it catches that pane_lifetime doesn't: each pane's lock protects only
// that pane, so anything the parser shares *between* panes -- a static table
// built on first use, a global cache -- is touched by two reader threads with
// nothing ordering them. The inputs are chosen to reach the parser's corners:
// kitty images and OSC 52 clipboard writes (both base64), sixel, hyperlinks
// and prompt marks, synchronized updates, and the fuzzer's seed corpus.
//
// It starts with a cold start: before anything else in the process has parsed,
// four threads, each with its own grid and parser, are released at the same
// instant and parse the same stream. Shared state set up on first use is
// touched cold by several threads at once, with nothing between them. Later,
// the main thread's round-robin locking orders the readers' work through it,
// which can hide exactly this kind of race from ThreadSanitizer.
//
// Then the panes. Meanwhile this thread plays the main thread, under each pane's lock:
// reading the screen as rendering does, searching, selecting, scrolling,
// resizing, and collecting titles, clipboard text and images.
//
//   concurrent_parse_test <workloads dir> <fuzz seeds dir> [seconds]
#include "app_state.hpp"
#include "pane_output.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

std::string read_all(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string base64(const std::string& in) {
    static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) { out += a[(val >> bits) & 0x3F]; bits -= 6; }
    }
    if (bits > -6) out += a[((val << 8) >> (bits + 8)) & 0x3F];
    while (out.size() % 4) out += '=';
    return out;
}

// One input stream for one pane: the given files in order, OSC 52 writes,
// titles and resets mixed in, so every pane decodes base64 from the start.
std::string build_stream(const std::vector<std::string>& files, int seed) {
    std::mt19937 rng(seed);
    std::string s;
    for (int i = 0; i < 200; ++i) {
        s += "\x1b]52;c;" + base64("clipboard text " + std::to_string(rng())) + "\x07";
        s += "\x1b]2;title " + std::to_string(i) + "\x07";
    }
    for (const std::string& f : files) {
        s += read_all(f);
        s += "\x18\x1b\\\x1b[0m\r\n";  // end whatever the file left open, as termbench does
        if (rng() % 4 == 0) s += "\x1b" "c";  // a full reset now and then
    }
    return s;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: concurrent_parse_test <workloads dir> <fuzz seeds dir> [seconds]\n");
        return 2;
    }
    const std::string workloads = argv[1], seeds = argv[2];
    const int seconds = argc > 3 ? std::atoi(argv[3]) : 10;

    std::vector<std::string> seed_files;
    for (const char* sub : {"terminal", "images"}) {
        std::string d = seeds + "/" + sub;
        if (DIR* dir = opendir(d.c_str())) {
            while (dirent* e = readdir(dir)) {
                if (e->d_name[0] != '.') seed_files.push_back(d + "/" + e->d_name);
            }
            closedir(dir);
        }
    }
    if (seed_files.empty()) { std::fprintf(stderr, "no fuzz seeds under %s\n", seeds.c_str()); return 2; }

    // Each pane cats its own stream, then sleeps so it stays open.
    constexpr int kPanes = 3;
    std::vector<std::string> paths;
    for (int p = 0; p < kPanes; ++p) {
        std::vector<std::string> files = seed_files;
        std::shuffle(files.begin(), files.end(), std::mt19937(p));
        files.insert(files.begin() + p, workloads + "/images.bin");
        files.push_back(workloads + "/hyperlinks.bin");
        files.push_back(workloads + "/sync_tui.bin");
        std::string path = "/tmp/sink_concurrent_parse_" + std::to_string(getpid()) + "_" + std::to_string(p);
        std::ofstream(path, std::ios::binary) << build_stream(files, p);
        paths.push_back(path);
    }

    // Cold start: the first parsing this process does, on four threads at once.
    {
        const std::string stream = build_stream(seed_files, 99);
        std::atomic<bool> go{false};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&]() {
                TerminalGrid grid;
                grid.resize(100, 30);
                grid.set_cell_pixel_size(9, 18);
                ANSIParser parser;
                while (!go.load(std::memory_order_acquire)) {}
                for (size_t off = 0; off < stream.size(); off += 4096)
                    parser.parse(grid, stream.data() + off, std::min<size_t>(4096, stream.size() - off));
            });
        }
        go.store(true, std::memory_order_release);
        for (auto& t : threads) t.join();
    }

    PTYBridge::set_shell_integration_enabled(false);
    std::vector<std::unique_ptr<Pane>> panes;
    for (int p = 0; p < kPanes; ++p) {
        PTYBridge::ShellSettings sh;
        sh.command = "/bin/sh";
        sh.login = false;
        sh.args = {"-c", "stty -opost -echo; /bin/cat \"$0\"; exec /bin/sleep 600", paths[p]};
        PTYBridge::set_shell_settings(sh);
        auto pane = std::make_unique<Pane>();
        {
            auto g = pane->lock();
            g->resize(100, 30);
            g->set_cell_pixel_size(9, 18);
        }
        pane->typing_animation.store(p == 0);  // one pane with the typewriter decision live
        pane_output::attach_reader_parser(*pane);
        if (!pane->pty.spawn(100, 30)) { std::fprintf(stderr, "spawn failed\n"); return 1; }
        panes.push_back(std::move(pane));
    }

    std::mt19937 rng(7);
    long ops = 0, clipboard_seen = 0, titles_seen = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end) {
        for (auto& pane : panes) {
            auto g = pane->lock();
            // What rendering reads.
            for (int r = 0; r < g->get_rows(); r += 3)
                for (int c = 0; c < g->get_cols(); c += 7) (void)g->cell_string(g->get_cell_at(c, r));
            (void)g->images().placements().size();
            g->images().free_retired_textures();
            // What the frame collects.
            if (g->has_pending_title()) { (void)g->take_window_title(); ++titles_seen; }
            if (g->has_pending_clipboard_text()) { (void)g->take_clipboard_text(); ++clipboard_seen; }
            // What a user does.
            switch (rng() % 8) {
                case 0: g->set_search_query(rng() % 2 ? "e" : "line"); break;
                case 1: g->start_selection(1, 1); g->update_selection(20, 5); (void)g->get_selected_text(); g->clear_selection(); break;
                case 2: g->scroll_view(static_cast<int>(rng() % 50)); g->reset_scroll(); break;
                case 3: g->resize(90 + static_cast<int>(rng() % 20), 25 + static_cast<int>(rng() % 10)); break;
                default: break;
            }
            // Typewriter tick, as the frame runs it.
            if (!pane->animation_buffer.empty()) {
                g.parse(pane->animation_buffer.data(), pane->animation_buffer.size());
                pane->animation_buffer.clear();
            }
            ++ops;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    panes.clear();  // ~Pane stops each reader
    for (const std::string& p : paths) std::remove(p.c_str());

    if (clipboard_seen == 0 || titles_seen == 0) {
        std::fprintf(stderr, "the readers never delivered clipboard writes or titles (%ld, %ld): nothing was tested\n",
                     clipboard_seen, titles_seen);
        return 1;
    }
    std::printf("%d panes parsed concurrently for %ds; %ld main-thread operations, %ld clipboard writes and %ld titles collected\n",
                kPanes, seconds, ops, clipboard_seen, titles_seen);
    return 0;
}
