#include "ansi_parser.hpp"
#include <iostream>
#include <sstream>
#include <algorithm>

ANSIParser::ANSIParser() {}

ANSIParser::~ANSIParser() {}

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
                // Line feed: move cursor down (scrolling if it hits bottom)
                int cur_row = grid.get_cursor_row();
                if (cur_row >= grid.get_rows() - 1) {
                    grid.scroll_up();
                } else {
                    grid.set_cursor_row(cur_row + 1);
                }
            } else if (c == '\r') {
                // Carriage return: move cursor to start of line
                grid.set_cursor_col(0);
                grid.set_prompt_boundary(-1);
            } else if (c == '\b') {
                // Backspace: move cursor left one cell
                grid.set_cursor_col(grid.get_cursor_col() - 1);
            } else if (c == '\x07') {
                // Bell: Ignore audio alerts
            } else if (c == '\t') {
                // Tab: Move to next tab stop (multiples of 8)
                int next_tab = (grid.get_cursor_col() + 8) & ~7;
                grid.set_cursor_col(next_tab);
            } else {
                // Standard printable character
                grid.write_character(c);

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
            break;
        }
        case STATE_ESCAPE: {
            if (c == '[') {
                state_ = STATE_CSI;
            } else {
                state_ = STATE_NORMAL;
            }
            break;
        }
        case STATE_CSI: {
            if (c == '?') {
                is_private_mode_ = true;
            } else if (c >= '0' && c <= '9') {
                csi_buffer_ += static_cast<char>(c);
            } else if (c == ';') {
                if (csi_buffer_.empty()) {
                    csi_params_.push_back(0);
                } else {
                    csi_params_.push_back(std::stoi(csi_buffer_));
                    csi_buffer_.clear();
                }
            } else if (c >= 0x40 && c <= 0x7E) {
                if (!csi_buffer_.empty()) {
                    csi_params_.push_back(std::stoi(csi_buffer_));
                }
                process_csi_sequence(grid, static_cast<char>(c));
                state_ = STATE_NORMAL;
            }
            break;
        }
    }
}

void ANSIParser::process_csi_sequence(TerminalGrid& grid, char command) {
    auto get_param = [&](size_t index, int default_val) {
        if (index < csi_params_.size()) {
            return csi_params_[index];
        }
        return default_val;
    };

    switch (command) {
        case 'm': { // Select Graphic Rendition (SGR)
            if (csi_params_.empty()) {
                grid.set_current_fg({0.9f, 0.9f, 0.9f, 1.0f});
                grid.set_current_bg({0.0f, 0.0f, 0.0f, 0.0f});
                break;
            }
            
            // Standard ANSI 16-color palette
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

            for (size_t i = 0; i < csi_params_.size(); ++i) {
                int param = csi_params_[i];
                if (param == 0) {
                    // Reset styling
                    grid.set_current_fg({0.9f, 0.9f, 0.9f, 1.0f});
                    grid.set_current_bg({0.0f, 0.0f, 0.0f, 0.0f});
                } else if (param >= 30 && param <= 37) {
                    grid.set_current_fg(ansi_colors[param - 30]);
                } else if (param >= 40 && param <= 47) {
                    grid.set_current_bg(ansi_colors[param - 40]);
                } else if (param >= 90 && param <= 97) {
                    grid.set_current_fg(ansi_colors[param - 90 + 8]);
                } else if (param >= 100 && param <= 107) {
                    grid.set_current_bg(ansi_colors[param - 100 + 8]);
                } else if (param == 39) {
                    // Default foreground color
                    grid.set_current_fg({0.9f, 0.9f, 0.9f, 1.0f});
                } else if (param == 49) {
                    // Default background color
                    grid.set_current_bg({0.0f, 0.0f, 0.0f, 0.0f});
                }
            }
            break;
        }
        case 'H':
        case 'f': { // Cursor Position (CUP)
            int row = get_param(0, 1) - 1;
            int col = get_param(1, 1) - 1;
            grid.set_cursor_row(row);
            grid.set_cursor_col(col);
            break;
        }
        case 'A': { // Cursor Up (CUU)
            int offset = get_param(0, 1);
            grid.set_cursor_row(grid.get_cursor_row() - offset);
            break;
        }
        case 'B': { // Cursor Down (CUD)
            int offset = get_param(0, 1);
            grid.set_cursor_row(grid.get_cursor_row() + offset);
            break;
        }
        case 'C': { // Cursor Forward (CUF)
            int offset = get_param(0, 1);
            grid.set_cursor_col(grid.get_cursor_col() + offset);
            break;
        }
        case 'D': { // Cursor Backward (CUB)
            int offset = get_param(0, 1);
            grid.set_cursor_col(grid.get_cursor_col() - offset);
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
        case 'h': { // Set Mode (SM / DECSET)
            if (is_private_mode_ && get_param(0, 0) == 1049) {
                grid.set_alt_screen(true);
            }
            break;
        }
        case 'l': { // Reset Mode (RM / DECRST)
            if (is_private_mode_ && get_param(0, 0) == 1049) {
                grid.set_alt_screen(false);
            }
            break;
        }
        default:
            break;
    }
}
