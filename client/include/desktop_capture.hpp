#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <mutex>
#include <vector>

// OpenH264 refuses (and the box filter cannot use) anything below this; both
// the capturer and the encoder must agree on the floor.
inline constexpr int kMinEncodeDimension = 32;

// Capture/encoding quality parameters.
struct EncoderConfig {
    int fps = 30;
    int bitrate_kbps = 2048;
    int quality_level = 70;
    int width = 1920;
    int height = 1080;
    // Long edge of the encoded picture; wider desktops are downscaled to it.
    // 0 keeps the native resolution.
    int max_encode_width = 1920;
    // Pixel format of the GPU texture handed to a hardware encoder:
    // 0 = no GPU path (soft encode), 1 = NV12, 2 = BGRA.
    int gpu_input_format = 0;
};

struct CapturedFrame {
    uint64_t timestamp_us = 0;
    int width = 0;   // encoded size (after the optional downscale)
    int height = 0;
    int source_width = 0;   // native desktop size, kept for input mapping
    int source_height = 0;
    std::vector<uint8_t> i420;     // Y + U + V, strides width / width/2
    std::vector<uint8_t> h264_data;  // encoded output (post-encode)
    bool is_keyframe = false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> d3d11_texture;  // GPU texture (for hardware encoder path)
};

// Desktop capture via Desktop Duplication API (DXGI 1.2+),
// with automatic BitBlt fallback for unsupported environments.
// BGRA is scaled and turned into I420 while the staging texture is still
// mapped, so no full-resolution copy is ever taken.
class DesktopCapturer {
public:
    DesktopCapturer() = default;
    ~DesktopCapturer();

    DesktopCapturer(const DesktopCapturer&) = delete;
    DesktopCapturer& operator=(const DesktopCapturer&) = delete;

    bool initialize(int monitor_index = 0);
    // prefer_gpu_path: when true, capture produces a GPU texture for a
    // hardware encoder (zero CPU conversion); when false the classic
    // staged BGRA->I420 path feeds the software encoder.
    void configure(const EncoderConfig& config, bool prefer_gpu_path = false);

    // Returns true when a new frame was captured; false when unchanged.
    bool capture_frame(CapturedFrame& frame, DWORD wait_ms = 100);

    bool using_bitblt_fallback() const { return use_bitblt_; }

    // Pixel format of the GPU texture that capture_frame fills, or 0 when the
    // GPU path is unavailable: 1 = NV12 (VideoProcessor), 2 = BGRA (native
    // size passthrough).
    int gpu_texture_format() const;

    // The desktop underneath us changed (session switch, secure-desktop hop,
    // monitor reset): throw away the cached geometry and rebuild now instead
    // of waiting for the next capture to fail.
    void on_desktop_switched();

    // Encode size for the current configuration (even, 0 before configure()).
    void encode_size(int* width, int* height) const;

private:
    bool init_d3d();
    bool init_duplication(int monitor_index);
    // Rebuild device + duplication after a session switch or GPU loss, at most
    // once every few seconds. Returns true when duplication works again.
    bool try_recover_dxgi();
    bool ensure_staging_texture(int width, int height);
    bool capture_from_duplication(CapturedFrame& frame, DWORD wait_ms);
    bool capture_with_bitblt(CapturedFrame& frame);
    void prepare_resampling(int src_width, int src_height, int dst_width,
                            int dst_height);
    // Recompute encode_width_/encode_height_ from source_size + the cap.
    // Caller must already hold mutex_ (it is reached from initialize()).
    void apply_encode_size();
    // GPU path: VideoProcessor converts/scales the duplicated BGRA texture to
    // an NV12 texture the encoder can consume without leaving the GPU.
    bool init_video_processor();
    void teardown_video_processor();
    bool gpu_convert_frame(ID3D11Texture2D* source, CapturedFrame& frame);
    // Box-filter the mapped BGRA straight into the frame's I420 buffer,
    // splitting the destination rows over a few threads.
    void convert_to_i420(const uint8_t* src, int src_pitch,
                         CapturedFrame& frame) const;
    void convert_rows(const uint8_t* src, int src_pitch, CapturedFrame& frame,
                      int dy0, int dy1) const;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    // GPU conversion path (prefer_gpu_path_ == true).
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> vp_enumerator_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> video_processor_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> vp_output_view_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12_texture_;
    int vp_width_ = 0;
    int vp_height_ = 0;
    bool prefer_gpu_path_ = false;

    EncoderConfig config_;
    mutable std::mutex mutex_;
    bool use_bitblt_ = false;
    bool logged_no_desktop_ = false;
    bool logged_duplication_denied_ = false;
    ULONGLONG next_dxgi_retry_ms_ = 0;
    int staging_width_ = 0;
    int staging_height_ = 0;
    int source_width_ = 0;
    int source_height_ = 0;
    int encode_width_ = 0;
    int encode_height_ = 0;
    // Per-destination-column/row source windows of the box filter plus
    // 4096/tap-count reciprocals (integer averaging without division).
    std::vector<int32_t> x0_, x1_, xinv_, y0_, y1_, yinv_;
};
