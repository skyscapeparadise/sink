// Shared plumbing for sink's benchmarks: raw sample collection, JSON output,
// and output fingerprints.
//
// The benchmarks report raw samples, not summaries. bench/run.py does the
// statistics, so how a number was derived is the same everywhere and can be
// redone from the saved results file by anyone who doubts it.
//
// A fingerprint is a hash of what a benchmark *produced* -- the terminal's
// cells, cursor and scrollback, or the pixels of a frame. Every run checks its
// fingerprint against the blessed one in bench/expected.json, so a change that
// makes a benchmark faster by making it do less (or different) work fails the
// run instead of being reported as a speedup. That has happened here before:
// see "A warning about precision" in bench/README.md.
#pragma once

#include "terminal_grid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace bench {

using clock = std::chrono::steady_clock;

inline double seconds_since(clock::time_point t0) {
    return std::chrono::duration<double>(clock::now() - t0).count();
}

inline std::vector<char> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "could not open %s (run bench/gen_workloads.py)\n", path.c_str());
        std::exit(2);
    }
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// FNV-1a, 64-bit. Not cryptographic; it only has to notice that output changed.
struct Hash {
    uint64_t h = 0xcbf29ce484222325ull;
    void byte(uint8_t b) { h = (h ^ b) * 0x100000001b3ull; }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) byte(static_cast<uint8_t>(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) byte(static_cast<uint8_t>(v >> (8 * i))); }
    void bytes(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) byte(b[i]);
    }
    std::string hex() const {
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
        return buf;
    }
};

inline void hash_cell(Hash& h, const TerminalGrid& grid, const Cell& c) {
    // A cluster reference is an index into a table, so hash what it refers
    // to rather than the index.
    std::u32string text = grid.cell_string(c);
    h.u32(static_cast<uint32_t>(text.size()));
    for (char32_t cp : text) h.u32(static_cast<uint32_t>(cp));
    h.bytes(&c.fg, sizeof(c.fg));
    h.bytes(&c.bg, sizeof(c.bg));
    h.byte(c.attrs);
    h.u32(c.hyperlink_id);
}

// Everything a parse left behind that a user could see: every scrollback line,
// the screen, the cursor. Leaves the grid's scroll position where it found it
// (at the bottom).
inline std::string grid_fingerprint(TerminalGrid& grid) {
    Hash h;
    const int cols = grid.get_cols(), rows = grid.get_rows();
    h.u32(static_cast<uint32_t>(cols));
    h.u32(static_cast<uint32_t>(rows));
    const int history = static_cast<int>(grid.get_scrollback_size());
    h.u32(static_cast<uint32_t>(history));
    // Offset k puts history line (size - k) on screen row 0, so stepping k
    // down from the top visits each history line exactly once.
    for (int k = history; k >= 1; --k) {
        grid.reset_scroll();
        grid.scroll_view(k);
        for (int c = 0; c < cols; ++c) hash_cell(h, grid, grid.get_cell_at(c, 0));
    }
    grid.reset_scroll();
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) hash_cell(h, grid, grid.get_cell_at(c, r));
    h.u32(static_cast<uint32_t>(grid.get_cursor_col()));
    h.u32(static_cast<uint32_t>(grid.get_cursor_row()));
    return h.hex();
}

// The visible screen as plain text, and the cursor. Deliberately weaker than
// grid_fingerprint: it is the part of the result that a different terminal
// implementation (bench/vte_bench) can produce too, so the two can be checked
// for doing the same work. Blank cells and the trailing half of a wide
// character both count as a space; a cluster counts as its first codepoint.
// vte_bench computes exactly this; keep the two in step.
inline std::string screen_text_hash(const TerminalGrid& grid) {
    Hash h;
    for (int r = 0; r < grid.get_rows(); ++r) {
        for (int c = 0; c < grid.get_cols(); ++c) {
            char32_t cp = grid.cell_base(grid.get_cell_at(c, r));
            h.u32(cp == 0 ? 32u : static_cast<uint32_t>(cp));
        }
    }
    h.u32(static_cast<uint32_t>(grid.get_cursor_col()));
    h.u32(static_cast<uint32_t>(grid.get_cursor_row()));
    return h.hex();
}

// --- JSON output -----------------------------------------------------------
//
// Small and write-only. One object per metric:
//   { "id": "parser.plain_text", "unit": "MB/s", "better": "higher",
//     "samples": [...], "fingerprint": "...", "info": {...} }

inline std::string json_escape(const std::string& s) {
    std::string out;
    for (char ch : s) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
    return out;
}

