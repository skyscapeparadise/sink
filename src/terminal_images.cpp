#include "terminal_images.hpp"

#include <algorithm>

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
