#include "terminal_grid.hpp"
#include <algorithm>
#include <iostream>
#include <string>

// Helper to write ASCII strings to the terminal grid
static void write_string(TerminalGrid& grid, int col, int row, const std::string& str, const SDL_FColor& fg, const SDL_FColor& bg) {
    int cur_col = col;
    int cur_row = row;
    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '\n') {
            cur_row++;
            cur_col = col;
            continue;
        }
        grid.set_cell(cur_col++, cur_row, static_cast<char32_t>(str[i]), fg, bg);
    }
}

static std::string utf32_to_utf8(char32_t codepoint) {
    std::string out;
    if (codepoint < 0x80) {
        out += static_cast<char>(codepoint);
    } else if (codepoint < 0x800) {
        out += static_cast<char>(0xC0 | (codepoint >> 6));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
        out += static_cast<char>(0xE0 | (codepoint >> 12));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x110000) {
        out += static_cast<char>(0xF0 | (codepoint >> 18));
        out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    return out;
}

TerminalGrid::TerminalGrid() {}

TerminalGrid::~TerminalGrid() {}

void TerminalGrid::update_timers(float dt) {
    if (error_glow_opacity_ > 0.0f) {
        error_glow_opacity_ -= dt * 2.0f;
        if (error_glow_opacity_ < 0.0f) {
            error_glow_opacity_ = 0.0f;
        }
    }
}

void TerminalGrid::resize(int cols, int rows) {
    if (cols == cols_ && rows == rows_) return;
    
    // If the grid is empty, initialize dimensions and return
    if (cols_ == 0 || rows_ == 0 || cells_.empty()) {
        cols_ = cols;
        rows_ = rows;
        Cell default_cell = { 32, {0.9f, 0.9f, 0.9f, 1.0f}, {0.0f, 0.0f, 0.0f, 0.0f} };
        cells_.resize(cols * rows, default_cell);
        row_wrapped_.resize(rows, false);
        return;
    }

    // 1. Gather all rows (history + active) into a raw row sequence
    struct RawRow {
        std::vector<Cell> cells;
        bool wrapped = false;
    };
    std::vector<RawRow> raw_rows;
    for (const auto& hist : scrollback_history_) {
        raw_rows.push_back({ hist.cells, hist.wrapped });
    }
    for (int r = 0; r < rows_; ++r) {
        std::vector<Cell> row_cells(cols_);
        for (int c = 0; c < cols_; ++c) {
            row_cells[c] = cells_[r * cols_ + c];
        }
        bool is_wrapped = (r < static_cast<int>(row_wrapped_.size())) ? row_wrapped_[r] : false;
        raw_rows.push_back({ std::move(row_cells), is_wrapped });
    }

    // Track the 1D index of the cursor cell in the raw rows stream
    size_t cursor_1d_index = (scrollback_history_.size() + cursor_row_) * cols_ + cursor_col_;
    int cursor_line_idx = -1;
    int cursor_col_idx = -1;

    // 2. Reconstruct logical lines by joining soft-wrapped rows
    std::vector<std::vector<Cell>> logical_lines;
    std::vector<Cell> current_line;
    for (size_t row_idx = 0; row_idx < raw_rows.size(); ++row_idx) {
        const auto& rr = raw_rows[row_idx];
        for (size_t col_idx = 0; col_idx < rr.cells.size(); ++col_idx) {
            size_t cell_1d = row_idx * cols_ + col_idx;
            if (cell_1d == cursor_1d_index) {
                cursor_line_idx = static_cast<int>(logical_lines.size());
                cursor_col_idx = static_cast<int>(current_line.size() + col_idx);
            }
        }
        current_line.insert(current_line.end(), rr.cells.begin(), rr.cells.end());
        if (!rr.wrapped) {
            // Hard break: strip trailing spaces to make wrapping/unwrapping cleaner
            while (!current_line.empty() && current_line.back().codepoint == 32 && current_line.back().bg.a == 0.0f) {
                current_line.pop_back();
            }
            // Clamp cursor position if it fell into the stripped area
            if (cursor_line_idx == static_cast<int>(logical_lines.size())) {
                if (cursor_col_idx > static_cast<int>(current_line.size())) {
                    cursor_col_idx = static_cast<int>(current_line.size());
                }
            }
            logical_lines.push_back(std::move(current_line));
            current_line.clear();
        }
    }
    if (!current_line.empty()) {
        if (cursor_line_idx == static_cast<int>(logical_lines.size())) {
            if (cursor_col_idx > static_cast<int>(current_line.size())) {
                cursor_col_idx = static_cast<int>(current_line.size());
            }
        }
        logical_lines.push_back(std::move(current_line));
    }

    // 3. Re-wrap all logical lines to the new width `cols`
    std::vector<RawRow> wrapped_rows;
    Cell space_cell = { 32, current_fg_, current_bg_ };
    
    int new_cursor_row_wrapped = -1;
    int new_cursor_col_wrapped = -1;
    int current_wrapped_row_idx = 0;

    for (size_t line_idx = 0; line_idx < logical_lines.size(); ++line_idx) {
        const auto& line = logical_lines[line_idx];
        if (line.empty()) {
            if (static_cast<int>(line_idx) == cursor_line_idx) {
                new_cursor_row_wrapped = current_wrapped_row_idx;
                new_cursor_col_wrapped = 0;
            }
            std::vector<Cell> empty_cells(cols, space_cell);
            wrapped_rows.push_back({ std::move(empty_cells), false });
            current_wrapped_row_idx++;
            continue;
        }
        size_t idx = 0;
        while (idx < line.size()) {
            size_t chunk_size = std::min(static_cast<size_t>(cols), line.size() - idx);
            std::vector<Cell> row_cells(cols, space_cell);
            for (size_t i = 0; i < chunk_size; ++i) {
                row_cells[i] = line[idx + i];
            }
            
            // Check if this chunk contains the cursor
            if (static_cast<int>(line_idx) == cursor_line_idx) {
                bool is_last_chunk = (idx + chunk_size == line.size());
                if ((cursor_col_idx >= static_cast<int>(idx) && cursor_col_idx < static_cast<int>(idx + chunk_size)) ||
                    (is_last_chunk && cursor_col_idx == static_cast<int>(line.size()))) {
                    new_cursor_row_wrapped = current_wrapped_row_idx;
                    new_cursor_col_wrapped = cursor_col_idx - static_cast<int>(idx);
                }
            }
            
            idx += chunk_size;
            bool is_wrapped = (idx < line.size());
            wrapped_rows.push_back({ std::move(row_cells), is_wrapped });
            current_wrapped_row_idx++;
        }
    }

    // 4. Distribute wrapped rows into new active cells and scrollback history
    std::vector<ScrollbackRow> new_history;
    std::vector<Cell> new_cells(cols * rows, space_cell);
    std::vector<bool> new_row_wrapped(rows, false);

    int active_start_idx = 0;
    if (static_cast<int>(wrapped_rows.size()) > rows) {
        active_start_idx = static_cast<int>(wrapped_rows.size()) - rows;
    }

    for (int i = 0; i < active_start_idx; ++i) {
        new_history.push_back({ std::move(wrapped_rows[i].cells), wrapped_rows[i].wrapped });
    }
    if (new_history.size() > max_scrollback_size_) {
        size_t prune_cnt = new_history.size() - max_scrollback_size_;
        new_history.erase(new_history.begin(), new_history.begin() + prune_cnt);
    }

    for (int r = 0; r < rows; ++r) {
        int idx = active_start_idx + r;
        if (idx < static_cast<int>(wrapped_rows.size())) {
            for (int c = 0; c < cols; ++c) {
                new_cells[r * cols + c] = wrapped_rows[idx].cells[c];
            }
            new_row_wrapped[r] = wrapped_rows[idx].wrapped;
        }
    }

    // Adjust scrollback offset tracking boundaries
    int prev_history_size = static_cast<int>(scrollback_history_.size());
    int new_history_size = static_cast<int>(new_history.size());
    if (scroll_offset_ > 0) {
        scroll_offset_ += (new_history_size - prev_history_size);
        if (scroll_offset_ < 0) scroll_offset_ = 0;
        if (scroll_offset_ > new_history_size) scroll_offset_ = new_history_size;
    }

    cells_ = std::move(new_cells);
    row_wrapped_ = std::move(new_row_wrapped);
    scrollback_history_ = std::move(new_history);
    cols_ = cols;
    rows_ = rows;

    if (new_cursor_row_wrapped >= active_start_idx) {
        cursor_row_ = new_cursor_row_wrapped - active_start_idx;
        cursor_col_ = new_cursor_col_wrapped;
    } else {
        cursor_row_ = 0;
        cursor_col_ = 0;
    }

    cursor_col_ = std::clamp(cursor_col_, 0, cols_ - 1);
    cursor_row_ = std::clamp(cursor_row_, 0, rows_ - 1);
}

void TerminalGrid::set_cell(int col, int row, char32_t codepoint, const SDL_FColor& fg, const SDL_FColor& bg) {
    if (col >= 0 && col < cols_ && row >= 0 && row < rows_) {
        cells_[row * cols_ + col] = { codepoint, fg, bg };
    }
}

void TerminalGrid::write_character(char32_t codepoint) {
    // Wrap to the next line immediately if the column limit is reached
    if (cursor_col_ >= cols_) {
        cursor_col_ = 0;
        if (cursor_row_ < static_cast<int>(row_wrapped_.size())) {
            row_wrapped_[cursor_row_] = true;
        }
        cursor_row_++;
        if (cursor_row_ >= rows_) {
            scroll_up();
            cursor_row_ = rows_ - 1;
        }
    }
    
    set_cell(cursor_col_, cursor_row_, codepoint, current_fg_, current_bg_);
    cursor_col_++;
}

void TerminalGrid::scroll_up() {
    if (rows_ <= 1) return;
    
    // Copy top row to scrollback history
    std::vector<Cell> top_row(cols_);
    for (int c = 0; c < cols_; ++c) {
        top_row[c] = cells_[c];
    }
    
    bool top_wrapped = (!row_wrapped_.empty()) ? row_wrapped_[0] : false;
    scrollback_history_.push_back({ std::move(top_row), top_wrapped });
    
    // Keep viewport locked to the same historical lines if we are scrolled up
    if (scroll_offset_ > 0) {
        scroll_offset_++;
    }
    
    if (scrollback_history_.size() > max_scrollback_size_) {
        scrollback_history_.erase(scrollback_history_.begin());
        if (scroll_offset_ > 0) {
            scroll_offset_--;
        }
    }
    
    // Shift row data up by one row
    std::copy(cells_.begin() + cols_, cells_.end(), cells_.begin());
    
    // Shift wrapped flags up by one row
    for (int r = 1; r < rows_; ++r) {
        row_wrapped_[r - 1] = row_wrapped_[r];
    }
    row_wrapped_[rows_ - 1] = false;
    
    // Fill the new last row with spaces using current style
    Cell empty_cell = { 32, current_fg_, current_bg_ };
    std::fill(cells_.begin() + (rows_ - 1) * cols_, cells_.end(), empty_cell);
}

void TerminalGrid::clear_screen() {
    Cell empty_cell = { 32, current_fg_, current_bg_ };
    std::fill(cells_.begin(), cells_.end(), empty_cell);
    std::fill(row_wrapped_.begin(), row_wrapped_.end(), false);
    cursor_col_ = 0;
    cursor_row_ = 0;
}

void TerminalGrid::clear_scrollback() {
    scrollback_history_.clear();
    scroll_offset_ = 0;
}

void TerminalGrid::clear_line(int row, int mode) {
    if (row < 0 || row >= rows_) return;
    
    int start_col = 0;
    int end_col = cols_;
    
    if (mode == 0) { // Cursor to end of line
        start_col = std::clamp(cursor_col_, 0, cols_ - 1);
    } else if (mode == 1) { // Start of line to cursor
        end_col = std::clamp(cursor_col_ + 1, 0, cols_);
    }
    
    Cell empty_cell = { 32, current_fg_, current_bg_ };
    std::fill(cells_.begin() + row * cols_ + start_col, cells_.begin() + row * cols_ + end_col, empty_cell);
}

void TerminalGrid::set_cursor_col(int col) {
    cursor_col_ = std::clamp(col, 0, cols_ - 1);
}

void TerminalGrid::set_cursor_row(int row) {
    cursor_row_ = std::clamp(row, 0, rows_ - 1);
}

void TerminalGrid::scroll_view(int delta) {
    scroll_offset_ += delta;
    int max_offset = static_cast<int>(scrollback_history_.size());
    if (scroll_offset_ < 0) scroll_offset_ = 0;
    if (scroll_offset_ > max_offset) scroll_offset_ = max_offset;
}

void TerminalGrid::reset_scroll() {
    scroll_offset_ = 0;
}

Cell TerminalGrid::get_cell_at(int col, int row) const {
    int total_history = static_cast<int>(scrollback_history_.size());
    int line_idx = row + (total_history - scroll_offset_);
    
    if (line_idx < total_history) {
        const auto& hist_row = scrollback_history_[line_idx].cells;
        if (col >= 0 && col < static_cast<int>(hist_row.size())) {
            return hist_row[col];
        }
        return Cell{ 32, current_fg_, current_bg_ };
    } else {
        int active_row = line_idx - total_history;
        if (col >= 0 && col < cols_ && active_row >= 0 && active_row < rows_) {
            return cells_[active_row * cols_ + col];
        }
        return Cell{ 32, current_fg_, current_bg_ };
    }
}

void TerminalGrid::initialize_mock_data() {
    clear_screen();
    if (cols_ < 40 || rows_ < 10) return;

    // Palette Colors
    SDL_FColor col_grey = {0.15f, 0.16f, 0.18f, 1.0f};
    SDL_FColor col_white = {1.0f, 1.0f, 1.0f, 1.0f};
    SDL_FColor col_cyan = {0.0f, 0.8f, 0.9f, 1.0f};
    SDL_FColor col_green = {0.1f, 0.9f, 0.2f, 1.0f};
    SDL_FColor col_yellow = {0.9f, 0.8f, 0.1f, 1.0f};
    SDL_FColor col_magenta = {0.9f, 0.2f, 0.8f, 1.0f};
    SDL_FColor col_red = {0.9f, 0.1f, 0.2f, 1.0f};
    SDL_FColor col_blue = {0.2f, 0.4f, 1.0f, 1.0f};
    SDL_FColor col_dark_blue = {0.05f, 0.08f, 0.15f, 0.5f};

    // 1. Top Window Header Bar
    for (int c = 0; c < cols_; ++c) {
        set_cell(c, 0, ' ', col_white, col_grey);
    }
    write_string(*this, 1, 0, "[o][x][-] sink - /Users/kady/Projects/sink (zsh)", {0.8f, 0.8f, 0.8f, 1.0f}, col_grey);

    // 2. Mock Command and Build Output
    write_string(*this, 1, 2, "kady@macbook sink % cmake -B build", col_cyan, {0,0,0,0});
    write_string(*this, 1, 3, "-- The CXX compiler identification is AppleClang 15.0.0", col_white, {0,0,0,0});
    write_string(*this, 1, 4, "-- Detecting CXX compiler ABI info - done", col_white, {0,0,0,0});
    write_string(*this, 1, 5, "-- Found SDL3: /opt/homebrew/lib (found version \"3.1.3\")", col_white, {0,0,0,0});
    write_string(*this, 1, 6, "-- Found SDL3_ttf: /opt/homebrew/lib (found version \"3.2.2\")", col_white, {0,0,0,0});
    write_string(*this, 1, 7, "-- Configuring complete", col_white, {0,0,0,0});
    write_string(*this, 1, 8, "-- Generating complete", col_white, {0,0,0,0});

    write_string(*this, 1, 10, "kady@macbook sink % cmake --build build --config Release", col_cyan, {0,0,0,0});
    write_string(*this, 1, 11, "[ 33%] Building CXX object src/font_manager.cpp.o", col_white, {0,0,0,0});
    write_string(*this, 1, 12, "[ 66%] Building CXX object src/terminal_grid.cpp.o", col_white, {0,0,0,0});
    write_string(*this, 1, 13, "[100%] Linking CXX executable sink_terminal", col_white, {0,0,0,0});
    write_string(*this, 1, 14, "[100%] Built target sink_terminal", col_green, {0,0,0,0});

    write_string(*this, 1, 16, "kady@macbook sink % ./build/sink_terminal --verify", col_cyan, {0,0,0,0});
    write_string(*this, 1, 17, "[STATUS] Running core engine verification:", col_yellow, {0,0,0,0});
    write_string(*this, 3, 18, "- SDL3 Initialization....... [ OK ]", col_green, {0,0,0,0});
    write_string(*this, 3, 19, "- SDL_ttf Library Load...... [ OK ]", col_green, {0,0,0,0});
    write_string(*this, 3, 20, "- High-res Font Atlas....... [ OK ]", col_green, {0,0,0,0});
    write_string(*this, 3, 21, "- GPU Batch Draw Calls...... [ OK ] (2 draw calls, 1200+ FPS)", col_green, {0,0,0,0});

    // 3. System Status Panel on the right (cols_ >= 80)
    if (cols_ >= 80) {
        int panel_col = cols_ - 36;
        for (int r = 2; r < 14; ++r) {
            for (int c = panel_col; c < cols_ - 1; ++c) {
                set_cell(c, r, ' ', col_white, col_dark_blue);
            }
        }
        write_string(*this, panel_col + 2, 3, "SYSTEM STATUS", col_cyan, col_dark_blue);
        write_string(*this, panel_col + 2, 4, "==============================", col_cyan, col_dark_blue);
        write_string(*this, panel_col + 2, 6, "CPU [|||||||||||||||||       ] 68%", col_yellow, col_dark_blue);
        write_string(*this, panel_col + 2, 7, "RAM [||||||||||              ] 41%", col_green, col_dark_blue);
        write_string(*this, panel_col + 2, 8, "GPU [|||||||                 ] 29%", col_green, col_dark_blue);
        write_string(*this, panel_col + 2, 10, "Display: HDR Mode Active (BT.2020)", col_white, col_dark_blue);
        write_string(*this, panel_col + 2, 11, "Renderer: Metal (Hardware Accel)", col_white, col_dark_blue);
        write_string(*this, panel_col + 2, 12, "Background: Dynamic Video Loop", col_white, col_dark_blue);
    }

    // 4. Color Palette Test (near bottom)
    if (rows_ >= 28) {
        int palette_row = 24;
        write_string(*this, 1, palette_row, "Palette test:", col_white, {0,0,0,0});
        
        std::vector<SDL_FColor> palette = {
            {0.0f, 0.0f, 0.0f, 1.0f}, {0.8f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.8f, 0.0f, 1.0f}, {0.8f, 0.8f, 0.0f, 1.0f},
            {0.0f, 0.0f, 0.8f, 1.0f}, {0.8f, 0.0f, 0.8f, 1.0f}, {0.0f, 0.8f, 0.8f, 1.0f}, {0.8f, 0.8f, 0.8f, 1.0f},
            {0.5f, 0.5f, 0.5f, 1.0f}, {1.0f, 0.3f, 0.3f, 1.0f}, {0.3f, 1.0f, 0.3f, 1.0f}, {1.0f, 1.0f, 0.3f, 1.0f},
            {0.3f, 0.3f, 1.0f, 1.0f}, {1.0f, 0.3f, 1.0f, 1.0f}, {0.3f, 1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}
        };

        for (size_t i = 0; i < palette.size(); ++i) {
            int c = 15 + static_cast<int>(i) * 3;
            set_cell(c, palette_row, ' ', col_white, palette[i]);
            set_cell(c + 1, palette_row, ' ', col_white, palette[i]);
        }
    }

    // 5. Bottom Status Line
    for (int c = 0; c < cols_; ++c) {
        set_cell(c, rows_ - 1, ' ', col_white, col_grey);
    }
    write_string(*this, 1, rows_ - 1, " [Zsh]   1:sink_terminal*  2:config  3:bash     utf-8   macOS   12:18 PM ", {0.9f, 0.9f, 0.9f, 1.0f}, col_grey);

    cursor_col_ = 28;
    cursor_row_ = 16;
}

void TerminalGrid::render(SDL_Renderer* renderer, const FontManager& font_manager, float start_x, float start_y, float display_scale, float dt, bool animated_typing) {
    float cell_w = font_manager.get_cell_width() + 1.0f * display_scale;
    float cell_h = font_manager.get_cell_height() - 0.8f * display_scale;
    SDL_Texture* atlas = font_manager.get_atlas_texture();
    if (!atlas) return;

    SDL_Texture* dyn_atlas = font_manager.get_dynamic_atlas_texture();

    int win_w = 0, win_h = 0;
    SDL_GetRenderOutputSize(renderer, &win_w, &win_h);

    float atlas_w = 0.0f;
    float atlas_h = 0.0f;
    if (!SDL_GetTextureSize(atlas, &atlas_w, &atlas_h)) {
        return;
    }

    float dyn_atlas_w = 0.0f;
    float dyn_atlas_h = 0.0f;
    if (dyn_atlas) {
        SDL_GetTextureSize(dyn_atlas, &dyn_atlas_w, &dyn_atlas_h);
    }

    bg_vertices_.clear();
    bg_indices_.clear();
    text_vertices_.clear();
    text_indices_.clear();
    dyn_text_vertices_.clear();
    dyn_text_indices_.clear();
    

    // 1. Draw Grid Cells
    for (int r = 0; r < rows_; ++r) {
        for (int c = 0; c < cols_; ++c) {
            Cell cell = get_cell_at(c, r);

            float x0 = start_x + c * cell_w;
            float y0 = start_y + r * cell_h;
            float x1 = x0 + cell_w;
            float y1 = y0 + cell_h;

            // Populate Background Geometry
            SDL_FColor bg_color = cell.bg;
            bool selected = is_cell_selected(c, r);
            if (selected) {
                bg_color = { 1.00f, 0.60f, 0.00f, 0.35f }; // Premium Translucent Amber Gold Selection
            }

            if (bg_color.a > 0.0f) {
                int base_idx = static_cast<int>(bg_vertices_.size());
                
                bg_vertices_.push_back({ {x0, y0}, bg_color, {0.0f, 0.0f} });
                bg_vertices_.push_back({ {x1, y0}, bg_color, {0.0f, 0.0f} });
                bg_vertices_.push_back({ {x0, y1}, bg_color, {0.0f, 0.0f} });
                bg_vertices_.push_back({ {x1, y1}, bg_color, {0.0f, 0.0f} });

                bg_indices_.push_back(base_idx + 0);
                bg_indices_.push_back(base_idx + 1);
                bg_indices_.push_back(base_idx + 2);
                bg_indices_.push_back(base_idx + 2);
                bg_indices_.push_back(base_idx + 1);
                bg_indices_.push_back(base_idx + 3);
            }

            // Populate Text Geometry
            if (cell.codepoint != 32 && cell.codepoint != 0) {
                const GlyphInfo* glyph = font_manager.get_glyph(renderer, cell.codepoint);
                if (glyph && glyph->src_rect.w > 0.0f && glyph->src_rect.h > 0.0f) {
                    bool is_dynamic = (cell.codepoint < 32 || cell.codepoint > 126);
                    float tex_w = is_dynamic ? dyn_atlas_w : atlas_w;
                    float tex_h = is_dynamic ? dyn_atlas_h : atlas_h;

                    if (tex_w > 0.0f && tex_h > 0.0f) {
                        float u0 = glyph->src_rect.x / tex_w;
                        float v0 = glyph->src_rect.y / tex_h;
                        float u1 = (glyph->src_rect.x + glyph->src_rect.w) / tex_w;
                        float v1 = (glyph->src_rect.y + glyph->src_rect.h) / tex_h;

                        float glyph_w = glyph->src_rect.w;
                        float glyph_h = glyph->src_rect.h;

                        if (glyph->is_color) {
                            float target_w = cell_w;
                            float scale_factor = target_w / glyph_w;
                            glyph_w = target_w;
                            glyph_h *= scale_factor;
                        }

                        float gx0 = x0 + (cell_w - glyph_w) / 2.0f;
                        float gy0 = y0 + (cell_h - glyph_h) / 2.0f;
                        float gx1 = gx0 + glyph_w;
                        float gy1 = gy0 + glyph_h;

                        SDL_FColor render_color = cell.fg;
                        if (glyph->is_color) {
                            render_color = {1.0f, 1.0f, 1.0f, 1.0f}; // Don't color-tint color emojis
                        }

                        if (is_dynamic) {
                            int base_idx = static_cast<int>(dyn_text_vertices_.size());
                            dyn_text_vertices_.push_back({ {gx0, gy0}, render_color, {u0, v0} });
                            dyn_text_vertices_.push_back({ {gx1, gy0}, render_color, {u1, v0} });
                            dyn_text_vertices_.push_back({ {gx0, gy1}, render_color, {u0, v1} });
                            dyn_text_vertices_.push_back({ {gx1, gy1}, render_color, {u1, v1} });

                            dyn_text_indices_.push_back(base_idx + 0);
                            dyn_text_indices_.push_back(base_idx + 1);
                            dyn_text_indices_.push_back(base_idx + 2);
                            dyn_text_indices_.push_back(base_idx + 2);
                            dyn_text_indices_.push_back(base_idx + 1);
                            dyn_text_indices_.push_back(base_idx + 3);
                        } else {
                            int base_idx = static_cast<int>(text_vertices_.size());
                            text_vertices_.push_back({ {gx0, gy0}, render_color, {u0, v0} });
                            text_vertices_.push_back({ {gx1, gy0}, render_color, {u1, v0} });
                            text_vertices_.push_back({ {gx0, gy1}, render_color, {u0, v1} });
                            text_vertices_.push_back({ {gx1, gy1}, render_color, {u1, v1} });

                            text_indices_.push_back(base_idx + 0);
                            text_indices_.push_back(base_idx + 1);
                            text_indices_.push_back(base_idx + 2);
                            text_indices_.push_back(base_idx + 2);
                            text_indices_.push_back(base_idx + 1);
                            text_indices_.push_back(base_idx + 3);
                        }
                    }
                }
            }
        }
    }

    // 2. Populate Padding Margin Geometry to eliminate gaps and smearing parallel lines
    if (cols_ > 0 && rows_ > 0) {
        float grid_w = cols_ * cell_w;
        float grid_h = rows_ * cell_h;
        float end_x = start_x + grid_w;
        float end_y = start_y + grid_h;

        SDL_FColor tl_color = get_cell_at(0, 0).bg;
        SDL_FColor tr_color = get_cell_at(cols_ - 1, 0).bg;
        SDL_FColor bl_color = get_cell_at(0, rows_ - 1).bg;
        SDL_FColor br_color = get_cell_at(cols_ - 1, rows_ - 1).bg;

        // Top Margin (fills full width, from y=0 to y=start_y)
        if (tl_color.a > 0.0f) {
            int base_idx = static_cast<int>(bg_vertices_.size());
            bg_vertices_.push_back({ {0.0f, 0.0f}, tl_color, {0.0f, 0.0f} });
            bg_vertices_.push_back({ {static_cast<float>(win_w), 0.0f}, tl_color, {0.0f, 0.0f} });
            bg_vertices_.push_back({ {0.0f, start_y}, tl_color, {0.0f, 0.0f} });
            bg_vertices_.push_back({ {static_cast<float>(win_w), start_y}, tl_color, {0.0f, 0.0f} });
            
            bg_indices_.push_back(base_idx + 0);
            bg_indices_.push_back(base_idx + 1);
            bg_indices_.push_back(base_idx + 2);
            bg_indices_.push_back(base_idx + 2);
            bg_indices_.push_back(base_idx + 1);
            bg_indices_.push_back(base_idx + 3);
        }

        // Bottom Margin (fills full width, from y=end_y to y=win_h)
        if (bl_color.a > 0.0f) {
            int base_idx = static_cast<int>(bg_vertices_.size());
            bg_vertices_.push_back({ {0.0f, end_y}, bl_color, {0.0f, 0.0f} });
            bg_vertices_.push_back({ {static_cast<float>(win_w), end_y}, bl_color, {0.0f, 0.0f} });
            bg_vertices_.push_back({ {0.0f, static_cast<float>(win_h)}, bl_color, {0.0f, 0.0f} });
            bg_vertices_.push_back({ {static_cast<float>(win_w), static_cast<float>(win_h)}, bl_color, {0.0f, 0.0f} });
            
            bg_indices_.push_back(base_idx + 0);
            bg_indices_.push_back(base_idx + 1);
            bg_indices_.push_back(base_idx + 2);
            bg_indices_.push_back(base_idx + 2);
            bg_indices_.push_back(base_idx + 1);
            bg_indices_.push_back(base_idx + 3);
        }

        // Left Margin (from y=start_y to y=end_y, x=0 to x=start_x)
        if (tl_color.a > 0.0f) {
            int base_idx = static_cast<int>(bg_vertices_.size());
            bg_vertices_.push_back({ {0.0f, start_y}, tl_color, {0.0f, 0.0f} });
            bg_vertices_.push_back({ {start_x, start_y}, tl_color, {0.0f, 0.0f} });
            bg_vertices_.push_back({ {0.0f, end_y}, tl_color, {0.0f, 0.0f} });
            bg_vertices_.push_back({ {start_x, end_y}, tl_color, {0.0f, 0.0f} });
            
            bg_indices_.push_back(base_idx + 0);
            bg_indices_.push_back(base_idx + 1);
            bg_indices_.push_back(base_idx + 2);
            bg_indices_.push_back(base_idx + 2);
            bg_indices_.push_back(base_idx + 1);
            bg_indices_.push_back(base_idx + 3);
        }

        // Right Margin (from y=start_y to y=end_y, x=end_x to x=win_w)
        if (tr_color.a > 0.0f) {
            int base_idx = static_cast<int>(bg_vertices_.size());
            bg_vertices_.push_back({ {end_x, start_y}, tr_color, {0.0f, 0.0f} });
            bg_vertices_.push_back({ {static_cast<float>(win_w), start_y}, tr_color, {0.0f, 0.0f} });
            bg_vertices_.push_back({ {end_x, end_y}, tr_color, {0.0f, 0.0f} });
            bg_vertices_.push_back({ {static_cast<float>(win_w), end_y}, tr_color, {0.0f, 0.0f} });
            
            bg_indices_.push_back(base_idx + 0);
            bg_indices_.push_back(base_idx + 1);
            bg_indices_.push_back(base_idx + 2);
            bg_indices_.push_back(base_idx + 2);
            bg_indices_.push_back(base_idx + 1);
            bg_indices_.push_back(base_idx + 3);
        }
    }

    // Update visual animated cursor position
    float target_col = static_cast<float>(cursor_col_);
    float target_row = static_cast<float>(cursor_row_ + scroll_offset_);

    float diff_col = std::abs(target_col - visual_cursor_col_);
    float diff_row = std::abs(target_row - visual_cursor_row_);

    if (!animated_typing || diff_col > 3.0f || diff_row > 1.0f) {
        visual_cursor_col_ = target_col;
        visual_cursor_row_ = target_row;
    } else {
        visual_cursor_col_ += (target_col - visual_cursor_col_) * 25.0f * dt;
        visual_cursor_row_ += (target_row - visual_cursor_row_) * 25.0f * dt;
    }

    // 2. Render Opaque Block Cursor (Append to background draw call)
    if (visual_cursor_col_ >= 0.0f && visual_cursor_col_ < cols_ && visual_cursor_row_ >= 0.0f && visual_cursor_row_ < rows_) {
        float cx0 = start_x + visual_cursor_col_ * cell_w;
        float cy0 = start_y + visual_cursor_row_ * cell_h;
        float cx1 = cx0 + cell_w;
        float cy1 = cy0 + cell_h;
        
        SDL_FColor cursor_color = {1.0f, 1.0f, 1.0f, 1.0f}; // Solid, fully opaque white block cursor
        int base_idx = static_cast<int>(bg_vertices_.size());
        
        bg_vertices_.push_back({ {cx0, cy0}, cursor_color, {0.0f, 0.0f} });
        bg_vertices_.push_back({ {cx1, cy0}, cursor_color, {0.0f, 0.0f} });
        bg_vertices_.push_back({ {cx0, cy1}, cursor_color, {0.0f, 0.0f} });
        bg_vertices_.push_back({ {cx1, cy1}, cursor_color, {0.0f, 0.0f} });

        bg_indices_.push_back(base_idx + 0);
        bg_indices_.push_back(base_idx + 1);
        bg_indices_.push_back(base_idx + 2);
        bg_indices_.push_back(base_idx + 2);
        bg_indices_.push_back(base_idx + 1);
        bg_indices_.push_back(base_idx + 3);
    }

    // 3. Draw Background Color Rectangles
    if (!bg_vertices_.empty()) {
        SDL_RenderGeometry(renderer, nullptr, bg_vertices_.data(), static_cast<int>(bg_vertices_.size()), bg_indices_.data(), static_cast<int>(bg_indices_.size()));
    }



    // 5. Draw Final Crisp Text Glyphs
    if (!text_vertices_.empty()) {
        SDL_RenderGeometry(renderer, atlas, text_vertices_.data(), static_cast<int>(text_vertices_.size()), text_indices_.data(), static_cast<int>(text_indices_.size()));
    }
    if (dyn_atlas && !dyn_text_vertices_.empty()) {
        SDL_RenderGeometry(renderer, dyn_atlas, dyn_text_vertices_.data(), static_cast<int>(dyn_text_vertices_.size()), dyn_text_indices_.data(), static_cast<int>(dyn_text_indices_.size()));
    }

    // 6. Draw Translucent macOS Scrollbar Overlay
    if (win_w > 0 && win_h > 0) {
        int total_history = static_cast<int>(scrollback_history_.size());
        int total_rows = total_history + rows_;
        
        if (total_rows > rows_) {
            float track_y = 8.0f * display_scale;
            float track_h = static_cast<float>(win_h) - 16.0f * display_scale;
            
            float thumb_h = std::max(24.0f * display_scale, track_h * (static_cast<float>(rows_) / total_rows));
            
            float frac = 0.0f;
            if (total_history > 0) {
                frac = static_cast<float>(scroll_offset_) / total_history;
            }
            float thumb_y = track_y + (track_h - thumb_h) * (1.0f - frac);
            
            float thumb_w = 6.0f * display_scale;
            float thumb_x = static_cast<float>(win_w) - 10.0f * display_scale;
            
            SDL_FRect thumb_rect = { thumb_x, thumb_y, thumb_w, thumb_h };
            
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 60); // Translucent white (25% alpha capsule)
            SDL_RenderFillRect(renderer, &thumb_rect);
        }
    }

    // 7. Render Soft Red Error Glow Vignette around screen borders
    if (error_glow_opacity_ > 0.0f && win_w > 0 && win_h > 0) {
        SDL_Vertex glow_vertices[8];
        SDL_FColor red_outer = { 1.0f, 0.15f, 0.20f, 0.25f * error_glow_opacity_ }; // Soft dark red edge
        SDL_FColor red_inner = { 1.0f, 0.15f, 0.20f, 0.0f };                      // Fades to fully transparent
        
        float border_d = 24.0f * display_scale;
        float w = static_cast<float>(win_w);
        float h = static_cast<float>(win_h);
        
        // Top edge
        {
            SDL_Vertex verts[4] = {
                { {0.0f, 0.0f}, red_outer, {0.0f, 0.0f} },
                { {w, 0.0f}, red_outer, {0.0f, 0.0f} },
                { {0.0f, border_d}, red_inner, {0.0f, 0.0f} },
                { {w, border_d}, red_inner, {0.0f, 0.0f} }
            };
            int ind[6] = { 0, 1, 2, 2, 1, 3 };
            SDL_RenderGeometry(renderer, nullptr, verts, 4, ind, 6);
        }
        // Bottom edge
        {
            SDL_Vertex verts[4] = {
                { {0.0f, h - border_d}, red_inner, {0.0f, 0.0f} },
                { {w, h - border_d}, red_inner, {0.0f, 0.0f} },
                { {0.0f, h}, red_outer, {0.0f, 0.0f} },
                { {w, h}, red_outer, {0.0f, 0.0f} }
            };
            int ind[6] = { 0, 1, 2, 2, 1, 3 };
            SDL_RenderGeometry(renderer, nullptr, verts, 4, ind, 6);
        }
        // Left edge
        {
            SDL_Vertex verts[4] = {
                { {0.0f, 0.0f}, red_outer, {0.0f, 0.0f} },
                { {border_d, 0.0f}, red_inner, {0.0f, 0.0f} },
                { {0.0f, h}, red_outer, {0.0f, 0.0f} },
                { {border_d, h}, red_inner, {0.0f, 0.0f} }
            };
            int ind[6] = { 0, 1, 2, 2, 1, 3 };
            SDL_RenderGeometry(renderer, nullptr, verts, 4, ind, 6);
        }
        // Right edge
        {
            SDL_Vertex verts[4] = {
                { {w - border_d, 0.0f}, red_inner, {0.0f, 0.0f} },
                { {w, 0.0f}, red_outer, {0.0f, 0.0f} },
                { {w - border_d, h}, red_inner, {0.0f, 0.0f} },
                { {w, h}, red_outer, {0.0f, 0.0f} }
            };
            int ind[6] = { 0, 1, 2, 2, 1, 3 };
            SDL_RenderGeometry(renderer, nullptr, verts, 4, ind, 6);
        }
    }
}

