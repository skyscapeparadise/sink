#include "sink_demo.hpp"
#include "terminal_grid.hpp"
#include "pty_bridge.hpp"
#include "ansi_parser.hpp"
#include "video_engine.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <random>
#include <cstring>
#include <vector>
#include <atomic>
#include <filesystem>
#include <functional>
#include <cmath>
#include <cctype>
#include <mutex>
#include <mach-o/dyld.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "app_state.hpp"

namespace SinkDemo {

struct BufferData {
    const uint8_t* ptr = nullptr;
    size_t size = 0;
    size_t pos = 0;
};

static int read_packet_cb(void* opaque, uint8_t* buf, int buf_size) {
    BufferData* bd = static_cast<BufferData*>(opaque);
    if (!bd || bd->pos >= bd->size) {
        return AVERROR_EOF;
    }
    int len = std::min(static_cast<size_t>(buf_size), bd->size - bd->pos);
    std::memcpy(buf, bd->ptr + bd->pos, len);
    bd->pos += len;
    return len;
}

static int64_t seek_cb(void* opaque, int64_t offset, int whence) {
    BufferData* bd = static_cast<BufferData*>(opaque);
    if (!bd) return -1;
    if (whence == AVSEEK_SIZE) {
        return static_cast<int64_t>(bd->size);
    }
    int64_t new_pos = bd->pos;
    if (whence == SEEK_SET) {
        new_pos = offset;
    } else if (whence == SEEK_CUR) {
        new_pos += offset;
    } else if (whence == SEEK_END) {
        new_pos = static_cast<int64_t>(bd->size) + offset;
    }
    if (new_pos < 0 || new_pos > static_cast<int64_t>(bd->size)) {
        return -1;
    }
    bd->pos = static_cast<size_t>(new_pos);
    return bd->pos;
}

static std::string get_executable_dir() {
    char path[1024];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        std::filesystem::path p(path);
        return p.parent_path().string();
    }
    return ".";
}

static std::string resolve_splash_path() {
    std::string exe_dir = get_executable_dir();
    std::vector<std::string> candidates = {
        "demo/splash.dat",
        exe_dir + "/demo/splash.dat",
        exe_dir + "/../demo/splash.dat",
        exe_dir + "/../../demo/splash.dat",
        exe_dir + "/../Resources/demo/splash.dat"
    };

    for (const auto& p : candidates) {
        if (std::filesystem::exists(p)) {
            return p;
        }
    }
    return "demo/splash.dat";
}

bool is_demo_running(TerminalWindow* tw) {
    return tw ? tw->demo_running.load() : false;
}

void request_skip(TerminalWindow* tw) {
    if (tw) {
        tw->demo_skip_requested.store(true);
    }
}

void request_abort(TerminalWindow* tw) {
    if (tw) {
        // Sticky: unlike skip, this is never cleared. It tells every wait
        // point in run_demo/run_sing to unwind instead of continuing on to
        // the next step, so the thread finishes quickly and tw can be freed.
        tw->demo_abort.store(true);
        tw->demo_skip_requested.store(true);
    }
}

static bool sleep_interruptible(TerminalWindow* tw, int ms) {
    if (!tw) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return false;
    }
    int ticks = ms / 10;
    if (ticks <= 0) ticks = 1;
    for (int t = 0; t < ticks; ++t) {
        if (tw->demo_skip_requested.load() || tw->demo_abort.load()) {
            tw->demo_skip_requested.store(false);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return tw->demo_skip_requested.exchange(false) || tw->demo_abort.load();
}

const char* SONG_COELACANTH = R"(i can’t begin
to understand what state you’re in and
i wish we had more time
and deep within
the cataclysmic hate you win all
punishment no crime
don’t let it be
the cemetery of your needs where
the silence so sublime
don’t look at me
the saddest thing you’ll ever see for
your senior year alive

take me back to where we started, starlet
hate me bad because i know you can’t, no you can’t
take me back to where we started, starlet
hate me bad, kill off your coelacanth

take me back to where we started, starlet
hate me bad because i know you can’t, no you can’t
take me back to where we started, starlet
hate me bad, kill off your coelacanth

hooked up to machines
i don’t know what they mean it’s
nothing you can cope
in your final scene
with blinking ekgs and
tubing down your throat
don't get off the plane
you’re taking on today the
place where i am whole
you still taste the same
and can’t live with your cane it’s
out of your control

take me back to where we started, starlet
hate me bad because i know you can’t, no you can’t
take me back to where we started, starlet
hate me bad, kill off your coelacanth

take me back to where we started, starlet
hate me bad because i know you can’t, no you can’t
take me back to where we started, starlet
hate me bad, kill off your coelacanth
)";

const char* SONG_SNAKE = R"(now stand and face the bar
mirror image seen
i am a broken swan
an imposition to my mission so discreet
returning to the pointe
pictures on a screen
all these younger faces taking all your places
sinner tells the winner
how to fade into the scenery

condense into your shape and become a snake
it’s better off this way to be unashamed
accepting it’s your fate in your nation state
you only had to do what you wanted to

condense into your shape and become a snake
it’s better off this way to be unashamed
accepting it’s your fate in your nation state
you only had to do what you wanted to

condense into your shape and become a snake
it’s better off this way to be unashamed
rejecting what they need in your vacancy
you only had to do what you wanted to

condense into your shape and become a snake
it’s better off this way to be unashamed
confronting what i’ve done
where i’m coming from
you only had to do what you wanted to
)";

const char* SONG_SINK = R"(someone came to take your mind away
darkest days trip and fall somewhere
deep down i know you're perfect
but i am so burned
i destroy what is broken
and live with what i've learned
i sever what is stolen
and live with what i've earned

