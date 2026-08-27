#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <mutex>
#include <vector>

// Capture/encoding quality parameters.
struct EncoderConfig {
    int fps = 30;
    int bitrate_kbps = 2048;
    int quality_level = 70;
    int width = 1920;
    int height = 1080;
};

struct CapturedFrame {
    uint64_t timestamp_us = 0;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> raw_bgra;   // BGRA pixels (pre-encode)
    std::vector<uint8_t> h264_data;  // encoded output (post-encode)
    bool is_keyframe = false;
};

// Desktop capture via Desktop Duplication API (DXGI 1.2+),
// with automatic BitBlt fallback for unsupported environments.
class DesktopCapturer {
public:
    DesktopCapturer() = default;
    ~DesktopCapturer();

    DesktopCapturer(const DesktopCapturer&) = delete;
    DesktopCapturer& operator=(const DesktopCapturer&) = delete;

    bool initialize(int monitor_index = 0);
    void configure(const EncoderConfig& config);

    // Returns true when a new frame was captured; false when unchanged.
    bool capture_frame(CapturedFrame& frame, DWORD wait_ms = 100);

    bool using_bitblt_fallback() const { return use_bitblt_; }

private:
    bool init_d3d();
    bool init_duplication(int monitor_index);
    bool ensure_staging_texture(int width, int height);
    bool capture_from_duplication(CapturedFrame& frame, DWORD wait_ms);
    bool capture_with_bitblt(CapturedFrame& frame);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;

    EncoderConfig config_;
    std::mutex mutex_;
    bool use_bitblt_ = false;
    int staging_width_ = 0;
    int staging_height_ = 0;
};
