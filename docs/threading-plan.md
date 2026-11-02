# Plan: parse terminal output off the drawing thread

Status: proposed, 2026-11-01. Not started. Intended for after 1.0.

## Why

sink parses everything it reads from a shell on its main thread, the same
thread that draws. Two things follow from that, and the benchmarks measured
both:

- **A query that arrives while a frame is being drawn waits for the frame.**
  With a video background and the CRT effect, 21 of 100 device-attributes
  queries took over 1 ms, the slowest 7.2 ms. With a solid color background,
  none did, and the median beat Alacritty's (`bench/results/term-sink-*`,
  2026-11-01). Programs ask the terminal things constantly: vim and tmux at
  startup, prompts asking where the cursor is, keyboard-protocol probes.
- **During a flood, parsing and drawing take turns.** Each frame spends up to
  8 ms parsing (`kFrameParseBudgetNs`) before it draws, which is why the
  budget exists at all: it caps throughput (`pty_paced.*`) to keep frames
  interactive.

Alacritty parses on the thread that reads the pty, and Ghostty uses a
dedicated I/O thread. In both, a terminal's answer doesn't depend on drawing.

## How we'll know it worked

Each of these is checked with the existing suite. "Before" numbers are
captured in phase 0, with the same machine and the same window size.

| | measured by | target |
|---|---|---|
| queries with video + CRT on | `termbench.py --size 143x66`, query round trip | 90th percentile under 0.5 ms (today 2.75 ms), none over 2 ms |
| queries with a solid background | same | no worse than today (median 0.07 ms) |
| throughput | `termbench`, `pty.*` | no worse on any workload; expected higher |
| frames during a flood | new `pty_bench` metric (phase 5) | the main thread never waits more than 1 ms for a pane's lock |
| render cost | `render_bench`, interleaved `ab` | no detectable difference |
| behaviour | output fingerprints, all tests, `tmux_compat`, termbench cursor check | identical |
| races | ThreadSanitizer builds of the tests and fuzzers | zero reports |

If the query target can't be met without risking the rest, the project
stops after phase 4 and the switch stays off. Phases 0–2 are worth keeping
regardless.

## How it works today

- **Threads.** Each pane has a pty reader thread (`PTYBridge::read_loop`)
  that only moves bytes into a buffer. Video decoding has its own thread.
  `sinkdemo` runs on its own thread and already takes `Pane::grid_mutex`.
- **The frame** (`SDL_AppIterate`), per pane, in `drain_pane_output()`:
  read the buffer, parse under the 8 ms budget through `sync_output::parse`,
  then send what the parser queued: replies, title, OSC 52 clipboard. Then
  the typewriter tick, then render. Between frames, `wait_for_next_frame()`
  parses small output early so queries don't wait for the next frame.
- **Grid access from the main thread** is everywhere and mostly unlocked.
  `main.cpp` calls 62 different `TerminalGrid` methods (selection,
  search, scrollback, resize, mouse modes, keyboard flags, hyperlinks, the
  input anchor), `prompt_editing.cpp` 19 more, and only 7 places in
  `main.cpp` take `grid_mutex`. One thread owned the grid, so nothing
  needed to.
- **Side effects that must happen on the main thread are already queued**
  on the grid and collected by the frame: replies, title, clipboard text,
  working directory. Image textures are created at draw time, not in the
  parser. This part needs no redesign.
- **Lifetimes.** `Pane` declares `terminal`, `pty`, `parser`, in that order.
  Members are destroyed in reverse, so `parser` goes first, while the
  reader thread is joined later in `pty`'s destructor. Harmless while the
  reader never touches the parser; a use-after-free on every pane close
  once it does.

## The design

**Parse on each pane's reader thread, under that pane's lock, in slices.**

- The reader reads, takes the pane's lock, parses up to one slice (64 KB,
  `sync_output::kSliceBytes`), releases it, and repeats until the buffer is
  empty. Replies are written to the pty straight from the reader thread, as
  soon as the slice that produced them is parsed.
- The main thread takes the same lock whenever it touches that pane's grid,
  which is in rendering, event handling, selection, search and resize.
