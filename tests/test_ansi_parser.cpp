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

    // Marks attach to a double-width base without disturbing its pair. There
    // is no precomposed form here, so the cell becomes a cluster -- it used to
    // drop the mark entirely.
    TerminalGrid g3; g3.resize(20, 4);
    ANSIParser p3;
    feed(p3, g3, "\xe4\xbd\xa0\xcc\x81");        // CJK then a mark
    {
        // cell_string, not cell_text: get_cell_at returns by value, and
        // cell_text hands back a pointer into the Cell for an ordinary one.
        std::u32string t = g3.cell_string(g3.get_cell_at(0, 0));
        CHECK(t.size() == 2);
        CHECK(t[0] == 0x4F60);
        CHECK(t[1] == 0x0301);
    }
    CHECK(g3.cell_base(g3.get_cell_at(0, 0)) == 0x4F60);
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

// Grapheme clusters: several codepoints that form one visible character. A
// cell holds one codepoint, so these used to be dropped (marks with no
// precomposed form, variation selectors) or given a cell each (ZWJ sequences).
static void test_grapheme_clusters() {
    auto cluster_of = [](const std::string& bytes, int col = 0) {
        TerminalGrid g; g.resize(20, 2);
        ANSIParser p;
        feed(p, g, bytes);
        return g.cell_string(g.get_cell_at(col, 0));
    };
    auto cursor_after = [](const std::string& bytes) {
        TerminalGrid g; g.resize(20, 2);
        ANSIParser p;
        feed(p, g, bytes);
        return g.get_cursor_col();
    };

    const std::string man   = "\xf0\x9f\x91\xa8";
    const std::string woman = "\xf0\x9f\x91\xa9";
    const std::string girl  = "\xf0\x9f\x91\xa7";
    const std::string boy   = "\xf0\x9f\x91\xa6";
    const std::string zwj   = "\xe2\x80\x8d";

    // A family emoji is one character occupying two columns, not four
    // characters occupying eight.
    const std::string family = man + zwj + woman + zwj + girl + zwj + boy;
    CHECK(cursor_after(family) == 2);
    {
        std::u32string t = cluster_of(family);
        CHECK(t.size() == 7);
        CHECK(t[0] == 0x1F468);
        CHECK(t[1] == 0x200D);
        CHECK(t[6] == 0x1F466);
    }

    // A precomposed form still collapses to a single codepoint rather than
    // becoming a cluster -- 'e' + U+0301 is U+00E9, as it always was.
    {
        std::u32string t = cluster_of("e\xcc\x81");
        CHECK(t.size() == 1);
        CHECK(t[0] == 0x00E9);
    }

    // One with no precomposed form is kept as a cluster instead of dropped.
    {
        std::u32string t = cluster_of("a\xcd\x88");
        CHECK(t.size() == 2);
        CHECK(t[0] == U'a');
        CHECK(t[1] == 0x0348);
    }

    // A variation selector selects emoji presentation and must survive.
    {
        std::u32string t = cluster_of("\xe2\x9d\xa4\xef\xb8\x8f");
        CHECK(t.size() == 2);
        CHECK(t[1] == 0xFE0F);
    }
    CHECK(cursor_after("\xe2\x9d\xa4\xef\xb8\x8f") == 1);

    // ZWNJ joins the cluster too, but unlike ZWJ does not pull in the
    // character after it.
    {
        std::u32string t = cluster_of("a\xe2\x80\x8c" "b");
        CHECK(t.size() == 2);
        CHECK(t[1] == 0x200C);
    }
    CHECK(cursor_after("a\xe2\x80\x8c" "b") == 2);

    // Zero-width characters that are not part of a cluster stay dropped.
    for (const char* zw : {"\xe2\x80\x8b", "\xe2\x81\xa0", "\xef\xbb\xbf"}) {
        std::u32string t = cluster_of(std::string("a") + zw);
        CHECK(t.size() == 1);
        CHECK(t[0] == U'a');
    }

    // A cluster cannot grow without bound off a stream of marks.
    {
        std::string many = "a";
        for (int i = 0; i < 200; ++i) many += "\xcd\x88";
        CHECK(cluster_of(many).size() <= 32);
    }

    // Copying a cluster cell gives back the codepoints that made it, so a
    // round trip through the clipboard preserves the character.
    {
        TerminalGrid g; g.resize(20, 2);
        ANSIParser p;
        feed(p, g, family);
        g.start_selection(0, 0);
        g.update_selection(1, 0);
        g.end_selection();
        CHECK(g.get_selected_text() == family);
    }

    // The cell after a cluster is unaffected, and a wide cluster still marks
    // its second column as the trailing half of the pair.
    {
        TerminalGrid g; g.resize(20, 2);
        ANSIParser p;
        feed(p, g, family + "X");
        CHECK((g.get_cell_at(1, 0).attrs & ATTR_WIDE_CONT) != 0);
        CHECK(g.cell_base(g.get_cell_at(2, 0)) == U'X');
    }
}

