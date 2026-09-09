#include "terminal_grid.hpp"
#include "unicode_tables.hpp"
#include <algorithm>
#include <cmath>
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

// See the declaration. Ranges follow Unicode East Asian Width (W and F) plus
// the emoji blocks that have Emoji_Presentation, which is what terminals
// converge on regardless of what the standard technically says about
// Ambiguous width. Box-drawing (U+2500-257F) is deliberately absent: it is
// Ambiguous, and every terminal treats it as narrow.
bool is_combining_mark(char32_t cp) {
    if (cp < 0x0300 || cp > kCombiningMaxCodepoint) return false;
    // Block pre-filter first: this is asked about every non-ASCII character
    // written, and CJK and emoji -- the bulk of them -- sit in blocks holding
    // no marks at all, so they reject on a load and a test rather than a
    // binary search over 350-odd ranges.
    unsigned block = static_cast<unsigned>(cp) >> 8;
    if (!(kCombiningBlockBits[block >> 3] & (1u << (block & 7)))) return false;

    int lo = 0, hi = kCombiningRangeCount - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp < kCombiningRanges[mid].lo) hi = mid - 1;
        else if (cp > kCombiningRanges[mid].hi) lo = mid + 1;
        else return true;
    }
    return false;
}

char32_t compose_pair(char32_t base, char32_t mark) {
    int lo = 0, hi = kComposePairCount - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const ComposePair& e = kComposePairs[mid];
        if (e.base < base || (e.base == base && e.mark < mark)) lo = mid + 1;
        else if (e.base > base || (e.base == base && e.mark > mark)) hi = mid - 1;
        else return e.composed;
    }
    return 0;
}

int char_display_width(char32_t cp) {
    if (cp < 0x1100) return 1;   // the overwhelming common case, checked first

    if ((cp >= 0x1100 && cp <= 0x115F) ||   // Hangul Jamo initial consonants
        (cp >= 0x2E80 && cp <= 0x303E) ||   // CJK radicals, Kangxi, CJK symbols
        (cp >= 0x3041 && cp <= 0x33FF) ||   // Kana, Bopomofo, enclosed CJK, compat
        (cp >= 0x3400 && cp <= 0x4DBF) ||   // CJK unified ext A
        (cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK unified
        (cp >= 0xA000 && cp <= 0xA4CF) ||   // Yi
        (cp >= 0xA960 && cp <= 0xA97F) ||   // Hangul Jamo ext A
        (cp >= 0xAC00 && cp <= 0xD7A3) ||   // Hangul syllables
        (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK compatibility ideographs
        (cp >= 0xFE10 && cp <= 0xFE19) ||   // vertical forms
        (cp >= 0xFE30 && cp <= 0xFE6F) ||   // CJK compat forms, small forms
        (cp >= 0xFF00 && cp <= 0xFF60) ||   // fullwidth ASCII forms
        (cp >= 0xFFE0 && cp <= 0xFFE6)) {   // fullwidth signs
        return 2;
    }

    if ((cp >= 0x1F300 && cp <= 0x1F64F) ||   // pictographs, emoticons
        (cp >= 0x1F680 && cp <= 0x1F6FF) ||   // transport and map
        (cp >= 0x1F900 && cp <= 0x1F9FF) ||   // supplemental pictographs
        (cp >= 0x1FA70 && cp <= 0x1FAFF) ||   // extended-A pictographs
        (cp >= 0x20000 && cp <= 0x2FFFD) ||   // CJK unified ext B onward
        (cp >= 0x30000 && cp <= 0x3FFFD)) {
        return 2;
    }

    return 1;
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

TerminalGrid::TerminalGrid() { reset_palette(); }

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
        Cell default_cell = { 32, pack_color({0.9f, 0.9f, 0.9f, 1.0f}), pack_color({0.0f, 0.0f, 0.0f, 0.0f}) };
        cells_.resize(cols * rows, default_cell);
        row_base_ = 0;
        row_wrapped_.resize(rows, false);
        row_prompt_.resize(rows, false);
        reset_tab_stops();
        scroll_top_ = 0;
        scroll_bottom_ = rows_ - 1;
        origin_mode_ = false;
        return;
    }

    // 1. Gather all rows (history + active) into a raw row sequence
    struct RawRow {
        std::vector<Cell> cells;
        bool wrapped = false;
        bool prompt = false;
    };
    std::vector<RawRow> raw_rows;
    for (const auto& hist : scrollback_history_) {
        raw_rows.push_back({ hist.cells, hist.wrapped, hist.prompt });
    }
    for (int r = 0; r < rows_; ++r) {
        std::vector<Cell> row_cells(cols_);
        for (int c = 0; c < cols_; ++c) {
            row_cells[c] = row_data(r)[c];
        }
        bool is_wrapped = (r < static_cast<int>(row_wrapped_.size())) ? row_wrapped_[r] : false;
        bool is_prompt = (r < static_cast<int>(row_prompt_.size())) ? row_prompt_[r] : false;
        raw_rows.push_back({ std::move(row_cells), is_wrapped, is_prompt });
    }

    // Track the 1D index of the cursor cell in the raw rows stream
    size_t cursor_1d_index = (scrollback_history_.size() + cursor_row_) * cols_ + cursor_col_;
    int cursor_line_idx = -1;
    int cursor_col_idx = -1;

    // 2. Reconstruct logical lines by joining soft-wrapped rows
    std::vector<std::vector<Cell>> logical_lines;
    std::vector<bool> logical_prompt; // parallel: line began at a prompt mark
    std::vector<Cell> current_line;
    bool current_line_prompt = false;
    for (size_t row_idx = 0; row_idx < raw_rows.size(); ++row_idx) {
        const auto& rr = raw_rows[row_idx];
        if (current_line.empty()) current_line_prompt = rr.prompt;
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
            while (!current_line.empty() && current_line.back().codepoint == 32 && current_line.back().bg.a == 0) {
                current_line.pop_back();
            }
            // Clamp cursor position if it fell into the stripped area
            if (cursor_line_idx == static_cast<int>(logical_lines.size())) {
                if (cursor_col_idx > static_cast<int>(current_line.size())) {
                    cursor_col_idx = static_cast<int>(current_line.size());
                }
            }
            logical_lines.push_back(std::move(current_line));
            logical_prompt.push_back(current_line_prompt);
            current_line.clear();
            current_line_prompt = false;
        }
    }
    if (!current_line.empty()) {
        if (cursor_line_idx == static_cast<int>(logical_lines.size())) {
            if (cursor_col_idx > static_cast<int>(current_line.size())) {
                cursor_col_idx = static_cast<int>(current_line.size());
            }
        }
        logical_lines.push_back(std::move(current_line));
        logical_prompt.push_back(current_line_prompt);
    }

    // Trailing blank lines with nothing on them -- and not holding the
    // cursor -- only exist because the terminal was previously taller than
    // its actual content (e.g. after growing to a much bigger window).
    // Left in, step 4 below counts them as real lines when deciding how
    // many lines to push into scrollback on a shrink, which pushes the
    // *content* (and the cursor) out of the new viewport entirely -- the
    // active area ends up all blank, and the shell's next redraw then
    // reprints the prompt fresh on top of it, leaving the original still
    // sitting in scrollback: a visible duplicate.
    while (!logical_lines.empty() && logical_lines.back().empty() &&
           cursor_line_idx != static_cast<int>(logical_lines.size()) - 1) {
        logical_lines.pop_back();
        logical_prompt.pop_back();
    }

    // 3. Re-wrap all logical lines to the new width `cols`
    std::vector<RawRow> wrapped_rows;
    Cell space_cell = { 32, current_fg_packed_, current_bg_packed_ };
    
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
            wrapped_rows.push_back({ std::move(empty_cells), false, logical_prompt[line_idx] });
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
            
            bool first_chunk = (idx == 0);
            idx += chunk_size;
            bool is_wrapped = (idx < line.size());
            wrapped_rows.push_back({ std::move(row_cells), is_wrapped,
                                     first_chunk && logical_prompt[line_idx] });
            current_wrapped_row_idx++;
        }
    }

    // 4. Distribute wrapped rows into new active cells and scrollback history
    std::deque<ScrollbackRow> new_history;
    std::vector<Cell> new_cells(cols * rows, space_cell);
    std::vector<uint8_t> new_row_wrapped(rows, false);
    std::vector<uint8_t> new_row_prompt(rows, false);

    int active_start_idx = 0;
    if (static_cast<int>(wrapped_rows.size()) > rows) {
        active_start_idx = static_cast<int>(wrapped_rows.size()) - rows;
    }

    for (int i = 0; i < active_start_idx; ++i) {
        new_history.push_back({ std::move(wrapped_rows[i].cells), wrapped_rows[i].wrapped,
                                wrapped_rows[i].prompt });
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
            new_row_prompt[r] = wrapped_rows[idx].prompt;
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
    row_base_ = 0; // new_cells was built in logical row order
    row_wrapped_ = std::move(new_row_wrapped);
    row_prompt_ = std::move(new_row_prompt);
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
    wrap_pending_ = false;

    // Stops are per column, so a width change has to grow or shrink the table.
    // Existing stops are kept and only the newly exposed columns get the
    // default every-8 pattern -- an app that set its own stops keeps them
    // across a resize rather than silently reverting.
    {
        int old_cols = static_cast<int>(tab_stops_.size());
        tab_stops_.resize(cols_ > 0 ? cols_ : 0, 0);
        for (int c = ((old_cols + 7) / 8) * 8; c < cols_; c += 8) {
            if (c >= old_cols) tab_stops_[c] = 1;
        }
    }

    // Margins are tied to the old geometry; xterm resets them on resize too
    scroll_top_ = 0;
    scroll_bottom_ = rows_ - 1;
    origin_mode_ = false;
}

void TerminalGrid::kbd_set_flags(int flags, int mode) {
    flags &= kKbdSupported;
    int& current = kbd_stack_.back();
    switch (mode) {
        case 1: current = flags; break;   // set to exactly these
        case 2: current |= flags; break;  // turn these on, leave the rest
        case 3: current &= ~flags; break; // turn these off
        default: break;                   // unknown mode: change nothing
    }
}

void TerminalGrid::kbd_push_flags(int flags) {
    if (kbd_stack_.size() >= kMaxKbdStack) {
        // Drop the oldest rather than refusing the push. A program that
        // pushes without ever popping then degrades gradually instead of
        // wedging the stack, and the entry lost is the one least likely to
        // still matter.
        kbd_stack_.erase(kbd_stack_.begin());
    }
    kbd_stack_.push_back(flags & kKbdSupported);
}

void TerminalGrid::kbd_pop_flags(int count) {
    // The bottom entry is the legacy mode and stays: popping past it would
    // leave nothing to read, and "no flags" is where a pop should bottom out
    // in any case.
    for (int i = 0; i < count && kbd_stack_.size() > 1; ++i) {
        kbd_stack_.pop_back();
    }
}

