#include "prompt_editing.hpp"
#include "terminal_grid.hpp"

#include <algorithm>

// How a click becomes cursor keys.
//
// The shell moves its cursor one *character* per arrow key, through a buffer
// that may contain newlines. The screen shows that buffer laid out in cells
// after the prompt. So placing the cursor means measuring, in characters, how
// far the click is from the cursor -- which needs three things the screen
// does not state outright:
//
// 1. Where the input begins. The anchor (TerminalGrid::InputAnchor): the
//    cursor's resting place after output, taken until the user starts editing
//    and then held, so the shell's own redraws can't move it. It used to be a
//    column reset by every carriage return and re-latched wherever the cursor
//    sat when output paused -- and zsh redraws a wrapped or multi-line command
//    with carriage returns, so the boundary drifted into the middle of the
//    command and everything to its left stopped responding to clicks. That
//    stayed broken after the command was edited back down to one line,
//    because nothing put the boundary back.
//
// 2. Where each row ends. A row that soft-wraps contributes all its cells and
//    no newline. A row that ends in a real line break contributes its text up
//    to the last character, then one newline -- the blank cells after it are
//    not characters. Counting cells instead put every row break off by the
//    width of the blank space after it, which is why only the first line of
//    a multi-line command could ever be clicked.
//
// 3. How many characters a cell is. A wide character (CJK, emoji) covers two
//    cells and is one character; its right half is skipped.
//
// The cursor and the click are both measured from the input start, and the
// difference is sent as arrow keys.

