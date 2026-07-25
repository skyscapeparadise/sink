#include "sink_demo.hpp"
#include "terminal_grid.hpp"
#include "pty_bridge.hpp"
#include "ansi_parser.hpp"
#include "video_engine.hpp"

#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <random>
#include <cstring>
#include <vector>
#include <atomic>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

// Forward struct definitions
struct TerminalWindow {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    FontManager font_manager;
    TerminalGrid terminal;
    PTYBridge pty;
    ANSIParser parser;
    VideoEngine video_engine;
    std::mutex grid_mutex;
};

struct AppState {
    std::vector<TerminalWindow*> windows;
    TerminalWindow* active_window = nullptr;
    std::string video_path;
};

namespace SinkDemo {

static std::atomic<bool> g_demo_running{false};
static std::atomic<bool> g_skip_requested{false};

bool is_demo_running() {
    return g_demo_running.load();
}

void request_skip() {
    g_skip_requested.store(true);
}

static bool sleep_interruptible(int ms) {
    int ticks = ms / 10;
    if (ticks <= 0) ticks = 1;
    for (int t = 0; t < ticks; ++t) {
        if (g_skip_requested.load()) {
            g_skip_requested.store(false);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return g_skip_requested.exchange(false);
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
    std::lock_guard<std::mutex> lock(tw->grid_mutex);
    tw->parser.parse(tw->terminal, data.c_str(), data.length());
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

bool is_demo_command(const std::string& cmd) {
    std::string trimmed = cmd;
    trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
    trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);
    return (trimmed == "sinkdemo" || trimmed == "./sinkdemo");
}

bool is_sing_command(const std::string& cmd) {
    std::string trimmed = cmd;
    trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
    return (trimmed.rfind("sinksing", 0) == 0 || trimmed.rfind("./sinksing", 0) == 0);
}

void run_sing(TerminalWindow* tw, const std::string& song_name) {
    std::string lyrics = get_song_lyrics(song_name);
    if (lyrics.empty()) {
        feed_to_terminal(tw, "\r\nUsage: sinksing <coelacanth | snake | sink | you>\r\n");
        const char c = '\x0c';
        tw->pty.write_to_pty(&c, 1);
        return;
    }

    std::vector<std::string> lines;
    std::stringstream ss(lyrics);
    std::string line;
    while (std::getline(ss, line)) {
        lines.push_back(line);
    }

    if (lines.empty()) return;

    // Create a new line before starting lyrics
    feed_to_terminal(tw, "\r\n");

    double delay_per_line_ms = 3000.0 / lines.size();
    for (const auto& l : lines) {
        feed_to_terminal(tw, l + "\r\n");
        if (sleep_interruptible(static_cast<int>(delay_per_line_ms))) {
            break;
        }
    }

    // Ensure finished lyrics return cleanly to the shell prompt
    feed_to_terminal(tw, "\r\n");
    const char c = '\r';
    tw->pty.write_to_pty(&c, 1);
}

static void type_simulated_text(TerminalWindow* tw, const std::string& text) {
    static std::mt19937 rng(1337);
    std::uniform_int_distribution<int> dist(35, 65);
    for (size_t i = 0; i < text.length(); ++i) {
        std::string s(1, text[i]);
        feed_to_terminal(tw, s);
        if (sleep_interruptible(dist(rng))) {
            if (i + 1 < text.length()) {
                feed_to_terminal(tw, text.substr(i + 1));
            }
            break;
        }
    }
}

static void play_cpp_video_as_text(TerminalWindow* tw, const std::string& video_path) {
    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, video_path.c_str(), nullptr, nullptr) != 0) {
        return;
    }
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        avformat_close_input(&fmt_ctx);
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
        return;
    }