void TerminalGrid::soft_reset() {
    // DECSTR. The point of it is to restore known *modes* without disturbing
    // what is on screen, so this deliberately does not do most of what
    // full_reset() does: the screen contents, the scrollback, the alternate
    // screen and the tab stops all survive. Clearing any of those would
    // destroy exactly what a caller reaches for DECSTR to preserve.
    //
    // The cursor is left where it is. The VT510 reference lists a home for
    // DECSTR, but the common use in practice is a program tidying its modes
    // mid-session, and moving the cursor there visibly corrupts the line it
    // was writing. xterm-compatible behaviour is the safer reading, and it is
    // what programs are actually written against.
    cursor_visible_ = true;
    origin_mode_ = false;
    // The VT510 list for DECSTR does not mention the keypad, but leaving it in
    // application mode is exactly the kind of stranded state a soft reset is
    // sent to clear -- an app that exits without restoring it would otherwise
    // leave the shell's keypad emitting SS3 with no way back short of RIS.
    app_keypad_ = false;
    scroll_top_ = 0;
    scroll_bottom_ = rows_ - 1;
    cursor_shape_ = CursorShape::Block;
    wrap_pending_ = false;

    // The saved cursor does go home -- DECSC/DECRC around a DECSTR should not
    // restore a position from before it.
    saved_cursor_col_ = 0;
    saved_cursor_row_ = 0;

    // The *default* colours are not reset here. DECSTR restores the SGR state
    // to the defaults; it does not undo an OSC 10/11/12 that changed what the
    // defaults are. RIS does both.
    current_fg_ = default_fg_;
    current_bg_ = default_bg_;
    current_fg_packed_ = pack_color(current_fg_);
    current_bg_packed_ = pack_color(current_bg_);
    current_attrs_ = 0;
    current_hyperlink_id_ = 0;
    blank_row_valid_ = false;
}

void TerminalGrid::reset_tab_stops() {
    tab_stops_.assign(cols_ > 0 ? cols_ : 0, 0);
    for (int c = 8; c < cols_; c += 8) tab_stops_[c] = 1;
}

void TerminalGrid::set_tab_stop() {
    if (cursor_col_ >= 0 && cursor_col_ < static_cast<int>(tab_stops_.size())) {
        tab_stops_[cursor_col_] = 1;
    }
}

void TerminalGrid::clear_tab_stop() {
    if (cursor_col_ >= 0 && cursor_col_ < static_cast<int>(tab_stops_.size())) {
        tab_stops_[cursor_col_] = 0;
    }
}

void TerminalGrid::clear_all_tab_stops() {
    std::fill(tab_stops_.begin(), tab_stops_.end(), 0);
}

void TerminalGrid::tab_forward(int count) {
    if (cols_ <= 0) return;
    int col = cursor_col_;
    for (int n = 0; n < count; ++n) {
        int next = cols_ - 1; // no stop ahead: stop at the right margin
        for (int c = col + 1; c < cols_; ++c) {
            if (is_tab_stop(c)) { next = c; break; }
        }
        col = next;
        if (col >= cols_ - 1) break;
    }
    set_cursor_col(col);
    // A tab satisfies a pending wrap the same way a printable character
    // would not: it moves within the row it is already on.
    wrap_pending_ = false;
}

void TerminalGrid::tab_backward(int count) {
    if (cols_ <= 0) return;
    int col = cursor_col_;
    for (int n = 0; n < count; ++n) {
        int prev = 0; // no stop behind: stop at the left margin
        for (int c = col - 1; c > 0; --c) {
            if (is_tab_stop(c)) { prev = c; break; }
        }
        col = prev;
        if (col == 0) break;
    }
    set_cursor_col(col);
    wrap_pending_ = false;
}

void TerminalGrid::set_cell(int col, int row, char32_t codepoint, const SDL_FColor& fg, const SDL_FColor& bg) {
    if (col >= 0 && col < cols_ && row >= 0 && row < rows_) {
        row_data(row)[col] = { codepoint, pack_color(fg), pack_color(bg) };
    }
}

int TerminalGrid::write_run(const char* ascii, int n) {
    // Callers gate on is_wrap_pending(); a pending wrap goes through
    // write_character() so that logic is not duplicated here.
    if (n <= 0 || cols_ <= 0 || rows_ <= 0) return 0;
    if (cursor_row_ < 0 || cursor_row_ >= rows_ ||
        cursor_col_ < 0 || cursor_col_ >= cols_) {
        return 0;
    }

    int avail = cols_ - cursor_col_;
    if (n > avail) n = avail;

    Cell* row = row_data(cursor_row_);
    const PackedColor fg = current_fg_packed_;
    const PackedColor bg = current_bg_packed_;
    const uint8_t attrs = current_attrs_;
    const uint32_t link = current_hyperlink_id_;
    for (int k = 0; k < n; ++k) {
        row[cursor_col_ + k] = {
            static_cast<char32_t>(static_cast<unsigned char>(ascii[k])),
            fg, bg, attrs, link
        };
    }

    // Matches applying write_character() n times: on filling the row the
    // cursor parks on the last column with the wrap deferred (xenl), rather
    // than moving past it.
    if (cursor_col_ + n >= cols_) {
        cursor_col_ = cols_ - 1;
        wrap_pending_ = true;
    } else {
        cursor_col_ += n;
    }
    return n;
}

// Appends a cell's text as UTF-8: one codepoint, or the whole cluster. Control
// cells and blanks are the caller's business; this only widens what a cell can
// contain.
void TerminalGrid::append_cell_utf8(const Cell& cell, std::string& out) const {
    int n = 0;
    const char32_t* text = cell_text(cell, n);
    for (int i = 0; i < n; ++i) out += utf32_to_utf8(text[i]);
}

const char32_t* TerminalGrid::cell_text(const Cell& cell, int& count) const {
    if (is_cluster_ref(cell.codepoint)) {
        uint32_t i = cluster_index_of(cell.codepoint);
        if (i < clusters_.size()) {
            count = static_cast<int>(clusters_[i].size());
            return clusters_[i].data();
        }
        count = 0; // dangling reference: renders as nothing
        return nullptr;
    }
    count = 1;
    return &cell.codepoint;
}

std::u32string TerminalGrid::cell_string(const Cell& cell) const {
    int n = 0;
    const char32_t* text = cell_text(cell, n);
    return std::u32string(text, text + n);
}

char32_t TerminalGrid::cell_base(const Cell& cell) const {
    int n = 0;
    const char32_t* text = cell_text(cell, n);
    return n > 0 ? text[0] : U' ';
}

// Column holding the character a combining mark or ZWJ attaches to. That is
// the cell just written: with a wrap deferred the cursor is still parked on
// it, otherwise it sits one to the left -- or two, if that character was
// double-width.
int TerminalGrid::combining_base_col() const {
    if (cursor_row_ < 0 || cursor_row_ >= rows_ || cols_ <= 0) return -1;
    int base_col = wrap_pending_ ? cursor_col_ : cursor_col_ - 1;
    const Cell* row = row_data(cursor_row_);
    if (base_col > 0 && (row[base_col].attrs & ATTR_WIDE_CONT)) base_col--;
    return (base_col >= 0 && base_col < cols_) ? base_col : -1;
}

void TerminalGrid::append_to_cluster(int base_col, char32_t cp) {
    if (base_col < 0 || base_col >= cols_) return;
    Cell& cell = row_data(cursor_row_)[base_col];

    std::u32string next;
    if (is_cluster_ref(cell.codepoint)) {
        uint32_t i = cluster_index_of(cell.codepoint);
        if (i >= clusters_.size()) return; // dangling; nothing to extend
        if (clusters_[i].size() >= kMaxClusterLen) return; // bounded
        next = clusters_[i];
    } else {
        next = std::u32string(1, cell.codepoint);
    }
    next += cp;

    auto it = cluster_ids_.find(next);
    if (it != cluster_ids_.end()) {
        cell.codepoint = kClusterTag | static_cast<char32_t>(it->second);
        return;
    }
    if (clusters_.size() >= kMaxClusters) return; // full: drop the mark
    clusters_.push_back(next);
    uint32_t id = static_cast<uint32_t>(clusters_.size() - 1);
    cluster_ids_.emplace(std::move(next), id);
    cell.codepoint = kClusterTag | static_cast<char32_t>(id);
}

void TerminalGrid::write_character(char32_t codepoint) {
    // Deferred auto-wrap (xenl): if wrap is pending from a previous char hitting the rightmost column, wrap now
    if (wrap_pending_) {
        wrap_pending_ = false;
        if (cursor_row_ < static_cast<int>(row_wrapped_.size())) {
            row_wrapped_[cursor_row_] = true;
        }
        // Same motion as index(): scroll at the bottom margin (which may be
        // a DECSTBM region bottom, not the last screen row)
        if (cursor_row_ == get_scroll_bottom()) {
            scroll_up();
        } else if (cursor_row_ < rows_ - 1) {
            cursor_row_++;
        }
        cursor_col_ = 0;
    }
    
    // ZERO WIDTH JOINER. It carries no width of its own and binds the
    // character after it into the same visible character -- which is how
    // family and profession emoji are built. Without this each part of
    // MAN ZWJ WOMAN ZWJ GIRL ZWJ BOY took its own cell, so one emoji rendered
    // as four across eight columns.
    // ZWJ and ZWNJ. Neither has a width; both belong to the cluster they
    // follow. ZWJ additionally binds the character *after* it into the same
    // cell, which is how family and profession emoji are built -- without it
    // MAN ZWJ WOMAN ZWJ GIRL ZWJ BOY took four cells across eight columns and
    // rendered as four separate people.
    if (codepoint == 0x200D || codepoint == 0x200C) {
        int base_col = combining_base_col();
        if (base_col >= 0) {
            append_to_cluster(base_col, codepoint);
            zwj_pending_ = (codepoint == 0x200D);
        }
        return;
    }
    if (zwj_pending_) {
        zwj_pending_ = false;
        int base_col = combining_base_col();
        if (base_col >= 0) {
            append_to_cluster(base_col, codepoint);
            return;
        }
    }

    // A combining mark carries no width: it composes onto the character
    // already written rather than taking a cell. macOS hands out filenames in
    // NFD, so `ls` in any directory with accented names produces exactly these
    // base+mark sequences -- previously each mark consumed its own cell, so
    // the text was both wrong and a column wider per accent.
    if (is_combining_mark(codepoint)) {
        if (cursor_row_ >= 0 && cursor_row_ < rows_ && cols_ > 0) {
            // The base is the cell just written. With a wrap deferred the
            // cursor is still parked on it; otherwise it sits one to the left,
            // or two if that character was double-width.
            int base_col = wrap_pending_ ? cursor_col_ : cursor_col_ - 1;
            Cell* row = row_data(cursor_row_);
            if (base_col > 0 && (row[base_col].attrs & ATTR_WIDE_CONT)) {
                base_col--;
            }
            if (base_col >= 0 && base_col < cols_) {
                char32_t composed = is_cluster_ref(row[base_col].codepoint)
                                        ? 0
                                        : compose_pair(row[base_col].codepoint, codepoint);
                if (composed) {
                    // A precomposed form exists, so one codepoint still says
                    // it: 'e' + U+0301 becomes U+00E9 and stays a plain cell.
                    row[base_col].codepoint = composed;
                } else {
                    // No precomposed form -- stacked marks, Devanagari, Hebrew
                    // points, variation selectors. The mark used to be dropped
                    // here, because one codepoint per cell could not say
                    // "base plus mark". The cell now holds a cluster and the
                    // whole sequence is shaped together at render time.
                    append_to_cluster(base_col, codepoint);
                }
            }
        }
        return;
    }

    zwj_pending_ = false;

    const int width = char_display_width(codepoint);

    // A double-width glyph cannot straddle a line break, so if only one column
    // is left it wraps first rather than being split. This is the one case
    // where the wrap happens *before* the write instead of being deferred.
    if (width == 2 && cursor_col_ == cols_ - 1) {
        if (cursor_row_ < static_cast<int>(row_wrapped_.size())) {
            row_wrapped_[cursor_row_] = true;
        }
        // The column left behind is blanked: leaving the old contents there
        // would show a stale glyph in a cell the text has moved past.
        if (cursor_row_ >= 0 && cursor_row_ < rows_) {
            row_data(cursor_row_)[cursor_col_] =
                { 32, current_fg_packed_, current_bg_packed_, current_attrs_, 0 };
        }
        if (cursor_row_ == get_scroll_bottom()) {
            scroll_up();
        } else if (cursor_row_ < rows_ - 1) {
            cursor_row_++;
        }
        cursor_col_ = 0;
        wrap_pending_ = false;
    }

    if (cursor_col_ >= 0 && cursor_col_ < cols_ && cursor_row_ >= 0 && cursor_row_ < rows_) {
        Cell* row = row_data(cursor_row_);
        row[cursor_col_] =
            { codepoint, current_fg_packed_, current_bg_packed_, current_attrs_, current_hyperlink_id_ };
        // The trailing half carries the same style so the background reads as
        // one block, but no codepoint: the lead cell draws across both.
        if (width == 2 && cursor_col_ + 1 < cols_) {
            row[cursor_col_ + 1] = {
                0, current_fg_packed_, current_bg_packed_,
                static_cast<uint8_t>(current_attrs_ | ATTR_WIDE_CONT),
                current_hyperlink_id_
            };
        }
    }

    if (cursor_col_ + width >= cols_) {
        cursor_col_ = cols_ - 1;
        wrap_pending_ = true;
    } else {
        cursor_col_ += width;
    }
}

