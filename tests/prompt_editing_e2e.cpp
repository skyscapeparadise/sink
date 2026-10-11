// End-to-end tests for mouse editing of the command line.
//
// A real shell runs in a real PTY. Its output goes through sink's ANSIParser
// and TerminalGrid exactly as it would in the app, and clicks are turned into
// keystrokes by prompt_edit -- the same code main.cpp calls -- and written
// back to the shell. Nothing here touches the window server: no window, no
// mouse, no keyboard. It runs alongside whatever else the machine is doing.
//
// The answer comes from the shell, not from sink. A widget bound to a key
// sink never sends writes the shell's own idea of its cursor and buffer to a
// file, without drawing anything. A click is correct when the shell's cursor
// ends up on the character that was under the mouse.
//
// Where that character *should* be is worked out independently of sink: the
// test lays the buffer out from the prompt the way the shell does, checks that
// layout against what the grid actually shows, and only then judges clicks
// against it. A layout that disagrees with the screen is reported as such,
// rather than blamed on the click code.
//
//   prompt_editing_e2e [--verbose] [--only <scenario>] [--shell zsh|bash]

#include "ansi_parser.hpp"
#include "prompt_editing.hpp"
#include "terminal_grid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <optional>
#include <poll.h>
#include <random>
#include <signal.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <util.h>
#include <vector>

namespace {

bool g_verbose = false;

using Clock = std::chrono::steady_clock;

std::u32string utf8_decode(const std::string& s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp;
        int n;
        if (c < 0x80)      { cp = c;        n = 1; }
        else if (c < 0xE0) { cp = c & 0x1F; n = 2; }
        else if (c < 0xF0) { cp = c & 0x0F; n = 3; }
        else               { cp = c & 0x07; n = 4; }
        for (int k = 1; k < n && i + k < s.size(); ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        out += cp;
        i += n;
    }
    return out;
}

// Display width as the shell sees it. Only what the scenarios use: ASCII,
// CJK ideographs and the emoji block are two columns.
int display_width(char32_t cp) {
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0x1F300 && cp <= 0x1FAFF)) {
        return 2;
    }
    return 1;
}

struct Report {
    int cursor = -1;     // character index
    std::u32string buffer;
};

class Session;

struct Layout {
    bool ok = false;
    std::string why;
    int start_row = -1, start_col = -1, end_row = -1;
    // Buffer index under each cell, or -1. Cells past the end of a line on its
    // last row map to that line's terminator, which is where a click there
    // leaves the cursor in every editor.
    std::vector<std::vector<int>> index_at;
};

Layout lay_out(Session& s, const Report& rep, const std::string& prompt);
bool screen_shows(const Layout& L, const Report& rep, int cursor_row, int cursor_col);

// Times a report's screen never caught up with the shell within the wait.
int g_sync_timeouts = 0;

// ---------------------------------------------------------------------------
// A shell in a PTY, rendered by sink's parser into a grid.

class Session {
public:
    Session(const std::string& shell, int cols, int rows, const std::string& extra_rc = "")
        : cols_(cols), rows_(rows) {
        char tmpl[] = "/tmp/sink_pe_XXXXXX";
        dir_ = mkdtemp(tmpl);
        report_path_ = dir_ + "/report";
        grid_.resize(cols, rows);

        std::string rc;
        std::vector<std::string> argv;
        if (shell == "zsh") {
            write_file(dir_ + "/.zshrc",
                "PS1='ps1> '\n"
                "RPROMPT=''\n"
                "HISTFILE=$ZDOTDIR/hist; HISTSIZE=100; SAVEHIST=0\n"
                "unsetopt BEEP\n"
                "zmodload -F zsh/files b:zf_mv\n"
                "typeset -gi _sink_n=0\n"
                "_sink_report() {\n"
                "  zle -R\n"
                "  (( _sink_n++ ))\n"
                "  print -rn -- \"$_sink_n\"$'\\n'\"$CURSOR\"$'\\n'\"$BUFFER\" >| $SINK_REPORT.tmp\n"
                "  zf_mv -f $SINK_REPORT.tmp $SINK_REPORT\n"
                "}\n"
                "zle -N _sink_report\n"
                "bindkey '^[[24~' _sink_report\n" + extra_rc);
            argv = { "/bin/zsh", "-i" };
        } else {
            write_file(dir_ + "/bashrc",
                "PS1='ps1> '\n"
                "bind 'set bell-style none'\n"
                "_sink_n=0\n"
                // No mv: bash has no builtin for it, and a fork per report
                // occasionally makes job control print a setpgid warning into
                // the line under load. One small write instead, ended with a
                // marker so a read that catches it half-done is ignored.
                "_sink_report() { ((_sink_n++)); printf '%s\\n%s\\n%s\\n\\x1fEND' \"$_sink_n\" "
                "\"$READLINE_POINT\" \"$READLINE_LINE\" > \"$SINK_REPORT\"; }\n"
                "bind -x '\"\\e[24~\": _sink_report'\n" + extra_rc);
            argv = { "/opt/homebrew/bin/bash", "--noprofile", "--rcfile", dir_ + "/bashrc", "-i" };
            bash_ = true;
        }

        struct winsize ws{};
        ws.ws_col = static_cast<unsigned short>(cols);
        ws.ws_row = static_cast<unsigned short>(rows);
        pid_ = forkpty(&fd_, nullptr, nullptr, &ws);
        if (pid_ == 0) {
            // The same environment sink gives a shell (see pty_bridge.cpp).
            setenv("TERM", "xterm-256color", 1);
            setenv("TERM_PROGRAM", "sink", 1);
            setenv("LANG", "en_US.UTF-8", 1);
            setenv("LC_ALL", "en_US.UTF-8", 1);
            setenv("ZDOTDIR", dir_.c_str(), 1);
            setenv("SINK_REPORT", report_path_.c_str(), 1);
            unsetenv("PROMPT_COMMAND");
            std::vector<char*> args;
            for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
            args.push_back(nullptr);
            execv(args[0], args.data());
            _exit(127);
        }
        fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
        settle(800, 3000);
    }

