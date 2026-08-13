#pragma once

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <string>
#include <unordered_map>

struct GlyphInfo {
    SDL_FRect src_rect;  // Location of the glyph on the atlas texture
    float advance;       // Horizontal spacing
    bool is_color = false; // True if this is a color emoji (no foreground tint)
};

class FontManager {
public:
    FontManager();
    ~FontManager();

    bool initialize();
    void cleanup();

    bool load_font(SDL_Renderer* renderer, const std::string& font_path, float font_size, bool bold = false);

    SDL_Texture* get_atlas_texture() const { return atlas_texture_; }
    SDL_Texture* get_dynamic_atlas_texture() const { return dynamic_atlas_texture_; }
    const GlyphInfo* get_glyph(SDL_Renderer* renderer, char32_t codepoint) const;

    // Same codepoint, rasterized from a font instance loaded at ~2x point
    // size, for ligature substitute glyphs (e.g. "->" drawn as a single
    // arrow) that need to visually span two character cells. Terminal_grid
    // stretches whatever glyph it gets to fit that 2-cell span; feeding it
    // one that's already roughly the right size means that stretch is a
    // near-1:1 blit instead of a ~2x upscale, which is what was making
    // ligatures look soft/blurry compared to normal glyphs.
    const GlyphInfo* get_ligature_glyph(SDL_Renderer* renderer, char32_t codepoint) const;

    float get_cell_width() const { return cell_width_; }
    float get_cell_height() const { return cell_height_; }

private:
    TTF_Font* font_ = nullptr;
    TTF_Font* emoji_font_ = nullptr;
    TTF_Font* ligature_font_ = nullptr; // same face as font_, ~2x point size
    SDL_Texture* atlas_texture_ = nullptr;
    SDL_Texture* dynamic_atlas_texture_ = nullptr;
    std::unordered_map<char32_t, GlyphInfo> glyph_cache_;
    mutable std::unordered_map<char32_t, GlyphInfo> dynamic_glyph_cache_;
    // Separate from dynamic_glyph_cache_ (though they share the same atlas
    // texture/packer) since a ligature symbol could in principle also want
    // its normal-size rendering, and the two must not collide under the
    // same codepoint key.
    mutable std::unordered_map<char32_t, GlyphInfo> ligature_glyph_cache_;
    
    // Fast O(1) ASCII glyph cache
    GlyphInfo ascii_cache_[128];
    bool has_ascii_cache_[128] = {false};
    
    float cell_width_ = 0.0f;
    float cell_height_ = 0.0f;
    bool is_ttf_initialized_ = false;

    // Dynamic packer coordinates
    mutable int dynamic_x_ = 0;
    mutable int dynamic_y_ = 0;
    mutable int dynamic_row_h_ = 0;

    bool build_atlas(SDL_Renderer* renderer);
};