// The image model both graphics protocols decode into. What matters here is
// that a placement stays attached to its text: pinned to a screen row it would
// slide up the screen as output scrolled, which is the bug this shape exists
// to avoid.
static void test_image_placements() {
    TerminalGrid g; g.resize(20, 5);
    g.set_max_scrollback(10);
    ANSIParser p;
    TerminalImages& im = g.images();

    std::vector<uint32_t> px(4 * 4, 0xFFFFFFFFu);
    uint64_t id = im.store(0, 4, 4, std::move(px));
    CHECK(id != 0);
    CHECK(im.image_count() == 1);

    // Placed on the cursor's line, whatever row that currently is.
    ImagePlacement pl;
    pl.image_id = id;
    pl.line_id = g.line_id_for_row(g.get_cursor_row());
    pl.col = 0; pl.cols = 2; pl.rows = 2;
    im.place(pl);
    CHECK(im.placements().size() == 1);
    uint64_t pinned = im.placements()[0].line_id;

    // Scrolling does not move it: the line id is the same line it always was,
    // even though that text is now several rows higher.
    feed(p, g, "\r\n\r\n\r\n");
    CHECK(im.placements().size() == 1);
    CHECK(im.placements()[0].line_id == pinned);

    // Once the line falls out of scrollback the placement goes with it, and
    // the image it was the last reference to is freed.
    for (int i = 0; i < 40; ++i) feed(p, g, "x\r\n");
    CHECK(g.oldest_line_id() > pinned);
    CHECK(im.placements().empty());
    CHECK(im.image_count() == 0);
    CHECK(im.total_bytes() == 0);

    // A placement id names a slot, so re-sending it moves the image rather
    // than stacking another copy.
    std::vector<uint32_t> px2(4 * 4, 0xFF00FF00u);
    uint64_t id2 = im.store(7, 4, 4, std::move(px2));
    CHECK(id2 == 7);
    ImagePlacement a; a.image_id = 7; a.placement_id = 1;
    a.line_id = g.line_id_for_row(0); a.col = 0; a.cols = 1; a.rows = 1;
    im.place(a);
    a.col = 5;
    im.place(a);
    CHECK(im.placements().size() == 1);
    CHECK(im.placements()[0].col == 5);

    // Without one, two placements of the same image coexist.
    ImagePlacement b = a; b.placement_id = 0; b.col = 9;
    im.place(b);
    CHECK(im.placements().size() == 2);

    // Deleting the image takes its placements with it.
    im.delete_image(7);
    CHECK(im.placements().empty());
    CHECK(im.image_count() == 0);

    // Rejections: zero-sized, and larger than the per-image cap.
    CHECK(im.store(0, 0, 4, std::vector<uint32_t>(4)) == 0);
    CHECK(im.store(0, 100000, 100000, std::vector<uint32_t>(16)) == 0);

    // Erasing the screen drops what was drawn on it.
    std::vector<uint32_t> px3(4 * 4, 0xFF0000FFu);
    uint64_t id3 = im.store(0, 4, 4, std::move(px3));
    ImagePlacement c; c.image_id = id3; c.line_id = g.line_id_for_row(0);
    c.col = 0; c.cols = 1; c.rows = 1;
    im.place(c);
    CHECK(im.placements().size() == 1);
    feed(p, g, "\x1b[2J");
    CHECK(im.placements().empty());
}

