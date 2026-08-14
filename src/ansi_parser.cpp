#include "ansi_parser.hpp"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <limits>

ANSIParser::ANSIParser() {}

ANSIParser::~ANSIParser() {}

// Parses a CSI numeric parameter without throwing on overflow or malformed input
// (terminal input is attacker-controlled, e.g. `cat`'d files or remote shell output).
static int parse_csi_param(const std::string& buffer) {
    if (buffer.empty()) return 0;
    try {
        return std::stoi(buffer);
    } catch (const std::out_of_range&) {
        return std::numeric_limits<int>::max();
    } catch (const std::invalid_argument&) {
        return 0;
    }
}

// Standard ANSI 16-color palette (indices 0-15 of the xterm-256 palette)
static const SDL_FColor ansi_colors[16] = {
    {0.05f, 0.05f, 0.05f, 1.0f},     // 0: Black
    {0.85f, 0.15f, 0.15f, 1.0f},     // 1: Red
    {0.15f, 0.85f, 0.15f, 1.0f},     // 2: Green
    {0.85f, 0.75f, 0.15f, 1.0f},     // 3: Yellow
    {0.15f, 0.15f, 0.85f, 1.0f},     // 4: Blue
    {0.85f, 0.15f, 0.85f, 1.0f},     // 5: Magenta
    {0.15f, 0.85f, 0.85f, 1.0f},     // 6: Cyan
    {0.85f, 0.85f, 0.85f, 1.0f},     // 7: White
    {0.30f, 0.30f, 0.30f, 1.0f},     // 8: Bright Black (Grey)
    {1.00f, 0.30f, 0.30f, 1.0f},     // 9: Bright Red
    {0.30f, 1.00f, 0.30f, 1.0f},     // 10: Bright Green
    {1.00f, 1.00f, 0.30f, 1.0f},     // 11: Bright Yellow
    {0.30f, 0.30f, 1.00f, 1.0f},     // 12: Bright Blue
    {1.00f, 0.30f, 1.00f, 1.0f},     // 13: Bright Magenta
    {0.30f, 1.00f, 1.00f, 1.0f},     // 14: Bright Cyan
    {1.00f, 1.00f, 1.00f, 1.0f}      // 15: Bright White
};

// xterm 256-color palette lookup: 0-15 named colors, 16-231 a 6x6x6 RGB
// cube (levels 0,95,135,175,215,255), 232-255 a 24-step grayscale ramp.
static SDL_FColor xterm_256_color(int idx) {
    idx = std::clamp(idx, 0, 255);
    if (idx < 16) {
        return ansi_colors[idx];
    }
    if (idx < 232) {
        int n = idx - 16;
        int levels[3] = { n / 36, (n / 6) % 6, n % 6 };
        float rgb[3];
        for (int i = 0; i < 3; ++i) {
            rgb[i] = (levels[i] == 0 ? 0 : levels[i] * 40 + 55) / 255.0f;
        }
        return { rgb[0], rgb[1], rgb[2], 1.0f };
    }
    float v = (8 + 10 * (idx - 232)) / 255.0f;
    return { v, v, v, 1.0f };
}

void ANSIParser::reset_csi() {
    csi_params_.clear();
    csi_buffer_.clear();
    is_private_mode_ = false;
}