- After each slice, the reader marks the pane dirty and wakes the main
  thread, which draws at its normal cadence.
- The 8 ms parse budget goes away. Parsing no longer costs frame time; the
  slice size bounds how long the main thread can wait for the lock.

**Decisions, and why:**

- **One mutex per pane, not a grid copy per frame.** Copying a grid with
  10,000 lines of scrollback every frame costs more than it saves, and
  render only needs the lock for the CPU half of a frame, ~0.3 ms
  (`render.*.cpu`). The GPU half runs after the lock is released.
- **Fairness is explicit.** `std::mutex` doesn't promise the waiting thread
  gets it next, so a flooding parser could keep reacquiring it. The main
  thread sets an atomic "waiting" flag before locking, and the parser checks
  it between slices and yields. Phase 4 measures this, rather than assuming
  it.
- **Synchronized output keeps its meaning.** `sync_output::parse` already
  holds the bytes of an open update until it closes, so the grid only ever
  shows finished updates, whichever thread parses. The 1 s abandon timeout
  needs a clock tick on the reader side: its poll timeout (100 ms) provides
  one.
- **The typewriter stays on the frame cadence.** It works by parsing a few
  characters per frame, which is the effect itself. The reader applies the
  existing heuristics (chunk size, escapes, burst detection) and either
  parses or appends to `animation_buffer` under the lock. The frame's tick
  parses from that buffer as today.
- **A switch, until it's proven.** `SINK_THREADED_PARSER=1` in development,
  and a hidden `sink.toml` key during the soak, so the old path stays one
  setting away until it's removed in phase 6.

**Not in this project:**
- lock-free or double-buffered grids
- parsing one pane on several threads
- moving rendering off the main thread (SDL requires it there)
- anything that changes what the parser produces

## Order of operations

Each phase is its own commit, or a few, and leaves sink shippable: the suite
passes, fingerprints are unchanged, and nothing is half-switched. Phases 0–2
change nothing users can see.

### Phase 0: tooling and baseline (small)

1. A ThreadSanitizer build: `cmake -B build-tsan -DSINK_TSAN=ON`, applying
   `-fsanitize=thread` to the app, tests and benches, mirroring
   `build-asan`.
2. Run `ctest` (including the integration tests) and the fuzzers
   (`tests/fuzz/`) under it **on today's code**. Anything found is a real
   race already, most likely around sinkdemo or pty shutdown, and gets
   fixed here, so later reports belong to new work.
3. Baseline, plugged in and quiet:
   - `bench/run.py aa`, then `bench/run.py run`
   - `termbench.py --size 143x66` three times in sink with your video and
     CRT on, three times with a solid background, and once each in
     Alacritty and Zed
   - all committed to `bench/results/`

**Done when:** TSan is clean on today's code and the baseline is committed.

**Progress, 2026-11-01:** steps 1 and 2 are done. `SINK_TSAN` is in
CMakeLists.txt. Under it, all 11 test suites pass with zero reports, as do
`pty_bench` (reader threads under a flood) and `render_bench`'s video scene
(the decode thread). A deliberate race in a scratch program was caught and
written to its report file, so the clean result is real. Two things it
can't see:
- **sinkdemo's race.** Its thread parses into a pane's grid under
  `grid_mutex`, and rendering takes that lock too, but the main thread's
  event handling (selection, scrolling, typing) touches the same grid
  without it. Nothing automated runs sinkdemo, so no test reaches this.
  Phase 1's locked grid access fixes it by construction.
- **Races inside vendored libraries** (SDL, FFmpeg), which aren't
  instrumented.

Step 3, the baseline, needs a plugged-in, quiet machine and termbench runs
in each terminal.

### Phase 1: one way to reach a grid (medium; most of the edits)

1. Make `Pane::terminal` and `Pane::parser` private. Access goes through a
   lock guard: `Pane::Locked l = pane.lock();` gives `l.grid()` and
   `l.parser()` for as long as `l` lives. Code that doesn't hold the lock
   can't name the grid, so the compiler finds every access.
