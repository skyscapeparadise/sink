#pragma once

#include <vector>
#include <string>
#include "terminal_grid.hpp"

enum ParserState {
    STATE_NORMAL,
    STATE_ESCAPE,
    STATE_CSI
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
    std::string trigger_buffer_;
    bool is_private_mode_ = false;

    // UTF-8 state variables to parse multi-byte characters
    int utf8_bytes_needed_ = 0;
    char32_t utf8_codepoint_ = 0;

    // Helper to process individual decoded UTF-8 codepoints
    void process_char(TerminalGrid& grid, char32_t c);
    void process_csi_sequence(TerminalGrid& grid, char command);
    void reset_csi();
};