void ANSIParser::parse(TerminalGrid& grid, const char* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        uint8_t byte = static_cast<uint8_t>(data[i]);
        
        // Decode multi-byte UTF-8 byte streams
        if (utf8_bytes_needed_ == 0) {
            if (byte < 0x80) {
                // Standard 1-byte ASCII character
                process_char(grid, static_cast<char32_t>(byte));
            } else if ((byte & 0xE0) == 0xC0) {
                // 2-byte sequence starting byte
                utf8_codepoint_ = byte & 0x1F;
                utf8_bytes_needed_ = 1;
            } else if ((byte & 0xF0) == 0xE0) {
                // 3-byte sequence starting byte
                utf8_codepoint_ = byte & 0x0F;
                utf8_bytes_needed_ = 2;
            } else if ((byte & 0xF8) == 0xF0) {
                // 4-byte sequence starting byte
                utf8_codepoint_ = byte & 0x07;
                utf8_bytes_needed_ = 3;
            } else {
                // Invalid start byte, treat as replacement/raw character
                process_char(grid, static_cast<char32_t>(byte));
            }
        } else {
            if ((byte & 0xC0) == 0x80) {
                // Continuation byte
                utf8_codepoint_ = (utf8_codepoint_ << 6) | (byte & 0x3F);
                utf8_bytes_needed_--;
                
                if (utf8_bytes_needed_ == 0) {
                    process_char(grid, utf8_codepoint_);
                }
            } else {
                // Invalid continuation byte, abort sequence and process character raw
                utf8_bytes_needed_ = 0;
                process_char(grid, static_cast<char32_t>(byte));
            }
        }
    }
}

