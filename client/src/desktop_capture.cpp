#include "desktop_capture.hpp"
#include <mfapi.h>
#include <dwmapi.h>
#include <mutex>
#pragma comment(lib, "dwmapi.lib")

DesktopCapturer::DesktopCapturer() {}

DesktopCapturer::~DesktopCapturer() {
    if (duplication_) {
        duplication_->Release();
        duplication_ = nullptr;
    }
}

bool DesktopCapturer::initialize(int monitor_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Step 1: Initialize D3D11 device
    if (!init_d3d_device()) {
        std::cerr << "Failed to initialize D3D11 device" << std::endl;
        return false;
    }
    
    // Step 2: Get display output
    if (!init_output(monitor_index)) {
        std::cerr << "Failed to get display output" << std::endl;
        d3d_device_.Reset();
        return false;
    }
    
    // Step 3: Create duplicate interface
    HRESULT hr = output_.as(&dxgi_device_);
    if (FAILED(hr)) {
        std::cerr << "Failed to get DXGIDevice: 0x" << std::hex << hr << std::dec << std::endl;
        output_.Reset();
        d3d_device_.Reset();
        return false;
    }
    
    // Get output from device
    IDXGIOutput* dxgi_output = nullptr;
    dxgi_device_->GetAdapter(0)->EnumOutputs(0, &dxgi_output);
    
    hr = dxgi_output->DuplicateOutput1(d3d_device_.Get(), 0, 
                                       IID_PPV_ARGS(&duplication_));
    dxgi_output->Release();
    
    if (FAILED(hr)) {
        std::cerr << "DXGI duplication failed: 0x" << std::hex << hr << std::dec << std::endl;
        
        // Check for remote desktop or fullscreen game detection
        if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
            std::cerr << "Warning: Desktop Duplication not available in this session" << std::endl;
            std::cerr << "Falling back to BitBlt method (lower performance)" << std::endl;
            return capture_with_bitblt(frame_buffer_);
        }
        
        return false;
    }
    
    // Set default configuration
    config_.width = GetSystemMetrics(SM_CXSCREEN);
    config_.height = GetSystemMetrics(SM_CYSCREEN);
    
    std::cout << "DesktopCapturer initialized successfully" << std::endl;
    return true;
}