// Sixel: DCS <params> q <data> ST. A sixel character encodes six vertically
// stacked pixels, bit 0 topmost; '-' starts the next band, '$' returns to the
// left margin, '#' selects or defines a colour, '!' repeats.
static void test_sixel() {
    // A 2x6 block: two sixel characters with every bit set.
    {
        TerminalGrid g; g.resize(80, 24);
        g.set_cell_pixel_size(10, 20);
        ANSIParser p;
        feed(p, g, "\x1bP0;0;0q#0;2;100;0;0~~\x1b\\");
        CHECK(g.images().image_count() == 1);
        CHECK(g.images().placements().size() == 1);
        const TerminalImage* img = g.images().find(g.images().placements()[0].image_id);
        CHECK(img != nullptr);
        CHECK(img->width == 2);
        CHECK(img->height == 6);
        // '~' is 0x7E - 0x3F = 63, all six bits, in the colour just defined.
        CHECK(img->pixels[0] == (0xFFu << 24 | 0xFFu)); // RGBA32: red, opaque
        // Two columns wide and one row tall at a 10x20 cell.
        CHECK(g.images().placements()[0].cols == 1);
        CHECK(g.images().placements()[0].rows == 1);
        // The cursor ends at the start of the line below the image.
        CHECK(g.get_cursor_row() == 1);
        CHECK(g.get_cursor_col() == 0);
    }

    // Bands: '-' moves down six pixels, so this is 1x12.
    {
        TerminalGrid g; g.resize(80, 24);
        g.set_cell_pixel_size(10, 20);
        ANSIParser p;
        feed(p, g, "\x1bPq~-~\x1b\\");
        const TerminalImage* img = g.images().find(g.images().placements()[0].image_id);
        CHECK(img->width == 1);
        CHECK(img->height == 12);
    }

    // Repeat: "!5~" is five columns.
    {
        TerminalGrid g; g.resize(80, 24);
        g.set_cell_pixel_size(10, 20);
        ANSIParser p;
        feed(p, g, "\x1bPq!5~\x1b\\");
        const TerminalImage* img = g.images().find(g.images().placements()[0].image_id);
        CHECK(img->width == 5);
        CHECK(img->height == 6);
    }

    // Only the low bit set lights the top pixel and nothing below it. P2
    // decides what "nothing" looks like: 0 paints the background opaque
    // black, 1 leaves it clear.
    {
        TerminalGrid g; g.resize(80, 24);
        g.set_cell_pixel_size(10, 20);
        ANSIParser p;
        feed(p, g, "\x1bP0;0;0q#0;2;0;100;0@\x1b\\");
        const TerminalImage* img = g.images().find(g.images().placements()[0].image_id);
        CHECK(img->height == 6);
        CHECK(img->pixels[0] == (0xFFu << 24 | 0xFFu << 8)); // green, opaque
        CHECK(img->pixels[1 * img->width] == 0xFF000000u);   // opaque black
    }
    {
        TerminalGrid g; g.resize(80, 24);
        g.set_cell_pixel_size(10, 20);
        ANSIParser p;
        feed(p, g, "\x1bP0;1;0q#0;2;0;100;0@\x1b\\");
        const TerminalImage* img = g.images().find(g.images().placements()[0].image_id);
        CHECK(img->pixels[0] != 0);
        CHECK(img->pixels[1 * img->width] == 0); // transparent
    }

    // A DCS that is not sixel is consumed and ignored, as it always was.
    {
        TerminalGrid g; g.resize(80, 24);
        ANSIParser p;
        feed(p, g, "\x1bP+q616263\x1b\\hello");
        CHECK(g.images().image_count() == 0);
        CHECK(row_text(g, 0) == "hello");
    }

    // Truncated payloads must not crash or leave the parser stuck.
    {
        TerminalGrid g; g.resize(80, 24);
        ANSIParser p;
        feed(p, g, "\x1bPq~~~");        // no terminator
        feed(p, g, "\x1b\\");
        feed(p, g, "\x1bPq#\x1b\\");    // colour introducer with no parameters
        feed(p, g, "\x1bPq\x1b\\");     // no data at all
        feed(p, g, "after");
        CHECK(row_text(g, g.get_cursor_row()) == "after");
    }

    // The image scrolls with its text and is retired with it.
    {
        TerminalGrid g; g.resize(80, 5);
        g.set_cell_pixel_size(10, 20);
        g.set_max_scrollback(4);
        ANSIParser p;
        feed(p, g, "\x1bPq~\x1b\\");
        CHECK(g.images().placements().size() == 1);
        for (int i = 0; i < 30; ++i) feed(p, g, "x\r\n");
        CHECK(g.images().placements().empty());
        CHECK(g.images().image_count() == 0);
    }
}

