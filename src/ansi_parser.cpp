#include "ansi_parser.hpp"

#include <array>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <limits>

// Dispatch table for STATE_NORMAL.
//
// The if-chain this replaces compared a byte against eight control characters
// in sequence, then against five Unicode ranges, before writing it -- roughly
// fifteen branches for the most common byte in any terminal stream. One
// indexed load plus a jump table reaches the same place directly.
//
// Everything without a special meaning maps to NA_PRINT, *including* the
// control characters this parser doesn't handle (NUL, SUB, DEL and friends).
// That is deliberate: they previously fell through the whole if-chain into the
// printable branch and were written to the grid, so mapping them to NA_PRINT
// preserves that behaviour exactly rather than quietly starting to drop them.
enum NormalAction : uint8_t {
    NA_PRINT = 0,
    NA_ESC,
    NA_LF,
    NA_CR,
    NA_BS,
    NA_TAB,
    NA_IGNORE,   // BEL, SO, SI: consumed with no effect
};

static constexpr std::array<uint8_t, 128> make_normal_table() {
    std::array<uint8_t, 128> t{};   // NA_PRINT (0) everywhere by default
    t[0x07] = NA_IGNORE;            // BEL
    t[0x08] = NA_BS;
    t[0x09] = NA_TAB;
    t[0x0A] = NA_LF;                // LF
    t[0x0B] = NA_LF;                // VT
    t[0x0C] = NA_LF;                // FF
    t[0x0D] = NA_CR;
    t[0x0E] = NA_IGNORE;            // SO
    t[0x0F] = NA_IGNORE;            // SI
    t[0x1B] = NA_ESC;
    return t;
}
static constexpr std::array<uint8_t, 128> kNormalAction = make_normal_table();

ANSIParser::ANSIParser() {}

ANSIParser::~ANSIParser() {}

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
    csi_acc_ = 0;
    csi_acc_digits_ = false;
    is_private_mode_ = false;
}

void ANSIParser::parse(TerminalGrid& grid, const char* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        uint8_t byte = static_cast<uint8_t>(data[i]);

        // Fast path: a run of plain printable ASCII in the normal state.
        //
        // The per-character path recomputes the row pointer (ring index plus a
        // multiply) and reloads fg/bg/attrs/hyperlink off the grid for every
        // character, all of which are invariant across a run of ordinary text.
        // Handing the whole span to write_run() does that work once, and skips
        // the UTF-8 check, the state switch and the STATE_NORMAL table lookup
        // for every character after the first.
        //
        // Gated on the byte being printable ASCII first, so streams that are
        // mostly escape sequences pay a single comparison to skip all of this.
        // DEC graphics mode is excluded because it translates 0x60-0x7E into
        // line-drawing glyphs, and a pending wrap is excluded so the deferred
        // wrap stays in write_character() alone.
        if (byte >= 0x20 && byte < 0x7F &&
            state_ == STATE_NORMAL &&
            utf8_bytes_needed_ == 0 &&
            !g0_dec_graphics_ &&
            !grid.is_wrap_pending()) {
            size_t j = i + 1;
            while (j < size) {
                uint8_t b = static_cast<uint8_t>(data[j]);
                if (b < 0x20 || b >= 0x7F) break;
                ++j;
            }
            int wrote = grid.write_run(data + i, static_cast<int>(j - i));
            if (wrote > 0) {
                for (int k = 0; k < wrote; ++k) {
                    note_trigger_char(grid, static_cast<unsigned char>(data[i + k]));
                }
                i += static_cast<size_t>(wrote) - 1; // the loop's ++i consumes the last
                continue;
            }
            // write_run declined (degenerate grid); fall through to the
            // per-character path, which handles that case as before.
        }

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

// See the declaration. Kept out of line so both the per-character path and
// the batched run path call the identical code.
void ANSIParser::note_trigger_char(TerminalGrid& grid, char32_t c) {
    // Not std::tolower: it is locale-aware, so it stays a real libsystem_c
    // call per character. Callers restrict c to printable ASCII.
    char lc = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32)
                                     : static_cast<char>(c);
    trigger_ring_[trigger_pos_] = lc;
    trigger_ring_[trigger_pos_ + kTrigWindow] = lc;
    // One past the most recent character, in the contiguous copy.
    const char* end = trigger_ring_ + trigger_pos_ + kTrigWindow + 1;
    trigger_pos_ = (trigger_pos_ + 1) & (kTrigWindow - 1);

    // Gate on the last letter first: only 'r' can finish "error" and only 'd'
    // can finish "failed", so almost every character costs a single
    // comparison. The leading zeros the ring starts with cannot match a
    // letter, so no "enough characters yet" counter is needed.
    if ((lc == 'r' && std::memcmp(end - 5, "error", 5) == 0) ||
        (lc == 'd' && std::memcmp(end - 6, "failed", 6) == 0)) {
        grid.trigger_error_flash();
        std::memset(trigger_ring_, 0, sizeof(trigger_ring_));
        trigger_pos_ = 0;
    }
}