thunderstorms your face interplanetary space
diving down so deep you won’t be
woken from your sleep
i cannot promise if you leave
i still won't need to hear you say
there was something in the water
that made you feel safe
there was something in the water
that made you feel safe

someone came to take your life today
no escape giving birth betrayed
i know you think it’s hopeless
but you just couldn't wait
determine what the truth is
consider what’s at stake
determine to be ruthless
with what you'll throw away

thunderstorms your face interplanetary space
diving down so deep you won’t be
woken from your sleep
i cannot promise if you leave
i still won't need to hear you say
there was something in the water
that made you feel safe
there was something in the water
that made you feel safe

there was something in the water
that made you feel safe
there was something in the water
that made you feel safe
there was something in the water
that made you feel safe
there was something in the water
that made you feel safe
)";

const char* SONG_YOU = R"(yeah but you and me
are as beautiful as we could be
yeah but you and me
are as beautiful as we could be
i never wanted this i never want to leave
i could go anywhere but i can’t do anything
oh but you and me
are as beautiful as we can be

all that you can see
are the people you could not be
and all that you could need
is the sacrosanct ephemeral bleed
you never wanted this
drowning in misery
you couldn’t stop yourself
but you can’t stop anything

oh but you and me
are as beautiful as we could be
yeah but you and me
are as beautiful as we could be
i never wanted this i never want to leave
i could go anywhere but i can’t do anything
oh but you and me
are as beautiful as we can be

oh but you and me
are as beautiful as we could be
yeah but you and me
are as beautiful as we could be
i never wanted this i never want to leave
i could go anywhere but i can’t do anything
oh but you and me
are as beautiful as we can be
)";

static void feed_to_terminal(TerminalWindow* tw, const std::string& data) {
    std::lock_guard<std::mutex> lock(tw->demo_target().grid_mutex);
    tw->demo_target().parser.parse(tw->demo_target().terminal, data.c_str(), data.length());
}

std::string get_song_lyrics(const std::string& song_name) {
    std::string lower = song_name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.find("coelacanth") != std::string::npos) return SONG_COELACANTH;
    if (lower.find("snake") != std::string::npos) return SONG_SNAKE;
    if (lower.find("sink") != std::string::npos || lower.find("thunderstorm") != std::string::npos) return SONG_SINK;
    if (lower.find("you") != std::string::npos) return SONG_YOU;
    return "";
}

// Types a song out a character at a time, so the cursor glides across each
// line the way it does when the demo types a command -- the renderer eases the
// cursor toward wherever the last character landed. Lyrics used to appear a
// whole line at a time.
//
// Paced to the song, not to a typist: at command-typing speed a song runs to a
// minute, far too long for sinkdemo. Each song takes about kLyricSongMs, with a
// short pause at the end of every line for phrasing. Progress follows the real
// clock rather than counting sleeps, so a stalled frame catches up instead of
// stretching the song. Characters are emitted whole: the lyrics have curly
// apostrophes, which are several bytes each.
//
// Returns true if the user skipped (Tab): the line in progress is finished at
// once and the rest of the song is left out, as before.
static constexpr double kLyricSongMs = 8000.0;
static constexpr double kLyricLinePauseMs = 90.0;
static constexpr double kLyricMinMsPerChar = 3.0;

static bool type_lyrics(TerminalWindow* tw, const std::string& lyrics) {
    std::vector<std::string> lines;
    std::stringstream ss(lyrics);
    std::string line;
    while (std::getline(ss, line)) lines.push_back(line);
    if (lines.empty()) return false;

    auto is_continuation = [](unsigned char b) { return (b & 0xC0) == 0x80; };
    size_t chars = 0;
    for (const auto& l : lines) {
        for (unsigned char b : l) chars += is_continuation(b) ? 0 : 1;
    }
    double typing_ms = kLyricSongMs - kLyricLinePauseMs * static_cast<double>(lines.size());
    double ms_per_char = std::max(kLyricMinMsPerChar, typing_ms / static_cast<double>(std::max<size_t>(chars, 1)));

    using clock = std::chrono::steady_clock;
    for (const auto& l : lines) {
        size_t i = 0;
        double owed = 0.0;  // characters due but not yet typed
        auto last = clock::now();
        while (i < l.size()) {
            if (sleep_interruptible(tw, 10)) {
                feed_to_terminal(tw, l.substr(i) + "\r\n");
                return true;
            }
            auto now = clock::now();
            owed += std::chrono::duration<double, std::milli>(now - last).count() / ms_per_char;
            last = now;
            size_t j = i;
            while (owed >= 1.0 && j < l.size()) {
                ++j;
                while (j < l.size() && is_continuation(static_cast<unsigned char>(l[j]))) ++j;
                owed -= 1.0;
            }
            if (j > i) {
                feed_to_terminal(tw, l.substr(i, j - i));
                i = j;
            }
        }
        feed_to_terminal(tw, "\r\n");
        if (sleep_interruptible(tw, static_cast<int>(kLyricLinePauseMs))) return true;
    }
    return false;
}