void TerminalGrid::start_selection(int col, int row) {
    int total_history = static_cast<int>(scrollback_history_.size());
    int grid_row = row + (total_history - scroll_offset_);
    
    select_start_col_ = std::clamp(col, 0, cols_ - 1);
    select_start_row_ = std::clamp(grid_row, 0, total_history + rows_ - 1);
    select_end_col_ = select_start_col_;
    select_end_row_ = select_start_row_;
    selecting_ = true;
    has_selection_ = true;
}

void TerminalGrid::update_selection(int col, int row) {
    if (!selecting_) return;
    int total_history = static_cast<int>(scrollback_history_.size());
    int grid_row = row + (total_history - scroll_offset_);
    
    select_end_col_ = std::clamp(col, 0, cols_ - 1);
    select_end_row_ = std::clamp(grid_row, 0, total_history + rows_ - 1);
}

void TerminalGrid::end_selection() {
    selecting_ = false;
    // If it's a single cell click, discard selection highlight
    if (select_start_col_ == select_end_col_ && select_start_row_ == select_end_row_) {
        clear_selection();
    }
}

void TerminalGrid::clear_selection() {
    has_selection_ = false;
    selecting_ = false;
    select_start_col_ = -1;
    select_start_row_ = -1;
    select_end_col_ = -1;
    select_end_row_ = -1;
}

