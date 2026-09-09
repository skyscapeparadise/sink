#pragma once

#include <SDL3/SDL.h>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

// Inline images, shared by the two protocols that produce them: sixel (DCS,
// see ansi_parser's sixel decoder) and the kitty graphics protocol (APC).
//
// The wire formats have nothing in common; what they need from the terminal is
// the same. An image is pixels; a placement is that image pinned to a position
// in the scrollback so it moves with the text it was printed next to, and dies
// when that text falls out of history.
//
// Textures are created lazily on first draw, because decoding happens on the
// parse path where there is no renderer, and uploading every image whether or
// not it is ever on screen would make a `cat` of a hundred images cost a
// hundred texture uploads.
struct TerminalImage {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels; // RGBA32, width*height
    SDL_Texture* texture = nullptr;
    bool upload_failed = false;   // don't retry a texture that already failed

    size_t byte_size() const { return pixels.size() * sizeof(uint32_t); }
};

// One appearance of an image on screen.
//
// `line_id` is a scrollback-absolute line number, not a screen row: screen rows
// renumber constantly as output scrolls, and an image pinned to one would slide
// up the screen while the text around it stayed put. Line ids never change and
// never repeat, so a placement stays attached to its line for as long as that
// line exists and can be discarded the moment it does not.
struct ImagePlacement {
    uint64_t image_id = 0;
    uint64_t placement_id = 0; // 0 unless the protocol named it (kitty does)
    uint64_t line_id = 0;      // absolute line of the placement's top row
    int col = 0;               // leftmost column
    int cols = 0;              // extent in cells
    int rows = 0;
    // Source rectangle within the image, in pixels. The kitty protocol can
    // place a crop of an image; sixel always places the whole thing.
    int src_x = 0, src_y = 0, src_w = 0, src_h = 0;
    int z = 0;                 // draw order; negative sits behind text
};

// Decodes a sixel payload -- everything after the DCS introducer's 'q' -- into
// RGBA pixels. `background_transparent` is what the introducer's P2 asked for:
// with it set, pixels no sixel touched stay clear rather than being painted in
// the current background colour.
//
// Returns false when nothing decodable was found. Malformed input stops at the
// point it goes wrong and keeps whatever decoded cleanly up to there, because
// a truncated image is a better outcome than none and the payload arrives from
// a pty that can be cut off mid-stream.
bool decode_sixel(const char* data, size_t size, bool background_transparent,
                  std::vector<uint32_t>& out_pixels, int& out_width, int& out_height);

// Decodes PNG bytes into RGBA, for the kitty graphics protocol's f=100.
// Returns false on anything SDL3_image will not read.
bool decode_png(const void* data, size_t size,
                std::vector<uint32_t>& out_pixels, int& out_width, int& out_height);

class TerminalImages {
public:
    ~TerminalImages();

    // Stores pixels under `id`, replacing anything already there. An id of 0
    // means "pick one", which is what sixel needs since it names nothing.
    // Returns the id actually used, or 0 if the image was rejected.
    uint64_t store(uint64_t id, int width, int height, std::vector<uint32_t>&& pixels);

    bool has_image(uint64_t id) const { return images_.count(id) != 0; }
    const TerminalImage* find(uint64_t id) const;

    void place(const ImagePlacement& placement);

    // Everything currently placed, in draw order (ascending z, then age).
    const std::vector<ImagePlacement>& placements() const { return placements_; }

    // Drops placements whose line has scrolled out of history, then any image
    // no placement refers to any more. Called as scrollback is trimmed.
    void retire_before(uint64_t oldest_line_id);

    void clear_placements();
    void clear_all();

    // The alternate screen gets its own set: entering vim should not discard
    // what was printed to the shell, and line ids on the two screens overlap
    // because both are addressed as active rows.
    std::vector<ImagePlacement> take_placements();
    void set_placements(std::vector<ImagePlacement> placements);

    // Deletes by id, which is how the kitty protocol's delete commands work.
    void delete_image(uint64_t id);
    void delete_placement(uint64_t image_id, uint64_t placement_id);

    // Uploads on first use. Null if the image is gone or will not upload.
    SDL_Texture* texture_for(SDL_Renderer* renderer, uint64_t id);

    // Textures belong to a renderer; a window that recreates its own must be
    // able to say so before the old one is destroyed.
    void release_textures();

    size_t image_count() const { return images_.size(); }
    size_t total_bytes() const { return total_bytes_; }

    // Ceiling on decoded pixel data held at once. Untrusted output can send
    // images as fast as the terminal will take them, so this is a cap and not
    // a guideline; the least recently stored image is dropped to stay under it.
    static constexpr size_t kMaxBytes = 96u * 1024 * 1024;
    // A single image is also capped, well below the total, so one absurd
    // raster attribute cannot evict everything else on its way in.
    static constexpr size_t kMaxImagePixels = 8192u * 8192u;

private:
    std::unordered_map<uint64_t, TerminalImage> images_;
    std::deque<uint64_t> store_order_; // oldest first, for eviction
    std::vector<ImagePlacement> placements_;
    size_t total_bytes_ = 0;
    uint64_t next_auto_id_ = 1;

    void evict_until_fits(size_t incoming);
    void drop_image(uint64_t id);
    void sort_placements();
};
