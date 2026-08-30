#pragma once

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <string>
#include <unordered_map>
#include <vector>

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

    // `cell_bold`/`cell_italic` select which of the 4 synthetic style
    // variants (regular/bold/italic/bold-italic) to rasterize from -- all
    // opened from the same font file the user configured, styled via
    // TTF_SetFontStyle, since sink only takes one font path rather than a
    // family of weight-specific files.
    const GlyphInfo* get_glyph(SDL_Renderer* renderer, char32_t codepoint, bool cell_bold = false, bool cell_italic = false) const;

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
    // 4 style variants, all opened from the same font file/size the user
    // configured (styled via TTF_SetFontStyle) since sink takes one font
    // path rather than a family of weight-specific files. font_ (regular)
    // is what cell metrics, the emoji fallback, and the ligature companion
    // instance are all still keyed off of.
    TTF_Font* font_ = nullptr;
    TTF_Font* font_bold_ = nullptr;
    TTF_Font* font_italic_ = nullptr;
    TTF_Font* font_bold_italic_ = nullptr;
    TTF_Font* emoji_font_ = nullptr;

    // System faces consulted, in order, when the configured font has no glyph
    // for a codepoint. Before this existed the chain was just the configured
    // font then Apple Color Emoji, and anything neither covered fell back to a
    // space -- so with the bundled Monaspace Neon (779 glyphs, no box-drawing
    // block) every ncurses frame, every DEC Special Graphics line and all CJK
    // rendered as blank cells.
    //
    // Menlo comes first: it is monospaced, metrically close, and carries the
    // box-drawing, block-element and symbol ranges terminals lean on. The CJK
    // face follows, then emoji, which stays last because it is the only
    // colour font and the only one whose glyphs are square rather than
    // cell-shaped.
    std::vector<TTF_Font*> fallback_fonts_;
    TTF_Font* first_fallback_with(char32_t codepoint) const;
    TTF_Font* ligature_font_ = nullptr; // same face as font_, ~2x point size
    SDL_Texture* atlas_texture_ = nullptr;
    SDL_Texture* dynamic_atlas_texture_ = nullptr;
    std::unordered_map<char32_t, GlyphInfo> glyph_cache_;
    // One dynamic cache per style, since a bold/italic glyph rasterizes to
    // different pixels than its regular counterpart and both must be able
    // to live in the atlas at once (e.g. mixed bold/regular text on
    // screen). Styles 1-3 (anything with bold and/or italic) always go
    // through here, even for ASCII -- only plain regular text gets the
    // static-atlas fast path below, since it's the overwhelmingly common
    // case and the dynamic path (already handling arbitrary non-ASCII
    // content) generalizes to the rest without a second bespoke atlas
    // builder. Indexed by style_index(bold, italic); index 0 unused here
    // since regular goes through dynamic_glyph_cache_[0] only as a
    // non-ASCII fallback, same as before this feature existed.
    mutable std::unordered_map<char32_t, GlyphInfo> dynamic_glyph_cache_[4];
    // Separate from dynamic_glyph_cache_ (though they share the same atlas
    // texture/packer) since a ligature symbol could in principle also want
    // its normal-size rendering, and the two must not collide under the
    // same codepoint key. Ligatures are only ever rasterized from the
    // regular face (see get_ligature_glyph).
    mutable std::unordered_map<char32_t, GlyphInfo> ligature_glyph_cache_;

    // Fast O(1) ASCII glyph cache -- regular style only (see above)
    GlyphInfo ascii_cache_[128];
    bool has_ascii_cache_[128] = {false};

    static int style_index(bool bold, bool italic) { return (bold ? 1 : 0) | (italic ? 2 : 0); }
    TTF_Font* font_for_style(int idx) const {
        switch (idx) {
            case 1: return font_bold_ ? font_bold_ : font_;
            case 2: return font_italic_ ? font_italic_ : font_;
            case 3: return font_bold_italic_ ? font_bold_italic_ : font_;
            default: return font_;
        }
    }
    
    float cell_width_ = 0.0f;
    float cell_height_ = 0.0f;
    bool is_ttf_initialized_ = false;

    // Dynamic packer coordinates
    mutable int dynamic_x_ = 0;
    mutable int dynamic_y_ = 0;
    mutable int dynamic_row_h_ = 0;

    bool build_atlas(SDL_Renderer* renderer);
};
