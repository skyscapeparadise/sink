#include "terminal_images.hpp"

#include <algorithm>
#include <cmath>

TerminalImages::~TerminalImages() {
    release_textures();
}

void TerminalImages::release_textures() {
    for (auto& entry : images_) {
        if (entry.second.texture) {
            SDL_DestroyTexture(entry.second.texture);
            entry.second.texture = nullptr;
        }
        entry.second.upload_failed = false;
    }
}

const TerminalImage* TerminalImages::find(uint64_t id) const {
    auto it = images_.find(id);
    return it == images_.end() ? nullptr : &it->second;
}

void TerminalImages::drop_image(uint64_t id) {
    auto it = images_.find(id);
    if (it == images_.end()) return;
    if (it->second.texture) SDL_DestroyTexture(it->second.texture);
    total_bytes_ -= it->second.byte_size();
    images_.erase(it);
    store_order_.erase(std::remove(store_order_.begin(), store_order_.end(), id),
                       store_order_.end());
    placements_.erase(std::remove_if(placements_.begin(), placements_.end(),
                                     [id](const ImagePlacement& p) { return p.image_id == id; }),
                      placements_.end());
}

void TerminalImages::evict_until_fits(size_t incoming) {
    // Oldest first. An image still on screen can be evicted, which is a visible
    // loss -- but the alternative is letting a stream of images grow without
    // bound, and a missing picture is a better failure than an unusable
    // machine. In practice the cap is far above any realistic working set.
    while (!store_order_.empty() && total_bytes_ + incoming > kMaxBytes) {
        drop_image(store_order_.front());
    }
}

