#include "ansi_parser.hpp"
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <unordered_map>

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

// The palette lives on the grid now, so that OSC 4 can change it. This is the
// same lookup it always was, just reading state instead of a static table.
static SDL_FColor xterm_256_color(const TerminalGrid& grid, int idx) {
    return grid.palette_color(idx);
}

void ANSIParser::reset_csi() {
    csi_params_.clear();
    csi_acc_ = 0;
    csi_acc_digits_ = false;
    csi_private_ = 0;
    csi_intermediate_ = 0;
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
                note_trigger_run(grid, data + i, wrote);
                last_graphic_ = static_cast<unsigned char>(data[i + wrote - 1]);
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
void ANSIParser::note_trigger_run(TerminalGrid& grid, const char* run, int n) {
    // Not std::tolower: it is locale-aware, so it stays a real libsystem_c
    // call per character. Callers restrict the run to printable ASCII.
    auto lower = [](char ch) -> char {
        return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch + 32) : ch;
    };

    // Offsets below zero index the tail carried over from previous calls, so
    // a word split across a pty read -- or across a control character, which
    // breaks the run but not the word -- still matches. Slots never written
    // read as zero and cannot match a letter.
    uint64_t carried = trigger_tail_;
    int usable_from = -kTrigWindow;
    auto char_at = [&](int off) -> char {
        if (off >= 0) return lower(run[off]);
        int shift = 8 * (-off - 1);
        if (shift >= 64) return '\0';
        return lower(static_cast<char>((carried >> shift) & 0xFF));
    };
    bool fired = false;

    for (int k = 0; k < n; ++k) {
        // Gate on the final letter: only 'r' can finish "error" and only 'd'
        // can finish "failed", so almost every character costs one compare
        // and no memory traffic at all.
        char lc = lower(run[k]);
        const char* word;
        int len;
        if (lc == 'r')      { word = "error";  len = 5; }
        else if (lc == 'd') { word = "failed"; len = 6; }
        else continue;

        int start = k - len + 1;
        if (start < usable_from) continue;

        bool hit = true;
        for (int j = 0; j < len - 1; ++j) {
            if (char_at(start + j) != word[j]) { hit = false; break; }
        }
        if (!hit) continue;

        grid.trigger_error_flash();
        // The window resets on a hit, as it did before, so one occurrence
        // cannot fire twice through overlapping text.
        carried = 0;
        usable_from = k + 1;
        fired = true;
    }

    // Carry the tail forward: at most eight shift-and-or steps on a register,
    // with nothing older than the window able to survive them. memcpy/memmove
    // with a runtime length were tried here first and were the reason short
    // runs regressed -- they compile to real libc calls, which for a run of a
    // few characters cost more than the scan they support.
    int from = std::max(fired ? usable_from : 0, n - kTrigWindow);
    if (from == n - kTrigWindow) {
        // The run alone supplies the whole window: a fixed eight steps the
        // compiler can unroll, with no carried value from before the run.
        uint64_t t = 0;
        const char* w = run + n - kTrigWindow;
        for (int k = 0; k < kTrigWindow; ++k) {
            t = (t << 8) | static_cast<unsigned char>(w[k]);
        }
        trigger_tail_ = t;
    } else {
        uint64_t t = fired ? 0 : trigger_tail_;
        for (int k = from; k < n; ++k) {
            t = (t << 8) | static_cast<unsigned char>(run[k]);
        }
        trigger_tail_ = t;
    }
}

void ANSIParser::note_trigger_char(TerminalGrid& grid, char32_t c) {
    char ch = static_cast<char>(c);
    note_trigger_run(grid, &ch, 1);
}

// Standard base64, defined further down next to the OSC 52 clipboard decode
// that was its first caller.
static std::string base64_decode(const std::string& in);

// Places a kitty-protocol image at the cursor. The protocol lets the sender
// crop the source (x/y/w/h) and choose how many cells to fill (c/r); left to
// itself it uses the whole image at its natural size.
static void place_kitty_image(TerminalGrid& grid, uint64_t image_id, uint64_t placement_id,
                              int src_x, int src_y, int src_w, int src_h,
                              int want_cols, int want_rows, int z) {
    const TerminalImage* img = grid.images().find(image_id);
    if (!img) return;

    if (src_w <= 0) src_w = img->width - src_x;
    if (src_h <= 0) src_h = img->height - src_y;
    src_x = std::max(0, std::min(src_x, img->width));
    src_y = std::max(0, std::min(src_y, img->height));
    src_w = std::max(0, std::min(src_w, img->width - src_x));
    src_h = std::max(0, std::min(src_h, img->height - src_y));
    if (src_w == 0 || src_h == 0) return;

    int cell_w = grid.effective_cell_px_w();
    int cell_h = grid.effective_cell_px_h();
    int cols = want_cols > 0 ? want_cols : (src_w + cell_w - 1) / cell_w;
    int rows = want_rows > 0 ? want_rows : (src_h + cell_h - 1) / cell_h;
    cols = std::min(cols, grid.get_cols() - grid.get_cursor_col());
    if (cols <= 0 || rows <= 0) return;

    ImagePlacement placement;
    placement.image_id = image_id;
    placement.placement_id = placement_id;
    placement.line_id = grid.line_id_for_row(grid.get_cursor_row());
    placement.col = grid.get_cursor_col();
    placement.cols = cols;
    placement.rows = rows;
    placement.src_x = src_x;
    placement.src_y = src_y;
    placement.src_w = src_w;
    placement.src_h = src_h;
    placement.z = z;
    grid.images().place(placement);

    // Unlike sixel, the kitty protocol leaves the cursor where it was: an
    // application placing an image is positioning it itself and does not want
    // the terminal moving the cursor out from under it.
}