// Kitty graphics protocol: ESC _ G <key=value,...> ; <base64> ST.
static void test_kitty_graphics() {
    // Base64 of four RGBA pixels, all opaque red (FF 00 00 FF x4).
    const char* red4 = "/wAA//8AAP//AAD//wAA/w==";

    // Transmit and display in one go.
    {
        TerminalGrid g; g.resize(80, 24);
        g.set_cell_pixel_size(10, 20);
        ANSIParser p;
        feed(p, g, std::string("\x1b_Ga=T,f=32,s=2,v=2,i=5;") + red4 + "\x1b\\");
        CHECK(g.images().image_count() == 1);
        CHECK(g.images().has_image(5));
        CHECK(g.images().placements().size() == 1);
        const TerminalImage* img = g.images().find(5);
        CHECK(img->width == 2 && img->height == 2);
        CHECK(img->pixels[0] == (0xFFu << 24 | 0xFFu)); // RGBA32 red
        // Unlike sixel, the cursor does not move: the app is positioning the
        // image itself.
        CHECK(g.get_cursor_row() == 0 && g.get_cursor_col() == 0);
        CHECK(g.take_pending_reply() == "\x1b_Gi=5;OK\x1b\\");
    }

    // Transmit only, then place separately.
    {
        TerminalGrid g; g.resize(80, 24);
        g.set_cell_pixel_size(10, 20);
        ANSIParser p;
        feed(p, g, std::string("\x1b_Ga=t,f=32,s=2,v=2,i=9,q=2;") + red4 + "\x1b\\");
        CHECK(g.images().has_image(9));
        CHECK(g.images().placements().empty());
        CHECK(!g.has_pending_reply()); // q=2 silences everything

        feed(p, g, "\x1b[5;3H");
        feed(p, g, "\x1b_Ga=p,i=9,p=1,c=4,r=2,q=2\x1b\\");
        CHECK(g.images().placements().size() == 1);
        CHECK(g.images().placements()[0].col == 2);
        CHECK(g.images().placements()[0].cols == 4);
        CHECK(g.images().placements()[0].rows == 2);
        CHECK(g.images().placements()[0].placement_id == 1);
    }

    // Chunked transmission, which is how any real image arrives.
    {
        TerminalGrid g; g.resize(80, 24);
        g.set_cell_pixel_size(10, 20);
        ANSIParser p;
        feed(p, g, "\x1b_Ga=T,f=32,s=2,v=2,i=3,m=1;/wAA//8AAP8=\x1b\\");
        CHECK(g.images().image_count() == 0); // nothing until the last chunk
        feed(p, g, "\x1b_Gm=0;/wAA//8AAP8=\x1b\\");
        CHECK(g.images().has_image(3));
        CHECK(g.images().find(3)->width == 2);
        CHECK(g.images().placements().size() == 1);
    }

    // 24-bit raw gets an opaque alpha.
    {
        TerminalGrid g; g.resize(80, 24);
        ANSIParser p;
        feed(p, g, "\x1b_Ga=t,f=24,s=1,v=1,i=2,q=2;AP8A\x1b\\"); // 00 FF 00
        CHECK(g.images().find(2)->pixels[0] == (0xFFu << 24 | 0xFFu << 8));
    }

    // Delete: lowercase drops the placement, uppercase the pixels too.
    {
        TerminalGrid g; g.resize(80, 24);
        g.set_cell_pixel_size(10, 20);
        ANSIParser p;
        feed(p, g, std::string("\x1b_Ga=T,f=32,s=2,v=2,i=4,q=2;") + red4 + "\x1b\\");
        CHECK(g.images().placements().size() == 1);
        feed(p, g, "\x1b_Ga=d,d=i,i=4,q=2\x1b\\");
        CHECK(g.images().placements().empty());
        CHECK(g.images().has_image(4)); // still placeable
        feed(p, g, "\x1b_Ga=d,d=I,i=4,q=2\x1b\\");
        CHECK(!g.images().has_image(4));
    }

    // Refusals are reported so the client can fall back rather than hang.
    {
        TerminalGrid g; g.resize(80, 24);
        ANSIParser p;
        feed(p, g, "\x1b_Ga=t,t=f,i=1;L3RtcC94\x1b\\");
        std::string reply = g.take_pending_reply();
        CHECK(reply.find("EBADF") != std::string::npos);
        CHECK(g.images().image_count() == 0);

        feed(p, g, "\x1b_Ga=t,f=32,o=z,s=2,v=2,i=1;AA==\x1b\\");
        CHECK(g.take_pending_reply().find("EINVAL") != std::string::npos);

        feed(p, g, "\x1b_Ga=p,i=999\x1b\\");
        CHECK(g.take_pending_reply().find("ENOENT") != std::string::npos);

        // s/v not matching the payload is refused rather than read past.
        feed(p, g, "\x1b_Ga=t,f=32,s=100,v=100,i=1;AA==\x1b\\");
        CHECK(g.take_pending_reply().find("EINVAL") != std::string::npos);
    }

    // A capability probe gets an answer, which is how a client discovers the
    // protocol exists at all.
    {
        TerminalGrid g; g.resize(80, 24);
        ANSIParser p;
        feed(p, g, "\x1b_Ga=q,i=1\x1b\\");
        CHECK(g.take_pending_reply() == "\x1b_Gi=1;OK\x1b\\");
    }

    // An APC that is not a graphics command is consumed and ignored.
    {
        TerminalGrid g; g.resize(80, 24);
        ANSIParser p;
        feed(p, g, "\x1b_Xsomething\x1b\\hello");
        CHECK(g.images().image_count() == 0);
        CHECK(row_text(g, 0) == "hello");
    }
}

