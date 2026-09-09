#pragma once

#include <SDL3/SDL.h>
#include <vector>
#include <deque>
#include <string>
#include <unordered_map>
#include "font_manager.hpp"
#include "terminal_images.hpp"

// Per-cell SGR attribute flags. Bold is also folded into the color at parse
// time (base palette -> bright variant).
enum CellAttr : uint8_t {
    ATTR_BOLD          = 1 << 0,
    ATTR_DIM           = 1 << 1,
    ATTR_ITALIC        = 1 << 2,
    ATTR_UNDERLINE     = 1 << 3,
    ATTR_REVERSE       = 1 << 4,
    ATTR_STRIKETHROUGH = 1 << 5,
    // Second cell of a double-width pair. Carries no glyph of its own: the
    // lead cell draws across both. Bits 6-7 were free, so this costs nothing.
    ATTR_WIDE_CONT     = 1 << 6,
};

// Cell colours packed to 8 bits per channel.
//
// Every source of a terminal colour is already 8-bit -- the 16-colour and
// xterm-256 palettes are byte triples, and truecolor SGR (38;2;r;g;b) parses
// three 0-255 values -- so storing them as four floats each cost 24 bytes per
// cell and bought no precision. Cell is the hottest structure in the program:
// scroll_up() memmoves the entire grid one row on every newline, which
// profiled at ~70% of parse time, and the cost is directly proportional to
// sizeof(Cell). 44 bytes -> 20.
struct PackedColor {
    uint8_t r = 0, g = 0, b = 0, a = 0;
};

inline PackedColor pack_color(const SDL_FColor& c) {
    auto q = [](float v) -> uint8_t {
        float s = v * 255.0f + 0.5f;
        if (s <= 0.0f) return 0;
        if (s >= 255.0f) return 255;
        return static_cast<uint8_t>(s);
    };
    return { q(c.r), q(c.g), q(c.b), q(c.a) };
}

inline SDL_FColor unpack_color(const PackedColor& c) {
    constexpr float k = 1.0f / 255.0f;
    return { c.r * k, c.g * k, c.b * k, c.a * k };
}

// Field order is unchanged from when fg/bg were SDL_FColor: with 4-byte
// alignment, moving hyperlink_id ahead of attrs would not shrink this any
// further, and keeping the order means the aggregate initialisers throughout
// terminal_grid.cpp stay correct.
struct Cell {
    char32_t codepoint;
    PackedColor fg;
    PackedColor bg;
    uint8_t attrs = 0;
    uint32_t hyperlink_id = 0; // 0 = no link; see TerminalGrid::get_hyperlink_uri
};

// A cell holds one codepoint, and plenty of text needs more than one to make a
// single visible character: a base with stacking marks that have no
// precomposed form, and the ZWJ sequences behind family and profession emoji.
//
// Rather than grow Cell -- whose size is the reason scroll_up() is as cheap as
// it is; see the note on its layout above -- the top bit of `codepoint` tags
// the remainder as an index into the grid's cluster table instead of a literal
// codepoint. Unicode needs 21 bits, so the tag is free and Cell stays 20 bytes.
inline constexpr char32_t kClusterTag = 0x80000000u;
inline bool is_cluster_ref(char32_t cp) { return (cp & kClusterTag) != 0; }
inline uint32_t cluster_index_of(char32_t cp) {
    return static_cast<uint32_t>(cp & ~kClusterTag);
}

// Columns a codepoint occupies: 2 for East Asian Wide/Fullwidth and emoji
// presentation, 1 otherwise.
//
// Combining marks never reach this: write_character() composes them onto the
// preceding character before any width question arises. See is_combining_mark.
int char_display_width(char32_t cp);

// True for Unicode Mn/Me: marks that attach to the preceding base character
// and occupy no column of their own.
bool is_combining_mark(char32_t cp);

// Canonical composition of a base and a combining mark, or 0 if the pair has
// no precomposed form. 'e' + U+0301 gives U+00E9.
char32_t compose_pair(char32_t base, char32_t mark);