void ANSIParser::process_char(TerminalGrid& grid, char32_t c) {
    switch (state_) {
        case STATE_NORMAL: {
            if (c == '\x1b') {
                state_ = STATE_ESCAPE;
                reset_csi();
            } else if (c == '\n' || c == '\v' || c == '\f') {
                // Line feed: move cursor down, scrolling at the bottom margin
                grid.index();
            } else if (c == '\r') {
                // Carriage return: move cursor to start of line
                grid.set_cursor_col(0);
                grid.set_prompt_boundary(-1);
            } else if (c == '\b') {
                // Backspace: move cursor left one cell
                grid.set_cursor_col(grid.get_cursor_col() - 1);
            } else if (c == '\x07') {
                // Bell: Ignore audio alerts
            } else if (c == 0x0E || c == 0x0F) {
                // SO/SI: shift between G0/G1. G1 isn't tracked (ncurses on
                // xterm-likes designates G0 directly), so just consume them.
            } else if (c == '\t') {
                // Tab: Move to next tab stop (multiples of 8)
                int next_tab = (grid.get_cursor_col() + 8) & ~7;
                grid.set_cursor_col(next_tab);
            } else {
                // Standard printable character
                // Ignore Unicode Variation Selectors and Zero-Width characters
                if (!((c >= 0xFE00 && c <= 0xFE0F) || 
                      (c >= 0xE0100 && c <= 0xE01EF) ||
                      (c >= 0x200B && c <= 0x200D) ||
                      c == 0x2060 ||
                      c == 0xFEFF)) {
                    char32_t out = c;
                    if (g0_dec_graphics_ && c >= 0x60 && c <= 0x7E) {
                        // DEC Special Graphics: 0x60-0x7E become line-drawing
                        // and symbol glyphs while ESC ( 0 is in effect.
                        static const char32_t dec_graphics[31] = {
                            0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A,
                            0x00B0, 0x00B1, 0x2424, 0x240B, 0x2518, 0x2510,
                            0x250C, 0x2514, 0x253C, 0x23BA, 0x23BB, 0x2500,
                            0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, 0x252C,
                            0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3,
                            0x00B7
                        };
                        out = dec_graphics[c - 0x60];
                    }
                    grid.write_character(out);

                    if (c >= 32 && c < 127) {
                        trigger_buffer_ += std::tolower(static_cast<char>(c));
                        if (trigger_buffer_.length() > 32) {
                            trigger_buffer_ = trigger_buffer_.substr(trigger_buffer_.length() - 32);
                        }
                        
                        if (trigger_buffer_.find("error") != std::string::npos ||
                            trigger_buffer_.find("failed") != std::string::npos) {
                            grid.trigger_error_flash();
                            trigger_buffer_.clear();
                        }
                    }
                }
            }
            break;
        }
        case STATE_ESCAPE: {
            if (c == '[') {
                state_ = STATE_CSI;
            } else if (c == ']' || c == 'P' || c == '_' || c == '^' || c == 'X') {
                // OSC / DCS / APC / PM / SOS: a string payload terminated by
                // BEL or ST (ESC \). OSC payloads are accumulated and acted
                // on (titles, prompt marks); the rest are consumed so their
                // payload isn't printed to the screen as literal text.
                str_is_osc_ = (c == ']');
                osc_buffer_.clear();
                state_ = STATE_STR;
            } else if (c == '7') { // DECSC: Save Cursor
                grid.save_cursor();
                state_ = STATE_NORMAL;
            } else if (c == '8') { // DECRC: Restore Cursor
                grid.restore_cursor();
                state_ = STATE_NORMAL;
            } else if (c == 'D') { // IND: Index (down one, scroll at margin)
                grid.index();
                state_ = STATE_NORMAL;
            } else if (c == 'E') { // NEL: Next Line (index + carriage return)
                grid.index();
                grid.set_cursor_col(0);
                state_ = STATE_NORMAL;
            } else if (c == 'M') { // RI: Reverse Index (up one, scroll at margin)
                grid.reverse_index();
                state_ = STATE_NORMAL;
            } else if (c == '(' || c == ')' || c == '*' || c == '+') {
                // SCS: designate character set for G0-G3; the final byte
                // (e.g. 'B' = US-ASCII, '0' = DEC line drawing) follows.
                charset_designator_ = static_cast<char>(c);
                state_ = STATE_CHARSET;
            } else {
                state_ = STATE_NORMAL;
            }
            break;
        }
        case STATE_STR: {
            if (c == 0x07 || c == 0x9C) { // BEL, or single-byte ST (C1)
                if (str_is_osc_) dispatch_osc(grid);
                state_ = STATE_NORMAL;
            } else if (c == 0x1b) { // possible start of two-byte ST (ESC \)
                state_ = STATE_STR_ESC;
            } else if (str_is_osc_ && osc_buffer_.size() < kOscMaxLen) {
                // Payload is re-encoded as UTF-8 (titles can be non-ASCII)
                if (c < 0x80) {
                    osc_buffer_ += static_cast<char>(c);
                } else if (c < 0x800) {
                    osc_buffer_ += static_cast<char>(0xC0 | (c >> 6));
                    osc_buffer_ += static_cast<char>(0x80 | (c & 0x3F));
                } else if (c < 0x10000) {
                    osc_buffer_ += static_cast<char>(0xE0 | (c >> 12));
                    osc_buffer_ += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                    osc_buffer_ += static_cast<char>(0x80 | (c & 0x3F));
                } else {
                    osc_buffer_ += static_cast<char>(0xF0 | (c >> 18));
                    osc_buffer_ += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
                    osc_buffer_ += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                    osc_buffer_ += static_cast<char>(0x80 | (c & 0x3F));
                }
            }
            break;
        }
        case STATE_STR_ESC: {
            if (c == '\\') {
                if (str_is_osc_) dispatch_osc(grid);
                state_ = STATE_NORMAL; // ST: sequence complete
            } else {
                // Not a valid ST -- the string was implicitly aborted by a new
                // escape sequence starting. Re-dispatch this byte as if we'd
                // just seen ESC.
                state_ = STATE_ESCAPE;
                process_char(grid, c);
            }
            break;
        }
        case STATE_CHARSET: {
            if (charset_designator_ == '(') {
                g0_dec_graphics_ = (c == '0');
            }
            state_ = STATE_NORMAL;
            break;
        }
        case STATE_CSI: {
            if (c == '?') {
                is_private_mode_ = true;
            } else if (c >= '0' && c <= '9') {
                csi_buffer_ += static_cast<char>(c);
            } else if (c == ';' || c == ':') {
                // ':' is the ITU subparameter separator (e.g. SGR 38:5:196).
                // Treating it like ';' keeps the digits from concatenating
                // into a single garbage parameter; the colon-form extended
                // color sequences then parse identically to the ';' form.
                csi_params_.push_back(parse_csi_param(csi_buffer_));
                csi_buffer_.clear();
            } else if (c >= 0x40 && c <= 0x7E) {
                if (!csi_buffer_.empty()) {
                    csi_params_.push_back(parse_csi_param(csi_buffer_));
                }
                process_csi_sequence(grid, static_cast<char>(c));
                state_ = STATE_NORMAL;
            }
            break;
        }
    }
}