// Kitty graphics protocol: ESC _ G <key=value,...> ; <base64 payload> ESC \
//
// Deliberately narrower than the full protocol, and the omissions are choices
// rather than gaps:
//
// - Only t=d, the payload arriving inline. t=f and t=t have the terminal open
//   a path the sender names, and t=s attaches shared memory it names. That is
//   a filesystem read performed on behalf of whatever can write to the tty,
//   which is any command output and any remote host on the other end of an
//   ssh session. Clients that ask get an EBADF back and fall back to sending
//   the bytes inline, which is slower and entirely safe.
// - No o=z compression, no animation frames, no unicode placeholders. Clients
//   are told so and pick something else.
void ANSIParser::dispatch_apc(TerminalGrid& grid) {
    if (apc_buffer_.empty() || apc_buffer_[0] != 'G') return;

    std::string body = apc_buffer_.substr(1);
    size_t semi = body.find(';');
    std::string controls = (semi == std::string::npos) ? body : body.substr(0, semi);
    std::string payload = (semi == std::string::npos) ? std::string() : body.substr(semi + 1);

    auto parse_controls = [](const std::string& text) {
        std::unordered_map<char, std::string> out;
        size_t i = 0;
        while (i < text.size()) {
            size_t comma = text.find(',', i);
            if (comma == std::string::npos) comma = text.size();
            size_t eq = text.find('=', i);
            if (eq != std::string::npos && eq < comma && eq > i) {
                out[text[i]] = text.substr(eq + 1, comma - eq - 1);
            }
            i = comma + 1;
        }
        return out;
    };

    std::unordered_map<char, std::string> keys = parse_controls(controls);
    auto num = [&](char k, long fallback) -> long {
        auto it = keys.find(k);
        if (it == keys.end() || it->second.empty()) return fallback;
        char* end = nullptr;
        long v = std::strtol(it->second.c_str(), &end, 10);
        return end == it->second.c_str() ? fallback : v;
    };
    auto letter = [&](char k, char fallback) -> char {
        auto it = keys.find(k);
        return (it == keys.end() || it->second.empty()) ? fallback : it->second[0];
    };

    // Chunking. Continuation chunks carry only m= and payload, so the first
    // chunk's control data is what governs the whole transfer.
    std::string data;
    long more = num('m', 0);
    if (kitty_.active) {
        kitty_.data += base64_decode(payload);
        if (kitty_.data.size() > kApcMaxLen) { kitty_ = KittyTransfer{}; return; }
        if (more == 1) return;
        controls = kitty_.controls;
        keys = parse_controls(controls);
        data.swap(kitty_.data);
        kitty_ = KittyTransfer{};
    } else if (more == 1) {
        kitty_.active = true;
        kitty_.controls = controls;
        kitty_.data = base64_decode(payload);
        return;
    } else {
        data = base64_decode(payload);
    }

    const long quiet = num('q', 0);
    const uint64_t image_id = static_cast<uint64_t>(num('i', 0));
    const uint64_t placement_id = static_cast<uint64_t>(num('p', 0));
    auto respond = [&](const char* status, bool is_error) {
        // q=1 silences the successes, q=2 silences everything. A client that
        // asked for silence and gets chatter has its own output corrupted.
        if (quiet >= 2 || (quiet >= 1 && !is_error)) return;
        std::string reply = "\x1b_G";
        if (image_id) reply += "i=" + std::to_string(image_id);
        if (placement_id) {
            if (image_id) reply += ",";
            reply += "p=" + std::to_string(placement_id);
        }
        reply += ";";
        reply += status;
        reply += "\x1b\\";
        grid.queue_reply(reply);
    };

    const char action = letter('a', 't');

    if (action == 'd') {
        // Delete. The uppercase forms free the pixels as well as the
        // placement; the lowercase ones leave the image available to place
        // again, which is the whole point of the distinction.
        char what = letter('d', 'a');
        bool free_data = (what >= 'A' && what <= 'Z');
        char lower = static_cast<char>(free_data ? what + 32 : what);
        if (lower == 'a') {
            if (free_data) grid.images().clear_all();
            else grid.images().clear_placements();
        } else if (lower == 'i') {
            if (free_data) grid.images().delete_image(image_id);
            else grid.images().delete_placement(image_id, placement_id);
        }
        return;
    }

    if (action == 'q') {
        // A capability probe. Answering OK is the whole point: it is how a
        // client learns the protocol is available at all.
        respond("OK", false);
        return;
    }

    if (action == 't' || action == 'T') {
        const char medium = letter('t', 'd');
        if (medium != 'd') {
            respond("EBADF:only direct transmission is supported", true);
            return;
        }
        if (keys.count('o')) {
            respond("EINVAL:compression is not supported", true);
            return;
        }

        const long format = num('f', 32);
        std::vector<uint32_t> pixels;
        int width = 0, height = 0;

        if (format == 100) {
            if (!decode_png(data.data(), data.size(), pixels, width, height)) {
                respond("EINVAL:could not decode PNG", true);
                return;
            }
        } else if (format == 32 || format == 24) {
            width = static_cast<int>(num('s', 0));
            height = static_cast<int>(num('v', 0));
            const int stride = (format == 32) ? 4 : 3;
            if (width <= 0 || height <= 0 ||
                data.size() < static_cast<size_t>(width) * height * stride) {
                respond("EINVAL:raw pixels do not match s and v", true);
                return;
            }
            pixels.resize(static_cast<size_t>(width) * height);
            const unsigned char* src = reinterpret_cast<const unsigned char*>(data.data());
            for (size_t i = 0; i < pixels.size(); ++i) {
                const unsigned char* p = src + i * stride;
                unsigned alpha = (stride == 4) ? p[3] : 0xFFu;
                pixels[i] = static_cast<uint32_t>(p[0]) |
                            (static_cast<uint32_t>(p[1]) << 8) |
                            (static_cast<uint32_t>(p[2]) << 16) |
                            (alpha << 24);
            }
        } else {
            respond("EINVAL:unsupported format", true);
            return;
        }

        uint64_t stored = grid.images().store(image_id, width, height, std::move(pixels));
        if (stored == 0) {
            respond("ENOMEM:image rejected", true);
            return;
        }
        if (action == 'T') {
            place_kitty_image(grid, stored, placement_id,
                              static_cast<int>(num('x', 0)), static_cast<int>(num('y', 0)),
                              static_cast<int>(num('w', 0)), static_cast<int>(num('h', 0)),
                              static_cast<int>(num('c', 0)), static_cast<int>(num('r', 0)),
                              static_cast<int>(num('z', 0)));
        }
        respond("OK", false);
        return;
    }

    if (action == 'p') {
        if (!grid.images().has_image(image_id)) {
            respond("ENOENT:no such image", true);
            return;
        }
        place_kitty_image(grid, image_id, placement_id,
                              static_cast<int>(num('x', 0)), static_cast<int>(num('y', 0)),
                              static_cast<int>(num('w', 0)), static_cast<int>(num('h', 0)),
                              static_cast<int>(num('c', 0)), static_cast<int>(num('r', 0)),
                              static_cast<int>(num('z', 0)));
        respond("OK", false);
        return;
    }
}