// OSC 10/11/12: the terminal's default colours. The query half is what
// applications use to find out whether they are drawing on something light or
// something dark, so they can pick a readable palette.
static void test_osc_colors() {
    TerminalGrid g; g.resize(20, 4);
    ANSIParser p;

    // Query the background. The default is transparent so media shows through,
    // but "transparent" answers nobody's question, so what is reported is the
    // opaque base the terminal composites onto.
    feed(p, g, "\x1b]11;?\x1b\\");
    CHECK(g.take_pending_reply() == "\x1b]11;rgb:0ccd/0ccd/0f5c\x1b\\");
    CHECK(g.get_default_bg().a == 0.0f); // still transparent for rendering

    // The terminator is echoed back: a client that sent BEL parses for BEL.
    feed(p, g, "\x1b]11;?\x07");
    CHECK(g.take_pending_reply() == "\x1b]11;rgb:0ccd/0ccd/0f5c\x07");

    // Setting one, in both accepted spellings.
    feed(p, g, "\x1b]10;#ff0000\x1b\\");
    CHECK(g.get_default_fg().r == 1.0f);
    CHECK(g.get_default_fg().g == 0.0f);
    feed(p, g, "\x1b]10;rgb:0000/ffff/0000\x1b\\");
    CHECK(g.get_default_fg().g == 1.0f);
    CHECK(g.get_default_fg().r == 0.0f);

    // Short forms scale by their width: "#f00" is full red, not 1/16th.
    feed(p, g, "\x1b]12;#00f\x1b\\");
    CHECK(g.get_default_cursor_color().b == 1.0f);

    // A background an application sets is opaque and is what gets reported,
    // because it has taken responsibility for what the text sits on.
    feed(p, g, "\x1b]11;#ffffff\x1b\\");
    CHECK(g.get_default_bg().a == 1.0f);
    feed(p, g, "\x1b]11;?\x1b\\");
    CHECK(g.take_pending_reply() == "\x1b]11;rgb:ffff/ffff/ffff\x1b\\");

    // SGR 39/49 restore to whatever the defaults now are, not to a literal.
    feed(p, g, "\x1b[31m\x1b[39m");
    CHECK(g.get_current_fg().g == 1.0f); // the green set above
    feed(p, g, "\x1b[44m\x1b[49m");
    CHECK(g.get_current_bg().r == 1.0f); // the white set above

    // OSC 11x reset, which carries no semicolon at all.
    feed(p, g, "\x1b]111\x1b\\");
    CHECK(g.get_default_bg().a == 0.0f);
    feed(p, g, "\x1b]110\x1b\\");
    CHECK(g.get_default_fg().r > 0.85f && g.get_default_fg().r < 0.95f);
    feed(p, g, "\x1b]112\x1b\\");
    CHECK(g.get_default_cursor_color().b == 1.0f);

    // Several colours in one query, selector advancing across the list.
    feed(p, g, "\x1b]10;?;?;?\x1b\\");
    std::string reply = g.take_pending_reply();
    CHECK(reply.find("\x1b]10;") != std::string::npos);
    CHECK(reply.find("\x1b]11;") != std::string::npos);
    CHECK(reply.find("\x1b]12;") != std::string::npos);

    // Malformed specs are ignored rather than producing a garbage colour.
    SDL_FColor before = g.get_default_fg();
    feed(p, g, "\x1b]10;not-a-color\x1b\\");
    CHECK(g.get_default_fg().r == before.r);
    feed(p, g, "\x1b]10;#12345\x1b\\");
    CHECK(g.get_default_fg().r == before.r);

    // RIS puts the defaults back, unlike DECSTR which only resets SGR state.
    feed(p, g, "\x1b]11;#ffffff\x1b\\");
    feed(p, g, "\x1b[!p");
    CHECK(g.get_default_bg().a == 1.0f); // soft reset leaves the default alone
    feed(p, g, "\x1b" "c");
    CHECK(g.get_default_bg().a == 0.0f);
}

