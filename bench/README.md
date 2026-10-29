# sink's benchmarks

Numbers here are meant to be ones sink can stand behind: anyone with the repo
can re-run them, every one comes with the raw samples it was derived from, and
each published claim is a test that fails when it stops being true.

```sh
python3 bench/gen_workloads.py       # once: writes the workload files, checks them against the manifest
python3 bench/run.py run             # the suite -> bench/results/<date>-<commit>.json and .md
python3 bench/run.py claims          # re-check bench/claims.json against the latest results
```

Run it plugged in, with nothing else busy. `run` refuses otherwise (on
battery, Low Power Mode, load average over 3, thermal throttling) unless you
pass `--allow-noisy`, and marks the results if you do.

## What's measured

| bench | what | from | to |
|---|---|---|---|
| `sink_bench` | parser and grid | bytes in memory | grid state |
| `pty_bench` | the input pipeline | a real pty and child process | grid state |
| `render_bench` | frames | a filled grid, a background, effects | finished pixels |
| `vte_bench` | the same parser workloads, on `vte` (Alacritty's parser) | bytes in memory | grid state |
| `termbench.py` | any terminal, from outside | a program writing to it | the terminal's answer |

Metric ids, which results files, claims and comparisons all use:

- `parser.<workload>`: MB/s through the path the app uses (`sync_output::parse`, which holds synchronized updates).
- `parser_core.<workload>`: MB/s through `ANSIParser::parse` alone. This is the like-for-like figure against `vte.<workload>`, since vte has nothing like sync_output.
- `findbar.<lines>`: ms for one find-bar keystroke at that scrollback depth.
- `reflow.<lines>`: ms to rewrap everything for one resize step, which a window drag triggers every frame.
- `pty.<workload>`: MB/s through a real pty, drained and parsed as fast as possible.
- `pty_paced.<workload>`: MB/s when draining once per 16.7 ms frame under the app's parse budget, plus `.worst_frame`, the longest any one frame spent parsing.
- `render.<scene>.cpu`: ms of CPU to build a frame.
- `render.<scene>.frame`: ms to finished pixels, GPU included.
- `term.<workload>`, `term.query_latency`: termbench, below.

Workloads: plain text, SGR-heavy colorized output, cursor-heavy TUI redraws,
UTF-8 (emoji/CJK/box drawing), distinct CJK (an atlas stress test), and since
October 2026 also synchronized-output TUI frames, grapheme clusters, OSC 8
links with OSC 133 marks, and sixel and kitty images.

Render scenes: text of each kind, deep scrollback, scrollback with search,
four panes, and each background kind (gradient, gel, shader, 4K picture,
video), the CRT effect, HDR text, and all of it at once.

## Why you can trust a number from here

Each of these exists because its absence produced a wrong number here at some
point.

**Inputs are pinned.** The workload files aren't committed. Their SHA-256s
are, in `workloads.sha256`, and nothing runs on files that don't match. A
seeded generator isn't a promise of identical bytes, because Python could
change what `random.choice` returns for a seed. The first five workloads still
match the files the September 2026 results were measured on.

**Binaries match their source.** `run` builds into `build-bench/`, in Release,
and rebuilds from clean whenever the *content* of the sources changes. make
decides by mtime, and a restored file with an old mtime once left a stale
binary in place, which then contradicted arithmetic. That happened again while
building this suite.

**Output is checked, not just timed.** Every metric fingerprints what it
produced: every cell of scrollback and screen, the cursor, search match
counts. The fingerprints are checked against `expected.json`, and a mismatch
fails the run. A change that makes something faster by making it do less
gets caught, not reported as a speedup. When a change in output is intended,
`run.py bless` accepts the new fingerprints, and `expected.json` gets
committed with the change.

**The benches check each other.**

- The pty path must leave exactly the grid that parsing the same bytes in
  memory does. It must also deliver exactly the file's bytes, no more and no
  fewer.
- `vte_bench` must end on the same screen as sink's parser before a
  comparison counts as like-for-like. When this check was added, it found
  that vte_bench gave U+FE0F (an emoji variation selector) a column of its
  own, so the UTF-8 comparison had never been like-for-like. It also found
  that vte_bench never read its colour fields, which lets the optimizer drop
  the writes and makes that side do less work. Both are fixed.

**Measure what ships.**

- `render_bench` uses the app's renderer (SDL's `gpu` backend; linear
  colorspace for HDR text). It composes frames in `main.cpp`'s order, with
  the app's own background and effect classes, at the app's font size and
  the window's real pixel density. The old version used the Metal 2D backend
  at 1x, which sink doesn't draw with.
