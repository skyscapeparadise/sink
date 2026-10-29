// Headless benchmark for sink's parser and grid: the same ANSIParser and
// TerminalGrid the app feeds every byte through, driven through the same entry
// point (sync_output::parse, which holds synchronized updates the way the app
// does). No window, renderer or pty: this is the CPU-bound half of "how fast is
// this terminal", from bytes already in memory.
//
// Four groups of metrics:
//   parser.*       MB/s for each workload file, the way the app parses
//   parser_core.*  MB/s through ANSIParser::parse alone, for the four
//                  workloads bench/vte_bench also runs. This is the
//                  like-for-like figure against vte: vte has nothing
//                  corresponding to sync_output's scan for DECSET 2026.
//   findbar.*  ms for one find-bar keystroke at three scrollback depths
//   reflow.*   ms to rewrap the whole buffer for one resize step, which live
//              reflow does on every frame of a window drag
//
// Every metric carries a fingerprint of what it produced (bench/harness.hpp),
// and every parser run is checked for determinism: the grid after the first
// sample must hash the same as the grid after the last.
//
//   ./build/sink_bench bench/workloads            # table
//   ./build/sink_bench --json bench/workloads     # what bench/run.py reads
#include "ansi_parser.hpp"
#include "harness.hpp"
#include "sync_output.hpp"
#include "terminal_grid.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int kCols = 120, kRows = 50;  // what vte_bench uses; keep the two in step
constexpr size_t kChunk = 8192;         // a realistic pty read

struct Workload {
    const char* file;
    const char* id;
    const char* label;
};

const Workload kWorkloads[] = {
    {"plain_text.bin",    "plain_text",    "plain text (cat-like)"},
    {"colored_text.bin",  "colored_text",  "SGR-heavy (colorized ls)"},
    {"cursor_heavy.bin",  "cursor_heavy",  "cursor-heavy (TUI redraw)"},
    {"unicode_heavy.bin", "unicode_heavy", "UTF-8 heavy (emoji/CJK/box)"},
    {"sync_tui.bin",      "sync_tui",      "synchronized-output TUI"},
    {"graphemes.bin",     "graphemes",     "grapheme clusters"},
    {"hyperlinks.bin",    "hyperlinks",    "OSC 8 links + OSC 133 marks"},
    {"images.bin",        "images",        "sixel + kitty images"},
};

// Feeds `data` the way the app's frame loop does, minus the time budget: in
// pty-read-sized chunks, through sync_output::parse, carrying whatever it
// holds back (an open synchronized update) into the next chunk.
void feed(TerminalGrid& grid, ANSIParser& parser, sync_output::Hold& hold, const std::vector<char>& data) {
    std::vector<char> pending;
    const auto never = []() { return false; };
    for (size_t off = 0; off < data.size(); off += kChunk) {
        size_t len = std::min(kChunk, data.size() - off);
        pending.insert(pending.end(), data.begin() + static_cast<long>(off), data.begin() + static_cast<long>(off + len));
        size_t used = sync_output::parse(parser, grid, hold, pending.data(), pending.size(), 0, never);
        pending.erase(pending.begin(), pending.begin() + static_cast<long>(used));
    }
    // An update still open at the end is shown as is, as the app does after
    // kMaxUpdateMs: parse it with the clock past the deadline.
    while (!pending.empty()) {
        size_t used = sync_output::parse(parser, grid, hold, pending.data(), pending.size(),
                                         sync_output::kMaxUpdateMs * 10, never);
        if (used == 0) break;
        pending.erase(pending.begin(), pending.begin() + static_cast<long>(used));
    }
    // Replies (DSR, DA, kitty acks) are queued on the grid; the app drains
    // them every frame. Drain here too so the queue doesn't grow across runs.
    if (grid.has_pending_reply()) (void)grid.take_pending_reply();
}

struct Parsed {
    double seconds;
    std::string fingerprint;
    std::string screen;
};

// What bench/vte_bench does on its side: the parser alone, in 8KB chunks.
void feed_core(TerminalGrid& grid, ANSIParser& parser, const std::vector<char>& data) {
    for (size_t off = 0; off < data.size(); off += kChunk)
        parser.parse(grid, data.data() + off, std::min(kChunk, data.size() - off));
    if (grid.has_pending_reply()) (void)grid.take_pending_reply();
}

Parsed parse_once(const std::vector<char>& data, bool fingerprint, bool core = false) {
    TerminalGrid grid;
    grid.resize(kCols, kRows);
    ANSIParser parser;
    sync_output::Hold hold;
    auto t0 = bench::clock::now();
    if (core) feed_core(grid, parser, data);
    else feed(grid, parser, hold, data);
    Parsed p{bench::seconds_since(t0), {}, {}};
    if (fingerprint) {
        p.fingerprint = bench::grid_fingerprint(grid);
        p.screen = bench::screen_text_hash(grid);
    }
    return p;
}

} // namespace

