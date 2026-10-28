#pragma once

// sinkview: shows pictures in the terminal.
//
//     sinkview photo.jpg diagram.png
//     curl -s https://example.com/cat.png | sinkview
//
// It's sink's own binary run as `sink --view`; shells sink starts have a
// `sinkview` on their PATH that does that (see pty_bridge.cpp). Anything
// macOS can open works -- JPEG, PNG, HEIC, GIF (its first frame), WebP,
// TIFF -- turned upright the way Photos shows it, and shrunk to fit the
// window when it's bigger. The picture goes out as the kitty graphics
// protocol.

#include <cstdint>
#include <string>
#include <vector>

namespace sinkview {

// `sink --view <args>`: the exit code.
int run(const std::vector<std::string>& args);

// How big to show a picture: `px_w` x `px_h` pixels of it, across `cols`
// cells and down `rows`. Never bigger than the picture itself; shrunk,
// proportions kept, to fit `max_cols` x `max_rows` cells of `cell_w` x
// `cell_h` pixels.
struct Fit {
    int px_w = 0, px_h = 0;
    int cols = 0, rows = 0;
};
Fit fit(int image_w, int image_h, int max_cols, int max_rows, int cell_w, int cell_h);

// The kitty graphics escape sequences that show `rgba` (straight, not
// premultiplied, `w` x `h`) at the cursor `cols` cells wide, zlib-compressed
// and split into the protocol's chunks. Opaque pictures go as RGB, a quarter
// smaller. q=2, so the terminal doesn't answer into the shell.
std::string kitty_sequence(const std::vector<uint8_t>& rgba, int w, int h, int cols);

} // namespace sinkview