    ~Session() {
        if (pid_ > 0) {
            kill(pid_, SIGKILL);
            waitpid(pid_, nullptr, 0);
        }
        if (fd_ >= 0) close(fd_);
        std::string cmd = "rm -rf '" + dir_ + "'";
        (void)std::system(cmd.c_str());
    }

    TerminalGrid& grid() { return grid_; }
    bool is_bash() const { return bash_; }
    int cols() const { return cols_; }
    int rows() const { return rows_; }

    // --- input, sent the way main.cpp sends it ------------------------------

    // Any keypress. main.cpp tells the grid what each key does to the line
    // before sending it -- Enter, Ctrl-C and Ctrl-D submit it, anything else
    // edits it -- so this does too.
    void key(const std::string& bytes) {
        bool submit = bytes == "\r" || bytes == "\x03" ||
                      (bytes == "\x04" && prompt_edit::current_line_text(grid_).empty());
        if (submit) grid_.note_user_submit();
        else        grid_.note_user_edit();
        write_all(bytes);
    }
    void type(const std::string& text) {
        // One keypress per character, as typing arrives.
        for (size_t i = 0; i < text.size();) {
            size_t n = 1;
            unsigned char c = static_cast<unsigned char>(text[i]);
            if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
            key(text.substr(i, n));
            i += n;
        }
    }
    void enter()     { key("\r"); }
    void backspace(int n = 1) { for (int i = 0; i < n; ++i) key("\x7f"); }
    void up()        { key(grid_.is_app_cursor_keys() ? "\x1bOA" : "\x1b[A"); }
    void ctrl(char c) { key(std::string(1, static_cast<char>(c & 0x1F))); }
    void paste(const std::string& text) {
        grid_.note_user_edit();
        if (grid_.is_bracketed_paste_active()) {
            write_all("\x1b[200~" + text + "\x1b[201~");
        } else {
            write_all(text);
        }
    }

    // A window resize, as the app handles it: the grid reflows, then the PTY
    // is told, and the shell redraws for the new width.
    void tell_pty_size(int cols, int rows) {
        struct winsize ws{};
        ws.ws_col = static_cast<unsigned short>(cols);
        ws.ws_row = static_cast<unsigned short>(rows);
        ioctl(fd_, TIOCSWINSZ, &ws);
    }

    void resize(int cols, int rows) {
        cols_ = cols;
        rows_ = rows;
        grid_.resize(cols, rows);
        tell_pty_size(cols, rows);
        // Wait for the shell to react before settling: a shell handles
        // SIGWINCH at its own pace, and a sweep that starts before the
        // repaint lands is measuring the screen as it was.
        auto deadline = Clock::now() + std::chrono::seconds(2);
        while (Clock::now() < deadline && !pump(10)) {}
        settle(150, 1500);
        // Round-trip once more so any repaint queued behind the signal has
        // been drawn before the caller looks at the screen.
        report();
    }

    // A window drag: a run of sizes a frame apart. The grid reflows at every
    // one -- the hardest case for the shell, since the app may lay out less
    // often -- and, as in the app, a shell at its command line is told the
    // size once it has settled (main.cpp, kPtyResizeSettleMs); a shell
    // running a command, or a full-screen app, gets every size.
    void resize_drag(const std::vector<int>& widths, int rows) {
        std::vector<std::pair<int, int>> sizes;
        for (int w : widths) sizes.push_back({ w, rows });
        resize_drag(sizes);
    }
    void resize_drag(const std::vector<std::pair<int, int>>& sizes) {
        int r = 0, c = 0;
        bool at_command_line = grid_.input_start(r, c);
        for (auto [w, h] : sizes) {
            cols_ = w;
            rows_ = h;
            grid_.resize(w, h);
            if (!at_command_line) tell_pty_size(w, h);
            pump(8);
        }
        if (at_command_line) tell_pty_size(sizes.back().first, sizes.back().second);
        std::vector<int> widths;
        for (auto [w, h] : sizes) widths.push_back(w);
        capture_ = g_verbose;
        captured_.clear();
        if (g_verbose) {
            pump(5);
            dump(("drag reflowed to " + std::to_string(widths.back()) + ", before the shell redraws").c_str());
        }
        auto deadline = Clock::now() + std::chrono::seconds(2);
        while (Clock::now() < deadline && !pump(10)) {}
        settle(200, 2000);
        if (g_verbose) {
            capture_ = false;
            std::string esc;
            for (unsigned char ch : captured_) {
                if (ch == 0x1b) esc += "\\e";
                else if (ch == '\r') esc += "\\r";
                else if (ch == '\n') esc += "\\n";
                else if (ch < 32) { char b[8]; std::snprintf(b, sizeof(b), "\\x%02x", ch); esc += b; }
                else esc += static_cast<char>(ch);
            }
            std::printf("    shell's redraw: %s\n", esc.c_str());
        }
        report();
        if (g_verbose) dump(("after the shell redrew at " + std::to_string(widths.back())).c_str());
    }