// OSC 4: the 256-colour palette, which themes recolour at runtime.
static void test_osc_palette() {
    TerminalGrid g; g.resize(20, 4);
    ANSIParser p;

    // Query an entry. Index 1 is red in the shipped palette.
    feed(p, g, "\x1b]4;1;?\x1b\\");
    std::string reply = g.take_pending_reply();
    CHECK(reply.compare(0, 7, "\x1b]4;1;") == 0 || reply.find("\x1b]4;1;") == 0);
    CHECK(reply.find("rgb:") != std::string::npos);

    // Set one, and see it in the SGR colour that uses it.
    feed(p, g, "\x1b]4;1;#00ff00\x1b\\");
    CHECK(g.palette_color(1).g == 1.0f);
    CHECK(g.palette_color(1).r == 0.0f);
    feed(p, g, "\x1b[31mX");
    CHECK(g.get_cell_at(0, 0).fg.g > 250);
    CHECK(g.get_cell_at(0, 0).fg.r < 5);

    // Several pairs in one sequence.
    feed(p, g, "\x1b]4;2;#0000ff;3;#ff00ff\x1b\\");
    CHECK(g.palette_color(2).b == 1.0f);
    CHECK(g.palette_color(3).r == 1.0f && g.palette_color(3).b == 1.0f);

    // Entries above 15 are reachable too -- the cube and the grey ramp are
    // real entries now, not arithmetic.
    feed(p, g, "\x1b]4;200;#123456\x1b\\");
    CHECK(g.palette_color(200).r > 0.06f && g.palette_color(200).r < 0.08f);
    feed(p, g, "\x1b[38;5;200mY");
    CHECK(g.get_cell_at(1, 0).fg.b > 0x50);

    // Reset one entry, then all of them.
    feed(p, g, "\x1b]104;1\x1b\\");
    CHECK(g.palette_color(1).r > 0.8f); // red again
    CHECK(g.palette_color(2).b == 1.0f); // untouched
    feed(p, g, "\x1b]104\x1b\\");
    CHECK(g.palette_color(2).b < 0.9f);
    CHECK(g.palette_color(200).r > 0.9f); // back to the cube value

    // Out-of-range and malformed entries change nothing.
    SDL_FColor before = g.palette_color(5);
    feed(p, g, "\x1b]4;999;#000000\x1b\\");
    feed(p, g, "\x1b]4;5;nonsense\x1b\\");
    CHECK(g.palette_color(5).r == before.r);
    CHECK(!g.has_pending_reply());

    // RIS restores the whole palette.
    feed(p, g, "\x1b]4;7;#000000\x1b\\");
    feed(p, g, "\x1b" "c");
    CHECK(g.palette_color(7).r > 0.8f);
}

