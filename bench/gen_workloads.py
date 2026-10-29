#!/usr/bin/env python3
"""Generates fixed-content binary workload files used by both the sink
(C++) and vte (Rust) parser benchmarks, so both sides measure the exact
same input bytes rather than two independently-generated approximations of
"similar" content.

Each workload targets ~8MB, large enough that per-call overhead is
negligible relative to total parse time, small enough that the whole suite
runs in a few seconds.

The files are not committed; their SHA-256s are, in workloads.sha256. A
result is only comparable with another if both were measured on the same
bytes, and a seeded generator is not a promise of that -- Python could change
what random.choice() returns for a seed. So this checks what it wrote against
the manifest and fails on any difference, and bench/run.py refuses to run on
files that don't match.

    python3 bench/gen_workloads.py            # write, then verify
    python3 bench/gen_workloads.py --check    # verify only
    python3 bench/gen_workloads.py --update-manifest   # after a deliberate change

The first five workloads share the module-level RNG in order, as they always
have, so their bytes stay what earlier results were measured on. Workloads
added later each get their own RNG, so adding one never shifts another.
"""
import base64
import hashlib
import os
import random
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, "workloads")
MANIFEST = os.path.join(HERE, "workloads.sha256")
TARGET_BYTES = 8 * 1024 * 1024

random.seed(1234)  # deterministic content -> reproducible benchmark runs

ESC = "\x1b"


def write_workload(name: str, chunks, whole=False):
    """`chunks` is a generator yielding str fragments; keeps producing until
    TARGET_BYTES is reached, then truncates to exactly that length.

    The first five workloads end mid-fragment -- possibly mid-escape or
    mid-character -- and keep doing so, since that is the input their results
    were measured on. Later ones (`whole`) stop at the last whole fragment
    instead, so the stream ends in the ground state and a terminal reading it
    is left ready for whatever comes next."""
    path = os.path.join(OUT_DIR, name)
    total = 0
    with open(path, "wb") as f:
        for chunk in chunks:
            b = chunk.encode("utf-8")
            if total + len(b) > TARGET_BYTES:
                if whole:
                    break
                f.write(b[: TARGET_BYTES - total])
                total = TARGET_BYTES
                break
            f.write(b)
            total += len(b)
    print(f"{name}: {total} bytes")


# ---- Workload 1: plain scrolling text (like `cat somelog.txt`) -----------
def gen_plain_text():
    words = ["connecting", "session", "established", "worker", "task",
             "completed", "retrying", "timeout", "reading", "buffer",
             "flushed", "request", "response", "status", "elapsed", "ms"]
    i = 0
    while True:
        line = f"[{i:08d}] " + " ".join(random.choice(words) for _ in range(8))
        yield line + "\r\n"
        i += 1


# ---- Workload 2: heavily colorized text (like `ls --color`, ripgrep) -----
def gen_colored_text():
    # Mix of basic (30-37/90-97), 256-color (38;5;N), and truecolor (38;2;R;G;B)
    names = ["src", "lib", "build", "node_modules", "target", "main.cpp",
             "README.md", "config.toml", "index.js", ".git", "Cargo.lock"]
    i = 0
    while True:
        parts = []
        for _ in range(6):
            mode = i % 4
            name = random.choice(names)
            if mode == 0:
                fg = random.randint(31, 36)
                parts.append(f"{ESC}[{fg}m{name}{ESC}[0m")
            elif mode == 1:
                idx = random.randint(16, 231)
                parts.append(f"{ESC}[38;5;{idx}m{name}{ESC}[0m")
            elif mode == 2:
                r, g, b = random.randint(0, 255), random.randint(0, 255), random.randint(0, 255)
                parts.append(f"{ESC}[38;2;{r};{g};{b}m{name}{ESC}[0m")
            else:
                parts.append(f"{ESC}[1;4m{name}{ESC}[0m")
            i += 1
        yield "  ".join(parts) + "\r\n"


# ---- Workload 3: cursor-heavy full-screen TUI redraw (like htop/vim) -----
def gen_cursor_heavy():
    rows, cols = 50, 120
    frame = 0
    while True:
        out = [f"{ESC}[2J{ESC}[H"]  # clear + home, like a full TUI redraw
        for r in range(1, rows + 1):
            out.append(f"{ESC}[{r};1H")
            if r == 1:
                out.append(f"{ESC}[7m{'status bar':<{cols}}{ESC}[0m")
            else:
                pct = (frame + r) % 100
                out.append(f"{ESC}[K row {r:3d}  load={pct:3d}%  ")
        frame += 1
        yield "".join(out)