void run_sing(TerminalWindow* tw, const std::string& song_name) {
    if (!tw) return;
    tw->demo_running.store(true);
    tw->demo_skip_requested.store(false);

    std::string lyrics = get_song_lyrics(song_name);
    if (lyrics.empty()) {
        feed_to_terminal(tw, "\r\nUsage: sinksing <coelacanth | snake | sink | you>\r\n");
        tw->demo_running.store(false);
        tw->demo_skip_requested.store(false);
        const char c = '\r';
        tw->demo_target().pty.write_to_pty(&c, 1);
        return;
    }

    // Create a new line before starting lyrics
    feed_to_terminal(tw, "\r\n");
    type_lyrics(tw, lyrics);

    // Ensure finished lyrics return cleanly to the shell prompt
    feed_to_terminal(tw, "\r\n");

    tw->demo_running.store(false);
    tw->demo_skip_requested.store(false);

    if (tw->demo_abort.load()) return;

    const char c = '\r';
    tw->demo_target().pty.write_to_pty(&c, 1);
}

static void type_simulated_text(TerminalWindow* tw, const std::string& text) {
    static std::mt19937 rng(1337);
    std::uniform_int_distribution<int> dist(35, 65);
    for (size_t i = 0; i < text.length(); ++i) {
        std::string s(1, text[i]);
        feed_to_terminal(tw, s);
        if (sleep_interruptible(tw, dist(rng))) {
            if (i + 1 < text.length()) {
                feed_to_terminal(tw, text.substr(i + 1));
            }
            break;
        }
    }
}

// One character cell of the demo's ASCII video: a glyph, the colour it's
// drawn in and the colour behind it.
struct AsciiCell {
    char ch = ' ';
    uint8_t fg[3] = {0, 0, 0};
    uint8_t bg[3] = {0, 0, 0};
};

static const char kAsciiRamp[] = " .:-=+*#%@";
static constexpr int kAsciiRampLen = sizeof(kAsciiRamp) - 1;

static int ramp_index(char ch) {
    const char* at = std::strchr(kAsciiRamp, ch);
    return (at && ch) ? static_cast<int>(at - kAsciiRamp) : 0;
}

static void set_rgb(uint8_t out[3], int r, int g, int b) {
    out[0] = static_cast<uint8_t>(std::clamp(r, 0, 255));
    out[1] = static_cast<uint8_t>(std::clamp(g, 0, 255));
    out[2] = static_cast<uint8_t>(std::clamp(b, 0, 255));
}

// The video's look: a glyph picked by brightness, drawn brighter than the
// pixel over a background darker than it.
static AsciiCell ascii_cell_from_rgb(uint8_t cr, uint8_t cg, uint8_t cb) {
    AsciiCell cell;
    float lum = 0.2126f * cr + 0.7152f * cg + 0.0722f * cb;
    set_rgb(cell.bg, static_cast<int>(cr * 0.45f), static_cast<int>(cg * 0.45f), static_cast<int>(cb * 0.45f));
    set_rgb(cell.fg, static_cast<int>(cr * 1.25f + 15), static_cast<int>(cg * 1.25f + 15), static_cast<int>(cb * 1.25f + 15));
    cell.ch = kAsciiRamp[std::clamp(static_cast<int>(lum / 256.0f * kAsciiRampLen), 0, kAsciiRampLen - 1)];
    return cell;
}

static void append_cup(std::string& out, int row, int col) {
    out += "\033[" + std::to_string(row + 1) + ";" + std::to_string(col + 1) + "H";
}

static void append_cell(std::string& out, const AsciiCell& cell) {
    out += "\033[48;2;" + std::to_string(cell.bg[0]) + ";" + std::to_string(cell.bg[1]) + ";" + std::to_string(cell.bg[2]) + "m";
    out += "\033[38;2;" + std::to_string(cell.fg[0]) + ";" + std::to_string(cell.fg[1]) + ";" + std::to_string(cell.fg[2]) + "m";
    out += cell.ch;
}

// Where the finale's "swim with me" was typed, so the transition into the
// video can pick it up from there. Zero-based, like the grid's cursor.
struct SwimIntro {
    std::string prompt;
    int row = 0;
    int prompt_col = 0;
    std::string text;
};

// A stable pseudo-random value in [0, 1) for a cell, so per-cell timing and
// shading survive a resize without any state to rebuild.
static float cell_hash(int x, int y, uint32_t salt) {
    uint32_t h = static_cast<uint32_t>(x) * 0x8da6b343u ^ static_cast<uint32_t>(y) * 0xd8163841u ^ salt * 0xcb1ab31fu;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return static_cast<float>(h & 0xFFFFFF) / static_cast<float>(0x1000000);
}

static constexpr int kIntroFps = 30;
static constexpr double kShakeMs = 2000.0;
static constexpr double kBurstMs = 650.0;
// How far into the burst the video starts playing underneath it. The rest of
// the burst overlaps the dissolve, so the motion runs straight on into the
// video instead of settling first.
static constexpr double kVideoStartMs = 450.0;
static constexpr double kDissolveMs = 300.0;
// How far past the farthest corner the shockwave ends up, so the corners are
// well inside it by the time the video starts.
static constexpr double kBurstOvershoot = 1.08;
// The video's ending: the water drains out of the screen over its last
// kDrainMs, leaving the terminal behind.
static constexpr double kDrainMs = 900.0;

// Calls draw(t) about kIntroFps times a second, t being the fraction of
// duration_ms gone by on the real clock, ending with draw(1). A slow frame is
// caught up rather than stretching the phase. Returns false if Tab or a
// closing window cut it short.
static bool run_timed_phase(TerminalWindow* tw, double duration_ms, const std::function<void(double)>& draw) {
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    const auto frame = std::chrono::microseconds(1000000 / kIntroFps);
    auto next = start;
    for (;;) {
        if (tw->demo_skip_requested.load() || tw->demo_abort.load()) return false;
        double t = std::chrono::duration<double, std::milli>(clock::now() - start).count() / duration_ms;
        if (t >= 1.0) {
            draw(1.0);
            return true;
        }
        draw(t);
        next += frame;
        auto now = clock::now();
        if (next < now) next = now;
        std::this_thread::sleep_until(next);
    }
}