void ANSIParser::dispatch_osc(TerminalGrid& grid) {
    // Payload shape: "Ps;Pt" -- numeric selector, then text
    size_t semi = osc_buffer_.find(';');
    if (semi == std::string::npos) {
        osc_buffer_.clear();
        return;
    }
    int ps = 0;
    try {
        ps = std::stoi(osc_buffer_.substr(0, semi));
    } catch (...) {
        osc_buffer_.clear();
        return;
    }
    std::string pt = osc_buffer_.substr(semi + 1);
    osc_buffer_.clear();

    switch (ps) {
        case 0: // set icon name + window title
        case 2: // set window title
            grid.set_window_title(pt);
            break;
        case 8: {
            // Hyperlink: OSC 8;params;URI ST. `pt` is "params;URI" -- params
            // (e.g. id=xxx, used to group multiple spans as one link) are
            // parsed by real terminals for hover-highlighting a link's other
            // spans; sink doesn't do that yet, so they're just skipped past.
            // OSC 8;;ST (empty URI) closes the link.
            size_t inner_semi = pt.find(';');
            std::string uri = (inner_semi != std::string::npos) ? pt.substr(inner_semi + 1) : "";
            grid.set_current_hyperlink(uri);
            break;
        }
        case 133:
            // Shell integration prompt marks (FinalTerm/iTerm2 protocol).
            // 'A' = prompt start -- the anchor Cmd+Up/Down jump between.
            // B/C/D (command start/output start/command end) are accepted
            // but unused for now.
            if (!pt.empty() && pt[0] == 'A') {
                grid.mark_prompt_row();
            }
            break;
        default:
            break;
    }
}

