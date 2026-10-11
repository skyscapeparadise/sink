// Fuzzes everything untrusted output can reach: the escape-sequence parser and
// the grid behind it, interleaved with the things a user does to that grid --
// resizing, clicking, selecting, scrolling, searching. Built with libFuzzer and
// AddressSanitizer + UndefinedBehaviorSanitizer, so a memory error or undefined
// behaviour anywhere on these paths stops the run with the input that caused it.
//
// Beyond not crashing, it checks properties that make a terminal safe:
//
// - Every byte sink sends back to the shell is something sink composed itself.
//   A reply that carries text from the input is how a terminal becomes a
//   command-injection vector: `cat` a file whose escape sequences make the
//   terminal "type" a command. Each reply is matched against the complete list
//   of formats sink produces, with only numbers and fixed strings variable.
// - Mouse editing only ever produces cursor movement and Delete keys.
// - The cursor and the grid's dimensions stay consistent.
//
// Pure computation: nothing here touches files, processes or the network.
// See README.md for how it is built and sandboxed.

#include "ansi_parser.hpp"
#include "prompt_editing.hpp"
#include "terminal_grid.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <regex>
#include <string>

namespace {

// Reads the fuzzer's input as a sequence of operations.
struct Input {
    const uint8_t* p;
    size_t n;
    bool empty() const { return n == 0; }
    uint8_t byte() {
        if (!n) return 0;
        --n;
        return *p++;
    }
    int range(int lo, int hi) {  // inclusive
        uint16_t v = static_cast<uint16_t>(byte()) | (static_cast<uint16_t>(byte()) << 8);
        return lo + static_cast<int>(v % static_cast<uint32_t>(hi - lo + 1));
    }
    std::string bytes(size_t max) {
        size_t len = std::min<size_t>(static_cast<size_t>(range(0, static_cast<int>(max))), n);
        std::string out(reinterpret_cast<const char*>(p), len);
        p += len;
        n -= len;
        return out;
    }
};

[[noreturn]] void fail(const char* what, const std::string& detail) {
    std::fprintf(stderr, "PROPERTY VIOLATED: %s\n", what);
    for (unsigned char c : detail) {
        if (c >= 32 && c < 127) std::fputc(c, stderr);
        else std::fprintf(stderr, "\\x%02x", c);
    }
    std::fputc('\n', stderr);
    std::abort();
}

// Every reply format sink can produce (ansi_parser.cpp, all queue_reply call
// sites). Only numbers and sink's own fixed strings vary. Adding a reply to
// the parser means adding it here -- deliberately: the point is that nothing
// slips into a reply unexamined.
const std::regex& reply_grammar() {
    static const std::regex re(
        "("
        // kitty graphics status
        "\x1b_G(i=[0-9]+)?(,?p=[0-9]+)?;"
        "(OK|EBADF:only direct transmission is supported|EINVAL:compression is not supported|"
        "EINVAL:could not decode PNG|EINVAL:raw pixels do not match s and v|"
        "EINVAL:unsupported format|ENOMEM:image rejected|ENOENT:no such image)\x1b\\\\"
        // OSC 4 / 10 / 11 / 12 colour reports
        "|\x1b\\]((4;[0-9]+)|1[012]);rgb:[0-9a-f]{4}/[0-9a-f]{4}/[0-9a-f]{4}(\x07|\x1b\\\\)"
        // XTWINOPS size reports
        "|\x1b\\[[4689];[0-9]+;[0-9]+t"
        // DECRQM
        "|\x1b\\[\\??[0-9]+;[0-9]+\\$y"
        // DSR: cursor position (plain and DECXCPR), status
        "|\x1b\\[\\?[0-9]+;[0-9]+;1R|\x1b\\[[0-9]+;[0-9]+R|\x1b\\[0n"
        // DA1, DA2
        "|\x1b\\[\\?62;22c|\x1b\\[>0;[0-9]+;0c"
        // kitty keyboard flags
        "|\x1b\\[\\?[0-9]+u"
        ")+");
    return re;
}

void check_replies(TerminalGrid& g) {
    if (!g.has_pending_reply()) return;
    std::string reply = g.take_pending_reply();
    if (!std::regex_match(reply, reply_grammar())) fail("reply not composed by sink", reply);
}

void check_movement_only(const std::string& payload, bool allow_delete) {
    for (size_t i = 0; i < payload.size();) {
        if (payload.compare(i, 3, "\x1b[C") == 0 || payload.compare(i, 3, "\x1b[D") == 0 ||
            payload.compare(i, 3, "\x1bOC") == 0 || payload.compare(i, 3, "\x1bOD") == 0) {
            i += 3;
        } else if (allow_delete && payload.compare(i, 4, "\x1b[3~") == 0) {
            i += 4;
        } else {
            fail("mouse editing produced something other than cursor keys", payload);
        }
    }
}

void check_invariants(const TerminalGrid& g) {
    int cols = g.get_cols(), rows = g.get_rows();
    if (cols < 1 || rows < 1) fail("grid has no cells", std::to_string(cols) + "x" + std::to_string(rows));
    int cr = g.get_cursor_row(), cc = g.get_cursor_col();
    if (cr < 0 || cr >= rows || cc < 0 || cc >= cols) {
        fail("cursor outside the grid", std::to_string(cr) + "," + std::to_string(cc) + " in " +
                                        std::to_string(cols) + "x" + std::to_string(rows));
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    Input in{ data, size };
    TerminalGrid g;
    // Small enough that a lot happens at the edges, where off-by-ones live.
    g.resize(in.range(1, 100), in.range(1, 40));
    g.set_max_scrollback(static_cast<size_t>(in.range(0, 200)));
    ANSIParser parser;

    int steps = 0;
    while (!in.empty() && steps++ < 64) {
        switch (in.byte() % 12) {
            case 0: case 1: case 2: case 3: case 4: {
                // Untrusted output, the bulk of every run.
                std::string chunk = in.bytes(4096);
                parser.parse(g, chunk.data(), chunk.size());
                g.note_output_idle();
                break;
            }
            case 5:
                g.resize(in.range(1, 160), in.range(1, 60));
                break;
            case 6: {
                int row = in.range(-5, g.get_rows() + 5), col = in.range(-5, g.get_cols() + 5);
                std::string payload = prompt_edit::click_payload(g, row, col);
                check_movement_only(payload, false);
                if (!payload.empty()) g.note_user_edit();
                break;
            }
            case 7: {
                g.start_selection(in.range(-3, g.get_cols() + 3), in.range(-3, g.get_rows() + 3));
                g.update_selection(in.range(-3, g.get_cols() + 3), in.range(-3, g.get_rows() + 3));
                g.end_selection();
                std::string payload;
                if (prompt_edit::delete_selection_payload(g, payload)) check_movement_only(payload, true);
                (void)g.get_selected_text();
                g.clear_selection();
                break;
            }
            case 8:
                (void)prompt_edit::current_line_text(g);
                (void)g.get_all_text();
                break;
            case 9:
                switch (in.byte() % 4) {
                    case 0: g.note_user_edit(); break;
                    case 1: g.note_user_submit(); break;
                    case 2: g.scroll_view(in.range(-50, 50)); break;
                    default: g.reset_scroll(); break;
                }
                break;
            case 10: {
                g.set_search_active(true);
                g.set_search_query(in.bytes(16));
                if (in.byte() & 1) g.set_search_active(false);
                break;
            }
            case 11:
                if (in.byte() & 1) g.scroll_to_prev_prompt(); else g.scroll_to_next_prompt();
                if (in.byte() & 1) g.select_all();
                (void)g.take_clipboard_text();
                (void)g.take_window_title();
                break;
        }
        check_replies(g);
        check_invariants(g);
    }
    return 0;
}
