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
- video backgrounds, HDR video included (mp4, mov, mkv), and pictures (JPEG, PNG, HEIC, WebP and more)
- exposure and hue shift controls, an optional CRT effect, and HDR text that glows brighter than the rest of your screen on an HDR display
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
- inline images with sixel and the kitty graphics protocol, and `sinkview` to show any picture right in your terminal
- the kitty keyboard protocol, clickable links, clipboard support, and synchronized output
- emoji, combining characters, and wide characters (CJK) that render properly, with font fallback so nothing shows up blank
- text that reflows live while you resize
- tested against tmux's own idea of what the screen should look like

## pictures in the terminal

`sinkview` shows pictures right where you're typing:

```sh
sinkview photo.jpg screenshot.png
curl -s https://example.com/cat.png | sinkview
```

anything macOS can open works (JPEG, PNG, HEIC, GIF, WebP, TIFF), turned the right way up, and shrunk to fit the window if it's bigger. it's there in every shell sink opens. it doesn't work inside tmux yet, which doesn't pass pictures through.

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
| ⌘W or ⇧⌘W | close pane (the tab, when it's the only pane) |
| ⌥⌘W | close tab, splits and all |
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

most things live in the settings window (⌘,): your background, font, exposure, hue shift, the CRT effect, HDR text, ligatures, the typing animation, shell integration, and presets.

under the hood it's all in `~/.config/sink/sink.toml`, and you can edit it by hand too. sink only ever rewrites the lines for settings you change in the settings window, so your comments and layout stay put. save the file and open windows pick up the change within a second.

```toml
version = 1
preset = "clouds"          # a built-in like "pool", or one of yours below

[terminal]
scrollback_lines = 10000

[shell]
integration = true         # false keeps sink's shell integration out of your shell
command = "fish"           # run something other than your login shell (a name or a path)
args = []                  # arguments for it
login = true               # start it as a login shell (-l); off for things like tmux
working_directory = "~"    # where new terminals start
                           # (new tabs and splits still open where you are)

[shell.env]                # environment variables for it
EDITOR = "nvim"

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
| `background` | `"pool"` | a video (`"pool"` or a path), or `"color"`, `"gradient"`, `"gel"`, `"clear"`, `"frosted glass"`, `"liquid glass"`, `"shader"`, `"image"` (see below) |
| `background_fit` | `"stretch"` | how a video or image meets the window: `"stretch"` fills it, `"fill"` fills it in proportion and crops, `"fit"` shows all of it in proportion |
| `font` | `"Monaspace Neon"` | an installed font by name (`"Menlo"`, `"Menlo Bold"`), the bundled font, or a path to a font file |
| `font_size` | `15` | in points. view > bigger and smaller change one window, default size comes back to this |
| `line_height` | `1` | row height as a multiple of the font size. `1.15` gives monaspace neon its roomier spacing |
| `exposure` | `0.7` | brightness of the background, 0 to 2 |
| `hue_shift` | `0` | rotates the background's colors, in degrees |
| `crt` | `false` | scanlines, curvature and glow |
| `hdr_text` | `false` | text brighter than SDR white on HDR displays |
| `ligatures` | `true` | draws `->`, `!=` and friends as single symbols |
| `title_bar` | `true` | the translucent title bar |
| `typing_animation` | `true` | output types itself onto the screen |
| `theme` | `"sink"` | the color theme (see below) |

themes color your text, cursor, selection, and the 16 colors programs pick from. sink ships with its own (`sink`) plus `tokyo night`, `catppuccin mocha`, `dracula`, `gruvbox dark`, `nord`, `rose pine`, `solarized dark`, and the light ones: `catppuccin latte`, `solarized light`, `rose pine dawn`, `gruvbox light`. switching themes recolors what's already on screen too. tweak any color of a theme in the preset's `colors` table:

```toml
[presets.clouds]
theme = "tokyo night"

[presets.clouds.colors]
foreground = "#e0e0ff"
cursor = "#ff9e64"
selection = "#7aa2f7"
background = "#1a1b26"   # what programs paint with; your video still shows through
palette = ["#15161e", "#f7768e"]   # replaces black and red, keeps the rest
```

backgrounds don't have to be videos. pick one from the type button in settings, or set it in the file:

- `color` fills the window with the theme's background color
- `gradient` blends from the theme's background toward its blue, or your own colors
- `gel` is a slowly churning nebula. with sink's own theme it's the original deep purple; with another theme it takes that theme's colors
- `clear`, `frosted glass` and `liquid glass` show your desktop through the window, as is, blurred, or through macOS's liquid glass
- `shader` runs your own GLSL shader (see below)
- `image` shows a picture: JPEG, PNG, HEIC, WebP, TIFF, GIF, anything macOS opens

videos and pictures stretch to fill the window by default. the fit button in settings (or `background_fit`) switches to `fill`, which keeps their proportions and crops the edges, or `fit`, which shows all of it with the theme's background around it.

```toml
[presets.nebula]
background = "gel"

[presets.nebula.gel]
speed = 0.5                      # 0 holds it still
colors = ["#020104", "#1c092b", "#4d1361", "#8f33a3", "#d18fe6"]   # depths to wisps

[presets.dusk]
background = "gradient"

[presets.dusk.gradient]
colors = ["#1a1b26", "#414868", "#7aa2f7"]
angle = 45                       # 0 is left to right, 90 top to bottom

[presets.postcard]
background = "image"

[presets.postcard.image]
file = "~/Pictures/big sur.heic"

[presets.see-through]
background = "frosted glass"

[presets.see-through.glass]
tint = 0.3                       # how much of the theme's background lies over the glass
```

exposure and hue shift work on all of them, not just videos.

### shader backgrounds

pick `shader` as the background and choose a `.frag` file. sink runs it the way [shadertoy](https://www.shadertoy.com) does, so most shaders from there work if you paste them into a file:

```glsl
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    fragColor = vec4(0.5 + 0.5 * cos(iTime + uv.xyx + vec3(0, 2, 4)), 1.0);
}
```

`fragCoord` is in pixels from the bottom left, and you get `iResolution`, `iTime`, `iTimeDelta`, `iFrameRate`, `iFrame`, `iMouse` (where the mouse is over the window) and `iDate`. shadertoy's textures (`iChannel0` and friends) aren't there, so shaders that read them won't load. save the file while sink is open and the background picks up the change; if it doesn't compile, settings shows the error and its line.

```toml
[presets.trippy]
background = "shader"