// Shockwave colour by how long ago the wave passed (as a fraction of its
// full travel): white-hot at the front, cooling through cyan to deep water.
static void burst_colour(double age, AsciiCell& cell) {
    auto mix = [](int a, int b, double f) { return static_cast<int>(a + (b - a) * f); };
    int r, g, b;
    if (age < 0.08) {
        double f = age / 0.08;
        r = mix(255, 110, f); g = mix(255, 225, f); b = mix(255, 255, f);
    } else {
        double f = std::min(1.0, (age - 0.08) / 0.42);
        r = mix(110, 25, f); g = mix(225, 95, f); b = mix(255, 150, f);
    }
    set_rgb(cell.fg, r, g, b);
    set_rgb(cell.bg, static_cast<int>(r * 0.45f), static_cast<int>(g * 0.45f), static_cast<int>(b * 0.45f));
}

// Glyph brightness behind the shockwave: dense at the front, thinning out
// as it cools.
static int burst_ramp_index(double age) {
    return std::clamp(kAsciiRampLen - 1 - static_cast<int>(age * 12.0), 2, kAsciiRampLen - 1);
}

// The shockwave's geometry on a grid of a given size. Distances count rows
// double, since a cell is about twice as tall as it is wide -- so the wave
// spreads as a circle. Rebuilt from the grid size each frame, so a resize
// mid-transition just carries on at the new size.
struct SwimBurst {
    double x = 0.0;      // the column it bursts from: the middle of the text
    int row = 0;
    double max_d = 1.0;  // distance to the farthest corner

    SwimBurst(const SwimIntro& intro, int cols, int rows)
        : x(intro.prompt_col + static_cast<double>(intro.prompt.size()) +
            (static_cast<double>(intro.text.size()) - 1.0) / 2.0),
          row(std::clamp(intro.row, 0, rows - 1)) {
        max_d = std::max({dist(0, 0), dist(cols - 1, 0), dist(0, rows - 1), dist(cols - 1, rows - 1), 1.0});
    }

    double dist(int cx, int cy) const {
        double dx = cx - x, dy = 2.0 * (cy - row);
        return std::sqrt(dx * dx + dy * dy);
    }

    // How far the front has got burst_ms in: fast at first, easing off as it
    // reaches the corners.
    double radius(double burst_ms) const {
        double t = std::min(1.0, burst_ms / kBurstMs);
        return max_d * kBurstOvershoot * (1.0 - std::pow(1.0 - t, 3.0));
    }

    // The shockwave at a cell, burst_ms in, its glyph flickering frame to
    // frame. False if the front hasn't reached the cell yet.
    bool cell(int cx, int cy, double burst_ms, std::mt19937& rng, AsciiCell& out) const {
        double d = dist(cx, cy), r = radius(burst_ms);
        if (d > r) return false;
        double age = (r - d) / max_d;
        burst_colour(age, out);
        int jitter = static_cast<int>(rng() % 5) - 2;
        out.ch = kAsciiRamp[std::clamp(burst_ramp_index(age) + jitter, 1, kAsciiRampLen - 1)];
        return true;
    }
};