// OSC 7: the shell reporting where it is, so a new tab or split can start in
// the same directory instead of wherever sink was launched from.
static void test_osc_cwd() {
    TerminalGrid g; g.resize(20, 4);
    ANSIParser p;
    CHECK(g.get_working_directory().empty());

    feed(p, g, "\x1b]7;file://localhost/Users/kady/Projects/sink\x1b\\");
    CHECK(g.get_working_directory() == "/Users/kady/Projects/sink");

    // No host at all is the other common spelling.
    feed(p, g, "\x1b]7;file:///tmp\x1b\\");
    CHECK(g.get_working_directory() == "/tmp");

    // Percent-decoding, since the shell hooks that emit this escape anything
    // outside the unreserved set.
    feed(p, g, "\x1b]7;file:///Users/kady/My%20Documents/a%2Bb\x1b\\");
    CHECK(g.get_working_directory() == "/Users/kady/My Documents/a+b");

    // A path from another machine names nothing here, so it is refused rather
    // than opening a local directory that happens to share its name.
    std::string before = g.get_working_directory();
    feed(p, g, "\x1b]7;file://some-other-box/Users/kady\x1b\\");
    CHECK(g.get_working_directory() == before);

    // Relative paths and NUL-smuggling are refused: this ends up as an
    // argument to chdir() in a forked child.
    feed(p, g, "\x1b]7;file://localhost" "relative/path\x1b\\");
    CHECK(g.get_working_directory() == before);
    feed(p, g, std::string("\x1b]7;file:///tmp/a%00b\x1b\\"));
    CHECK(g.get_working_directory() == before);

    // Malformed sequences leave it alone rather than clearing it.
    feed(p, g, "\x1b]7;not-a-uri\x1b\\");
    CHECK(g.get_working_directory() == before);
    feed(p, g, "\x1b]7;\x1b\\");
    CHECK(g.get_working_directory() == before);
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
    test_grapheme_clusters();
    test_image_placements();
    test_sixel();
    test_kitty_graphics();
    test_osc_colors();
    test_osc_palette();
    test_osc_cwd();

    std::printf("%d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
