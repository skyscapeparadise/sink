#!/usr/bin/env python3
"""Cross-terminal throughput and responsiveness, measured from outside.

Run it inside the terminal you want to measure -- sink, Alacritty, kitty,
Ghostty, Terminal.app, anything -- with the window at the same size each
time:

    python3 bench/termbench.py                  # writes bench/results/term-<name>-<date>.json
    python3 bench/run.py compare-terms bench/results/term-*.json

How it measures, and why it can't be fooled by the terminal under test:

  Throughput. Each workload file (the same bytes sink_bench parses) is written
  to the terminal, followed by a Primary Device Attributes query (ESC [ c).
  Terminals process their input in order, so the answer can only come back
  once every byte before it has been consumed. The clock runs from the first
  byte written to the answer arriving. Nothing here asks the terminal how
  fast it is; it only has to answer a query every terminal answers.

  Responsiveness. The same query with nothing in front of it, many times: how
  long the terminal takes to notice input and reply. For sink this includes
  waiting for the next frame, since replies are sent from the frame loop --
  that is real latency a program asking the terminal something will see.

  Agreement. After each workload the cursor position is asked for (ESC [ 6 n).
  Two terminals given the same bytes at the same size should agree on where
  the cursor ended up; run.py flags any that don't, which catches a terminal
  that dropped or misparsed part of the stream.

What this does NOT measure, and why:
  - Whether the terminal drew what it consumed. A terminal that parses
    everything and skips rendering during a flood looks fast here, and many
    do (sink renders at most once per frame, as do most). Throughput is
    "bytes consumed per second", not "frames drawn".
  - Keypress-to-photon latency. That needs a camera or a light sensor on the
    screen; it cannot be measured honestly from inside the terminal.
  - Rendering quality or correctness beyond the cursor check.

It writes to the main screen, as `cat` would, so it fills the window's
scrollback; it clears the screen and scrollback when it's done.
"""
import argparse
import datetime
import fcntl
import hashlib
import json
import os
import platform
import re
import select
import struct
import subprocess
import sys
import termios
import time
import tty

HERE = os.path.dirname(os.path.abspath(__file__))
WORKLOAD_DIR = os.path.join(HERE, "workloads")
MANIFEST = os.path.join(HERE, "workloads.sha256")

DEFAULT_WORKLOADS = ["plain_text", "colored_text", "cursor_heavy", "unicode_heavy", "sync_tui"]

DA1 = b"\x1b[c"
DA1_REPLY = re.compile(rb"\x1b\[\?[0-9;]*c")
CPR = b"\x1b[6n"
CPR_REPLY = re.compile(rb"\x1b\[([0-9]+);([0-9]+)R")
XTVERSION = b"\x1b[>0q"
XTVERSION_REPLY = re.compile(rb"\x1bP>\|(.*?)\x1b\\")
# Ends whatever a truncated workload left open: CAN aborts an escape or
# control sequence, ST ends a string (OSC/DCS/APC), then attributes reset.
# The first five workloads end mid-sequence (see gen_workloads.py).
SETTLE = b"\x18\x1b\\\x1b[0m"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def check_manifest(names):
    want = {}
    for line in open(MANIFEST):
        if line.strip():
            digest, name = line.split()
            want[name] = digest
    for n in names:
        path = os.path.join(WORKLOAD_DIR, n + ".bin")
        if not os.path.exists(path):
            sys.exit(f"missing {path}: run python3 bench/gen_workloads.py")
        if sha256(path) != want.get(n + ".bin"):
            sys.exit(f"{path} does not match bench/workloads.sha256; regenerate it")


class Tty:
    def __init__(self):
        self.fd_in = sys.stdin.fileno()
        self.fd_out = sys.stdout.fileno()
        self.saved = termios.tcgetattr(self.fd_in)
        self.buf = b""

    def __enter__(self):
        tty.setraw(self.fd_in)  # also turns off output post-processing on this tty
        return self

    def __exit__(self, *exc):
        termios.tcsetattr(self.fd_in, termios.TCSADRAIN, self.saved)

    def write(self, data):
        view = memoryview(data)
        while view:
            n = os.write(self.fd_out, view)
            view = view[n:]

    def wait_for(self, pattern, timeout):
        """Reads until `pattern` matches; returns the match, or None on timeout.
        Anything before the match (focus events, stray replies) is dropped."""
        deadline = time.perf_counter() + timeout
        while True:
            m = pattern.search(self.buf)
            if m:
                self.buf = self.buf[m.end():]
                return m
            left = deadline - time.perf_counter()
            if left <= 0:
                return None
            r, _, _ = select.select([self.fd_in], [], [], left)
            if r:
                self.buf += os.read(self.fd_in, 65536)

    def drain(self):
        while select.select([self.fd_in], [], [], 0.05)[0]:
            self.buf += os.read(self.fd_in, 65536)
        self.buf = b""

    def size(self):
        rows, cols, xpix, ypix = struct.unpack("HHHH", fcntl.ioctl(self.fd_out, termios.TIOCGWINSZ, b"\0" * 8))
        return cols, rows, xpix, ypix