// See blank_row_cache_'s declaration.
const Cell* TerminalGrid::blank_row() {
    if (!blank_row_valid_ ||
        static_cast<int>(blank_row_cache_.size()) != cols_ ||
        blank_row_fg_.r != current_fg_packed_.r || blank_row_fg_.g != current_fg_packed_.g ||
        blank_row_fg_.b != current_fg_packed_.b || blank_row_fg_.a != current_fg_packed_.a ||
        blank_row_bg_.r != current_bg_packed_.r || blank_row_bg_.g != current_bg_packed_.g ||
        blank_row_bg_.b != current_bg_packed_.b || blank_row_bg_.a != current_bg_packed_.a) {
        blank_row_cache_.assign(static_cast<size_t>(cols_),
                                Cell{ 32, current_fg_packed_, current_bg_packed_ });
        blank_row_fg_ = current_fg_packed_;
        blank_row_bg_ = current_bg_packed_;
        blank_row_valid_ = true;
    }
    return blank_row_cache_.data();
}

void TerminalGrid::scroll_up() {
    if (rows_ <= 1) return;

    // With a partial DECSTBM region active, only the rows between the
    // margins move and nothing enters scrollback -- that's what lets vim
    // and less scroll a viewport without destroying the shell's history.
    // The alt screen never feeds scrollback either: its rows are a full
    // screen app's transient UI, not output the user will want to revisit.
    if (alt_screen_active_ || scroll_top_ != 0 || get_scroll_bottom() != rows_ - 1) {
        shift_rows_up(scroll_top_, get_scroll_bottom(), 1);
        return;
    }

    // Copy top row to scrollback history.
    //
    // At steady state history sits at its cap, so every scroll pushes one row
    // and drops one -- meaning the allocation being freed is the same size as
    // the one being requested, once per line of output. Steal the outgoing
    // row's storage up front and reuse it instead of round-tripping the
    // allocator on every newline. The element it was taken from is popped a
    // few lines below, before anything can observe its emptied vector.
    std::vector<Cell> top_row;
    bool at_cap = scrollback_history_.size() >= max_scrollback_size_;
    if (at_cap && !scrollback_history_.empty()) {
        top_row = std::move(scrollback_history_.front().cells);
    }
    top_row.resize(cols_);
    const Cell* top_src = row_data(0);
    std::copy(top_src, top_src + cols_, top_row.begin());
    
    bool top_wrapped = (!row_wrapped_.empty()) ? row_wrapped_[0] : false;
    bool top_prompt = (!row_prompt_.empty()) ? row_prompt_[0] : false;
    scrollback_history_.push_back({ std::move(top_row), top_wrapped, top_prompt });
    
    // Keep viewport locked to the same historical lines if we are scrolled up
    if (scroll_offset_ > 0) {
        scroll_offset_++;
    }
    
    if (scrollback_history_.size() > max_scrollback_size_) {
        scrollback_history_.pop_front();
        // The line just dropped can never be addressed again, so anything
        // pinned to it goes too. Counting evictions is what keeps every other
        // line's id stable across the trim.
        lines_evicted_++;
        images_.retire_before(lines_evicted_);
        if (scroll_offset_ > 0) {
            scroll_offset_--;
        }
    }
    
    // Rotate the ring instead of moving the grid. The row that was logical
    // row 0 becomes the new last row and is blanked below.
    row_base_ = phys_row(1);
    
    // Shift wrapped/prompt flags up by one row. These stay in logical order
    // rather than joining the cell ring: they are one byte per row, so the
    // shift is a short memmove that std::copy lowers to directly.
    if (rows_ > 1) {
        std::copy(row_wrapped_.begin() + 1, row_wrapped_.end(), row_wrapped_.begin());
        std::copy(row_prompt_.begin() + 1, row_prompt_.end(), row_prompt_.begin());
    }
    row_wrapped_[rows_ - 1] = false;
    row_prompt_[rows_ - 1] = false;
    
    // Fill the new last row with spaces using current style
    const Cell* blank = blank_row();
    Cell* recycled = row_data(rows_ - 1);
    std::copy(blank, blank + cols_, recycled);

    if (saved_cursor_row_ > 0) {
        saved_cursor_row_--;
    }
}

void TerminalGrid::set_current_hyperlink(const std::string& uri) {
    if (uri.empty()) {
        current_hyperlink_id_ = 0;
        return;
    }
    auto it = hyperlink_id_by_uri_.find(uri);
    if (it != hyperlink_id_by_uri_.end()) {
        current_hyperlink_id_ = it->second;
        return;
    }
    if (hyperlink_uris_.size() >= kMaxHyperlinkTableSize) {
        // Same amortized-reset the font atlas uses when it fills up: cells
        // already on screen/in scrollback referencing old ids just degrade
        // to "no URI" rather than the table growing without bound.
        hyperlink_uris_.clear();
        hyperlink_id_by_uri_.clear();
    }
    hyperlink_uris_.push_back(uri);
    current_hyperlink_id_ = static_cast<uint32_t>(hyperlink_uris_.size()); // 1-based
    hyperlink_id_by_uri_[uri] = current_hyperlink_id_;
}

const std::string& TerminalGrid::get_hyperlink_uri(uint32_t id) const {
    static const std::string empty;
    if (id == 0 || id > hyperlink_uris_.size()) return empty;
    return hyperlink_uris_[id - 1];
}

void TerminalGrid::set_alt_screen(bool active) {
    if (active == alt_screen_active_) return; // apps re-send 1049h defensively
    alt_screen_active_ = active;

    // Each screen keeps its own images. Entering a full-screen app must not
    // discard what was printed to the shell, and the two screens address the
    // same active rows, so their line ids would otherwise collide.
    std::vector<ImagePlacement> other = images_.take_placements();
    images_.set_placements(std::move(saved_primary_placements_));
    saved_primary_placements_ = std::move(other);

    // Margins don't survive the buffer switch: a full-screen app's region
    // must never keep constraining the shell's scrolling.
    scroll_top_ = 0;
    scroll_bottom_ = rows_ - 1;
    origin_mode_ = false;

    // A selection or scrolled-back view of the old buffer is meaningless on
    // the new one
    has_selection_ = false;
    selecting_ = false;
    scroll_offset_ = 0;

    if (active) {
        // Flattened to logical order rather than copying the ring as-is,
        // so the restore path below does not need the saved base.
        saved_primary_cells_.resize(static_cast<size_t>(rows_) * cols_);
        for (int r = 0; r < rows_; ++r) {
            const Cell* src = row_data(r);
            std::copy(src, src + cols_, saved_primary_cells_.begin() + static_cast<size_t>(r) * cols_);
        }
        saved_primary_row_wrapped_.assign(row_wrapped_.begin(), row_wrapped_.end());
        saved_primary_row_prompt_.assign(row_prompt_.begin(), row_prompt_.end());
        saved_primary_cols_ = cols_;
        saved_primary_rows_ = rows_;
        saved_primary_cursor_col_ = cursor_col_;
        saved_primary_cursor_row_ = cursor_row_;
        saved_primary_wrap_pending_ = wrap_pending_;
        wrap_pending_ = false;

        // The alt screen starts blank with the cursor at home
        Cell empty_cell = { 32, current_fg_packed_, current_bg_packed_ };
        std::fill(cells_.begin(), cells_.end(), empty_cell);
        row_base_ = 0; // every cell is identical, so rebasing is safe here
        std::fill(row_wrapped_.begin(), row_wrapped_.end(), false);
        std::fill(row_prompt_.begin(), row_prompt_.end(), false);
        cursor_col_ = 0;
        cursor_row_ = 0;
    } else {
        // Restore whatever still fits the current geometry; a resize while
        // the app was running leaves the rest blank
        Cell empty_cell = { 32, pack_color({0.9f, 0.9f, 0.9f, 1.0f}), pack_color({0.0f, 0.0f, 0.0f, 0.0f}) };
        std::fill(cells_.begin(), cells_.end(), empty_cell);
        row_base_ = 0; // every cell is identical, so rebasing is safe here
        std::fill(row_wrapped_.begin(), row_wrapped_.end(), false);
        std::fill(row_prompt_.begin(), row_prompt_.end(), false);
        int copy_rows = std::min(rows_, saved_primary_rows_);
        int copy_cols = std::min(cols_, saved_primary_cols_);
        for (int r = 0; r < copy_rows; ++r) {
            for (int c = 0; c < copy_cols; ++c) {
                row_data(r)[c] = saved_primary_cells_[r * saved_primary_cols_ + c];
            }
            row_wrapped_[r] = saved_primary_row_wrapped_[r];
            row_prompt_[r] = saved_primary_row_prompt_[r];
        }
        cursor_col_ = std::clamp(saved_primary_cursor_col_, 0, cols_ - 1);
        cursor_row_ = std::clamp(saved_primary_cursor_row_, 0, rows_ - 1);
        wrap_pending_ = saved_primary_wrap_pending_;
        saved_primary_cells_.clear();
        saved_primary_row_wrapped_.clear();
        saved_primary_row_prompt_.clear();

        // Crash safety: an app that died without undoing civis or DECRSTing
        // its mouse mode must not leave the shell cursorless or click-deaf
        cursor_visible_ = true;
        mouse_mode_ = 0;
        mouse_sgr_ = false;
        app_cursor_keys_ = false;
        current_hyperlink_id_ = 0;
    }
}