// The finale's transition into the video. The typed "swim with me" shakes
// and swells, faster and harder, for the two seconds the demo used to just
// pause for; then bursts, a shockwave of ASCII filling the screen. The video
// starts underneath before the burst is over, and blend_burst_into_video
// dissolves one into the other.
//
// Returns whether it ran to the end. Tab cuts it short, straight to the
// video with no dissolve; so does a closing window.
static bool play_swim_intro(TerminalWindow* tw, const SwimIntro& intro) {
    std::mt19937 rng(1337);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    auto dims = [&](int& cols, int& rows) {
        cols = tw->demo_target().terminal.get_cols();
        rows = tw->demo_target().terminal.get_rows();
        if (cols <= 0) cols = 80;
        if (rows <= 0) rows = 24;
    };

    const int n = static_cast<int>(intro.text.size());
    const int text_col = intro.prompt_col + static_cast<int>(intro.prompt.size());
    const double text_mid = text_col + (n - 1) / 2.0;

    // Where letter i sits for a given swell and throb, before any shaking.
    auto letter_x = [&](int i, double e, double throb) {
        double spread = 1.0 + e * (0.35 + 0.2 * throb);
        return text_mid + (text_col + i - text_mid) * spread;
    };

    // 1. Shake: the letters throb, spread, swap between upper and lower case
    // and heat up to white, faster as it builds, throwing sparks near the end.
    auto draw_shake = [&](double t) {
        int cols, rows;
        dims(cols, rows);
        const int row = std::clamp(intro.row, 0, rows - 1);
        const double e = t * t;
        const double throb = std::sin(2.0 * M_PI * (1.5 * t + 4.0 * t * t * t));

        std::string out = "\033[0m";
        for (int r = std::max(0, row - 2); r <= std::min(rows - 1, row + 2); ++r) {
            append_cup(out, r, 0);
            out += "\033[2K";
        }
        if (intro.prompt_col < cols) {
            append_cup(out, row, intro.prompt_col);
            out += intro.prompt.substr(0, cols - intro.prompt_col);
        }

        AsciiCell hot;
        set_rgb(hot.fg, static_cast<int>(170 + 85 * e), static_cast<int>(230 + 25 * e), static_cast<int>(245 + 10 * e));
        out += "\033[38;2;" + std::to_string(hot.fg[0]) + ";" + std::to_string(hot.fg[1]) + ";" + std::to_string(hot.fg[2]) + "m";
        if (e > 0.5) out += "\033[1m";
        for (int i = 0; i < n; ++i) {
            char ch = intro.text[i];
            if (ch == ' ') continue;
            if (throb > 1.0 - 1.6 * e) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            double x = letter_x(i, e, throb);
            int y = row;
            if (unit(rng) < 0.25 + 0.75 * e) x += (unit(rng) * 2.0 - 1.0) * 2.5 * e;
            if (unit(rng) < 0.6 * e * e) y += unit(rng) < 0.5 ? -1 : 1;
            int col = static_cast<int>(std::lround(x));
            if (col < 0 || col >= cols || y < 0 || y >= rows) continue;
            append_cup(out, y, col);
            out += ch;
        }
        if (e > 0.6) {
            static const char sparks[] = ".'`*+";
            int count = static_cast<int>((e - 0.6) * 30.0);
            double reach = (n / 2.0 + 3.0) * (1.0 + 0.55 * e);
            out += "\033[38;2;255;240;220m";
            for (int k = 0; k < count; ++k) {
                int col = static_cast<int>(std::lround(text_mid + (unit(rng) * 2.0 - 1.0) * reach));
                int y = row + static_cast<int>(rng() % 5) - 2;
                if (col < 0 || col >= cols || y < 0 || y >= rows) continue;
                append_cup(out, y, col);
                out += sparks[rng() % 5];
            }
        }
        out += "\033[0m";
        feed_to_terminal(tw, out);
    };

    auto finish = [&](bool completed) {
        // Tab skips straight to the video; it's spent here so the video
        // doesn't take it as a request to skip itself too.
        if (!completed && !tw->demo_abort.load()) tw->demo_skip_requested.store(false);
        return completed;
    };

    if (!finish(run_timed_phase(tw, kShakeMs, draw_shake))) return false;

    // The letters fly off as debris from where they were at the peak.
    struct Debris { double x, y, vx, vy; char ch; };
    std::vector<Debris> debris;
    {
        int cols, rows;
        dims(cols, rows);
        SwimBurst burst(intro, cols, rows);
        const double speed = burst.max_d * 1.3 / (kBurstMs / 1000.0);
        for (int i = 0; i < n; ++i) {
            if (intro.text[i] == ' ') continue;
            for (int k = 0; k < 10; ++k) {
                double angle = unit(rng) * 2.0 * M_PI;
                double v = speed * (0.5 + unit(rng));
                char ch = k < 3 ? intro.text[i] : "*+'."[rng() % 4];
                debris.push_back({letter_x(i, 1.0, 1.0), static_cast<double>(burst.row),
                                  std::cos(angle) * v, std::sin(angle) * v * 0.5, ch});
            }
        }
    }

    // 2. Burst: a shockwave fills the screen behind it with ASCII, up to the
    // point the video takes over.
    std::vector<char> overlay;
    auto draw_burst = [&](double t) {
        int cols, rows;
        dims(cols, rows);
        SwimBurst burst(intro, cols, rows);
        const double burst_ms = t * kVideoStartMs;
        const double secs = burst_ms / 1000.0;

        overlay.assign(static_cast<size_t>(cols) * rows, 0);
        for (const Debris& d : debris) {
            int x = static_cast<int>(std::lround(d.x + d.vx * secs));
            int y = static_cast<int>(std::lround(d.y + d.vy * secs));
            if (x >= 0 && x < cols && y >= 0 && y < rows) overlay[static_cast<size_t>(y) * cols + x] = d.ch;
        }

        std::string out;
        out.reserve(static_cast<size_t>(cols) * rows * 25);
        for (int y = 0; y < rows; ++y) {
            append_cup(out, y, 0);
            for (int x = 0; x < cols; ++x) {
                AsciiCell cell;
                char debris_ch = overlay[static_cast<size_t>(y) * cols + x];
                if (burst.cell(x, y, burst_ms, rng, cell)) {
                    append_cell(out, cell);
                } else if (debris_ch) {
                    out += "\033[0m\033[1;38;2;255;250;235m";
                    out += debris_ch;
                } else {
                    out += "\033[0m ";
                }
            }
        }
        out += "\033[0m";
        feed_to_terminal(tw, out);
    };

    return finish(run_timed_phase(tw, kVideoStartMs, draw_burst));
}

// 3. Dissolve, over the video's first kDissolveMs: outward from where it
// burst, each cell settles from the still-running shockwave into the playing
// video, its glyph flickering toward the frame's. video_ms is how far into
// the video this frame is.
static void blend_burst_into_video(std::vector<AsciiCell>& cells, int cols, int rows,
                                   const SwimIntro& intro, double video_ms, std::mt19937& rng) {
    SwimBurst burst(intro, cols, rows);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const double t = video_ms / kDissolveMs;
    const double burst_ms = kVideoStartMs + video_ms;
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < cols; ++x) {
            double delay = 0.6 * std::min(1.0, burst.dist(x, y) / burst.max_d) + 0.12 * cell_hash(x, y, 1);
            double p = std::clamp((t - delay) / 0.28, 0.0, 1.0);
            if (p >= 1.0) continue;
            AsciiCell from;
            if (!burst.cell(x, y, burst_ms, rng, from)) continue;
            p = p * p * (3.0 - 2.0 * p);

            AsciiCell& cell = cells[static_cast<size_t>(y) * cols + x];
            const AsciiCell want = cell;
            for (int c = 0; c < 3; ++c) {
                cell.fg[c] = static_cast<uint8_t>(from.fg[c] + (want.fg[c] - from.fg[c]) * p);
                cell.bg[c] = static_cast<uint8_t>(from.bg[c] + (want.bg[c] - from.bg[c]) * p);
            }
            if (unit(rng) >= p * p) {
                double idx = ramp_index(from.ch) + (ramp_index(want.ch) - ramp_index(from.ch)) * p;
                int jitter = static_cast<int>(rng() % 3) - 1;
                cell.ch = kAsciiRamp[std::clamp(static_cast<int>(std::lround(idx)) + jitter, 0, kAsciiRampLen - 1)];
            }
        }
    }
}