bool DesktopCapturer::init_d3d_device() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    
    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_1
    };
    
    HRESULT hr = D3D11CreateDevice(
        nullptr,                              // Default adapter
        D3D_DRIVER_TYPE_HARDWARE,             // Hardware accelerated
        nullptr,                              // No software renderer
        flags,                                // Device flags
        feature_levels,                       // Feature levels
        ARRAYSIZE(feature_levels),
        D3D11_SDK_VERSION,
        &d3d_device_,                         // Output device
        nullptr,                              // Feature level out
        &immediate_ctx_                       // Context output
    );
    
    if (FAILED(hr)) {
        std::cerr << "D3D11CreateDevice failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }
    
    // Query for IDXGIDevice for later use
    hr = d3d_device_.as(&dxgi_device_);
    if (FAILED(hr)) {
        std::cerr << "Could not query IDXGIDevice: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }
    
    return true;
}

bool DesktopCapturer::init_output(int monitor_index) {
    IDXGIDevice* dxgi_device = nullptr;
    d3d_device_.as(&dxgi_device);
    
    IDXGIAdapter* adapter = nullptr;
    HRESULT hr = dxgi_device->GetAdapter(&adapter);
    dxgi_device->Release();
    
    if (FAILED(hr)) {
        std::cerr << "Could not get adapter: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }
    
    // Enumerate outputs (monitors)
    IDXGIOutput* output = nullptr;
    hr = adapter->EnumOutputs(monitor_index, &output);
    adapter->Release();
    
    if (FAILED(hr)) {
        std::cerr << "Could not enumerate output " << monitor_index << ": 0x" 
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }
    
    // Release old output if exists
    output_.Reset();
    
    // Query for output1 (required for Desktop Duplication API)
    hr = output->QueryInterface(IID_PPV_ARGS(&output_));
    output->Release();
    
    if (FAILED(hr)) {
        std::cerr << "Could not query IDXGIOutput1: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }
    
    return true;
}

void DesktopCapturer::configure(const EncoderConfig& cfg) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = cfg;
}

bool DesktopCapturer::capture_frame(CapturedFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!duplication_) {
        return capture_with_bitblt(frame);
    }
    
    // Acquire next frame from desktop duplication
    DWORD timeout = INFINITE;
    HRESULT hr = duplication_->AcquireNextFrame(timeout, &buffer_info_, &frame_resource_);
    
    if (hr == WAIT_TIMEOUT) {
        // Frame hasn't changed since last capture - skip this frame
        return false;
    } else if (FAILED(hr)) {
        std::cerr << "AcquireNextFrame failed: 0x" << std::hex << hr << std::dec << std::endl;
        
        // Try to re-acquire duplication interface
        duplication_->ReleaseFrame();
        hr = duplication_->AcquireNextFrame(100, &buffer_info_, &frame_resource_);
        if (SUCCEEDED(hr)) {
            return process_frame_data(frame, hr);
        }
        return false;
    }
    
    // Get the texture data
    D3D11_MAPPED_RESOURCE_DESC res_desc;
    hr = immediate_ctx_->Map(frame_resource_.Get(), 0, D3D11_MAP_READ, 0, &res_desc);
    
    if (FAILED(hr)) {
        std::cerr << "Map resource failed: 0x" << std::hex << hr << std::dec << std::endl;
        duplication_->ReleaseFrame();
        return false;
    }
    
    // Read pixel data (BGRA format)
    const uint8_t* pixels = static_cast<const uint8_t*>(res_desc.pData);
    int stride = res_desc.RowSize / 4;  // BGRA = 4 bytes per pixel
    
    // Copy to capture frame buffer
    frame.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
    frame.width = config_.width;
    frame.height = config_.height;
    
    // Store raw pixel data temporarily (will be encoded by VideoEncoder)
    frame.raw_bgra.assign(pixels, pixels + frame.height * stride * 4);
    
    // Unmap and release
    immediate_ctx_->Unmap(frame_resource_.Get(), 0);
    duplication_->ReleaseFrame();
    
    return true;
}

bool DesktopCapturer::process_frame_data(CapturedFrame& frame, HRESULT hr_dup) {
    if (!previous_frame_) {
        // First frame - create previous frame texture
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = config_.width;
        desc.Height = config_.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        
        D3D11_SUBRESOURCE_DATA initData{};
        initData.pSysMem = frame.raw_bgra.data();
        initData.SysStride = config_.width * 4;
        
        HRESULT hr = d3d_device_->CreateTexture2D(&desc, &initData, &previous_frame_);
        if (FAILED(hr)) {
            std::cerr << "Failed to create previous frame texture: 0x" 
                      << std::hex << hr << std::dec << std::endl;
            return false;
        }
        
        // Send keyframe (first frame)
        frame.is_keyframe = true;
        return true;
    }
    
    // Compare frames to check if anything changed
    immediate_ctx_->CopyResource(previous_frame_.Get(), frame_resource_.Get());
    
    frame.is_keyframe = false;  // Not a keyframe if we reached here
    
    return true;
}

bool DesktopCapturer::capture_with_bitblt(CapturedFrame& frame) {
    HDC hdc_screen = GetDC(nullptr);
    HDC hdc_mem = CreateCompatibleDC(hdc_screen);
    
    BITMAPINFOHEADER bih{sizeof(BITMAPINFOHEADER), config_.width, -config_.height};
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    bih.biPlanes = 1;
    
    void* pixels = nullptr;
    HBITMAP hbmp = CreateDIBSection(hdc_mem, reinterpret_cast<BITMAPINFO*>(&bih), 
                                    DIB_RGB_COLORS, &pixels, nullptr, 0);
    
    if (!hbmp || !pixels) {
        std::cerr << "CreateDIBSection failed" << std::endl;
        DeleteDC(hdc_mem);
        ReleaseDC(nullptr, hdc_screen);
        return false;
    }
    
    SelectObject(hdc_mem, hbmp);
    BitBlt(hdc_mem, 0, 0, config_.width, config_.height, hdc_screen, 0, 0, SRCCOPY);
    
    // Copy RGBA data to frame
    frame.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
    frame.width = config_.width;
    frame.height = config_.height;
    
    const uint8_t* rgba_data = static_cast<const uint8_t*>(pixels);
    frame.raw_bgra.assign(rgba_data, rgba_data + config_.height * config_.width * 4);
    
    frame.is_keyframe = true;  // BitBlt always captures full frame
    
    DeleteDC(hdc_mem);
    ReleaseDC(nullptr, hdc_screen);
    DeleteObject(hbmp);
    
    return true;
}

bool DesktopCapturer::is_hw_encoding_available() {
    HRESULT hr = MFStartup(MF_VERSION);
    bool available = SUCCEEDED(hr);
    
    if (available) {
        MFCleanup();
    }
    
    return available;
}