// DECSCUSR cursor shapes (CSI Ps SP q). Editors switch between these to show
// their mode -- a bar while inserting, a block while in normal mode -- so an
// ignored DECSCUSR leaves vim users with no visible mode indicator at all.
enum class CursorShape { Block, Underline, Bar };

struct ScrollbackRow {
    std::vector<Cell> cells;
    bool wrapped = false;
    bool prompt = false; // OSC 133;A fired on this row (shell prompt start)
};

class TerminalGrid {
public:
    TerminalGrid();
    ~TerminalGrid();

    void resize(int cols, int rows);
    void initialize_mock_data();
    
    void set_cell(int col, int row, char32_t codepoint, const SDL_FColor& fg, const SDL_FColor& bg);
    void write_character(char32_t codepoint);

    // Writes up to n plain ASCII characters into the current row, starting at
    // the cursor and all sharing the current style. Returns how many were
    // written, clamped to the columns left in the row.
    //
    // Exactly equivalent to calling write_character() for each, for characters
    // that need no translation -- the per-character path recomputes the row
    // pointer (ring index plus a multiply) and reloads the style off `this`
    // every time, both of which are invariant across a run of ordinary text.
    //
    // The caller must check is_wrap_pending() first: a deferred wrap is left
    // to write_character() so the wrap-entry logic lives in exactly one place.
    int write_run(const char* ascii, int n);
    
    void scroll_up();
    void clear_screen();

    // Scroll region (DECSTBM) API. Rows are 0-based and inclusive; anything
    // out of range or degenerate resets to the full screen.
    void set_scroll_region(int top, int bottom);
    int get_scroll_top() const { return scroll_top_; }
    int get_scroll_bottom() const;

    // DECOM (CSI ?6h/l): while set, CUP/HVP row 1 means the scroll region's
    // top margin rather than the screen's top, and cursor positioning is
    // confined to the region. Toggling it, like DECSTBM, homes the cursor.
    void set_origin_mode(bool on) { origin_mode_ = on; }
    bool is_origin_mode() const { return origin_mode_; }
    void cursor_home(); // (scroll_top_, 0) if origin mode is set, else (0, 0)
    void index();                       // IND / LF: down one, scrolling at the bottom margin
    void reverse_index();               // RI: up one, scrolling at the top margin
    void scroll_region_up(int count);   // SU (CSI S)
    void scroll_region_down(int count); // SD (CSI T)
    void insert_lines(int count);       // IL (CSI L)
    void delete_lines(int count);       // DL (CSI M)
    void clear_scrollback();

    // RIS (ESC c): return the terminal to its power-on state. vtebench sends
    // this between every sample, and full-screen apps send it to recover a
    // confused terminal, so a terminal that ignores it accumulates modes --
    // an alt screen never exited, a scroll region never widened -- until
    // output scrolls inside a few rows and the screen looks frozen.
    void full_reset();

    // DECSTR (CSI ! p): a soft reset. Much narrower than full_reset() -- see
    // the implementation for exactly what it does and does not touch.
    void soft_reset();
    // Tab stops. HTS (ESC H) sets one at the cursor, TBC (CSI g) clears one or
    // all, and CHT/CBT (CSI I / CSI Z) walk between them. tab_forward(1) is
    // what a plain HT does.
    void set_tab_stop();
    void clear_tab_stop();
    void clear_all_tab_stops();
    void reset_tab_stops(); // back to every 8th column
    void tab_forward(int count);
    void tab_backward(int count);
    bool is_tab_stop(int col) const {
        return col >= 0 && col < static_cast<int>(tab_stops_.size()) && tab_stops_[col];
    }

    void clear_line(int row, int mode); // 0 = cursor to end, 1 = start to cursor, 2 = entire line
    void insert_character(int count); // ICH: open count blank cells at the cursor
    void delete_character(int count);
    void erase_characters(int count); // ECH: blank count cells at the cursor in place, no shift
    