    AVCodecParameters* codec_params = fmt_ctx->streams[video_stream_idx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codec_params->codec_id);
    if (!codec) {
        avformat_close_input(&fmt_ctx);
        return;
    }

    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, codec_params);
    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return;
    }

    int cols = tw->terminal.get_cols();
    int rows = tw->terminal.get_rows();
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
    const char* chars = " .:-=+*#%@";
    int char_len = std::strlen(chars);

    feed_to_terminal(tw, "\033[?25l\033[2J\033[3J\033[H"); // Hide cursor, clear screen & scrollback

    auto start_clock = std::chrono::high_resolution_clock::now();
    int frame_count = 0;
    double frame_delay_sec = 1.0 / 23.976;

    auto render_frame_lambda = [&]() {
        if (frame->width <= 0 || frame->height <= 0) return;

        int cur_cols = tw->terminal.get_cols();
        int cur_rows = tw->terminal.get_rows();
        if (cur_cols <= 0) cur_cols = 80;
        if (cur_rows <= 0) cur_rows = 24;

        if (cur_cols != cols || cur_rows != rows || !buffer) {
            cols = cur_cols;
            rows = cur_rows;
            update_buffer(cols, rows);
        }

        sws_ctx = sws_getCachedContext(
            sws_ctx,
            frame->width, frame->height, (AVPixelFormat)frame->format,
            cols, rows, AV_PIX_FMT_RGB24,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );

        if (!sws_ctx) return;

        sws_scale(
            sws_ctx,
            (const uint8_t* const*)frame->data, frame->linesize,
            0, frame->height,
            rgb_frame->data, rgb_frame->linesize
        );

        auto target_time = start_clock + std::chrono::microseconds(static_cast<long long>(frame_count * frame_delay_sec * 1000000.0));
        std::this_thread::sleep_until(target_time);

        std::string frame_buf;
        frame_buf.reserve(cols * rows * 25);

        for (int r = 0; r < rows; ++r) {
            frame_buf += "\033[" + std::to_string(r + 1) + ";1H";

            for (int c = 0; c < cols; ++c) {
                int idx = (r * rgb_frame->linesize[0]) + c * 3;
                uint8_t cr = rgb_frame->data[0][idx];
                uint8_t cg = rgb_frame->data[0][idx + 1];
                uint8_t cb = rgb_frame->data[0][idx + 2];
                float lum = 0.2126f * cr + 0.7152f * cg + 0.0722f * cb;

                int bg_r = std::max(0, static_cast<int>(cr * 0.45f));
                int bg_g = std::max(0, static_cast<int>(cg * 0.45f));
                int bg_b = std::max(0, static_cast<int>(cb * 0.45f));

                int fg_r = std::min(255, static_cast<int>(cr * 1.25f + 15));
                int fg_g = std::min(255, static_cast<int>(cg * 1.25f + 15));
                int fg_b = std::min(255, static_cast<int>(cb * 1.25f + 15));

                int ch_idx = std::min(char_len - 1, static_cast<int>(lum / 256.0f * char_len));
                if (ch_idx < 0) ch_idx = 0;
                char ch = chars[ch_idx];

                frame_buf += "\033[48;2;" + std::to_string(bg_r) + ";" + std::to_string(bg_g) + ";" + std::to_string(bg_b) + "m";
                frame_buf += "\033[38;2;" + std::to_string(fg_r) + ";" + std::to_string(fg_g) + ";" + std::to_string(fg_b) + "m";
                frame_buf += ch;
            }
        }
        frame_buf += "\033[0m";
        feed_to_terminal(tw, frame_buf);
        frame_count++;
    };

    while (av_read_frame(fmt_ctx, &packet) >= 0) {
        if (g_skip_requested.load()) {
            g_skip_requested.store(false);
            av_packet_unref(&packet);
            break;
        }
        if (packet.stream_index == video_stream_idx) {
            int ret = avcodec_send_packet(codec_ctx, &packet);
            while (ret == AVERROR(EAGAIN)) {
                while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                    render_frame_lambda();
                    if (g_skip_requested.load()) break;
                }
                if (g_skip_requested.load()) break;
                ret = avcodec_send_packet(codec_ctx, &packet);
            }
            if (ret >= 0) {
                while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                    render_frame_lambda();
                    if (g_skip_requested.load()) break;
                }
            }
        }
        av_packet_unref(&packet);
    }

    feed_to_terminal(tw, "\033[?25h\033[0m\033[2J\033[3J\033[H"); // Restore cursor, clear screen & scrollback

    if (buffer) av_free(buffer);
    av_frame_free(&rgb_frame);
    av_frame_free(&frame);
    if (sws_ctx) sws_freeContext(sws_ctx);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
}

void run_demo(TerminalWindow* tw, AppState* state) {
    g_demo_running.store(true);
    g_skip_requested.store(false);

    std::string prompt = "moon@Thunderstorm ~ % ";
    
    // 1. Initial screen clear
    feed_to_terminal(tw, "\033[2J\033[H");

    auto do_song_step = [&](const std::string& song_name) {
        feed_to_terminal(tw, prompt);
        std::string cmd = "sinksing " + song_name;
        type_simulated_text(tw, cmd);
        sleep_interruptible(200); // 0.2s hesitation
        feed_to_terminal(tw, "\r\n\r\n");

        // Print song lyrics line by line
        std::string lyrics = get_song_lyrics(song_name);
        std::vector<std::string> lines;
        std::stringstream ss(lyrics);
        std::string line;
        while (std::getline(ss, line)) {
            lines.push_back(line);
        }

        if (!lines.empty()) {
            double delay_per_line_ms = 3000.0 / lines.size();
            for (const auto& l : lines) {
                feed_to_terminal(tw, l + "\r\n");
                if (sleep_interruptible(static_cast<int>(delay_per_line_ms))) {
                    break; // TAB pressed: skip rest of song immediately!
                }
            }
        }

        sleep_interruptible(200);
        feed_to_terminal(tw, prompt);
        sleep_interruptible(200); // 0.2s delay
        type_simulated_text(tw, "clear");
        sleep_interruptible(200); // 0.2s hesitation
        feed_to_terminal(tw, "\r\n\033[2J\033[H"); // Clear screen
    };

    do_song_step("coelacanth");
    do_song_step("snake");
    do_song_step("sink");
    do_song_step("you");

    // Finale sequence
    feed_to_terminal(tw, prompt);
    sleep_interruptible(1000); // 1s pause
    type_simulated_text(tw, "swim with me");
    sleep_interruptible(200); // 0.2s hesitation
    feed_to_terminal(tw, "\r\n");

    sleep_interruptible(2000); // 2s pause

    // Resolve splash.mp4 path
    std::string splash_path = "/Users/kady/Code/sinkdemo/splash.mp4";
    play_cpp_video_as_text(tw, splash_path);

    // Reset demo state flags
    g_demo_running.store(false);
    g_skip_requested.store(false);

    // Return to the user's interactive shell prompt at row 0 (top of window)
    const char c = '\x0c'; // Ctrl+L clears terminal and redraws prompt at row 0
    tw->pty.write_to_pty(&c, 1);
}

} // namespace SinkDemo