namespace prompt_edit {
namespace {

struct Region {
    int start_row = 0, start_col = 0;  // first input cell; row < 0 is in history
    int end_row = 0;                   // last row belonging to the line
    int cursor_row = 0, cursor_col = 0;
    int cols = 0;
};

bool cell_blank(const Cell& cell) {
    if (cell.attrs & ATTR_WIDE_CONT) return false;
    return cell.codepoint == 0 || cell.codepoint == U' ';
}

bool row_blank(const TerminalGrid& g, int row) {
    int len = 0;
    const Cell* cells = g.line_cells(row, len);
    if (!cells) return true;
    for (int c = 0; c < len; ++c) {
        if (!cell_blank(cells[c])) return false;
    }
    return true;
}

int index_at(const TerminalGrid& g, const Region& R, int row, int col);
bool soft_wrapped(const TerminalGrid& g, const Region& R, int row);

// Finds the line being edited: from the input start down through the cursor,
// and on through any rows directly below it that still hold text -- the
// cursor can be partway up a multi-line command.
bool find_region(const TerminalGrid& g, Region& R) {
    if (g.is_alt_screen_active()) return false;
    if (!g.input_start(R.start_row, R.start_col)) return false;
    R.cursor_row = g.get_cursor_row();
    R.cursor_col = g.get_cursor_col();
    R.cols = g.get_cols();
    if (R.start_row > R.cursor_row) return false;

    int end = R.cursor_row;
    while (end + 1 < g.get_rows() && !row_blank(g, end + 1)) ++end;
    // A line that exactly fills its last row ends at the start of the next,
    // blank, one -- that is where the shell puts the cursor at its end. A
    // full row with its wrap flag set really does continue; a wrap flag on a
    // row that no longer reaches the edge is left over from when the line
    // was longer.
    int len = 0;
    const Cell* last = g.line_cells(end, len);
    if (end + 1 < g.get_rows() && g.line_wrapped(end) && last && len > 0 &&
        !cell_blank(last[len - 1])) {
        ++end;
    }
    R.end_row = end;

    // When the shell has said how long the line is, where it ends needn't be
    // guessed from the screen: it is the row holding that position. Text
    // drawn after the line -- an autosuggestion -- otherwise looks like more
    // of it, and when it reached the edge, the blank row below was taken in
    // too. A line ending exactly at the edge of a wrapping row ends at the
    // start of the next.
    const int length = g.input_anchor().line_length;
    if (length >= 0) {
        for (int r = R.start_row; r < R.end_row; ++r) {
            int end_of_row = index_at(g, R, r, R.cols);
            if (end_of_row > length || (end_of_row == length && !soft_wrapped(g, R, r))) {
                R.end_row = std::max(r, R.cursor_row);
                break;
            }
        }
    }
    return true;
}

// A wrap flag on the region's last row is stale -- left over from when the
// command was longer -- since nothing of the line follows it.
bool soft_wrapped(const TerminalGrid& g, const Region& R, int row) {
    return row < R.end_row && g.line_wrapped(row);
}

// One past the last cell of `row` that holds a character of the line.
int row_extent(const TerminalGrid& g, const Region& R, int row) {
    int len = 0;
    const Cell* cells = g.line_cells(row, len);
    if (!cells) return 0;
    int ext;
    if (soft_wrapped(g, R, row)) {
        ext = len;
        // A wide character that would not fit in the last column goes to the
        // next row, leaving a blank that is padding rather than a space.
        int next_len = 0;
        const Cell* next = g.line_cells(row + 1, next_len);
        if (len > 0 && cell_blank(cells[len - 1]) && next && next_len > 1 &&
            (next[1].attrs & ATTR_WIDE_CONT)) {
            ext = len - 1;
        }
    } else {
        int limit = len;
        // The first row may carry a right-aligned prompt; it is not part of
        // the line. Recognised by its text, at its distance from the edge,
        // with a blank before it -- once the command grows into it the shell
        // stops drawing it, and then those cells are the command's.
        const auto& anchor = g.input_anchor();
        int rp = static_cast<int>(anchor.right_prompt.size());
        int rs = len - anchor.right_prompt_gap - rp;
        if (row == R.start_row && rp > 0 && rs > R.start_col && cell_blank(cells[rs - 1])) {
            bool same = true;
            for (int k = 0; k < rp && same; ++k) {
                same = g.cell_base(cells[rs + k]) == anchor.right_prompt[k];
            }
            if (same) limit = rs;
        }
        ext = 0;
        for (int c = limit - 1; c >= 0; --c) {
            if (!cell_blank(cells[c])) { ext = c + 1; break; }
        }
        // Spaces the user typed at the end of the line are characters, and
        // the cursor sits after them.
        if (row == R.cursor_row) ext = std::max(ext, R.cursor_col);
    }
    if (row == R.start_row) ext = std::max(ext, R.start_col);
    return std::min(ext, len);
}

// Characters in cells [from, to) of `row`: every cell but a wide character's
// right half.
int chars_in(const TerminalGrid& g, int row, int from, int to) {
    int len = 0;
    const Cell* cells = g.line_cells(row, len);
    if (!cells) return 0;
    int n = 0;
    for (int c = std::max(0, from); c < std::min(to, len); ++c) {
        if (!(cells[c].attrs & ATTR_WIDE_CONT)) ++n;
    }
    return n;
}

// Buffer index of the character at (row, col), counted from the input start.
// A position past the end of a row's text is that row's line end.
int index_at(const TerminalGrid& g, const Region& R, int row, int col) {
    int idx = 0;
    for (int r = R.start_row; r < row; ++r) {
        int from = (r == R.start_row) ? R.start_col : 0;
        idx += chars_in(g, r, from, row_extent(g, R, r));
        if (!soft_wrapped(g, R, r)) ++idx;  // the newline
    }
    int len = 0;
    const Cell* cells = g.line_cells(row, len);
    // The right half of a wide character means that character.
    if (cells && col > 0 && col < len && (cells[col].attrs & ATTR_WIDE_CONT)) --col;
    int from = (row == R.start_row) ? R.start_col : 0;
    idx += chars_in(g, row, from, std::min(col, row_extent(g, R, row)));
    return idx;
}

// Whether (row, col) is part of the line rather than the prompt or output
// around it.
bool in_line(const Region& R, int row, int col) {
    if (row < R.start_row || row > R.end_row) return false;
    if (row == R.start_row && col < R.start_col) return false;
    return true;
}

// Arrow keys as the keyboard sends them, so DECCKM is honoured: a shell that
// switched to application cursor keys gets the form it asked for.
std::string arrows(int n, bool app_keys) {
    const char* right = app_keys ? "\x1bOC" : "\x1b[C";
    const char* left = app_keys ? "\x1bOD" : "\x1b[D";
    std::string out;
    for (int i = 0; i < n; ++i) out += right;
    for (int i = 0; i < -n; ++i) out += left;
    return out;
}

} // namespace

std::string click_payload(const TerminalGrid& grid, int row, int col) {
    Region R;
    if (!find_region(grid, R)) return {};
    // The click is in viewport rows; scrolled back, those are not the
    // active rows the line is measured in.
    row -= grid.get_scroll_offset();
    col = std::clamp(col, 0, R.cols - 1);
    if (!in_line(R, row, col)) return {};

    int target = index_at(grid, R, row, col);
    int cursor = index_at(grid, R, R.cursor_row, R.cursor_col);
    // The screen can't tell a line's own text from text drawn after it -- an
    // autosuggestion looks like more of the line. When the shell has said how
    // long the line really is, a click past that stops at its end: one step
    // further right is what accepts a suggestion.
    const int length = grid.input_anchor().line_length;
    if (length >= 0) target = std::min(target, length);
    return arrows(target - cursor, grid.is_app_cursor_keys());
}

bool delete_selection_payload(const TerminalGrid& grid, std::string& out) {
    if (!grid.has_selection()) return false;
    Region R;
    if (!find_region(grid, R)) return false;

    // Selection rows count scrollback first; the line is in active rows.
    const int history = static_cast<int>(grid.get_scrollback_size());
    int r0 = grid.get_select_start_row() - history, c0 = grid.get_select_start_col();
    int r1 = grid.get_select_end_row() - history, c1 = grid.get_select_end_col();
    if (r1 < r0 || (r1 == r0 && c1 < c0)) { std::swap(r0, r1); std::swap(c0, c1); }
    if (!in_line(R, r0, c0) || !in_line(R, r1, c1)) return false;

    int a = index_at(grid, R, r0, c0);
    int b = index_at(grid, R, r1, c1);  // inclusive
    // Past the end of the last row there is nothing to delete -- nor past the
    // length the shell reported, if it has.
    int line_end = index_at(grid, R, R.end_row, R.cols);
    if (grid.input_anchor().line_length >= 0) line_end = std::min(line_end, grid.input_anchor().line_length);
    b = std::min(b, line_end - 1);

    out.clear();
    if (b < a) return true;  // selection covers no characters; nothing to do
    int len = b - a + 1;
    int cursor = index_at(grid, R, R.cursor_row, R.cursor_col);
    bool app = grid.is_app_cursor_keys();

    out += arrows(a - cursor, app);
    for (int i = 0; i < len; ++i) out += "\x1b[3~";
    // Back to where the cursor was, less whatever was deleted before it.
    int final_pos = (cursor > b) ? cursor - len : (cursor >= a ? a : cursor);
    out += arrows(final_pos - a, app);
    return true;
}

std::string current_line_text(const TerminalGrid& grid) {
    Region R;
    if (!find_region(grid, R)) return {};
    std::string text;
    for (int r = R.start_row; r <= R.end_row; ++r) {
        int len = 0;
        const Cell* cells = grid.line_cells(r, len);
        if (!cells) continue;
        int from = (r == R.start_row) ? R.start_col : 0;
        int to = row_extent(grid, R, r);
        for (int c = from; c < to; ++c) {
            if (cells[c].attrs & ATTR_WIDE_CONT) continue;
            if (cells[c].codepoint == 0) { text += ' '; continue; }
            grid.append_cell_utf8(cells[c], text);
        }
        if (r < R.end_row && !soft_wrapped(grid, R, r)) text += '\n';
    }
    size_t last = text.find_last_not_of(" \t\r\n");
    return (last == std::string::npos) ? std::string() : text.substr(0, last + 1);
}

} // namespace prompt_edit
