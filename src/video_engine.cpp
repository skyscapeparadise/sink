#include "video_engine.hpp"
#include <iostream>
#include <chrono>

extern "C" {
#include <libavutil/imgutils.h>
}

// Callback invoked by the FFmpeg codec context to select the hardware pixel format
static enum AVPixelFormat get_hw_format(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {
    for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_VIDEOTOOLBOX) {
            return *p;
        }
    }
    // Fall back to standard software format if VideoToolbox is not in the list
    return pix_fmts[0];
}

VideoEngine::VideoEngine() {}

VideoEngine::~VideoEngine() {
    close_video();
}

bool VideoEngine::open_video(SDL_Renderer* renderer, const std::string& filepath) {
    filepath_ = filepath;

    if (avformat_open_input(&format_ctx_, filepath.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "Failed to open video file: " << filepath << std::endl;
        return false;
    }

    if (avformat_find_stream_info(format_ctx_, nullptr) < 0) {
        std::cerr << "Failed to find stream info for video: " << filepath << std::endl;
        close_video();
        return false;
    }

    // Find the primary video stream
    for (unsigned int i = 0; i < format_ctx_->nb_streams; i++) {
        if (format_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index_ = i;
            break;
        }
    }

    if (video_stream_index_ == -1) {
        std::cerr << "No video stream found in file: " << filepath << std::endl;
        close_video();
        return false;
    }

    AVCodecParameters* codec_params = format_ctx_->streams[video_stream_index_]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codec_params->codec_id);
    if (!codec) {
        std::cerr << "Suitable decoder not found for video codec." << std::endl;
        close_video();
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        std::cerr << "Failed to allocate codec context." << std::endl;
        close_video();
        return false;
    }

    if (avcodec_parameters_to_context(codec_ctx_, codec_params) < 0) {
        std::cerr << "Failed to copy codec parameters to context." << std::endl;
        close_video();
        return false;
    }

    // Only attempt hardware acceleration if dimensions are aligned to standard 8-pixel macroblock boundaries.
    // This avoids VideoToolbox failures on custom dimensions (e.g. 2400x1676).
    bool dims_aligned = (codec_params->width % 8 == 0) && (codec_params->height % 8 == 0);
    bool is_hw_supported_codec = (codec_params->codec_id == AV_CODEC_ID_H264 || 
                                  codec_params->codec_id == AV_CODEC_ID_HEVC || 
                                  codec_params->codec_id == AV_CODEC_ID_PRORES);

    if (dims_aligned && is_hw_supported_codec) {
        AVBufferRef* hw_device_ctx = nullptr;
        int hw_ret = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0);
        if (hw_ret >= 0) {
            codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx);
            codec_ctx_->get_format = get_hw_format;
            av_buffer_unref(&hw_device_ctx);
            std::cout << "Video Background: VideoToolbox hardware accelerated decoding enabled (HEVC/H.264/ProRes)." << std::endl;
        } else {
            std::cout << "Video Background: VideoToolbox hardware device context creation failed. Using software decoding." << std::endl;
        }
    } else {
        std::cout << "Video Background: Non-standard dimensions or unsupported codec for hardware. Using software decoding." << std::endl;
    }

    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
        std::cerr << "Failed to open video codec." << std::endl;
        close_video();
        return false;
    }

    width_ = codec_ctx_->width;
    height_ = codec_ctx_->height;
    
    AVRational tb = format_ctx_->streams[video_stream_index_]->time_base;
    time_base_ = av_q2d(tb);

    // Detect HDR metadata:
    if (codec_ctx_->color_trc == AVCOL_TRC_SMPTE2084 || 
        codec_ctx_->color_trc == AVCOL_TRC_ARIB_STD_B67 ||
        codec_ctx_->color_primaries == AVCOL_PRI_BT2020) {
        is_hdr_ = true;
        std::cout << "Video Background: HDR metadata detected (BT.2020 / PQ / HLG)" << std::endl;
    } else {
        is_hdr_ = false;
        std::cout << "Video Background: SDR content detected (sRGB / BT.709)" << std::endl;
    }

    // Map FFmpeg video colorspace parameters to the correct SDL3 YUV colorspace
    SDL_Colorspace sdl_colorspace = SDL_COLORSPACE_BT709_LIMITED; // standard HD default
    
    if (codec_ctx_->colorspace == AVCOL_SPC_BT2020_NCL || 
        codec_ctx_->colorspace == AVCOL_SPC_BT2020_CL) {
        if (codec_ctx_->color_trc == AVCOL_TRC_ARIB_STD_B67) {
            // Construct a custom HLG colorspace using SDL3's bitfield definition macro
            sdl_colorspace = static_cast<SDL_Colorspace>(
                SDL_DEFINE_COLORSPACE(
                    SDL_COLOR_TYPE_YCBCR,
                    (codec_ctx_->color_range == AVCOL_RANGE_JPEG ? SDL_COLOR_RANGE_FULL : SDL_COLOR_RANGE_LIMITED),
                    SDL_COLOR_PRIMARIES_BT2020,
                    SDL_TRANSFER_CHARACTERISTICS_HLG,
                    SDL_MATRIX_COEFFICIENTS_BT2020_NCL,
                    SDL_CHROMA_LOCATION_LEFT
                )
            );
            std::cout << "Video Background: Custom HLG colorspace mapped successfully." << std::endl;
        } else {
            if (codec_ctx_->color_range == AVCOL_RANGE_JPEG) {
                sdl_colorspace = SDL_COLORSPACE_BT2020_FULL;
            } else {
                sdl_colorspace = SDL_COLORSPACE_BT2020_LIMITED;
            }
        }
    } else if (codec_ctx_->colorspace == AVCOL_SPC_BT470BG || 
               codec_ctx_->colorspace == AVCOL_SPC_SMPTE170M || 
               codec_ctx_->colorspace == AVCOL_SPC_SMPTE240M) {
        if (codec_ctx_->color_range == AVCOL_RANGE_JPEG) {
            sdl_colorspace = SDL_COLORSPACE_BT601_FULL;
        } else {
            sdl_colorspace = SDL_COLORSPACE_BT601_LIMITED;
        }
    } else {
        if (codec_ctx_->color_range == AVCOL_RANGE_JPEG) {
            sdl_colorspace = SDL_COLORSPACE_BT709_FULL;
        } else {
            sdl_colorspace = SDL_COLORSPACE_BT709_LIMITED;
        }
    }

    // Create SDL streaming texture using SDL Properties API to support color management
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STREAMING);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_IYUV);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, sdl_colorspace);
    
    if (is_hdr_) {
        // Calibrate diffuse white point to 250.0f to deepen shadow curves and contrast
        SDL_SetFloatProperty(props, SDL_PROP_TEXTURE_CREATE_SDR_WHITE_POINT_FLOAT, 250.0f);
        SDL_SetFloatProperty(props, SDL_PROP_TEXTURE_CREATE_HDR_HEADROOM_FLOAT, 4.0f);
    }
    
    texture_ = SDL_CreateTextureWithProperties(renderer, props);
    SDL_DestroyProperties(props);

    if (!texture_) {
        std::cerr << "Failed to create video background texture: " << SDL_GetError() << std::endl;
        close_video();
        return false;
    }

    std::cout << "Successfully opened video: " << filepath_ << " (" << width_ << "x" << height_ << ")" << std::endl;
    first_frame_rendered_ = false;
    return true;
}