- `pty_bench` drains with `sync_output::kFrameParseBudgetNs`, the same
  constant `main.cpp` uses, instead of a copy of it.
- The child is `/bin/cat`, not your login shell, so rc files and prompts
  aren't in the measurement.

**Evidence that frames were drawn.** Each scene is read back after timing. A
frame that comes back as one flat colour fails the run, because a renderer
that silently dropped its work would otherwise look very fast. The pixels'
hash is kept, and the frames are saved as PNGs next to the results. Frames are
never presented while timing: presenting waits for the display even on a
hidden window with vsync off. The first version of this bench measured
8.33 ms for every scene, which was the display's 120 Hz refresh, not sink.

**Statistics, not anecdotes.** Every raw sample is saved. Summaries are
medians with a 95% bootstrap interval, seeded so re-analysing a file gives the
same interval. The old suite reported best-of-5, which says nothing about
spread and hides a bimodal result.

**Comparisons are interleaved and controlled.** `run.py ab A B` builds both
sides and runs them in alternating rounds (ABBA), so thermal drift and
background load land on both. Each round is one paired observation, and the
interval comes from the spread between rounds. Treating every sample as
independent made intervals too narrow: identical binaries "differed".

`run.py aa` is the control: two *separate builds of the same source*, compared
the same way. On this machine they differ by up to ~3–5% on the parser, from
code layout alone. An A/B here once reported a 3% "regression" in code the
change never touched, and that is where it came from. So `ab` holds each
metric to the floor the latest `aa` run measured: a change only counts if it
is larger than what identical source can show. Floors are estimates from one
control run; re-run `aa` when conditions change, and on a quiet machine they
tighten. `compare` (two results files from different sessions) works, but
says plainly that session drift is in its ratios.

**Claims are executable.** `claims.json` is the list of what the README and
release notes may say about performance. Each claim is a threshold on a
metric, or on a ratio such as sink against vte, for a named CPU. A claim is
judged on the conservative end of its interval: a `>=` claim must hold at the
interval's low end. `run` checks every claim after a full run. When a claim
fails, it stops being said until it passes again.

## Comparing against other terminals: termbench

`termbench.py` runs inside the terminal being measured, so it works with any
terminal and needs nothing from it beyond answering standard queries:

```sh
# in each terminal, windows the same size:
python3 bench/termbench.py
python3 bench/run.py compare-terms bench/results/term-*.json
```

It writes each workload, then a Device Attributes query (`ESC [ c`).
Terminals handle input in order, so the answer only comes back once every
byte before it has been consumed. Nothing asks the terminal how fast it is.

- **Responsiveness** is the same query with nothing in front of it. For sink
  this includes waiting for the next frame, since replies go out from the
  frame loop.
- **Agreement:** after each workload it asks for the cursor position.
  Terminals given the same bytes at the same size should agree, and
  `compare-terms` flags any that don't.
- **Size check:** `compare-terms` refuses to compare windows of different
  sizes, since throughput scales with cell count.

It was checked against tmux, run detached, which answers the same queries.

What it can't see: whether the terminal drew what it consumed (it measures
bytes consumed per second, not frames drawn), and keypress-to-photon latency,
which needs a camera or light sensor on the screen. Those limits are why it
doesn't claim more.

`run_vtebench.sh` still runs Alacritty's vtebench for anyone who wants that
tool's numbers. termbench is the one this suite stands behind, because it
checks the terminal consumed the bytes rather than assuming it did.

## sink's parse budget

sink deliberately caps how much of each frame goes into parsing (8 ms; see
`kParseBudgetNs` in `main.cpp`), so a runaway `cat` stays interruptible.
Throughput measured end to end, by `pty_paced` or by termbench, is limited by
that cap on purpose. Any published cross-terminal number should say so. The
budget/throughput trade is linear:

| budget | throughput | share of frame |
|---|---|---|
| 5 ms | 32 MB/s | 33% |
| 8 ms | 57 MB/s | 52% |
| 10 ms | 65 MB/s | 64% |
| 12 ms | 72 MB/s | 76% |
| 14 ms | 89 MB/s | 86% |

(Measured 2026-09-08, before the October fix below raised the 8 ms figure.)