    // Render the grid with display_scale to support cell padding offsets and smooth cursor animation
    void render(SDL_Renderer* renderer, const FontManager& font_manager, float start_x, float start_y, float display_scale = 1.0f, float dt = 0.016f, bool animated_typing = false);

    int get_cols() const { return cols_; }
    int get_rows() const { return rows_; }

    int get_cursor_col() const { return cursor_col_; }
    int get_cursor_row() const { return cursor_row_; }
    void set_cursor_col(int col);
    void set_cursor_row(int row);

    void set_current_fg(const SDL_FColor& fg) { current_fg_ = fg; current_fg_packed_ = pack_color(fg); }
    void set_current_bg(const SDL_FColor& bg) { current_bg_ = bg; current_bg_packed_ = pack_color(bg); }
    const SDL_FColor& get_current_fg() const { return current_fg_; }
    const SDL_FColor& get_current_bg() const { return current_bg_; }
    void set_current_attrs(uint8_t attrs) { current_attrs_ = attrs; }
    uint8_t get_current_attrs() const { return current_attrs_; }

    // OSC 8 hyperlinks. set_current_hyperlink("") clears (the OSC 8;;ST
    // close form); a non-empty URI is deduped against previously-seen URIs
    // so repeated links (e.g. every row of an `ls --hyperlink` listing
    // pointing at the same directory) don't grow the table per-cell.
    void set_current_hyperlink(const std::string& uri);
    const std::string& get_hyperlink_uri(uint32_t id) const;

    // The codepoints a cell renders as: its own, or the whole sequence when it
    // holds a cluster. `count` comes back as the length; a cell referring to a
    // cluster that no longer exists reports zero, which renders as nothing.
    //
    // For an ordinary cell the pointer is into `cell` itself, so it is only
    // valid while that Cell is -- do not call this on a temporary. get_cell_at()
    // returns by value, so pair it with cell_string() instead.
    const char32_t* cell_text(const Cell& cell, int& count) const;

    // Same contents, copied. Safe with a temporary Cell.
    std::u32string cell_string(const Cell& cell) const;

    // First codepoint of a cell, which is what decides its width and what
    // anything treating a cell as a single character should look at.
    char32_t cell_base(const Cell& cell) const;

    // Appends a cell's text as UTF-8 -- one codepoint, or a whole cluster.
    void append_cell_utf8(const Cell& cell, std::string& out) const;

    // Scrollback view control helpers
    void scroll_view(int delta);
    void reset_scroll();
    // Ends the smooth-scroll glide immediately, putting the view where
    // scroll_offset_ already says it is. Hit-testing maps pixels to rows
    // through scroll_offset_, so anything that starts a click needs the rows
    // on screen to agree with it rather than still be gliding towards it.
    void snap_scroll_view() { display_scroll_offset_ = static_cast<float>(scroll_offset_); }
    int get_scroll_offset() const { return scroll_offset_; }
    Cell get_cell_at(int col, int row) const;

    // Clipboard & Selection API
    void start_selection(int col, int row);
    void update_selection(int col, int row);
    void end_selection();
    void clear_selection();
    bool is_cell_selected(int col, int row) const;
    std::string get_selected_text() const;
    void select_all();
    bool has_selection() const { return has_selection_; }
    bool is_selecting() const { return selecting_; }

    void trigger_error_flash() { error_glow_opacity_ = 1.0f; }
    float get_error_glow_opacity() const { return error_glow_opacity_; }
    void update_timers(float dt);

    // Switches to/from the alternate screen buffer (DECSET/DECRST 1049 and
    // friends): the primary screen and cursor are saved on entry and restored
    // on exit, so quitting a full-screen app brings the shell contents back.
    void set_alt_screen(bool active);
    bool is_alt_screen_active() const { return alt_screen_active_; }

    void set_cursor_visible(bool visible) { cursor_visible_ = visible; }
    bool is_cursor_visible() const { return cursor_visible_; }

    // Takes a raw DECSCUSR parameter (0-6); anything else falls back to a
    // block, which is what the parameter's default means.
    void set_cursor_shape(int decscusr_param);
    CursorShape get_cursor_shape() const { return cursor_shape_; }

