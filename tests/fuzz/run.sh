#!/bin/bash
# Runs a sink fuzzer inside the fuzz sandbox, at low priority. See README.md.
#
#   tests/fuzz/run.sh terminal [seconds]          fuzz (default 600s)
#   tests/fuzz/run.sh images [seconds]
#   tests/fuzz/run.sh repro terminal <input>      replay one input, symbolized
#   tests/fuzz/run.sh regress                     replay every seed once
#
# Writes only under build-fuzz/<name>/: the corpus it grows, and any inputs
# that crash or break a property (artifacts/). The sandbox refuses any other
# write, any network access, and starting processes.
set -euo pipefail
cd "$(dirname "$0")/../.."

usage() { sed -n '4,8p' "$0" >&2; exit 2; }
[ $# -ge 1 ] || usage

if [ "$1" = "repro" ]; then
    [ $# -eq 3 ] || usage
    NAME=$2 INPUT=$3
    WORK="$PWD/build-fuzz/$NAME"
    # The suites' profile, not the fuzz one: turning addresses into function
    # names runs a symbolizer process, which the fuzz profile forbids. Writes
    # are still confined to temporary folders and the scratch folder.
    ASAN_OPTIONS=detect_leaks=0:symbolize=1:abort_on_error=0 \
    UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
        sandbox-exec -f tests/fuzz/sandbox_tests.sb -D SCRATCH="$WORK" \
        "build-fuzz/fuzz_$NAME" "$INPUT"
    exit $?
fi

if [ "$1" = "regress" ]; then
    # Every seed once, including inputs that found bugs before (regress-*):
    # a quick check that nothing fixed has come back. -runs=0 executes the
    # given inputs and stops; it does not fuzz. -timeout=2 fails an input that
    # takes longer than two seconds, so a slow-path regression (one took 8s
    # in this sanitizer build) fails rather than passing slowly.
    status=0
    for NAME in terminal images; do
        WORK="$PWD/build-fuzz/$NAME"
        mkdir -p "$WORK"
        echo "== $NAME"
        ASAN_OPTIONS=detect_leaks=0:symbolize=0:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:symbolize=0 \
            sandbox-exec -f tests/fuzz/sandbox_fuzz.sb -D SCRATCH="$WORK" \
            "build-fuzz/fuzz_$NAME" -runs=0 -timeout=2 "tests/fuzz/seeds/$NAME" 2>&1 \
            | grep -E "INITED|DONE|ERROR|PROPERTY|SUMMARY" || true
        [ "${PIPESTATUS[0]}" -eq 0 ] || status=1
    done
    exit $status
fi

NAME=$1
SECONDS_TO_RUN=${2:-600}
case "$NAME" in
    terminal) MAX_LEN=65536; DICT=(-dict=tests/fuzz/terminal.dict) ;;
    images)   MAX_LEN=262144; DICT=() ;;
    *) usage ;;
esac
BIN="build-fuzz/fuzz_$NAME"
[ -x "$BIN" ] || { echo "$BIN not built; run tests/fuzz/build.sh" >&2; exit 1; }

WORK="$PWD/build-fuzz/$NAME"
mkdir -p "$WORK/corpus" "$WORK/artifacts"

# symbolize=0: the symbolizer is a separate process, which the sandbox
# forbids. Crashes are reported with addresses; `repro` gives the names.
ASAN_OPTIONS=detect_leaks=0:symbolize=0:abort_on_error=0:halt_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1:symbolize=0 \
    nice -n 15 sandbox-exec -f tests/fuzz/sandbox_fuzz.sb -D SCRATCH="$WORK" \
    "$BIN" "$WORK/corpus" "tests/fuzz/seeds/$NAME" "${DICT[@]+"${DICT[@]}"}" \
    -max_total_time="$SECONDS_TO_RUN" -timeout=10 -rss_limit_mb=2048 \
    -max_len="$MAX_LEN" -artifact_prefix="$WORK/artifacts/" -print_final_stats=1
