#pragma once

#include <vector>
#include <cstdint>
#include <d3d11.h>
#include <dxgi1_2.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// Configuration for desktop capture and encoding
struct EncoderConfig {
    int fps = 30;           // Target frames per second (15-60)
    int bitrate_kbps = 2048; // Target bitrate in kbps
    int quality_level = 70;  // Quality level 1-100
    int width = 1920;        // Capture width (default Full HD)
    int height = 1080;       // Capture height
    
    // Encode preset: ULTRA_LOW_LATENCY, LOW_LATENCY, REALTIME, QUALITY
    const wchar_t* preset = L"RealTime";
};

// Frame metadata
struct CapturedFrame {
    uint64_t timestamp_us;     // Capture timestamp in microseconds
    int width;
    int height;
    std::vector<uint8_t> h264_data;  // Encoded H.264 data
    bool is_keyframe;          // Whether this is an I-frame
};

// Desktop capturer using Desktop Duplication API (DXGI 1.2+)
class DesktopCapturer {
public:
    DesktopCapturer();
    ~DesktopCapturer();
    
    // Initialize DXGI device and duplication interface
    bool initialize(int monitor_index = 0);
    
    // Set capture configuration
    void configure(const EncoderConfig& config);
    
    // Capture one frame and encode to H.264
    // Returns true if successful, false if error or frame too similar
    bool capture_frame(CapturedFrame& frame);
    
    // Check if hardware encoding is available
    static bool is_hw_encoding_available();
    
private:
    Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediate_ctx_;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device_;
    Microsoft::WRL::ComPtr<IDXGIOutput1> output_;
    Microsoft::WRL::ComPtr<IDirectXGIOutputDuplication> duplication_;
    
    EncoderConfig config_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> previous_frame_;
    
    // Initialize DirectX 11 device
    bool init_d3d_device();
    
    // Get primary display output
    bool init_output(int monitor_index);
    
    // Create resource for duplicate frame
    void create_duplicate_resources();
    
    // Fallback to BitBlt if DXGI fails (Win7 compatibility)
    bool capture_with_bitblt(CapturedFrame& frame);
};
