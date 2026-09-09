#!/bin/bash
# Runs Alacritty's vtebench in whatever terminal it is launched from.
#
# Unlike bench/sink_bench (parser only, headless) and bench/render_bench (frame
# building only), this measures the whole program end to end: PTY read, parse,
# render, present. It is the only one of the three that can compare sink to
# another terminal as a user experiences it.
#
# It works by writing escape sequences to stdout and timing how long the
# terminal takes to consume them, so it MUST run in a real terminal window and
# must not be piped or redirected.
#
#   Run it in Alacritty, then in sink, then compare the two .dat files.
#
# Results scale with the number of cells, so the two windows must be the same
# size. The script prints the size it sees; check they match before comparing.

set -u

VTEBENCH="${VTEBENCH:-}"
if [ -z "$VTEBENCH" ]; then
    if command -v vtebench >/dev/null 2>&1; then
        VTEBENCH="$(command -v vtebench)"
    elif [ -x "$HOME/.cargo/bin/vtebench" ]; then
        VTEBENCH="$HOME/.cargo/bin/vtebench"
    fi
fi
if [ -z "$VTEBENCH" ] || [ ! -x "$VTEBENCH" ]; then
    echo "vtebench not found. Install it with:" >&2
    echo "  cargo install --git https://github.com/alacritty/vtebench" >&2
    exit 1
fi

# The benchmark data ships in the source tree, not the installed binary.
BENCHMARKS="${VTEBENCH_BENCHMARKS:-}"
if [ -z "$BENCHMARKS" ]; then
    BENCHMARKS="$(ls -d "$HOME"/.cargo/git/checkouts/vtebench-*/*/benchmarks 2>/dev/null | head -1)"
fi
if [ -z "$BENCHMARKS" ] || [ ! -d "$BENCHMARKS" ]; then
    echo "vtebench benchmark data not found." >&2
    echo "Set VTEBENCH_BENCHMARKS=/path/to/vtebench/benchmarks, or clone:" >&2
    echo "  git clone https://github.com/alacritty/vtebench" >&2
    exit 1
fi

if [ ! -t 1 ]; then
    echo "stdout is not a terminal. vtebench measures the terminal consuming" >&2
    echo "its output, so it has to run in a real window, unpiped." >&2
    exit 1
fi

LABEL="${1:-$(basename "${TERM_PROGRAM:-terminal}")}"
OUT="${2:-/tmp/vtebench-${LABEL}.dat}"

cols=$(tput cols 2>/dev/null || echo "?")
lines=$(tput lines 2>/dev/null || echo "?")

echo "vtebench    : $VTEBENCH"
echo "benchmarks  : $BENCHMARKS"
echo "label       : $LABEL"
echo "terminal    : ${cols}x${lines}   TERM=$TERM   TERM_PROGRAM=${TERM_PROGRAM:-unset}"
echo "output      : $OUT"
echo
echo "Both terminals must be ${cols}x${lines} for the numbers to be comparable."
echo "Starting in 3 seconds; do not resize or focus away."
sleep 3

"$VTEBENCH" -b "$BENCHMARKS" --dat "$OUT"

printf '\033[?1049l\033[0m\033c'   # leave alt screen, reset attributes and state
echo
echo "wrote $OUT"
echo "compare with:  paste /tmp/vtebench-*.dat | column -t"