    // Synchronized output (DECSET/DECRST 2026). Apps that redraw a whole
    // frame -- neovim, helix, fzf, lazygit -- wrap the update in BSU/ESU so
    // the terminal shows the finished frame rather than the half-drawn states
    // in between.
    //
    // This is only the protocol state: whether the application currently has
    // an update open. How long that is honoured is the render loop's decision,
    // because the guarantee that matters -- that the screen keeps updating --
    // can only be expressed against actual presents. An earlier version put a
    // deadline here instead and re-armed it on each BSU, which bounded a
    // single update but not a run of them: vtebench's sync benchmark sends a
    // BSU/ESU pair roughly every 400 bytes, so the deadline was always freshly
    // armed and the window never presented again.
    void set_synchronized_output(bool active) { synchronized_output_ = active; }
    bool is_synchronized_output() const { return synchronized_output_; }

    // Focus reporting (DECSET/DECRST 1004): the app is told when the terminal
    // gains or loses focus, which vim uses to drive autoread and tmux to
    // track the active client.
    void set_focus_reporting(bool active) { focus_reporting_ = active; }
    bool is_focus_reporting() const { return focus_reporting_; }

    void set_bracketed_paste(bool active) { bracketed_paste_active_ = active; }
    bool is_bracketed_paste_active() const { return bracketed_paste_active_; }

    // Mouse reporting state, set via DECSET/DECRST by the running app.
    // mode: 0 = off, 9 = X10 press-only, 1000 = press/release,
    // 1002 = press/release + drag motion, 1003 = all motion.
    // SGR (1006) selects the extended encoding for whichever mode is active.
    void set_mouse_mode(int mode) { mouse_mode_ = mode; }
    int get_mouse_mode() const { return mouse_mode_; }
    void set_mouse_sgr(bool sgr) { mouse_sgr_ = sgr; }
    bool is_mouse_sgr() const { return mouse_sgr_; }

    // DECCKM (?1): arrows send ESC O A style when the app asked for it
    void set_app_cursor_keys(bool app) { app_cursor_keys_ = app; }
    bool is_app_cursor_keys() const { return app_cursor_keys_; }

    // Alternate scroll (?1007): on the alt screen with no mouse mode, wheel
    // ticks are delivered as arrow keys so pagers scroll naturally. On by
    // default, matching Terminal.app and iTerm2.
    void set_alternate_scroll(bool on) { alternate_scroll_ = on; }
    bool is_alternate_scroll() const { return alternate_scroll_; }
    
    void set_prompt_boundary(int col) { prompt_boundary_col_ = col; }
    int get_prompt_boundary() const { return prompt_boundary_col_; }
    void lock_prompt_boundary_if_unset() { if (prompt_boundary_col_ == -1) prompt_boundary_col_ = cursor_col_; }

    void select_word_at(int col, int row);
    void select_line_at(int row);
    std::string get_all_text() const;
    std::string get_current_line_text() const;

    size_t get_scrollback_size() const { return scrollback_history_.size(); }
    void set_max_scrollback(size_t lines);
    size_t get_max_scrollback() const { return max_scrollback_size_; }
    int get_select_start_col() const { return select_start_col_; }
    int get_select_start_row() const { return select_start_row_; }
    int get_select_end_col() const { return select_end_col_; }
    int get_select_end_row() const { return select_end_row_; }

    void save_cursor();
    void restore_cursor();

    void clear_wrap_pending() { wrap_pending_ = false; }
    bool is_wrap_pending() const { return wrap_pending_; }
    const std::vector<uint8_t>& get_row_wrapped() const { return row_wrapped_; }

    // Scrollback Search API
    struct SearchResult {
        int absolute_row; // Row index relative to history + active grid
        int col;
        int len;
    };

