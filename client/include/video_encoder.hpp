#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "desktop_capture.hpp"

// Live H.264 encoder backed by OpenH264 (self-contained, no Media
// Foundation dependency). Encodes BGRA desktop frames to Annex-B.
class VideoEncoder {
public:
    VideoEncoder() = default;
    ~VideoEncoder();

    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    bool initialize(const EncoderConfig& config);
    void shutdown();

    bool encode_frame(const uint8_t* bgra_data, int width, int height,
                      CapturedFrame* frame_out);
    void force_keyframe();
    bool is_initialized() const { return initialized_; }
    // Cumulative EncodeFrame skips since the last exchange (diagnostics).
    uint64_t exchange_skips() { return skips_.exchange(0); }

private:
    void bgra_to_i420(const uint8_t* bgra, int width, int height,
                      uint8_t* y, uint8_t* u, uint8_t* v);

    void* encoder_ = nullptr;  // ISVCEncoder*
    std::mutex mutex_;
    EncoderConfig config_;
    std::vector<uint8_t> y_plane_, u_plane_, v_plane_;
    bool initialized_ = false;
    bool keyframe_requested_ = false;
    std::atomic<uint64_t> skips_{0};
};
