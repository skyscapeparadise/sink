#pragma once

#include <SDL3/SDL.h>
#include <string>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

class VideoEngine {
public:
    VideoEngine();
    ~VideoEngine();

    // Open video, initialize FFmpeg contexts, detect HDR, and create the texture
    bool open_video(SDL_Renderer* renderer, const std::string& filepath);
    void close_video();

    // Start / stop decoding playback clocks
    void start();
    void stop();

    // Synchronize frame rates and upload decoded frames to the GPU texture
    void update_frame(SDL_Renderer* renderer, float dt);

    SDL_Texture* get_texture() const { return texture_; }
    bool is_hdr() const { return is_hdr_; }
    bool has_rendered_first_frame() const { return first_frame_rendered_; }

    int get_width() const { return width_; }
    int get_height() const { return height_; }

private:
    std::string filepath_;
    SDL_Texture* texture_ = nullptr;
    
    // FFmpeg demux/decode contexts
    AVFormatContext* format_ctx_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    int video_stream_index_ = -1;
    
    int width_ = 0;
    int height_ = 0;
    double time_base_ = 0.0;
    bool is_hdr_ = false;

    // Asynchronous decoding thread
    std::thread decode_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> eof_{false};
    std::atomic<bool> first_frame_rendered_{false};

    // Thread-safe decoded frame queue
    std::mutex queue_mutex_;
    std::queue<AVFrame*> frame_queue_;
    const size_t max_queue_size_ = 10;

    // Presentation clocks
    double playback_clock_ = 0.0;
    double last_frame_pts_ = -1.0;

    void decode_loop();
    void clear_queue();
};
