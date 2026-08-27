#pragma once

#include <cstdint>

#include "desktop_capture.hpp"

// H.264 encoder backed by Media Foundation (implemented in M4).
class VideoEncoder {
public:
    VideoEncoder() = default;
    ~VideoEncoder();

    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    bool initialize(const EncoderConfig& config);
    void shutdown();

    // Encode one BGRA frame; on success fills frame->h264_data.
    bool encode_frame(const uint8_t* bgra_data, int width, int height,
                      CapturedFrame* frame_out);

    void force_keyframe();

    bool is_initialized() const { return initialized_; }

private:
    EncoderConfig config_;
    bool initialized_ = false;
    bool keyframe_requested_ = false;
};