    // Every row of scrollback and screen, as text.
    std::vector<std::string> all_rows() {
        std::vector<std::string> out;
        int history = static_cast<int>(grid_.get_scrollback_size());
        for (int r = -history; r < rows_; ++r) {
            int len = 0;
            const Cell* cells = grid_.line_cells(r, len);
            std::string line;
            for (int c = 0; c < len && cells; ++c) {
                if (cells[c].attrs & ATTR_WIDE_CONT) continue;
                char32_t cp = cells[c].codepoint;
                line += utf8(cp < 32 ? U' ' : cp);
            }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            out.push_back(line);
        }
        return out;
    }

    // A click and release on the same cell, as main.cpp handles it.
    std::string click(int row, int col) {
        std::string payload = prompt_edit::click_payload(grid_, row, col);
        if (!payload.empty()) {
            write_all(payload);
            grid_.note_user_edit();
        }
        return payload;
    }

    // A drag from one cell to another followed by Delete, as main.cpp handles
    // it. Returns false if sink declined to treat it as a line edit.
    bool select_and_delete(int r0, int c0, int r1, int c1) {
        grid_.start_selection(c0, r0);
        grid_.update_selection(c1, r1);
        grid_.end_selection();
        std::string payload;
        bool ok = prompt_edit::delete_selection_payload(grid_, payload);
        if (ok) {
            if (!payload.empty()) {
                write_all(payload);
                grid_.note_user_edit();
            }
            grid_.clear_selection();
        } else {
            // Not a line edit: main.cpp would send a plain Delete.
            grid_.clear_selection();
        }
        return ok;
    }

    // --- synchronisation ------------------------------------------------------

    // Asks the shell where its cursor is. Doubles as the sync point: the
    // widget runs only after every key sent before it has been processed, and
    // zsh's redraws for those keys reach the PTY before the file does.
    Report report() {
        int before = seq_;
        since_report_.clear();
        collecting_ = true;
        write_all("\x1b[24~");
        auto deadline = Clock::now() + std::chrono::seconds(3);
        Report r;
        while (Clock::now() < deadline) {
            pump(5);
            std::string text;
            if (read_file(report_path_, text)) {
                size_t a = text.find('\n');
                size_t b = (a == std::string::npos) ? std::string::npos : text.find('\n', a + 1);
                // bash's report ends with a marker; without it the file was
                // read mid-write.
                const std::string end_marker = "\n\x1f" "END";
                bool complete = !bash_ || (text.size() >= end_marker.size() &&
                    text.compare(text.size() - end_marker.size(), end_marker.size(), end_marker) == 0);
                if (bash_ && complete) text.resize(text.size() - end_marker.size());
                if (b != std::string::npos && complete) {
                    int seq = std::atoi(text.substr(0, a).c_str());
                    if (seq > before) {
                        seq_ = seq;
                        int point = std::atoi(text.substr(a + 1, b - a - 1).c_str());
                        std::string buf = text.substr(b + 1);
                        r.buffer = utf8_decode(buf);
                        // Both report a character index (bash 5.1 and later;
                        // older bash counted bytes, and is not supported here).
                        r.cursor = point;
                        break;
                    }
                }
            }
        }
        // Whatever the shell draws after the widget returns. zsh has already
        // redrawn (the widget calls zle -R first); bash's readline repaints
        // the whole prompt and line once a bind -x command finishes, and a
        // click judged against a half-repainted screen is judged wrongly.
        //
        // So wait for the screen to say what the shell said -- its buffer
        // laid out after the prompt, the cursor where it reports it -- rather
        // than for a quiet spell of some fixed length. On a machine busy with
        // other work a repaint can stall for longer than any sensible fixed
        // wait, and this suite is meant to run on such a machine.
        //
        // bash needs one thing more. Its repaint redraws identical text, so
        // the screen already matches *before* the repaint starts, and a click
        // computed then lands in the middle of rows being cleared and
        // rewritten. The repaint always prints the prompt again, so for bash
        // the prompt must also have come through since the report key.
        if (!sync_prompt_.empty()) {
            auto until = Clock::now() + std::chrono::seconds(2);
            bool caught_up = false;
            while (Clock::now() < until) {
                bool repainted = !bash_ || since_report_.find(sync_prompt_) != std::string::npos;
                if (repainted) {
                    Layout L = lay_out(*this, r, sync_prompt_);
                    if (screen_shows(L, r, grid_.get_cursor_row(), grid_.get_cursor_col())) {
                        caught_up = true;
                        break;
                    }
                }
                pump(5);
            }
            if (!caught_up) ++g_sync_timeouts;
        } else {
            settle(bash_ ? 80 : 15, 1000);
        }
        collecting_ = false;
        return r;
    }

