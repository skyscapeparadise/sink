#include "sinkview.hpp"

#include "picture.hpp"

#include <sys/ioctl.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <iterator>


namespace sinkview {

namespace {

// sink holds one picture's data, compressed, to 8 MB (ANSIParser::kApcMaxLen);
// a little under that leaves room for the chunk headers.
constexpr size_t kMaxCompressed = 7u * 1024 * 1024 + 512u * 1024;
constexpr size_t kChunk = 4096;  // base64 characters per chunk, as the protocol asks

std::string base64(const std::string& in) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t n = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
        out += k[n >> 18]; out += k[(n >> 12) & 63]; out += k[(n >> 6) & 63]; out += k[n & 63];
    }
    if (i + 1 == in.size()) {
        const uint32_t n = uint8_t(in[i]) << 16;
        out += k[n >> 18]; out += k[(n >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        const uint32_t n = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8);
        out += k[n >> 18]; out += k[(n >> 12) & 63]; out += k[(n >> 6) & 63]; out += '=';
    }
    return out;
}

std::string deflate_bytes(const std::string& in) {
    uLongf size = compressBound(static_cast<uLong>(in.size()));
    std::string out(size, '\0');
    if (compress2(reinterpret_cast<Bytef*>(&out[0]), &size, reinterpret_cast<const Bytef*>(in.data()),
                  static_cast<uLong>(in.size()), 6) != Z_OK) {
        return {};
    }
    out.resize(size);
    return out;
}

struct Window {
    int cols = 80, rows = 24;
    int cell_w = 8, cell_h = 16;  // pixels; a guess when the terminal doesn't say
};

Window window_size() {
    Window w;
    struct winsize ws {};
    for (int fd : { STDOUT_FILENO, STDERR_FILENO, STDIN_FILENO }) {
        if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
            w.cols = ws.ws_col;
            w.rows = ws.ws_row;
            if (ws.ws_xpixel > 0 && ws.ws_ypixel > 0) {
                w.cell_w = std::max(1, ws.ws_xpixel / ws.ws_col);
                w.cell_h = std::max(1, ws.ws_ypixel / ws.ws_row);
            }
            break;
        }
    }
    return w;
}

// Shows one picture. `name` is for messages.
bool show(const picture::Source& src, const std::string& name) {
    int w = 0, h = 0;
    if (!src.ok()) {
        std::cerr << "sinkview: " << name << " isn't a picture macOS can open" << std::endl;
        return false;
    }
    if (!src.size(w, h)) {
        std::cerr << "sinkview: " << name << " has no size" << std::endl;
        return false;
    }

    const Window win = window_size();
    // A line is left for the prompt, so the top of the picture stays on screen.
    Fit f = fit(w, h, win.cols, std::max(1, win.rows - 1), win.cell_w, win.cell_h);
    std::string sequence;
    picture::Pixels pic;
    // A picture that's still too much after compression is sent smaller.
    for (int attempt = 0; attempt < 6; ++attempt) {
        if (!src.decode(std::max(f.px_w, f.px_h), pic)) {
            std::cerr << "sinkview: couldn't read " << name << std::endl;
            return false;
        }
        sequence = kitty_sequence(pic.rgba, pic.w, pic.h, f.cols);
        if (!sequence.empty()) break;
        f.px_w = f.px_w * 3 / 4;
        f.px_h = f.px_h * 3 / 4;
    }
    if (sequence.empty()) {
        std::cerr << "sinkview: " << name << " is too big to send" << std::endl;
        return false;
    }
    // The terminal leaves the cursor after the picture, on its last row; the
    // next line is below it.
    sequence += '\n';
    std::fwrite(sequence.data(), 1, sequence.size(), stdout);
    std::fflush(stdout);
    return true;
}

void usage(std::ostream& out) {
    out << "usage: sinkview <picture>...\n"
           "       <command> | sinkview\n"
           "\n"
           "Shows pictures in the terminal: JPEG, PNG, HEIC, GIF, WebP, TIFF and\n"
           "anything else macOS opens, shrunk to fit the window.\n";
}

} // namespace

Fit fit(int image_w, int image_h, int max_cols, int max_rows, int cell_w, int cell_h) {
    Fit f;
    if (image_w <= 0 || image_h <= 0 || max_cols <= 0 || max_rows <= 0 || cell_w <= 0 || cell_h <= 0) return f;
    const double scale = std::min({ 1.0, static_cast<double>(max_cols) * cell_w / image_w,
                                    static_cast<double>(max_rows) * cell_h / image_h });
    f.px_w = std::max(1, static_cast<int>(std::lround(image_w * scale)));
    f.px_h = std::max(1, static_cast<int>(std::lround(image_h * scale)));
    f.cols = std::clamp(static_cast<int>(std::ceil(static_cast<double>(f.px_w) / cell_w - 0.001)), 1, max_cols);
    f.rows = std::max(1, static_cast<int>(std::ceil(static_cast<double>(f.px_h) / cell_h - 0.001)));
    return f;
}

std::string kitty_sequence(const std::vector<uint8_t>& rgba, int w, int h, int cols) {
    if (w <= 0 || h <= 0 || cols <= 0 || rgba.size() < static_cast<size_t>(w) * h * 4) return {};
    const size_t pixels = static_cast<size_t>(w) * h;
    bool opaque = true;
    for (size_t i = 0; i < pixels && opaque; ++i) opaque = rgba[i * 4 + 3] == 255;
    std::string raw;
    if (opaque) {
        raw.resize(pixels * 3);
        for (size_t i = 0; i < pixels; ++i) {
            raw[i * 3] = static_cast<char>(rgba[i * 4]);
            raw[i * 3 + 1] = static_cast<char>(rgba[i * 4 + 1]);
            raw[i * 3 + 2] = static_cast<char>(rgba[i * 4 + 2]);
        }
    } else {
        raw.assign(reinterpret_cast<const char*>(rgba.data()), pixels * 4);
    }
    const std::string packed = deflate_bytes(raw);
    if (packed.empty() || packed.size() > kMaxCompressed) return {};
    const std::string text = base64(packed);

    std::string out;
    for (size_t at = 0; at < text.size(); at += kChunk) {
        const bool last = at + kChunk >= text.size();
        out += "\x1b_G";
        if (at == 0) {
            out += "a=T,f=" + std::string(opaque ? "24" : "32") + ",s=" + std::to_string(w) + ",v=" +
                   std::to_string(h) + ",o=z,c=" + std::to_string(cols) + ",q=2,";
        }
        out += last ? "m=0;" : "m=1;";
        out.append(text, at, kChunk);
        out += "\x1b\\";
    }
    return out;
}

int run(const std::vector<std::string>& args) {
    std::vector<std::string> files;
    for (const std::string& a : args) {
        if (a == "-h" || a == "--help") {
            usage(std::cout);
            return 0;
        }
        files.push_back(a);
    }
    if (files.empty()) {
        if (isatty(STDIN_FILENO)) {
            usage(std::cerr);
            return 2;
        }
        const std::string bytes((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
        return show(picture::Source::from_bytes(bytes), "what was piped in") ? 0 : 1;
    }
    int status = 0;
    for (const std::string& file : files) {
        if (access(file.c_str(), R_OK) != 0) {
            std::cerr << "sinkview: can't open " << file << std::endl;
            status = 1;
            continue;
        }
        if (!show(picture::Source::from_file(file), file)) status = 1;
    }
    return status;
}

} // namespace sinkview
