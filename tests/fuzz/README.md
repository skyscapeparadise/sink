# Fuzzing sink

A terminal's attack surface is the bytes it displays. Anything that can write
to it -- a file you `cat`, a command's output, a server at the far end of an
`ssh` session -- can send it arbitrary escape sequences and image data. These
fuzzers throw millions of hostile inputs per minute at everything those bytes
reach, with AddressSanitizer and UndefinedBehaviorSanitizer turning any memory
error or undefined behaviour into a stopped run and a saved input.

## What is fuzzed

**`fuzz_terminal`** -- the escape-sequence parser and the grid behind it, fed
arbitrary bytes interleaved with what a user does: resizing (down to 1x1),
clicking, selecting and deleting, scrolling, searching. Beyond crashes, it
checks the properties that make a terminal safe to point at untrusted output:

- **Every reply sink sends back to the shell is something sink composed.** A
  reply carrying input text is how a terminal becomes a command-injection
  vector: a file whose escape sequences make the terminal "type" a command.
  Each reply is matched against the full list of formats in `ansi_parser.cpp`,
  in which only numbers and sink's own fixed strings vary. A new reply in the
  parser has to be added to `reply_grammar()` -- deliberately.
- Mouse editing produces nothing but cursor keys and Delete.
- The cursor stays inside the grid.

**`fuzz_images`** -- the sixel decoder (sink's own) and the PNG decoder
(SDL_image), which kitty-protocol images reach directly. Checks that decoded
dimensions are within limits and match the pixel buffer.

## Safety

The fuzzed code is pure computation -- no file, process or network calls
(`grep` it: none in `ansi_parser.cpp`, `terminal_grid.cpp`,
`terminal_images.cpp`, `prompt_editing.cpp`). That is not taken on trust.
Every run goes through `sandbox-exec` with `sandbox_fuzz.sb`, enforced by the
kernel:

- file writes are refused everywhere except `build-fuzz/<fuzzer>/`;
- all network access is refused, local sockets included;
- starting a process is refused.

`sandbox_tests.sb` is the looser profile for the test suites and for `repro`
(which needs to run a symbolizer): writes only to temporary folders and the
scratch folder, no internet.

Both were checked by trying to break them -- writing to the home folder and
the repo's source, reaching the internet, forking -- and every attempt was
refused. To re-check:

    S=$PWD/build-fuzz/scratch; mkdir -p "$S"
    sandbox-exec -f tests/fuzz/sandbox_fuzz.sb -D SCRATCH="$S" /usr/bin/touch ~/canary   # refused
    sandbox-exec -f tests/fuzz/sandbox_fuzz.sb -D SCRATCH="$S" /usr/bin/touch "$S/ok"   # allowed

Runs are niced, so they stay out of the way of other work.

## Running

Needs Homebrew's LLVM (`brew install llvm`): Xcode's clang has the
sanitizers but not libFuzzer.

    tests/fuzz/build.sh                        # builds into build-fuzz/
    tests/fuzz/run.sh terminal 3600            # fuzz for an hour
    tests/fuzz/run.sh images 3600
    tests/fuzz/run.sh regress                  # replay every seed once (seconds)

Longer is better: coverage keeps growing for hours. Each fuzzer keeps the
inputs that reached new code in `build-fuzz/<fuzzer>/corpus/`, so successive
runs build on each other.

## When it finds something

The input is saved to `build-fuzz/<fuzzer>/artifacts/`. Replay it with names
in the stack trace (the fuzz sandbox can't run the symbolizer):

    tests/fuzz/run.sh repro terminal build-fuzz/terminal/artifacts/crash-...

Once fixed, copy the input into `seeds/<fuzzer>/` as `regress-<what>`, so every
later run -- and `regress` -- tries it again.

## Seeds

`make_seeds.py` writes `seeds/`: examples of every kind of sequence sink
handles, encoded in each fuzzer's input format, so fuzzing starts from valid
structure rather than noise. `terminal.dict` lists escape-sequence fragments
for libFuzzer to splice in. The `regress-*` seeds are inputs that found real
bugs:

- `regress-sixel-past-size-limit`: a sixel that grew past the size limit
  reported success with a 0x0 image, dropping the part it meant to keep.
- `regress-motion-count-overflow`: `CSI 2147483647 B` overflowed a signed
  int -- undefined behaviour -- and moved the cursor the wrong way.
- `regress-sixel-quadratic-growth`: 1.5KB of sixel took over a second to
  decode (8s under the sanitizers), regrowing the whole image at every step
  toward a 154MB image the image store then refused anyway.

## Limits

- SDL_image is not instrumented, so ASan sees memory errors *inside* the PNG
  decoder only when they crash or corrupt something sink touches.
- Rendering is not fuzzed (it needs a GPU renderer), nor is `main.cpp`'s input
  handling -- the paste sanitiser there has its own tests in the commit that
  added it.
- Real-shell testing with hostile input belongs in a disposable VM; these
  fuzzers never run a shell at all.