    // The prompt report() waits to see the shell's buffer drawn after. Set
    // by a sweep for its duration; empty falls back to waiting for quiet.
    std::string sync_prompt_;

    // Reads until the shell has been quiet for `quiet_ms`.
    void settle(int quiet_ms, int max_ms) {
        auto start = Clock::now();
        auto last = Clock::now();
        while (true) {
            if (pump(5)) last = Clock::now();
            auto now = Clock::now();
            if (now - last > std::chrono::milliseconds(quiet_ms)) break;
            if (now - start > std::chrono::milliseconds(max_ms)) break;
        }
    }

    std::string row_text(int r) {
        std::string s;
        for (int c = 0; c < cols_; ++c) {
            Cell cell = grid_.get_cell_at(c, r);
            char32_t cp = cell.codepoint;
            if (cell.attrs & ATTR_WIDE_CONT) continue;
            if (cp < 32) cp = ' ';
            s += utf8(cp);
        }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    }

    void dump(const char* label) {
        int ar = 0, ac = 0;
        bool anchored = grid_.input_start(ar, ac);
        std::printf("    --- screen (%s) cursor r%d c%d input start %s r%d c%d ---\n", label,
                    grid_.get_cursor_row(), grid_.get_cursor_col(),
                    anchored ? "at" : "unknown, last", ar, ac);
        const auto& wrapped = grid_.get_row_wrapped();
        for (int r = 0; r < rows_; ++r) {
            bool w = r < static_cast<int>(wrapped.size()) && wrapped[r];
            std::printf("    %2d%c|%s\n", r, w ? '~' : ' ', row_text(r).c_str());
        }
    }

    static std::string utf8(char32_t cp) {
        std::string out;
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        return out;
    }

private:
    // Reads whatever is available; true if anything arrived. Mirrors the
    // app's per-chunk handling: parse, answer queries, latch the boundary.
    bool pump(int wait_ms) {
        struct pollfd p{ fd_, POLLIN, 0 };
        if (poll(&p, 1, wait_ms) <= 0) return false;
        bool any = false;
        char buf[65536];
        while (true) {
            ssize_t n = ::read(fd_, buf, sizeof(buf));
            if (n <= 0) break;
            if (capture_) captured_.append(buf, static_cast<size_t>(n));
            if (collecting_) since_report_.append(buf, static_cast<size_t>(n));
            any = true;
            parser_.parse(grid_, buf, static_cast<size_t>(n));
        }
        if (any) {
            if (grid_.has_pending_reply()) write_all(grid_.take_pending_reply());
            grid_.note_output_idle();
        }
        return any;
    }

    void write_all(const std::string& s) {
        size_t off = 0;
        while (off < s.size()) {
            ssize_t n = ::write(fd_, s.data() + off, s.size() - off);
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR) { pump(1); continue; }
                return;
            }
            off += static_cast<size_t>(n);
        }
    }

    static void write_file(const std::string& path, const std::string& text) {
        FILE* f = std::fopen(path.c_str(), "w");
        if (f) { std::fputs(text.c_str(), f); std::fclose(f); }
    }
    static bool read_file(const std::string& path, std::string& out) {
        FILE* f = std::fopen(path.c_str(), "r");
        if (!f) return false;
        char buf[8192];
        out.clear();
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        std::fclose(f);
        return true;
    }

    bool capture_ = false;
    std::string captured_;
    bool collecting_ = false;
    std::string since_report_;  // everything the shell sent since the last report key
    int cols_, rows_;
    int fd_ = -1;
    pid_t pid_ = -1;
    bool bash_ = false;
    int seq_ = 0;
    std::string dir_, report_path_;
    TerminalGrid grid_;
    ANSIParser parser_;
};

// ---------------------------------------------------------------------------
// Where the shell drew each character of its buffer.


