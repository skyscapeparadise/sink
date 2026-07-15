#include "font_manager.hpp"
#include <iostream>
#include <algorithm>

FontManager::FontManager() {}

FontManager::~FontManager() {
    cleanup();
}

bool FontManager::initialize() {
    if (!is_ttf_initialized_) {
        if (!TTF_Init()) {
            std::cerr << "Failed to initialize SDL_ttf: " << SDL_GetError() << std::endl;
            return false;
        }
        is_ttf_initialized_ = true;
    }
    return true;
}

void FontManager::cleanup() {
    if (atlas_texture_) {
        SDL_DestroyTexture(atlas_texture_);
        atlas_texture_ = nullptr;
    }
    if (dynamic_atlas_texture_) {
        SDL_DestroyTexture(dynamic_atlas_texture_);
        dynamic_atlas_texture_ = nullptr;
    }
    if (font_) {
        TTF_CloseFont(font_);
        font_ = nullptr;
    }
    if (emoji_font_) {
        TTF_CloseFont(emoji_font_);
        emoji_font_ = nullptr;
    }
    if (is_ttf_initialized_) {
        TTF_Quit();
        is_ttf_initialized_ = false;
    }
    glyph_cache_.clear();
    dynamic_glyph_cache_.clear();
    dynamic_x_ = 0;
    dynamic_y_ = 0;
    dynamic_row_h_ = 0;
}

bool FontManager::load_font(SDL_Renderer* renderer, const std::string& font_path, float font_size, bool bold) {
    if (!initialize()) {
        return false;
    }

    if (font_) {
        TTF_CloseFont(font_);
        font_ = nullptr;
    }
    if (emoji_font_) {
        TTF_CloseFont(emoji_font_);
        emoji_font_ = nullptr;
    }
    if (atlas_texture_) {
        SDL_DestroyTexture(atlas_texture_);
        atlas_texture_ = nullptr;
    }
    if (dynamic_atlas_texture_) {
        SDL_DestroyTexture(dynamic_atlas_texture_);
        dynamic_atlas_texture_ = nullptr;
    }
    glyph_cache_.clear();
    dynamic_glyph_cache_.clear();
    dynamic_x_ = 0;
    dynamic_y_ = 0;
    dynamic_row_h_ = 0;

    font_ = TTF_OpenFont(font_path.c_str(), font_size);
    if (!font_) {
        std::cerr << "Failed to open font " << font_path << ": " << SDL_GetError() << std::endl;
        return false;
    }

    if (bold) {
        TTF_SetFontStyle(font_, TTF_STYLE_BOLD);
    }

    // Load fallback emoji font
    emoji_font_ = TTF_OpenFont("/System/Library/Fonts/Apple Color Emoji.ttc", font_size);
    if (!emoji_font_) {
        std::cerr << "Warning: Failed to open Apple Color Emoji font. Emoji support may be degraded." << std::endl;
    }

    // Create dynamic atlas texture (1024x1024 RGBA32)
    dynamic_atlas_texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 1024, 1024);
    if (!dynamic_atlas_texture_) {
        std::cerr << "Failed to create dynamic atlas texture: " << SDL_GetError() << std::endl;
        return false;
    }
    SDL_SetTextureBlendMode(dynamic_atlas_texture_, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(dynamic_atlas_texture_, SDL_SCALEMODE_LINEAR);

    // Initialize dynamic atlas to transparent black
    std::vector<uint32_t> empty_pixels(1024 * 1024, 0);
    SDL_UpdateTexture(dynamic_atlas_texture_, nullptr, empty_pixels.data(), 1024 * sizeof(uint32_t));

    // Get monospace cell dimensions
    cell_height_ = static_cast<float>(TTF_GetFontHeight(font_));
    
    // Query glyph metrics for 'n' to establish cell width
    int advance = 0;
    if (TTF_GetGlyphMetrics(font_, 'n', nullptr, nullptr, nullptr, nullptr, &advance)) {
        cell_width_ = static_cast<float>(advance);
    } else {
        // Fallback: render 'n' and check width
        SDL_Color white = {255, 255, 255, 255};
        SDL_Surface* surf = TTF_RenderGlyph_Blended(font_, 'n', white);
        if (surf) {
            cell_width_ = static_cast<float>(surf->w);
            SDL_DestroySurface(surf);
        } else {
            cell_width_ = font_size * 0.6f; // rough estimate
        }
    }

    if (cell_width_ <= 0.0f) {
        cell_width_ = font_size * 0.6f;
    }

    return build_atlas(renderer);
}

