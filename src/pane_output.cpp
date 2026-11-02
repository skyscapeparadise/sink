// What happens to a pane's pty output: the typewriter-or-parse decision, and,
// with the threaded parser, parsing and replying on the pty reader thread.
// Out of main.cpp so tests and benchmarks run this code rather than a copy of
// it. See pane_output.hpp and docs/threading-plan.md.
#include "pane_output.hpp"

#include "sync_output.hpp"

#include <SDL3/SDL.h>
#include <cstring>
#include <string>

namespace pane_output {

// The typewriter-or-parse decision for output that has just arrived, shared by
// the frame (drain_pane_output) and, with the threaded parser, the reader
// thread (parse_on_reader). The pane's lock is held through `g`. Returns how
// many bytes of `data` were taken -- parsed, or queued for the typewriter; the
// caller keeps the rest for next time.
size_t take_output(Pane& pane, const Pane::Locked& g, const char* data, size_t size,
                   bool typing, const std::function<bool()>& out_of_time) {
    size_t consumed = 0;
    if (typing) {
        // A human at the keyboard produces isolated chunks (a keystroke's
        // echo) tens-to-hundreds of ms apart. A program streaming
        // continuous screen updates (cacademo, htop, vim, ...) produces
        // many chunks back-to-back with almost no gap between them, even
        // when each individual chunk happens to be small -- a single
        // chunk-size check can't tell those apart. Track the gap between
        // consecutive chunks and once several land faster than typing
        // plausibly could, treat the whole burst as program output so
        // the typewriter pacing doesn't make rendering fall behind it.
        Uint64 now = SDL_GetTicks();
        Uint64 gap = now - pane.last_output_chunk_time;
        pane.last_output_chunk_time = now;
        if (gap < 40) {
            pane.rapid_chunk_streak++;
        } else {
            pane.rapid_chunk_streak = 0;
        }
        bool fast_turnover = pane.rapid_chunk_streak >= 3;

        // Output inside a synchronized update is held, not typed out.
        // A chunk with an escape in it isn't keyboard echo -- it's a
        // program moving the cursor, or asking the terminal something
        // and waiting for the answer. Queued for the typewriter, a
        // 3-byte query (ESC [ c) was parsed after this pass's replies
        // went out and answered a frame late.
        const bool has_escape = std::memchr(data, 0x1b, size) != nullptr;
        if (size > 5 || fast_turnover || has_escape || g->is_synchronized_output()) {
            // Large chunk / fast turnover (command or program output): bypass typing effect
            if (!pane.animation_buffer.empty()) {
                g.parse(pane.animation_buffer.data(), pane.animation_buffer.size());
                pane.animation_buffer.clear();
            }
            consumed = sync_output::parse(g.parser(), g.grid(), pane.sync_hold, data, size,
                                          SDL_GetTicks(), out_of_time);
        } else {
            // Small, unhurried chunk (user typing): queue for animated
            // typing, which does its own pacing from there.
            pane.animation_buffer.insert(pane.animation_buffer.end(), data, data + size);
            consumed = size;
        }
    } else {
        consumed = sync_output::parse(g.parser(), g.grid(), pane.sync_hold, data, size,
                                      SDL_GetTicks(), out_of_time);
    }
    g->note_output_idle();
    return consumed;
}

// With the threaded parser, each pane's pty reader thread calls this with
// every chunk it reads, and with nothing every 100ms (PTYBridge's output
// handler). The output is parsed here, on the reader thread, under the pane's
// lock, so a program asking the terminal something gets its answer while the
// main thread is busy drawing, and parsing a flood no longer takes turns with
// drawing. See docs/threading-plan.md, phase 3.
//
// Replies are sent from here, and only from here, so they leave in the order
// they were made, and never with the lock held. A reply that can't be written
// without waiting stays in reply_backlog for the next call: waiting here
// while the shell waited on its output would stop both.
void parse_on_reader(Pane& pane, const char* data, size_t size) {
    std::string reply;
    {
        auto g = pane.lock();
        if (size > 0) pane.pty_pending.insert(pane.pty_pending.end(), data, data + size);
        if (pane.demo_owned.load(std::memory_order_relaxed)) {
            pane.pty_pending.clear();  // a demo has the pane; its output is dropped, as in the frame
        } else if (!pane.pty_pending.empty()) {
            const auto never = []() { return false; };
            size_t used;
            if (size > 0) {
                used = take_output(pane, g, pane.pty_pending.data(), pane.pty_pending.size(),
                                   pane.typing_animation.load(std::memory_order_relaxed), never);
            } else {
                // Nothing new: only a held synchronized update is left, and
                // the clock may have run out on it (sync_output::kMaxUpdateMs).
                used = sync_output::parse(g.parser(), g.grid(), pane.sync_hold, pane.pty_pending.data(),
                                          pane.pty_pending.size(), SDL_GetTicks(), never);
            }
            pane.pty_pending.erase(pane.pty_pending.begin(), pane.pty_pending.begin() + static_cast<long>(used));
        }
        if (g->has_pending_reply()) pane.reply_backlog += g->take_pending_reply();
        reply.swap(pane.reply_backlog);
    }
    if (reply.empty()) return;
    const size_t sent = pane.pty.write_without_waiting(reply.data(), reply.size());
    if (sent < reply.size()) {
        auto g = pane.lock();
        pane.reply_backlog.insert(0, reply, sent, std::string::npos);  // ahead of anything newer
    }
}

void attach_reader_parser(Pane& pane) {
    Pane* p = &pane;
    pane.pty.set_output_handler([p](const char* data, size_t size) { parse_on_reader(*p, data, size); });
}

} // namespace pane_output