    void set_search_query(const std::string& query);
    void set_search_active(bool active);
    bool is_search_active() const { return search_active_; }
    void search_next();
    void search_prev();
    int get_search_match_count() const { return static_cast<int>(search_matches_.size()); }
    int get_current_search_index() const { return current_match_index_; }
    const std::string& get_search_query() const { return search_query_; }
    bool is_cell_search_matched(int col, int row) const;

    // Ligature support setting
    void set_enable_ligatures(bool enable) { enable_ligatures_ = enable; }
    bool get_enable_ligatures() const { return enable_ligatures_; }

    // Window title, set by OSC 0/2 and polled by the frame loop
    void set_window_title(const std::string& title) {
        window_title_ = title;
        title_dirty_ = true;
    }
    bool has_pending_title() const { return title_dirty_; }
    std::string take_window_title() {
        title_dirty_ = false;
        return window_title_;
    }

    // OSC 52 clipboard write (decoded by the parser), polled by the frame
    // loop the same way the window title is. Write-only by design: this
    // grid never reports clipboard *contents* back to the app -- OSC 52
    // read-back is a known escape-sequence abuse vector (untrusted output,
    // e.g. from `cat`ing a file or a compromised remote SSH session, could
    // otherwise silently exfiltrate whatever's on the system clipboard).
    void set_clipboard_text(const std::string& text) {
        pending_clipboard_text_ = text;
        clipboard_dirty_ = true;
    }
    bool has_pending_clipboard_text() const { return clipboard_dirty_; }
    std::string take_clipboard_text() {
        clipboard_dirty_ = false;
        return pending_clipboard_text_;
    }

    // Inline images. Both graphics protocols decode into this.
    TerminalImages& images() { return images_; }
    const TerminalImages& images() const { return images_; }

    // Scrollback-absolute line number of an active row. Screen rows renumber
    // as output scrolls; these do not, and never repeat, so an image pinned to
    // one stays with its text and can be retired the moment that text falls
    // out of history.
    uint64_t line_id_for_row(int row) const {
        return lines_evicted_ + static_cast<uint64_t>(scrollback_history_.size()) +
               static_cast<uint64_t>(row);
    }
    uint64_t oldest_line_id() const { return lines_evicted_; }

    // Replies the terminal owes the shell: DSR cursor reports, DA capability
    // responses. Queued rather than written straight out because TerminalGrid
    // has no pty of its own -- main.cpp drains this once a frame and writes
    // it, exactly as it already does for the window title and OSC 52.
    //
    // Capped, because the trigger is untrusted input: a file full of CSI 6n,
    // or a remote shell echoing them, would otherwise queue replies faster
    // than the main loop sends them. A program waiting on a report blocks
    // until it arrives, so it can only ever have one outstanding; anything
    // past the cap is abuse rather than a request that will be missed.
    // Pixel size of one cell, pushed in by the layout because the grid has no
    // font of its own. 0 means unknown, in which case the pixel geometry
    // reports stay silent rather than answering with a made-up number.
    void set_cell_pixel_size(int w, int h) { cell_px_w_ = w; cell_px_h_ = h; }
    int get_cell_pixel_width() const { return cell_px_w_; }
    int get_cell_pixel_height() const { return cell_px_h_; }

    // How many cells the whole display would hold, for the XTWINOPS screen
    // size report. Pushed in by the layout for the same reason the cell size
    // is: the grid knows nothing about windows or monitors. 0 means unknown.
    void set_screen_size_chars(int cols, int rows) { screen_cols_ = cols; screen_rows_ = rows; }
    int get_screen_cols() const { return screen_cols_; }
    int get_screen_rows() const { return screen_rows_; }

    void queue_reply(const std::string& bytes);
    bool has_pending_reply() const { return !pending_reply_.empty(); }
    std::string take_pending_reply();

