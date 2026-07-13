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
    if (font_) {
        TTF_CloseFont(font_);
        font_ = nullptr;
    }
    if (is_ttf_initialized_) {
        TTF_Quit();
        is_ttf_initialized_ = false;
    }
    glyph_cache_.clear();
}

bool FontManager::load_font(SDL_Renderer* renderer, const std::string& font_path, float font_size, bool bold) {
    if (!initialize()) {
        return false;
    }

    if (font_) {
        TTF_CloseFont(font_);
        font_ = nullptr;
    }
    if (atlas_texture_) {
        SDL_DestroyTexture(atlas_texture_);
        atlas_texture_ = nullptr;
    }
    glyph_cache_.clear();

    font_ = TTF_OpenFont(font_path.c_str(), font_size);
    if (!font_) {
        std::cerr << "Failed to open font " << font_path << ": " << SDL_GetError() << std::endl;
        return false;
    }

    if (bold) {
        TTF_SetFontStyle(font_, TTF_STYLE_BOLD);
    }

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

const GlyphInfo* FontManager::get_glyph(char32_t codepoint) const {
    auto it = glyph_cache_.find(codepoint);
    if (it != glyph_cache_.end()) {
        return &it->second;
    }
    // Return space fallback if character not in cache
    it = glyph_cache_.find(32);
    if (it != glyph_cache_.end()) {
        return &it->second;
    }
    return nullptr;
}
