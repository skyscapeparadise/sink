#pragma once

#include <SDL3/SDL.h>
#include <vector>
#include <deque>
#include <string>
#include <unordered_map>
#include "font_manager.hpp"

// Per-cell SGR attribute flags. Bold is also folded into the color at parse
// time (base palette -> bright variant).
enum CellAttr : uint8_t {
    ATTR_BOLD          = 1 << 0,
    ATTR_DIM           = 1 << 1,
    ATTR_ITALIC        = 1 << 2,
    ATTR_UNDERLINE     = 1 << 3,
    ATTR_REVERSE       = 1 << 4,
    ATTR_STRIKETHROUGH = 1 << 5,
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
    void clear_line(int row, int mode); // 0 = cursor to end, 1 = start to cursor, 2 = entire line
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

    // Scrollback view control helpers
    void scroll_view(int delta);
    void reset_scroll();
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
    void update_timers(float dt);

    // Switches to/from the alternate screen buffer (DECSET/DECRST 1049 and
    // friends): the primary screen and cursor are saved on entry and restored
    // on exit, so quitting a full-screen app brings the shell contents back.
    void set_alt_screen(bool active);
    bool is_alt_screen_active() const { return alt_screen_active_; }

    void set_cursor_visible(bool visible) { cursor_visible_ = visible; }
    bool is_cursor_visible() const { return cursor_visible_; }

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
    const std::vector<bool>& get_row_wrapped() const { return row_wrapped_; }

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
    std::vector<bool> row_wrapped_;
    std::vector<bool> row_prompt_; // per active row, parallel to row_wrapped_
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
    std::vector<bool> saved_primary_row_wrapped_;
    std::vector<bool> saved_primary_row_prompt_;
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
    bool bracketed_paste_active_ = false;
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
    
    // Batch rendering buffers
    std::vector<SDL_Vertex> bg_vertices_;
    std::vector<int> bg_indices_;
    std::vector<SDL_Vertex> text_vertices_;
    std::vector<int> text_indices_;
    std::vector<SDL_Vertex> dyn_text_vertices_;
    std::vector<int> dyn_text_indices_;
};