    // Kitty keyboard protocol state.
    //
    // The legacy input encoding is ambiguous by construction: Ctrl+I and Tab
    // are both 0x09, Ctrl+M and Enter are both 0x0D, Ctrl+[ and Escape are
    // both 0x1B, and there is no encoding at all for a key release. The
    // protocol replaces those with unambiguous CSI sequences, negotiated
    // through a flags word.
    //
    // Flags live on a stack because the protocol's enter/leave form is
    // push/pop: a full-screen app pushes what it needs on startup and pops on
    // exit, so one that dies without popping cannot leave the terminal in its
    // mode forever -- the next app pushes and pops over the top of it.
    static constexpr int kKbdDisambiguate = 1; // Ctrl/Alt/Esc get CSI u forms
    static constexpr int kKbdReportEvents = 2; // press/repeat/release
    // What sink actually honours. Incoming flags are masked to this, so the
    // query reply describes what will really happen rather than what was
    // asked for -- which is how the protocol expects negotiation to work.
    static constexpr int kKbdSupported = kKbdDisambiguate | kKbdReportEvents;

    int kbd_flags() const { return kbd_stack_.back(); }
    void kbd_set_flags(int flags, int mode); // CSI = flags ; mode u
    void kbd_push_flags(int flags);          // CSI > flags u
    void kbd_pop_flags(int count);           // CSI < count u

    // OSC 133 shell-integration prompt marks and jump navigation
    void mark_prompt_row();
    bool is_prompt_row(int row) const {
        return row >= 0 && row < static_cast<int>(row_prompt_.size()) && row_prompt_[row];
    }
    void scroll_to_prev_prompt();
    void scroll_to_next_prompt();

private:
    int cols_ = 0;
    int rows_ = 0;
    std::vector<Cell> cells_;

    // Row ring base. scroll_up() used to memmove the entire grid up one row on
    // every newline, which profiled at ~55% of parse time even after Cell
    // shrank to 20 bytes. With a rotating base a full-screen scroll just
    // advances this index and blanks the row that falls off, so the cost goes
    // from O(rows x cols) to O(cols).
    //
    // cells_ is therefore in ring order, not logical order: logical row r
    // lives at phys_row(r). Everything that touches a row goes through
    // row_data(). Note that row_wrapped_/row_prompt_ stay in logical order and
    // are still shifted explicitly -- they are one bool per row, so rotating
    // them buys nothing.
    int row_base_ = 0;

    // A prebuilt row of blank cells in the current SGR colours. Blanking a row
    // is std::fill over a 20-byte element, which cannot become a memset and so
    // writes cell by cell; scroll_up() does exactly that on every newline and
    // it profiled at ~13% of parse time on scrolling text. Copying from a
    // cached row hands the work to memmove instead. Rebuilt only when the
    // colours or the width change, which is rare next to how often it is read.
    std::vector<Cell> blank_row_cache_;
    PackedColor blank_row_fg_{};
    PackedColor blank_row_bg_{};
    bool blank_row_valid_ = false;
    const Cell* blank_row();

    // row_base_ and r are both in [0, rows_), so their sum is below 2 * rows_
    // and a conditional subtract replaces the modulo.
    int phys_row(int r) const {
        int p = r + row_base_;
        return p >= rows_ ? p - rows_ : p;
    }
    Cell* row_data(int r) { return cells_.data() + phys_row(r) * cols_; }
    const Cell* row_data(int r) const { return cells_.data() + phys_row(r) * cols_; }

    // A screen row's backing cells, resolved once so the render loop doesn't
    // re-derive them per column. `cells` is null (and `len` 0) for a row that
    // has nothing behind it -- above the oldest scrollback line, or below the
    // last active row -- which the caller renders as blanks.
    //
    // `view_offset` is the scrollback distance to read at. It is a parameter
    // rather than just scroll_offset_ because render() reads at the *animated*
    // position, which lags the target while a scroll is still gliding.
    struct RowView {
        const Cell* cells = nullptr;
        int len = 0;
    };
    RowView row_view(int row, int view_offset) const;

    // Columns [first, last] of `row` covered by the selection, inclusive.
    // Empty when last < first. Hoisting this out of the per-cell test lets the
    // render loop ask once per row instead of once per cell.
    void selected_span(int row, int view_offset, int& first, int& last) const;

