#pragma once

// What happens to a pane's pty output once it has been read. See
// docs/threading-plan.md.

#include "app_state.hpp"

#include <cstddef>
#include <functional>

namespace pane_output {

// The typewriter-or-parse decision for output that has just arrived: a
// keystroke's echo is queued for the typewriter animation (when `typing`),
// anything else is parsed, through sync_output::parse, until `out_of_time`.
// The pane's lock must be held, through `g`. Returns how many bytes of `data`
// were taken; the caller keeps the rest for next time.
size_t take_output(Pane& pane, const Pane::Locked& g, const char* data, size_t size,
                   bool typing, const std::function<bool()>& out_of_time);

// The threaded parser: parses `data` (or, given nothing, lets go of a
// synchronized update held too long), then writes the replies parsing
// produced without ever waiting to. Runs on the pane's pty reader thread.
void parse_on_reader(Pane& pane, const char* data, size_t size);

// Makes `pane`'s pty reader thread parse its output with parse_on_reader.
// Before the pane's shell is spawned. The pane outlives its reader (~Pane).
void attach_reader_parser(Pane& pane);

} // namespace pane_output
