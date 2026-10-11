// Fuzzes sink's image decoders directly: sixel (sink's own decoder) and PNG
// (SDL_image, which kitty-protocol images reach -- so a remote host can make
// sink decode a PNG of its choosing). The terminal fuzzer reaches both through
// DCS and APC sequences too; feeding the decoders directly gets far more of
// their own code covered per second.
//
// Checks, besides memory safety: decoded dimensions are within sink's limits
// and the pixel buffer is exactly as large as they say.
//
// The first input byte picks the decoder. Pure computation; see README.md.

#include "terminal_images.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

[[noreturn]] void fail(const char* what, int w, int h, size_t n) {
    std::fprintf(stderr, "PROPERTY VIOLATED: %s (%dx%d, %zu pixels)\n", what, w, h, n);
    std::abort();
}

void check(bool ok, const std::vector<uint32_t>& pixels, int w, int h) {
    if (!ok) return;
    if (w <= 0 || h <= 0) fail("decoded image has no area", w, h, pixels.size());
    if (static_cast<size_t>(w) * static_cast<size_t>(h) > TerminalImages::kMaxImagePixels) {
        fail("decoded image exceeds the pixel limit", w, h, pixels.size());
    }
    if (pixels.size() != static_cast<size_t>(w) * static_cast<size_t>(h)) {
        fail("pixel buffer does not match dimensions", w, h, pixels.size());
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return 0;
    const uint8_t which = data[0];
    const char* body = reinterpret_cast<const char*>(data + 1);
    const size_t len = size - 1;

    std::vector<uint32_t> pixels;
    int w = 0, h = 0;
    if (which & 1) {
        check(decode_png(body, len, pixels, w, h), pixels, w, h);
    } else {
        check(decode_sixel(body, len, (which & 2) != 0, pixels, w, h), pixels, w, h);
    }
    return 0;
}