// The video's ending: the screen drains like a sink. A wavy water surface
// falls from above the top row to below the bottom one as u goes from 0 to
// 1, slowly at first and then faster. Above it the video is gone, but for a
// few droplets left behind; the surface itself is a line of foam that follows
// the waves' slopes; below it the video plays on, rippling and lit up near
// the surface. Everything is worked out from u, so a drain carried on over a
// frozen last frame picks up exactly where the playing one left off.
static void draw_drain(std::string& out, const std::vector<AsciiCell>& video, int cols, int rows, double u) {
    const double secs = u * kDrainMs / 1000.0;
    const double eased = u * u * (3.0 - 2.0 * u);
    const double level = -2.5 + (rows + 5.0) * eased;
    auto surface = [&](int x) {
        return level + 1.6 * (0.65 * std::sin(0.23 * x + 5.0 * secs) +
                              0.35 * std::sin(0.09 * x - 3.1 * secs + 1.3));
    };

    out.reserve(out.size() + static_cast<size_t>(cols) * rows * 40);
    for (int y = 0; y < rows; ++y) {
        append_cup(out, y, 0);
        for (int x = 0; x < cols; ++x) {
            const double s = surface(x);
            const int top = static_cast<int>(std::lround(s));
            if (y < top) {
                // Spray clinging just above the water, thinning out as it drains.
                if (y > s - 4.0 && cell_hash(x, y, 7) < 0.06 * (1.0 - u)) {
                    out += "\033[0m\033[38;2;120;200;230m";
                    out += cell_hash(x, y, 8) < 0.5 ? '.' : '\'';
                } else {
                    out += "\033[0m ";
                }
            } else if (y == top) {
                double slope = surface(x + 1) - surface(x - 1);
                char ch = slope > 0.7 ? '\\' : slope < -0.7 ? '/' : (cell_hash(x, 0, 9) < 0.7 ? '~' : '-');
                out += "\033[0m\033[1;38;2;225;248;255m";
                out += ch;
            } else {
                // Below the surface: the video, bent sideways and brightened
                // the nearer it is to the top of the water.
                double near = std::max(0.0, 1.0 - (y - s) / 4.0);
                int sx = x + static_cast<int>(std::lround(std::sin(0.7 * y + 0.3 * x + 6.0 * secs) * near));
                AsciiCell cell = video[static_cast<size_t>(y) * cols + std::clamp(sx, 0, cols - 1)];
                static const int glow[3] = {200, 240, 255};
                for (int c = 0; c < 3; ++c) {
                    cell.fg[c] = static_cast<uint8_t>(cell.fg[c] + (glow[c] - cell.fg[c]) * 0.35 * near);
                }
                append_cell(out, cell);
            }
        }
    }
    out += "\033[0m";
}

