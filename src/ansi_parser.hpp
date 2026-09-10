#pragma once

#include <vector>
#include <string>
#include <cstdint>
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
    // Cap on parameters kept from one CSI sequence. xterm's own limit is in
    // the same range; anything beyond this is malformed or hostile, and
    // storing it without bound is a memory-exhaustion hole.
    static constexpr size_t kMaxCsiParams = 256;
    // CSI parameters accumulate into an int as digits arrive, rather than into
    // a string that std::stoi then re-parses. That path profiled at ~12% of
    // total parse time -- locale-aware strtol plus the try/catch's exception
    // machinery in the hot loop. Only digits ever reach here (see STATE_CSI in
    // process_char), so this is exactly equivalent.
    int csi_acc_ = 0;
    bool csi_acc_digits_ = false;
    // The eight most recent printable ASCII characters, most recent in the low
    // byte. Only "error" and "failed" are looked for, so six is the most that
    // can ever matter. Unset slots read as zero, which cannot match a letter,
    // so no "have we seen enough characters yet" counter is needed.
    //
    // This exists only to carry state *between* calls. Matching within a run
    // reads the run itself, because only 'r' and 'd' can end a trigger word
    // and so the scan is a byte compare with nothing to maintain per
    // character. The previous shape kept a double-written ring updated on
    // every printable character -- two stores, a rotating index and a compare
    // each -- which sampled at 36% of parse time on plain text, more than
    // writing those characters into the grid cost.
    //
    // A 64-bit register was tried for the *per-character* version once and was
    // slower, because each character's shift depended on the previous one and
    // that serialised the loop. Updating once per run instead removes exactly
    // that dependency, which is what makes the same representation win here.
    static constexpr int kTrigWindow = 8;
    uint64_t trigger_tail_ = 0;

    // CSI private marker (0x3C-0x3F: '<' '=' '>' '?') and intermediate byte
    // (0x20-0x2F: space, '!', '$', ...), 0 when absent.
    //
    // Both used to be dropped on the floor apart from '?', which was kept as a
    // bool. That made "CSI > c" (secondary DA) indistinguishable from "CSI c"
    // (primary), and the space in "CSI Ps SP q" (DECSCUSR) simply vanished --
    // so neither could be answered even in principle.
    char csi_private_ = 0;
    char csi_intermediate_ = 0;
    bool is_private_mode() const { return csi_private_ == '?'; }

    // Last graphic character written, which is what REP (CSI Ps b) repeats.
    // 0 means nothing has been written yet; ECMA-48 leaves that case
    // undefined and repeating a space would quietly corrupt the line, so REP
    // does nothing until there is something to repeat.
    char32_t last_graphic_ = 0;

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
    // Whether the string sequence being read ended with BEL rather than ST.
    // Replies have to echo the terminator back: a client that sent BEL and
    // parses for BEL will sit waiting forever for an ST it does not expect.
    bool str_ended_with_bel_ = false;

    // DCS payloads are captured too now, because sixel arrives in one. Kept
    // separate from the OSC buffer and capped far higher: an OSC carries a
    // title or a URI, a sixel carries a picture, and even a modest one runs to
    // hundreds of kilobytes.
    bool str_is_dcs_ = false;
    std::string dcs_buffer_;
    static constexpr size_t kDcsMaxLen = 8u * 1024 * 1024;
    void dispatch_dcs(TerminalGrid& grid);

    // Kitty graphics protocol, which arrives in an APC (ESC _ G ... ST).
    bool str_is_apc_ = false;
    std::string apc_buffer_;
    static constexpr size_t kApcMaxLen = 8u * 1024 * 1024;
    void dispatch_apc(TerminalGrid& grid);

    // A transmission split across chunks (m=1) accumulates here. Senders chunk
    // by default -- kitty's own client uses 4096-byte pieces -- so this is the
    // normal path for anything bigger than a thumbnail, not an edge case.
    struct KittyTransfer {
        bool active = false;
        std::string controls; // the first chunk's control data governs
        std::string data;     // payload decoded so far
    };
    KittyTransfer kitty_;

    // UTF-8 state variables to parse multi-byte characters
    int utf8_bytes_needed_ = 0;
    char32_t utf8_codepoint_ = 0;

    // Helper to process individual decoded UTF-8 codepoints
    void process_char(TerminalGrid& grid, char32_t c);
    void process_csi_sequence(TerminalGrid& grid, char command);
    void reset_csi();

    // Scans a run of printable ASCII for the error/failed trigger words and
    // carries the tail forward. note_trigger_char() is the single-character
    // case, expressed in terms of it so the per-character and batched paths
    // cannot drift apart.
    void note_trigger_run(TerminalGrid& grid, const char* run, int n);
    void note_trigger_char(TerminalGrid& grid, char32_t c);
};
