#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "desktop_capture.hpp"

// Live H.264 encoder backed by OpenH264 (self-contained, no Media
// Foundation dependency). Consumes the I420 frame the capturer produced and
// emits Annex-B access units.
class VideoEncoder {
public:
    VideoEncoder() = default;
    ~VideoEncoder();

    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    bool initialize(const EncoderConfig& config);
    void shutdown();

    // Encodes frame.i420 into frame.h264_data. Reconfigures itself when the
    // desktop resolution (and therefore the frame size) changed.
    bool encode_frame(CapturedFrame& frame);
    void force_keyframe();
    bool is_initialized() const { return initialized_; }
    // Cumulative EncodeFrame skips since the last exchange (diagnostics).
    uint64_t exchange_skips() { return skips_.exchange(0); }

private:
    bool initialize_locked();

    void* encoder_ = nullptr;  // ISVCEncoder*
    std::mutex mutex_;
    EncoderConfig config_;
    bool initialized_ = false;
    bool keyframe_requested_ = false;
    std::atomic<uint64_t> skips_{0};
};
