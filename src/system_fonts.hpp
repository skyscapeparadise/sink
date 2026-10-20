#pragma once

// Finding an installed font by name, through the platform's font database.
// macOS uses CoreText; elsewhere there is nothing yet, and a font name that
// isn't sink's own simply isn't found (sink falls back to its bundled font).

#include <optional>
#include <string>

namespace system_fonts {

struct Found {
    std::string path;    // the font file
    std::string family;  // which face to use, when the file holds several (.ttc)
    std::string style;
};

// `name` is a family ("JetBrains Mono", the regular upright face is chosen) or
// a full or PostScript name ("JetBrains Mono Bold", "JetBrainsMono-Bold").
// Family names match regardless of case.
std::optional<Found> find(const std::string& name);

} // namespace system_fonts