// Lays `buffer` out after the prompt, the way zsh and readline draw it:
// characters advance by their width, a wide character that would straddle the
// right edge moves to the next row first, and a newline starts the next row.
Layout lay_out(Session& s, const Report& rep, const std::string& prompt) {
    Layout L;
    TerminalGrid& g = s.grid();
    int cols = s.cols(), rows = s.rows();

    // The prompt row: the nearest one at or above the cursor that starts with
    // the prompt. Scenarios keep the prompt string out of their buffers.
    int cur_row = g.get_cursor_row();
    for (int r = cur_row; r >= 0; --r) {
        if (s.row_text(r).rfind(prompt, 0) == 0) { L.start_row = r; break; }
    }
    if (L.start_row < 0) { L.why = "prompt not found on screen"; return L; }
    L.start_col = static_cast<int>(utf8_decode(prompt).size());

    L.index_at.assign(rows, std::vector<int>(cols, -1));
    int r = L.start_row, c = L.start_col;
    int n = static_cast<int>(rep.buffer.size());
    for (int i = 0; i <= n; ++i) {
        if (r >= rows) { L.why = "buffer runs off the bottom of the screen"; return L; }
        bool at_end = (i == n);
        char32_t ch = at_end ? 0 : rep.buffer[i];
        if (at_end || ch == U'\n') {
            // The terminator owns the rest of its row.
            for (int k = c; k < cols; ++k) L.index_at[r][k] = i;
            L.end_row = r;
            if (!at_end) { ++r; c = 0; }
            continue;
        }
        int w = display_width(ch);
        if (c + w > cols) {
            for (int k = c; k < cols; ++k) L.index_at[r][k] = i;
            ++r; c = 0;
            if (r >= rows) { L.why = "buffer runs off the bottom of the screen"; return L; }
        }
        // Check the layout against the screen before trusting it.
        Cell cell = g.get_cell_at(c, r);
        if (cell.codepoint != ch) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "char %d (U+%04X) expected at r%d c%d, screen has U+%04X",
                          i, static_cast<unsigned>(ch), r, c, static_cast<unsigned>(cell.codepoint));
            L.why = buf;
            return L;
        }
        for (int k = 0; k < w; ++k) L.index_at[r][c + k] = i;
        c += w;
        if (c == cols) { ++r; c = 0; }
    }
    L.ok = true;
    return L;
}

