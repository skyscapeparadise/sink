#pragma once

#include <SDL3/SDL.h>
#include <vector>
#include <string>
#include "font_manager.hpp"

struct Cell {
    char32_t codepoint;
    SDL_FColor fg;
    SDL_FColor bg;
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
    void clear_scrollback();
    void clear_line(int row, int mode); // 0 = cursor to end, 1 = start to cursor, 2 = entire line
    void delete_character(int count);
    
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

    void set_alt_screen(bool active) { alt_screen_active_ = active; }
    bool is_alt_screen_active() const { return alt_screen_active_; }

    void set_bracketed_paste(bool active) { bracketed_paste_active_ = active; }
    bool is_bracketed_paste_active() const { return bracketed_paste_active_; }
    
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
    bool bracketed_paste_active_ = false;
    int prompt_boundary_col_ = -1;

    // Cursor position & formatting attributes
    int cursor_col_ = 0;
    int cursor_row_ = 0;
    float visual_cursor_col_ = 0.0f;
    float visual_cursor_row_ = 0.0f;
    float error_glow_opacity_ = 0.0f;
    SDL_FColor current_fg_ = {0.9f, 0.9f, 0.9f, 1.0f};
    SDL_FColor current_bg_ = {0.0f, 0.0f, 0.0f, 0.0f};
    
    // Batch rendering buffers
    std::vector<SDL_Vertex> bg_vertices_;
    std::vector<int> bg_indices_;
    std::vector<SDL_Vertex> text_vertices_;
    std::vector<int> text_indices_;
    std::vector<SDL_Vertex> dyn_text_vertices_;
    std::vector<int> dyn_text_indices_;
};
