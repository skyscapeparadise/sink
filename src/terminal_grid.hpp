#pragma once

#include <SDL3/SDL.h>
#include <vector>
#include <string>
#include "font_manager.hpp"

// Per-cell SGR attribute flags. Bold is also folded into the color at parse
// time (base palette -> bright variant); the flag is kept for a future bold
// font face. Italic is parsed but not yet rendered.
enum CellAttr : uint8_t {
    ATTR_BOLD          = 1 << 0,
    ATTR_DIM           = 1 << 1,
    ATTR_ITALIC        = 1 << 2,
    ATTR_UNDERLINE     = 1 << 3,
    ATTR_REVERSE       = 1 << 4,
    ATTR_STRIKETHROUGH = 1 << 5,
};

struct Cell {
    char32_t codepoint;
    SDL_FColor fg;
    SDL_FColor bg;
    uint8_t attrs = 0;
};

struct ScrollbackRow {
    std::vector<Cell> cells;
    bool wrapped = false;
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

    void set_current_fg(const SDL_FColor& fg) { current_fg_ = fg; }
    void set_current_bg(const SDL_FColor& bg) { current_bg_ = bg; }
    const SDL_FColor& get_current_fg() const { return current_fg_; }
    const SDL_FColor& get_current_bg() const { return current_bg_; }
    void set_current_attrs(uint8_t attrs) { current_attrs_ = attrs; }
    uint8_t get_current_attrs() const { return current_attrs_; }

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

private:
    int cols_ = 0;
    int rows_ = 0;
    std::vector<Cell> cells_;
    std::vector<bool> row_wrapped_;
    bool wrap_pending_ = false;
    int saved_cursor_col_ = 0;
    int saved_cursor_row_ = 0;

    // DECSTBM margins (0-based inclusive). scroll_bottom_ is re-anchored to
    // rows_-1 on every resize; helpers that never touch scrollback do the
    // actual row movement for partial-region scrolls, IL and DL.
    int scroll_top_ = 0;
    int scroll_bottom_ = 0;
    void shift_rows_up(int top, int bottom, int count);
    void shift_rows_down(int top, int bottom, int count);

    // Primary screen contents stashed while the alternate screen is active.
    // Saved with its own geometry: if the window is resized mid-app, restore
    // clamp-copies whatever still fits rather than reflowing.
    std::vector<Cell> saved_primary_cells_;
    std::vector<bool> saved_primary_row_wrapped_;
    int saved_primary_cols_ = 0;
    int saved_primary_rows_ = 0;
    int saved_primary_cursor_col_ = 0;
    int saved_primary_cursor_row_ = 0;
    bool saved_primary_wrap_pending_ = false;
    
    // Scrollback history buffers
    std::vector<ScrollbackRow> scrollback_history_;
    int scroll_offset_ = 0;
    float display_scroll_offset_ = 0.0f; // Smooth sub-pixel interpolated scroll offset
    const size_t max_scrollback_size_ = 2000; // Store up to 2000 lines of scrollback history
    
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
    SDL_FColor current_fg_ = {0.9f, 0.9f, 0.9f, 1.0f};
    SDL_FColor current_bg_ = {0.0f, 0.0f, 0.0f, 0.0f};
    uint8_t current_attrs_ = 0;
    
    // Batch rendering buffers
    std::vector<SDL_Vertex> bg_vertices_;
    std::vector<int> bg_indices_;
    std::vector<SDL_Vertex> text_vertices_;
    std::vector<int> text_indices_;
    std::vector<SDL_Vertex> dyn_text_vertices_;
    std::vector<int> dyn_text_indices_;
};