void ANSIParser::dispatch_dcs(TerminalGrid& grid) {
    // Sixel is "DCS <params> q <data> ST". Anything else in a DCS is consumed
    // and ignored, as it was before this captured them at all.
    size_t q = dcs_buffer_.find('q');
    if (q == std::string::npos) return;
    for (size_t i = 0; i < q; ++i) {
        char ch = dcs_buffer_[i];
        if (!((ch >= '0' && ch <= '9') || ch == ';')) return; // not a sixel introducer
    }

    // P2 selects what happens to pixels no sixel touches: 1 leaves them
    // transparent, anything else paints them the background colour.
    int params[3] = {0, 0, 0};
    int index = 0;
    int value = 0;
    bool any = false;
    for (size_t i = 0; i < q && index < 3; ++i) {
        char ch = dcs_buffer_[i];
        if (ch == ';') {
            params[index++] = any ? value : 0;
            value = 0;
            any = false;
        } else {
            value = value * 10 + (ch - '0');
            any = true;
        }
    }
    if (index < 3) params[index] = any ? value : 0;

    std::vector<uint32_t> pixels;
    int width = 0, height = 0;
    if (!decode_sixel(dcs_buffer_.data() + q + 1, dcs_buffer_.size() - q - 1,
                      params[1] == 1, pixels, width, height)) {
        return;
    }
    uint64_t id = grid.images().store(0, width, height, std::move(pixels));
    if (id == 0) return;
    grid.place_image_at_cursor(id, width, height);
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
                        // HT: to the next tab stop. The stops are a real table
                        // now rather than (col + 8) & ~7 arithmetic, so a
                        // program that moved them with HTS/TBC gets the stops
                        // it asked for instead of the default ones.
                        grid.tab_forward(1);
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
                // Zero-width characters that carry no meaning for the cell
                // they land next to: a break opportunity, a word joiner, a
                // byte-order mark. Dropped, as they always were.
                //
                // The joiners and the variation selectors used to be dropped
                // here too, and must not be: they are part of the grapheme
                // cluster they follow, and the grid now attaches them to it
                // rather than giving them a cell of their own. Dropping the
                // ZWJ is what made a family emoji render as four separate
                // people, and dropping U+FE0F is what left emoji in their
                // text presentation.
                if (c == 0x200B || c == 0x2060 || c == 0xFEFF) {
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
            last_graphic_ = out;

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
                str_is_dcs_ = (c == 'P');
                str_is_apc_ = (c == '_');
                osc_buffer_.clear();
                dcs_buffer_.clear();
                apc_buffer_.clear();
                state_ = STATE_STR;
            } else if (c == 'c') {
                // RIS: full reset. Also clears the parser's own carried state
                // -- a pending charset designation or half-built CSI must not
                // survive a reset any more than the grid's modes do.
                grid.full_reset();
                g0_dec_graphics_ = false;
                last_graphic_ = 0;
                utf8_bytes_needed_ = 0;
                utf8_codepoint_ = 0;
                reset_csi();
                trigger_tail_ = 0;
                state_ = STATE_NORMAL;
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
            } else if (c == 'H') { // HTS: set a tab stop at the cursor column
                grid.set_tab_stop();
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
                str_ended_with_bel_ = (c == 0x07);
                if (str_is_osc_) dispatch_osc(grid);
                if (str_is_dcs_) dispatch_dcs(grid);
                if (str_is_apc_) dispatch_apc(grid);
                state_ = STATE_NORMAL;
            } else if (c == 0x1b) { // possible start of two-byte ST (ESC \)
                state_ = STATE_STR_ESC;
            } else if (str_is_dcs_) {
                // Sixel is bytes, not text: no UTF-8 re-encoding, and anything
                // above ASCII is not part of the format so it is dropped
                // rather than widened.
                if (c < 0x80 && dcs_buffer_.size() < kDcsMaxLen) {
                    dcs_buffer_ += static_cast<char>(c);
                }
            } else if (str_is_apc_) {
                if (c < 0x80 && apc_buffer_.size() < kApcMaxLen) {
                    apc_buffer_ += static_cast<char>(c);
                }
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
                str_ended_with_bel_ = false;
                if (str_is_osc_) dispatch_osc(grid);
                if (str_is_dcs_) dispatch_dcs(grid);
                if (str_is_apc_) dispatch_apc(grid);
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
            if (c >= 0x3C && c <= 0x3F) {
                // Private markers '<' '=' '>' '?'. Only the last byte counts;
                // no real sequence carries two.
                csi_private_ = static_cast<char>(c);
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
            } else if (c >= 0x20 && c <= 0x2F) {
                // Intermediate bytes, which select between sequences sharing a
                // final byte: "CSI Ps SP q" is DECSCUSR, "CSI ! p" is DECSTR.
                csi_intermediate_ = static_cast<char>(c);
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

// Parses an X11-style colour specification: "#RGB", "#RRGGBB", "#RRRGGGBBB",
// "#RRRRGGGGBBBB" and the "rgb:R/G/B" form with 1-4 hex digits per component.
// Named colours are not accepted -- they would mean carrying the whole X11
// colour table for a form nothing emits any more.
static bool parse_color_spec(const std::string& spec, SDL_FColor& out) {
    auto hex_component = [](const std::string& text) -> float {
        if (text.empty() || text.size() > 4) return -1.0f;
        unsigned value = 0;
        for (char ch : text) {
            int digit;
            if (ch >= '0' && ch <= '9') digit = ch - '0';
            else if (ch >= 'a' && ch <= 'f') digit = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10;
            else return -1.0f;
            value = value * 16 + static_cast<unsigned>(digit);
        }
        // Scaled by the width actually given, so "f", "ff" and "ffff" are all
        // full intensity rather than 1/16th, 1/256th and 1.
        unsigned max = (1u << (4 * text.size())) - 1u;
        return static_cast<float>(value) / static_cast<float>(max);
    };

    if (spec.size() > 4 && (spec.compare(0, 4, "rgb:") == 0 || spec.compare(0, 4, "RGB:") == 0)) {
        std::string body = spec.substr(4);
        size_t a = body.find('/');
        if (a == std::string::npos) return false;
        size_t b = body.find('/', a + 1);
        if (b == std::string::npos) return false;
        float r = hex_component(body.substr(0, a));
        float g = hex_component(body.substr(a + 1, b - a - 1));
        float bl = hex_component(body.substr(b + 1));
        if (r < 0 || g < 0 || bl < 0) return false;
        out = { r, g, bl, 1.0f };
        return true;
    }

    if (!spec.empty() && spec[0] == '#') {
        std::string body = spec.substr(1);
        if (body.size() % 3 != 0 || body.empty() || body.size() > 12) return false;
        size_t width = body.size() / 3;
        float r = hex_component(body.substr(0, width));
        float g = hex_component(body.substr(width, width));
        float bl = hex_component(body.substr(2 * width, width));
        if (r < 0 || g < 0 || bl < 0) return false;
        out = { r, g, bl, 1.0f };
        return true;
    }
    return false;
}

// The form xterm answers queries in: 16 bits per component.
static std::string format_color_spec(const SDL_FColor& c) {
    auto component = [](float v) {
        int scaled = static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535.0f));
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%04x", scaled);
        return std::string(buf);
    };
    return "rgb:" + component(c.r) + "/" + component(c.g) + "/" + component(c.b);
}

void ANSIParser::dispatch_osc(TerminalGrid& grid) {
    // Payload shape: "Ps;Pt" -- numeric selector, then text. The text half is
    // optional: the colour-reset sequences (OSC 110/111/112) are a bare number
    // with no semicolon at all, and were being dropped here before they were
    // implemented because this required one.
    size_t semi = osc_buffer_.find(';');
    std::string selector = (semi == std::string::npos) ? osc_buffer_
                                                       : osc_buffer_.substr(0, semi);
    std::string pt = (semi == std::string::npos) ? std::string()
                                                 : osc_buffer_.substr(semi + 1);
    osc_buffer_.clear();

    if (selector.empty()) return;
    int ps = 0;
    for (char ch : selector) {
        if (ch < '0' || ch > '9') return;
        if (ps > 100000) return; // no real selector is this large
        ps = ps * 10 + (ch - '0');
    }

    switch (ps) {
        case 0: // set icon name + window title
        case 2: // set window title
            grid.set_window_title(pt);
            break;
        case 4: {
            // OSC 4 ; index ; spec [; index ; spec ...] -- set or query
            // palette entries. Themes use it to recolour the 256-colour
            // palette at runtime; "?" in place of a spec asks instead.
            size_t start = 0;
            while (start < pt.size()) {
                size_t mid = pt.find(';', start);
                if (mid == std::string::npos) break;
                size_t end = pt.find(';', mid + 1);
                std::string index_text = pt.substr(start, mid - start);
                std::string spec = pt.substr(mid + 1, end == std::string::npos
                                                          ? std::string::npos
                                                          : end - mid - 1);
                int index = -1;
                if (!index_text.empty()) {
                    index = 0;
                    for (char ch : index_text) {
                        if (ch < '0' || ch > '9') { index = -1; break; }
                        index = index * 10 + (ch - '0');
                        if (index > 255) { index = -1; break; }
                    }
                }
                if (index >= 0) {
                    if (spec == "?") {
                        grid.queue_reply("\x1b]4;" + std::to_string(index) + ";" +
                                         format_color_spec(grid.palette_color(index)) +
                                         (str_ended_with_bel_ ? "\x07" : "\x1b\\"));
                    } else {
                        SDL_FColor parsed;
                        if (parse_color_spec(spec, parsed)) grid.set_palette_color(index, parsed);
                    }
                }
                if (end == std::string::npos) break;
                start = end + 1;
            }
            break;
        }
        case 104: {
            // Reset palette entries, or the whole palette when no index is
            // given -- which is how a shell tidies up after a theme.
            if (pt.empty()) {
                grid.reset_palette();
                break;
            }
            size_t start = 0;
            while (start <= pt.size()) {
                size_t end = pt.find(';', start);
                std::string item = pt.substr(start, end == std::string::npos
                                                        ? std::string::npos
                                                        : end - start);
                int index = 0;
                bool ok = !item.empty();
                for (char ch : item) {
                    if (ch < '0' || ch > '9') { ok = false; break; }
                    index = index * 10 + (ch - '0');
                    if (index > 255) { ok = false; break; }
                }
                if (ok) grid.reset_palette_color(index);
                if (end == std::string::npos) break;
                start = end + 1;
            }
            break;
        }
        case 10:   // default foreground
        case 11:   // default background
        case 12: { // cursor colour
            // Query ("?") or set. The query is the valuable half: OSC 11 is
            // how an application finds out whether it is drawing on something
            // light or something dark, and picks a readable palette. Without
            // an answer it guesses, and against sink's media backgrounds it
            // will usually guess wrong.
            //
            // Unlike the window-title report this is safe to answer: the reply
            // is a colour the terminal formats itself, not text an attacker
            // planted and got echoed back into the input stream.
            //
            // Several colours can be queried in one sequence
            // (OSC 10;?;?;? asks for 10, 11 and 12 in turn), so this walks the
            // ';'-separated list with the selector advancing as it goes.
            int which = ps;
            size_t start = 0;
            while (start <= pt.size() && which <= 12) {
                size_t end = pt.find(';', start);
                std::string item = pt.substr(start, end == std::string::npos
                                                        ? std::string::npos
                                                        : end - start);
                if (item == "?") {
                    const SDL_FColor& c = (which == 10)  ? grid.get_default_fg()
                                          : (which == 11) ? grid.get_reported_bg()
                                                          : grid.get_default_cursor_color();
                    std::string reply = "\x1b]" + std::to_string(which) + ";" +
                                        format_color_spec(c) +
                                        (str_ended_with_bel_ ? "\x07" : "\x1b\\");
                    grid.queue_reply(reply);
                } else {
                    SDL_FColor parsed;
                    if (parse_color_spec(item, parsed)) {
                        if (which == 10)      grid.set_default_fg(parsed);
                        else if (which == 11) grid.set_default_bg(parsed);
                        else                  grid.set_default_cursor_color(parsed);
                    }
                }
                if (end == std::string::npos) break;
                start = end + 1;
                ++which;
            }
            break;
        }
        case 110: grid.reset_default_fg(); break;
        case 111: grid.reset_default_bg(); break;
        case 112: grid.reset_default_cursor_color(); break;
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

// DECRQM reply values (the Pm field of CSI ? Ps ; Pm $ y).
enum : int {
    kModeNotRecognised    = 0,
    kModeSet              = 1,
    kModeReset            = 2,
    kModePermanentlySet   = 3,
    kModePermanentlyReset = 4,
};

// State of a DEC private mode, for DECRQM.
//
// "Not recognised" is the honest answer for anything sink has no state for,
// and it is also the safe one: a program reading 0 falls back to whatever it
// would have done without the query. Reporting a plausible 1 or 2 for a mode
// that does nothing here would be worse than saying nothing.
static int query_private_mode(const TerminalGrid& grid, int ps) {
    auto b = [](bool on) { return on ? kModeSet : kModeReset; };
    switch (ps) {
        case 1:    return b(grid.is_app_cursor_keys());        // DECCKM
        case 6:    return b(grid.is_origin_mode());            // DECOM
        // DECAWM. sink always wraps and offers no way to turn it off, which
        // is exactly what "permanently set" is for.
        case 7:    return kModePermanentlySet;
        case 9:    return b(grid.get_mouse_mode() == 9);       // X10 mouse
        case 25:   return b(grid.is_cursor_visible());         // DECTCEM
        case 47:
        case 1047:
        case 1049: return b(grid.is_alt_screen_active());
        case 1000:
        case 1002:
        case 1003: return b(grid.get_mouse_mode() == ps);
        case 1004: return b(grid.is_focus_reporting());
        case 1006: return b(grid.is_mouse_sgr());
        case 1007: return b(grid.is_alternate_scroll());
        case 2004: return b(grid.is_bracketed_paste_active());
        // The one this feature is really for: sink implements synchronized
        // output, and DECRQM is how a program finds that out. Without an
        // answer here every app that probes concluded it was unsupported and
        // fell back to unsynchronized redraws.
        case 2026: return b(grid.is_synchronized_output());
        // 1048 is deliberately absent. It is DECSC/DECRC dressed as a mode --
        // an action with no state to report -- so there is no true answer.
        default:   return kModeNotRecognised;
    }
}

// State of an ANSI (non-private) mode.
static int query_ansi_mode(int ps) {
    switch (ps) {
        // IRM. sink always replaces rather than inserts, and has no way to
        // change that, so the reset state is permanent.
        case 4:  return kModePermanentlyReset;
        // LNM, likewise: a bare LF never implies a carriage return here.
        case 20: return kModePermanentlyReset;
        default: return kModeNotRecognised;
    }
}

// Cursor Position Report. Row and column go on the wire 1-based. Under origin
// mode (DECOM) the row is relative to the scroll region's top margin, so that
// the number reported is the same one CUP would take to put the cursor back
// where it is.
static std::string cursor_position_report(const TerminalGrid& grid, bool extended) {
    int row = grid.get_cursor_row();
    if (grid.is_origin_mode()) row -= grid.get_scroll_top();
    if (row < 0) row = 0;

    std::string out = "\x1b[";
    if (extended) out += '?';
    out += std::to_string(row + 1);
    out += ';';
    out += std::to_string(grid.get_cursor_col() + 1);
    // DECXCPR carries a page number as well; sink has one page.
    out += extended ? ";1R" : "R";
    return out;
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
                    grid.set_current_fg(grid.palette_color(idx));
                }
            };
            auto reset_all = [&]() {
                grid.set_current_fg(grid.get_default_fg());
                grid.set_current_bg(grid.get_default_bg());
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
                    grid.set_current_bg(grid.palette_color(param - 40));
                } else if (param >= 90 && param <= 97) {
                    // Explicit bright: not subject to bold re-brightening
                    fg_base_index_ = -1;
                    grid.set_current_fg(grid.palette_color(param - 90 + 8));
                } else if (param >= 100 && param <= 107) {
                    grid.set_current_bg(grid.palette_color(param - 100 + 8));
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
                        SDL_FColor color = xterm_256_color(grid, csi_params_[i + 2]);
                        if (is_fg) grid.set_current_fg(color);
                        else       grid.set_current_bg(color);
                        i += 2;
                    }
                } else if (param == 39) {
                    // Default foreground color
                    fg_base_index_ = -1;
                    grid.set_current_fg(grid.get_default_fg());
                } else if (param == 49) {
                    // Default background color
                    grid.set_current_bg(grid.get_default_bg());
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
        case 't': { // XTWINOPS -- window manipulation and geometry reports
            // Only the reports are implemented, and deliberately so.
            //
            // The manipulation operations (1-9: move, resize, raise, lower,
            // iconify, maximise) hand anything that can write to the terminal
            // control of the user's window. xterm gates them behind
            // allowWindowOps, off by default; sink simply does not have them.
            //
            // 21 (report window title) is omitted for the same reason OSC 52
            // clipboard read-back is refused elsewhere in this file: it gives
            // untrusted output a way to read back state it did not write, and
            // titles routinely carry the working directory and often the
            // command being run.
            //
            //
            // 19 (screen size) *is* answered, but only from a real display
            // measurement pushed in by the layout -- never from the window's
            // own size, which would be a wrong answer dressed as a right one.
            int rows = grid.get_rows();
            int cols = grid.get_cols();
            int cw = grid.get_cell_pixel_width();
            int ch = grid.get_cell_pixel_height();
            switch (get_param(0, 0)) {
                case 14: // text area size in pixels
                    if (cw > 0 && ch > 0) {
                        grid.queue_reply("\x1b[4;" + std::to_string(rows * ch) +
                                         ";" + std::to_string(cols * cw) + "t");
                    }
                    break;
                case 16: // character cell size in pixels
                    if (cw > 0 && ch > 0) {
                        grid.queue_reply("\x1b[6;" + std::to_string(ch) +
                                         ";" + std::to_string(cw) + "t");
                    }
                    break;
                case 18: // text area size in characters
                    grid.queue_reply("\x1b[8;" + std::to_string(rows) +
                                     ";" + std::to_string(cols) + "t");
                    break;
                case 19: // whole screen size in characters
                    if (grid.get_screen_cols() > 0 && grid.get_screen_rows() > 0) {
                        grid.queue_reply("\x1b[9;" + std::to_string(grid.get_screen_rows()) +
                                         ";" + std::to_string(grid.get_screen_cols()) + "t");
                    }
                    break;
                default:
                    break;
            }
            break;
        }
        case 'p': {
            // Two different sequences share this final byte, told apart by
            // their intermediate: '!' is DECSTR, '$' is DECRQM. Before
            // intermediates were tracked neither could be recognised.
            if (csi_intermediate_ == '$') { // DECRQM -- Request Mode
                int ps = get_param(0, 0);
                bool priv = is_private_mode();
                int state = priv ? query_private_mode(grid, ps) : query_ansi_mode(ps);
                std::string reply = "\x1b[";
                if (priv) reply += '?';
                reply += std::to_string(ps);
                reply += ';';
                reply += std::to_string(state);
                reply += "$y";
                grid.queue_reply(reply);
                break;
            }
            // The '!' intermediate is what makes this DECSTR rather than one
            // of the several other sequences ending in 'p'.
            if (csi_intermediate_ == '!') {
                grid.soft_reset();
                // The parser's own carried state goes with it, as it does for
                // RIS -- a half-finished charset designation surviving a reset
                // would keep translating text the caller has just asked to
                // stop translating.
                g0_dec_graphics_ = false;
                last_graphic_ = 0;
            }
            break;
        }
        case 'b': { // REP -- Repeat the preceding graphic character
            if (last_graphic_ != 0) {
                int n = get_count_param(0, 1);
                // Bounded to a screenful. The count is attacker-controlled,
                // and "CSI 2147483647 b" would otherwise sit in the parser
                // for minutes -- which would also walk straight through the
                // per-frame parse budget, since that is checked between
                // slices and not inside one sequence.
                int limit = grid.get_cols() * grid.get_rows();
                if (limit > 0 && n > limit) n = limit;
                for (int k = 0; k < n; ++k) grid.write_character(last_graphic_);
            }
            break;
        }
        case 'I': { // CHT -- Cursor Forward Tabulation
            grid.tab_forward(get_count_param(0, 1));
            break;
        }
        case 'Z': { // CBT -- Cursor Backward Tabulation
            grid.tab_backward(get_count_param(0, 1));
            break;
        }
        case 'g': { // TBC -- Tab Clear
            // 0 (and the default) clears the stop under the cursor; 3 clears
            // every stop. The other values address stops in a vertical
            // dimension the VT100 had and this does not.
            int mode = get_param(0, 0);
            if (mode == 0) grid.clear_tab_stop();
            else if (mode == 3) grid.clear_all_tab_stops();
            break;
        }
        case 'E': { // Cursor Next Line (CNL)
            // Down n rows *and* to the first column, which is what separates
            // it from CUD. Shells and TUIs reach for it when moving to the
            // start of a following line, and with it missing the cursor
            // stayed in whatever column it was in.
            grid.set_cursor_row(grid.get_cursor_row() + get_count_param(0, 1));
            grid.set_cursor_col(0);
            break;
        }
        case 'F': { // Cursor Preceding Line (CPL)
            grid.set_cursor_row(grid.get_cursor_row() - get_count_param(0, 1));
            grid.set_cursor_col(0);
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
            if (!is_private_mode()) break;
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
                    case 2026: grid.set_synchronized_output(set); break;
                    case 1004: grid.set_focus_reporting(set); break;
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
        case '@': { // ICH -- Insert Character
            grid.insert_character(get_count_param(0, 1));
            break;
        }
        case 'q': { // DECSCUSR -- Set Cursor Style
            // The space intermediate is what separates this from DECLL
            // (CSI Ps q, load LEDs), which sink has no LEDs to load. Before
            // intermediates were tracked the two were the same sequence here.
            if (csi_intermediate_ == ' ') {
                grid.set_cursor_shape(get_param(0, 0));
            }
            break;
        }
        case 'n': { // DSR -- Device Status Report
            // These are the sequences a terminal is obliged to answer. A
            // program that asks blocks until the reply arrives, so ignoring
            // them did not degrade gracefully: it stalled whatever asked
            // until that program's own timeout fired, if it had one.
            int ps = get_param(0, 0);
            if (is_private_mode()) {
                // DECDSR. Only the cursor report has an analogue here; the
                // rest describe printers, user-defined keys and keyboard
                // hardware that sink has nothing to say about, and answering
                // them falsely is worse than staying quiet.
                if (ps == 6) grid.queue_reply(cursor_position_report(grid, true));
            } else if (ps == 5) {
                grid.queue_reply("\x1b[0n"); // terminal OK, no malfunction
            } else if (ps == 6) {
                grid.queue_reply(cursor_position_report(grid, false));
            }
            break;
        }
        case 'c': { // DA -- Device Attributes
            if (csi_private_ == '>') {
                // Secondary DA: terminal type, firmware version, cartridge.
                // Type 0 is the VT100 family; the version field is by
                // convention a patch level as a bare integer, so 0.8.0 reports
                // 800. Derived from the project version rather than written
                // out, so a release bump cannot leave this behind.
                grid.queue_reply("\x1b[>0;" + std::to_string(SINK_VERSION_NUM) + ";0c");
            } else if (csi_private_ == 0 && get_param(0, 0) == 0) {
                // Primary DA. 62 = VT220-class, 22 = ANSI colour.
                //
                // Deliberately narrow: sink has no selective erase (6), no
                // printer (2), no UDKs (8) and no technical character set
                // (15), and claiming them would only persuade programs to
                // send sequences that get ignored. Under-claiming costs a
                // fallback path; over-claiming costs correctness.
                grid.queue_reply("\x1b[?62;22c");
            }
            break;
        }
        case 's': { // Save Cursor (ANSI.SYS)
            // CSI ? Ps s is XTSAVE (save private mode values), which ncurses
            // emits on every mouse enable -- it must not clobber the cursor
            if (!is_private_mode()) grid.save_cursor();
            break;
        }
        case 'u': {
            // The private marker selects between four kitty keyboard protocol
            // sequences and, with no marker at all, ANSI.SYS restore-cursor.
            switch (csi_private_) {
                case '?': // query the flags currently in effect
                    grid.queue_reply("\x1b[?" + std::to_string(grid.kbd_flags()) + "u");
                    break;
                case '=': // set: Ps2 chooses assign (1), or (2), and-not (3)
                    grid.kbd_set_flags(get_param(0, 0), get_param(1, 1));
                    break;
                case '>': // push a new flags entry (an app entering its mode)
                    grid.kbd_push_flags(get_param(0, 0));
                    break;
                case '<': // pop entries (an app leaving it)
                    grid.kbd_pop_flags(get_count_param(0, 1));
                    break;
                default:
                    grid.restore_cursor();
                    break;
            }
            break;
        }
        default:
            break;
    }
}