void TerminalGrid::cursor_home() {
    cursor_row_ = origin_mode_ ? scroll_top_ : 0;
    cursor_col_ = 0;
    wrap_pending_ = false;
}

int TerminalGrid::get_scroll_bottom() const {
    return (scroll_bottom_ <= 0 || scroll_bottom_ >= rows_) ? rows_ - 1 : scroll_bottom_;
}

void TerminalGrid::set_scroll_region(int top, int bottom) {
    if (top < 0) top = 0;
    if (bottom < 0 || bottom >= rows_) bottom = rows_ - 1;
    if (top >= bottom) { // degenerate region resets to full screen
        top = 0;
        bottom = rows_ - 1;
    }
    scroll_top_ = top;
    scroll_bottom_ = bottom;
}

// Shifts rows [top..bottom] up by count, blank-filling at the bottom.
// Rows shifted past `top` are discarded, never pushed to scrollback:
// this is the primitive behind partial-region scrolls and DL.
void TerminalGrid::shift_rows_up(int top, int bottom, int count) {
    if (count <= 0 || top < 0 || bottom >= rows_ || top >= bottom) return;
    count = std::min(count, bottom - top + 1);

    Cell empty_cell = { 32, current_fg_packed_, current_bg_packed_ };
    for (int r = top; r <= bottom; ++r) {
        int src = r + count;
        if (src <= bottom) {
            const Cell* s_row = row_data(src);
            std::copy(s_row, s_row + cols_, row_data(r));
            row_wrapped_[r] = row_wrapped_[src];
            row_prompt_[r] = row_prompt_[src];
        } else {
            Cell* d_row = row_data(r);
            std::fill(d_row, d_row + cols_, empty_cell);
            row_wrapped_[r] = false;
            row_prompt_[r] = false;
        }
    }
    wrap_pending_ = false;
}

// Mirror of shift_rows_up: rows [top..bottom] move down, blank-filling at
// the top and discarding rows pushed past `bottom`.
void TerminalGrid::shift_rows_down(int top, int bottom, int count) {
    if (count <= 0 || top < 0 || bottom >= rows_ || top >= bottom) return;
    count = std::min(count, bottom - top + 1);

    Cell empty_cell = { 32, current_fg_packed_, current_bg_packed_ };
    for (int r = bottom; r >= top; --r) {
        int src = r - count;
        if (src >= top) {
            const Cell* s_row = row_data(src);
            std::copy(s_row, s_row + cols_, row_data(r));
            row_wrapped_[r] = row_wrapped_[src];
            row_prompt_[r] = row_prompt_[src];
        } else {
            Cell* d_row = row_data(r);
            std::fill(d_row, d_row + cols_, empty_cell);
            row_wrapped_[r] = false;
            row_prompt_[r] = false;
        }
    }
    wrap_pending_ = false;
}

void TerminalGrid::index() {
    wrap_pending_ = false;
    if (cursor_row_ == get_scroll_bottom()) {
        scroll_up();
    } else if (cursor_row_ < rows_ - 1) {
        // Below the bottom margin the cursor still moves down freely; at the
        // last screen row it stays put.
        cursor_row_++;
    }
}

void TerminalGrid::reverse_index() {
    wrap_pending_ = false;
    if (cursor_row_ == scroll_top_) {
        shift_rows_down(scroll_top_, get_scroll_bottom(), 1);
    } else if (cursor_row_ > 0) {
        cursor_row_--;
    }
}

void TerminalGrid::scroll_region_up(int count) {
    if (scroll_top_ == 0 && get_scroll_bottom() == rows_ - 1) {
        // Full-screen scroll goes through scroll_up so the departing rows
        // land in scrollback like a normal line feed would put them there.
        for (int n = 0; n < count; ++n) scroll_up();
    } else {
        shift_rows_up(scroll_top_, get_scroll_bottom(), count);
    }
}

void TerminalGrid::scroll_region_down(int count) {
    shift_rows_down(scroll_top_, get_scroll_bottom(), count);
}

void TerminalGrid::insert_lines(int count) {
    // IL/DL only act with the cursor inside the region, and home the cursor
    // to the left margin.
    if (cursor_row_ < scroll_top_ || cursor_row_ > get_scroll_bottom()) return;
    shift_rows_down(cursor_row_, get_scroll_bottom(), count);
    cursor_col_ = 0;
}

void TerminalGrid::delete_lines(int count) {
    if (cursor_row_ < scroll_top_ || cursor_row_ > get_scroll_bottom()) return;
    shift_rows_up(cursor_row_, get_scroll_bottom(), count);
    cursor_col_ = 0;
}

void TerminalGrid::clear_screen() {
    // Erasing the screen erases what was drawn on it. Images pinned to lines
    // already in scrollback are untouched: they belong to that text, not to
    // the screen being cleared.
    images_.clear_placements();

    // Copy the cached blank row across every row: std::fill over a 20-byte
    // element cannot become a memset, but these are plain memmove.
    if (!cells_.empty() && cols_ > 0) {
        const Cell* blank = blank_row();
        for (int r = 0; r < rows_; ++r) {
            std::copy(blank, blank + cols_, cells_.begin() + static_cast<size_t>(r) * cols_);
        }
    }
    std::fill(row_wrapped_.begin(), row_wrapped_.end(), false);
    std::fill(row_prompt_.begin(), row_prompt_.end(), false);
    cursor_col_ = 0;
    cursor_row_ = 0;
    wrap_pending_ = false;
    saved_cursor_col_ = 0;
    saved_cursor_row_ = 0;
}

void TerminalGrid::set_max_scrollback(size_t lines) {
    max_scrollback_size_ = lines;
    if (scrollback_history_.size() > max_scrollback_size_) {
        size_t prune = scrollback_history_.size() - max_scrollback_size_;
        scrollback_history_.erase(scrollback_history_.begin(),
                                  scrollback_history_.begin() + prune);
        lines_evicted_ += prune;
        images_.retire_before(lines_evicted_);
        // Keep the view anchored to the same lines if scrolled back
        scroll_offset_ = std::min(scroll_offset_,
                                  static_cast<int>(scrollback_history_.size()));
    }
}

void TerminalGrid::full_reset() {
    // Leave the alternate screen before clearing, so the saved primary buffer
    // is discarded rather than restored over the top of the reset.
    if (alt_screen_active_) {
        alt_screen_active_ = false;
        saved_primary_cells_.clear();
        saved_primary_row_wrapped_.clear();
        saved_primary_row_prompt_.clear();
    }

    reset_default_fg();
    reset_default_bg();
    reset_default_cursor_color();
    reset_palette();
    current_fg_ = default_fg_;
    current_bg_ = default_bg_;
    current_fg_packed_ = pack_color(current_fg_);
    current_bg_packed_ = pack_color(current_bg_);
    current_attrs_ = 0;
    current_hyperlink_id_ = 0;
    blank_row_valid_ = false;

    clear_screen();
    clear_scrollback();

    cursor_col_ = 0;
    cursor_row_ = 0;
    saved_cursor_col_ = 0;
    saved_cursor_row_ = 0;
    wrap_pending_ = false;

    scroll_top_ = 0;
    scroll_bottom_ = rows_ - 1;
    origin_mode_ = false;
    app_keypad_ = false;
    cursor_shape_ = CursorShape::Block;
    reset_tab_stops();
    kbd_stack_.assign(1, 0);
    images_.clear_all();
    saved_primary_placements_.clear();

    scroll_offset_ = 0;
    display_scroll_offset_ = 0.0f;

    cursor_visible_ = true;
    bracketed_paste_active_ = false;
    synchronized_output_ = false;
    focus_reporting_ = false;
    mouse_mode_ = 0;
    mouse_sgr_ = false;
    app_cursor_keys_ = false;

    clear_selection();
    prompt_boundary_col_ = -1;
}

void TerminalGrid::clear_scrollback() {
    lines_evicted_ += scrollback_history_.size();
    scrollback_history_.clear();
    images_.retire_before(lines_evicted_);
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
    
    const Cell* blank = blank_row();
    Cell* line = row_data(row);
    if (end_col > start_col) {
        std::copy(blank, blank + (end_col - start_col), line + start_col);
    }
    wrap_pending_ = false;
}

void TerminalGrid::set_cursor_col(int col) {
    cursor_col_ = std::clamp(col, 0, cols_ - 1);
    wrap_pending_ = false;
}

void TerminalGrid::save_cursor() {
    saved_cursor_col_ = cursor_col_;
    saved_cursor_row_ = cursor_row_;
}

void TerminalGrid::restore_cursor() {
    cursor_col_ = std::clamp(saved_cursor_col_, 0, cols_ - 1);
    cursor_row_ = std::clamp(saved_cursor_row_, 0, rows_ - 1);
    wrap_pending_ = false;
}

void TerminalGrid::set_cursor_row(int row) {
    cursor_row_ = std::clamp(row, 0, rows_ - 1);
    wrap_pending_ = false;
}

void TerminalGrid::insert_character(int count) {
    // ICH (Insert Character, CSI Ps @): open `count` blank cells at the
    // cursor, shifting the rest of the line right. Whatever falls off the
    // right edge is gone -- ICH does not wrap -- and the cursor stays put.
    //
    // The mirror of delete_character (DCH). Readline and editors use the pair
    // to insert and delete mid-line without repainting the tail, so with ICH
    // missing the tail was overwritten instead of shifted: the program's model
    // of the screen and the screen itself silently diverged, which shows up as
    // characters being eaten while typing into the middle of a long line.
    if (cursor_row_ < 0 || cursor_row_ >= rows_) return;
    if (cursor_col_ < 0 || cursor_col_ >= cols_) return;
    if (count <= 0) return;
    wrap_pending_ = false;

    Cell* row_cells = row_data(cursor_row_);
    int remaining = cols_ - cursor_col_;
    int to_insert = std::min(count, remaining);

    // Rightward, so overlapping source and destination don't clobber.
    for (int i = cols_ - 1; i >= cursor_col_ + to_insert; --i) {
        row_cells[i] = row_cells[i - to_insert];
    }

    Cell empty_cell = { 32, current_fg_packed_, current_bg_packed_ };
    for (int i = cursor_col_; i < cursor_col_ + to_insert; ++i) {
        row_cells[i] = empty_cell;
    }
}