static bool is_word_delimiter(char32_t c) {
    if (c <= 32) return true;
    const std::string delim = " \t!\"#$%&'()*+,./:;<=>?@[\\]^`{|}~";
    if (c < 128) {
        return delim.find(static_cast<char>(c)) != std::string::npos;
    }
    return false;
}

void TerminalGrid::select_word_at(int col, int row) {
    int total_history = static_cast<int>(scrollback_history_.size());
    int grid_row = row + (total_history - scroll_offset_);
    
    if (grid_row < 0 || grid_row >= total_history + rows_) return;
    
    int start = std::clamp(col, 0, cols_ - 1);
    Cell clicked_cell = get_cell_at(start, row);
    
    if (!is_word_delimiter(clicked_cell.codepoint)) {
        while (start > 0) {
            Cell cell = get_cell_at(start - 1, row);
            if (is_word_delimiter(cell.codepoint)) {
                break;
            }
            start--;
        }
        
        int end = std::clamp(col, 0, cols_ - 1);
        while (end < cols_ - 1) {
            Cell cell = get_cell_at(end + 1, row);
            if (is_word_delimiter(cell.codepoint)) {
                break;
            }
            end++;
        }
        
        select_start_col_ = start;
        select_end_col_ = end;
    } else {
        select_start_col_ = start;
        select_end_col_ = start;
    }
    
    select_start_row_ = grid_row;
    select_end_row_ = grid_row;
    has_selection_ = true;
    selecting_ = false;
}