2. Convert every access: per event handler or per frame section, not per
   call, aiming for 20–30 lock scopes. This covers `main.cpp`,
   `prompt_editing.cpp`, `sink_demo.cpp` (which already locks, so switch it
   to the guard), and the benches and tests.
3. Watch for code that calls out while holding the lock (writing to the
   pty, SDL calls, settings changes). Fine while uncontended; in phase 3 a
   blocking call under the lock stalls that pane's parsing. Move such calls
   after the scope where it's easy, and note the rest.

**Done when:**
- it builds with the grid private
- termbench in the app shows no change against phase 0. The benchmark
  binaries drive `TerminalGrid` directly and don't compile `main.cpp`, so an
  `ab` of this phase would compare identical code; termbench is the only
  measurement that runs it. The locks are uncontended, at tens of
  nanoseconds each, and none is in a per-byte path.
- tests are unchanged and TSan is clean

**Progress, 2026-11-02:** done, pending your termbench run and a feel check.
- `Pane`'s grid and parser are private, reached through `pane.lock()`,
  which guards a `std::recursive_mutex`, so the compiler found every access:
  about 150 across `main.cpp` and `sink_demo.cpp`, which fixes sinkdemo's
  race.
- Single calls became `pane.lock()->f()`. Read-then-act sequences hold one
  guard:
  - deleting a selection in the prompt
  - cut
  - click-to-move-the-cursor
  - the Return/Ctrl-D submit decision
  - double- and triple-click selection
  - drag-selection