void VideoEngine::close_video() {
    stop();
    first_frame_rendered_ = false;
    if (texture_) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
    if (format_ctx_) {
        avformat_close_input(&format_ctx_);
        format_ctx_ = nullptr;
    }
    video_stream_index_ = -1;
    width_ = 0;
    height_ = 0;
    is_hdr_ = false;
}

void VideoEngine::start() {
    if (running_) return;
    running_ = true;
    eof_ = false;
    playback_clock_ = 0.0;
    last_frame_pts_ = -1.0;
    decode_thread_ = std::thread(&VideoEngine::decode_loop, this);
}

void VideoEngine::stop() {
    if (!running_) return;
    running_ = false;
    if (decode_thread_.joinable()) {
        decode_thread_.join();
    }
    clear_queue();
}

void VideoEngine::update_frame(SDL_Renderer* renderer, float dt) {
    if (!running_) return;

    // Advance our playback clock based on frame delta time (capped at 0.1 seconds to prevent spikes)
    playback_clock_ += std::min(0.1, static_cast<double>(dt));
    double elapsed_sec = playback_clock_;

    AVFrame* frame_to_upload = nullptr;

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        
        while (!frame_queue_.empty()) {
            AVFrame* frame = frame_queue_.front();
            double frame_time = frame->pts * time_base_;

            // If the frame's PTS is due (or past due), mark it for display
            if (elapsed_sec >= frame_time) {
                if (frame_to_upload) {
                    av_frame_free(&frame_to_upload); // Drop late frame
                }
                frame_to_upload = frame;
                frame_queue_.pop();
                
                // If PTS jumps backward (e.g. video looped), adjust our playback clock
                if (frame_time < last_frame_pts_) {
                    playback_clock_ = frame_time;
                    elapsed_sec = frame_time;
                }
                last_frame_pts_ = frame_time;
            } else {
                // Next frame is in the future
                break;
            }
        }
    }

    if (frame_to_upload) {
        // Upload YUV planes directly to texture (GPU handles hardware YUV -> RGB conversion)
        SDL_UpdateYUVTexture(
            texture_,
            nullptr,
            frame_to_upload->data[0], frame_to_upload->linesize[0],
            frame_to_upload->data[1], frame_to_upload->linesize[1],
            frame_to_upload->data[2], frame_to_upload->linesize[2]
        );
        av_frame_free(&frame_to_upload);
        first_frame_rendered_ = true;
    }
}

void VideoEngine::clear_queue() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    while (!frame_queue_.empty()) {
        AVFrame* frame = frame_queue_.front();
        av_frame_free(&frame);
        frame_queue_.pop();
    }
}

