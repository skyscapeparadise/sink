#pragma once

// Opening pictures with macOS's own decoders (ImageIO): JPEG, PNG, HEIC, GIF
// (its first frame), WebP, TIFF, BMP and whatever else macOS knows. Used by
// sinkview and by image backgrounds.
//
// Pictures come out upright, the way Photos shows them (EXIF orientation
// applied), in sRGB, as straight (not premultiplied) RGBA.

#include <cstdint>
#include <string>
#include <vector>

namespace picture {

struct Pixels {
    std::vector<uint8_t> rgba;  // w * h * 4, straight alpha
    int w = 0, h = 0;
};

class Source {
public:
    static Source from_file(const std::string& path);
    static Source from_bytes(const std::string& bytes);
    ~Source();
    Source(Source&& o) noexcept : ref_(o.ref_) { o.ref_ = nullptr; }
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;
    Source& operator=(Source&&) = delete;

    // False when macOS can't open it as a picture.
    bool ok() const;
    // Its size upright, without decoding it. False if it has none.
    bool size(int& w, int& h) const;
    // Decodes it, at most `max_side` pixels on its longer side.
    bool decode(int max_side, Pixels& out) const;

private:
    explicit Source(void* ref) : ref_(ref) {}
    void* ref_ = nullptr;  // a CGImageSourceRef
};

// Whether a path names a picture, by its extension -- for telling a picture
// from a video where a setting could be either.
bool looks_like_picture(const std::string& path);

} // namespace picture