- One reference into the grid outlived its guard (a hyperlink's URI) and
  is now a copy.
- pty writes stay outside the guards.
- All 11 suites pass, under TSan too, with zero reports.

### Phase 2: lifetimes (small, bug-prone)

1. Give `Pane` an explicit `stop()` that shuts the pty down (joining the
   reader) before anything else is destroyed, and call it from every path
   that frees a pane:
   - closing a pane
   - closing a tab or window
   - quit (`SDL_AppQuit`, including the Cocoa run-loop case fixed in
     b27338d)
   - failed window creation (7712128)
2. Reorder `Pane`'s members so the reader's state outlives the reader
   regardless.
3. Recreating the renderer for HDR text doesn't touch the grid, but it
   frees the FontManager's textures, so check that no glyph or image
   texture pointer stays cached on the grid side.
4. A test: open a pane, flood it, close it mid-flood, 1,000 times, under
   TSan and ASan.

**Done when:** that test is clean under both sanitizers.

**Progress, 2026-11-02:**
- **The destructor.** `~Pane()` now stops the pty reader before any member
  is destroyed. A destructor body runs first, so every way a pane is freed
  is covered, including ones that don't call `shutdown()`. The rule that
  comes with it: never free a pane while holding its lock.
- **Member order.** `pty` is already declared after the grid and parser
  (phase 1), so they outlive it. The state declared after `pty` (pending
  output, the sync hold, the typewriter buffer) is exactly what the
  destructor protects.
- **The test.** `tests/test_pane_lifetime.cpp` (ctest `pane_lifetime`,
  integration label) frees 1,000 panes mid-flood, every tenth with its
  reader stalled on backpressure, under ThreadSanitizer and
  AddressSanitizer.
  - **Its limit today:** the reader touches only `PTYBridge`'s own buffer,
    so the test would pass even without the destructor. It's the guard for
    phase 3. Once the reader parses, prove the test has teeth by removing
    `~Pane()` once and watching it fail under ASan.
- **HDR renderer recreation** already releases inline-image textures
  (`release_textures()`, keeping the pixels), and glyph textures live in
  the per-window FontManager, not the grid. Nothing to fix.
- **Found for phase 3:** the image store destroys GPU textures from inside
  the parser, when a kitty delete command arrives and when old images are
  evicted at the memory cap (`TerminalImages::drop_image()` and the
  eviction loop). After phase 3 that would run on the reader thread, and
  SDL textures may only be destroyed on the render thread. Phase 3 step 0,
  below.
- **Noted, not changed:** closing a pane takes ~50 ms, because
  `PTYBridge::shutdown()` gives the shell 50 ms to exit before SIGKILL.
  Closing a window with four panes takes ~200 ms.

### Phase 3: parse on the reader thread, behind the switch (medium)

0. Make the image store defer freeing GPU textures. Textures of deleted
   or evicted images go on a list the frame frees, instead of being
   destroyed inside the parser (found in phase 2).
1. `PTYBridge` gets an output handler, called on the reader thread with each
   chunk. With the switch on, the handler is the pane's:
   - lock, then `sync_output::parse` in slices, yielding between slices
     when the main thread is waiting
   - write replies straight to the pty
   - mark the pane dirty and signal `g_pty_wake`
2. The typewriter decision moves into the handler as described above.
3. With the switch on, the frame skips parsing in `drain_pane_output()`
   entirely, and collects only what the grid queued for the main thread:
   title, clipboard, working directory. `wait_for_next_frame()` keeps waking
   on the semaphore, but has nothing to parse.
4. With the switch off, everything is exactly as before.

**Done when:**
- with the switch on, all tests pass, `tmux_compat` agrees, the termbench
  cursor check agrees with Alacritty and Zed, and TSan is clean
- you've used it for a day without noticing anything

**Progress, 2026-11-02:** built behind `SINK_THREADED_PARSER=1`.
- **Step 0, image textures.** The image store no longer destroys GPU
  textures inside the parser. Deleted, evicted and reset images retire
  their textures to a list that `TerminalGrid::render()` frees. Only the
  HDR renderer swap and the destructor still destroy directly, both on the
  main thread.
- **The parse path is out of `main.cpp`.** The typewriter-or-parse decision
  (`take_output`) and the reader-thread handler (`parse_on_reader`) now
  live in `src/pane_output.*`, so tests run the code the app runs.
- **`PTYBridge`.**
  - An output handler is called on the reader thread with each chunk
    (64 KB at most), and with nothing every 100 ms so a held synchronized
    update can time out.
  - With a handler, the pty is non-blocking.
  - `write_to_pty` takes a write lock, so a reply can't land inside a
    paste.
  - `write_without_waiting` never blocks, neither on the lock nor on the
    shell's input.
- **Replies.** Only the reader sends them, in order, and never with the
  pane's lock held. Whatever can't be written at once waits in
  `Pane::reply_backlog` for the reader's next turn, at most 100 ms later.
  This also removes a deadlock that exists today: a huge paste into a
  program that's busy printing could stall both the main thread and the
  shell.
- **Window state the reader needs** (typewriter on, sinkdemo owning the
  pane) is mirrored into per-pane atomics by the frame.
- **The frame, with the switch on,** doesn't parse. It collects the title
  and OSC 52 clipboard writes, and runs the typewriter tick as before.
- **Tests.** `pane_lifetime_threaded` frees panes mid-flood while their
  readers parse and the test thread reads the grid like a renderer. With
  `~Pane()` removed, AddressSanitizer reports a heap-use-after-free in
  `TerminalGrid::write_run` called from `parse_on_reader`: the test has
  teeth, as phase 2 required.

### Phase 4: fairness and frame consistency (small–medium, measured)

1. Implement and measure the waiting flag. New `pty_bench` mode: a flood
   into a pane while another thread renders it every 16.7 ms, reporting how
   long each render waited for the lock. Target: worst under 1 ms.
2. Check what a frame can show mid-flood: a slice boundary can fall in the
   middle of a screen update that isn't synchronized. Today a frame boundary
   can too, so this is no worse, but confirm it with the `sync_tui` and
   `cursor_heavy` workloads and the snapshot frames.
3. If the lock wait misses its target, shrink the slice (16 KB) and
   re-measure. If it still misses, stop here and decide.

**Done when:** the lock-wait target is met and recorded in a results file.

**Progress, 2026-11-02:** measured first, and the fairness mechanism turned
out not to be needed.
- **The new mode.** `pty_bench` gained `pty_threaded.<w>`: the app's own
  `Pane` and `parse_on_reader` parse a flood through a real pty, while the
  bench thread locks the pane and reads the whole screen every 4 ms.
- **The control.** `pty_threaded.idle.lock_wait` runs the same probes with
  a child that prints nothing, which gives the probe's own noise.

| | median of each run's worst wait | worst seen |
|---|---|---|
| control (no output) | 0.004 ms | 1.74 ms (the scheduler, not the parser) |
| plain text | 0.033 ms | 0.57 ms |
| TUI redraws | 0.032 ms | 0.11 ms |
| synchronized TUI | 0.049 ms | 0.15 ms |
| sixel + kitty images | 0.097 ms | 0.13 ms |

Why the waits are so short:
- the pty hands over output in small reads, so one parse holds the lock
  only briefly
- macOS mutexes hand the lock to waiters in order by default, so the
  reader can't keep taking it back

The millisecond outliers appear in the control too, so they're the probe
thread being descheduled. Neither the waiting flag nor smaller slices
would change them, so neither was added.

**Behaviour.** The threaded parser leaves exactly the grid in-memory parsing
does: `pty_threaded.*` fingerprints equal the blessed `parser.*` ones on all
four workloads. Synchronized updates are still held whole, by
`sync_output::parse`, so a frame mid-flood can show a partly written
unsynchronized redraw, as it could at a frame boundary before. Checking
that by eye is part of your phase 3 trial.

**Not covered:** a single very large image (a 4K photo through
`sinkview`) is decoded inside the parse, so the renderer would wait for the
whole decode. That's no worse than today, where the same decode blocks the
entire frame, but if it shows up, decoding outside the lock is the fix.

### Phase 5: benchmarks and claims (small)

1. `pty_bench` gains a threaded mode that mirrors the app, so "measure what
   ships" still holds. The paced/budget mode stays for the old path until
   phase 6 removes it.
2. Full suite plus termbench in all three terminals, at the phase 0
   settings, with an interleaved `ab` of switch on against switch off.
3. Update `bench/claims.json`:
   - the 9 ms worst-frame parse claims no longer apply
   - add query-latency and lock-wait claims, judged from results files
     like the rest
4. A fuzzer variant that parses on one thread while another renders the
   grid (headless), under TSan, so races in the parser–renderer boundary
   are searched for rather than waited for.

**Done when:** the success table above is filled in from committed results.

### Phase 6: soak and cleanup (calendar time, little code)

1. Switch on by default in a beta. A week or two of your own daily use,
   plus whoever runs the betas.
2. Then remove the old path:
   - the per-frame parse budget and `kFrameParseBudgetNs`
   - the parse half of `drain_pane_output()`
   - `wait_for_next_frame()`'s service pass
   - the switch
3. Update `bench/README.md`'s parse-budget section, the comments that
   describe one thread owning the grid, and the project memory.

## Size

- Roughly 4–6 working sessions, and 600–1,200 lines changed.
- Phase 1 has most of the edits. Phases 2 and 4 have most of the risk.
- Then the soak, which is calendar time rather than work.
- Phase 0's TSan build is useful on its own and could happen before 1.0.

## Risks

| risk | where | mitigation |
|---|---|---|
| a grid access missed while unlocked | phase 1 | the grid is private, so the compiler finds them; TSan |
| use-after-free when a pane closes mid-parse | phase 2 | explicit `stop()`, member order, the close-under-flood test |
| the renderer starves during a flood | phase 4 | the waiting flag, slice size, a measured target |
| the typewriter feels different | phase 3 | same heuristics, same frame tick; your call |
| deadlock: the reader holds the lock and blocks writing a reply while the shell blocks writing output | phase 3 | write replies after releasing the lock |
| timing-dependent bugs nobody can reproduce | phases 3–6 | TSan on tests and fuzzers, the concurrent fuzzer, the switch, the soak |

## Open questions for Kady

1. Should arrow-key and other escape echoes stay outside the typewriter
   animation? They already are, since the 2026-11-01 query fix, and the
   threaded design keeps that.
2. Is a hidden `sink.toml` switch fine for the soak, or should it be
   visible in Settings?
3. Should the TSan build and baseline (phase 0) happen before 1.0?
