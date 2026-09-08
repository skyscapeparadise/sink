#!/usr/bin/env python3
"""Generates fixed-content binary workload files used by both the sink
(C++) and vte (Rust) parser benchmarks, so both sides measure the exact
same input bytes rather than two independently-generated approximations of
"similar" content.

Each workload targets ~8MB, large enough that per-call overhead is
negligible relative to total parse time, small enough that the whole suite
runs in a few seconds.
"""
import os
import random

OUT_DIR = os.path.join(os.path.dirname(__file__), "workloads")
TARGET_BYTES = 8 * 1024 * 1024

random.seed(1234)  # deterministic content -> reproducible benchmark runs

ESC = "\x1b"


def write_workload(name: str, chunks):
    """`chunks` is a generator yielding str fragments; keeps producing until
    TARGET_BYTES is reached, then truncates to exactly that length."""
    path = os.path.join(OUT_DIR, name)
    total = 0
    with open(path, "wb") as f:
        for chunk in chunks:
            b = chunk.encode("utf-8")
            if total + len(b) > TARGET_BYTES:
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


if __name__ == "__main__":
    os.makedirs(OUT_DIR, exist_ok=True)
    write_workload("plain_text.bin", gen_plain_text())
    write_workload("colored_text.bin", gen_colored_text())
    write_workload("cursor_heavy.bin", gen_cursor_heavy())
    write_workload("unicode_heavy.bin", gen_unicode_heavy())
    write_workload("distinct_cjk.bin", gen_distinct_cjk())
