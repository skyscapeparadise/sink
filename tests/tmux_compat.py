#!/usr/bin/env python3
"""tmux compatibility proof.

Runs a real tmux in a real PTY, captures every byte it writes, replays that
stream through sink's parser, and compares the result against tmux's own
`capture-pane` output.

That comparison is the point: capture-pane is tmux stating what it believes it
drew, and the replay is what sink actually rendered from the escape sequences
tmux sent. Any disagreement is a genuine difference in how sink interprets
them, not a judgement call about appearance.

Full-screen apps lean on scroll regions, alt-screen switching, cursor
addressing and SGR far harder than ordinary shell output does, which is why
this surfaces VT bugs that nothing else in the test suite reaches.

    python3 tests/tmux_compat.py [--keep] [path/to/tmux_replay]
"""
import os, sys, pty, time, fcntl, struct, termios, subprocess, tempfile, shutil

COLS, ROWS = 80, 24
REPLAY = sys.argv[-1] if sys.argv[-1].endswith("tmux_replay") else "build/tmux_replay"
KEEP = "--keep" in sys.argv


def tmux(sock, *args, check=True):
    return subprocess.run(["tmux", "-S", sock, *args],
                          capture_output=True, text=True, check=check)


def capture_client_output(sock, session, drive=None, settle=0.6):
    """Attach a client on a PTY and return every byte tmux writes to it.

    drive() runs once the initial paint has settled, so incremental redraws
    are captured too rather than only the first full repaint.
    """
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
    env = dict(os.environ, TERM="xterm-256color")   # what sink advertises
    proc = subprocess.Popen(
        ["tmux", "-S", sock, "attach", "-t", session],
        stdin=slave, stdout=slave, stderr=slave, env=env, close_fds=True)
    os.close(slave)

    out = bytearray()

    def drain(seconds):
        end = time.time() + seconds
        os.set_blocking(master, False)
        while time.time() < end:
            try:
                chunk = os.read(master, 65536)
                if chunk:
                    out.extend(chunk)
                    end = time.time() + 0.25   # keep going while data flows
                else:
                    break
            except (BlockingIOError, OSError):
                time.sleep(0.02)

    drain(settle)
    if drive:
        drive()
        drain(settle)

    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
    os.close(master)
    return bytes(out)


def run_case(case):
    name, mode = case["name"], case["mode"]
    sock = tempfile.mktemp(prefix="sink-tmux-")
    session = "sinktest"
    try:
        tmux(sock, "-f", "/dev/null", "new-session", "-d", "-s", session,
             "-x", str(COLS), "-y", str(ROWS))
        case["setup"](sock, session)
        time.sleep(0.4)

        drive = case.get("drive")
        stream = capture_client_output(
            sock, session,
            drive=(lambda: drive(sock, session)) if drive else None)
        got = subprocess.run([REPLAY, str(COLS), str(ROWS)],
                             input=stream, capture_output=True
                             ).stdout.decode("utf-8", "replace")
        got_lines = [l.rstrip() for l in got.rstrip("\n").split("\n")]

        if mode == "compare":
            expected = tmux(sock, "capture-pane", "-p", "-t", session).stdout
            exp_lines = [l.rstrip() for l in expected.rstrip("\n").split("\n")]
            # tmux trims trailing blank rows; compare only what it reports.
            trimmed = got_lines[:len(exp_lines)]
            if trimmed == exp_lines:
                print(f"  PASS  {name}  ({len(stream)} bytes)")
                return True
            print(f"  FAIL  {name}  ({len(stream)} bytes)")
            for i, (e, g) in enumerate(zip(exp_lines, trimmed)):
                if e != g:
                    print(f"          row {i}\n            tmux: {e!r}\n            sink: {g!r}")
            if len(exp_lines) != len(trimmed):
                print(f"          row count: tmux {len(exp_lines)}, sink {len(trimmed)}")
        else:
            screen = "\n".join(got_lines)
            missing = [w for w in case["want"] if w not in screen]
            if not missing:
                print(f"  PASS  {name}  ({len(stream)} bytes)")
                return True
            print(f"  FAIL  {name}  ({len(stream)} bytes)")
            for w in missing:
                print(f"          expected on screen but absent: {w!r}")

        if KEEP:
            path = f"/tmp/tmux-stream-{name.replace(' ', '_')}.bin"
            open(path, "wb").write(stream)
            print(f"          stream kept at {path}")
        return False
    finally:
        tmux(sock, "kill-server", check=False)
        if os.path.exists(sock):
            os.unlink(sock)