# ---- Workload 4: UTF-8 heavy (emoji, box-drawing, wide chars) ------------
def gen_unicode_heavy():
    box = "┌─┬─┐│ │├─┼─┤└─┴─┘"
    emoji = "🚀🔥✨🐍🧪📦🛠️💡🎯🌊"
    cjk = "系统状态正常运行中"
    i = 0
    while True:
        yield f"{box} {emoji[i % len(emoji)]} {cjk} line {i}\r\n"
        i += 1


# ---- Workload 5: every cell a different glyph ---------------------------
# Not realistic text -- real CJK reuses characters heavily -- but it is the
# worst case for the glyph atlas, which caches by codepoint. It exists so
# render_bench can show what happens when a single screen wants more distinct
# glyphs than the atlas holds: the packer resets, every cached glyph is
# dropped, and the whole screen re-rasterizes on the next frame too.
def gen_distinct_cjk():
    # CJK Unified Ideographs, walked in order and wrapped rather than sampled,
    # so the file is deterministic and every screenful is distinct.
    lo, hi = 0x4E00, 0x9FA0
    cp = lo
    while True:
        line = []
        for _ in range(100):  # 100 wide chars = 200 columns
            line.append(chr(cp))
            cp = lo if cp + 1 > hi else cp + 1
        yield "".join(line) + "\r\n"


# ---- Workload 6: a TUI that uses synchronized output ------------------
# What current full-screen programs actually send (neovim, helix, lazygit,
# btop): each repaint bracketed by DECSET/DECRST 2026 so the terminal shows it
# whole. sink holds the bytes of an open update until it closes (see
# src/sync_output.hpp), so this exercises a path the plain cursor-heavy
# workload never reaches. Partial repaints, coloured, like a real redraw.
def gen_sync_tui():
    rng = random.Random(6)
    rows, cols = 50, 120
    frame = 0
    while True:
        out = [f"{ESC}[?2026h"]
        if frame % 20 == 0:
            out.append(f"{ESC}[H{ESC}[2J")
        for _ in range(rng.randint(5, 25)):
            r = rng.randint(1, rows)
            c = rng.randint(1, cols - 40)
            fg = rng.randint(16, 231)
            out.append(f"{ESC}[{r};{c}H{ESC}[38;5;{fg}m{ESC}[K")
            out.append("".join(rng.choice("abcdefghijklmnopqrstuvwxyz ") for _ in range(rng.randint(10, 38))))
        out.append(f"{ESC}[{rows};1H{ESC}[7m{'-- NORMAL --  frame ' + str(frame):<{cols}}{ESC}[0m")
        out.append(f"{ESC}[?2026l")
        frame += 1
        yield "".join(out)


# ---- Workload 7: grapheme clusters --------------------------------------
# Text whose visible characters are more than one codepoint: ZWJ emoji
# sequences, skin tones, flags, variation selectors, and stacked combining
# marks (some with a precomposed form, some without). sink keeps these whole
# in a cluster table (terminal_grid.hpp), which plain UTF-8 never touches.
def gen_graphemes():
    rng = random.Random(7)
    clusters = [
        "\U0001F468\u200D\U0001F469\u200D\U0001F467\u200D\U0001F466",  # family
        "\U0001F469\U0001F3FD\u200D\U0001F4BB",                          # technologist, tone
        "\U0001F3F3\uFE0F\u200D\U0001F308",                              # rainbow flag
        "\U0001F1EF\U0001F1F5", "\U0001F1F3\U0001F1FF",                  # flags
        "\u2764\uFE0F", "\u263A\uFE0F",                                  # VS16
        "e\u0301", "a\u0323\u0302", "o\u0308\u0304", "n\u0303",        # combining
        "\u0915\u094D\u0937",                                             # Devanagari conjunct
    ]
    words = ["status", "build", "deploy", "review", "merge", "ship"]
    i = 0
    while True:
        parts = []
        for _ in range(8):
            parts.append(rng.choice(words))
            parts.append(rng.choice(clusters))
        yield f"{i:06d} " + " ".join(parts) + "\r\n"
        i += 1


# ---- Workload 8: hyperlinks and shell-integration marks -----------------
# `ls --hyperlink` and a shell with OSC 133 prompt marks and OSC 7: the
# output of an ordinary interactive session with sink's shell integration on.
# Each link interns a URI (TerminalGrid::get_hyperlink_uri), each mark is
# recorded, and none of this is in the original four.
def gen_hyperlinks():
    rng = random.Random(8)
    names = ["main.cpp", "README.md", "Cargo.toml", "index.ts", "build", "notes.txt", "photo.heic"]
    n = 0
    while True:
        cwd = f"/Users/bench/project{n % 7}"
        out = [f"{ESC}]133;A{ESC}\\{ESC}]7;file://localhost{cwd}{ESC}\\",
               f"{ESC}[32m~/project{n % 7}{ESC}[0m % ", f"{ESC}]133;B{ESC}\\",
               "ls --hyperlink\r\n", f"{ESC}]133;C{ESC}\\"]
        for _ in range(rng.randint(3, 12)):
            name = rng.choice(names) + str(rng.randint(0, 999))
            out.append(f"{ESC}]8;;file://localhost{cwd}/{name}{ESC}\\{ESC}[34m{name}{ESC}[0m{ESC}]8;;{ESC}\\  ")
        out.append(f"\r\n{ESC}]133;D;0{ESC}\\")
        n += 1
        yield "".join(out)