uint64_t TerminalImages::store(uint64_t id, int width, int height,
                               std::vector<uint32_t>&& pixels) {
    if (width <= 0 || height <= 0) return 0;
    size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (pixel_count > kMaxImagePixels || pixels.size() < pixel_count) return 0;

    size_t incoming = pixel_count * sizeof(uint32_t);
    if (incoming > kMaxBytes) return 0;

    if (id == 0) {
        id = next_auto_id_++;
        // Ids the terminal picks stay clear of the low numbers a protocol
        // client is likely to choose for itself.
        if (next_auto_id_ < 0x4000'0000ull) next_auto_id_ = 0x4000'0000ull;
    }

    drop_image(id); // replacing an existing id
    evict_until_fits(incoming);

    TerminalImage image;
    image.width = width;
    image.height = height;
    image.pixels = std::move(pixels);
    image.pixels.resize(pixel_count);
    total_bytes_ += image.byte_size();
    images_.emplace(id, std::move(image));
    store_order_.push_back(id);
    return id;
}

void TerminalImages::sort_placements() {
    // Stable, so equal-z placements keep the order they arrived in and a later
    // image drawn at the same spot lands on top of an earlier one.
    std::stable_sort(placements_.begin(), placements_.end(),
                     [](const ImagePlacement& a, const ImagePlacement& b) { return a.z < b.z; });
}

void TerminalImages::place(const ImagePlacement& placement) {
    if (!has_image(placement.image_id)) return;
    if (placement.cols <= 0 || placement.rows <= 0) return;

    // A placement id names a slot: sending the same one again moves the image
    // rather than stacking a second copy, which is how the kitty protocol
    // expects an application to update a picture in place.
    if (placement.placement_id != 0) {
        placements_.erase(
            std::remove_if(placements_.begin(), placements_.end(),
                           [&](const ImagePlacement& p) {
                               return p.image_id == placement.image_id &&
                                      p.placement_id == placement.placement_id;
                           }),
            placements_.end());
    }
    placements_.push_back(placement);
    sort_placements();
}

void TerminalImages::retire_before(uint64_t oldest_line_id) {
    size_t before = placements_.size();
    placements_.erase(
        std::remove_if(placements_.begin(), placements_.end(),
                       [&](const ImagePlacement& p) {
                           // The whole placement is gone only once its *last*
                           // row has fallen out; one still half on screen keeps
                           // drawing the part that remains.
                           return p.line_id + static_cast<uint64_t>(p.rows) <= oldest_line_id;
                       }),
        placements_.end());
    if (placements_.size() == before) return;

    // An image nothing refers to any more is dead weight, and images are the
    // large allocation here -- a placement is a few dozen bytes.
    for (auto it = images_.begin(); it != images_.end();) {
        uint64_t id = it->first;
        bool referenced = std::any_of(placements_.begin(), placements_.end(),
                                      [id](const ImagePlacement& p) { return p.image_id == id; });
        if (referenced) {
            ++it;
            continue;
        }
        if (it->second.texture) SDL_DestroyTexture(it->second.texture);
        total_bytes_ -= it->second.byte_size();
        store_order_.erase(std::remove(store_order_.begin(), store_order_.end(), id),
                           store_order_.end());
        it = images_.erase(it);
    }
}

void TerminalImages::clear_placements() {
    placements_.clear();
}

std::vector<ImagePlacement> TerminalImages::take_placements() {
    std::vector<ImagePlacement> out;
    out.swap(placements_);
    return out;
}

void TerminalImages::set_placements(std::vector<ImagePlacement> placements) {
    placements_ = std::move(placements);
    // Anything whose image was evicted while the other screen was up is
    // dropped rather than drawn as a hole.
    placements_.erase(std::remove_if(placements_.begin(), placements_.end(),
                                     [this](const ImagePlacement& p) {
                                         return !has_image(p.image_id);
                                     }),
                      placements_.end());
    sort_placements();
}

void TerminalImages::clear_all() {
    release_textures();
    images_.clear();
    store_order_.clear();
    placements_.clear();
    total_bytes_ = 0;
}

void TerminalImages::delete_image(uint64_t id) {
    drop_image(id);
}

void TerminalImages::delete_placement(uint64_t image_id, uint64_t placement_id) {
    placements_.erase(
        std::remove_if(placements_.begin(), placements_.end(),
                       [&](const ImagePlacement& p) {
                           if (p.image_id != image_id) return false;
                           return placement_id == 0 || p.placement_id == placement_id;
                       }),
        placements_.end());
}

SDL_Texture* TerminalImages::texture_for(SDL_Renderer* renderer, uint64_t id) {
    auto it = images_.find(id);
    if (it == images_.end() || !renderer) return nullptr;
    TerminalImage& image = it->second;
    if (image.texture) return image.texture;
    if (image.upload_failed) return nullptr;

    image.texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                      SDL_TEXTUREACCESS_STATIC, image.width, image.height);
    if (!image.texture) {
        image.upload_failed = true;
        return nullptr;
    }
    SDL_SetTextureBlendMode(image.texture, SDL_BLENDMODE_BLEND);
    // Nearest, not linear: an image scaled to a whole number of cells is
    // usually being shown at or near its natural size, and smoothing it makes
    // pixel art and terminal-rendered plots look worse rather than better.
    SDL_SetTextureScaleMode(image.texture, SDL_SCALEMODE_NEAREST);
    if (!SDL_UpdateTexture(image.texture, nullptr, image.pixels.data(),
                           image.width * static_cast<int>(sizeof(uint32_t)))) {
        SDL_DestroyTexture(image.texture);
        image.texture = nullptr;
        image.upload_failed = true;
        return nullptr;
    }
    return image.texture;
}

// --- sixel ----------------------------------------------------------------
//
// A sixel is one character encoding a vertical strip of six pixels: bit 0 is
// the topmost. Bands of six run left to right; '-' starts the next band, '$'
// returns to the left margin of the current one. '#' selects or defines a
// colour, '!' repeats the next sixel, '"' declares the raster size.

