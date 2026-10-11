#!/usr/bin/env python3
"""Writes the fuzzers' starting inputs into tests/fuzz/seeds/.

A fuzzer mutates what it is given, so it starts far deeper with examples of
every kind of sequence sink handles than with random bytes. fuzz_terminal
reads its input as operations (see fuzz_terminal.cpp); these seeds encode
each sample as a grid setup plus one "parse these bytes" operation, so the
fuzzer begins from valid structure and mutates the payloads.

    python3 -I tests/fuzz/make_seeds.py
"""
import base64, os, struct, zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "seeds")
ESC = b"\x1b"
CSI = ESC + b"["
OSC = ESC + b"]"
ST = ESC + b"\\"
BEL = b"\x07"


def u16(v):
    return struct.pack("<H", v)


def terminal_seed(payload, cols=40, rows=12, scrollback=50):
    """Grid setup, then one parse operation carrying `payload`."""
    # range(1,100) -> cols, range(1,40) -> rows, range(0,200) -> scrollback:
    # each reads two bytes, value = lo + v % span.
    out = u16(cols - 1) + u16(rows - 1) + u16(scrollback)
    # op 0 = parse; the length is read as range(0, 4096).
    for i in range(0, len(payload), 4096):
        chunk = payload[i:i + 4096]
        out += b"\x00" + u16(len(chunk)) + chunk
    return out


def png_1x1():
    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    ihdr = struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)
    idat = zlib.compress(b"\x00\xff\x00\x00\xff")
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")


SIXEL = b"#0;2;100;0;0#1;2;0;100;0\"1;1;6;6#0!6~-#1!6~$#0??~~??-"

TERMINAL = {
    "sgr": CSI + b"1;31mred" + CSI + b"38;5;196mx" + CSI + b"38;2;10;20;30;48:5:4my" + CSI + b"0m\r\nplain\r\n",
    "cursor": CSI + b"5;10H" + CSI + b"2A" + CSI + b"3C" + CSI + b"s" + CSI + b"u" + CSI + b"10G" + ESC + b"7" + ESC + b"8",
    "erase": b"aaaaaaaaaabbbbbbbbbb" + CSI + b"1;5H" + CSI + b"K" + CSI + b"1K" + CSI + b"2K" + CSI + b"J" + CSI + b"1J" + CSI + b"2J" + CSI + b"3J",
    "edit": b"hello world" + CSI + b"5D" + CSI + b"3@" + CSI + b"2P" + CSI + b"4X" + CSI + b"2L" + CSI + b"1M" + b"x" + CSI + b"5b",
    "regions": CSI + b"3;8r" + CSI + b"?6h" + b"in region\n\n\n\n\n\n" + CSI + b"2S" + CSI + b"1T" + CSI + b"r" + CSI + b"?6l",
    "modes": CSI + b"?1049h" + b"alt" + CSI + b"?1049l" + CSI + b"?2004h" + CSI + b"?1000h" + CSI + b"?1006h" + CSI + b"?1h" + ESC + b"=" + ESC + b">" + CSI + b"?2026h" + CSI + b"?2026l" + CSI + b"!p" + ESC + b"c",
    "tabs": b"a\tb" + ESC + b"H" + CSI + b"3g" + CSI + b"0g" + b"\tc" + CSI + b"2I" + CSI + b"1Z",
    "charset": ESC + b"(0lqqk\r\nx  x\r\nmqqj" + ESC + b"(B",
    "osc": OSC + b"0;title" + BEL + OSC + b"2;win" + ST + OSC + b"7;file://localhost/tmp" + ST
           + OSC + b"8;id=1;https://example.com" + ST + b"link" + OSC + b"8;;" + ST
           + OSC + b"52;c;aGVsbG8=" + BEL + OSC + b"52;c;?" + BEL
           + OSC + b"133;A" + BEL + b"$ " + OSC + b"133;B" + BEL + b"ls" + OSC + b"133;C" + BEL,
    "colours": OSC + b"4;1;rgb:ff/00/00;2;#00ff00" + ST + OSC + b"4;1;?" + ST + OSC + b"10;?" + BEL
               + OSC + b"11;?" + ST + OSC + b"12;#123" + BEL + OSC + b"104;1" + ST + OSC + b"110" + ST,
    "queries": CSI + b"6n" + CSI + b"?6n" + CSI + b"5n" + CSI + b"c" + CSI + b">c" + CSI + b"?2026$p"
               + CSI + b"4$p" + CSI + b"14t" + CSI + b"16t" + CSI + b"18t" + CSI + b"19t" + CSI + b"?u"
               + CSI + b">1u" + CSI + b"<u" + CSI + b"=1;1u",
    "sixel": ESC + b"Pq" + SIXEL + ST + ESC + b"P0;1;0q" + SIXEL + ST,
    "kitty": ESC + b"_Ga=T,f=24,s=2,v=2;" + base64.b64encode(b"\xff\x00\x00" * 4) + ST
             + ESC + b"_Ga=t,i=7,f=32,s=1,v=1,m=1;" + base64.b64encode(b"\x00\x00") + ST
             + ESC + b"_Gm=0;" + base64.b64encode(b"\x00\xff") + ST
             + ESC + b"_Ga=p,i=7,p=2,c=2,r=1" + ST + ESC + b"_Ga=q,i=9;" + ST
             + ESC + b"_Ga=d,d=i,i=7" + ST + ESC + b"_Ga=t,t=f;" + base64.b64encode(b"/nonexistent/sink-fuzz") + ST,
    "kitty_png": ESC + b"_Ga=T,f=100;" + base64.b64encode(png_1x1()) + ST,
    "utf8": "wide 日本語 emoji 🎉 zwj 👩‍👩‍👧 combining é flags 🇯🇵 ".encode() + b"bad \xff\xfe\xc3 \xe2\x82",
    # What zsh sent when a multi-line command's first line filled the row
    # exactly -- the redraw that exposed the ED bug (see the git history).
    "zsh_redraw": b"ps1> " + ESC + b"[7mecho " + b"x" * 30 + ESC + b"[27m \x1b[K\r\r\n" + ESC + b"[7mecho second" + ESC + b"[27m\x1b[K"
                  + ESC + b"[A\r\r\x1b[0m\x1b[27m\x1b[24m\x1b[Jps1> echo again" + ESC + b"[K",
}


def write(sub, name, data):
    d = os.path.join(ROOT, sub)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name), "wb") as f:
        f.write(data)


for name, payload in TERMINAL.items():
    write("terminal", name, terminal_seed(payload))
# A narrow grid for the same content, where wrapping and edges dominate.
write("terminal", "narrow_osc", terminal_seed(TERMINAL["osc"] + TERMINAL["utf8"], cols=3, rows=2))

# fuzz_images: first byte picks the decoder (odd = PNG, even = sixel; bit 1
# makes sixel's background transparent).
write("images", "sixel", b"\x00" + SIXEL)
write("images", "sixel_transparent", b"\x02" + SIXEL)
write("images", "png", b"\x01" + png_1x1())

print("seeds written to", ROOT)
