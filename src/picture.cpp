#include "picture.hpp"

#include <algorithm>
#include <cctype>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#endif

namespace picture {

bool looks_like_picture(const std::string& path) {
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos || path.find('/', dot) != std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    static const char* kExtensions[] = { "png", "jpg", "jpeg", "heic", "heif", "webp", "tif", "tiff",
                                         "gif", "bmp", "avif", "jxl" };
    for (const char* e : kExtensions) {
        if (ext == e) return true;
    }
    return false;
}

#if defined(__APPLE__)

namespace {

template <typename T> struct CFHolder {
    T ref = nullptr;
    explicit CFHolder(T r) : ref(r) {}
    ~CFHolder() { if (ref) CFRelease(ref); }
    CFHolder(const CFHolder&) = delete;
    CFHolder& operator=(const CFHolder&) = delete;
};

int number_property(CFDictionaryRef props, CFStringRef key, int fallback) {
    int v = fallback;
    if (CFNumberRef n = props ? static_cast<CFNumberRef>(CFDictionaryGetValue(props, key)) : nullptr) {
        CFNumberGetValue(n, kCFNumberIntType, &v);
    }
    return v;
}

CGImageSourceRef as_source(void* ref) { return static_cast<CGImageSourceRef>(ref); }

} // namespace

Source Source::from_file(const std::string& path) {
    CFHolder<CFURLRef> url(CFURLCreateFromFileSystemRepresentation(
        nullptr, reinterpret_cast<const UInt8*>(path.c_str()), static_cast<CFIndex>(path.size()), false));
    return Source(url.ref ? const_cast<void*>(static_cast<const void*>(CGImageSourceCreateWithURL(url.ref, nullptr)))
                          : nullptr);
}

Source Source::from_bytes(const std::string& bytes) {
    CFHolder<CFDataRef> data(CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(bytes.data()),
                                          static_cast<CFIndex>(bytes.size())));
    return Source(data.ref ? const_cast<void*>(static_cast<const void*>(CGImageSourceCreateWithData(data.ref, nullptr)))
                           : nullptr);
}

Source::~Source() {
    if (ref_) CFRelease(as_source(ref_));
}

bool Source::ok() const {
    return ref_ && CGImageSourceGetCount(as_source(ref_)) > 0;
}

bool Source::size(int& w, int& h) const {
    if (!ok()) return false;
    CFHolder<CFDictionaryRef> props(CGImageSourceCopyPropertiesAtIndex(as_source(ref_), 0, nullptr));
    w = number_property(props.ref, kCGImagePropertyPixelWidth, 0);
    h = number_property(props.ref, kCGImagePropertyPixelHeight, 0);
    if (number_property(props.ref, kCGImagePropertyOrientation, 1) >= 5) std::swap(w, h);  // turned sideways
    return w > 0 && h > 0;
}

bool Source::decode(int max_side, Pixels& out) const {
    if (!ok() || max_side <= 0) return false;
    const void* keys[] = { kCGImageSourceCreateThumbnailFromImageAlways, kCGImageSourceCreateThumbnailWithTransform,
                           kCGImageSourceThumbnailMaxPixelSize, kCGImageSourceShouldCacheImmediately };
    CFHolder<CFNumberRef> side(CFNumberCreate(nullptr, kCFNumberIntType, &max_side));
    const void* values[] = { kCFBooleanTrue, kCFBooleanTrue, side.ref, kCFBooleanTrue };
    CFHolder<CFDictionaryRef> opts(CFDictionaryCreate(nullptr, keys, values, 4, &kCFTypeDictionaryKeyCallBacks,
                                                      &kCFTypeDictionaryValueCallBacks));
    CFHolder<CGImageRef> image(CGImageSourceCreateThumbnailAtIndex(as_source(ref_), 0, opts.ref));
    if (!image.ref) return false;
    out.w = static_cast<int>(CGImageGetWidth(image.ref));
    out.h = static_cast<int>(CGImageGetHeight(image.ref));
    if (out.w <= 0 || out.h <= 0) return false;
    out.rgba.assign(static_cast<size_t>(out.w) * out.h * 4, 0);
    CFHolder<CGColorSpaceRef> srgb(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    CFHolder<CGContextRef> ctx(CGBitmapContextCreate(
        out.rgba.data(), out.w, out.h, 8, static_cast<size_t>(out.w) * 4, srgb.ref,
        static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) | static_cast<uint32_t>(kCGBitmapByteOrder32Big)));
    if (!ctx.ref) return false;
    CGContextSetInterpolationQuality(ctx.ref, kCGInterpolationHigh);
    CGContextDrawImage(ctx.ref, CGRectMake(0, 0, out.w, out.h), image.ref);
    // Core Graphics only draws premultiplied; straight is what's wanted.
    for (size_t i = 0; i < out.rgba.size(); i += 4) {
        const unsigned a = out.rgba[i + 3];
        if (a == 0 || a == 255) continue;
        for (int c = 0; c < 3; ++c) out.rgba[i + c] = static_cast<uint8_t>(std::min(255u, out.rgba[i + c] * 255u / a));
    }
    return true;
}

#else

Source Source::from_file(const std::string&) { return Source(nullptr); }
Source Source::from_bytes(const std::string&) { return Source(nullptr); }
Source::~Source() {}
bool Source::ok() const { return false; }
bool Source::size(int&, int&) const { return false; }
bool Source::decode(int, Pixels&) const { return false; }

#endif

} // namespace picture