def machine():
    def sysctl(k):
        try:
            return subprocess.run(["sysctl", "-n", k], capture_output=True, text=True).stdout.strip()
        except OSError:
            return ""
    return {"cpu": sysctl("machdep.cpu.brand_string"), "os": platform.platform(), "python": platform.python_version()}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--label", help="terminal name (default: from TERM_PROGRAM / XTVERSION)")
    ap.add_argument("--out", help="results file (default: bench/results/term-<label>-<date>.json)")
    ap.add_argument("--reps", type=int, default=7, help="timed runs per workload (default 7)")
    ap.add_argument("--warmup", type=int, default=1)
    ap.add_argument("--queries", type=int, default=100, help="responsiveness queries (default 100)")
    ap.add_argument("--workloads", nargs="*", default=DEFAULT_WORKLOADS)
    ap.add_argument("--timeout", type=float, default=120.0, help="seconds to wait for one workload")
    args = ap.parse_args()

    if not (os.isatty(0) and os.isatty(1)):
        sys.exit("termbench must run in a terminal window, with stdin and stdout unredirected.")
    check_manifest(args.workloads)
    data = {n: open(os.path.join(WORKLOAD_DIR, n + ".bin"), "rb").read() for n in args.workloads}

    print("termbench: measuring this terminal. Don't resize, type, or switch away until it's done.")
    print("It fills this window's scrollback, and clears it at the end.")
    time.sleep(2)

    metrics = []
    with Tty() as t:
        t.drain()
        cols, rows, xpix, ypix = t.size()
        t.write(XTVERSION + DA1)
        m = t.wait_for(XTVERSION_REPLY, 1.0)
        version = m.group(1).decode(errors="replace") if m else ""
        if not t.wait_for(DA1_REPLY, 5.0):
            sys.exit("this terminal never answered a device attributes query (ESC [ c)")
        program = os.environ.get("TERM_PROGRAM", "")
        label = args.label or (version.split()[0] if version else program) or "unknown"

        # Responsiveness first, on a quiet terminal.
        lat = []
        for _ in range(args.queries):
            t0 = time.perf_counter_ns()
            t.write(DA1)
            if not t.wait_for(DA1_REPLY, 5.0):
                sys.exit("no answer to a device attributes query within 5s")
            lat.append((time.perf_counter_ns() - t0) / 1e6)
            time.sleep(0.003)  # not in lockstep with the terminal's frame clock
        metrics.append({"id": "term.query_latency", "label": "query round trip", "unit": "ms",
                        "better": "lower", "fingerprint": None, "info": {}, "samples": lat})

        for name in args.workloads:
            payload = data[name]
            samples, cprs = [], []
            for rep in range(args.warmup + args.reps):
                # Same starting state every time: soft reset, clear, home. Not
                # timed, and finished (answered) before the clock starts.
                t.write(b"\x1b[!p\x1b[H\x1b[2J\x1b[3J" + DA1)
                t.wait_for(DA1_REPLY, 10.0)
                t0 = time.perf_counter_ns()
                t.write(payload)
                t.write(SETTLE + DA1)
                if not t.wait_for(DA1_REPLY, args.timeout):
                    sys.exit(f"{name}: no answer within {args.timeout}s")
                elapsed = (time.perf_counter_ns() - t0) / 1e9
                t.write(CPR)
                c = t.wait_for(CPR_REPLY, 5.0)
                if rep >= args.warmup:
                    samples.append(len(payload) / (1024 * 1024) / elapsed)
                    cprs.append(f"{int(c.group(1))};{int(c.group(2))}" if c else "none")
            metrics.append({"id": f"term.{name}", "label": name, "unit": "MB/s", "better": "higher",
                            "fingerprint": None,
                            "info": {"bytes": len(payload), "cursor": sorted(set(cprs))},
                            "samples": samples})

        t.write(b"\x1b[!p\x1b[0m\x1b[H\x1b[2J\x1b[3J")

    result = {
        "bench": "termbench",
        "config": {
            "terminal": label, "xtversion": version, "term_program": program,
            "term_program_version": os.environ.get("TERM_PROGRAM_VERSION", ""),
            "term": os.environ.get("TERM", ""),
            "cols": cols, "rows": rows, "xpixel": xpix, "ypixel": ypix,
            "reps": args.reps, "warmup": args.warmup,
        },
        "machine": machine(),
        "date": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "metrics": metrics,
    }
    out = args.out
    if not out:
        os.makedirs(os.path.join(HERE, "results"), exist_ok=True)
        safe = re.sub(r"[^A-Za-z0-9._-]+", "_", label)
        out = os.path.join(HERE, "results", f"term-{safe}-{datetime.date.today().isoformat()}.json")
    with open(out, "w") as f:
        json.dump(result, f, indent=1)

    print(f"termbench: {label} ({version or program}), {cols}x{rows} cells")
    for m in metrics:
        s = sorted(m["samples"])
        med = s[len(s) // 2] if len(s) % 2 else (s[len(s) // 2 - 1] + s[len(s) // 2]) / 2
        print(f"  {m['label']:<20} median {med:9.2f} {m['unit']:<5}  (n={len(s)}, {s[0]:.2f}..{s[-1]:.2f})")
    print(f"wrote {out}")
    print("Compare terminals with: python3 bench/run.py compare-terms bench/results/term-*.json")


if __name__ == "__main__":
    main()
