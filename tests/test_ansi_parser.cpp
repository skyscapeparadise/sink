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