void VideoEngine::decode_loop() {
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    SwsContext* sws_ctx = nullptr;

    while (running_) {
        // Cap frame queue size to prevent excessive memory usage
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (frame_queue_.size() >= max_queue_size_) {
                // Briefly yield lock to avoid spinning
                lock.~lock_guard();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
        }

        int ret = av_read_frame(format_ctx_, packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                // Loop: Seek back to frame 0
                avformat_seek_file(format_ctx_, video_stream_index_, 0, 0, 0, AVSEEK_FLAG_ANY);
                avcodec_flush_buffers(codec_ctx_);
                eof_ = true;
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (packet->stream_index == video_stream_index_) {
            ret = avcodec_send_packet(codec_ctx_, packet);
            if (ret < 0) {
                av_packet_unref(packet);
                continue;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(codec_ctx_, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    if (codec_ctx_->hw_device_ctx) {
                        std::cout << "Video Background: Hardware decoding failed at runtime. Falling back to software decoding..." << std::endl;
                        
                        // Deallocate hardware context
                        avcodec_free_context(&codec_ctx_);
                        
                        // Instantiate standard software decoder context
                        AVCodecParameters* sw_params = format_ctx_->streams[video_stream_index_]->codecpar;
                        const AVCodec* sw_codec = avcodec_find_decoder(sw_params->codec_id);
                        codec_ctx_ = avcodec_alloc_context3(sw_codec);
                        avcodec_parameters_to_context(codec_ctx_, sw_params);
                        
                        if (avcodec_open2(codec_ctx_, sw_codec, nullptr) < 0) {
                            std::cerr << "Video Background: Failed to open software fallback decoder." << std::endl;
                            running_ = false;
                        } else {
                            // Restart stream playback cleanly in software mode
                            avformat_seek_file(format_ctx_, video_stream_index_, 0, 0, 0, AVSEEK_FLAG_ANY);
                            avcodec_flush_buffers(codec_ctx_);
                        }
                    }
                    break;
                }

                AVFrame* queued_frame = av_frame_alloc();

                // If this is a hardware-accelerated frame, copy its YUV pixels back to CPU memory
                if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
                    AVFrame* hw_cpu_frame = av_frame_alloc();
                    
                    // Let av_hwframe_transfer_data allocate buffer matching the hardware's native format
                    int transfer_ret = av_hwframe_transfer_data(hw_cpu_frame, frame, 0);
                    if (transfer_ret >= 0) {
                        hw_cpu_frame->pts = frame->pts;
                        
                        if (hw_cpu_frame->format != AV_PIX_FMT_YUV420P) {
                            // Convert the native hardware format (e.g. NV12, P010, UYVY) to standard YUV420P
                            queued_frame->format = AV_PIX_FMT_YUV420P;
                            queued_frame->width = width_;
                            queued_frame->height = height_;
                            av_frame_get_buffer(queued_frame, 0);

                            if (!sws_ctx) {
                                sws_ctx = sws_getContext(
                                    width_, height_, (AVPixelFormat)hw_cpu_frame->format,
                                    width_, height_, AV_PIX_FMT_YUV420P,
                                    SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
                                );
                            }

                            sws_scale(
                                sws_ctx, hw_cpu_frame->data, hw_cpu_frame->linesize, 0, height_,
                                queued_frame->data, queued_frame->linesize
                            );

                            queued_frame->pts = hw_cpu_frame->pts;
                        } else {
                            // Direct copy if it already matches
                            av_frame_move_ref(queued_frame, hw_cpu_frame);
                        }
                    } else {
                        std::cerr << "Video Background: Failed to transfer hardware frame to CPU: " << transfer_ret << std::endl;
                        av_frame_free(&hw_cpu_frame);
                        av_frame_free(&queued_frame);
                        break;
                    }
                    av_frame_free(&hw_cpu_frame);
                } else if (codec_ctx_->pix_fmt != AV_PIX_FMT_YUV420P) {
                    // Frame format is not standard YUV420P. Set up scaling converter.
                    queued_frame->format = AV_PIX_FMT_YUV420P;
                    queued_frame->width = width_;
                    queued_frame->height = height_;
                    av_frame_get_buffer(queued_frame, 0); // Allocate ref-counted buffer

                    if (!sws_ctx) {
                        sws_ctx = sws_getContext(
                            width_, height_, codec_ctx_->pix_fmt,
                            width_, height_, AV_PIX_FMT_YUV420P,
                            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
                        );
                    }

                    sws_scale(
                        sws_ctx, frame->data, frame->linesize, 0, height_,
                        queued_frame->data, queued_frame->linesize
                    );

                    queued_frame->pts = frame->pts;
                } else {
                    // Fast zero-copy path for standard YUV420P inputs
                    av_frame_move_ref(queued_frame, frame);
                }

                // Push onto safe queue
                {
                    std::lock_guard<std::mutex> lock(queue_mutex_);
                    frame_queue_.push(queued_frame);
                }
            }
        }
        av_packet_unref(packet);
    }

    if (sws_ctx) {
        sws_freeContext(sws_ctx);
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
}