[presets.trippy.shader]
file = "~/shaders/plasma.frag"
```

changes you make in the settings window stick, even across restarts, but they don't touch your presets: they're kept in an `[edits]` table on top of the preset you're using, and the preset shows as "(edited)". **save** writes them into the preset, **new** saves the whole look as a new preset, and picking a preset from the list (even the one you're on) puts it back the way it was saved. `pool` is built in, so it can't be saved over, renamed or deleted.

```toml
preset = "clouds"

[edits]                  # changed in settings, not saved into clouds yet
exposure = 1.1
```

### keybindings

every shortcut is an action you can move, free up, or swap for a key that types something:

```toml
[keybindings]
"cmd+shift+e" = "split_right"         # moves split right off cmd+d
"cmd+k" = "none"                       # frees cmd+k
"cmd+u" = { text = "\u0015" }          # sends ctrl+u
"ctrl+alt+l" = { text = "ls -la\n" }
"⌥⌘←" = "previous_pane"               # keys can be written the way menus show them
```

moving an action frees its old key, since a menu can only show one shortcut (bind the old key too to keep both). the menus always show what the keys really do. `sink --show-config --default` lists every action with its default key. macOS keeps a few for itself (⌘Q, ⌘H, ⌘M, ⌘\`), so those can't be changed.

you can split your config across files. `include` reads other files first, as a base that the next one, and finally `sink.toml`, override setting by setting. handy for themes you share between machines, or keeping one machine's tweaks out of a dotfiles repo:

```toml
include = ["~/dotfiles/sink-shared.toml", "this-mac.toml"]   # relative paths start beside the file naming them
```

presets that only an included file has show up tagged "included". you can change them in settings (the changes go into `sink.toml`), but not rename or delete them, since sink never writes your included files.

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
