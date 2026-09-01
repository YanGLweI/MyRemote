#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "desktop_capture.hpp"

// ---------------------------------------------------------------------------
// Encoder backend identifiers and the abstract encoder interface.
//
// The agent picks one backend per session:
//   HARDWARE_HEVC_MF   Media Foundation HEVC hardware encoder (preferred)
//   HARDWARE_H264_MF   Media Foundation H.264 hardware encoder
//   SOFTWARE_OPENH264  OpenH264 software encoder (always available)
// A backend that fails at runtime hot-swaps down this list without dropping
// the session.
// ---------------------------------------------------------------------------
enum class EncoderBackend {
    SOFTWARE_OPENH264 = 0,
    HARDWARE_H264_MF = 1,
    HARDWARE_HEVC_MF = 2,
};

// What this machine's encoders can do, reported to the control centre so the
// far side can pick a decoder before the first frame arrives.
struct CodecInfo {
    uint16_t supported_codecs = 0;     // proto::kCodecMask* bitmask
    int hardware_hevc_available = 0;   // 1 = yes
    int hardware_h264_available = 0;   // 1 = yes
};

class IStreamEncoder {
public:
    virtual ~IStreamEncoder() = default;

    virtual bool initialize(const EncoderConfig& config) = 0;
    virtual bool encode_frame(CapturedFrame& frame) = 0;
    virtual void force_keyframe() = 0;
    virtual bool is_initialized() const = 0;
    // Cumulative encode skips since the last exchange (diagnostics).
    virtual uint64_t exchange_skips() = 0;
    virtual EncoderBackend backend() const = 0;
    // Short human-readable name for logs and the session toolbar badge.
    virtual std::string backend_name() const = 0;
};

// Live H.264 software encoder backed by OpenH264 (self-contained, no Media
// Foundation dependency). Consumes the I420 frame the capturer produced and
// emits Annex-B access units.
class VideoEncoder : public IStreamEncoder {
public:
    VideoEncoder() = default;
    ~VideoEncoder() override;

    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    bool initialize(const EncoderConfig& config) override;
    void shutdown();
    // Encodes frame.i420 into frame.h264_data. Reconfigures itself when the
    // desktop resolution (and therefore the frame size) changed.
    bool encode_frame(CapturedFrame& frame) override;
    void force_keyframe() override;
    bool is_initialized() const override { return initialized_; }
    uint64_t exchange_skips() override { return skips_.exchange(0); }
    EncoderBackend backend() const override {
        return EncoderBackend::SOFTWARE_OPENH264;
    }
    std::string backend_name() const override { return "H.264 软编 (OpenH264)"; }

private:
    bool initialize_locked();

    void* encoder_ = nullptr;  // ISVCEncoder*
    std::mutex mutex_;
    EncoderConfig config_;
    bool initialized_ = false;
    bool keyframe_requested_ = false;
    std::atomic<uint64_t> skips_{0};
};

// Picks the best encoder this machine can run and builds it.
class EncoderFactory {
public:
    static CodecInfo probe_capabilities();
    // HEVC hard -> H.264 hard -> OpenH264, first one that initialises wins.
    static std::unique_ptr<IStreamEncoder> create_selected(const EncoderConfig& config);
    static std::unique_ptr<IStreamEncoder> create_backend(EncoderBackend backend,
                                                         const EncoderConfig& config);
};