namespace {

// VT340 default palette, as RGB percentages -- which is how the format itself
// expresses colour, so they are converted the same way a '#' definition is.
constexpr uint8_t kSixelDefaultPalette[16][3] = {
    {  0,  0,  0 }, { 20, 20, 80 }, { 80, 13, 13 }, { 20, 80, 20 },
    { 80, 20, 80 }, { 20, 80, 80 }, { 80, 80, 20 }, { 53, 53, 53 },
    { 26, 26, 26 }, { 33, 33, 60 }, { 60, 26, 26 }, { 33, 60, 33 },
    { 60, 33, 60 }, { 33, 60, 60 }, { 60, 60, 33 }, { 80, 80, 80 },
};

inline uint8_t percent_to_byte(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return static_cast<uint8_t>((percent * 255 + 50) / 100);
}

inline uint32_t pack_rgba(uint8_t r, uint8_t g, uint8_t b) {
    // Matches SDL_PIXELFORMAT_RGBA32, which is byte-order R,G,B,A.
    return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(b) << 16) | (0xFFu << 24);
}

// HLS as sixel defines it: hue in degrees where 0 is blue, lightness and
// saturation as percentages. Not the same convention as HSL elsewhere, which
// is why this is spelled out rather than reached for from a library.
uint32_t hls_to_rgba(int h, int l, int s) {
    double lf = std::min(100, std::max(0, l)) / 100.0;
    double sf = std::min(100, std::max(0, s)) / 100.0;
    double hf = static_cast<double>(((h % 360) + 360) % 360);
    hf = std::fmod(hf + 240.0, 360.0); // rotate so 0 degrees is blue
    double c = (1.0 - std::fabs(2.0 * lf - 1.0)) * sf;
    double x = c * (1.0 - std::fabs(std::fmod(hf / 60.0, 2.0) - 1.0));
    double m = lf - c / 2.0;
    double r = 0, g = 0, b = 0;
    if (hf < 60)       { r = c; g = x; }
    else if (hf < 120) { r = x; g = c; }
    else if (hf < 180) { g = c; b = x; }
    else if (hf < 240) { g = x; b = c; }
    else if (hf < 300) { r = x; b = c; }
    else               { r = c; b = x; }
    auto q = [&](double v) { return static_cast<uint8_t>(std::lround((v + m) * 255.0)); };
    return pack_rgba(q(r), q(g), q(b));
}

} // namespace