static void play_cpp_video_as_text(TerminalWindow* tw, const std::string& dat_path,
                                   const SwimIntro* intro = nullptr) {
    std::ifstream file(dat_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return;

    std::streamsize size = file.tellg();
    if (size <= 16) return;

    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> raw_data(size);
    if (!file.read(reinterpret_cast<char*>(raw_data.data()), size)) {
        return;
    }
    file.close();

    // Verify magic header "SINKDEMO_ARCHIVE"
    const char magic[16] = {'S','I','N','K','D','E','M','O','_','A','R','C','H','I','V','E'};
    if (std::memcmp(raw_data.data(), magic, 16) != 0) {
        return;
    }

    // Unmask obfuscated payload bytes
    static const uint8_t KEY[4] = { 0x53, 0x49, 0x4E, 0x4B };
    size_t payload_size = raw_data.size() - 16;
    std::vector<uint8_t> video_bytes(payload_size);
    for (size_t i = 0; i < payload_size; ++i) {
        video_bytes[i] = raw_data[16 + i] ^ KEY[i % 4];
    }

    BufferData bd = { video_bytes.data(), video_bytes.size(), 0 };

    size_t avio_ctx_buffer_size = 32768;
    uint8_t* avio_ctx_buffer = (uint8_t*)av_malloc(avio_ctx_buffer_size);
    if (!avio_ctx_buffer) return;

    AVIOContext* avio_ctx = avio_alloc_context(
        avio_ctx_buffer, static_cast<int>(avio_ctx_buffer_size),
        0, &bd, &read_packet_cb, nullptr, &seek_cb
    );

    if (!avio_ctx) {
        av_free(avio_ctx_buffer);
        return;
    }

    AVFormatContext* fmt_ctx = avformat_alloc_context();
    if (!fmt_ctx) {
        av_freep(&avio_ctx->buffer);
        avio_context_free(&avio_ctx);
        return;
    }

    fmt_ctx->pb = avio_ctx;
    if (avformat_open_input(&fmt_ctx, nullptr, nullptr, nullptr) != 0) {
        av_freep(&avio_ctx->buffer);
        avio_context_free(&avio_ctx);
        avformat_free_context(fmt_ctx);
        return;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        avformat_close_input(&fmt_ctx);
        av_freep(&avio_ctx->buffer);
        avio_context_free(&avio_ctx);
        return;
    }

    int video_stream_idx = -1;
    for (unsigned int i = 0; i < static_cast<unsigned int>(fmt_ctx->nb_streams); i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            break;
        }
    }

    if (video_stream_idx == -1) {
        avformat_close_input(&fmt_ctx);
        av_freep(&avio_ctx->buffer);
        avio_context_free(&avio_ctx);
        return;
    }

    AVCodecParameters* codec_params = fmt_ctx->streams[video_stream_idx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codec_params->codec_id);
    if (!codec) {
        avformat_close_input(&fmt_ctx);
        av_freep(&avio_ctx->buffer);
        avio_context_free(&avio_ctx);
        return;
    }

    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, codec_params);
    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        av_freep(&avio_ctx->buffer);
        avio_context_free(&avio_ctx);
        return;
    }

    int cols = tw->demo_target().terminal.get_cols();
    int rows = tw->demo_target().terminal.get_rows();
    if (cols <= 0) cols = 80;
    if (rows <= 0) rows = 24;

    SwsContext* sws_ctx = nullptr;
    AVFrame* frame = av_frame_alloc();
    AVFrame* rgb_frame = av_frame_alloc();
    uint8_t* buffer = nullptr;

    auto update_buffer = [&](int c, int r) {
        if (buffer) av_free(buffer);
        int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, c, r, 1);
        buffer = (uint8_t*)av_malloc(num_bytes * sizeof(uint8_t));
        av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize, buffer, AV_PIX_FMT_RGB24, c, r, 1);
    };

    update_buffer(cols, rows);

    AVPacket packet;

    // With an intro, "swim with me" stays on screen for it to pick up;
    // otherwise playback starts from a clean screen.
    feed_to_terminal(tw, intro ? "\033[?25l\033[3J" : "\033[?25l\033[2J\033[3J\033[H"); // Hide cursor, clear scrollback (and screen)

    auto start_clock = std::chrono::high_resolution_clock::now();
    int frame_count = 0;
    double frame_delay_sec = 1.0 / 23.976;
    bool intro_pending = intro != nullptr;
    bool blending = false;
    std::mt19937 blend_rng(1337);

    // When to start draining (see draw_drain), on the same frame-count clock
    // the frames are paced by. Unknown length: drain after the last frame.
    AVStream* video_stream = fmt_ctx->streams[video_stream_idx];
    double total_frames = 0.0;
    if (video_stream->nb_frames > 0) {
        total_frames = static_cast<double>(video_stream->nb_frames);
    } else if (video_stream->duration != AV_NOPTS_VALUE) {
        total_frames = video_stream->duration * av_q2d(video_stream->time_base) / frame_delay_sec;
    }
    const double drain_start_ms = total_frames * frame_delay_sec * 1000.0 - kDrainMs;
    double drain_u = 0.0;
    std::vector<AsciiCell> cells;

    // Scales the current decoded frame to a c x r grid of cells.
    auto scale_frame = [&](int c, int r, std::vector<AsciiCell>& out) {
        if (frame->width <= 0 || frame->height <= 0) return false;

        if (c != cols || r != rows || !buffer) {
            cols = c;
            rows = r;
            update_buffer(cols, rows);
        }

        sws_ctx = sws_getCachedContext(
            sws_ctx,
            frame->width, frame->height, (AVPixelFormat)frame->format,
            cols, rows, AV_PIX_FMT_RGB24,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );

        if (!sws_ctx) return false;

        sws_scale(
            sws_ctx,
            (const uint8_t* const*)frame->data, frame->linesize,
            0, frame->height,
            rgb_frame->data, rgb_frame->linesize
        );

        out.resize(static_cast<size_t>(cols) * rows);
        for (int y = 0; y < rows; ++y) {
            const uint8_t* line = rgb_frame->data[0] + y * rgb_frame->linesize[0];
            for (int x = 0; x < cols; ++x) {
                out[static_cast<size_t>(y) * cols + x] = ascii_cell_from_rgb(line[x * 3], line[x * 3 + 1], line[x * 3 + 2]);
            }
        }
        return true;
    };

    auto render_frame_lambda = [&]() {
        if (frame->width <= 0 || frame->height <= 0) return;

        // The intro plays once the first frame is decoded and ready, so the
        // video can start the moment the intro hands over; its clock starts
        // then too.
        if (intro_pending) {
            intro_pending = false;
            blending = play_swim_intro(tw, *intro);
            if (tw->demo_abort.load()) return;
            start_clock = std::chrono::high_resolution_clock::now();
            frame_count = 0;
        }

        int cur_cols = tw->demo_target().terminal.get_cols();
        int cur_rows = tw->demo_target().terminal.get_rows();
        if (cur_cols <= 0) cur_cols = 80;
        if (cur_rows <= 0) cur_rows = 24;

        if (!scale_frame(cur_cols, cur_rows, cells)) return;

        if (blending) {
            double video_ms = frame_count * frame_delay_sec * 1000.0;
            if (video_ms < kDissolveMs) {
                blend_burst_into_video(cells, cols, rows, *intro, video_ms, blend_rng);
            } else {
                blending = false;
            }
        }

        auto target_time = start_clock + std::chrono::microseconds(static_cast<long long>(frame_count * frame_delay_sec * 1000000.0));
        std::this_thread::sleep_until(target_time);

        std::string frame_buf;
        frame_buf.reserve(cols * rows * 25);

        double video_ms = frame_count * frame_delay_sec * 1000.0;
        if (total_frames > 0.0 && video_ms >= drain_start_ms) {
            drain_u = std::min(1.0, (video_ms - drain_start_ms) / kDrainMs);
            draw_drain(frame_buf, cells, cols, rows, drain_u);
        } else {
            for (int r = 0; r < rows; ++r) {
                frame_buf += "\033[" + std::to_string(r + 1) + ";1H";
                for (int c = 0; c < cols; ++c) {
                    append_cell(frame_buf, cells[static_cast<size_t>(r) * cols + c]);
                }
            }
            frame_buf += "\033[0m";
        }
        feed_to_terminal(tw, frame_buf);
        frame_count++;
    };

    auto should_stop = [&]() {
        return tw->demo_skip_requested.load() || tw->demo_abort.load();
    };

    // Tab during the video ends it there and then, drain and all. The skip
    // is spent below, so remember it.
    bool cut_short = false;
    while (av_read_frame(fmt_ctx, &packet) >= 0) {
        if (should_stop()) {
            cut_short = true;
            tw->demo_skip_requested.store(false);
            av_packet_unref(&packet);
            break;
        }
        if (packet.stream_index == video_stream_idx) {
            int ret = avcodec_send_packet(codec_ctx, &packet);
            while (ret == AVERROR(EAGAIN)) {
                while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                    render_frame_lambda();
                    if (should_stop()) break;
                }
                if (should_stop()) break;
                ret = avcodec_send_packet(codec_ctx, &packet);
            }
            if (ret >= 0) {
                while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                    render_frame_lambda();
                    if (should_stop()) break;
                }
            }
        }
        av_packet_unref(&packet);
    }

    cut_short = cut_short || should_stop();
    if (!cut_short) {
        // The decoder holds back its last few frames until told the input
        // has ended; without them the drain would come up short.
        avcodec_send_packet(codec_ctx, nullptr);
        while (!should_stop() && avcodec_receive_frame(codec_ctx, frame) == 0) {
            render_frame_lambda();
        }
    }

    // The video ran out before the water did (or its length was unknown):
    // finish draining over the last frame.
    if (!cut_short && !should_stop() && !cells.empty() && drain_u < 1.0) {
        const double from = drain_u;
        bool finished = run_timed_phase(tw, (1.0 - from) * kDrainMs, [&](double t) {
            std::string out;
            draw_drain(out, cells, cols, rows, from + (1.0 - from) * t);
            feed_to_terminal(tw, out);
        });
        if (!finished && !tw->demo_abort.load()) tw->demo_skip_requested.store(false);
    }

    feed_to_terminal(tw, "\033[?25h\033[0m\033[2J\033[3J\033[H"); // Restore cursor, clear screen & scrollback

    if (buffer) av_free(buffer);
    av_frame_free(&rgb_frame);
    av_frame_free(&frame);
    if (sws_ctx) sws_freeContext(sws_ctx);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    av_freep(&avio_ctx->buffer);
    avio_context_free(&avio_ctx);
}