// Whether the screen has caught up with a report: the buffer lays out against
// what is drawn, and the cursor sits where the shell says it is -- the first
// cell belonging to that index, which for a line end is just past the text.
bool screen_shows(const Layout& L, const Report& rep, int cursor_row, int cursor_col) {
    if (!L.ok) return false;
    for (size_t r = 0; r < L.index_at.size(); ++r) {
        for (size_t c = 0; c < L.index_at[r].size(); ++c) {
            if (L.index_at[r][c] == rep.cursor) {
                return static_cast<int>(r) == cursor_row && static_cast<int>(c) == cursor_col;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------

struct Totals { int clicks = 0, failed = 0, scenarios = 0, broken = 0; };
Totals g_totals;

// Clicks across every row the buffer touches plus one either side, and checks
// each against the shell. Expected is the character under the click, or "no
// movement" for the prompt and rows outside the line.
void sweep(Session& s, const char* name, const std::string& prompt, int col_step = 1) {
    s.sync_prompt_ = prompt;
    Report base = s.report();
    Layout L = lay_out(s, base, prompt);
    if (!L.ok) {
        std::printf("  [%s] LAYOUT: %s\n", name, L.why.c_str());
        s.dump("layout");
        ++g_totals.broken;
        return;
    }
    int failed = 0, clicks = 0;
    bool dumped = false;
    int r_lo = std::max(0, L.start_row - 1), r_hi = std::min(s.rows() - 1, L.end_row + 1);
    // Cells are visited in a shuffled order, fixed by the scenario's name so a
    // failure reproduces. In screen order every click starts next to the
    // previous one, and an error that both positions share -- a row counted
    // wrongly above them both -- cancels out and goes unseen.
    std::vector<std::pair<int, int>> targets;
    for (int r = r_lo; r <= r_hi; ++r) {
        for (int c = 0; c < s.cols(); c += col_step) targets.push_back({ r, c });
    }
    std::mt19937 rng(static_cast<unsigned>(std::hash<std::string>{}(name)));
    std::shuffle(targets.begin(), targets.end(), rng);

    // Each click's report is the next click's starting point, which halves
    // the round trips to the shell.
    Report before = base;
    for (auto [r, c] : targets) {
        {
            std::optional<int> want;
            if (L.index_at[r][c] >= 0) want = L.index_at[r][c];
            std::string payload = s.click(r, c);
            Report after = s.report();
            ++clicks;
            int expect = want ? *want : before.cursor;
            bool buffer_ok = after.buffer == base.buffer;
            if (after.cursor != expect || !buffer_ok) {
                ++failed;
                if (failed <= 6 || g_verbose) {
                    std::printf("  [%s] click r%d c%d: cursor %d -> %d, expected %d%s%s\n", name, r, c,
                                before.cursor, after.cursor, expect,
                                want ? "" : " (no move)", buffer_ok ? "" : "  BUFFER CHANGED");
                }
                if (!dumped) { s.dump("first failure, after click"); dumped = true; }
            }
            before = after;
        }
    }
    g_totals.clicks += clicks;
    g_totals.failed += failed;
    ++g_totals.scenarios;
    s.sync_prompt_.clear();
    std::printf("  [%s] %d/%d clicks correct\n", name, clicks - failed, clicks);
}

// Selection deletes: drag over a span of the buffer, press Delete, and check
// the shell's buffer lost exactly those characters.
void delete_check(Session& s, const char* name, const std::string& prompt,
                  int r0, int c0, int r1, int c1) {
    Report base = s.report();
    Layout L = lay_out(s, base, prompt);
    if (!L.ok) {
        std::printf("  [%s] LAYOUT: %s\n", name, L.why.c_str());
        ++g_totals.broken;
        return;
    }
    int a = L.index_at[r0][c0], b = L.index_at[r1][c1];
    std::u32string want = base.buffer;
    if (a >= 0 && b >= a) want.erase(a, b - a + 1);
    bool handled = s.select_and_delete(r0, c0, r1, c1);
    Report after = s.report();
    ++g_totals.clicks;
    ++g_totals.scenarios;
    std::string got8, want8;
    for (char32_t ch : after.buffer) got8 += (ch == U'\n') ? std::string("\\n") : Session::utf8(ch);
    for (char32_t ch : want) want8 += (ch == U'\n') ? std::string("\\n") : Session::utf8(ch);
    if (!handled || after.buffer != want) {
        ++g_totals.failed;
        std::printf("  [%s] delete r%d c%d..r%d c%d: %s\n      got  \"%s\"\n      want \"%s\"\n", name,
                    r0, c0, r1, c1, handled ? "wrong result" : "not treated as a line edit",
                    got8.c_str(), want8.c_str());
        s.dump("after delete");
    } else {
        std::printf("  [%s] delete ok\n", name);
    }
}

const char* kPrompt = "ps1> ";

// Output printed before a resize must survive it: present, and only once. A
// shell redrawing after a reflow moves up by the height its line had before,
// so a reflow that changes that height used to send it into the output above
// (erasing it) or leave a stale copy of the line behind.
void survives_resizes(Session& s, const char* name, const std::vector<std::string>& lines) {
    std::vector<std::string> rows = s.all_rows();
    bool ok = true;
    for (const std::string& want : lines) {
        int n = static_cast<int>(std::count(rows.begin(), rows.end(), want));
        if (n != 1) {
            std::printf("  [%s] \"%s\" appears %d times after resizing, expected once\n",
                        name, want.c_str(), n);
            ok = false;
        }
    }
    int prompts = 0;
    for (const std::string& r : rows) prompts += r.rfind(kPrompt, 0) == 0 ? 1 : 0;
    ++g_totals.clicks;
    if (!ok) {
        ++g_totals.failed;
        s.dump("after resizing");
    } else {
        std::printf("  [%s] earlier output intact\n", name);
    }
}

// --- scenarios ---------------------------------------------------------------

using Scenario = std::function<void(Session&)>;
struct Named { const char* name; Scenario run; };

std::vector<Named> scenarios() {
    return {
        { "short line", [](Session& s) {
            s.type("echo hello world");
            sweep(s, "short line", kPrompt);
        }},
        { "soft-wrapped line", [](Session& s) {
            s.type("echo the quick brown fox jumps over the lazy dog and keeps running");
            sweep(s, "soft-wrapped line", kPrompt);
        }},
        { "wrapped, edited back to one row", [](Session& s) {
            s.type("echo the quick brown fox jumps over the lazy dog and keeps running");
            s.report();
            s.backspace(40);
            sweep(s, "wrapped, edited back to one row", kPrompt);
        }},
        { "pasted multi-line", [](Session& s) {
            s.paste("for i in 1 2 3\ndo\n  echo $i\ndone");
            sweep(s, "pasted multi-line", kPrompt);
        }},
        { "multi-line, edited back to one line", [](Session& s) {
            s.paste("echo one\necho two");
            s.report();
            // Join the lines: back to the start of line two, then rub out the
            // newline before it. Arrows rather than Ctrl-A, which means the
            // start of the line in zsh and the start of the buffer in bash.
            for (int i = 0; i < 8; ++i) s.key("\x1b[D");
            s.backspace(1);
            sweep(s, "multi-line, edited back to one line", kPrompt);
        }},
        { "multi-line recalled from history", [](Session& s) {
            s.paste("for i in 1 2\ndo\n  echo item $i\ndone");
            s.report();
            s.enter();
            s.settle(200, 2000);
            s.up();
            sweep(s, "multi-line recalled from history", kPrompt);
        }},
        { "history multi-line, then single line", [](Session& s) {
            s.paste("for i in 1 2\ndo\n  echo item $i\ndone");
            s.report();
            s.enter();
            s.settle(200, 2000);
            s.type("echo after");
            sweep(s, "history multi-line, then single line", kPrompt);
        }},
        { "multi-line with wrapping lines", [](Session& s) {
            s.paste("echo the quick brown fox jumps over the lazy dog\necho short\necho another fairly long line that wraps around");
            sweep(s, "multi-line with wrapping lines", kPrompt);
        }},
        { "wide characters", [](Session& s) {
            s.type("echo 日本語のテキスト and 🎉 emoji");
            sweep(s, "wide characters", kPrompt);
        }},
        { "near the bottom, scrolling", [](Session& s) {
            for (int i = 0; i < 14; ++i) { s.type("echo line " + std::to_string(i)); s.enter(); }
            s.settle(200, 2000);
            s.paste("echo first line here\necho second line here\necho third line here");
            sweep(s, "near the bottom, scrolling", kPrompt);
        }},
        { "output without trailing newline", [](Session& s) {
            s.type("printf 'no newline'");
            s.enter();
            s.settle(200, 2000);
            s.type("echo next command");
            sweep(s, "output without trailing newline", kPrompt);
        }},
        { "after clear screen", [](Session& s) {
            s.type("echo some text before clearing");
            s.report();
            s.ctrl('l');
            s.settle(150, 1500);
            sweep(s, "after clear screen", kPrompt);
        }},
        { "clear screen with the prompt lower down", [](Session& s) {
            for (int i = 0; i < 4; ++i) { s.type("echo line " + std::to_string(i)); s.enter(); }
            s.settle(200, 2000);
            s.paste("echo first line\necho second line that is long enough to wrap");
            s.report();
            s.ctrl('l');
            s.settle(150, 1500);
            sweep(s, "clear screen with the prompt lower down", kPrompt);
        }},
        { "resized narrower mid-edit", [](Session& s) {
            s.type("echo the quick brown fox jumps over the lazy dog and keeps running");
            s.report();
            s.resize(30, 16);
            sweep(s, "resized narrower mid-edit", kPrompt);
        }},
        { "resized wider mid-edit", [](Session& s) {
            s.paste("echo the quick brown fox jumps over the lazy dog\necho second line");
            s.report();
            s.resize(64, 16);
            sweep(s, "resized wider mid-edit", kPrompt);
        }},
        { "dragged wider then narrower", [](Session& s) {
            std::vector<std::string> outs;
            for (char c = 'A'; c <= 'D'; ++c) {
                s.type(std::string("printf 'out-%s\\n' ") + c);
                s.enter();
                outs.push_back(std::string("out-") + c);
            }
            s.settle(200, 2000);
            s.type("echo the quick brown fox jumps over the lazy dog and keeps running far away");
            s.report();
            s.resize_drag({ 42, 45, 48, 52, 56, 60 }, 16);
            survives_resizes(s, "dragged wider then narrower (wider)", outs);
            s.resize_drag({ 55, 50, 44, 38, 33, 30 }, 16);
            survives_resizes(s, "dragged wider then narrower (narrower)", outs);
            s.resize_drag({ 36, 44, 52, 60, 52, 44, 40 }, 16);
            survives_resizes(s, "dragged wider then narrower (back and forth)", outs);
            sweep(s, "dragged wider then narrower", kPrompt);
        }},
        { "resized after editing a multi-line command", [](Session& s) {
            std::vector<std::string> outs;
            for (char c = 'A'; c <= 'C'; ++c) {
                s.type(std::string("printf 'out-%s\\n' ") + c);
                s.enter();
                outs.push_back(std::string("out-") + c);
            }
            s.settle(200, 2000);
            s.paste("echo the quick brown fox jumps over the lazy dog\necho second line\necho third line that is also fairly long");
            Report rep = s.report();
            Layout L = lay_out(s, rep, kPrompt);
            s.click(L.start_row + 1, 8);
            s.report();
            s.type(" edited");
            s.report();
            s.resize_drag({ 46, 52, 60 }, 16);
            s.resize_drag({ 50, 40, 32 }, 16);
            s.resize_drag({ 40 }, 16);
            survives_resizes(s, "resized after editing a multi-line command", outs);
            sweep(s, "resized after editing a multi-line command", kPrompt);
        }},
        { "resized at the bottom of the screen", [](Session& s) {
            std::vector<std::string> outs;
            for (char c = 'A'; c <= 'F'; ++c) {
                s.type(std::string("printf 'out-%s\\n' ") + c);
                s.enter();
                outs.push_back(std::string("out-") + c);
            }
            s.settle(200, 2000);
            s.paste("echo the quick brown fox jumps over the lazy dog and keeps running\necho second line");
            s.report();
            // Narrower and shorter, then wider and taller, then back.
            s.resize_drag({ { 36, 15 }, { 32, 14 }, { 28, 12 } });
            survives_resizes(s, "resized at the bottom of the screen (smaller)", outs);
            s.resize_drag({ { 36, 14 }, { 48, 16 }, { 60, 18 } });
            survives_resizes(s, "resized at the bottom of the screen (larger)", outs);
            s.resize_drag({ { 50, 16 }, { 40, 16 } });
            survives_resizes(s, "resized at the bottom of the screen", outs);
            sweep(s, "resized at the bottom of the screen", kPrompt);
        }},
        { "click, type, click again", [](Session& s) {
            s.type("echo the quick brown fox jumps over the lazy dog and keeps running");
            Report rep = s.report();
            Layout L = lay_out(s, rep, kPrompt);
            s.click(L.start_row, 15);
            s.report();
            s.type("INSERTED ");
            s.report();
            s.click(L.start_row + 1, 3);
            s.report();
            s.backspace(2);
            sweep(s, "click, type, click again", kPrompt);
        }},
        { "edit inside a multi-line command", [](Session& s) {
            s.paste("for i in 1 2 3\ndo\n  echo $i\ndone");
            Report rep = s.report();
            Layout L = lay_out(s, rep, kPrompt);
            s.click(L.start_row + 2, 10);
            s.report();
            s.type(" and some more words to make this line wrap around");
            sweep(s, "edit inside a multi-line command", kPrompt);
        }},
        { "continuation prompt", [](Session& s) {
            s.type("echo \"first line");
            s.enter();
            s.settle(150, 1500);
            s.type("second line of the same command");
            sweep(s, "continuation prompt", s.is_bash() ? "> " : "dquote> ");
        }},
        { "after a full-screen app quit with a key", [](Session& s) {
            // What less, man or htop do: switch to the alternate screen, take
            // keys, and leave on a single keypress -- no Enter to mark the end.
            s.type(s.is_bash()
                ? "printf '\\e[?1049h'; read -rsn1 k; printf '\\e[?1049l'"
                : "printf '\\e[?1049h'; read -rsk1 k; printf '\\e[?1049l'");
            s.enter();
            s.settle(200, 2000);
            s.key("q");
            s.settle(200, 2000);
            s.type("echo after the app");
            sweep(s, "after a full-screen app quit with a key", kPrompt);
        }},
        { "ctrl-d deleting mid-line", [](Session& s) {
            s.type("echo the quick brown fox jumps over the lazy dog");
            Report rep = s.report();
            Layout L = lay_out(s, rep, kPrompt);
            s.click(L.start_row, 12);
            s.report();
            s.ctrl('d');                 // delete-char, not end of input
            s.settle(100, 1000);
            s.type("X");
            sweep(s, "ctrl-d deleting mid-line", kPrompt);
        }},
        { "right prompt with a multi-line command", [](Session& s) {
            // Set per scenario so the other scenarios keep a bare prompt.
            s.type(s.is_bash() ? "true" : "RPROMPT='[right]'");
            s.enter();
            s.settle(200, 2000);
            s.paste("for i in 1 2 3\ndo\n  echo $i\ndone");
            sweep(s, "right prompt with a multi-line command", kPrompt);
        }},
        { "delete selection, one row", [](Session& s) {
            s.type("echo alpha beta gamma");
            s.report();
            Layout L = lay_out(s, s.report(), kPrompt);
            int r = L.start_row;
            delete_check(s, "delete selection, one row", kPrompt, r, 11, r, 15);
        }},
        { "delete selection across a newline", [](Session& s) {
            s.paste("echo one\necho two");
            Report rep = s.report();
            Layout L = lay_out(s, rep, kPrompt);
            int r = L.start_row;
            // "one" through the "e" of the second "echo".
            delete_check(s, "delete selection across a newline", kPrompt, r, 10, r + 1, 0);
        }},
    };
}

} // namespace

int main(int argc, char** argv) {
    std::string only, shell = "zsh";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--verbose") g_verbose = true;
        else if (a == "--only" && i + 1 < argc) only = argv[++i];
        else if (a == "--shell" && i + 1 < argc) shell = argv[++i];
    }
    if (shell == "bash" && access("/opt/homebrew/bin/bash", X_OK) != 0) {
        std::printf("bash 4+ not found at /opt/homebrew/bin/bash; skipping\n");
        return 0;
    }

    std::printf("prompt editing, %s\n", shell.c_str());
    for (const Named& sc : scenarios()) {
        if (!only.empty() && only != sc.name) continue;
        // A fresh shell per scenario, so one can't leave state for the next.
        Session s(shell, 40, 16);
        sc.run(s);
    }

    // The same clicks with the shell in application cursor-key mode, which
    // oh-my-zsh and others switch on while the line editor is active. The
    // arrows sink sends have to follow the mode, as a real keypress does.
    if (shell == "zsh" && (only.empty() || only == "application cursor keys")) {
        const std::string smkx =
            "zle-line-init() { echoti smkx }\n"
            "zle-line-finish() { echoti rmkx }\n"
            "zle -N zle-line-init\n"
            "zle -N zle-line-finish\n";
        Session s(shell, 40, 16, smkx);
        s.paste("for i in 1 2 3\ndo\n  echo 日本 $i\ndone");
        sweep(s, "application cursor keys", kPrompt);
        if (!s.grid().is_app_cursor_keys()) {
            std::printf("  [application cursor keys] shell never enabled DECCKM; test proves nothing\n");
            ++g_totals.broken;
        }
    }
    std::printf("%d clicks in %d scenarios, %d wrong, %d scenarios could not be laid out\n",
                g_totals.clicks, g_totals.scenarios, g_totals.failed, g_totals.broken);
    if (g_sync_timeouts) {
        std::printf("(the screen lagged the shell by over 2s %d times)\n", g_sync_timeouts);
    }
    return (g_totals.failed == 0 && g_totals.broken == 0) ? 0 : 1;
}