bool decode_sixel(const char* data, size_t size, bool background_transparent,
                  std::vector<uint32_t>& out_pixels, int& out_width, int& out_height) {
    // Bounded independently of TerminalImages::kMaxImagePixels, since this
    // allocates while decoding rather than at the end.
    constexpr int kMaxDim = 8192;

    std::vector<uint32_t> palette(256, pack_rgba(0, 0, 0));
    for (int i = 0; i < 16; ++i) {
        palette[i] = pack_rgba(percent_to_byte(kSixelDefaultPalette[i][0]),
                               percent_to_byte(kSixelDefaultPalette[i][1]),
                               percent_to_byte(kSixelDefaultPalette[i][2]));
    }

    const uint32_t background = background_transparent ? 0u : pack_rgba(0, 0, 0);
    std::vector<uint32_t> pixels;
    int width = 0, height = 0;
    int x = 0, band_top = 0, color = 0;
    int max_x = 0;

    // Grows to fit as the image is drawn, since the raster attribute is
    // optional and plenty of encoders omit it.
    auto ensure = [&](int need_w, int need_h) -> bool {
        if (need_w > kMaxDim || need_h > kMaxDim) return false;
        if (need_w <= width && need_h <= height) return true;
        int new_w = std::max(width, need_w);
        int new_h = std::max(height, need_h);
        if (static_cast<size_t>(new_w) * new_h > TerminalImages::kMaxImagePixels) return false;
        std::vector<uint32_t> grown(static_cast<size_t>(new_w) * new_h, background);
        for (int row = 0; row < height; ++row) {
            std::copy(pixels.begin() + static_cast<size_t>(row) * width,
                      pixels.begin() + static_cast<size_t>(row) * width + width,
                      grown.begin() + static_cast<size_t>(row) * new_w);
        }
        pixels.swap(grown);
        width = new_w;
        height = new_h;
        return true;
    };

    // Reads a ';'-separated parameter list, stopping at the first byte that
    // starts neither a digit nor a separator.
    auto read_params = [&](size_t& i, int* params, int max_params) -> int {
        int count = 0;
        while (count < max_params) {
            long value = 0;
            bool any = false;
            while (i < size && data[i] >= '0' && data[i] <= '9') {
                if (value < 1000000) value = value * 10 + (data[i] - '0');
                any = true;
                ++i;
            }
            params[count++] = any ? static_cast<int>(value) : 0;
            if (i < size && data[i] == ';') { ++i; continue; }
            break;
        }
        return count;
    };

    for (size_t i = 0; i < size;) {
        unsigned char c = static_cast<unsigned char>(data[i]);
        if (c == '"') {
            ++i;
            int params[4] = {0, 0, 0, 0};
            int n = read_params(i, params, 4);
            // Pan/Pad are the aspect ratio, which sink does not honour; Ph/Pv
            // are a size hint, useful because it avoids repeated regrowth.
            if (n >= 4 && params[2] > 0 && params[3] > 0) {
                if (!ensure(params[2], params[3])) return false;
            }
        } else if (c == '#') {
            ++i;
            int params[5] = {0, 0, 0, 0, 0};
            int n = read_params(i, params, 5);
            int index = params[0] & 0xFF;
            if (n >= 5) {
                if (params[1] == 2) {
                    palette[index] = pack_rgba(percent_to_byte(params[2]),
                                               percent_to_byte(params[3]),
                                               percent_to_byte(params[4]));
                } else if (params[1] == 1) {
                    palette[index] = hls_to_rgba(params[2], params[3], params[4]);
                }
            }
            color = index;
        } else if (c == '!') {
            ++i;
            int params[1] = {0};
            read_params(i, params, 1);
            int repeat = params[0] > 0 ? params[0] : 1;
            if (i >= size) break;
            unsigned char sx = static_cast<unsigned char>(data[i]);
            if (sx < 0x3F || sx > 0x7E) { ++i; continue; }
            ++i;
            int bits = sx - 0x3F;
            if (repeat > kMaxDim) repeat = kMaxDim;
            if (!ensure(x + repeat, band_top + 6)) return !pixels.empty();
            for (int r = 0; r < repeat; ++r, ++x) {
                for (int b = 0; b < 6; ++b) {
                    if (bits & (1 << b)) {
                        pixels[static_cast<size_t>(band_top + b) * width + x] = palette[color];
                    }
                }
            }
            max_x = std::max(max_x, x);
        } else if (c == '$') {
            x = 0;
            ++i;
        } else if (c == '-') {
            x = 0;
            band_top += 6;
            ++i;
        } else if (c >= 0x3F && c <= 0x7E) {
            int bits = c - 0x3F;
            if (!ensure(x + 1, band_top + 6)) return !pixels.empty();
            for (int b = 0; b < 6; ++b) {
                if (bits & (1 << b)) {
                    pixels[static_cast<size_t>(band_top + b) * width + x] = palette[color];
                }
            }
            ++x;
            max_x = std::max(max_x, x);
            ++i;
        } else {
            ++i; // whitespace, newlines, anything else: skipped
        }
    }

    if (pixels.empty() || width <= 0 || height <= 0) return false;

    // Trim the right-hand side back to what was actually drawn, so a raster
    // attribute wider than the content does not leave a band of background.
    if (max_x > 0 && max_x < width) {
        std::vector<uint32_t> trimmed(static_cast<size_t>(max_x) * height);
        for (int row = 0; row < height; ++row) {
            std::copy(pixels.begin() + static_cast<size_t>(row) * width,
                      pixels.begin() + static_cast<size_t>(row) * width + max_x,
                      trimmed.begin() + static_cast<size_t>(row) * max_x);
        }
        pixels.swap(trimmed);
        width = max_x;
    }

    out_pixels = std::move(pixels);
    out_width = width;
    out_height = height;
    return true;
}
