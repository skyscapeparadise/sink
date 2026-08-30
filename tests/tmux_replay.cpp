// Replays a captured byte stream through sink's ANSIParser + TerminalGrid and
// prints the resulting screen, one row per line with trailing blanks trimmed.
//
// That output format matches `tmux capture-pane -p`, which is what makes the
// comparison in tests/tmux_compat.py possible: tmux tells us what it believes
// it drew, this says what sink actually rendered from the escape sequences it
// sent, and any disagreement is a real interpretation difference.
//
//   tmux_replay <cols> <rows> < stream.bin
#include "ansi_parser.hpp"
#include "terminal_grid.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <iostream>

static std::string utf8_of(char32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

int main(int argc, char** argv) {
    int cols = (argc > 1) ? std::atoi(argv[1]) : 80;
    int rows = (argc > 2) ? std::atoi(argv[2]) : 24;

    std::vector<char> data((std::istreambuf_iterator<char>(std::cin)),
                            std::istreambuf_iterator<char>());

    TerminalGrid grid;
    grid.resize(cols, rows);
    ANSIParser parser;

    // Fed in chunks, as a real PTY read would arrive, so any state the parser
    // has to carry across calls is exercised rather than bypassed.
    constexpr size_t kChunk = 4096;
    for (size_t off = 0; off < data.size(); off += kChunk) {
        size_t n = std::min(kChunk, data.size() - off);
        parser.parse(grid, data.data() + off, n);
    }

    for (int r = 0; r < rows; ++r) {
        std::string line;
        for (int c = 0; c < cols; ++c) {
            Cell cell = grid.get_cell_at(c, r);
            if (cell.attrs & ATTR_WIDE_CONT) continue;  // drawn by its lead cell
            line += utf8_of(cell.codepoint ? cell.codepoint : U' ');
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        std::printf("%s\n", line.c_str());
    }
    return 0;
}
