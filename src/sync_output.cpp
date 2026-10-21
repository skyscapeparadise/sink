#include "sync_output.hpp"

#include "ansi_parser.hpp"
#include "terminal_grid.hpp"

#include <algorithm>
#include <string_view>

namespace sync_output {

namespace {

constexpr std::string_view kBegin = "\x1b[?2026h";
constexpr std::string_view kEnd = "\x1b[?2026l";

// Where `needle` ends in `data`, or 0 if it isn't there.
size_t end_of(std::string_view data, std::string_view needle) {
    const size_t at = data.find(needle);
    return at == std::string_view::npos ? 0 : at + needle.size();
}

bool expired(const TerminalGrid& grid, Hold& hold, uint64_t now_ms) {
    const uint64_t update = grid.synchronized_updates();
    if (update != hold.update_seen) {
        hold.update_seen = update;
        hold.since_ms = now_ms;
        return false;
    }
    return now_ms - hold.since_ms >= kMaxUpdateMs;
}

} // namespace

size_t parse(ANSIParser& parser, TerminalGrid& grid, Hold& hold,
             const char* data, size_t size, uint64_t now_ms,
             const std::function<bool()>& out_of_time) {
    size_t pos = 0;
    while (pos < size) {
        const std::string_view rest(data + pos, size - pos);
        if (grid.is_synchronized_output()) {
            const size_t update = end_of(rest, kEnd);
            if (update > 0 && update <= kMaxUpdateBytes) {
                parser.parse(grid, rest.data(), update);
                pos += update;
            } else if (update == 0 && rest.size() <= kMaxUpdateBytes && !expired(grid, hold, now_ms)) {
                break;  // still open: hold the rest
            } else {
                // Abandoned, or too big to hold: show it as it comes.
                grid.set_synchronized_output(false);
                continue;
            }
        } else {
            // Up to and including the next opening sequence, so nothing after
            // it is parsed before it's held.
            const size_t open = end_of(rest, kBegin);
            const size_t take = std::min(kSliceBytes, open > 0 ? open : rest.size());
            parser.parse(grid, rest.data(), take);
            pos += take;
        }
        if (out_of_time()) break;
    }
    return pos;
}

} // namespace sync_output