void TerminalGrid::delete_character(int count) {
    if (cursor_row_ < 0 || cursor_row_ >= rows_) return;
    if (cursor_col_ < 0 || cursor_col_ >= cols_) return;
    if (count <= 0) return;
    wrap_pending_ = false;
    
    Cell* row_cells = row_data(cursor_row_);
    int remaining = cols_ - cursor_col_;
    int to_delete = std::min(count, remaining);
    
    // Shift cells leftward
    for (int i = cursor_col_; i < cols_ - to_delete; ++i) {
        row_cells[i] = row_cells[i + to_delete];
    }
    
    // Fill the end of the row with empty cells
    Cell empty_cell = { 32, current_fg_packed_, current_bg_packed_ };
    for (int i = cols_ - to_delete; i < cols_; ++i) {
        row_cells[i] = empty_cell;
    }
}

void TerminalGrid::erase_characters(int count) {
    // ECH (Erase Character, CSI Ps X): blank `count` cells starting at the
    // cursor, in place -- unlike delete_character (DCH), nothing shifts and
    // the cursor doesn't move. ncurses/slang-based apps (cacademo, htop,
    // etc.) use this heavily to clear stale glyphs before redrawing; without
    // it those cells are never blanked and old frames visibly bleed through.
    if (cursor_row_ < 0 || cursor_row_ >= rows_) return;
    if (cursor_col_ < 0 || cursor_col_ >= cols_) return;
    if (count <= 0) return;

    Cell* row_cells = row_data(cursor_row_);
    int remaining = cols_ - cursor_col_;
    int to_erase = std::min(count, remaining);

    Cell empty_cell = { 32, current_fg_packed_, current_bg_packed_ };
    for (int i = cursor_col_; i < cursor_col_ + to_erase; ++i) {
        row_cells[i] = empty_cell;
    }
}

void TerminalGrid::mark_prompt_row() {
    if (cursor_row_ >= 0 && cursor_row_ < static_cast<int>(row_prompt_.size())) {
        row_prompt_[cursor_row_] = true;
    }
}

// Prompt navigation works in "absolute rows": scrollback rows first, then
// the active grid. The view's top row sits at absolute index
// (history_size - scroll_offset_); jumping puts the target prompt there.
void TerminalGrid::scroll_to_prev_prompt() {
    int history_size = static_cast<int>(scrollback_history_.size());
    int view_top = history_size - scroll_offset_;
    int best = -1;
    for (int i = 0; i < history_size && i < view_top; ++i) {
        if (scrollback_history_[i].prompt) best = i;
    }
    for (int r = 0; r < rows_; ++r) {
        int abs_row = history_size + r;
        if (abs_row >= view_top) break;
        if (r < static_cast<int>(row_prompt_.size()) && row_prompt_[r]) best = abs_row;
    }
    if (best >= 0) {
        scroll_offset_ = std::clamp(history_size - best, 0, history_size);
    }
}

void TerminalGrid::scroll_to_next_prompt() {
    int history_size = static_cast<int>(scrollback_history_.size());
    int view_top = history_size - scroll_offset_;
    for (int i = std::max(0, view_top + 1); i < history_size; ++i) {
        if (scrollback_history_[i].prompt) {
            scroll_offset_ = history_size - i;
            return;
        }
    }
    for (int r = 0; r < rows_; ++r) {
        int abs_row = history_size + r;
        if (abs_row <= view_top) continue;
        if (r < static_cast<int>(row_prompt_.size()) && row_prompt_[r]) {
            // Active-screen prompts can't be brought above the live view;
            // snapping to the live bottom is the closest match
            scroll_offset_ = 0;
            return;
        }
    }
    scroll_offset_ = 0; // no prompt below: rejoin the live view
}

void TerminalGrid::set_cursor_shape(int decscusr_param) {
    // 0/1 blinking block, 2 steady block, 3 blinking underline, 4 steady
    // underline, 5 blinking bar, 6 steady bar.
    //
    // The blink half of each pair is parsed and then deliberately dropped.
    // sink's cursor has always been steady, and honouring blink would mean
    // that any shell emitting a cursor-shape reset -- which sends 0 or 1,
    // "blinking block" -- starts the cursor blinking. That is a visible change
    // to how the terminal looks arriving as a side effect of adding shape
    // support, so it is a decision to make on its own. Terminals with a
    // "disable cursor blink" preference present exactly this behaviour.
    switch (decscusr_param) {
        case 3: case 4: cursor_shape_ = CursorShape::Underline; break;
        case 5: case 6: cursor_shape_ = CursorShape::Bar; break;
        default:        cursor_shape_ = CursorShape::Block; break;
    }
}

void TerminalGrid::place_image_at_cursor(uint64_t image_id, int pixel_w, int pixel_h) {
    if (pixel_w <= 0 || pixel_h <= 0 || cols_ <= 0 || rows_ <= 0) return;
    const TerminalImage* img = images_.find(image_id);
    if (!img) return;

    // Rounded up: a picture that does not divide evenly into cells occupies
    // the partial one rather than being cropped by it.
    int cw = effective_cell_px_w();
    int ch = effective_cell_px_h();
    int span_cols = std::min(cols_ - cursor_col_, (pixel_w + cw - 1) / cw);
    int span_rows = (pixel_h + ch - 1) / ch;
    if (span_cols <= 0 || span_rows <= 0) return;

    ImagePlacement placement;
    placement.image_id = image_id;
    placement.line_id = line_id_for_row(cursor_row_);
    placement.col = cursor_col_;
    placement.cols = span_cols;
    placement.rows = span_rows;
    placement.src_x = 0;
    placement.src_y = 0;
    placement.src_w = pixel_w;
    placement.src_h = pixel_h;
    images_.place(placement);

    // The cursor lands at the start of the line below the image, scrolling
    // whatever is needed to get there -- which is what carries the placement's
    // line into scrollback along with the text around it.
    wrap_pending_ = false;
    for (int i = 0; i < span_rows; ++i) {
        if (cursor_row_ >= get_scroll_bottom()) {
            scroll_up();
        } else {
            cursor_row_++;
        }
    }
    cursor_col_ = 0;
}

// The palette sink ships with. 0-15 are chosen rather than derived; the rest
// is the xterm arrangement, which every application's colour arithmetic
// assumes: a 6x6x6 cube on the levels 0, 95, 135, 175, 215, 255, then a
// 24-step greyscale ramp.
SDL_FColor TerminalGrid::default_palette_color(int index) {
    static const SDL_FColor named[16] = {
        {0.05f, 0.05f, 0.05f, 1.0f},  {0.85f, 0.15f, 0.15f, 1.0f},
        {0.15f, 0.85f, 0.15f, 1.0f},  {0.85f, 0.75f, 0.15f, 1.0f},
        {0.15f, 0.15f, 0.85f, 1.0f},  {0.85f, 0.15f, 0.85f, 1.0f},
        {0.15f, 0.85f, 0.85f, 1.0f},  {0.85f, 0.85f, 0.85f, 1.0f},
        {0.30f, 0.30f, 0.30f, 1.0f},  {1.00f, 0.30f, 0.30f, 1.0f},
        {0.30f, 1.00f, 0.30f, 1.0f},  {1.00f, 1.00f, 0.30f, 1.0f},
        {0.30f, 0.30f, 1.00f, 1.0f},  {1.00f, 0.30f, 1.00f, 1.0f},
        {0.30f, 1.00f, 1.00f, 1.0f},  {1.00f, 1.00f, 1.00f, 1.0f},
    };
    index = std::clamp(index, 0, 255);
    if (index < 16) return named[index];
    if (index < 232) {
        int n = index - 16;
        int levels[3] = { n / 36, (n / 6) % 6, n % 6 };
        float rgb[3];
        for (int i = 0; i < 3; ++i) {
            rgb[i] = (levels[i] == 0 ? 0 : levels[i] * 40 + 55) / 255.0f;
        }
        return { rgb[0], rgb[1], rgb[2], 1.0f };
    }
    float v = (8 + 10 * (index - 232)) / 255.0f;
    return { v, v, v, 1.0f };
}

const SDL_FColor& TerminalGrid::palette_color(int index) const {
    return palette_[std::clamp(index, 0, 255)];
}

void TerminalGrid::set_palette_color(int index, const SDL_FColor& c) {
    if (index >= 0 && index < 256) palette_[index] = c;
}

void TerminalGrid::reset_palette_color(int index) {
    if (index >= 0 && index < 256) palette_[index] = default_palette_color(index);
}

void TerminalGrid::reset_palette() {
    for (int i = 0; i < 256; ++i) palette_[i] = default_palette_color(i);
}

void TerminalGrid::reset_default_fg() { default_fg_ = kDefaultFg; }

void TerminalGrid::reset_default_bg() {
    default_bg_ = kDefaultBg;
    reported_bg_ = kDefaultReportedBg;
    blank_row_valid_ = false;
}

void TerminalGrid::reset_default_cursor_color() { default_cursor_ = kDefaultCursor; }

void TerminalGrid::queue_reply(const std::string& bytes) {
    if (pending_reply_.size() + bytes.size() > kMaxPendingReplyBytes) return;
    pending_reply_ += bytes;
}