void TerminalGrid::select_line_at(int row) {
    int total_history = static_cast<int>(scrollback_history_.size());
    int grid_row = row + (total_history - scroll_offset_);
    
    if (grid_row < 0 || grid_row >= total_history + rows_) return;
    
    select_start_col_ = 0;
    select_end_col_ = cols_ - 1;
    select_start_row_ = grid_row;
    select_end_row_ = grid_row;
    has_selection_ = true;
    selecting_ = false;
}

bool TerminalGrid::is_cell_selected(int col, int row) const {
    if (!has_selection_) return false;
    
    int total_history = static_cast<int>(scrollback_history_.size());
    int grid_row = row + (total_history - scroll_offset_);
    
    int r0 = select_start_row_;
    int c0 = select_start_col_;
    int r1 = select_end_row_;
    int c1 = select_end_col_;
    
    if (r0 > r1 || (r0 == r1 && c0 > c1)) {
        std::swap(r0, r1);
        std::swap(c0, c1);
    }
    
    if (grid_row > r0 && grid_row < r1) return true;
    if (grid_row == r0 && grid_row == r1) return (col >= c0 && col <= c1);
    if (grid_row == r0 && grid_row < r1) return (col >= c0);
    if (grid_row == r1 && grid_row > r0) return (col <= c1);
    
    return false;
}

