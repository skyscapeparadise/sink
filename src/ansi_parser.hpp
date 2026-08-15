#pragma once

#include <vector>
#include <string>
#include "terminal_grid.hpp"

enum ParserState {
    STATE_NORMAL,
    STATE_ESCAPE,
    STATE_CSI,
    STATE_STR,      // OSC/DCS/APC/PM/SOS payload: consumed and discarded until a terminator
    STATE_STR_ESC,  // saw ESC while inside a string sequence; next byte decides if it's ST ('\')
    STATE_CHARSET   // saw ESC ( / ) / * / + ; next byte designates a character set
};

class ANSIParser {
public:
    ANSIParser();
    ~ANSIParser();

    // Parse bytes and apply text content, scrolls, cursor movements, and formatting to the grid
    void parse(TerminalGrid& grid, const char* data, size_t size);

private:
    ParserState state_ = STATE_NORMAL;
    std::vector<int> csi_params_;
    std::string csi_buffer_;
    // Sliding window of the last kTriggerBufSize printable chars, scanned
    // for "error"/"failed" to trigger the error-flash effect. A fixed
    // array shifted via memmove, not std::string: this runs on literally
    // every printable character parsed, and std::string::substr() here
    // was heap-allocating + copying on almost every call once the window
    // filled -- by far the hottest allocation in the whole parse path.
    static constexpr int kTriggerBufSize = 32;
    char trigger_buffer_[kTriggerBufSize] = {};
    int trigger_len_ = 0;

    bool is_private_mode_ = false;

    // Base-palette (SGR 30-37) foreground index currently in effect, or -1
    // for default/truecolor/explicit-bright. Needed so bold can brighten the
    // color whether SGR 1 arrives before or after the color parameter.
    int fg_base_index_ = -1;

    // Character-set designation (SCS). Only G0 via ESC ( affects rendering;
    // designations for the other banks are parsed but ignored.
    char charset_designator_ = 0;
    bool g0_dec_graphics_ = false;

    // OSC payload accumulation. Only OSC strings are kept (DCS/APC/PM/SOS
    // are still consumed and discarded); the buffer is capped so a hostile
    // stream can't grow it without bound.
    bool str_is_osc_ = false;
    std::string osc_buffer_;
    static constexpr size_t kOscMaxLen = 4096;
    void dispatch_osc(TerminalGrid& grid);

    // UTF-8 state variables to parse multi-byte characters
    int utf8_bytes_needed_ = 0;
    char32_t utf8_codepoint_ = 0;

    // Helper to process individual decoded UTF-8 codepoints
    void process_char(TerminalGrid& grid, char32_t c);
    void process_csi_sequence(TerminalGrid& grid, char command);
    void reset_csi();
};