    // Half-open range of search_matches_ entries falling on `row`.
    //
    // set_search_query() fills the vector in ascending absolute-row order, so
    // this is a binary search. It replaces a linear scan of *every* match for
    // *every* cell on screen: searching a common substring in a deep
    // scrollback finds tens of thousands of matches, and 10k cells x that many
    // comparisons per frame measured at 271ms -- a 4fps window for as long as
    // the search bar stayed open.
    void search_span(int row, int view_offset, size_t& begin, size_t& end) const;
    // uint8_t rather than bool: std::vector<bool> is bit-packed, so every
    // read and write is a shift-and-mask, and scroll_up() shifts both of these
    // one position on every newline. A byte per row makes that shift a plain
    // memmove and shows up in the parser benchmark.
    // One flag per column. Defaults to every 8th, which is what the hardcoded
    // (col + 8) & ~7 arithmetic this replaces always produced -- but that
    // arithmetic had nowhere to record a stop, so HTS and TBC could not be
    // implemented on top of it and a program that moved its stops silently
    // kept the default ones.
    std::vector<uint8_t> tab_stops_;

    std::vector<uint8_t> row_wrapped_;
    std::vector<uint8_t> row_prompt_; // per active row, parallel to row_wrapped_
    bool wrap_pending_ = false;
    int saved_cursor_col_ = 0;
    int saved_cursor_row_ = 0;

    // DECSTBM margins (0-based inclusive). scroll_bottom_ is re-anchored to
    // rows_-1 on every resize; helpers that never touch scrollback do the
    // actual row movement for partial-region scrolls, IL and DL.
    int scroll_top_ = 0;
    int scroll_bottom_ = 0;
    bool origin_mode_ = false;
    void shift_rows_up(int top, int bottom, int count);
    void shift_rows_down(int top, int bottom, int count);

    // Primary screen contents stashed while the alternate screen is active.
    // Saved with its own geometry: if the window is resized mid-app, restore
    // clamp-copies whatever still fits rather than reflowing.
    std::vector<Cell> saved_primary_cells_;
    std::vector<uint8_t> saved_primary_row_wrapped_;
    std::vector<uint8_t> saved_primary_row_prompt_;
    int saved_primary_cols_ = 0;
    int saved_primary_rows_ = 0;
    int saved_primary_cursor_col_ = 0;
    int saved_primary_cursor_row_ = 0;
    bool saved_primary_wrap_pending_ = false;
    
    // Scrollback history buffers
    // deque, not vector: scroll_up()/set_max_scrollback() trim from the
    // *front* every time history exceeds the cap, which is an O(elements
    // remaining) shift on a vector -- meaning heavy scroll volume (a big
    // `cat`, a noisy build log) degrades quadratically. deque's random
    // access (every other use here: get_cell_at, search, resize's reflow)
    // stays O(1); front-erase drops to O(elements removed) instead.
    // Lines dropped off the front of history over this grid's lifetime, so a
    // line's absolute id survives trimming. Never reset, including by RIS:
    // reusing an id would let a stale placement latch onto new text.
    uint64_t lines_evicted_ = 0;

    TerminalImages images_;
    std::vector<ImagePlacement> saved_primary_placements_;

    std::deque<ScrollbackRow> scrollback_history_;
    int scroll_offset_ = 0;
    float display_scroll_offset_ = 0.0f; // Smooth sub-pixel interpolated scroll offset
    size_t max_scrollback_size_ = 10000; // Configurable via preset scrollback_lines
    
    // Window title state (OSC 0/2)
    std::string window_title_;
    bool title_dirty_ = false;

    // Clipboard state (OSC 52)
    std::string pending_clipboard_text_;
    bool clipboard_dirty_ = false;

    // Queued shell replies; see queue_reply().
    int cell_px_w_ = 0;
    int cell_px_h_ = 0;
    int screen_cols_ = 0;
    int screen_rows_ = 0;