void ANSIParser::process_char(TerminalGrid& grid, char32_t c) {
    switch (state_) {
        case STATE_NORMAL: {
            // ASCII dispatches through the table. Anything above 0x7F is text
            // as far as this state is concerned -- every control character it
            // recognises is single-byte ASCII -- so it skips straight to the
            // write path below.
            if (c < 128) {
                switch (kNormalAction[c]) {
                    case NA_ESC:
                        state_ = STATE_ESCAPE;
                        reset_csi();
                        return;
                    case NA_LF:
                        // Line feed: move cursor down, scrolling at the bottom margin
                        grid.index();
                        return;
                    case NA_CR:
                        // Carriage return: move cursor to start of line
                        grid.set_cursor_col(0);
                        grid.set_prompt_boundary(-1);
                        return;
                    case NA_BS:
                        // Backspace: move cursor left one cell
                        grid.set_cursor_col(grid.get_cursor_col() - 1);
                        return;
                    case NA_TAB: {
                        // Tab: Move to next tab stop (multiples of 8)
                        int next_tab = (grid.get_cursor_col() + 8) & ~7;
                        grid.set_cursor_col(next_tab);
                        return;
                    }
                    case NA_IGNORE:
                        // BEL (no audio alerts), and SO/SI: G1 isn't tracked
                        // (ncurses on xterm-likes designates G0 directly), so
                        // both are simply consumed.
                        return;
                    default:
                        break;   // NA_PRINT: fall through to the write path
                }
            } else if (c >= 0x200B) {
                // Variation selectors and zero-width characters occupy no cell.
                // All of them sit above 0x200B, so ASCII never reaches this
                // test -- which is why it is guarded rather than applied to
                // every character as it was before.
                if ((c >= 0xFE00 && c <= 0xFE0F) ||
                    (c >= 0xE0100 && c <= 0xE01EF) ||
                    (c >= 0x200B && c <= 0x200D) ||
                    c == 0x2060 ||
                    c == 0xFEFF) {
                    return;
                }
            }

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
                note_trigger_char(grid, c);
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
                // Saturate instead of overflowing. CSI parameters are
                // attacker-controlled (a cat'd file, remote shell output) and
                // the std::stoi path this replaces clamped out-of-range values
                // to INT_MAX rather than throwing.
                if (csi_acc_ <= (std::numeric_limits<int>::max() - 9) / 10) {
                    csi_acc_ = csi_acc_ * 10 + static_cast<int>(c - '0');
                } else {
                    csi_acc_ = std::numeric_limits<int>::max();
                }
                csi_acc_digits_ = true;
            } else if (c == ';' || c == ':') {
                // ':' is the ITU subparameter separator (e.g. SGR 38:5:196).
                // Treating it like ';' keeps the digits from concatenating
                // into a single garbage parameter; the colon-form extended
                // color sequences then parse identically to the ';' form.
                // An empty parameter (";;" or a leading ";") is 0, which is
                // what the accumulator already holds when no digits arrived.
                csi_params_.push_back(csi_acc_);
                csi_acc_ = 0;
                csi_acc_digits_ = false;
            } else if (c >= 0x40 && c <= 0x7E) {
                if (csi_acc_digits_) {
                    csi_params_.push_back(csi_acc_);
                }
                process_csi_sequence(grid, static_cast<char>(c));
                state_ = STATE_NORMAL;
            }
            break;
        }
    }
}

// Standard base64 decode (RFC 4648, no URL-safe alphabet). Malformed input
// (bad characters, truncated padding) just stops decoding at that point and
// returns whatever was successfully decoded so far rather than failing the
// whole payload -- OSC 52 senders occasionally get padding slightly wrong,
// and a partial clipboard write is a much better failure mode than none.
static std::string base64_decode(const std::string& in) {
    static int8_t decode_table[256];
    static bool initialized = false;
    if (!initialized) {
        std::fill(std::begin(decode_table), std::end(decode_table), -1);
        const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) decode_table[static_cast<uint8_t>(alphabet[i])] = static_cast<int8_t>(i);
        initialized = true;
    }

    std::string out;
    out.reserve(in.size() * 3 / 4 + 3);
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        int8_t d = decode_table[c];
        if (d < 0) continue; // skip whitespace/newlines some senders wrap at
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 0) {
            out += static_cast<char>((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return out;
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
        case 52: {
            // Clipboard: OSC 52;Pc;Pd. Pc selects which selection (c =
            // clipboard, p = primary, s = selection, ...) -- sink only has
            // one system clipboard, so it's ignored; Pd is base64, or the
            // literal string "?" to *query* the current clipboard, which
            // this deliberately does not answer (see set_clipboard_text).
            size_t inner_semi = pt.find(';');
            std::string pd = (inner_semi != std::string::npos) ? pt.substr(inner_semi + 1) : "";
            if (!pd.empty() && pd != "?") {
                grid.set_clipboard_text(base64_decode(pd));
            }
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
            if (grid.is_origin_mode()) {
                // Row 1 means the scroll region's top margin, not the
                // screen's; also can't be positioned outside the region.
                row = std::clamp(row + grid.get_scroll_top(), grid.get_scroll_top(), grid.get_scroll_bottom());
            }
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
            grid.cursor_home(); // DECSTBM homes the cursor (to the origin if DECOM is set)
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
                    case 6:    // DECOM: like DECSTBM, toggling it homes the cursor
                        grid.set_origin_mode(set);
                        grid.cursor_home();
                        break;
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