int main(int argc, char** argv) {
    bench::Options opt = bench::parse_options(argc, argv);
    std::FILE* json_out = opt.json ? bench::json_stream() : stdout;
    const std::string dir = opt.positional.empty() ? "bench/workloads" : opt.positional[0];
    const int samples = opt.samples > 0 ? opt.samples : 20;
    const int warmup = opt.warmup >= 0 ? opt.warmup : 3;
    int failures = 0;

    bench::Report report("sink_bench");
    report.config("cols", kCols);
    report.config("rows", kRows);
    report.config("chunk_bytes", static_cast<double>(kChunk));
    report.config("samples", samples);
    report.config("warmup", warmup);

    for (int pass = 0; pass < 2; ++pass)
    for (size_t wi = 0; wi < sizeof(kWorkloads) / sizeof(kWorkloads[0]); ++wi) {
        const Workload& w = kWorkloads[wi];
        const bool core = pass == 1;
        if (core && wi >= 4) break;  // vte_bench runs only the first four
        const std::string id = std::string(core ? "parser_core." : "parser.") + w.id;
        if (!opt.wants(id)) continue;
        std::vector<char> data = bench::read_file(dir + "/" + w.file);
        const double mb = static_cast<double>(data.size()) / (1024.0 * 1024.0);

        bench::Metric m{id, std::string(core ? "core: " : "") + w.label, "MB/s", "higher", {}, {}, {}};
        Parsed first = parse_once(data, true, core);
        for (int i = 0; i < warmup; ++i) parse_once(data, false, core);
        for (int i = 0; i < samples; ++i) m.samples.push_back(mb / parse_once(data, false, core).seconds);
        Parsed last = parse_once(data, true, core);
        if (first.fingerprint != last.fingerprint) {
            std::fprintf(stderr, "%s: output differs between runs (%s vs %s) -- the parser is not deterministic\n",
                         id.c_str(), first.fingerprint.c_str(), last.fingerprint.c_str());
            ++failures;
        }
        m.fingerprint = first.fingerprint;
        m.add_info("screen_text", first.screen);
        m.add_info("bytes", static_cast<double>(data.size()));
        report.add(std::move(m));
    }

    // Find bar. set_search_query() rescans the whole buffer and the find bar
    // calls it on every keystroke, so this is the cost of one keypress, at the
    // scrollback depths people set. 200 columns, as render_bench and the
    // README's figures use.
    std::vector<char> corpus = bench::read_file(dir + "/plain_text.bin");
    for (int lines : {1000, 10000, 100000}) {
        const std::string id = "findbar." + std::to_string(lines);
        if (!opt.wants(id)) continue;
        TerminalGrid grid;
        grid.resize(200, 50);
        grid.set_max_scrollback(static_cast<size_t>(lines));
        ANSIParser parser;
        // Enough text to fill the scrollback to its cap, and then some.
        const size_t want = static_cast<size_t>(lines) * 90 + 400000;
        size_t fed = 0;
        while (fed < want) {
            size_t take = std::min(corpus.size(), want - fed);
            parser.parse(grid, corpus.data(), take);
            fed += take;
        }
        bench::Metric m{id, std::to_string(lines) + " lines, per keystroke", "ms", "lower", {}, {}, {}};
        int matches = -1;
        for (int i = 0; i < warmup + samples; ++i) {
            // Alternate the query so a future cache can't make this free.
            grid.set_search_query(i % 2 ? "0" : "1");
            auto t0 = bench::clock::now();
            grid.set_search_query("0");
            double ms = bench::seconds_since(t0) * 1000.0;
            if (i >= warmup) m.samples.push_back(ms);
            matches = grid.get_search_match_count();
        }
        m.fingerprint = "matches=" + std::to_string(matches);
        m.add_info("scrollback", static_cast<double>(grid.get_scrollback_size()));
        report.add(std::move(m));
    }

    // Live reflow. Dragging a window edge resizes the grid every frame, and
    // each resize rewraps every line in scrollback. One sample is one step of
    // a drag: 200 columns to 199 and back, timed separately and both counted.
    for (int lines : {1000, 10000}) {
        const std::string id = "reflow." + std::to_string(lines);
        if (!opt.wants(id)) continue;
        TerminalGrid grid;
        grid.resize(200, 50);
        grid.set_max_scrollback(static_cast<size_t>(lines));
        ANSIParser parser;
        size_t want = static_cast<size_t>(lines) * 90 + 400000, fed = 0;
        while (fed < want) {
            size_t take = std::min(corpus.size(), want - fed);
            parser.parse(grid, corpus.data(), take);
            fed += take;
        }
        bench::Metric m{id, std::to_string(lines) + " lines, per resize", "ms", "lower", {}, {}, {}};
        for (int i = 0; i < warmup + samples; ++i) {
            for (int cols : {199, 200}) {
                auto t0 = bench::clock::now();
                grid.resize(cols, 50);
                double ms = bench::seconds_since(t0) * 1000.0;
                if (i >= warmup) m.samples.push_back(ms);
            }
        }
        // Back at 200 columns after an even number of steps: what it holds
        // now is what a round trip of rewraps left, which should be stable.
        m.fingerprint = bench::grid_fingerprint(grid);
        m.add_info("scrollback", static_cast<double>(grid.get_scrollback_size()));
        report.add(std::move(m));
    }

    if (opt.json) report.write(json_out);
    else report.print_table(stdout);
    return failures ? 1 : 0;
}