std::string TerminalGrid::take_pending_reply() {
    std::string out;
    out.swap(pending_reply_);
    return out;
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

TerminalGrid::RowView TerminalGrid::row_view(int row, int view_offset) const {
    int total_history = static_cast<int>(scrollback_history_.size());
    int line_idx = row + (total_history - view_offset);

    // Above the oldest line still in history. render() asks for row -1 to
    // cover the sub-row smooth-scroll shift, so this is reachable whenever the
    // view is pinned to the very top -- and indexing the deque at -1 would be
    // reading off the front of it.
    if (line_idx < 0) return {};

    if (line_idx < total_history) {
        const auto& hist_row = scrollback_history_[line_idx].cells;
        return { hist_row.data(), static_cast<int>(hist_row.size()) };
    }
    int active_row = line_idx - total_history;
    if (active_row >= rows_) return {};
    return { row_data(active_row), cols_ };
}

Cell TerminalGrid::get_cell_at(int col, int row) const {
    RowView view = row_view(row, scroll_offset_);
    if (col >= 0 && col < view.len) return view.cells[col];
    return Cell{ 32, current_fg_packed_, current_bg_packed_ };
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

    // Opens the glyph atlas's per-pass reset budget. Without it, a screen
    // wanting more distinct glyphs than the atlas holds re-rasterizes all of
    // them every frame forever instead of just missing a few.
    font_manager.begin_frame();

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

    // rows_ + 1: the cell loop may draw one row above the grid to cover the
    // sub-row smooth-scroll shift (see first_row below).
    size_t total_cells = static_cast<size_t>((rows_ + 1) * cols_);
    bg_vertices_.reserve(total_cells * 4 + 32);
    bg_indices_.reserve(total_cells * 6 + 48);
    text_vertices_.reserve(total_cells * 4);
    text_indices_.reserve(total_cells * 6);
    dyn_text_vertices_.reserve(total_cells * 4);
    dyn_text_indices_.reserve(total_cells * 6);
    

    // Smooth scrolling. display_scroll_offset_ is where the view actually is;
    // scroll_offset_ is where the wheel has asked it to be. The two are equal
    // only once the glide has settled.
    //
    // The rows drawn below are fetched at the *interpolated* position, with
    // only the sub-row remainder applied as a pixel shift. Fetching the target
    // rows and translating them by the whole lag instead -- which is what this
    // used to do -- leaves a band of the viewport with nothing drawn in it,
    // one row tall for every row of lag, and never fetches the rows that
    // belong there. Dragging slowly lags well under a row so it never shows;
    // a fast flick through a deep scrollback lags tens of rows, which is most
    // of the screen, and reads as the terminal failing to keep up with the
    // scroll rather than as an animation.
    float target_scroll = static_cast<float>(scroll_offset_);
    // Momentum can move scroll_offset_ by dozens of lines in a single frame,
    // far faster than the lerp below closes the gap. Past a screenful of lag
    // the glide has stopped conveying motion and is just latency, so cap it.
    float max_lag = static_cast<float>(rows_);
    display_scroll_offset_ = std::clamp(display_scroll_offset_,
                                        target_scroll - max_lag,
                                        target_scroll + max_lag);
    display_scroll_offset_ += (target_scroll - display_scroll_offset_) * std::min(1.0f, dt * 22.0f);
    // Settle exactly rather than asymptotically, so a view at rest sits on a
    // row boundary and needs no overscan row at all.
    if (std::fabs(target_scroll - display_scroll_offset_) < 0.01f) {
        display_scroll_offset_ = target_scroll;
    }

    int view_offset = static_cast<int>(std::floor(display_scroll_offset_));
    float scroll_diff_y = (display_scroll_offset_ - view_offset) * cell_h; // in [0, cell_h)
    // Shifting the rows down by a fraction of a row uncovers that fraction of
    // the row above the grid, so start the loop one row early. The clip
    // installed before the draw calls trims what it puts above start_y.
    int first_row = (scroll_diff_y > 0.0f) ? -1 : 0;

    // 1. Draw Grid Cells
    // Stands in for any column a row doesn't reach: scrollback rows are stored
    // at whatever width they were captured at, which can be narrower than the
    // grid is now.
    const Cell blank_cell{ 32, current_fg_packed_, current_bg_packed_ };
    for (int r = first_row; r < rows_; ++r) {
        // Everything row-invariant is resolved once here instead of per cell.
        // The search span especially: it used to be a linear scan of every
        // match for every one of the ~10k cells on screen.
        RowView row_view_cells = row_view(r, view_offset);
        int sel_first = 0, sel_last = -1;
        selected_span(r, view_offset, sel_first, sel_last);
        size_t match_begin = 0, match_end = 0;
        search_span(r, view_offset, match_begin, match_end);

        auto cell_of = [&](int c) -> const Cell& {
            return (c >= 0 && c < row_view_cells.len) ? row_view_cells.cells[c] : blank_cell;
        };

        for (int c = 0; c < cols_; ++c) {
            const Cell& cell = cell_of(c);

            float x0 = start_x + c * cell_w;
            float y0 = start_y + r * cell_h + scroll_diff_y;
            float x1 = x0 + cell_w;
            float y1 = y0 + cell_h;

            // Resolve SGR attributes into effective colors. Reverse swaps
            // the pair (a transparent bg reverses against near-black so the
            // glyph doesn't vanish); dim darkens the foreground.
            SDL_FColor cell_fg = unpack_color(cell.fg);
            SDL_FColor cell_bg = unpack_color(cell.bg);
            if (cell.attrs & ATTR_REVERSE) {
                SDL_FColor new_bg = cell_fg;
                cell_fg = (cell_bg.a > 0.0f) ? cell_bg
                                             : SDL_FColor{0.05f, 0.05f, 0.05f, 1.0f};
                cell_bg = new_bg;
            }
            if (cell.attrs & ATTR_DIM) {
                cell_fg.r *= 0.6f;
                cell_fg.g *= 0.6f;
                cell_fg.b *= 0.6f;
            }

            // Populate Background Geometry
            SDL_FColor bg_color = cell_bg;
            bool selected = (c >= sel_first && c <= sel_last);
            bool search_matched = false;
            for (size_t i = match_begin; i < match_end; ++i) {
                const SearchResult& m = search_matches_[i];
                if (c >= m.col && c < m.col + m.len) { search_matched = true; break; }
            }

            if (selected) {
                bg_color = { 1.00f, 0.60f, 0.00f, 0.35f }; // Premium Translucent Amber Gold Selection
            } else if (search_matched) {
                bg_color = { 0.00f, 0.90f, 1.00f, 0.45f }; // Translucent Electric Cyan Search Highlight
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

            // A cluster cell holds several codepoints that form one visible
            // character; it is rendered as a shaped unit further down and
            // takes no part in ligature substitution.
            int cp_count = 0;
            const char32_t* cp_text = cell_text(cell, cp_count);
            const bool is_cluster_cell = cp_count > 1;
            const char32_t base_cp = cp_count > 0 ? cp_text[0] : 32;

            // Ligature detection & Codepoint substitution
            char32_t render_cp = base_cp;
            bool skip_text = false;

            if (enable_ligatures_ && !is_cluster_cell && c < cols_ - 1) {
                const Cell& next_cell = cell_of(c + 1);
                char32_t c1 = base_cp;
                char32_t c2 = cell_base(next_cell);
                if (c1 == '-' && c2 == '>') render_cp = 0x2192; // →
                else if (c1 == '=' && c2 == '>') render_cp = 0x21D2; // ⇒
                else if (c1 == '!' && c2 == '=') render_cp = 0x2260; // ≠
                else if (c1 == '<' && c2 == '=') render_cp = 0x2264; // ≤
                else if (c1 == '>' && c2 == '=') render_cp = 0x2265; // ≥
                else if (c1 == '=' && c2 == '=') render_cp = 0x2261; // ≡
                else if (c1 == ':' && c2 == ':') render_cp = 0x2237; // ∷
                else if (c1 == '<' && c2 == '<') render_cp = 0x226A; // ≪
                else if (c1 == '>' && c2 == '>') render_cp = 0x226B; // ≫
            }
            if (enable_ligatures_ && !is_cluster_cell && c > 0) {
                const Cell& prev_cell = cell_of(c - 1);
                char32_t p1 = cell_base(prev_cell);
                char32_t p2 = base_cp;
                if ((p1 == '-' && p2 == '>') || (p1 == '=' && p2 == '>') ||
                    (p1 == '!' && p2 == '=') || (p1 == '<' && p2 == '=') ||
                    (p1 == '>' && p2 == '=') || (p1 == '=' && p2 == '=') ||
                    (p1 == ':' && p2 == ':') || (p1 == '<' && p2 == '<') ||
                    (p1 == '>' && p2 == '>')) {
                    skip_text = true;
                }
            }

            // Populate Text Geometry
            // A double-width glyph is drawn across both of its cells. The
            // trailing half holds codepoint 0 and so is skipped by the guard
            // below without needing a check of its own.
            bool is_wide = (char_display_width(base_cp) == 2);

            if (!skip_text && render_cp != 32 && render_cp != 0) {
                bool is_ligature = (render_cp != base_cp && render_cp >= 0x2000);
                // Ligature substitute glyphs are rasterized from a ~2x-size
                // font instance so the stretch below (to visually span two
                // character cells) is a near-1:1 blit instead of a ~2x
                // upscale -- see FontManager::get_ligature_glyph().
                // A cluster is shaped as a whole so the marks land on their
                // base and a ZWJ sequence becomes the one emoji it denotes.
                // If it will not rasterize, fall back to the base character
                // alone, which is what this drew before clusters existed.
                std::string cluster_utf8;
                const GlyphInfo* glyph = nullptr;
                if (is_cluster_cell) {
                    for (int i = 0; i < cp_count; ++i) cluster_utf8 += utf32_to_utf8(cp_text[i]);
                    glyph = font_manager.get_cluster_glyph(renderer, cluster_utf8,
                                                           (cell.attrs & ATTR_BOLD) != 0,
                                                           (cell.attrs & ATTR_ITALIC) != 0);
                }
                if (!glyph) {
                    glyph = is_ligature
                        ? font_manager.get_ligature_glyph(renderer, render_cp)
                        : font_manager.get_glyph(renderer, render_cp,
                                                 (cell.attrs & ATTR_BOLD) != 0,
                                                 (cell.attrs & ATTR_ITALIC) != 0);
                }
                if (glyph && glyph->src_rect.w > 0.0f && glyph->src_rect.h > 0.0f) {
                    // Only *unstyled* ASCII glyphs live in the static atlas
                    // (FontManager::build_atlas only ever rasterizes the
                    // regular face). Bold/italic ASCII -- and everything
                    // non-ASCII, any style -- comes from the dynamic atlas;
                    // src_rect is coordinates *within whichever texture the
                    // glyph actually landed in*, so getting this wrong means
                    // sampling the static atlas at dynamic-atlas coordinates
                    // (or vice versa) -- i.e. reading whatever unrelated
                    // glyph happens to sit there, not a missing/blank glyph.
                    bool is_ascii_regular = !is_cluster_cell &&
                                            render_cp >= 32 && render_cp <= 126 &&
                                            !(cell.attrs & (ATTR_BOLD | ATTR_ITALIC));
                    bool is_dynamic = !is_ascii_regular;
                    float tex_w = is_dynamic ? dyn_atlas_w : atlas_w;
                    float tex_h = is_dynamic ? dyn_atlas_h : atlas_h;

                    if (tex_w > 0.0f && tex_h > 0.0f) {
                        float u0 = glyph->src_rect.x / tex_w;
                        float v0 = glyph->src_rect.y / tex_h;
                        float u1 = (glyph->src_rect.x + glyph->src_rect.w) / tex_w;
                        float v1 = (glyph->src_rect.y + glyph->src_rect.h) / tex_h;

                        float glyph_w = glyph->src_rect.w;
                        float glyph_h = glyph->src_rect.h;

                        if (glyph->is_color || is_ligature || is_wide || is_cluster_cell) {
                            float target_w = (is_ligature || is_wide) ? (cell_w * 2.0f) : cell_w;
                            // Scaling purely to hit the width target assumes
                            // the glyph's natural aspect ratio already fits
                            // "N cells wide, 1 row tall" -- for some
                            // substitute glyphs (this font's arrow
                            // especially) that's off enough that doing so
                            // pushes the height well past the row.
                            // Strictly capping height to fit the row instead
                            // overcorrects the other way: it shrinks the
                            // whole glyph down until it's narrower than a
                            // single character. So: cap height at a modest
                            // overflow allowance (not a strict 1:1 fit) and
                            // let width fall short of the full span if the
                            // glyph's proportions demand it -- a compromise
                            // between "too big" and "too tiny" given a
                            // source glyph that isn't really shaped for
                            // this 2-cells-wide-by-1-row-tall box.
                            float max_h = cell_h * (is_ligature ? 1.4f : 1.0f);
                            float scale_factor = std::min(target_w / glyph_w, max_h / glyph_h);
                            glyph_w *= scale_factor;
                            glyph_h *= scale_factor;
                        }

                        float span_w = (is_ligature || is_wide) ? (cell_w * 2.0f) : cell_w;
                        float gx0 = x0 + (span_w - glyph_w) / 2.0f;
                        float gy0 = y0 + (cell_h - glyph_h) / 2.0f;
                        float gx1 = gx0 + glyph_w;
                        float gy1 = gy0 + glyph_h;

                        SDL_FColor render_color = cell_fg;
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

            // Underline / strikethrough decoration rects ride in the bg
            // batch (drawn beneath glyphs, which is where an underline
            // belongs -- descenders overlap it just like on paper). A
            // hyperlinked cell (OSC 8) is always underlined regardless of
            // its own SGR attrs -- the same "it's a link" affordance every
            // browser and editor uses, independent of whatever styling the
            // app around it applied.
            bool has_underline = (cell.attrs & ATTR_UNDERLINE) || cell.hyperlink_id != 0;
            if (has_underline || (cell.attrs & ATTR_STRIKETHROUGH)) {
                float thickness = std::max(1.0f * display_scale, cell_h * 0.06f);
                auto push_line = [&](float ly) {
                    int base_idx = static_cast<int>(bg_vertices_.size());
                    bg_vertices_.push_back({ {x0, ly}, cell_fg, {0.0f, 0.0f} });
                    bg_vertices_.push_back({ {x1, ly}, cell_fg, {0.0f, 0.0f} });
                    bg_vertices_.push_back({ {x0, ly + thickness}, cell_fg, {0.0f, 0.0f} });
                    bg_vertices_.push_back({ {x1, ly + thickness}, cell_fg, {0.0f, 0.0f} });
                    bg_indices_.push_back(base_idx + 0);
                    bg_indices_.push_back(base_idx + 1);
                    bg_indices_.push_back(base_idx + 2);
                    bg_indices_.push_back(base_idx + 2);
                    bg_indices_.push_back(base_idx + 1);
                    bg_indices_.push_back(base_idx + 3);
                };
                if (has_underline) push_line(y0 + cell_h * 0.88f);
                if (cell.attrs & ATTR_STRIKETHROUGH) push_line(y0 + cell_h * 0.52f);
            }
        }
    }

    // Everything pushed so far is grid cells, and only grid cells get clipped
    // to the text area when the batch is drawn -- the margin and cursor
    // geometry below deliberately paints outside it.
    const size_t cell_bg_index_count = bg_indices_.size();

    // 2. Populate Padding Margin Geometry to eliminate gaps and smearing parallel lines
    if (cols_ > 0 && rows_ > 0) {
        float grid_w = cols_ * cell_w;
        float grid_h = rows_ * cell_h;
        float end_x = start_x + grid_w;
        float end_y = start_y + grid_h;

        // Read at the animated offset, not scroll_offset_: the margin is
        // extending the colour of the corner cell actually on screen, and
        // mid-glide those are rows apart.
        auto corner_bg = [&](int col, int row) {
            RowView v = row_view(row, view_offset);
            const Cell& cell = (col >= 0 && col < v.len) ? v.cells[col] : blank_cell;
            return unpack_color(cell.bg);
        };
        SDL_FColor tl_color = corner_bg(0, 0);
        SDL_FColor tr_color = corner_bg(cols_ - 1, 0);
        SDL_FColor bl_color = corner_bg(0, rows_ - 1);
        SDL_FColor br_color = corner_bg(cols_ - 1, rows_ - 1);

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
    // display_scroll_offset_, not scroll_offset_, so the cursor stays pinned to
    // its row while the view glides instead of jumping to where the scroll is
    // headed and waiting there for the text to arrive.
    float target_row = static_cast<float>(cursor_row_) + display_scroll_offset_;

    float diff_col = std::abs(target_col - visual_cursor_col_);
    float diff_row = std::abs(target_row - visual_cursor_row_);

    if (!animated_typing || diff_col > 3.0f || diff_row > 1.0f) {
        visual_cursor_col_ = target_col;
        visual_cursor_row_ = target_row;
    } else {
        visual_cursor_col_ += (target_col - visual_cursor_col_) * 25.0f * dt;
        visual_cursor_row_ += (target_row - visual_cursor_row_) * 25.0f * dt;
    }

    // 2. Render Opaque Cursor (Append to background draw call)
    if (cursor_visible_ &&
        visual_cursor_col_ >= 0.0f && visual_cursor_col_ < cols_ && visual_cursor_row_ >= 0.0f && visual_cursor_row_ < rows_) {
        float cx0 = start_x + visual_cursor_col_ * cell_w;
        float cy0 = start_y + visual_cursor_row_ * cell_h;
        float cx1 = cx0 + cell_w;
        float cy1 = cy0 + cell_h;

        // DECSCUSR shape. Both thin forms take the same floor as the
        // underline/strikethrough decorations do, so they stay visible at
        // small cell sizes instead of thinning away to nothing.
        if (cursor_shape_ == CursorShape::Underline) {
            float thickness = std::max(2.0f * display_scale, cell_h * 0.12f);
            cy0 = cy1 - thickness;
        } else if (cursor_shape_ == CursorShape::Bar) {
            float thickness = std::max(2.0f * display_scale, cell_w * 0.15f);
            cx1 = cx0 + thickness;
        }
        
        SDL_FColor cursor_color = default_cursor_; // OSC 12 can change this
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

    // Clip the cell geometry to the text area. Mid-glide the loop above emits
    // a row that starts above start_y and pushes the bottom row past the last
    // row's baseline; without this, both slivers land in the window padding,
    // on top of the margin fill that is supposed to own it. Only the top and
    // bottom edges are tightened -- horizontally the clip is left as the
    // caller set it, because glyphs wider than their cell already overhang the
    // grid's left and right edges by design.
    SDL_Rect prev_clip{};
    const bool had_clip = SDL_RenderClipEnabled(renderer);
    if (had_clip) SDL_GetRenderClipRect(renderer, &prev_clip);

    SDL_Rect cell_clip = {
        had_clip ? prev_clip.x : 0,
        static_cast<int>(std::floor(start_y)),
        had_clip ? prev_clip.w : win_w,
        static_cast<int>(std::ceil(rows_ * cell_h))
    };
    if (had_clip && !SDL_GetRectIntersection(&prev_clip, &cell_clip, &cell_clip)) {
        cell_clip = { 0, 0, 0, 0 }; // nothing of the grid is visible
    }
    auto clip_to_cells = [&]() { SDL_SetRenderClipRect(renderer, &cell_clip); };
    auto clip_restore  = [&]() { SDL_SetRenderClipRect(renderer, had_clip ? &prev_clip : nullptr); };

    // 3. Draw Background Color Rectangles
    if (!bg_vertices_.empty()) {
        // Split at the boundary recorded above so the margin fill and the
        // cursor keep drawing over the full pane, in the same order as before.
        if (cell_bg_index_count > 0) {
            clip_to_cells();
            SDL_RenderGeometry(renderer, nullptr, bg_vertices_.data(), static_cast<int>(bg_vertices_.size()), bg_indices_.data(), static_cast<int>(cell_bg_index_count));
            clip_restore();
        }
        if (bg_indices_.size() > cell_bg_index_count) {
            SDL_RenderGeometry(renderer, nullptr, bg_vertices_.data(), static_cast<int>(bg_vertices_.size()), bg_indices_.data() + cell_bg_index_count, static_cast<int>(bg_indices_.size() - cell_bg_index_count));
        }
    }



    // 4. Draw inline images.
    //
    // Between the cell backgrounds and the glyphs, so text written over an
    // image stays readable -- which is what a shell prompt drawn after a
    // picture needs. Clipped to the text area like the cells are, so a
    // placement scrolling off the top does not spill into the padding.
    if (!images_.placements().empty()) {
        clip_to_cells();
        // Screen row of a placement: its line id, less the lines that have
        // fallen out of history, less the lines currently scrolled above the
        // viewport. Signed, because a placement can start above the top of the
        // screen and still have rows visible below it.
        const int64_t first_line = static_cast<int64_t>(lines_evicted_) +
                                   static_cast<int64_t>(scrollback_history_.size()) -
                                   static_cast<int64_t>(view_offset);
        for (const ImagePlacement& p : images_.placements()) {
            int64_t top_row = static_cast<int64_t>(p.line_id) - first_line;
            if (top_row + p.rows <= 0 || top_row >= rows_) continue; // off screen
            if (p.col >= cols_) continue;

            SDL_Texture* tex = images_.texture_for(renderer, p.image_id);
            if (!tex) continue;

            SDL_FRect dst = {
                start_x + p.col * cell_w,
                start_y + static_cast<float>(top_row) * cell_h + scroll_diff_y,
                p.cols * cell_w,
                p.rows * cell_h
            };
            const TerminalImage* img = images_.find(p.image_id);
            SDL_FRect src = {
                static_cast<float>(p.src_x), static_cast<float>(p.src_y),
                static_cast<float>(p.src_w > 0 ? p.src_w : (img ? img->width : 0)),
                static_cast<float>(p.src_h > 0 ? p.src_h : (img ? img->height : 0))
            };
            if (src.w <= 0.0f || src.h <= 0.0f) continue;
            SDL_RenderTexture(renderer, tex, &src, &dst);
        }
        clip_restore();
    }

    // 5. Draw Final Crisp Text Glyphs
    if (!text_vertices_.empty() || (dyn_atlas && !dyn_text_vertices_.empty())) {
        clip_to_cells();
        if (!text_vertices_.empty()) {
            SDL_RenderGeometry(renderer, atlas, text_vertices_.data(), static_cast<int>(text_vertices_.size()), text_indices_.data(), static_cast<int>(text_indices_.size()));
        }
        if (dyn_atlas && !dyn_text_vertices_.empty()) {
            SDL_RenderGeometry(renderer, dyn_atlas, dyn_text_vertices_.data(), static_cast<int>(dyn_text_vertices_.size()), dyn_text_indices_.data(), static_cast<int>(dyn_text_indices_.size()));
        }
        clip_restore();
    }

    // 6. Draw Translucent macOS Scrollbar Overlay
    if (win_w > 0 && win_h > 0) {
        int total_history = static_cast<int>(scrollback_history_.size());
        int total_rows = total_history + rows_;
        
        if (total_rows > rows_) {
            float track_y = 8.0f * display_scale;
            float track_h = static_cast<float>(win_h) - 16.0f * display_scale;
            
            float thumb_h = std::max(24.0f * display_scale, track_h * (static_cast<float>(rows_) / total_rows));
            
            // Tracks the animated position, so the thumb travels with the
            // text rather than arriving at the destination ahead of it.
            float frac = 0.0f;
            if (total_history > 0) {
                frac = std::clamp(display_scroll_offset_ / total_history, 0.0f, 1.0f);
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
    
    if (!is_word_delimiter(cell_base(clicked_cell))) {
        while (start > 0) {
            Cell cell = get_cell_at(start - 1, row);
            if (is_word_delimiter(cell_base(cell))) {
                break;
            }
            start--;
        }
        
        int end = std::clamp(col, 0, cols_ - 1);
        while (end < cols_ - 1) {
            Cell cell = get_cell_at(end + 1, row);
            if (is_word_delimiter(cell_base(cell))) {
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

void TerminalGrid::selected_span(int row, int view_offset, int& first, int& last) const {
    first = 0;
    last = -1; // empty
    if (!has_selection_) return;

    int total_history = static_cast<int>(scrollback_history_.size());
    int grid_row = row + (total_history - view_offset);

    int r0 = select_start_row_;
    int c0 = select_start_col_;
    int r1 = select_end_row_;
    int c1 = select_end_col_;

    if (r0 > r1 || (r0 == r1 && c0 > c1)) {
        std::swap(r0, r1);
        std::swap(c0, c1);
    }

    if (grid_row < r0 || grid_row > r1) return;
    // Interior rows are selected end to end; only the anchor rows are clipped
    // to the column the drag started or finished at.
    first = (grid_row == r0) ? c0 : 0;
    last = (grid_row == r1) ? c1 : cols_ - 1;
}

bool TerminalGrid::is_cell_selected(int col, int row) const {
    int first = 0, last = -1;
    selected_span(row, scroll_offset_, first, last);
    return col >= first && col <= last;
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
                    row_cells[c] = row_data(active_row)[c];
                }
                is_wrapped = (active_row < static_cast<int>(row_wrapped_.size())) ? row_wrapped_[active_row] : false;
            }
        }
        
        if (!row_cells.empty()) {
            // Strip trailing spaces on last row if it has a hard newline
            int limit_col = ec;
            if (!is_wrapped && r == r1) {
                while (limit_col >= sc && row_cells[limit_col].codepoint == 32 && row_cells[limit_col].bg.a == 0) {
                    limit_col--;
                }
            }
            
            for (int c = sc; c <= limit_col; ++c) {
                // Skip the trailing half of a double-width pair: it holds no
                // codepoint of its own and would otherwise emit a NUL byte.
                if (!(row_cells[c].attrs & ATTR_WIDE_CONT)) {
                    append_cell_utf8(row_cells[c], text);
                }
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
                    row_cells[c] = row_data(active_row)[c];
                }
                is_wrapped = (active_row < static_cast<int>(row_wrapped_.size())) ? row_wrapped_[active_row] : false;
            }
        }
        
        if (!row_cells.empty()) {
            int limit_col = cols_ - 1;
            // Trim trailing spaces if the row has a hard newline
            if (!is_wrapped) {
                while (limit_col >= 0 && row_cells[limit_col].codepoint == 32 && row_cells[limit_col].bg.a == 0) {
                    limit_col--;
                }
            }
            
            if (!is_wrapped && r < total_rows - 1) {
                text += "\n";
            }
        }
    }
    return text;
}

std::string TerminalGrid::get_current_line_text() const {
    if (cells_.empty() || cursor_row_ < 0 || cursor_row_ >= rows_) return "";
    
    int p_start_row = cursor_row_;
    while (p_start_row > 0 && (p_start_row - 1) < static_cast<int>(row_wrapped_.size()) && row_wrapped_[p_start_row - 1]) {
        p_start_row--;
    }
    
    int p_end_row = cursor_row_;
    while (p_end_row < rows_ - 1 && p_end_row < static_cast<int>(row_wrapped_.size()) && row_wrapped_[p_end_row]) {
        p_end_row++;
    }

    std::string line;
    for (int r = p_start_row; r <= p_end_row; ++r) {
        int start_col = (r == p_start_row && prompt_boundary_col_ >= 0) ? prompt_boundary_col_ : 0;
        for (int c = start_col; c < cols_; ++c) {
            const Cell& cell = row_data(r)[c];
            char32_t cp = cell_base(cell);
            if (cp >= 32 && cp <= 126 && !is_cluster_ref(cell.codepoint)) {
                line += static_cast<char>(cp);
            } else if (cp > 126 || is_cluster_ref(cell.codepoint)) {
                append_cell_utf8(cell, line);
            }
        }
    }
    size_t last = line.find_last_not_of(" \t\r\n");
    if (last != std::string::npos) {
        return line.substr(0, last + 1);
    }
    return line;
}

void TerminalGrid::set_search_active(bool active) {
    search_active_ = active;
    if (!active) {
        search_query_.clear();
        search_matches_.clear();
        current_match_index_ = -1;
    }
}

// ASCII-only case folding for search. ::tolower on a byte >= 0x80 is
// undefined for a plain char and case rules outside ASCII need real Unicode
// tables, so matching stays case-insensitive for ASCII and exact elsewhere.
static inline char32_t search_fold(char32_t cp) {
    return (cp >= U'A' && cp <= U'Z') ? cp + 32 : cp;
}

// Decodes a UTF-8 query into folded codepoints. Malformed input is skipped
// rather than rejected: the find bar re-runs the search on every keystroke,
// and backspace pops a byte at a time, so a partial character is a normal
// transient state here rather than an error.
static std::vector<char32_t> decode_search_query(const std::string& q) {
    std::vector<char32_t> out;
    for (size_t i = 0; i < q.size(); ) {
        unsigned char b = static_cast<unsigned char>(q[i]);
        char32_t cp;
        int n;
        if (b < 0x80)                { cp = b;          n = 1; }
        else if ((b & 0xE0) == 0xC0) { cp = b & 0x1Fu;  n = 2; }
        else if ((b & 0xF0) == 0xE0) { cp = b & 0x0Fu;  n = 3; }
        else if ((b & 0xF8) == 0xF0) { cp = b & 0x07u;  n = 4; }
        else { ++i; continue; }
        if (i + static_cast<size_t>(n) > q.size()) break; // truncated tail
        for (int k = 1; k < n; ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(q[i + k]) & 0x3Fu);
        }
        out.push_back(search_fold(cp));
        i += static_cast<size_t>(n);
    }
    return out;
}

void TerminalGrid::set_search_query(const std::string& query) {
    search_query_ = query;
    search_matches_.clear();
    current_match_index_ = -1;

    if (query.empty()) return;

    // Matching runs over codepoints, not UTF-8 bytes.
    //
    // This used to build a std::string of each row's text alongside a parallel
    // byte-index -> column map, then lowercase a *copy* of that string (the
    // helper took its argument by value) and run std::string::find over the
    // result. Three allocations and a full re-encode per row, for every row of
    // scrollback, on every keystroke in the find bar -- 15ms per keypress at
    // the default 10,000-line scrollback and 133ms at 100,000.
    //
    // Comparing codepoints directly drops the encode, the map and the copy,
    // and columns fall out of the walk instead of having to be recovered from
    // a byte offset -- which is exactly what an earlier bug here got wrong.
    const std::vector<char32_t> needle = decode_search_query(query);
    if (needle.empty()) return;
    const int qn = static_cast<int>(needle.size());

    int total_history = static_cast<int>(scrollback_history_.size());
    int total_rows = total_history + rows_;

    // Folded view of one cell. Control cells read as a space, matching what
    // the grid draws. A cluster matches on its base character, so searching
    // for a plain letter still finds one carrying a combining mark.
    auto cell_char = [this](const Cell& cell) {
        char32_t cp = cell_base(cell);
        return search_fold(cp < 32 ? U' ' : cp);
    };

    for (int abs_r = 0; abs_r < total_rows; ++abs_r) {
        const Cell* cells;
        int len;
        if (abs_r < total_history) {
            const auto& row_cells = scrollback_history_[abs_r].cells;
            cells = row_cells.data();
            len = static_cast<int>(row_cells.size());
        } else {
            cells = row_data(abs_r - total_history);
            len = cols_;
        }
        // A row holds at most one character per column, so this cannot match.
        if (len < qn) continue;

        // Scanned straight off the cells, with no per-row buffer of any kind.
        // Nearly every column fails on the first character, so the inner walk
        // almost never runs and this costs about one folded compare per cell.
        for (int c = 0; c + qn <= len; ) {
            // The trailing half of a double-width pair carries no character of
            // its own and can neither start nor appear within a match.
            if (cells[c].attrs & ATTR_WIDE_CONT) { ++c; continue; }
            if (cell_char(cells[c]) != needle[0]) { ++c; continue; }

            int matched = 1;
            int cc = c + 1;
            while (matched < qn && cc < len) {
                if (cells[cc].attrs & ATTR_WIDE_CONT) { ++cc; continue; }
                if (cell_char(cells[cc]) != needle[matched]) break;
                ++matched;
                ++cc;
            }
            if (matched < qn) { ++c; continue; }

            // cc sits just past the last matched *lead* cell; a double-width
            // character's second column belongs to the match too.
            while (cc < len && (cells[cc].attrs & ATTR_WIDE_CONT)) ++cc;

            SearchResult res;
            res.absolute_row = abs_r;
            res.col = c;
            res.len = std::max(1, cc - c);
            search_matches_.push_back(res);
            c = cc; // matches don't overlap, as before
        }
    }

    if (!search_matches_.empty()) {
        current_match_index_ = 0;
        int match_abs_r = search_matches_[0].absolute_row;
        int target_scroll = total_history - (match_abs_r - rows_ / 2);
        if (target_scroll < 0) target_scroll = 0;
        if (target_scroll > total_history) target_scroll = total_history;
        scroll_offset_ = target_scroll;
    }
}

void TerminalGrid::search_next() {
    if (search_matches_.empty()) return;
    current_match_index_ = (current_match_index_ + 1) % search_matches_.size();
    
    int total_history = static_cast<int>(scrollback_history_.size());
    int match_abs_r = search_matches_[current_match_index_].absolute_row;
    int target_scroll = total_history - (match_abs_r - rows_ / 2);
    if (target_scroll < 0) target_scroll = 0;
    if (target_scroll > total_history) target_scroll = total_history;
    scroll_offset_ = target_scroll;
}

void TerminalGrid::search_prev() {
    if (search_matches_.empty()) return;
    current_match_index_ = (current_match_index_ - 1 + static_cast<int>(search_matches_.size())) % static_cast<int>(search_matches_.size());
    
    int total_history = static_cast<int>(scrollback_history_.size());
    int match_abs_r = search_matches_[current_match_index_].absolute_row;
    int target_scroll = total_history - (match_abs_r - rows_ / 2);
    if (target_scroll < 0) target_scroll = 0;
    if (target_scroll > total_history) target_scroll = total_history;
    scroll_offset_ = target_scroll;
}

void TerminalGrid::search_span(int row, int view_offset, size_t& begin, size_t& end) const {
    begin = 0;
    end = 0;
    if (!search_active_ || search_matches_.empty()) return;

    int total_history = static_cast<int>(scrollback_history_.size());
    int abs_row = row + (total_history - view_offset);

    auto by_row = [](const SearchResult& m, int r) { return m.absolute_row < r; };
    auto lo = std::lower_bound(search_matches_.begin(), search_matches_.end(), abs_row, by_row);
    auto hi = std::lower_bound(lo, search_matches_.end(), abs_row + 1, by_row);
    begin = static_cast<size_t>(lo - search_matches_.begin());
    end = static_cast<size_t>(hi - search_matches_.begin());
}

bool TerminalGrid::is_cell_search_matched(int col, int row) const {
    size_t begin = 0, end = 0;
    search_span(row, scroll_offset_, begin, end);
    for (size_t i = begin; i < end; ++i) {
        const SearchResult& m = search_matches_[i];
        if (col >= m.col && col < m.col + m.len) return true;
    }
    return false;
}