bool FontManager::build_atlas(SDL_Renderer* renderer) {
    // We will render ASCII characters 32 to 126
    const char32_t start_char = 32;
    const char32_t end_char = 126;
    const int num_chars = end_char - start_char + 1;

    // Grid layout: 16 columns
    const int cols = 16;
    const int rows = (num_chars + cols - 1) / cols;

    // Add spacing (padding) between cells to prevent bilinear bleeding artifacts
    const int spacing = 4;
    int cell_w_spaced = static_cast<int>(cell_width_) + spacing;
    int cell_h_spaced = static_cast<int>(cell_height_) + spacing;

    int atlas_w = cols * cell_w_spaced;
    int atlas_h = rows * cell_h_spaced;

    // Align to power of two for optimal GPU performance
    int pow2_w = 1;
    while (pow2_w < atlas_w) pow2_w *= 2;
    int pow2_h = 1;
    while (pow2_h < atlas_h) pow2_h *= 2;

    SDL_Surface* atlas_surface = SDL_CreateSurface(pow2_w, pow2_h, SDL_PIXELFORMAT_RGBA32);
    if (!atlas_surface) {
        std::cerr << "Failed to create atlas surface: " << SDL_GetError() << std::endl;
        return false;
    }

    // Set blend mode on atlas surface to preserve transparency during blits
    SDL_SetSurfaceBlendMode(atlas_surface, SDL_BLENDMODE_BLEND);

    SDL_Color white = {255, 255, 255, 255};

    for (int i = 0; i < num_chars; ++i) {
        char32_t codepoint = start_char + i;
        SDL_Surface* glyph_surf = TTF_RenderGlyph_Blended(font_, codepoint, white);
        
        int col = i % cols;
        int row = i / cols;
        
        float x = col * cell_w_spaced;
        float y = row * cell_h_spaced;

        if (glyph_surf) {
            // Find advance metric
            int adv = 0;
            TTF_GetGlyphMetrics(font_, codepoint, nullptr, nullptr, nullptr, nullptr, &adv);
            float actual_advance = adv > 0 ? static_cast<float>(adv) : static_cast<float>(glyph_surf->w);

            // Disable surface blending on the glyph to perform a raw copy of color and alpha channels
            SDL_SetSurfaceBlendMode(glyph_surf, SDL_BLENDMODE_NONE);

            // Blit glyph to the atlas
            SDL_Rect dst_rect = {
                static_cast<int>(x),
                static_cast<int>(y),
                glyph_surf->w,
                glyph_surf->h
            };
            
            SDL_BlitSurface(glyph_surf, nullptr, atlas_surface, &dst_rect);

            // Store mapping info
            GlyphInfo info;
            info.src_rect = {
                x,
                y,
                static_cast<float>(glyph_surf->w),
                static_cast<float>(glyph_surf->h)
            };
            info.advance = actual_advance;
            glyph_cache_[codepoint] = info;

            SDL_DestroySurface(glyph_surf);
        } else {
            // Fallback for missing/empty glyphs (e.g. space character)
            GlyphInfo info;
            info.src_rect = { x, y, 0.0f, 0.0f };
            info.advance = cell_width_;
            glyph_cache_[codepoint] = info;
        }
    }

    atlas_texture_ = SDL_CreateTextureFromSurface(renderer, atlas_surface);
    SDL_DestroySurface(atlas_surface);

    if (!atlas_texture_) {
        std::cerr << "Failed to create atlas texture: " << SDL_GetError() << std::endl;
        return false;
    }

    // Set texture blending
    SDL_SetTextureBlendMode(atlas_texture_, SDL_BLENDMODE_BLEND);

    // Apply linear scaling filtering to enable GPU hardware bilinear anti-aliasing
    SDL_SetTextureScaleMode(atlas_texture_, SDL_SCALEMODE_LINEAR);

    return true;
}

static bool is_emoji_codepoint(char32_t cp) {
    if (cp >= 0x1F000 && cp <= 0x1FBF8) return true;
    if (cp >= 0x2600 && cp <= 0x27BF) return true;
    if (cp >= 0x2300 && cp <= 0x23FF) return true;
    if (cp == 0x2B50) return true; // Star
    return false;
}

