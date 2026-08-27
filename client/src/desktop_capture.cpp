#include "desktop_capture.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>

#include "log.hpp"

DesktopCapturer::~DesktopCapturer() = default;

bool DesktopCapturer::initialize(int monitor_index) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!init_d3d()) {
        mlog::warn("D3D11 init failed, falling back to BitBlt capture");
        use_bitblt_ = true;
        return true;
    }

    if (!init_duplication(monitor_index)) {
        mlog::warn("Desktop Duplication unavailable, falling back to BitBlt capture");
        use_bitblt_ = true;
        return true;
    }

    mlog::info("Desktop capture initialized (Desktop Duplication API)");
    return true;
}

bool DesktopCapturer::init_d3d() {
    D3D_FEATURE_LEVEL feature_levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                   feature_levels, ARRAYSIZE(feature_levels),
                                   D3D11_SDK_VERSION, &device_, nullptr, &context_);
    if (FAILED(hr)) {
        mlog::error("D3D11CreateDevice failed");
        return false;
    }
    return true;
}

bool DesktopCapturer::init_duplication(int monitor_index) {
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(device_.As(&dxgi_device))) {
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_device->GetAdapter(&adapter))) {
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    HRESULT hr = adapter->EnumOutputs(monitor_index, &output);
    if (FAILED(hr)) {
        mlog::error("EnumOutputs failed for monitor " + std::to_string(monitor_index));
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
    if (FAILED(output.As(&output1))) {
        mlog::error("IDXGIOutput1 not supported (requires Windows 8+)");
        return false;
    }

    hr = output1->DuplicateOutput(device_.Get(), &duplication_);
    if (FAILED(hr)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "DuplicateOutput failed: 0x%08lX",
                 static_cast<unsigned long>(hr));
        mlog::error(msg);
        return false;
    }

    DXGI_OUTDUPL_DESC desc{};
    duplication_->GetDesc(&desc);
    config_.width = static_cast<int>(desc.ModeDesc.Width);
    config_.height = static_cast<int>(desc.ModeDesc.Height);
    return true;
}

void DesktopCapturer::configure(const EncoderConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Resolution comes from the duplication/primary screen; adopt only
    // fps/bitrate/quality from the caller.
    config_.fps = config.fps;
    config_.bitrate_kbps = config.bitrate_kbps;
    config_.quality_level = config.quality_level;
}

bool DesktopCapturer::ensure_staging_texture(int width, int height) {
    if (staging_ && staging_width_ == width && staging_height_ == height) {
        return true;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    staging_.Reset();
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &staging_);
    if (FAILED(hr)) {
        mlog::error("Failed to create staging texture");
        return false;
    }
    staging_width_ = width;
    staging_height_ = height;
    return true;
}

bool DesktopCapturer::capture_frame(CapturedFrame& frame, DWORD wait_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (use_bitblt_ || !duplication_) {
        return capture_with_bitblt(frame);
    }
    return capture_from_duplication(frame, wait_ms);
}

bool DesktopCapturer::capture_from_duplication(CapturedFrame& frame, DWORD wait_ms) {
    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    Microsoft::WRL::ComPtr<IDXGIResource> frame_resource;

    HRESULT hr = duplication_->AcquireNextFrame(wait_ms, &frame_info, &frame_resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false;  // desktop unchanged
    }
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            mlog::warn("Duplication access lost, re-initializing");
            duplication_.Reset();
            if (init_duplication(0)) {
                return false;
            }
            use_bitblt_ = true;
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg), "AcquireNextFrame failed: 0x%08lX",
                     static_cast<unsigned long>(hr));
            mlog::error(msg);
        }
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> frame_texture;
    hr = frame_resource.As(&frame_texture);
    if (SUCCEEDED(hr)) {
        D3D11_TEXTURE2D_DESC desc{};
        frame_texture->GetDesc(&desc);

        if (ensure_staging_texture(static_cast<int>(desc.Width),
                                   static_cast<int>(desc.Height))) {
            context_->CopyResource(staging_.Get(), frame_texture.Get());

            D3D11_MAPPED_SUBRESOURCE mapped{};
            hr = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
            if (SUCCEEDED(hr)) {
                frame.width = static_cast<int>(desc.Width);
                frame.height = static_cast<int>(desc.Height);
                frame.is_keyframe = frame_info.AccumulatedFrames > 0;
                frame.timestamp_us = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());

                const size_t row_bytes = static_cast<size_t>(desc.Width) * 4;
                frame.raw_bgra.resize(row_bytes * desc.Height);
                const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
                for (UINT row = 0; row < desc.Height; ++row) {
                    std::memcpy(frame.raw_bgra.data() + row * row_bytes,
                                src + static_cast<size_t>(row) * mapped.RowPitch,
                                row_bytes);
                }
                context_->Unmap(staging_.Get(), 0);
                duplication_->ReleaseFrame();
                return true;
            }
        }
    }

    duplication_->ReleaseFrame();
    return false;
}

bool DesktopCapturer::capture_with_bitblt(CapturedFrame& frame) {
    if (config_.width <= 0 || config_.height <= 0) {
        config_.width = GetSystemMetrics(SM_CXSCREEN);
        config_.height = GetSystemMetrics(SM_CYSCREEN);
    }

    HDC hdc_screen = GetDC(nullptr);
    HDC hdc_mem = CreateCompatibleDC(hdc_screen);

    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = config_.width;
    bih.biHeight = -config_.height;  // top-down
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(hdc_mem, reinterpret_cast<BITMAPINFO*>(&bih),
                                      DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap || !pixels) {
        DeleteDC(hdc_mem);
        ReleaseDC(nullptr, hdc_screen);
        mlog::error("CreateDIBSection failed");
        return false;
    }

    HGDIOBJ old = SelectObject(hdc_mem, bitmap);
    BitBlt(hdc_mem, 0, 0, config_.width, config_.height, hdc_screen, 0, 0, SRCCOPY);

    frame.timestamp_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    frame.width = config_.width;
    frame.height = config_.height;
    frame.is_keyframe = true;
    frame.raw_bgra.assign(static_cast<uint8_t*>(pixels),
                          static_cast<uint8_t*>(pixels) +
                              static_cast<size_t>(config_.width) * config_.height * 4);

    SelectObject(hdc_mem, old);
    DeleteObject(bitmap);
    DeleteDC(hdc_mem);
    ReleaseDC(nullptr, hdc_screen);
    return true;
}
