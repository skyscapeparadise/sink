#include "system_fonts.hpp"

#if defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>

#include <cmath>
#include <strings.h>
#include <vector>

namespace system_fonts {
namespace {

// Releases a CoreFoundation object when it goes out of scope.
template <typename T>
struct CF {
    T ref = nullptr;
    explicit CF(T r = nullptr) : ref(r) {}
    ~CF() { if (ref) CFRelease(ref); }
    CF(const CF&) = delete;
    CF& operator=(const CF&) = delete;
    explicit operator bool() const { return ref != nullptr; }
};

std::string to_string(CFStringRef s) {
    if (!s) return std::string();
    CFIndex len = CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
    std::vector<char> buf(static_cast<size_t>(len));
    if (!CFStringGetCString(s, buf.data(), len, kCFStringEncodingUTF8)) return std::string();
    return buf.data();
}

std::string attribute_string(CTFontDescriptorRef d, CFStringRef attribute) {
    CF<CFStringRef> v(static_cast<CFStringRef>(CTFontDescriptorCopyAttribute(d, attribute)));
    return v ? to_string(v.ref) : std::string();
}

std::optional<Found> found_from(CTFontDescriptorRef d) {
    CF<CFURLRef> url(static_cast<CFURLRef>(CTFontDescriptorCopyAttribute(d, kCTFontURLAttribute)));
    if (!url) return std::nullopt;
    char path[4096];
    if (!CFURLGetFileSystemRepresentation(url.ref, true, reinterpret_cast<UInt8*>(path), sizeof(path))) {
        return std::nullopt;
    }
    return Found{ path, attribute_string(d, kCTFontFamilyNameAttribute), attribute_string(d, kCTFontStyleNameAttribute) };
}

// How far a face is from regular upright: italic counts heavily, then
// distance from normal weight.
double distance_from_regular(CTFontDescriptorRef d) {
    CF<CFDictionaryRef> traits(static_cast<CFDictionaryRef>(CTFontDescriptorCopyAttribute(d, kCTFontTraitsAttribute)));
    double score = 0;
    if (traits) {
        if (auto sym = static_cast<CFNumberRef>(CFDictionaryGetValue(traits.ref, kCTFontSymbolicTrait))) {
            uint32_t bits = 0;
            CFNumberGetValue(sym, kCFNumberSInt32Type, &bits);
            if (bits & kCTFontItalicTrait) score += 10;
        }
        if (auto weight = static_cast<CFNumberRef>(CFDictionaryGetValue(traits.ref, kCTFontWeightTrait))) {
            double w = 0;
            CFNumberGetValue(weight, kCFNumberDoubleType, &w);
            score += std::fabs(w);
        }
    }
    return score;
}

// The installed family whose name matches `name` regardless of case.
std::optional<std::string> family_named(const std::string& name) {
    CF<CFArrayRef> families(CTFontManagerCopyAvailableFontFamilyNames());
    if (!families) return std::nullopt;
    for (CFIndex i = 0; i < CFArrayGetCount(families.ref); ++i) {
        std::string family = to_string(static_cast<CFStringRef>(CFArrayGetValueAtIndex(families.ref, i)));
        if (strcasecmp(family.c_str(), name.c_str()) == 0) return family;
    }
    return std::nullopt;
}

} // namespace

std::optional<Found> find(const std::string& name) {
    if (name.empty()) return std::nullopt;
    CF<CFStringRef> cf_name(CFStringCreateWithCString(nullptr, name.c_str(), kCFStringEncodingUTF8));
    if (!cf_name) return std::nullopt;

    // A family: its most regular face.
    if (std::optional<std::string> family = family_named(name)) {
        CF<CFStringRef> cf_family(CFStringCreateWithCString(nullptr, family->c_str(), kCFStringEncodingUTF8));
        const void* keys[] = { kCTFontFamilyNameAttribute };
        const void* values[] = { cf_family.ref };
        CF<CFDictionaryRef> attrs(CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                                     &kCFTypeDictionaryValueCallBacks));
        CF<CTFontDescriptorRef> wanted(CTFontDescriptorCreateWithAttributes(attrs.ref));
        CF<CFSetRef> mandatory(CFSetCreate(nullptr, keys, 1, &kCFTypeSetCallBacks));
        CF<CFArrayRef> faces(CTFontDescriptorCreateMatchingFontDescriptors(wanted.ref, mandatory.ref));
        if (faces && CFArrayGetCount(faces.ref) > 0) {
            CTFontDescriptorRef best = nullptr;
            double best_score = 0;
            for (CFIndex i = 0; i < CFArrayGetCount(faces.ref); ++i) {
                auto d = static_cast<CTFontDescriptorRef>(CFArrayGetValueAtIndex(faces.ref, i));
                double score = distance_from_regular(d);
                if (!best || score < best_score) { best = d; best_score = score; }
            }
            if (best) return found_from(best);
        }
    }

    // A full or PostScript name. CoreText substitutes a default font for a
    // name it doesn't know, so the match has to be checked.
    CF<CTFontDescriptorRef> named(CTFontDescriptorCreateWithNameAndSize(cf_name.ref, 0));
    CF<CTFontDescriptorRef> match(CTFontDescriptorCreateMatchingFontDescriptor(named.ref, nullptr));
    if (!match) return std::nullopt;
    const std::string postscript = attribute_string(match.ref, kCTFontNameAttribute);
    const std::string display = attribute_string(match.ref, kCTFontDisplayNameAttribute);
    if (strcasecmp(postscript.c_str(), name.c_str()) != 0 && strcasecmp(display.c_str(), name.c_str()) != 0) {
        return std::nullopt;
    }
    return found_from(match.ref);
}

} // namespace system_fonts

#else

namespace system_fonts {
std::optional<Found> find(const std::string&) { return std::nullopt; }
}

#endif