## What building this suite found

The first full runs, 2026-10-29, Apple M1 Max:

1. **A quadratic scan in `sync_output::parse`. Fixed.** Every 64 KB slice
   searched *all* pending output (up to the pty's 4 MB backpressure cap) for
   the next `ESC[?2026h`. On escape-dense output that cost more than parsing.
   Under a flood of TUI redraws, sink took in 15 MB/s and its worst frame
   spent 11.8 ms parsing, against an 8 ms budget. Interleaved A/B, 6 rounds:
   paced cursor-heavy throughput 4.98x (95% CI 4.91–5.05), worst frame
   0.71x (11.85 to 8.34 ms), paced plain text 1.21x. A unit test pins the
   slice-boundary case the fix also closes: a sequence straddling the end of
   a slice used to be split and missed.
2. **The sync-output layer costs 10–15% on escape-heavy output** even
   without the quadratic case (`parser.colored_text` 152 against
   `parser_core.colored_text` 180 MB/s): one `find` per chunk that can't skip
   ahead where escapes are dense. Open.
3. **The distinct-CJK atlas cliff is back at real resolution.** It was fixed
   in September at 16 px and 1x density, where it measured 0.30 ms. At the
   app's default (15 pt on a 2x display, so 30 px glyphs), one screen of
   distinct CJK doesn't fit the 2048 atlas, and frames take 60–90 ms. Open;
   it was invisible because the old bench didn't render at the real size.
4. **The gel is the most expensive thing in the render path:** ~5.2 ms of GPU
   per frame on a 4208x1678 backbuffer. A fairly heavy shader background
   costs 2.5 ms and the CRT effect 2.3 ms. With every effect on, a frame takes
   6.9 ms against a 120 Hz budget of 8.3 ms. Open.
5. **Live reflow takes ~11 ms per resize step at the default 10,000 lines of
   scrollback**, most of a 60 Hz frame during a window drag. Open.
6. **Find-bar keystrokes take 2.5 ms at the default depth** (the September
   figure was 1.9 ms); still comfortably inside a frame.

None of today's runs are publishable as they stand: the machine was on
battery, then busy (load average 5–11). The intervals in those files say so,
and so does the environment block at the top of each.

## Lessons kept from the first suite (2026-08/09)

These shaped the current design and still apply:

- **A mean says nothing about states the scenes don't reach.** The search
  cliff (271 ms/frame), the atlas thrash (152 ms/frame) and the scrollback
  flick gap were invisible until a scene reached them. Today's CJK finding is
  the same lesson again: a scene at the wrong *resolution* doesn't reach the
  state either.
- **Sample before optimizing.** The table-driven CSI rewrite once recorded as
  "the obvious next step" was 2.6% of parse time. The error/failed flash
  trigger was 36%.
- **A semantically neutral edit can move a harness 2x through codegen.**
  Adding wide-character handling to vte_bench made it 2.2x faster on a
  workload with no wide characters. Today's A/A' control measures the same
  effect, smaller: 3–5% between two builds of identical source.
- **Uniform drift across unrelated workloads is thermal, not code.** Measure
  both sides in the same session; `ab` does this by construction.
- **Keep the two parser harnesses doing the same work.** Parity broke twice
  (row ring, double width) and silently changed what was being compared. The
  screen-text cross-check now catches that instead of a careful reader.
- The fixes that came out of the first suite: the -O0 shipped builds, O(n²)
  scrollback trimming, per-character allocation, `std::stoi` CSI parsing,
  44-byte cells, a memmove per newline, `std::queue<char>` in the pty
  hand-off, and the missing per-frame parse budget. Details are in git
  history.

## Files

| file | |
|---|---|
| `run.py` | the runner: build, run, check, statistics, A/B, claims |
| `harness.hpp` | shared C++: JSON output, fingerprints |
| `sink_bench.cpp`, `pty_bench.cpp`, `render_bench.cpp` | the benches; each also runs on its own and prints a table |
| `vte_bench/` | the Rust comparison harness |
| `termbench.py` | cross-terminal, run inside the terminal |
| `gen_workloads.py`, `workloads.sha256` | workload generator and its manifest |
| `expected.json` | blessed output fingerprints |
| `claims.json` | what may be said publicly |
| `shaders/bench.frag` | the shader background scene's shader |
| `results/` | results files (frame PNGs are gitignored) |