def send(sock, session, keys):
    tmux(sock, "send-keys", "-t", session, keys, "Enter")
    time.sleep(0.35)


# Each case declares which oracle is valid for it.
#
#   "compare"  the replay must equal `tmux capture-pane -p`.
#
#   "expect"   capture-pane is *not* a valid oracle, so the case asserts on
#              sink's rendering directly. Two situations need this, and both
#              were mistaken for sink bugs before the oracle was checked:
#
#              - DEC Special Graphics. tmux stores ESC ( 0 line drawing as the
#                underlying ACS letters and capture-pane prints those, so it
#                reports "lqk" where a terminal must draw "\u250c\u2500\u2510".
#                sink translating them is correct and capture-pane disagreeing
#                is expected.
#
#              - Split panes. `capture-pane -t <session>` returns the active
#                pane alone, with no divider, while the replay renders the
#                whole client screen. They describe different things.
CASES = [
    {
        "name": "plain shell output",
        "mode": "compare",
        "setup": lambda sock, s: send(sock, s, "printf 'alpha\\nbravo\\ncharlie\\n'"),
    },
    {
        "name": "SGR colours and attributes",
        "mode": "compare",
        "setup": lambda sock, s: send(sock, s,
            r"printf '\033[31mred\033[0m \033[1mbold\033[0m \033[4munder\033[0m\n'"),
    },
    {
        "name": "UTF-8 box drawing",
        "mode": "compare",
        "setup": lambda sock, s: send(sock, s, r"printf '\342\224\214\342\224\200\342\224\220\n'"),
    },
    {
        "name": "DEC Special Graphics (ESC ( 0)",
        "mode": "expect",
        "setup": lambda sock, s: send(sock, s, r"printf '\033(0lqk\033(B\n'"),
        # tmux relays the ACS sequence; sink must draw the line-drawing glyphs.
        "want": ["\u250c\u2500\u2510"],
    },
    {
        "name": "status line and window rename",
        "mode": "compare",
        "setup": lambda sock, s: (
            tmux(sock, "set-option", "-t", s, "status", "on"),
            tmux(sock, "rename-window", "-t", s, "sinkwin"),
            time.sleep(0.3),
            send(sock, s, "printf 'named\\n'")),
    },
    {
        "name": "scroll beyond the screen",
        "mode": "compare",
        "setup": lambda sock, s: send(sock, s, "for i in $(seq 1 40); do echo line$i; done"),
    },
    {
        "name": "alt screen enter and exit",
        "mode": "compare",
        "setup": lambda sock, s: (
            send(sock, s, "printf 'before\\n'"),
            send(sock, s, r"printf '\033[?1049h\033[HALT-SCREEN\033[?1049l'"),
            send(sock, s, "printf 'after\\n'")),
    },
    {
        "name": "split panes",
        "mode": "expect",
        "setup": lambda sock, s: (
            tmux(sock, "split-window", "-h", "-t", s),
            time.sleep(0.3),
            send(sock, s, "printf 'right-pane\\n'"),
            tmux(sock, "select-pane", "-L", "-t", s),
            time.sleep(0.2),
            send(sock, s, "printf 'left-pane\\n'")),
        # Both panes and the divider tmux draws between them must be present.
        "want": ["left-pane", "right-pane", "\u2502"],
    },
    {
        "name": "incremental redraw while attached",
        "mode": "compare",
        "setup": lambda sock, s: send(sock, s, "printf 'initial\\n'"),
        # Runs after the client attaches, so this compares an incremental
        # update rather than the initial full repaint.
        "drive": lambda sock, s: send(sock, s, "printf 'appended\\n'"),
    },
]


def main():
    if not os.path.exists(REPLAY):
        print(f"replay binary not found at {REPLAY}", file=sys.stderr)
        print("build it with: cmake --build build --target tmux_replay", file=sys.stderr)
        return 2
    if shutil.which("tmux") is None:
        print("tmux not installed", file=sys.stderr)
        return 2

    ver = subprocess.run(["tmux", "-V"], capture_output=True, text=True).stdout.strip()
    print(f"{ver}  |  {COLS}x{ROWS}  |  TERM=xterm-256color")
    failures = 0
    for case in CASES:
        if not run_case(case):
            failures += 1
    print(f"\n{len(CASES) - failures}/{len(CASES)} cases agree with tmux")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
