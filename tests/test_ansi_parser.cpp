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
}

// East Asian Wide and Fullwidth characters, and emoji, occupy two columns.
// Before this the grid advanced one column per codepoint regardless, so CJK
// and emoji rendered at half the width every other terminal gives them and
// anything column-aligned drifted.
// DECSET/DECRST 2026 (synchronized output) and 1004 (focus reporting). Both
// are mode flags the app layer acts on -- holding the presented frame, and
// sending CSI I / CSI O -- so what is checked here is that the parser tracks
// them, including that an unknown neighbouring mode doesn't disturb them.
// SearchResult::col is a column, but set_search_query() was assigning it the
// byte offset std::string::find() returns. On any row containing multi-byte
// text the highlight drifted right, further with each such character before
// the match. These check the mapping through the user-visible predicate.
// Combining marks compose onto the character before them instead of taking a
// cell. This is the shape macOS produces constantly: it stores filenames in
// NFD, so `ls` in a directory with accented names emits base+mark sequences.
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

    CHECK(!g.is_frame_held());
    CHECK(!g.is_focus_reporting());

    feed(p, g, "\x1b[?2026h");
    CHECK(g.is_frame_held());
    feed(p, g, "\x1b[?2026l");
    CHECK(!g.is_frame_held());

    feed(p, g, "\x1b[?1004h");
    CHECK(g.is_focus_reporting());
    feed(p, g, "\x1b[?1004l");
    CHECK(!g.is_focus_reporting());

    // Set together, reset independently
    feed(p, g, "\x1b[?2026h\x1b[?1004h");
    CHECK(g.is_frame_held());
    CHECK(g.is_focus_reporting());
    feed(p, g, "\x1b[?2026l");
    CHECK(!g.is_frame_held());
    CHECK(g.is_focus_reporting());

    // An unrecognised mode in between must not clear either
    feed(p, g, "\x1b[?2026h\x1b[?7h");
    CHECK(g.is_frame_held());
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

int main() {
    test_plain_text();
    test_crlf_and_scroll();
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

    std::printf("%d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