    // Bottom entry is the legacy (no-flags) mode and is never popped, so
    // kbd_flags() always has something to read.
    std::vector<int> kbd_stack_{0};
    static constexpr size_t kMaxKbdStack = 16;
    std::string pending_reply_;
    static constexpr size_t kMaxPendingReplyBytes = 4096;

    // Search state
    std::string search_query_;
    std::vector<SearchResult> search_matches_;
    int current_match_index_ = -1;
    bool search_active_ = false;
    bool enable_ligatures_ = true;
    
    // Selection state variables
    bool has_selection_ = false;
    int select_start_col_ = -1;
    int select_start_row_ = -1;
    int select_end_col_ = -1;
    int select_end_row_ = -1;
    bool selecting_ = false;
    bool alt_screen_active_ = false;
    bool cursor_visible_ = true;
    CursorShape cursor_shape_ = CursorShape::Block;
    bool bracketed_paste_active_ = false;
    bool synchronized_output_ = false;
    bool focus_reporting_ = false;
    int mouse_mode_ = 0;
    bool mouse_sgr_ = false;
    bool app_cursor_keys_ = false;
    bool alternate_scroll_ = true;
    int prompt_boundary_col_ = -1;

    // Cursor position & formatting attributes
    int cursor_col_ = 0;
    int cursor_row_ = 0;
    float visual_cursor_col_ = 0.0f;
    float visual_cursor_row_ = 0.0f;
    float error_glow_opacity_ = 0.0f;
    // The SDL_FColor pair stays for the public getters and the render path;
    // the packed mirror is what actually goes into cells, kept in sync by the
    // setters above so the hot write path never converts.
    SDL_FColor current_fg_ = {0.9f, 0.9f, 0.9f, 1.0f};
    SDL_FColor current_bg_ = {0.0f, 0.0f, 0.0f, 0.0f};
    PackedColor current_fg_packed_ = {230, 230, 230, 255};
    PackedColor current_bg_packed_ = {0, 0, 0, 0};
    uint8_t current_attrs_ = 0;
    uint32_t current_hyperlink_id_ = 0;

    // Hyperlink dedup table. hyperlink_uris_[id - 1] is the URI for `id`
    // (id 0 reserved to mean "no link"). Capped so a session that streams
    // an unbounded number of distinct URIs over hours/days can't grow this
    // forever -- same amortized-reset pattern the font atlas already uses
    // when it fills up. Cells referencing an id from a cleared generation
    // just resolve to "no URI" (get_hyperlink_uri returns empty), which is
    // a harmless degrade, not a crash.
    std::vector<std::string> hyperlink_uris_;
    std::unordered_map<std::string, uint32_t> hyperlink_id_by_uri_;
    static constexpr size_t kMaxHyperlinkTableSize = 100000;

    // Multi-codepoint cell contents, deduplicated: the same emoji appearing a
    // thousand times costs one entry. Deduplication is what keeps this small
    // enough not to need eviction, since nothing here is ever freed when
    // scrollback is trimmed -- a cell in history may still refer to it.
    //
    // Once full, new clusters stop being created and further marks are
    // dropped, which is what the code did for all of them before this existed.
    // The alternative -- clearing and reusing indices -- would make old cells
    // silently render whatever new text landed on their index, and showing the
    // wrong character is worse than showing one fewer mark.
    std::vector<std::u32string> clusters_;
    std::unordered_map<std::u32string, uint32_t> cluster_ids_;
    static constexpr size_t kMaxClusters = 100000;
    static constexpr size_t kMaxClusterLen = 32;

    // Set by a ZWJ, consumed by the character after it, which joins the cell
    // before rather than taking one of its own.
    bool zwj_pending_ = false;

    int combining_base_col() const;
    void append_to_cluster(int base_col, char32_t cp);
    
    // Batch rendering buffers
    std::vector<SDL_Vertex> bg_vertices_;
    std::vector<int> bg_indices_;
    std::vector<SDL_Vertex> text_vertices_;
    std::vector<int> text_indices_;
    std::vector<SDL_Vertex> dyn_text_vertices_;
    std::vector<int> dyn_text_indices_;
};