std::string TerminalGrid::get_selected_text() const {
    if (!has_selection_) return "";
    
    int r0 = select_start_row_;
    int c0 = select_start_col_;
    int r1 = select_end_row_;
    int c1 = select_end_col_;
    
    if (r0 > r1 || (r0 == r1 && c0 > c1)) {
        std::swap(r0, r1);
        std::swap(c0, c1);
    }
    
    int total_history = static_cast<int>(scrollback_history_.size());
    std::string text;
    
    for (int r = r0; r <= r1; ++r) {
        int sc = (r == r0) ? c0 : 0;
        int ec = (r == r1) ? c1 : cols_ - 1;
        
        bool is_wrapped = false;
        std::vector<Cell> row_cells;
        
        if (r < total_history) {
            row_cells = scrollback_history_[r].cells;
            is_wrapped = scrollback_history_[r].wrapped;
        } else {
            int active_row = r - total_history;
            if (active_row >= 0 && active_row < rows_) {
                row_cells.resize(cols_);
                for (int c = 0; c < cols_; ++c) {
                    row_cells[c] = cells_[active_row * cols_ + c];
                }
                is_wrapped = (active_row < static_cast<int>(row_wrapped_.size())) ? row_wrapped_[active_row] : false;
            }
        }
        
        if (!row_cells.empty()) {
            // Strip trailing spaces on last row if it has a hard newline
            int limit_col = ec;
            if (!is_wrapped && r == r1) {
                while (limit_col >= sc && row_cells[limit_col].codepoint == 32 && row_cells[limit_col].bg.a == 0.0f) {
                    limit_col--;
                }
            }
            
            for (int c = sc; c <= limit_col; ++c) {
                text += utf32_to_utf8(row_cells[c].codepoint);
            }
            
            if (!is_wrapped && r < r1) {
                text += "\n";
            }
        }
    }
    return text;
}

