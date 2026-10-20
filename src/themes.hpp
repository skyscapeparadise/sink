#pragma once

// The color themes sink ships. A preset picks one with `theme`, and its
// [presets.<name>.colors] table can override any color of it.

#include <cstdint>
#include <string>
#include <vector>

namespace themes {

struct Rgb {
    uint8_t r = 0, g = 0, b = 0;
    bool operator==(const Rgb&) const = default;
};

struct Theme {
    const char* name;
    bool light;            // a light background: dark text on light
    Rgb foreground;
    Rgb background;        // where programs paint "the background", and what
                           // sink reports when one asks (OSC 11)
    Rgb cursor;
    Rgb selection;
    float selection_alpha; // selection is drawn over whatever is behind it
    Rgb palette[16];       // black red green yellow blue magenta cyan white, then bright
};

// Every bundled theme, "sink" first.
const std::vector<Theme>& all();
std::vector<std::string> names();

// The theme with this name (regardless of case), or nullptr.
const Theme* find(const std::string& name);

// "#rrggbb" or "#rgb" into `out`; false if it's neither.
bool parse_hex(const std::string& text, Rgb& out);
std::string to_hex(const Rgb& c);

} // namespace themes
