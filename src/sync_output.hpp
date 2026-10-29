#pragma once

// Synchronized output (DECSET 2026) lets a program mark a redraw as one
// update, so it's never seen half done. sink holds the program's *output*
// rather than the picture: bytes after the opening sequence wait, unparsed,
// until the closing one arrives, and then the whole update is parsed at once.
// The window keeps presenting every frame meanwhile -- the background video
// and cursor carry on, and the text shows the last finished update. (Holding
// presents instead froze the whole window, background and all.) kitty works
// the same way.
//
// Only the plain sequences are recognised in the byte stream. 2026 set
// together with other modes ("CSI ? 2026 ; 1049 h") still turns the mode on in
// the parser, and output is held from the end of that parse slice on.

#include <cstddef>
#include <cstdint>
#include <functional>

class ANSIParser;
class TerminalGrid;

namespace sync_output {

// Longest one update is held before sink decides it was abandoned and shows
// what it has, as Ghostty does after a second. A program killed mid-update
// (Ctrl+C on hollywood) never sends the closing sequence. A program that asks
// the terminal something inside an update and waits for the answer before
// closing it also waits this long, as it would in kitty.
constexpr uint64_t kMaxUpdateMs = 1000;

// Most output one update may hold. Past this it's parsed as it comes rather
// than held. Real updates are a screenful -- tens of kilobytes.
constexpr size_t kMaxUpdateBytes = 4u << 20;

// How long one frame may spend parsing before the rest waits for the next
// frame. Why 8ms, and what it costs in throughput, is in main.cpp beside
// parse_within_budget(). It lives here so bench/pty_bench drains with the
// app's number rather than a copy of it.
constexpr uint64_t kFrameParseBudgetNs = 8ull * 1000000ull;

// Most parsed in one step outside an update, so `out_of_time` is asked often.
constexpr size_t kSliceBytes = 64u << 10;

// Per pane: the update last seen open and when it was first seen. Updates are
// told apart by TerminalGrid::synchronized_updates(), so a stream of short
// ones never looks like one long one.
struct Hold {
    uint64_t update_seen = 0;
    uint64_t since_ms = 0;
};

// Parses `data` until it runs out, `out_of_time()` says to stop, or an
// update is open and its end hasn't arrived; returns how many bytes were
// parsed. The caller keeps the rest and passes it again with whatever arrives
// next. A complete update is parsed in one step whatever the time, so it
// lands in one frame.
size_t parse(ANSIParser& parser, TerminalGrid& grid, Hold& hold,
             const char* data, size_t size, uint64_t now_ms,
             const std::function<bool()>& out_of_time);

} // namespace sync_output
