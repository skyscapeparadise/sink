#!/bin/bash
# The measurements phase 5 of docs/threading-plan.md needs, run unattended once
# the Mac is quiet enough to trust them.
#
#   bench/phase5.sh            wait for a quiet machine, then run everything
#   bench/phase5.sh --now      skip the wait (results are marked noisy if it isn't quiet)
#   bench/phase5.sh --dry-run  print the steps and exit
#
# Start it from a terminal and leave the Mac alone: plugged in, nothing else
# busy. Quitting Zed helps, since an editor hosting an agent session keeps the
# load average around 3-4 on its own. It takes 30-45 minutes once it starts,
# and writes everything to bench/results/, plus a summary,
# <date>-phase5-summary.txt, listing what each step produced.
#
# What it doesn't do: termbench, which has to run inside each terminal. See
# the phase 5 section of the plan for that part.

set -u
cd "$(dirname "$0")/.."

NOW=0; DRY=0
for a in "$@"; do
    case "$a" in
        --now) NOW=1 ;;
        --dry-run) DRY=1 ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done

GEL_COMMIT=6b238d3   # "Halve the gel's GPU cost": its parent is the shader before
STEPS=(
    "A/A' control (all benchmarks): noise floors for these build flags|python3 bench/run.py aa --rounds 6 --allow-noisy"
    "full suite, with every claim checked|python3 bench/run.py run --allow-noisy"
    "threaded vs frame-paced, plain text|python3 bench/run.py pair pty_paced.plain_text pty_threaded.plain_text --rounds 8 --allow-noisy"
    "threaded vs frame-paced, TUI redraws|python3 bench/run.py pair pty_paced.cursor_heavy pty_threaded.cursor_heavy --rounds 8 --allow-noisy"
    "threaded vs frame-paced, synchronized TUI|python3 bench/run.py pair pty_paced.sync_tui pty_threaded.sync_tui --rounds 8 --allow-noisy"
    "gel CPU recheck: the gel commit against its parent|python3 bench/run.py ab ${GEL_COMMIT}^ ${GEL_COMMIT} --bench render --only gel --rounds 6 --allow-noisy"
)

if [ "$DRY" = 1 ]; then
    for s in "${STEPS[@]}"; do echo "- ${s%%|*}"; echo "    ${s#*|}"; done
    exit 0
fi

quiet() {
    pmset -g batt | grep -q "AC Power" || return 1
    python3 -c "import os,sys; sys.exit(0 if os.getloadavg()[0] < 2.5 else 1)"
}

if [ "$NOW" = 0 ]; then
    echo "waiting for a quiet machine (plugged in, 1-minute load under 2.5 for two minutes)..."
    streak=0
    while [ $streak -lt 4 ]; do
        if quiet; then streak=$((streak + 1)); else streak=0; fi
        sleep 30
    done
fi

STAMP=$(date +%Y-%m-%dT%H%M)
SUMMARY="bench/results/${STAMP}-phase5-summary.txt"
LOGDIR="bench/results/${STAMP}-phase5-logs"
mkdir -p "$LOGDIR"
{
    echo "phase 5 measurements, started $(date)"
    echo "commit $(git rev-parse --short HEAD)$(git diff --quiet || echo ' (uncommitted changes)')"
    echo "power: $(pmset -g batt | head -1)"
    echo "load: $(uptime | sed 's/.*load averages*: //')"
    echo
} > "$SUMMARY"

i=0
for s in "${STEPS[@]}"; do
    i=$((i + 1))
    label="${s%%|*}"; cmd="${s#*|}"
    log="$LOGDIR/step${i}.log"
    echo "[$i/${#STEPS[@]}] $label"
    start=$(date +%s)
    before=$(ls bench/results/ | sort)
    bash -c "$cmd" > "$log" 2>&1
    rc=$?
    after=$(ls bench/results/ | sort)
    made=$(comm -13 <(echo "$before") <(echo "$after") | grep -v "phase5" | tr '\n' ' ')
    {
        echo "[$i] $label"
        echo "    $cmd"
        echo "    exit $rc, $(( $(date +%s) - start ))s, load $(uptime | sed 's/.*load averages*: //')"
        echo "    wrote: ${made:-nothing}"
        echo "    log: $log"
        grep -E "verdict|better|worse|no detectable|FAIL|PASS|checks? failed|All output checks" "$log" | tail -12 | sed 's/^/      /'
        echo
    } >> "$SUMMARY"
done

echo "finished $(date)" >> "$SUMMARY"
echo
echo "done. summary: $SUMMARY"