# ---- Workload 9: inline images ------------------------------------------
# Sixel and kitty graphics interleaved with text, which is what sinkview,
# chafa, timg and notebook-style tools produce. Decoding dominates, so this is
# far below the text workloads in MB/s -- that is the point of measuring it.
# Every image asks for no reply (q=2), so the stream means the same thing in
# any terminal and never waits on one.
def _sixel(rng, w, h):
    out = [f"{ESC}Pq", f"\"1;1;{w};{h}"]
    colors = 4
    for c in range(colors):
        out.append(f"#{c};2;{rng.randint(0,100)};{rng.randint(0,100)};{rng.randint(0,100)}")
    for band in range((h + 5) // 6):
        for c in range(colors):
            out.append(f"#{c}")
            out.append("".join(chr(63 + rng.randint(0, 63)) for _ in range(w)))
            out.append("$")
        out.append("-")
    out.append(f"{ESC}\\")
    return "".join(out)


def _kitty(rng, image_id, w, h):
    raw = bytes(rng.randint(0, 255) for _ in range(w * h * 4))
    payload = base64.b64encode(zlib.compress(raw, 1) if image_id % 2 else raw).decode()
    chunks = [payload[i:i + 4096] for i in range(0, len(payload), 4096)]
    out = []
    for k, chunk in enumerate(chunks):
        more = 1 if k + 1 < len(chunks) else 0
        if k == 0:
            comp = ",o=z" if image_id % 2 else ""
            out.append(f"{ESC}_Ga=T,f=32,s={w},v={h},i={image_id},q=2{comp},m={more};{chunk}{ESC}\\")
        else:
            out.append(f"{ESC}_Gm={more};{chunk}{ESC}\\")
    return "".join(out)


def gen_images():
    rng = random.Random(9)
    n = 0
    while True:
        yield f"image {n}: sixel\r\n" + _sixel(rng, 96, 48) + "\r\n"
        yield f"image {n}: kitty\r\n" + _kitty(rng, n + 1, 48, 48) + "\r\n"
        for i in range(4):
            yield f"caption line {i} for image {n}\r\n"
        n += 1


WORKLOADS = [
    ("plain_text.bin", gen_plain_text),
    ("colored_text.bin", gen_colored_text),
    ("cursor_heavy.bin", gen_cursor_heavy),
    ("unicode_heavy.bin", gen_unicode_heavy),
    ("distinct_cjk.bin", gen_distinct_cjk),
    ("sync_tui.bin", gen_sync_tui),
    ("graphemes.bin", gen_graphemes),
    ("hyperlinks.bin", gen_hyperlinks),
    ("images.bin", gen_images),
]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def read_manifest():
    out = {}
    if os.path.exists(MANIFEST):
        for line in open(MANIFEST):
            if line.strip():
                digest, name = line.split()
                out[name] = digest
    return out


def check(out_dir):
    manifest = read_manifest()
    bad = 0
    for name, _ in WORKLOADS:
        path = os.path.join(out_dir, name)
        want = manifest.get(name)
        if not os.path.exists(path):
            print(f"MISSING  {name}")
            bad += 1
        elif want is None:
            print(f"UNPINNED {name} (not in {os.path.basename(MANIFEST)})")
            bad += 1
        elif sha256(path) != want:
            print(f"CHANGED  {name}: does not match the manifest")
            bad += 1
        else:
            print(f"ok       {name}")
    return bad


if __name__ == "__main__":
    args = sys.argv[1:]
    out_dir = OUT_DIR
    if "--out" in args:
        out_dir = args[args.index("--out") + 1]
    if "--check" not in args:
        os.makedirs(out_dir, exist_ok=True)
        global_out = OUT_DIR
        OUT_DIR = out_dir
        for i, (name, gen) in enumerate(WORKLOADS):
            write_workload(name, gen(), whole=i >= 5)
        OUT_DIR = global_out
    if "--update-manifest" in args:
        with open(MANIFEST, "w") as f:
            for name, _ in WORKLOADS:
                f.write(f"{sha256(os.path.join(out_dir, name))}  {name}\n")
        print(f"wrote {MANIFEST}")
    sys.exit(1 if check(out_dir) else 0)