void ANSIParser::process_csi_sequence(TerminalGrid& grid, char command) {
    auto get_param = [&](size_t index, int default_val) {
        if (index < csi_params_.size()) {
            return csi_params_[index];
        }
        return default_val;
    };

    // For count/position parameters (cursor motion, CHA/CUP, DCH), xterm/VT100
    // convention defaults Ps to 1 and also treats an explicitly-sent 0 (e.g.
    // "CSI 0 A") the same as if it were omitted -- unlike ED/EL/SM mode
    // parameters, where 0 is itself a meaningful, distinct value.
    auto get_count_param = [&](size_t index, int default_val) {
        int v = get_param(index, default_val);
        return v == 0 ? default_val : v;
    };

    switch (command) {
        case 'm': { // Select Graphic Rendition (SGR)
            uint8_t attrs = grid.get_current_attrs();
            // Applies bold-as-bright: a base-palette (30-37) foreground gets
            // its bright variant while bold is on, regardless of whether the
            // color or the bold arrived first in the parameter list.
            auto apply_base_fg = [&]() {
                if (fg_base_index_ >= 0) {
                    int idx = fg_base_index_ + ((attrs & ATTR_BOLD) ? 8 : 0);
                    grid.set_current_fg(ansi_colors[idx]);
                }
            };
            auto reset_all = [&]() {
                grid.set_current_fg({0.9f, 0.9f, 0.9f, 1.0f});
                grid.set_current_bg({0.0f, 0.0f, 0.0f, 0.0f});
                attrs = 0;
                fg_base_index_ = -1;
            };

            if (csi_params_.empty()) {
                reset_all();
                grid.set_current_attrs(attrs);
                break;
            }

            for (size_t i = 0; i < csi_params_.size(); ++i) {
                int param = csi_params_[i];
                if (param == 0) {
                    reset_all();
                } else if (param == 1) {
                    attrs |= ATTR_BOLD;
                    apply_base_fg();
                } else if (param == 2) {
                    attrs |= ATTR_DIM;
                } else if (param == 3) {
                    attrs |= ATTR_ITALIC;
                } else if (param == 4 || param == 21) { // 21: double underline
                    attrs |= ATTR_UNDERLINE;
                } else if (param == 7) {
                    attrs |= ATTR_REVERSE;
                } else if (param == 9) {
                    attrs |= ATTR_STRIKETHROUGH;
                } else if (param == 22) { // normal intensity
                    attrs &= ~(ATTR_BOLD | ATTR_DIM);
                    apply_base_fg();
                } else if (param == 23) {
                    attrs &= ~ATTR_ITALIC;
                } else if (param == 24) {
                    attrs &= ~ATTR_UNDERLINE;
                } else if (param == 27) {
                    attrs &= ~ATTR_REVERSE;
                } else if (param == 29) {
                    attrs &= ~ATTR_STRIKETHROUGH;
                } else if (param >= 30 && param <= 37) {
                    fg_base_index_ = param - 30;
                    apply_base_fg();
                } else if (param >= 40 && param <= 47) {
                    grid.set_current_bg(ansi_colors[param - 40]);
                } else if (param >= 90 && param <= 97) {
                    // Explicit bright: not subject to bold re-brightening
                    fg_base_index_ = -1;
                    grid.set_current_fg(ansi_colors[param - 90 + 8]);
                } else if (param >= 100 && param <= 107) {
                    grid.set_current_bg(ansi_colors[param - 100 + 8]);
                } else if (param == 38 || param == 48) {
                    if (param == 38) fg_base_index_ = -1;
                    // Extended color: 38/48;2;R;G;B (24-bit truecolor) or
                    // 38/48;5;N (256-color indexed palette)
                    bool is_fg = (param == 38);
                    if (i + 4 < csi_params_.size() && csi_params_[i + 1] == 2) {
                        float r = std::clamp(csi_params_[i + 2], 0, 255) / 255.0f;
                        float g = std::clamp(csi_params_[i + 3], 0, 255) / 255.0f;
                        float b = std::clamp(csi_params_[i + 4], 0, 255) / 255.0f;
                        if (is_fg) grid.set_current_fg({r, g, b, 1.0f});
                        else       grid.set_current_bg({r, g, b, 1.0f});
                        i += 4;
                    } else if (i + 2 < csi_params_.size() && csi_params_[i + 1] == 5) {
                        SDL_FColor color = xterm_256_color(csi_params_[i + 2]);
                        if (is_fg) grid.set_current_fg(color);
                        else       grid.set_current_bg(color);
                        i += 2;
                    }
                } else if (param == 39) {
                    // Default foreground color
                    fg_base_index_ = -1;
                    grid.set_current_fg({0.9f, 0.9f, 0.9f, 1.0f});
                } else if (param == 49) {
                    // Default background color
                    grid.set_current_bg({0.0f, 0.0f, 0.0f, 0.0f});
                }
            }
            grid.set_current_attrs(attrs);
            break;
        }
        case 'G': { // Cursor Horizontal Absolute (CHA)
            int col = get_count_param(0, 1) - 1;
            grid.set_cursor_col(col);
            break;
        }
        case 'd': { // Vertical Position Absolute (VPA): row only, column unchanged
            int row = get_count_param(0, 1) - 1;
            grid.set_cursor_row(row);
            break;
        }
        case 'H':
        case 'f': { // Cursor Position (CUP)
            int row = get_count_param(0, 1) - 1;
            int col = get_count_param(1, 1) - 1;
            grid.set_cursor_row(row);
            grid.set_cursor_col(col);
            break;
        }
        case 'A': { // Cursor Up (CUU)
            int offset = get_count_param(0, 1);
            grid.set_cursor_row(grid.get_cursor_row() - offset);
            break;
        }
        case 'B': { // Cursor Down (CUD)
            int offset = get_count_param(0, 1);
            grid.set_cursor_row(grid.get_cursor_row() + offset);
            break;
        }
        case 'C': { // Cursor Forward (CUF)
            int offset = get_count_param(0, 1);
            grid.set_cursor_col(grid.get_cursor_col() + offset);
            break;
        }
        case 'D': { // Cursor Backward (CUB)
            int offset = get_count_param(0, 1);
            grid.set_cursor_col(grid.get_cursor_col() - offset);
            break;
        }
        case 'r': { // Set Scrolling Region (DECSTBM)
            int top = get_count_param(0, 1) - 1;
            int bottom = get_param(1, grid.get_rows()) - 1;
            grid.set_scroll_region(top, bottom);
            // DECSTBM homes the cursor
            grid.set_cursor_row(0);
            grid.set_cursor_col(0);
            break;
        }
        case 'L': { // Insert Lines (IL)
            grid.insert_lines(get_count_param(0, 1));
            break;
        }
        case 'M': { // Delete Lines (DL)
            grid.delete_lines(get_count_param(0, 1));
            break;
        }
        case 'S': { // Scroll Up (SU)
            grid.scroll_region_up(get_count_param(0, 1));
            break;
        }
        case 'T': { // Scroll Down (SD)
            grid.scroll_region_down(get_count_param(0, 1));
            break;
        }
        case 'P': { // Delete Character (DCH)
            int count = get_count_param(0, 1);
            grid.delete_character(count);
            break;
        }
        case 'X': { // Erase Character (ECH)
            int count = get_count_param(0, 1);
            grid.erase_characters(count);
            break;
        }
        case 'J': { // Erase in Display (ED)
            int mode = get_param(0, 0);
            if (mode == 2) {
                grid.clear_screen();
            } else if (mode == 3) {
                grid.clear_screen();
                grid.clear_scrollback();
            }
            break;
        }
        case 'K': { // Erase in Line (EL)
            int mode = get_param(0, 0);
            grid.clear_line(grid.get_cursor_row(), mode);
            break;
        }
        case 'h':   // Set Mode (SM / DECSET)
        case 'l': { // Reset Mode (RM / DECRST)
            if (!is_private_mode_) break;
            bool set = (command == 'h');
            // Apps commonly gang modes into one sequence (CSI ?1002;1006h),
            // so every parameter gets applied, not just the first.
            for (int mode : csi_params_) {
                switch (mode) {
                    case 1:    grid.set_app_cursor_keys(set); break; // DECCKM
                    case 25:   grid.set_cursor_visible(set); break;  // DECTCEM
                    case 1049: // save cursor + switch to alt screen (and back)
                    case 47:   // older switch-only variants, same handling:
                    case 1047: // set_alt_screen snapshots/restores the cursor
                               // itself, deliberately not via DECSC state --
                               // apps ED-clear right after switching, which
                               // resets the DECSC slot and would restore 0,0
                        grid.set_alt_screen(set);
                        break;
                    case 1048: // DECSC/DECRC dressed up as a mode
                        if (set) grid.save_cursor();
                        else     grid.restore_cursor();
                        break;
                    case 2004: grid.set_bracketed_paste(set); break;
                    case 9:      // X10 press-only reporting
                    case 1000:   // press + release
                    case 1002:   // press + release + drag motion
                    case 1003:   // any motion
                        grid.set_mouse_mode(set ? mode : 0);
                        break;
                    case 1006: grid.set_mouse_sgr(set); break; // SGR encoding
                    case 1007: grid.set_alternate_scroll(set); break;
                    default: break;
                }
            }
            break;
        }
        case 's': { // Save Cursor (ANSI.SYS)
            // CSI ? Ps s is XTSAVE (save private mode values), which ncurses
            // emits on every mouse enable -- it must not clobber the cursor
            if (!is_private_mode_) grid.save_cursor();
            break;
        }
        case 'u': { // Restore Cursor (ANSI.SYS)
            if (!is_private_mode_) grid.restore_cursor(); // CSI ? Ps u = XTRESTORE
            break;
        }
        default:
            break;
    }
}
