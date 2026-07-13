#pragma once

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <string>
#include <unordered_map>

struct GlyphInfo {
    SDL_FRect src_rect;  // Location of the glyph on the atlas texture
    float advance;       // Horizontal spacing
};

class FontManager {
public:
    FontManager();
    ~FontManager();

    bool initialize();
    void cleanup();

    bool load_font(SDL_Renderer* renderer, const std::string& font_path, float font_size, bool bold = false);
    
    SDL_Texture* get_atlas_texture() const { return atlas_texture_; }
    const GlyphInfo* get_glyph(char32_t codepoint) const;
    
    float get_cell_width() const { return cell_width_; }
    float get_cell_height() const { return cell_height_; }

private:
    TTF_Font* font_ = nullptr;
    SDL_Texture* atlas_texture_ = nullptr;
    std::unordered_map<char32_t, GlyphInfo> glyph_cache_;
    
    float cell_width_ = 0.0f;
    float cell_height_ = 0.0f;
    bool is_ttf_initialized_ = false;

    bool build_atlas(SDL_Renderer* renderer);
};