struct Metric {
    std::string id;
    std::string label;
    std::string unit;
    std::string better;  // "higher" or "lower"
    std::vector<double> samples;
    std::string fingerprint;  // empty: this metric has no output to check
    std::vector<std::pair<std::string, std::string>> info;  // values already JSON-encoded

    void add_info(const std::string& k, const std::string& v) { info.push_back({k, "\"" + json_escape(v) + "\""}); }
    void add_info(const std::string& k, double v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.17g", v);
        info.push_back({k, buf});
    }
};

class Report {
public:
    explicit Report(std::string bench) : bench_(std::move(bench)) {}
    Metric& add(Metric m) { metrics_.push_back(std::move(m)); return metrics_.back(); }
    void config(const std::string& k, const std::string& v) { config_.push_back({k, "\"" + json_escape(v) + "\""}); }
    void config(const std::string& k, double v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.17g", v);
        config_.push_back({k, buf});
    }

    void write(std::FILE* out) const {
        std::fprintf(out, "{\n  \"bench\": \"%s\",\n  \"config\": {", json_escape(bench_).c_str());
        for (size_t i = 0; i < config_.size(); ++i)
            std::fprintf(out, "%s\"%s\": %s", i ? ", " : "", json_escape(config_[i].first).c_str(), config_[i].second.c_str());
        std::fprintf(out, "},\n  \"metrics\": [\n");
        for (size_t i = 0; i < metrics_.size(); ++i) {
            const Metric& m = metrics_[i];
            std::fprintf(out, "    {\"id\": \"%s\", \"label\": \"%s\", \"unit\": \"%s\", \"better\": \"%s\",\n",
                         json_escape(m.id).c_str(), json_escape(m.label).c_str(),
                         json_escape(m.unit).c_str(), json_escape(m.better).c_str());
            std::fprintf(out, "     \"fingerprint\": %s%s%s,\n", m.fingerprint.empty() ? "" : "\"",
                         m.fingerprint.empty() ? "null" : m.fingerprint.c_str(), m.fingerprint.empty() ? "" : "\"");
            std::fprintf(out, "     \"info\": {");
            for (size_t j = 0; j < m.info.size(); ++j)
                std::fprintf(out, "%s\"%s\": %s", j ? ", " : "", json_escape(m.info[j].first).c_str(), m.info[j].second.c_str());
            std::fprintf(out, "},\n     \"samples\": [");
            for (size_t j = 0; j < m.samples.size(); ++j) std::fprintf(out, "%s%.9g", j ? ", " : "", m.samples[j]);
            std::fprintf(out, "]}%s\n", i + 1 < metrics_.size() ? "," : "");
        }
        std::fprintf(out, "  ]\n}\n");
    }

    // A readable table for someone running the binary by hand. Median, not
    // best: see bench/README.md for why the minimum alone misleads.
    void print_table(std::FILE* out) const {
        std::fprintf(out, "%-38s %12s %12s %12s  %s\n", "metric", "median", "min", "max", "unit");
        for (const Metric& m : metrics_) {
            std::vector<double> s = m.samples;
            if (s.empty()) continue;
            std::sort(s.begin(), s.end());
            double med = s.size() % 2 ? s[s.size() / 2] : 0.5 * (s[s.size() / 2 - 1] + s[s.size() / 2]);
            std::fprintf(out, "%-38s %12.3f %12.3f %12.3f  %s\n", m.label.c_str(), med, s.front(), s.back(), m.unit.c_str());
        }
    }

private:
    std::string bench_;
    std::vector<std::pair<std::string, std::string>> config_;
    std::vector<Metric> metrics_;
};

// Common flags: --json (write JSON to stdout instead of a table), --samples N,
// --warmup N, --only SUBSTRING (run metrics whose id contains it).
struct Options {
    bool json = false;
    int samples = 0;
    int warmup = -1;
    std::string only;
    std::vector<std::string> positional;

    bool wants(const std::string& id) const { return only.empty() || id.find(only) != std::string::npos; }
};

// With --json, stdout carries the JSON and nothing else. Library code the
// benchmarks call (VideoEngine, for one) prints to stdout, so this moves fd 1
// to stderr and returns a stream on the original stdout for the report.
inline std::FILE* json_stream() {
    int fd = dup(1);
    dup2(2, 1);
    return fdopen(fd, "w");
}

inline Options parse_options(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--json") o.json = true;
        else if (a == "--samples" && i + 1 < argc) o.samples = std::atoi(argv[++i]);
        else if (a == "--warmup" && i + 1 < argc) o.warmup = std::atoi(argv[++i]);
        else if (a == "--only" && i + 1 < argc) o.only = argv[++i];
        else o.positional.push_back(a);
    }
    return o;
}

} // namespace bench
