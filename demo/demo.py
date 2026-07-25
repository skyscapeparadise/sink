#!/usr/bin/env python3
"""
Sink Terminal Demo Script
=========================
Simulates terminal interaction replacing prompt with "moon@Thunderstorm ~ %",
typing "sing <song>", printing lyrics line-by-line over 3 seconds,
clearing the screen, repeating for coelacanth, snake, thunderstorms, you,
typing "swim with me", and playing splash.mp4 as terminal text video with:
  1. Full 100% opaque background cell coverage (completely blocking Sink's GPU video background, like cacademo).
  2. Dark charcoal '.' characters filling all negative space on solid black background.
  3. Vibrant 24-bit RGB truecolor foreground & background rendering across full ocean aquatic palette.
"""

import sys
import os
import time
import random
import subprocess

PROMPT = "moon@Thunderstorm ~ % "
LYRICS_DIR = "/Users/kady/Code/sinkdemo/lyrics"
SPLASH_VIDEO = "/Users/kady/Code/sinkdemo/splash.mp4"

# Character ramp for video area
CHARS = ".:;+=xX$#@█"
CHAR_LEN = len(CHARS)

def type_text(text, min_delay=0.03, max_delay=0.06):
    """Simulates realistic character-by-character typing."""
    for char in text:
        sys.stdout.write(char)
        sys.stdout.flush()
        time.sleep(random.uniform(min_delay, max_delay))

def print_prompt():
    """Prints the custom interactive prompt."""
    sys.stdout.write(PROMPT)
    sys.stdout.flush()

def clear_screen():
    """Clears the terminal screen and resets cursor position."""
    sys.stdout.write("\033[2J\033[H")
    sys.stdout.flush()

def sing_song(song_name):
    """
    Simulates typing 'sing <song_name>', hesitating 0.2s before hitting enter,
    and printing the lyrics line by line over exactly 3 seconds.
    """
    print_prompt()
    cmd_text = f"sing {song_name}"
    type_text(cmd_text)
    time.sleep(0.2)  # 0.2s hesitation before hitting enter
    sys.stdout.write("\n")
    sys.stdout.flush()

    filepath = os.path.join(LYRICS_DIR, f"{song_name}.txt")
    if not os.path.exists(filepath):
        sys.stdout.write(f"Song '{song_name}' not found.\n")
        sys.stdout.flush()
        return

    with open(filepath, "r", encoding="utf-8") as f:
        lines = f.readlines()

    if not lines:
        return

    # Print entire song over exactly 3.0 seconds
    line_delay = 3.0 / len(lines)
    for line in lines:
        sys.stdout.write(line)
        sys.stdout.flush()
        time.sleep(line_delay)

def type_clear_command():
    """
    Simulates prompt appearance, 0.2s delay, typing 'clear',
    0.2s hesitation, hitting enter, and clearing screen.
    """
    print_prompt()
    time.sleep(0.2)  # 0.2s delay before typing clear
    type_text("clear")
    time.sleep(0.2)  # 0.2s hesitation before hitting enter
    sys.stdout.write("\n")
    sys.stdout.flush()
    clear_screen()

def play_video_as_text(video_path):
    """
    Plays back the MP4 video inside the terminal as real-time 24-bit truecolor text,
    with 100% opaque cell backgrounds that completely block Sink's background video,
    and dark charcoal '.' characters filling negative space on solid black.
    """
    try:
        cols, rows = os.get_terminal_size()
    except Exception:
        cols, rows = 80, 24

    fps = 23.976
    frame_time = 1.0 / fps

    cmd = [
        "ffmpeg",
        "-i", video_path,
        "-vf", f"scale={cols}:{rows}",
        "-f", "rawvideo",
        "-pix_fmt", "rgb24",
        "-v", "error",
        "pipe:1"
    ]

    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    except Exception as e:
        sys.stdout.write(f"Error starting video playback: {e}\n")
        return

    frame_bytes = cols * rows * 3

    sys.stdout.write("\033[?25l\033[2J")  # Hide cursor & clear screen
    sys.stdout.flush()

    start_time = time.perf_counter()
    frame_idx = 0

    try:
        while True:
            raw = proc.stdout.read(frame_bytes)
            if not raw or len(raw) < frame_bytes:
                break

            target_time = start_time + frame_idx * frame_time
            now = time.perf_counter()
            sleep_dur = target_time - now
            if sleep_dur > 0:
                time.sleep(sleep_dur)

            lines = []
            for r in range(rows):
                row_str = []
                for c in range(cols):
                    idx = (r * cols + c) * 3
                    cr, cg, cb = raw[idx], raw[idx+1], raw[idx+2]
                    lum = 0.2126 * cr + 0.7152 * cg + 0.0722 * cb

                    if lum < 18:
                        # Negative space (vignette): Solid black background, dark charcoal '.' text
                        bg_r, bg_g, bg_b = 0, 0, 0
                        fg_r, fg_g, fg_b = 35, 38, 42
                        ch = '.'
                    else:
                        # Video content area: Rich aquatic background + vibrant foreground text
                        bg_r = max(0, int(cr * 0.45))
                        bg_g = max(0, int(cg * 0.45))
                        bg_b = max(0, int(cb * 0.45))

                        fg_r = min(255, int(cr * 1.25 + 15))
                        fg_g = min(255, int(cg * 1.25 + 15))
                        fg_b = min(255, int(cb * 1.25 + 15))

                        ch_idx = min(CHAR_LEN - 1, int((lum - 18) / 238.0 * CHAR_LEN))
                        ch = CHARS[ch_idx]

                    row_str.append(f"\033[48;2;{bg_r};{bg_g};{bg_b}m\033[38;2;{fg_r};{fg_g};{fg_b}m{ch}")
                lines.append("".join(row_str))

            sys.stdout.write("\033[H" + "\033[0m\n".join(lines) + "\033[0m")
            sys.stdout.flush()
            frame_idx += 1
    finally:
        proc.terminate()
        sys.stdout.write("\033[?25h\033[0m\n")  # Restore cursor
        sys.stdout.flush()

def main():
    clear_screen()

    # Song sequence: coelacanth -> clear -> snake -> clear -> thunderstorms -> clear -> you -> clear
    songs = ["coelacanth", "snake", "thunderstorms", "you"]
    for song in songs:
        sing_song(song)
        type_clear_command()

    # Finale sequence: 1 second pause -> type "swim with me" -> 0.2s hesitation -> enter -> 2 second pause -> video as text
    print_prompt()
    time.sleep(1.0)  # 1 second pause before typing
    type_text("swim with me")
    time.sleep(0.2)  # 0.2s hesitation before hitting enter
    sys.stdout.write("\n")
    sys.stdout.flush()

    time.sleep(2.0)  # 2 second pause before video playback

    # Play video as text with 100% opaque cell backgrounds and filled negative space
    play_video_as_text(SPLASH_VIDEO)

    # Return to prompt after video ends
    print_prompt()
    sys.stdout.write("\n")
    sys.stdout.flush()

if __name__ == "__main__":
    main()
