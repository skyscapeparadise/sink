// Unit tests for ANSIParser + TerminalGrid. No window, renderer, or PTY is
// created: byte sequences are fed straight into the parser and the resulting
// grid state is asserted on. Run via `ctest --test-dir build` or ./build/sink_tests.
#include "ansi_parser.hpp"
#include "terminal_grid.hpp"

#include <cmath>
#include <cstdio>
#include <string>

static int checks_failed = 0;
static int checks_run = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++checks_run;                                                        \
        if (!(cond)) {                                                       \
            ++checks_failed;                                                 \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)

static void feed(ANSIParser& p, TerminalGrid& g, const std::string& bytes) {
    p.parse(g, bytes.data(), bytes.size());
}

// Cell colours are stored packed to 8 bits per channel (see PackedColor in
// terminal_grid.hpp), so unpack before comparing. Quantisation error is at
// most 0.5/255 ~= 0.002, comfortably inside the tolerance below.
static bool color_near(const PackedColor& pc, float r, float g, float b) {
    SDL_FColor c = unpack_color(pc);
    return std::fabs(c.r - r) < 0.005f && std::fabs(c.g - g) < 0.005f &&
           std::fabs(c.b - b) < 0.005f;
}

static std::string row_text(const TerminalGrid& g, int row) {
    std::string s;
    for (int c = 0; c < g.get_cols(); ++c) {
        char32_t cp = g.get_cell_at(c, row).codepoint;
        s += (cp >= 32 && cp < 127) ? static_cast<char>(cp) : '?';
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

static void test_plain_text() {
    TerminalGrid g; g.resize(20, 5);
    ANSIParser p;
    feed(p, g, "hello");
    CHECK(row_text(g, 0) == "hello");
    CHECK(g.get_cursor_col() == 5);
    CHECK(g.get_cursor_row() == 0);
}

static void test_crlf_and_scroll() {
    TerminalGrid g; g.resize(20, 3);
    ANSIParser p;
    feed(p, g, "one\r\ntwo\r\nthree\r\nfour");
    // "one" scrolled into history; screen shows two/three/four
    CHECK(g.get_scrollback_size() == 1);
    CHECK(row_text(g, 0) == "two");
    CHECK(row_text(g, 2) == "four");
}

// The row ring's base index wraps back past zero once you scroll more times
// than the grid has rows. test_crlf_and_scroll above scrolls exactly once, so
// it never reaches that case; this drives enough lines through a small grid to
// wrap the base several times and checks that both the visible rows and
// subsequent row-addressed operations still land on the right cells.
static void test_scroll_ring_wraparound() {
    TerminalGrid g; g.resize(20, 4);
    ANSIParser p;

    std::string feed_str;
    for (int i = 1; i <= 20; ++i) {
        feed_str += "line" + std::to_string(i);
        if (i != 20) feed_str += "\r\n";
    }
    feed(p, g, feed_str);   // 16 scrolls through a 4-row grid: base wraps 4x

    CHECK(g.get_scrollback_size() == 16);
    CHECK(row_text(g, 0) == "line17");
    CHECK(row_text(g, 1) == "line18");
    CHECK(row_text(g, 2) == "line19");
    CHECK(row_text(g, 3) == "line20");

    // Row-addressed operations must still resolve through the wrapped base
    feed(p, g, "\x1b[1;1H\x1b[2K");        // home, erase whole line
    CHECK(row_text(g, 0) == "");
    CHECK(row_text(g, 3) == "line20");

    feed(p, g, "\x1b[3;1Hxy");              // CUP row 3: overwrite "li" of "line19"
    CHECK(row_text(g, 2) == "xyne19");
    CHECK(row_text(g, 3) == "line20");
}

// scroll_up() steals the outgoing scrollback row's buffer to avoid allocating
// on every newline, which only happens once history is at its cap. The tests
// above all run with the default 10000-line cap and so never reach that path;
// this one caps history at 3 and scrolls well past it, then scrolls the view
// back into history to confirm the recycled rows hold the right contents and
// not a stale or emptied buffer.
static void test_scrollback_cap_recycles_rows() {
    TerminalGrid g; g.resize(20, 2);
    ANSIParser p;
    g.set_max_scrollback(3);

    std::string feed_str;
    for (int i = 1; i <= 12; ++i) {
        feed_str += "L" + std::to_string(i);
        if (i != 12) feed_str += "\r\n";
    }
    feed(p, g, feed_str);

    CHECK(g.get_scrollback_size() == 3);
    CHECK(row_text(g, 0) == "L11");
    CHECK(row_text(g, 1) == "L12");

    // Scroll the view back through all three retained history rows: they must
    // be L8, L9, L10 -- the three immediately preceding the visible pair.
    g.scroll_view(3);
    CHECK(g.get_scroll_offset() == 3);
    CHECK(row_text(g, 0) == "L8");
    CHECK(row_text(g, 1) == "L9");
    g.scroll_view(-1);
    CHECK(row_text(g, 0) == "L9");
    CHECK(row_text(g, 1) == "L10");
    g.reset_scroll();
    CHECK(row_text(g, 0) == "L11");
}

// The error/failed flash trigger had no coverage at all, despite its scan
// living on the hottest path in the parser and being rewritten twice (whole-
// window search -> tail-only test -> a 64-bit shift register). These pin the
// behaviour that matters: case-insensitivity, matching across separate parse()
// calls, non-printables not breaking a word, and -- the case that exercised
// the old sliding-window shift -- a match arriving well after the window that
// used to be 32 bytes had filled.
static void test_error_flash_trigger() {
    auto glow_after = [](const std::string& s1, const std::string& s2) {
        TerminalGrid g; g.resize(200, 5);
        ANSIParser p;
        feed(p, g, s1);
        if (!s2.empty()) feed(p, g, s2);
        return g.get_error_glow_opacity();
    };

    CHECK(glow_after("error", "") > 0.0f);
    CHECK(glow_after("failed", "") > 0.0f);
    CHECK(glow_after("ERROR", "") > 0.0f);          // lowercased before matching
    CHECK(glow_after("Failed", "") > 0.0f);
    CHECK(glow_after("build error here", "") > 0.0f);

    CHECK(glow_after("erro", "") == 0.0f);          // incomplete
    CHECK(glow_after("erroX", "") == 0.0f);
    CHECK(glow_after("faile", "") == 0.0f);

    // The window must survive across parse() calls -- a PTY read can split
    // anywhere, including mid-word.
    CHECK(glow_after("erro", "r") > 0.0f);
    CHECK(glow_after("fail", "ed") > 0.0f);

    // Non-printables are not added to the window, so they don't break a match.
    CHECK(glow_after("err\x01or", "") > 0.0f);

    // Well past the 32 characters the old sliding buffer held: the match still
    // has to be found from the tail of a long run of preceding text.
    CHECK(glow_after(std::string(200, 'a') + "error", "") > 0.0f);
    CHECK(glow_after(std::string(200, 'a'), "") == 0.0f);

    // Matching now scans each run and carries only a tail between runs, so
    // every split point has to work -- not just the one the old per-character
    // ring made trivially safe. Checked both with and without a long run
    // ahead of the word, since a long run takes a different carry path.
    for (const std::string& word : {std::string("error"), std::string("failed")}) {
        for (size_t cut = 0; cut <= word.size(); ++cut) {
            CHECK(glow_after(word.substr(0, cut), word.substr(cut)) > 0.0f);
            CHECK(glow_after(std::string(50, 'a') + word.substr(0, cut),
                             word.substr(cut)) > 0.0f);
        }
    }

    // A near miss either side of the window must still not fire.
    CHECK(glow_after("xerrorx", "") > 0.0f);   // contained in a longer run
    CHECK(glow_after("errar", "") == 0.0f);
    CHECK(glow_after("faild", "") == 0.0f);
    CHECK(glow_after(std::string(50, 'a') + "erro", "") == 0.0f);
}

// East Asian Wide and Fullwidth characters, and emoji, occupy two columns.
// Before this the grid advanced one column per codepoint regardless, so CJK
// and emoji rendered at half the width every other terminal gives them and
// anything column-aligned drifted.
// DECSET/DECRST 2026 (synchronized output) and 1004 (focus reporting). Both
// are mode flags the app layer acts on -- holding the presented frame, and
// sending CSI I / CSI O -- so what is checked here is that the parser tracks
// them, including that an unknown neighbouring mode doesn't disturb them.
//
// 2026 is deliberately pure state with no timing in it. How long a held frame
// may be honoured belongs to the render loop, measured from the last actual
// present; see kMaxSyncHoldMs in main.cpp.
// SearchResult::col is a column, but set_search_query() was assigning it the
// byte offset std::string::find() returns. On any row containing multi-byte
// text the highlight drifted right, further with each such character before
// the match. These check the mapping through the user-visible predicate.
// Combining marks compose onto the character before them instead of taking a
// cell. This is the shape macOS produces constantly: it stores filenames in
// NFD, so `ls` in a directory with accented names emits base+mark sequences.
// RIS (ESC c): return the terminal to its power-on state.
//
// vtebench sends this between every sample and again at the end, and its setup
// scripts enter the alt screen and set narrow scroll regions. A terminal that
// ignores RIS accumulates those, so output ends up scrolling inside a couple
// of rows and the screen appears frozen -- which is exactly what happened.
static void test_ris_full_reset() {
    TerminalGrid g; g.resize(20, 10);
    ANSIParser p;

    // Put the terminal into as many non-default states as possible.
    feed(p, g, "scrollback\r\n");
    feed(p, g, "\x1b[?1049h");          // alt screen
    feed(p, g, "\x1b[3;5r");            // narrow scroll region
    feed(p, g, "\x1b[?6h");             // origin mode
    feed(p, g, "\x1b[31;1mred bold");   // colour + attribute
    feed(p, g, "\x1b[?25l");            // hide cursor
    feed(p, g, "\x1b[?2004h");          // bracketed paste
    feed(p, g, "\x1b[?1000h");          // mouse reporting
    feed(p, g, "\x1b[?1h");             // application cursor keys
    feed(p, g, "\x1b[?2026h");          // synchronized output
    feed(p, g, "\x1b[?1004h");          // focus reporting
    feed(p, g, "\x1b(0");               // DEC Special Graphics

    CHECK(g.is_alt_screen_active());
    CHECK(g.get_scroll_top() == 2);
    CHECK(g.is_origin_mode());
    CHECK(!g.is_cursor_visible());
    CHECK(g.get_mouse_mode() == 1000);
    CHECK(g.is_synchronized_output());

    feed(p, g, "\x1b" "c");   // ESC c -- split so \x1bc is not one hex escape

    CHECK(!g.is_alt_screen_active());
    CHECK(g.get_scroll_top() == 0);
    CHECK(g.get_scroll_bottom() == 9);
    CHECK(!g.is_origin_mode());
    CHECK(g.is_cursor_visible());
    CHECK(!g.is_bracketed_paste_active());
    CHECK(g.get_mouse_mode() == 0);
    CHECK(!g.is_app_cursor_keys());
    CHECK(!g.is_synchronized_output());
    CHECK(!g.is_focus_reporting());
    CHECK(g.get_cursor_row() == 0);
    CHECK(g.get_cursor_col() == 0);
    CHECK(g.get_scrollback_size() == 0);
    CHECK(g.get_current_attrs() == 0);
    CHECK(row_text(g, 0) == "");

    // Charset selection is parser state and must reset too: 'q' would draw a
    // horizontal line while DEC Special Graphics is active.
    feed(p, g, "q");
    CHECK(g.get_cell_at(0, 0).codepoint == U'q');

    // And the terminal is usable afterwards, with default colours.
    feed(p, g, "\x1b[2;1Hafter");
    CHECK(row_text(g, 1) == "after");
    CHECK(color_near(g.get_cell_at(0, 1).fg, 0.9f, 0.9f, 0.9f));
}

static void test_combining_marks() {
    CHECK(is_combining_mark(0x0301));            // combining acute
    CHECK(is_combining_mark(0x0308));            // combining diaeresis
    CHECK(!is_combining_mark(U'a'));
    CHECK(!is_combining_mark(0x4F60));           // CJK is not a mark
    CHECK(compose_pair(U'e', 0x0301) == 0x00E9); // e + acute -> eacute
    CHECK(compose_pair(U'n', 0x0303) == 0x00F1); // n + tilde  -> ntilde
    CHECK(compose_pair(U'z', 0x0301) == 0x017A);
    CHECK(compose_pair(U'q', 0x0301) == 0);      // no precomposed form

    // NFD "café" -- c a f e U+0301 -- occupies four columns, not five, and
    // the last cell holds the composed character.
    TerminalGrid g; g.resize(20, 4);
    ANSIParser p;
    feed(p, g, "caf" "e\xcc\x81");
    CHECK(g.get_cell_at(0, 0).codepoint == U'c');
    CHECK(g.get_cell_at(1, 0).codepoint == U'a');
    CHECK(g.get_cell_at(2, 0).codepoint == U'f');
    CHECK(g.get_cell_at(3, 0).codepoint == 0x00E9);
    CHECK(g.get_cursor_col() == 4);

    // Text after it lands where it should rather than a column late.
    feed(p, g, "!");
    CHECK(g.get_cell_at(4, 0).codepoint == U'!');

    // A mark with no base to attach to is dropped, not written to a cell.
    TerminalGrid g2; g2.resize(20, 4);
    ANSIParser p2;
    feed(p2, g2, "\xcc\x81");
    CHECK(g2.get_cell_at(0, 0).codepoint == 32);
    CHECK(g2.get_cursor_col() == 0);

    // Marks compose onto a double-width base without disturbing its pair.
    TerminalGrid g3; g3.resize(20, 4);
    ANSIParser p3;
    feed(p3, g3, "\xe4\xbd\xa0\xcc\x81");        // CJK then a mark
    CHECK(g3.get_cell_at(0, 0).codepoint == 0x4F60); // no composed form, base intact
    CHECK((g3.get_cell_at(1, 0).attrs & ATTR_WIDE_CONT) != 0);
    CHECK(g3.get_cursor_col() == 2);
}

static void test_search_column_mapping() {
    // Latin-1 accented: two UTF-8 bytes, one column.
    {
        TerminalGrid g; g.resize(30, 4);
        ANSIParser p;
        feed(p, g, "\xc3\xa9" "abc");           // eacute a b c
        g.set_search_active(true);
        g.set_search_query("abc");
        CHECK(!g.is_cell_search_matched(0, 0));  // the accented char itself
        CHECK(g.is_cell_search_matched(1, 0));
        CHECK(g.is_cell_search_matched(2, 0));
        CHECK(g.is_cell_search_matched(3, 0));
        CHECK(!g.is_cell_search_matched(4, 0));
    }

    // CJK: three bytes and two columns each, so byte offsets drift twice over.
    {
        TerminalGrid g; g.resize(30, 4);
        ANSIParser p;
        feed(p, g, "\xe4\xbd\xa0\xe5\xa5\xbd" "abc");   // two CJK, then abc
        g.set_search_active(true);
        g.set_search_query("abc");
        CHECK(!g.is_cell_search_matched(3, 0));  // still inside the CJK pair
        CHECK(g.is_cell_search_matched(4, 0));
        CHECK(g.is_cell_search_matched(5, 0));
        CHECK(g.is_cell_search_matched(6, 0));
        CHECK(!g.is_cell_search_matched(7, 0));

        // A match on the wide character itself highlights both its columns.
        g.set_search_query("\xe4\xbd\xa0");
        CHECK(g.is_cell_search_matched(0, 0));
        CHECK(g.is_cell_search_matched(1, 0));
        CHECK(!g.is_cell_search_matched(2, 0));
    }

    // ASCII case-insensitivity still works, and does not corrupt UTF-8 bytes
    // on the same row.
    {
        TerminalGrid g; g.resize(30, 4);
        ANSIParser p;
        feed(p, g, "Caf\xc3\xa9" " World");
        g.set_search_active(true);
        g.set_search_query("world");
        CHECK(g.is_cell_search_matched(5, 0));
        CHECK(g.is_cell_search_matched(9, 0));
        CHECK(!g.is_cell_search_matched(10, 0));
        CHECK(!g.is_cell_search_matched(4, 0));
    }
}

static void test_synchronized_output_and_focus_modes() {
    TerminalGrid g; g.resize(20, 5);
    ANSIParser p;

    CHECK(!g.is_synchronized_output());
    CHECK(!g.is_focus_reporting());

    feed(p, g, "\x1b[?2026h");
    CHECK(g.is_synchronized_output());
    feed(p, g, "\x1b[?2026l");
    CHECK(!g.is_synchronized_output());

    feed(p, g, "\x1b[?1004h");
    CHECK(g.is_focus_reporting());
    feed(p, g, "\x1b[?1004l");
    CHECK(!g.is_focus_reporting());

    // Set together, reset independently
    feed(p, g, "\x1b[?2026h\x1b[?1004h");
    CHECK(g.is_synchronized_output());
    CHECK(g.is_focus_reporting());
    feed(p, g, "\x1b[?2026l");
    CHECK(!g.is_synchronized_output());
    CHECK(g.is_focus_reporting());

    // An unrecognised mode in between must not clear either
    feed(p, g, "\x1b[?2026h\x1b[?7h");
    CHECK(g.is_synchronized_output());
    CHECK(g.is_focus_reporting());
}

static void test_wide_characters() {
    CHECK(char_display_width(U'A') == 1);
    CHECK(char_display_width(U'\u00e9') == 1);       // Latin-1 accented
    CHECK(char_display_width(U'\u2500') == 1);       // box-drawing stays narrow
    CHECK(char_display_width(U'\u4f60') == 2);       // CJK
    CHECK(char_display_width(U'\u3042') == 2);       // kana
    CHECK(char_display_width(U'\uff21') == 2);       // fullwidth A
    CHECK(char_display_width(U'\uac00') == 2);       // Hangul syllable
    CHECK(char_display_width(0x1F680) == 2);          // emoji

    // Two CJK codepoints occupy four columns, each with a marked trailing half
    TerminalGrid g; g.resize(20, 4);
    ANSIParser p;
    feed(p, g, "\xe4\xbd\xa0\xe5\xa5\xbd");        // U+4F60 U+597D
    CHECK(g.get_cell_at(0, 0).codepoint == 0x4F60);
    CHECK((g.get_cell_at(1, 0).attrs & ATTR_WIDE_CONT) != 0);
    CHECK(g.get_cell_at(2, 0).codepoint == 0x597D);
    CHECK((g.get_cell_at(3, 0).attrs & ATTR_WIDE_CONT) != 0);
    CHECK(g.get_cursor_col() == 4);

    // Narrow text after a wide char lands on the right column
    feed(p, g, "ab");
    CHECK(g.get_cell_at(4, 0).codepoint == 'a');
    CHECK(g.get_cell_at(5, 0).codepoint == 'b');
    CHECK(g.get_cursor_col() == 6);
}

// A double-width glyph cannot straddle a line break. With one column left it
// has to wrap first, and the column it leaves behind must be blanked rather
// than keeping whatever was there.
static void test_wide_character_wrap() {
    TerminalGrid g; g.resize(5, 4);
    ANSIParser p;
    feed(p, g, "abcd");                              // fills columns 0-3
    CHECK(g.get_cursor_col() == 4);
    feed(p, g, "\xe4\xbd\xa0");                     // U+4F60, needs two columns
    CHECK(g.get_cell_at(4, 0).codepoint == 32);       // vacated column blanked
    CHECK(g.get_cell_at(0, 1).codepoint == 0x4F60);   // wrapped to the next row
    CHECK((g.get_cell_at(1, 1).attrs & ATTR_WIDE_CONT) != 0);
    CHECK(g.get_cursor_row() == 1);
    CHECK(g.get_cursor_col() == 2);
}

// Copying a region containing wide characters must not emit the trailing
// half, which carries codepoint 0 and would otherwise become a NUL byte.
static void test_wide_character_copy() {
    TerminalGrid g; g.resize(20, 4);
    ANSIParser p;
    feed(p, g, "x\xe4\xbd\xa0y");                   // x U+4F60 y
    g.select_all();
    std::string sel = g.get_selected_text();
    CHECK(sel.find('\0') == std::string::npos);
    CHECK(sel.find("x\xe4\xbd\xa0y") != std::string::npos);
}

static void test_cup_and_relative_motion() {
    TerminalGrid g; g.resize(20, 5);
    ANSIParser p;
    feed(p, g, "\x1b[3;5H");    // CUP row 3, col 5 (1-based)
    CHECK(g.get_cursor_row() == 2);
    CHECK(g.get_cursor_col() == 4);
    feed(p, g, "\x1b[2A\x1b[3C"); // up 2, right 3
    CHECK(g.get_cursor_row() == 0);
    CHECK(g.get_cursor_col() == 7);
    feed(p, g, "\x1b[0B");      // param 0 counts as 1
    CHECK(g.get_cursor_row() == 1);
}

static void test_sgr_16_color() {
    TerminalGrid g; g.resize(20, 5);
    ANSIParser p;
    feed(p, g, "\x1b[31mx");
    CHECK(color_near(g.get_cell_at(0, 0).fg, 0.85f, 0.15f, 0.15f));
    feed(p, g, "\x1b[0m\x1b[92my");
    CHECK(color_near(g.get_cell_at(1, 0).fg, 0.30f, 1.00f, 0.30f));
}

static void test_sgr_256_color() {
    TerminalGrid g; g.resize(20, 5);
    ANSIParser p;
    // Index 196 = cube(5,0,0) = rgb(255,0,0)
    feed(p, g, "\x1b[38;5;196ma");
    CHECK(color_near(g.get_cell_at(0, 0).fg, 1.0f, 0.0f, 0.0f));
    // Index 16 = cube(0,0,0) = black
    feed(p, g, "\x1b[38;5;16mb");
    CHECK(color_near(g.get_cell_at(1, 0).fg, 0.0f, 0.0f, 0.0f));
    // Index 231 = cube(5,5,5) = white
    feed(p, g, "\x1b[48;5;231mc");
    CHECK(color_near(g.get_cell_at(2, 0).bg, 1.0f, 1.0f, 1.0f));
    // Grayscale ramp: 232 = rgb(8,8,8), 255 = rgb(238,238,238)
    feed(p, g, "\x1b[38;5;232md");
    CHECK(color_near(g.get_cell_at(3, 0).fg, 8/255.0f, 8/255.0f, 8/255.0f));
    feed(p, g, "\x1b[38;5;255me");
    CHECK(color_near(g.get_cell_at(4, 0).fg, 238/255.0f, 238/255.0f, 238/255.0f));
    // Index 4 falls through to the stylized ANSI palette
    feed(p, g, "\x1b[38;5;4mf");
    CHECK(color_near(g.get_cell_at(5, 0).fg, 0.15f, 0.15f, 0.85f));
}

static void test_sgr_256_colon_form() {
    TerminalGrid g; g.resize(20, 5);
    ANSIParser p;
    feed(p, g, "\x1b[38:5:196ma");
    CHECK(color_near(g.get_cell_at(0, 0).fg, 1.0f, 0.0f, 0.0f));
    feed(p, g, "\x1b[0m\x1b[38:2:0:255:0mb");
    CHECK(color_near(g.get_cell_at(1, 0).fg, 0.0f, 1.0f, 0.0f));
}

static void test_sgr_truecolor() {
    TerminalGrid g; g.resize(20, 5);
    ANSIParser p;
    feed(p, g, "\x1b[38;2;10;20;30mx");
    CHECK(color_near(g.get_cell_at(0, 0).fg, 10/255.0f, 20/255.0f, 30/255.0f));
}

static void test_decom_origin_mode() {
    TerminalGrid g; g.resize(20, 6);
    ANSIParser p;
    // Region rows 2-4 (1-based); enable DECOM
    feed(p, g, "\x1b[2;4r\x1b[?6h");
    CHECK(g.is_origin_mode());
    // Enabling DECOM homes the cursor to the region's top margin, not (0,0)
    CHECK(g.get_cursor_row() == 1);
    CHECK(g.get_cursor_col() == 0);

    // CUP row 1 now means the region's top margin (absolute row 1), not
    // the screen's top (absolute row 0)
    feed(p, g, "\x1b[1;3H");
    CHECK(g.get_cursor_row() == 1);
    CHECK(g.get_cursor_col() == 2);

    // CUP can't be positioned outside the region while DECOM is set
    feed(p, g, "\x1b[10;1H");
    CHECK(g.get_cursor_row() == 3); // clamped to scroll_bottom (absolute row 3)

    // Disabling DECOM: CUP row 1 is the screen's top again, and disabling
    // itself homes the cursor back to absolute (0,0)
    feed(p, g, "\x1b[?6l");
    CHECK(!g.is_origin_mode());
    CHECK(g.get_cursor_row() == 0);
    feed(p, g, "\x1b[1;1H");
    CHECK(g.get_cursor_row() == 0);
}

static void test_decstbm_basic_scroll() {
    TerminalGrid g; g.resize(20, 6);
    ANSIParser p;
    // Fill six rows
    feed(p, g, "r0\r\nr1\r\nr2\r\nr3\r\nr4\r\nr5");
    // Region rows 2-4 (1-based), cursor homes to 0,0
    feed(p, g, "\x1b[2;4r");
    CHECK(g.get_scroll_top() == 1);
    CHECK(g.get_scroll_bottom() == 3);
    CHECK(g.get_cursor_row() == 0);
    // LF from region bottom scrolls only the region, no scrollback push
    size_t sb_before = g.get_scrollback_size();
    feed(p, g, "\x1b[4;1H\n");
    CHECK(g.get_scrollback_size() == sb_before);
    CHECK(row_text(g, 0) == "r0"); // outside region untouched
    CHECK(row_text(g, 1) == "r2"); // region shifted up
    CHECK(row_text(g, 2) == "r3");
    CHECK(row_text(g, 3) == "");   // blank fill at region bottom
    CHECK(row_text(g, 4) == "r4"); // below region untouched
    CHECK(row_text(g, 5) == "r5");
}

static void test_decstbm_reset() {
    TerminalGrid g; g.resize(20, 6);
    ANSIParser p;
    feed(p, g, "\x1b[2;4r\x1b[r");
    CHECK(g.get_scroll_top() == 0);
    CHECK(g.get_scroll_bottom() == 5);
}

static void test_reverse_index() {
    TerminalGrid g; g.resize(20, 4);
    ANSIParser p;
    feed(p, g, "a\r\nb\r\nc\r\nd");
    // RI above region top scrolls region down
    feed(p, g, "\x1b[1;3r\x1b[1;1H\x1bM");
    CHECK(row_text(g, 0) == "");
    CHECK(row_text(g, 1) == "a");
    CHECK(row_text(g, 2) == "b");
    CHECK(row_text(g, 3) == "d"); // outside region untouched
    // Plain RI mid-screen just moves up
    feed(p, g, "\x1b[3;1H\x1bM");
    CHECK(g.get_cursor_row() == 1);
}

static void test_insert_delete_lines() {
    TerminalGrid g; g.resize(20, 5);
    ANSIParser p;
    feed(p, g, "a\r\nb\r\nc\r\nd\r\ne");
    // Insert 2 lines at row 2: b,c shift down, d lost past bottom... (full-screen region)
    feed(p, g, "\x1b[2;1H\x1b[2L");
    CHECK(row_text(g, 0) == "a");
    CHECK(row_text(g, 1) == "");
    CHECK(row_text(g, 2) == "");
    CHECK(row_text(g, 3) == "b");
    CHECK(row_text(g, 4) == "c");
    // Delete 2 lines at row 2: b,c return
    feed(p, g, "\x1b[2;1H\x1b[2M");
    CHECK(row_text(g, 1) == "b");
    CHECK(row_text(g, 2) == "c");
    CHECK(row_text(g, 3) == "");
}

static void test_su_sd() {
    TerminalGrid g; g.resize(20, 4);
    ANSIParser p;
    feed(p, g, "a\r\nb\r\nc\r\nd");
    feed(p, g, "\x1b[2S"); // scroll up 2
    CHECK(row_text(g, 0) == "c");
    CHECK(row_text(g, 1) == "d");
    feed(p, g, "\x1b[1T"); // scroll down 1
    CHECK(row_text(g, 0) == "");
    CHECK(row_text(g, 1) == "c");
}

static void test_wrap_within_region() {
    TerminalGrid g; g.resize(5, 4);
    ANSIParser p;
    // Region rows 1-2; printing past the right edge at region bottom must
    // scroll the region, not the screen
    feed(p, g, "\x1b[1;2r\x1b[2;1Habcdefg");
    CHECK(g.get_scrollback_size() == 0);
    CHECK(row_text(g, 0) == "abcde");
    CHECK(row_text(g, 1) == "fg");
    CHECK(row_text(g, 2) == "");
}

static void test_modes() {
    TerminalGrid g; g.resize(20, 5);
    ANSIParser p;
    feed(p, g, "\x1b[?25l");
    CHECK(!g.is_cursor_visible());
    feed(p, g, "\x1b[?25h");
    CHECK(g.is_cursor_visible());
    feed(p, g, "\x1b[?2004h");
    CHECK(g.is_bracketed_paste_active());
    feed(p, g, "\x1b[?1049h");
    CHECK(g.is_alt_screen_active());
    feed(p, g, "\x1b[?1049l\x1b[?2004l");
    CHECK(!g.is_alt_screen_active());
    CHECK(!g.is_bracketed_paste_active());
}

static void test_sgr_attributes() {
    TerminalGrid g; g.resize(20, 5);
    ANSIParser p;
    feed(p, g, "\x1b[4mu\x1b[7mr\x1b[9ms\x1b[2md");
    CHECK(g.get_cell_at(0, 0).attrs & ATTR_UNDERLINE);
    CHECK(g.get_cell_at(1, 0).attrs & ATTR_REVERSE);
    CHECK(g.get_cell_at(2, 0).attrs & ATTR_STRIKETHROUGH);
    CHECK(g.get_cell_at(3, 0).attrs & ATTR_DIM);
    // Selective resets
    feed(p, g, "\x1b[24;27;29ma");
    CHECK(!(g.get_cell_at(4, 0).attrs & (ATTR_UNDERLINE | ATTR_REVERSE | ATTR_STRIKETHROUGH)));
    CHECK(g.get_cell_at(4, 0).attrs & ATTR_DIM); // 22 not sent yet
    feed(p, g, "\x1b[0mb");
    CHECK(g.get_cell_at(5, 0).attrs == 0);
}

static void test_bold_as_bright() {
    TerminalGrid g; g.resize(20, 5);
    ANSIParser p;
    // Color then bold: red brightens
    feed(p, g, "\x1b[31m\x1b[1ma");
    CHECK(color_near(g.get_cell_at(0, 0).fg, 1.00f, 0.30f, 0.30f));
    CHECK(g.get_cell_at(0, 0).attrs & ATTR_BOLD);
    // Bold then color, same result
    feed(p, g, "\x1b[0m\x1b[1;32mb");
    CHECK(color_near(g.get_cell_at(1, 0).fg, 0.30f, 1.00f, 0.30f));
    // SGR 22 drops back to the base color
    feed(p, g, "\x1b[22mc");
    CHECK(color_near(g.get_cell_at(2, 0).fg, 0.15f, 0.85f, 0.15f));
    // Bold must not brighten truecolor
    feed(p, g, "\x1b[0m\x1b[38;2;100;100;100m\x1b[1md");
    CHECK(color_near(g.get_cell_at(3, 0).fg, 100/255.0f, 100/255.0f, 100/255.0f));
}

static void test_alt_screen_buffer() {
    TerminalGrid g; g.resize(20, 4);
    ANSIParser p;
    feed(p, g, "shell$ ls");
    int prim_col = g.get_cursor_col();
    // Enter alt screen: blank, cursor home
    feed(p, g, "\x1b[?1049h");
    CHECK(g.is_alt_screen_active());
    CHECK(row_text(g, 0) == "");
    CHECK(g.get_cursor_row() == 0 && g.get_cursor_col() == 0);
    // App draws and scrolls; scrollback must not grow
    size_t sb = g.get_scrollback_size();
    feed(p, g, "APP UI\r\n1\r\n2\r\n3\r\n4\r\n5");
    CHECK(g.get_scrollback_size() == sb);
    // Exit: shell contents and cursor come back
    feed(p, g, "\x1b[?1049l");
    CHECK(!g.is_alt_screen_active());
    CHECK(row_text(g, 0) == "shell$ ls");
    CHECK(g.get_cursor_col() == prim_col);
    CHECK(g.get_cursor_row() == 0);
}

static void test_alt_screen_ed_clear() {
    // vim-style entry: 1049h followed by an ED clear must still restore the
    // primary cursor on exit (the ED wipes DECSC state)
    TerminalGrid g; g.resize(20, 4);
    ANSIParser p;
    feed(p, g, "abc\r\ndef");
    feed(p, g, "\x1b[?1049h\x1b[2J\x1b[Happ");
    feed(p, g, "\x1b[?1049l");
    CHECK(row_text(g, 0) == "abc");
    CHECK(row_text(g, 1) == "def");
    CHECK(g.get_cursor_row() == 1);
    CHECK(g.get_cursor_col() == 3);
}

static void test_mouse_modes() {
    TerminalGrid g; g.resize(20, 5);
    ANSIParser p;
    feed(p, g, "\x1b[?1000h");
    CHECK(g.get_mouse_mode() == 1000);
    CHECK(!g.is_mouse_sgr());
    feed(p, g, "\x1b[?1006h");
    CHECK(g.is_mouse_sgr());
    feed(p, g, "\x1b[?1000l");
    CHECK(g.get_mouse_mode() == 0);
    // Ganged parameters in a single DECSET
    feed(p, g, "\x1b[?1002;1006h");
    CHECK(g.get_mouse_mode() == 1002);
    CHECK(g.is_mouse_sgr());
    // Leaving the alt screen force-clears reporting even without DECRST
    feed(p, g, "\x1b[?1049h\x1b[?1049l");
    CHECK(g.get_mouse_mode() == 0);
    CHECK(!g.is_mouse_sgr());
}

static void test_cursor_key_and_scroll_modes() {
    TerminalGrid g; g.resize(20, 5);
    ANSIParser p;
    feed(p, g, "\x1b[?1h");
    CHECK(g.is_app_cursor_keys());
    feed(p, g, "\x1b[?1l");
    CHECK(!g.is_app_cursor_keys());
    CHECK(g.is_alternate_scroll()); // on by default
    feed(p, g, "\x1b[?1007l");
    CHECK(!g.is_alternate_scroll());
    feed(p, g, "\x1b[?1007h");
    CHECK(g.is_alternate_scroll());
}

static void test_xtsave_does_not_clobber_cursor() {
    TerminalGrid g; g.resize(20, 5);
    ANSIParser p;
    // Save at (3,3), then the ncurses mouse-enable preamble, then restore
    feed(p, g, "\x1b[3;3H\x1b" "7");
    feed(p, g, "\x1b[1;1H\x1b[?1001s\x1b[?1000h");
    feed(p, g, "\x1b" "8");
    CHECK(g.get_cursor_row() == 2);
    CHECK(g.get_cursor_col() == 2);
}

static void test_osc_consumed() {
    TerminalGrid g; g.resize(30, 5);
    ANSIParser p;
    feed(p, g, "\x1b]0;window title\x07visible");
    CHECK(row_text(g, 0) == "visible");
    feed(p, g, "\r\n\x1b]8;;http://x\x1b\\link");
    CHECK(row_text(g, 1) == "link");
}

static void test_ed_el() {
    TerminalGrid g; g.resize(10, 3);
    ANSIParser p;
    feed(p, g, "abcdef\x1b[1;3H\x1b[K"); // EL 0: erase from col 3 to end
    CHECK(row_text(g, 0) == "ab");
    feed(p, g, "\x1b[2J");
    CHECK(row_text(g, 0) == "");
}

static void test_osc_title() {
    TerminalGrid g; g.resize(20, 5);
    ANSIParser p;
    feed(p, g, "\x1b]2;my title\x07");
    CHECK(g.has_pending_title());
    CHECK(g.take_window_title() == "my title");
    CHECK(!g.has_pending_title());
    // OSC 0 (icon+title), ST-terminated, non-ASCII payload
    feed(p, g, "\x1b]0;caf\xc3\xa9\x1b\\");
    CHECK(g.take_window_title() == "caf\xc3\xa9");
}

static void test_osc_52_clipboard() {
    TerminalGrid g; g.resize(20, 5);
    ANSIParser p;
    // base64("hello") == "aGVsbG8="
    feed(p, g, "\x1b]52;c;aGVsbG8=" "\x07");
    CHECK(g.has_pending_clipboard_text());
    CHECK(g.take_clipboard_text() == "hello");
    CHECK(!g.has_pending_clipboard_text());

    // A query ("?") must not be answered -- no pending write, no crash
    feed(p, g, "\x1b]52;c;?" "\x07");
    CHECK(!g.has_pending_clipboard_text());

    // Selection-agnostic: "p" (primary) still just writes the one clipboard
    feed(p, g, "\x1b]52;p;d29ybGQ=" "\x07");
    CHECK(g.take_clipboard_text() == "world");
}

static void test_osc_8_hyperlinks() {
    TerminalGrid g; g.resize(30, 5);
    ANSIParser p;
    // "\x07" is followed by a hex-digit char ('l' isn't one, so this pair is
    // safe, but later ones deliberately use "\x07" "x" concatenation since
    // \x escapes are unbounded and would otherwise swallow a following
    // hex-digit char (e.g. "\x07a" parses as the single byte 0x7A = 'z')
    feed(p, g, "\x1b]8;;https://example.com\x07link\x1b]8;;\x07plain");
    CHECK(g.get_cell_at(0, 0).codepoint == 'l');
    uint32_t link_id = g.get_cell_at(0, 0).hyperlink_id;
    CHECK(link_id != 0);
    CHECK(g.get_hyperlink_uri(link_id) == "https://example.com");
    // "link" is 4 chars; the 5th cell ('p' of "plain") must not carry it
    CHECK(g.get_cell_at(3, 0).hyperlink_id == link_id);
    CHECK(g.get_cell_at(4, 0).hyperlink_id == 0);

    // Params before the URI (id=xxx) are skipped, not treated as the URI
    feed(p, g, "\r\n\x1b]8;id=abc;https://x.com" "\x07" "a");
    uint32_t link2 = g.get_cell_at(0, 1).hyperlink_id;
    CHECK(link2 != 0 && link2 != link_id);
    CHECK(g.get_hyperlink_uri(link2) == "https://x.com");

    // Same URI seen again dedupes to the same id ('b' lands at col 1: 'a'
    // from the previous feed left the cursor there)
    feed(p, g, "\x1b]8;;https://example.com\x1b\\b");
    CHECK(g.get_cell_at(1, 1).hyperlink_id == link_id);
}

static void test_osc_133_prompt_marks() {
    TerminalGrid g; g.resize(20, 4);
    ANSIParser p;
    feed(p, g, "\x1b]133;A\x07$ first\r\n");
    feed(p, g, "out1\r\nout2\r\n");
    // The trailing CRLF scrolled once (grid is 4 rows: prompt1 went to
    // scrollback), so the second prompt's mark rides its row up to row 2
    feed(p, g, "\x1b]133;A\x07$ second\r\n");
    CHECK(g.is_prompt_row(2));
    // Push everything into scrollback and verify marks travel with rows
    feed(p, g, "x\r\nx\r\nx\r\nx\r\nx\r\nx\r\n");
    CHECK(g.get_scrollback_size() >= 5);
    // Jump to previous prompt: view top should land on a marked row
    g.scroll_to_prev_prompt();
    int off1 = g.get_scroll_offset();
    CHECK(off1 > 0);
    g.scroll_to_prev_prompt();
    int off2 = g.get_scroll_offset();
    CHECK(off2 > off1); // earlier prompt is further back
    // And forward again
    g.scroll_to_next_prompt();
    CHECK(g.get_scroll_offset() == off1);
    g.scroll_to_next_prompt();
    CHECK(g.get_scroll_offset() == 0); // rejoins live view
}

static void test_utf8() {
    TerminalGrid g; g.resize(10, 3);
    ANSIParser p;
    feed(p, g, "\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80"); // é € 😀
    CHECK(g.get_cell_at(0, 0).codepoint == 0x00E9);
    CHECK(g.get_cell_at(1, 0).codepoint == 0x20AC);
    CHECK(g.get_cell_at(2, 0).codepoint == 0x1F600);
}

// Selection and search highlighting are resolved per row by render(), which
// keeps a per-cell entry point (is_cell_selected / is_cell_search_matched)
// only so the row-at-a-time and cell-at-a-time answers can be compared. These
// pin the cell-level contract those share.
static void test_selection_spans() {
    TerminalGrid g; g.resize(10, 4);
    ANSIParser p;
    feed(p, g, "abcdefghij\r\nklmnopqrst\r\nuvwxyzABCD");

    // Single row, mid-row to mid-row: inclusive at both ends.
    g.start_selection(2, 0);
    g.update_selection(5, 0);
    g.end_selection();
    CHECK(!g.is_cell_selected(1, 0));
    CHECK(g.is_cell_selected(2, 0));
    CHECK(g.is_cell_selected(5, 0));
    CHECK(!g.is_cell_selected(6, 0));
    CHECK(!g.is_cell_selected(3, 1));

    // Multi-row: the first row runs from the anchor to the end, interior rows
    // are selected end to end, and the last row stops at the release column.
    g.start_selection(7, 0);
    g.update_selection(3, 2);
    g.end_selection();
    CHECK(!g.is_cell_selected(6, 0));
    CHECK(g.is_cell_selected(7, 0));
    CHECK(g.is_cell_selected(9, 0));
    CHECK(g.is_cell_selected(0, 1));
    CHECK(g.is_cell_selected(9, 1));
    CHECK(g.is_cell_selected(0, 2));
    CHECK(g.is_cell_selected(3, 2));
    CHECK(!g.is_cell_selected(4, 2));

    // Dragging upwards selects the same cells as dragging downwards.
    g.start_selection(3, 2);
    g.update_selection(7, 0);
    g.end_selection();
    CHECK(g.is_cell_selected(7, 0));
    CHECK(!g.is_cell_selected(6, 0));
    CHECK(g.is_cell_selected(3, 2));
    CHECK(!g.is_cell_selected(4, 2));

    g.clear_selection();
    CHECK(!g.is_cell_selected(7, 0));
}

static void test_search_match_spans() {
    TerminalGrid g; g.resize(20, 3);
    ANSIParser p;
    // Three rows scrolled into history plus three on screen, so matches are
    // looked up across the history/active boundary rather than only in one.
    feed(p, g, "needle one\r\nhaystack\r\nneedle two\r\nhaystack\r\nhaystack\r\nneedle three");

    g.set_search_query("needle");
    g.set_search_active(true);
    CHECK(g.get_search_match_count() == 3);

    // Bottom row of the live view holds the third match.
    CHECK(g.is_cell_search_matched(0, 2));
    CHECK(g.is_cell_search_matched(5, 2));
    CHECK(!g.is_cell_search_matched(6, 2));
    CHECK(!g.is_cell_search_matched(0, 1));

    // Scroll far enough back that the first two matches are on screen.
    g.scroll_view(3);
    CHECK(g.is_cell_search_matched(0, 0));
    CHECK(g.is_cell_search_matched(5, 0));
    CHECK(!g.is_cell_search_matched(6, 0));
    CHECK(!g.is_cell_search_matched(0, 1)); // "haystack"
    CHECK(g.is_cell_search_matched(0, 2));

    g.set_search_active(false);
    CHECK(!g.is_cell_search_matched(0, 0));
}

// The row above the top of the visible view: render() reads it to fill the
// sliver a part-row smooth-scroll shift uncovers. Scrolled all the way back
// there is no such row, and asking for it used to index the scrollback deque
// one before its front.
static void test_read_above_top_of_history() {
    TerminalGrid g; g.resize(10, 3);
    ANSIParser p;
    feed(p, g, "one\r\ntwo\r\nthree\r\nfour\r\nfive");

    g.scroll_view(1000); // clamps to the full history
    CHECK(g.get_scroll_offset() == static_cast<int>(g.get_scrollback_size()));
    CHECK(row_text(g, 0) == "one");
    // Blank, not a crash and not the newest line wrapped around.
    CHECK(g.get_cell_at(0, -1).codepoint == 32);
    CHECK(!g.is_cell_selected(0, -1));
    CHECK(!g.is_cell_search_matched(0, -1));
}

// Search matching walks the cells directly rather than a re-encoded UTF-8
// copy of each row, so the cases that used to be handled by a byte-offset ->
// column map are worth pinning: case folding, double-width characters
// occupying two columns, and the partial UTF-8 a backspace leaves behind.
static void test_search_matching() {
    TerminalGrid g; g.resize(20, 3);
    ANSIParser p;
    // The \xe4\xb8\x96 escape is split from the following 'b' so the compiler
    // does not read it as one over-long hex escape.
    feed(p, g, "aXbc\r\na\xe4\xb8\x96" "b\r\nHELLO hello");

    g.set_search_active(true);

    // ASCII matching is case-insensitive in both directions.
    g.set_search_query("hello");
    CHECK(g.get_search_match_count() == 2);
    CHECK(g.is_cell_search_matched(0, 2));
    CHECK(g.is_cell_search_matched(4, 2));
    CHECK(!g.is_cell_search_matched(5, 2)); // the space between them
    CHECK(g.is_cell_search_matched(6, 2));
    CHECK(g.is_cell_search_matched(10, 2));
    CHECK(!g.is_cell_search_matched(11, 2));

    // A double-width character is highlighted across both of its columns,
    // not just the one holding the codepoint.
    g.set_search_query("\xe4\xb8\x96");
    CHECK(g.get_search_match_count() == 1);
    CHECK(!g.is_cell_search_matched(0, 1));
    CHECK(g.is_cell_search_matched(1, 1));
    CHECK(g.is_cell_search_matched(2, 1)); // trailing half of the pair
    CHECK(!g.is_cell_search_matched(3, 1));

    // A match running through a wide character and out the other side.
    g.set_search_query("\xe4\xb8\x96" "b");
    CHECK(g.get_search_match_count() == 1);
    CHECK(!g.is_cell_search_matched(0, 1));
    CHECK(g.is_cell_search_matched(1, 1));
    CHECK(g.is_cell_search_matched(2, 1));
    CHECK(g.is_cell_search_matched(3, 1));
    CHECK(!g.is_cell_search_matched(4, 1));

    g.set_search_query("zzz");
    CHECK(g.get_search_match_count() == 0);

    // Half a character, which is what the find bar holds mid-backspace.
    g.set_search_query("\xe4\xb8");
    CHECK(g.get_search_match_count() == 0);

    // Matches do not overlap: "aa" in "aaaa" is two matches, not three.
    TerminalGrid g2; g2.resize(10, 1);
    ANSIParser p2;
    feed(p2, g2, "aaaa");
    g2.set_search_active(true);
    g2.set_search_query("aa");
    CHECK(g2.get_search_match_count() == 2);
}

// DSR and DA are the sequences a terminal must answer rather than merely
// honour: the program that sends one blocks until the reply arrives.
static void test_dsr_and_da() {
    TerminalGrid g; g.resize(80, 24);
    ANSIParser p;

    // No reply until something asks for one.
    CHECK(!g.has_pending_reply());

    // CPR is 1-based on the wire, so the home position reports as 1;1.
    feed(p, g, "\x1b[6n");
    CHECK(g.take_pending_reply() == "\x1b[1;1R");
    CHECK(!g.has_pending_reply());

    feed(p, g, "\x1b[10;20H\x1b[6n");
    CHECK(g.take_pending_reply() == "\x1b[10;20R");

    // DSR 5 is "are you there"; 0n means no malfunction.
    feed(p, g, "\x1b[5n");
    CHECK(g.take_pending_reply() == "\x1b[0n");

    // DECXCPR adds the private marker and a page number.
    feed(p, g, "\x1b[1;1H\x1b[?6n");
    CHECK(g.take_pending_reply() == "\x1b[?1;1;1R");

    // Under origin mode the row is relative to the top margin, matching the
    // number CUP would need to put the cursor back.
    feed(p, g, "\x1b[5;20r\x1b[?6h\x1b[3;1H\x1b[6n");
    CHECK(g.take_pending_reply() == "\x1b[3;1R");
    feed(p, g, "\x1b[?6l\x1b[r");

    // Primary DA: VT220 class with ANSI colour. "CSI c" and "CSI 0 c" are
    // the same request.
    feed(p, g, "\x1b[c");
    CHECK(g.take_pending_reply() == "\x1b[?62;22c");
    feed(p, g, "\x1b[0c");
    CHECK(g.take_pending_reply() == "\x1b[?62;22c");

    // Secondary DA is a different request sharing the same final byte, and
    // used to be indistinguishable from the primary one. The version it
    // carries comes from the build rather than being written out here, so a
    // release bump does not have to remember this test.
    feed(p, g, "\x1b[>c");
    CHECK(g.take_pending_reply() ==
          "\x1b[>0;" + std::to_string(SINK_VERSION_NUM) + ";0c");

    // A parameter the terminal does not recognise gets no answer rather than
    // a wrong one.
    feed(p, g, "\x1b[3c");
    CHECK(!g.has_pending_reply());
    feed(p, g, "\x1b[?15n"); // DECDSR printer status
    CHECK(!g.has_pending_reply());

    // Untrusted input cannot queue replies without bound.
    for (int i = 0; i < 5000; ++i) feed(p, g, "\x1b[6n");
    CHECK(g.take_pending_reply().size() <= 4096);
}

// ICH is the mirror of DCH, and the pair is how readline and editors edit
// mid-line without repainting the tail.
static void test_ich() {
    TerminalGrid g; g.resize(10, 2);
    ANSIParser p;
    feed(p, g, "abcdef");

    // Two blanks opened at column 2; the tail shifts right, cursor stays.
    feed(p, g, "\x1b[1;3H\x1b[2@");
    CHECK(row_text(g, 0) == "ab  cdef");
    CHECK(g.get_cursor_col() == 2);

    // Omitted parameter means 1, and an explicit 0 means 1 too.
    feed(p, g, "\x1b[1;1H\x1b[@");
    CHECK(row_text(g, 0) == " ab  cdef");
    feed(p, g, "\x1b[1;1H\x1b[0@");
    CHECK(row_text(g, 0) == "  ab  cdef");

    // Characters pushed past the right edge are lost, not wrapped.
    TerminalGrid g2; g2.resize(6, 2);
    ANSIParser p2;
    feed(p2, g2, "abcdef\x1b[1;1H\x1b[2@");
    CHECK(row_text(g2, 0) == "  abcd");
    CHECK(row_text(g2, 1) == "");

    // A count past the end of the row blanks the rest of it rather than
    // running off the end.
    feed(p2, g2, "\x1b[1;3H\x1b[99@");
    CHECK(row_text(g2, 0) == "");

    // ICH and DCH undo each other.
    TerminalGrid g3; g3.resize(10, 1);
    ANSIParser p3;
    feed(p3, g3, "hello\x1b[1;3H\x1b[3@\x1b[3P");
    CHECK(row_text(g3, 0) == "hello");
}

// DECSCUSR shares its final byte with DECLL and is told apart only by the
// space intermediate, which the CSI parser used to discard.
static void test_decscusr() {
    TerminalGrid g; g.resize(10, 2);
    ANSIParser p;
    CHECK(g.get_cursor_shape() == CursorShape::Block);

    feed(p, g, "\x1b[5 q");
    CHECK(g.get_cursor_shape() == CursorShape::Bar);
    feed(p, g, "\x1b[3 q");
    CHECK(g.get_cursor_shape() == CursorShape::Underline);
    feed(p, g, "\x1b[2 q");
    CHECK(g.get_cursor_shape() == CursorShape::Block);

    // Blink and steady select the same shape; the blink bit is dropped.
    feed(p, g, "\x1b[6 q");
    CHECK(g.get_cursor_shape() == CursorShape::Bar);
    feed(p, g, "\x1b[4 q");
    CHECK(g.get_cursor_shape() == CursorShape::Underline);

    // 0 is "default", which is a block.
    feed(p, g, "\x1b[0 q");
    CHECK(g.get_cursor_shape() == CursorShape::Block);

    // Without the space intermediate this is DECLL, not DECSCUSR, and must
    // not move the cursor shape.
    feed(p, g, "\x1b[5 q");
    CHECK(g.get_cursor_shape() == CursorShape::Bar);
    feed(p, g, "\x1b[2q");
    CHECK(g.get_cursor_shape() == CursorShape::Bar);

    // RIS returns it to a block along with everything else.
    feed(p, g, "\x1b" "c"); // split so 'c' is not read into the hex escape
    CHECK(g.get_cursor_shape() == CursorShape::Block);
}

// CNL/CPL differ from CUD/CUU only in also returning to column 1, which is
// the whole reason programs use them.
static void test_cnl_cpl() {
    TerminalGrid g; g.resize(20, 10);
    ANSIParser p;

    feed(p, g, "\x1b[5;10H\x1b[2E");
    CHECK(g.get_cursor_row() == 6);
    CHECK(g.get_cursor_col() == 0);

    feed(p, g, "\x1b[5;10H\x1b[2F");
    CHECK(g.get_cursor_row() == 2);
    CHECK(g.get_cursor_col() == 0);

    // Omitted and explicit-zero parameters both mean one row.
    feed(p, g, "\x1b[5;10H\x1b[E");
    CHECK(g.get_cursor_row() == 5 && g.get_cursor_col() == 0);
    feed(p, g, "\x1b[5;10H\x1b[0F");
    CHECK(g.get_cursor_row() == 3 && g.get_cursor_col() == 0);

    // Clamped at the edges of the screen, still landing in column 1.
    feed(p, g, "\x1b[1;10H\x1b[99F");
    CHECK(g.get_cursor_row() == 0 && g.get_cursor_col() == 0);
    feed(p, g, "\x1b[1;10H\x1b[99E");
    CHECK(g.get_cursor_row() == 9 && g.get_cursor_col() == 0);
}

// Tab stops were arithmetic ((col + 8) & ~7) with nowhere to record a change,
// so HTS and TBC could not exist and a program that moved its stops silently
// kept the default ones.
static void test_tab_stops() {
    TerminalGrid g; g.resize(40, 3);
    ANSIParser p;

    // Defaults are every 8th column, and HT lands on them.
    CHECK(!g.is_tab_stop(0));
    CHECK(g.is_tab_stop(8));
    CHECK(g.is_tab_stop(16));
    feed(p, g, "\x1b[1;1H\t");
    CHECK(g.get_cursor_col() == 8);
    feed(p, g, "\t");
    CHECK(g.get_cursor_col() == 16);

    // Starting *on* a stop still advances to the next one.
    feed(p, g, "\x1b[1;9H\t");
    CHECK(g.get_cursor_col() == 16);

    // CHT walks several stops at once; CBT walks back.
    feed(p, g, "\x1b[1;1H\x1b[3I");
    CHECK(g.get_cursor_col() == 24);
    feed(p, g, "\x1b[2Z");
    CHECK(g.get_cursor_col() == 8);

    // Past the last stop, HT stops at the right margin rather than wrapping.
    feed(p, g, "\x1b[1;38H\t");
    CHECK(g.get_cursor_col() == 39);
    // Past the first, CBT stops at column 1.
    feed(p, g, "\x1b[1;3H\x1b[9Z");
    CHECK(g.get_cursor_col() == 0);

    // TBC 3 clears every stop; HT then runs to the right margin.
    feed(p, g, "\x1b[3g");
    CHECK(!g.is_tab_stop(8));
    feed(p, g, "\x1b[1;1H\t");
    CHECK(g.get_cursor_col() == 39);

    // HTS sets a stop at the cursor, and TBC 0 clears just that one.
    feed(p, g, "\x1b[1;5H\x1bH");
    CHECK(g.is_tab_stop(4));
    feed(p, g, "\x1b[1;1H\t");
    CHECK(g.get_cursor_col() == 4);
    feed(p, g, "\x1b[1;5H\x1b[0g");
    CHECK(!g.is_tab_stop(4));

    // RIS restores the default stops.
    feed(p, g, "\x1b" "c");
    CHECK(g.is_tab_stop(8) && g.is_tab_stop(16));

    // Custom stops survive a resize; newly exposed columns get defaults.
    feed(p, g, "\x1b[3g\x1b[1;5H\x1bH");
    g.resize(60, 3);
    CHECK(g.is_tab_stop(4));   // kept
    CHECK(!g.is_tab_stop(8));  // still cleared
    CHECK(g.is_tab_stop(40));  // new column, default pattern
}

// REP repeats the last graphic character, which means the parser has to
// remember it across both the per-character path and the batched ASCII run.
static void test_rep() {
    TerminalGrid g; g.resize(20, 3);
    ANSIParser p;

    feed(p, g, "a\x1b[4b");
    CHECK(row_text(g, 0) == "aaaaa");

    // The run fast path has to record the *last* character of the run.
    feed(p, g, "\x1b[2;1Hxyz\x1b[2b");
    CHECK(row_text(g, 1) == "xyzzz");

    // Omitted and explicit-zero counts both repeat once.
    feed(p, g, "\x1b[3;1HQ\x1b[b");
    CHECK(row_text(g, 2) == "QQ");
    feed(p, g, "\x1b[3;1HR\x1b[0b");
    CHECK(row_text(g, 2) == "RR"); // overwrites both cells the line already had

    // Nothing to repeat yet: REP must not invent a character.
    TerminalGrid g2; g2.resize(10, 1);
    ANSIParser p2;
    feed(p2, g2, "\x1b[5b");
    CHECK(row_text(g2, 0) == "");

    // A non-ASCII character is remembered too, and repeats as itself.
    TerminalGrid g3; g3.resize(10, 1);
    ANSIParser p3;
    feed(p3, g3, "\xc3\xa9" "\x1b[2b");
    CHECK(g3.get_cell_at(0, 0).codepoint == 0x00E9);
    CHECK(g3.get_cell_at(1, 0).codepoint == 0x00E9);
    CHECK(g3.get_cell_at(2, 0).codepoint == 0x00E9);

    // An absurd count is clamped rather than spinning in the parser.
    TerminalGrid g4; g4.resize(10, 2);
    ANSIParser p4;
    feed(p4, g4, "z\x1b[2000000000b");
    CHECK(g4.get_cols() == 10); // completed at all, promptly
}

// DECSTR restores modes without disturbing the screen, which is the whole
// difference between it and RIS.
static void test_decstr() {
    TerminalGrid g; g.resize(20, 10);
    ANSIParser p;

    feed(p, g, "hello\r\nworld");
    feed(p, g, "\x1b[3;8r\x1b[?6h\x1b[?25l\x1b[5 q\x1b[1;31m\x1b(0");
    CHECK(g.is_origin_mode());
    CHECK(!g.is_cursor_visible());
    CHECK(g.get_scroll_top() == 2);
    CHECK(g.get_cursor_shape() == CursorShape::Bar);

    feed(p, g, "\x1b[!p");

    // Modes are back to defaults.
    CHECK(!g.is_origin_mode());
    CHECK(g.is_cursor_visible());
    CHECK(g.get_scroll_top() == 0);
    CHECK(g.get_scroll_bottom() == 9);
    CHECK(g.get_cursor_shape() == CursorShape::Block);
    CHECK(g.get_current_attrs() == 0);

    // ...and the screen is untouched, unlike RIS.
    CHECK(row_text(g, 0) == "hello");
    CHECK(row_text(g, 1) == "world");

    // DEC line drawing is cancelled: 'q' is a horizontal line while it is on
    // and a plain letter afterwards.
    feed(p, g, "\x1b[5;1Hq");
    CHECK(g.get_cell_at(0, 4).codepoint == U'q');

    // Without the '!' intermediate this is not DECSTR and must do nothing.
    feed(p, g, "\x1b[?25l\x1b[p");
    CHECK(!g.is_cursor_visible());
    feed(p, g, "\x1b[!p");
    CHECK(g.is_cursor_visible());

    // RIS, by contrast, does clear the screen.
    feed(p, g, "\x1b" "c");
    CHECK(row_text(g, 0) == "");
}

// XTWINOPS: geometry reports are answered, window manipulation is not.
static void test_xtwinops() {
    TerminalGrid g; g.resize(80, 24);
    ANSIParser p;

    // Text area in characters, rows before columns.
    feed(p, g, "\x1b[18t");
    CHECK(g.take_pending_reply() == "\x1b[8;24;80t");

    // Pixel reports stay silent until the layout has said how big a cell is.
    feed(p, g, "\x1b[14t");
    CHECK(!g.has_pending_reply());
    feed(p, g, "\x1b[16t");
    CHECK(!g.has_pending_reply());

    g.set_cell_pixel_size(9, 18);
    feed(p, g, "\x1b[16t");
    CHECK(g.take_pending_reply() == "\x1b[6;18;9t");
    feed(p, g, "\x1b[14t");
    CHECK(g.take_pending_reply() == "\x1b[4;432;720t"); // 24*18, 80*9

    // Window manipulation is not implemented and must stay silent rather
    // than letting output move the user's window.
    for (const char* seq : {"\x1b[1t", "\x1b[2t", "\x1b[3;0;0t", "\x1b[4;100;100t",
                            "\x1b[5t", "\x1b[9;1t", "\x1b[10;1t"}) {
        feed(p, g, seq);
        CHECK(!g.has_pending_reply());
    }

    // Title read-back is refused, like OSC 52 read-back.
    feed(p, g, "\x1b[21t");
    CHECK(!g.has_pending_reply());

    // Screen size is answered only from a real display measurement. Until the
    // layout supplies one it stays silent rather than reporting the window.
    feed(p, g, "\x1b[19t");
    CHECK(!g.has_pending_reply());
    g.set_screen_size_chars(240, 67);
    feed(p, g, "\x1b[19t");
    CHECK(g.take_pending_reply() == "\x1b[9;67;240t");
    // ...and it stays distinct from the text area, which is the point of it.
    feed(p, g, "\x1b[18t");
    CHECK(g.take_pending_reply() == "\x1b[8;24;80t");
}

// DECRQM lets a program ask whether a mode is set instead of setting it and
// hoping. The case that motivated it: sink implements synchronized output,
// and this is how anything finds that out.
static void test_decrqm() {
    TerminalGrid g; g.resize(40, 10);
    ANSIParser p;

    // 2 = reset, 1 = set.
    feed(p, g, "\x1b[?2026$p");
    CHECK(g.take_pending_reply() == "\x1b[?2026;2$y");
    feed(p, g, "\x1b[?2026h\x1b[?2026$p");
    CHECK(g.take_pending_reply() == "\x1b[?2026;1$y");
    feed(p, g, "\x1b[?2026l");

    // A mode toggled through its own sequence reports the new state.
    feed(p, g, "\x1b[?25$p");
    CHECK(g.take_pending_reply() == "\x1b[?25;1$y");   // cursor visible by default
    feed(p, g, "\x1b[?25l\x1b[?25$p");
    CHECK(g.take_pending_reply() == "\x1b[?25;2$y");
    feed(p, g, "\x1b[?25h");

    feed(p, g, "\x1b[?2004h\x1b[?2004$p");
    CHECK(g.take_pending_reply() == "\x1b[?2004;1$y");

    // Mouse modes are mutually exclusive, so only the active one reports set.
    feed(p, g, "\x1b[?1002h");
    feed(p, g, "\x1b[?1002$p");
    CHECK(g.take_pending_reply() == "\x1b[?1002;1$y");
    feed(p, g, "\x1b[?1000$p");
    CHECK(g.take_pending_reply() == "\x1b[?1000;2$y");

    // Alt screen, reported through any of its three mode numbers.
    feed(p, g, "\x1b[?1049h\x1b[?1049$p");
    CHECK(g.take_pending_reply() == "\x1b[?1049;1$y");
    feed(p, g, "\x1b[?47$p");
    CHECK(g.take_pending_reply() == "\x1b[?47;1$y");
    feed(p, g, "\x1b[?1049l");

    // 3 = permanently set: sink always wraps and cannot be told not to.
    feed(p, g, "\x1b[?7$p");
    CHECK(g.take_pending_reply() == "\x1b[?7;3$y");

    // 4 = permanently reset, for ANSI modes sink does not implement.
    feed(p, g, "\x1b[4$p");
    CHECK(g.take_pending_reply() == "\x1b[4;4$y");     // IRM, note no '?'
    feed(p, g, "\x1b[20$p");
    CHECK(g.take_pending_reply() == "\x1b[20;4$y");    // LNM

    // 0 = not recognised, which is the answer that makes a program use its
    // fallback rather than trust a guess.
    feed(p, g, "\x1b[?12345$p");
    CHECK(g.take_pending_reply() == "\x1b[?12345;0$y");
    feed(p, g, "\x1b[?1048$p");                        // an action, not a state
    CHECK(g.take_pending_reply() == "\x1b[?1048;0$y");
    feed(p, g, "\x1b[99$p");
    CHECK(g.take_pending_reply() == "\x1b[99;0$y");

    // Without the '$' intermediate this is not DECRQM. CSI ? Ps p must not
    // answer, and CSI ! p is still DECSTR.
    feed(p, g, "\x1b[?2026p");
    CHECK(!g.has_pending_reply());
    feed(p, g, "\x1b[?25l\x1b[!p");
    CHECK(g.is_cursor_visible());
    CHECK(!g.has_pending_reply());
}

// Kitty keyboard protocol negotiation. The key *encoding* lives in main.cpp
// against SDL events and is not reachable from here; what is testable -- and
// what an application actually depends on -- is the flags handshake.
static void test_kitty_keyboard_flags() {
    TerminalGrid g; g.resize(40, 10);
    ANSIParser p;

    // Nothing negotiated to begin with.
    feed(p, g, "\x1b[?u");
    CHECK(g.take_pending_reply() == "\x1b[?0u");

    // CSI = flags ; 1 u assigns.
    feed(p, g, "\x1b[=1;1u\x1b[?u");
    CHECK(g.take_pending_reply() == "\x1b[?1u");
    feed(p, g, "\x1b[=3;1u\x1b[?u");
    CHECK(g.take_pending_reply() == "\x1b[?3u");

    // Mode 3 clears the given bits, mode 2 sets them.
    feed(p, g, "\x1b[=2;3u\x1b[?u");
    CHECK(g.take_pending_reply() == "\x1b[?1u");
    feed(p, g, "\x1b[=2;2u\x1b[?u");
    CHECK(g.take_pending_reply() == "\x1b[?3u");

    // Unsupported flags are masked out, so the reply describes what will
    // really happen rather than what was asked for. 4, 8 and 16 are not
    // implemented; asking for everything gets back only 1|2.
    feed(p, g, "\x1b[=31;1u\x1b[?u");
    CHECK(g.take_pending_reply() == "\x1b[?3u");

    // Push and pop: an app enters its mode and leaves it without having to
    // know what the shell underneath had set.
    feed(p, g, "\x1b[=1;1u");
    feed(p, g, "\x1b[>3u\x1b[?u");
    CHECK(g.take_pending_reply() == "\x1b[?3u");
    feed(p, g, "\x1b[<u\x1b[?u");
    CHECK(g.take_pending_reply() == "\x1b[?1u"); // back to what was underneath

    // Popping more than was pushed bottoms out at the base entry rather than
    // running off the stack. That entry keeps whatever was last assigned to
    // it -- a pop returns to the level below, it does not undo an assignment
    // made at the bottom, which is what CSI = 0 ; 1 u is for.
    feed(p, g, "\x1b[<99u\x1b[?u");
    CHECK(g.take_pending_reply() == "\x1b[?1u");
    feed(p, g, "\x1b[=0;1u\x1b[?u");
    CHECK(g.take_pending_reply() == "\x1b[?0u");

    // A program that pushes without ever popping degrades instead of wedging:
    // the stack is capped and evicts its oldest entry, so it stays bounded and
    // stays poppable. Enough pushes will evict the base entry, which is why
    // this ends at 1 rather than 0 -- bounded and recoverable, not stuck.
    for (int i = 0; i < 100; ++i) feed(p, g, "\x1b[>1u");
    feed(p, g, "\x1b[?u");
    CHECK(g.take_pending_reply() == "\x1b[?1u");
    feed(p, g, "\x1b[<99u\x1b[?u");
    CHECK(g.take_pending_reply() == "\x1b[?1u");
    feed(p, g, "\x1b[=0;1u\x1b[?u");
    CHECK(g.take_pending_reply() == "\x1b[?0u");

    // RIS clears the negotiation along with everything else.
    feed(p, g, "\x1b[=3;1u");
    feed(p, g, "\x1b" "c");
    feed(p, g, "\x1b[?u");
    CHECK(g.take_pending_reply() == "\x1b[?0u");

    // Without a private marker this is still ANSI.SYS restore-cursor, which
    // shares the final byte.
    feed(p, g, "\x1b[5;7H\x1b[s\x1b[1;1H\x1b[u");
    CHECK(g.get_cursor_row() == 4 && g.get_cursor_col() == 6);
    CHECK(!g.has_pending_reply());
}

int main() {
    test_plain_text();
    test_crlf_and_scroll();
    test_ris_full_reset();
    test_combining_marks();
    test_search_column_mapping();
    test_synchronized_output_and_focus_modes();
    test_wide_characters();
    test_wide_character_wrap();
    test_wide_character_copy();
    test_error_flash_trigger();
    test_scroll_ring_wraparound();
    test_scrollback_cap_recycles_rows();
    test_cup_and_relative_motion();
    test_sgr_16_color();
    test_sgr_256_color();
    test_sgr_256_colon_form();
    test_sgr_truecolor();
    test_decom_origin_mode();
    test_decstbm_basic_scroll();
    test_decstbm_reset();
    test_reverse_index();
    test_insert_delete_lines();
    test_su_sd();
    test_wrap_within_region();
    test_modes();
    test_sgr_attributes();
    test_bold_as_bright();
    test_alt_screen_buffer();
    test_alt_screen_ed_clear();
    test_mouse_modes();
    test_cursor_key_and_scroll_modes();
    test_xtsave_does_not_clobber_cursor();
    test_osc_consumed();
    test_ed_el();
    test_osc_title();
    test_osc_52_clipboard();
    test_osc_8_hyperlinks();
    test_osc_133_prompt_marks();
    test_utf8();
    test_selection_spans();
    test_search_match_spans();
    test_read_above_top_of_history();
    test_search_matching();
    test_dsr_and_da();
    test_ich();
    test_decscusr();
    test_cnl_cpl();
    test_tab_stops();
    test_rep();
    test_decstr();
    test_xtwinops();
    test_decrqm();
    test_kitty_keyboard_flags();

    std::printf("%d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