const GlyphInfo* FontManager::get_glyph(SDL_Renderer* renderer, char32_t codepoint) const {
    // 1. Static Cache Lookup (ASCII 32-126)
    auto it = glyph_cache_.find(codepoint);
    if (it != glyph_cache_.end()) {
        return &it->second;
    }

    // 2. Dynamic Cache Lookup
    auto dyn_it = dynamic_glyph_cache_.find(codepoint);
    if (dyn_it != dynamic_glyph_cache_.end()) {
        return &dyn_it->second;
    }

    // 3. Fallback/Ignored checks
    if (codepoint == 0 || codepoint == 32) {
        // Return space fallback
        it = glyph_cache_.find(32);
        if (it != glyph_cache_.end()) {
            return &it->second;
        }
        return nullptr;
    }

    if (!renderer || !dynamic_atlas_texture_) {
        // Return space fallback
        it = glyph_cache_.find(32);
        if (it != glyph_cache_.end()) {
            return &it->second;
        }
        return nullptr;
    }

    // 4. Determine font source & render
    bool is_color = false;
    SDL_Surface* glyph_surf = nullptr;
    SDL_Color white = {255, 255, 255, 255};

    if (is_emoji_codepoint(codepoint)) {
        if (emoji_font_ && TTF_FontHasGlyph(emoji_font_, codepoint)) {
            glyph_surf = TTF_RenderGlyph_Blended(emoji_font_, codepoint, white);
            is_color = true;
        }
    } else {
        if (font_ && TTF_FontHasGlyph(font_, codepoint)) {
            glyph_surf = TTF_RenderGlyph_Blended(font_, codepoint, white);
        } else if (emoji_font_ && TTF_FontHasGlyph(emoji_font_, codepoint)) {
            glyph_surf = TTF_RenderGlyph_Blended(emoji_font_, codepoint, white);
            is_color = true;
        }
    }

    if (!glyph_surf) {
        // Return space fallback
        it = glyph_cache_.find(32);
        if (it != glyph_cache_.end()) {
            return &it->second;
        }
        return nullptr;
    }

    // 5. Pack glyph into the dynamic atlas
    int w = glyph_surf->w;
    int h = glyph_surf->h;

    // Check if we need to wrap to next row
    if (dynamic_x_ + w + 4 > 1024) {
        dynamic_x_ = 0;
        dynamic_y_ += dynamic_row_h_ + 4;
        dynamic_row_h_ = 0;
    }

    // Check if atlas is full
    if (dynamic_y_ + h + 4 > 1024) {
        // Reset packer coordinates & clear cache
        dynamic_x_ = 0;
        dynamic_y_ = 0;
        dynamic_row_h_ = 0;
        dynamic_glyph_cache_.clear();

        // Clear dynamic texture
        std::vector<uint32_t> empty_pixels(1024 * 1024, 0);
        SDL_UpdateTexture(dynamic_atlas_texture_, nullptr, empty_pixels.data(), 1024 * sizeof(uint32_t));
    }

    // Copy surface pixels to dynamic texture
    SDL_Rect dst_rect = { dynamic_x_, dynamic_y_, w, h };
    SDL_UpdateTexture(dynamic_atlas_texture_, &dst_rect, glyph_surf->pixels, glyph_surf->pitch);

    // Get advance metric
    int adv = 0;
    if (is_color) {
        TTF_GetGlyphMetrics(emoji_font_, codepoint, nullptr, nullptr, nullptr, nullptr, &adv);
    } else {
        TTF_GetGlyphMetrics(font_, codepoint, nullptr, nullptr, nullptr, nullptr, &adv);
    }
    float actual_advance = adv > 0 ? static_cast<float>(adv) : static_cast<float>(w);

    // Create GlyphInfo
    GlyphInfo info;
    info.src_rect = {
        static_cast<float>(dynamic_x_),
        static_cast<float>(dynamic_y_),
        static_cast<float>(w),
        static_cast<float>(h)
    };
    info.advance = actual_advance;
    info.is_color = is_color;

    // Cache it
    dynamic_glyph_cache_[codepoint] = info;

    // Advance packer coordinates
    dynamic_x_ += w + 4;
    dynamic_row_h_ = std::max(dynamic_row_h_, h);

    SDL_DestroySurface(glyph_surf);

    return &dynamic_glyph_cache_[codepoint];
}