void TerminalGrid::select_all() {
    has_selection_ = true;
    selecting_ = false;
    select_start_row_ = 0;
    select_start_col_ = 0;
    select_end_row_ = static_cast<int>(scrollback_history_.size() + rows_ - 1);
    select_end_col_ = cols_ - 1;
}

std::string TerminalGrid::get_all_text() const {
    int total_history = static_cast<int>(scrollback_history_.size());
    int total_rows = total_history + rows_;
    std::string text;
    
    for (int r = 0; r < total_rows; ++r) {
        bool is_wrapped = false;
        std::vector<Cell> row_cells;
        
        if (r < total_history) {
            row_cells = scrollback_history_[r].cells;
            is_wrapped = scrollback_history_[r].wrapped;
        } else {
            int active_row = r - total_history;
            if (active_row >= 0 && active_row < rows_) {
                row_cells.resize(cols_);
                for (int c = 0; c < cols_; ++c) {
                    row_cells[c] = cells_[active_row * cols_ + c];
                }
                is_wrapped = (active_row < static_cast<int>(row_wrapped_.size())) ? row_wrapped_[active_row] : false;
            }
        }
        
        if (!row_cells.empty()) {
            int limit_col = cols_ - 1;
            // Trim trailing spaces if the row has a hard newline
            if (!is_wrapped) {
                while (limit_col >= 0 && row_cells[limit_col].codepoint == 32 && row_cells[limit_col].bg.a == 0.0f) {
                    limit_col--;
                }
            }
            
            for (int c = 0; c <= limit_col; ++c) {
                text += utf32_to_utf8(row_cells[c].codepoint);
            }
            
            if (!is_wrapped && r < total_rows - 1) {
                text += "\n";
            }
        }
    }
    return text;
}