void run_demo(TerminalWindow* tw, AppState* state) {
    if (!tw) return;
    tw->demo_running.store(true);
    tw->demo_skip_requested.store(false);

    std::string prompt = "moon@River ~ % ";
    
    // 1. Initial screen clear
    feed_to_terminal(tw, "\033[2J\033[H");

    auto do_song_step = [&](const std::string& song_name) {
        feed_to_terminal(tw, prompt);
        std::string cmd = "sinksing " + song_name;
        type_simulated_text(tw, cmd);
        sleep_interruptible(tw, 200); // 0.2s hesitation
        feed_to_terminal(tw, "\r\n\r\n");

        // The lyrics, typed out (Tab skips the rest of the song).
        type_lyrics(tw, get_song_lyrics(song_name));

        sleep_interruptible(tw, 200);
        feed_to_terminal(tw, prompt);
        sleep_interruptible(tw, 200); // 0.2s delay
        type_simulated_text(tw, "clear");
        sleep_interruptible(tw, 200); // 0.2s hesitation
        feed_to_terminal(tw, "\r\n\033[2J\033[H"); // Clear screen
    };

    // The window may be closing (demo_abort set): unwind between steps
    // instead of running the rest of the multi-second sequence, so the
    // thread finishes quickly and the caller can safely delete tw.
    if (tw->demo_abort.load()) { tw->demo_running.store(false); return; }
    do_song_step("coelacanth");
    if (tw->demo_abort.load()) { tw->demo_running.store(false); return; }

    // Finale sequence
    SwimIntro intro;
    intro.prompt = prompt;
    intro.text = "swim with me";
    {
        std::lock_guard<std::mutex> lock(tw->demo_target().grid_mutex);
        intro.row = tw->demo_target().terminal.get_cursor_row();
        intro.prompt_col = tw->demo_target().terminal.get_cursor_col();
    }
    feed_to_terminal(tw, prompt);
    sleep_interruptible(tw, 1000); // 1s pause
    type_simulated_text(tw, intro.text);
    sleep_interruptible(tw, 200); // 0.2s hesitation
    feed_to_terminal(tw, "\r\n");

    if (tw->demo_abort.load()) { tw->demo_running.store(false); return; }

    // Resolve splash.dat path dynamically. The words shake, burst and
    // dissolve into the video (see play_swim_intro).
    std::string splash_path = resolve_splash_path();
    play_cpp_video_as_text(tw, splash_path, &intro);

    // Reset demo state flags for this window
    tw->demo_running.store(false);
    tw->demo_skip_requested.store(false);

    if (tw->demo_abort.load()) return;

    // Return to the user's interactive shell prompt at row 0 (top of window)
    const char c = '\x0c'; // Ctrl+L clears terminal and redraws prompt at row 0
    tw->demo_target().pty.write_to_pty(&c, 1);
}

} // namespace SinkDemo
