https://github.com/user-attachments/assets/8023f170-16b9-4d54-ac7d-f3e399dfa444





# sink

an hdr video terminal for underwater programming

**sink** is a stupidly fast, lightweight GPU accelerated terminal emulator for macOS (and eventually other platforms as well) that supports word processor style text editing with a mouse for fast and easy manipulation of commands without having to use any modifier or arrow keys. Under the hood is an SDL3 powered rendering engine that supports custom image and video backgrounds including HDR content!

## getting sink

grab the latest `.dmg` from [releases](https://github.com/skyscapeparadise/sink/releases), open it, and drag **sink** into your Applications folder.

you'll need **macOS 26 or later** on a Mac with **Apple silicon**.

sink is still in beta, so you might hit a rough edge here and there. if you do, [open an issue](https://github.com/skyscapeparadise/sink/issues) and let me know!

## what's in the water

**make it yours**
- image and video backgrounds, HDR video included (mp4, mov, mkv, png, jpg, bmp)
- exposure and hue shift controls, an optional CRT shader, and HDR text that glows brighter than the rest of your screen on an HDR display
- presets, so you can switch between whole looks in one go
- cursor typing animation, ligatures, and whatever font you like

**edit commands like a word processor**
- click anywhere in a command to put the cursor there, even in multi-line and wrapped commands
- select text and delete or cut it right out of the command line
- clicking on a zsh autosuggestion won't accidentally accept it

**shell integration, built in**
- works with zsh and bash out of the box, no setup
- ⌘↑ and ⌘↓ jump between prompts, and new tabs and splits open in whatever folder you're in
- your dotfiles load exactly like they normally do (and if you'd rather it stayed out of your shell, see [config](#config))

**all the modern terminal stuff**
- splits and tabs
- inline images with sixel and the kitty graphics protocol
- the kitty keyboard protocol, clickable links, clipboard support, and synchronized output
- emoji, combining characters, and wide characters (CJK) that render properly, with font fallback so nothing shows up blank
- text that reflows live while you resize
- tested against tmux's own idea of what the screen should look like

## demos

- `sinkdemo` runs the whole show
- `sinksing coelacanth` sings you a skyscape paradise song (try `snake`, `sink`, or `you` too)
- press tab to skip ahead

## shortcuts

| shortcut | what it does |
| --- | --- |
| ⌘N | new window |
| ⌘T | new tab |
| ⌘1 – ⌘9 | jump to a tab (⌘9 is the last one) |
| ⌘D | split pane right |
| ⇧⌘D | split pane down |
| ⌥⌘ + arrow keys | move between panes |
| ⌘] / ⌘[ | next / previous pane |
| ⇧⌘↩ | zoom a pane to fill the window (and back) |
| ⇧⌘W | close pane |
| ⌘W | close window |
| ⌘↑ / ⌘↓ | jump to the previous / next prompt |
| ⌘K | clear the screen and scrollback |
| ⌘= / ⌘- / ⌘0 | bigger / smaller / default text size |
| ⌘F | find |
| ⌘, | settings |
| ⌘P | print |

and the editing keys your fingers already know from other Mac terminals:

| shortcut | what it does |
| --- | --- |
| ⌥← / ⌥→ | move back / forward a word |
| ⌘← / ⌘→ | jump to the start / end of the line |
| ⌥⌫ | delete the word before the cursor |
| ⌘⌫ | delete everything before the cursor |

## config

most things live in the settings window (⌘,): your background, font, exposure, hue shift, the CRT shader, HDR text, ligatures, the typing animation, and presets.

under the hood it's all in `~/.config/sink/sink.toml`, and you can edit it by hand too. sink only ever rewrites the lines for settings you change in the settings window, so your comments and layout stay put. save the file and open windows pick up the change within a second.

```toml
version = 1
preset = "clouds"          # a built-in like "pool", or one of yours below

[terminal]
scrollback_lines = 10000

[shell]
integration = true         # false keeps sink's shell integration out of your shell

[keyboard]
option_as_meta = false     # true makes option act as meta (for emacs and readline),
                           # or "left" / "right" for just one option key

# your presets. each one only lists what it changes.
[presets.clouds]
background = "~/Videos/dry ice.mp4"
exposure = 0.83
hue_shift = 70
crt = true
```

a preset can set:

| key | default | |
|---|---|---|
| `background` | `"pool"` | a built-in background, or a path to a video |
| `font` | `"Monaspace Neon"` | the bundled font, or a path to a font file |
| `exposure` | `0.7` | brightness of the background, 0 to 2 |
| `hue_shift` | `0` | rotates the background's colors, in degrees |
| `crt` | `false` | scanlines, curvature and glow |
| `hdr_text` | `false` | text brighter than SDR white on HDR displays |
| `ligatures` | `true` | draws `->`, `!=` and friends as single symbols |
| `title_bar` | `true` | the translucent title bar |
| `typing_animation` | `true` | output types itself onto the screen |

`pool` is built in, so you can't rename or delete it. change something while it's active and sink saves your changes as `pool copy`.

if the file has a mistake, sink tells you which line in the settings window and keeps using your last good settings, and it won't save over the file until you fix it. setting `SINK_CONFIG` to another path makes sink use that file instead.

coming from 0.11 or earlier? the first launch converts your file to this format and keeps the original as `sink.toml.v0.bak`.

sink can also tell you about your config from the command line, without opening a window:

```sh
alias sink=/Applications/sink.app/Contents/MacOS/sink
sink --show-config --default   # every setting at its default, with what it does
sink --show-config             # your config as sink reads it, defaults filled in
sink --check-config            # any problems, by line (exits 1 if there are some)
```

editors with TOML support (VS Code, Zed, Neovim, anything using taplo) autocomplete and check `sink.toml` against [sink.schema.json](sink.schema.json). files sink creates already point at it on their first line; for one of your own, add:

```toml
#:schema https://raw.githubusercontent.com/skyscapeparadise/sink/main/sink.schema.json
```

## security

anything you `cat`, any command's output, and any server you `ssh` into can talk to your terminal, so sink is careful about what it lets them do:

- programs can put things on your clipboard, but they can't read it back
- sink won't echo your window title back to programs, which is a classic way to sneak commands into a shell
- pasted text can't break out of bracketed paste and run commands on its own
- images can't make sink open files on your machine
- the parser and image decoders get fuzzed with millions of nasty inputs

found a security problem? please email **support@rainmultimedia.net** instead of opening a public issue.

## building from source

you'll need Xcode 26 or later and [Homebrew](https://brew.sh):

```sh
brew install cmake pkg-config sdl3 freetype
./build.sh
```

the first build downloads and builds LGPL-only versions of FFmpeg, SDL3_image, HarfBuzz, and SDL3_ttf, so it takes a little while. after that, `./build/sink` runs it, and `./releases/package.sh` wraps it up into `sink.app`.

to run the tests:

```sh
ctest --test-dir build                     # everything
ctest --test-dir build -LE integration     # just the quick ones
```

the integration tests drive real shells (and tmux, if you have it installed), and the bash ones want a newer bash from Homebrew. the fuzzers have their own instructions in [tests/fuzz](tests/fuzz/README.md).

## roadmap

○ linux version

○ windows version

## license

**sink** is free software released under the [GNU Lesser General Public License v2.1](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.en.html) or later. the libraries it bundles (FFmpeg, SDL3, SDL3_image, SDL3_ttf, FreeType, HarfBuzz, and libpng) keep their own licenses, and so do its fonts, Monaspace Neon and Mona Sans (both under the SIL Open Font License). copies of all of them ship inside the app.

Copyright © 2026 Rain Multimedia LLC
