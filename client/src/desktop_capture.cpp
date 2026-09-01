#include "desktop_capture.hpp"

#include <dxgicommon.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

#include "log.hpp"
#include "messages.hpp"

DesktopCapturer::~DesktopCapturer() = default;

bool DesktopCapturer::initialize(int monitor_index) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!init_d3d()) {
        mlog::warn("D3D11 init failed, falling back to BitBlt capture");
        use_bitblt_ = true;
    } else if (!init_duplication(monitor_index)) {
        mlog::warn("Desktop Duplication unavailable, falling back to BitBlt capture");
        use_bitblt_ = true;
    } else {
        mlog::info("Desktop capture initialized (Desktop Duplication API)");
    }

    if (use_bitblt_) {
        source_width_ = GetSystemMetrics(SM_CXSCREEN);
        source_height_ = GetSystemMetrics(SM_CYSCREEN);
        config_.width = source_width_;
        config_.height = source_height_;
    }
    return source_width_ > 0 && source_height_ > 0;
}

bool DesktopCapturer::init_d3d() {
    device_.Reset();
    context_.Reset();
    D3D_FEATURE_LEVEL feature_levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    // The VideoProcessor path (GPU BGRA->NV12 for the hardware encoder) needs
    // the device created with video support; without it the CPU I420 path is
    // still there, so a retry without the flag is the graceful fallback.
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                   feature_levels, ARRAYSIZE(feature_levels),
                                   D3D11_SDK_VERSION, &device_, nullptr, &context_);
    if (FAILED(hr)) {
        device_.Reset();
        context_.Reset();
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                               feature_levels, ARRAYSIZE(feature_levels),
                               D3D11_SDK_VERSION, &device_, nullptr, &context_);
    }
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
        char msg[80];
        snprintf(msg, sizeof(msg), "Desktop Duplication refused: 0x%08lX",
                 static_cast<unsigned long>(hr));
        // On the secure desktop this is refused outright and retrying does not
        // change it, so say it once per desktop instead of every 3s.
        if (!logged_duplication_denied_) {
            logged_duplication_denied_ = true;
            mlog::warn(msg);
        }
        return false;
    }
    logged_duplication_denied_ = false;

    DXGI_OUTDUPL_DESC desc{};
    duplication_->GetDesc(&desc);
    source_width_ = static_cast<int>(desc.ModeDesc.Width);
    source_height_ = static_cast<int>(desc.ModeDesc.Height);
    config_.width = source_width_;
    config_.height = source_height_;
    // The resampling tables are indexed by the *new* geometry from here on;
    // skipping this leaves try_recover_dxgi() reading out of bounds after a
    // session switch or a resolution change.
    apply_encode_size();
    return true;
}

void DesktopCapturer::configure(const EncoderConfig& config, bool prefer_gpu_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Resolution comes from the capture backend; adopt only the quality
    // parameters and the encode cap from the caller.
    config_.fps = config.fps;
    config_.bitrate_kbps = config.bitrate_kbps;
    config_.quality_level = config.quality_level;
    config_.max_encode_width = config.max_encode_width;
    prefer_gpu_path_ = prefer_gpu_path;
    if (!prefer_gpu_path_) {
        teardown_video_processor();
    }
    if (source_width_ > 0 && source_height_ > 0) {
        apply_encode_size();
        if (prefer_gpu_path_) {
            // Stand the VideoProcessor up right away so gpu_texture_format()
            // answers truthfully before the encoder is created.
            init_video_processor();
        }
    }
}

int DesktopCapturer::gpu_texture_format() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!prefer_gpu_path_ || !device_ || use_bitblt_) {
        return 0;
    }
    if (video_processor_ && nv12_texture_ && vp_width_ == encode_width_ &&
        vp_height_ == encode_height_ && encode_width_ > 0) {
        return 1;  // NV12 via VideoProcessor
    }
    return 0;
}

// ---------------------------------------------------------------------------
// GPU conversion path: duplicated BGRA texture -> NV12 texture, scaled to the
// encode size, via ID3D11VideoProcessor. The pixels never leave the GPU and
// the hardware encoder consumes the result directly.
// ---------------------------------------------------------------------------
void DesktopCapturer::teardown_video_processor() {
    vp_output_view_.Reset();
    nv12_texture_.Reset();
    video_processor_.Reset();
    vp_enumerator_.Reset();
    video_context_.Reset();
    video_device_.Reset();
    vp_width_ = 0;
    vp_height_ = 0;
}

bool DesktopCapturer::init_video_processor() {
    teardown_video_processor();
    if (!device_ || encode_width_ < kMinEncodeDimension ||
        encode_height_ < kMinEncodeDimension || source_width_ <= 0 ||
        source_height_ <= 0) {
        mlog::warn("VideoProcessor skipped: device=" +
                   std::to_string(device_ != nullptr) + " size=" +
                   std::to_string(encode_width_) + "x" +
                   std::to_string(encode_height_));
        return false;
    }
    if (FAILED(device_.As(&video_device_)) || FAILED(context_.As(&video_context_)) ||
        !video_device_ || !video_context_) {
        mlog::warn("VideoProcessor: D3D11 device lacks video support "
                   "(D3D11_CREATE_DEVICE_VIDEO_SUPPORT not granted)");
        teardown_video_processor();
        return false;
    }
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputWidth = static_cast<UINT>(source_width_);
    desc.InputHeight = static_cast<UINT>(source_height_);
    desc.OutputWidth = static_cast<UINT>(encode_width_);
    desc.OutputHeight = static_cast<UINT>(encode_height_);
    desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    if (FAILED(video_device_->CreateVideoProcessorEnumerator(&desc,
                                                             &vp_enumerator_)) ||
        FAILED(video_device_->CreateVideoProcessor(vp_enumerator_.Get(), 0,
                                                   &video_processor_))) {
        mlog::warn("VideoProcessor unavailable; hardware encoding will fall back");
        teardown_video_processor();
        return false;
    }

    D3D11_TEXTURE2D_DESC out{};
    out.Width = static_cast<UINT>(encode_width_);
    out.Height = static_cast<UINT>(encode_height_);
    out.MipLevels = 1;
    out.ArraySize = 1;
    out.Format = DXGI_FORMAT_NV12;
    out.SampleDesc.Count = 1;
    out.Usage = D3D11_USAGE_DEFAULT;
    out.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&out, nullptr, &nv12_texture_))) {
        teardown_video_processor();
        return false;
    }
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};
    ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ov.Texture2D.MipSlice = 0;
    if (FAILED(video_device_->CreateVideoProcessorOutputView(
            nv12_texture_.Get(), vp_enumerator_.Get(), &ov, &vp_output_view_))) {
        teardown_video_processor();
        return false;
    }
    vp_width_ = encode_width_;
    vp_height_ = encode_height_;
    mlog::info("VideoProcessor ready: " + std::to_string(source_width_) + "x" +
               std::to_string(source_height_) + " -> " +
               std::to_string(encode_width_) + "x" + std::to_string(encode_height_) +
               " NV12");
    return true;
}

bool DesktopCapturer::gpu_convert_frame(ID3D11Texture2D* source,
                                        CapturedFrame& frame) {
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> input_view;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
    iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    iv.Texture2D.MipSlice = 0;
    iv.Texture2D.ArraySlice = 0;
    if (FAILED(video_device_->CreateVideoProcessorInputView(
            source, vp_enumerator_.Get(), &iv, &input_view))) {
        return false;
    }

    // Desktop sRGB in, BT.601 limited-range NV12 out: what both the hardware
    // encoder and the Media Foundation decoder expect.
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_cs{};
    in_cs.Usage = 0;                       // playback
    in_cs.RGB_Range = 0;                   // full range
    in_cs.YCbCr_Matrix = 1;                // BT.709
    in_cs.YCbCr_xvYCC = 0;
    in_cs.Nominal_Range = 0;               // 0-255
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_cs{};
    out_cs.Usage = 0;
    out_cs.RGB_Range = 0;
    out_cs.YCbCr_Matrix = 0;               // BT.601
    out_cs.YCbCr_xvYCC = 0;
    out_cs.Nominal_Range = 1;              // 16-235 limited
    video_context_->VideoProcessorSetStreamColorSpace(video_processor_.Get(), 0,
                                                      &in_cs);
    video_context_->VideoProcessorSetOutputColorSpace(video_processor_.Get(),
                                                      &out_cs);
    RECT src_rect{0, 0, source_width_, source_height_};
    RECT dst_rect{0, 0, encode_width_, encode_height_};
    video_context_->VideoProcessorSetStreamSourceRect(video_processor_.Get(), 0,
                                                      TRUE, &src_rect);
    video_context_->VideoProcessorSetStreamDestRect(video_processor_.Get(), 0,
                                                    TRUE, &dst_rect);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.OutputIndex = 0;
    stream.InputFrameOrField = 0;
    stream.pInputSurface = input_view.Get();
    if (FAILED(video_context_->VideoProcessorBlt(
            video_processor_.Get(), vp_output_view_.Get(), 0, 1, &stream))) {
        return false;
    }
    frame.d3d11_texture = nv12_texture_;
    return true;
}

void DesktopCapturer::apply_encode_size() {
    if (source_width_ < kMinEncodeDimension ||
        source_height_ < kMinEncodeDimension) {
        // No usable desktop yet (logon screen, detached session, headless):
        // report "nothing to encode" instead of feeding the encoder a 1x1
        // phantom, and make sure no stale table survives the empty state.
        encode_width_ = 0;
        encode_height_ = 0;
        prepare_resampling(0, 0, 0, 0);
        return;
    }
    int w = source_width_;
    int h = source_height_;
    int cap = config_.max_encode_width;
    if (cap > 0 && w > cap) {
        h = static_cast<int>((static_cast<double>(h) * cap + w / 2) / w);
        w = cap;
    }
    // I420 needs even dimensions; the encoder needs a minimum size.
    w = w < kMinEncodeDimension ? kMinEncodeDimension : (w & ~1);
    h = h < kMinEncodeDimension ? kMinEncodeDimension : (h & ~1);
    const int prev_w = encode_width_;
    const int prev_h = encode_height_;
    encode_width_ = w;
    encode_height_ = h;
    config_.width = w;
    config_.height = h;
    prepare_resampling(source_width_, source_height_, w, h);
    if ((w != prev_w || h != prev_h) && (w != source_width_ || h != source_height_)) {
        mlog::info("Encode size " + std::to_string(w) + "x" + std::to_string(h) +
                   " (desktop " + std::to_string(source_width_) + "x" +
                   std::to_string(source_height_) + ")");
    }
}

void DesktopCapturer::encode_size(int* width, int* height) const {
    std::lock_guard<std::mutex> lock(mutex_);
    *width = encode_width_;
    *height = encode_height_;
}

// Destination pixel d covers source pixels [floor(d*s/D), floor((d+1)*s/D)),
// always at least one tap. The reciprocal keeps the averaging division-free.
void DesktopCapturer::prepare_resampling(int src_width, int src_height,
                                         int dst_width, int dst_height) {
    if (src_width <= 0 || src_height <= 0 || dst_width <= 0 || dst_height <= 0) {
        x0_.clear();
        x1_.clear();
        xinv_.clear();
        y0_.clear();
        y1_.clear();
        yinv_.clear();
        return;
    }
    x0_.resize(dst_width);
    x1_.resize(dst_width);
    xinv_.resize(dst_width);
    for (int d = 0; d < dst_width; ++d) {
        int a = static_cast<int>(static_cast<int64_t>(d) * src_width / dst_width);
        int b = static_cast<int>(static_cast<int64_t>(d + 1) * src_width / dst_width);
        if (b <= a) b = a + 1;
        if (b > src_width) b = src_width;
        if (a > src_width - 1) a = src_width - 1;
        int taps = b - a;
        x0_[d] = a;
        x1_[d] = b;
        xinv_[d] = 4096 / taps;
    }
    y0_.resize(dst_height);
    y1_.resize(dst_height);
    yinv_.resize(dst_height);
    for (int d = 0; d < dst_height; ++d) {
        int a = static_cast<int>(static_cast<int64_t>(d) * src_height / dst_height);
        int b = static_cast<int>(static_cast<int64_t>(d + 1) * src_height / dst_height);
        if (b <= a) b = a + 1;
        if (b > src_height) b = src_height;
        if (a > src_height - 1) a = src_height - 1;
        int taps = b - a;
        y0_[d] = a;
        y1_[d] = b;
        yinv_[d] = 4096 / taps;
    }
}

// BT.601 limited-range coefficients, matching what the control server decodes.
// Future: Add SSE2/AVX2 SIMD optimizations for BGRA->I420 conversion
namespace {
inline uint8_t clamp8(int v) {
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}
}  // namespace

// Box-filter destination rows [dy0, dy1) of the mapped BGRA image straight into
// the frame's I420 planes. Bounds are always even, so a chroma row pair never
// straddles two workers and each worker owns its row scratch.
void DesktopCapturer::convert_rows(const uint8_t* src, int src_pitch,
                                   CapturedFrame& frame, int dy0, int dy1) const {
    const int dw = frame.width;
    const int dh = frame.height;
    const int cw = dw / 2;
    // The tables were built for source_width_/source_height_. If they ever
    // disagree with the frame being filled, drop the rows instead of reading
    // out of bounds: a wrong picture is recoverable, an access violation is a
    // dead agent.
    if (dw <= 0 || dh <= 0 || dy1 <= dy0 ||
        frame.i420.size() < static_cast<size_t>(dw) * dh * 3 / 2 ||
        x0_.size() < static_cast<size_t>(dw) ||
        y0_.size() < static_cast<size_t>(dh)) {
        return;
    }
    uint8_t* yp = frame.i420.data();
    uint8_t* up = yp + static_cast<size_t>(dw) * dh;
    uint8_t* vp = up + static_cast<size_t>(cw) * (dh / 2);

    thread_local std::vector<uint8_t> row_a, row_b;
    thread_local std::vector<const uint8_t*> rows;
    if (row_a.size() < static_cast<size_t>(dw) * 3) {
        row_a.resize(static_cast<size_t>(dw) * 3);
        row_b.resize(static_cast<size_t>(dw) * 3);
    }
    const int32_t* c0 = x0_.data();
    const int32_t* c1 = x1_.data();

    for (int dy = dy0; dy < dy1; ++dy) {
        int r0 = y0_[dy];
        if (r0 < 0) r0 = 0;
        if (r0 > source_height_ - 1) r0 = source_height_ - 1;
        int nr = y1_[dy] - r0;
        if (nr < 1) nr = 1;
        if (r0 + nr > source_height_) nr = source_height_ - r0;
        if (static_cast<int>(rows.size()) < nr) {
            rows.resize(nr);
        }
        const uint8_t* base = src + static_cast<size_t>(r0) * src_pitch;
        for (int k = 0; k < nr; ++k) {
            rows[k] = base + static_cast<size_t>(k) * src_pitch;
        }

        const int xr = yinv_[dy];
        uint8_t* rgb = (dy & 1) ? row_b.data() : row_a.data();
        uint8_t* yrow = yp + static_cast<size_t>(dy) * dw;
        for (int dx = 0; dx < dw; ++dx) {
            int b = 0, g = 0, r = 0;
            int begin = c0[dx];
            int end = c1[dx];
            if (begin < 0) begin = 0;
            if (begin > source_width_ - 1) begin = source_width_ - 1;
            if (end > source_width_) end = source_width_;
            for (int k = 0; k < nr; ++k) {
                const uint32_t* row = reinterpret_cast<const uint32_t*>(rows[k]);
                for (int sx = begin; sx < end; ++sx) {
                    const uint32_t p = row[sx];
                    b += p & 255;
                    g += (p >> 8) & 255;
                    r += (p >> 16) & 255;
                }
            }
            const int xc = xinv_[dx];
            b = (b * xc >> 12) * xr >> 12;
            g = (g * xc >> 12) * xr >> 12;
            r = (r * xc >> 12) * xr >> 12;
            uint8_t* px = rgb + static_cast<size_t>(dx) * 3;
            px[0] = static_cast<uint8_t>(b);
            px[1] = static_cast<uint8_t>(g);
            px[2] = static_cast<uint8_t>(r);
            yrow[dx] = clamp8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }

        if (dy & 1) {
            const uint8_t* a = row_a.data();
            const uint8_t* p = row_b.data();
            uint8_t* urow = up + static_cast<size_t>(dy / 2) * cw;
            uint8_t* vrow = vp + static_cast<size_t>(dy / 2) * cw;
            for (int cx = 0; cx < cw; ++cx) {
                const uint8_t* a0 = a + static_cast<size_t>(cx) * 6;
                const uint8_t* a1 = a0 + 3;
                const uint8_t* b0 = p + static_cast<size_t>(cx) * 6;
                const uint8_t* b1 = b0 + 3;
                int bb = (a0[0] + a1[0] + b0[0] + b1[0] + 2) >> 2;
                int gg = (a0[1] + a1[1] + b0[1] + b1[1] + 2) >> 2;
                int rr = (a0[2] + a1[2] + b0[2] + b1[2] + 2) >> 2;
                urow[cx] = clamp8(((-38 * rr - 74 * gg + 112 * bb + 128) >> 8) + 128);
                vrow[cx] = clamp8(((112 * rr - 94 * gg - 18 * bb + 128) >> 8) + 128);
            }
        }
    }
}

void DesktopCapturer::convert_to_i420(const uint8_t* src, int src_pitch,
                                      CapturedFrame& frame) const {
    const int dh = frame.height;
    static const int cpu_count =
        std::max(1u, std::thread::hardware_concurrency());
    int workers = std::min(4, cpu_count);
    while (workers > 1 && dh / workers < 192) {
        --workers;
    }
    if (workers <= 1) {
        convert_rows(src, src_pitch, frame, 0, dh);
        return;
    }
    const int rows_per = ((dh / workers) + 1) & ~1;
    std::vector<std::thread> threads;
    for (int begin = rows_per; begin < dh; begin += rows_per) {
        const int end = std::min(begin + rows_per, dh);
        threads.emplace_back([this, src, src_pitch, &frame, begin, end]() {
            convert_rows(src, src_pitch, frame, begin, end);
        });
    }
    convert_rows(src, src_pitch, frame, 0, std::min(rows_per, dh));
    for (auto& t : threads) {
        t.join();
    }
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

bool DesktopCapturer::try_recover_dxgi() {
    ULONGLONG now = GetTickCount64();
    if (now < next_dxgi_retry_ms_) {
        return false;
    }
    next_dxgi_retry_ms_ = now + 3000;

    // A session switch or GPU reset invalidates the device itself, so rebuild
    // the whole chain rather than just the duplication object.
    duplication_.Reset();
    staging_.Reset();
    teardown_video_processor();
    context_.Reset();
    device_.Reset();
    bool ok = init_d3d() && init_duplication(0);
    if (ok) {
        mlog::info(use_bitblt_ ? "Desktop Duplication recovered"
                              : "Desktop Duplication re-initialized");
        use_bitblt_ = false;
    } else if (!use_bitblt_) {
        mlog::warn("Desktop Duplication lost, using BitBlt until it returns");
        use_bitblt_ = true;
    }
    return ok;
}

void DesktopCapturer::on_desktop_switched() {
    std::lock_guard<std::mutex> lock(mutex_);
    source_width_ = 0;
    source_height_ = 0;
    encode_width_ = 0;
    encode_height_ = 0;
    logged_no_desktop_ = false;
    logged_duplication_denied_ = false;
    next_dxgi_retry_ms_ = 0;  // a desktop hop is exactly when recovery is due
    staging_.Reset();
    duplication_.Reset();
    teardown_video_processor();
    if (!(init_d3d() && init_duplication(0))) {
        use_bitblt_ = true;
        source_width_ = GetSystemMetrics(SM_CXSCREEN);
        source_height_ = GetSystemMetrics(SM_CYSCREEN);
        apply_encode_size();
    }
    if (!use_bitblt_) {
        mlog::info("Capture rebuilt on the new desktop (" +
                   std::to_string(source_width_) + "x" +
                   std::to_string(source_height_) + ")");
    }
}

bool DesktopCapturer::capture_frame(CapturedFrame& frame, DWORD wait_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (use_bitblt_) {
        try_recover_dxgi();
    }
    if (use_bitblt_ || !duplication_) {
        return capture_with_bitblt(frame);
    }
    return capture_from_duplication(frame, wait_ms);
}

bool DesktopCapturer::capture_from_duplication(CapturedFrame& frame, DWORD wait_ms) {
    if (source_width_ < kMinEncodeDimension ||
        source_height_ < kMinEncodeDimension) {
        if (!logged_no_desktop_) {
            logged_no_desktop_ = true;
            mlog::warn("No desktop to capture yet, waiting");
        }
        return false;
    }
    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    Microsoft::WRL::ComPtr<IDXGIResource> frame_resource;

    HRESULT hr = duplication_->AcquireNextFrame(wait_ms, &frame_info, &frame_resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false;  // desktop unchanged
    }
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_DEVICE_REMOVED ||
            hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_INVALID_CALL) {
            try_recover_dxgi();
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

        const bool size_changed = static_cast<int>(desc.Width) != source_width_ ||
                                  static_cast<int>(desc.Height) != source_height_;
        if (size_changed) {
            source_width_ = static_cast<int>(desc.Width);
            source_height_ = static_cast<int>(desc.Height);
            apply_encode_size();
        }
        frame.source_width = source_width_;
        frame.source_height = source_height_;
        frame.width = encode_width_;
        frame.height = encode_height_;
        frame.is_keyframe = false;  // filled in by the encoder
        frame.timestamp_us = proto::steady_us();
        frame.d3d11_texture.Reset();

        // GPU path: convert BGRA->NV12 with the VideoProcessor and hand the
        // texture straight to the hardware encoder. No Map, no I420, no CPU.
        if (prefer_gpu_path_) {
            if (size_changed || !video_processor_ || vp_width_ != encode_width_ ||
                vp_height_ != encode_height_) {
                init_video_processor();
            }
            if (video_processor_ &&
                gpu_convert_frame(frame_texture.Get(), frame)) {
                duplication_->ReleaseFrame();
                return true;
            }
            // VP unavailable or blit failed: drop through to the CPU path so
            // the frame is not lost; the encoder failure counter handles a
            // persistently broken GPU path by downgrading to software.
        }

        if (ensure_staging_texture(static_cast<int>(desc.Width),
                                   static_cast<int>(desc.Height))) {
            context_->CopyResource(staging_.Get(), frame_texture.Get());

            D3D11_MAPPED_SUBRESOURCE mapped{};
            hr = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
            if (SUCCEEDED(hr)) {
                frame.i420.resize(static_cast<size_t>(frame.width) *
                                  frame.height * 3 / 2);
                convert_to_i420(static_cast<const uint8_t*>(mapped.pData),
                                static_cast<int>(mapped.RowPitch), frame);
                context_->Unmap(staging_.Get(), 0);
                duplication_->ReleaseFrame();
                return true;
            }
        }
    }

    duplication_->ReleaseFrame();
    try_recover_dxgi();
    return false;
}

bool DesktopCapturer::capture_with_bitblt(CapturedFrame& frame) {
    frame.d3d11_texture.Reset();  // BitBlt never fills the GPU texture
    int src_w = GetSystemMetrics(SM_CXSCREEN);
    int src_h = GetSystemMetrics(SM_CYSCREEN);
    if (src_w < kMinEncodeDimension || src_h < kMinEncodeDimension) {
        if (!logged_no_desktop_) {
            logged_no_desktop_ = true;
            mlog::warn("No desktop to capture yet (" + std::to_string(src_w) + "x" +
                       std::to_string(src_h) + "), waiting");
        }
        return false;
    }
    logged_no_desktop_ = false;
    if (src_w != source_width_ || src_h != source_height_) {
        source_width_ = src_w;
        source_height_ = src_h;
        apply_encode_size();
    }

    HDC hdc_screen = GetDC(nullptr);
    HDC hdc_mem = CreateCompatibleDC(hdc_screen);

    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = src_w;
    bih.biHeight = -src_h;  // top-down
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
    BitBlt(hdc_mem, 0, 0, src_w, src_h, hdc_screen, 0, 0, SRCCOPY);

    // Travels with the frame for ordering and diagnostics. The latency in the
    // control centre's header is a network round trip and never touches this.
    frame.timestamp_us = proto::steady_us();
    frame.source_width = src_w;
    frame.source_height = src_h;
    frame.width = encode_width_;
    frame.height = encode_height_;
    // The encoder fills this in: only it knows whether it emitted an IDR.
    frame.is_keyframe = false;
    frame.i420.resize(static_cast<size_t>(frame.width) * frame.height * 3 / 2);
    convert_to_i420(static_cast<const uint8_t*>(pixels), src_w * 4, frame);

    SelectObject(hdc_mem, old);
    DeleteObject(bitmap);
    DeleteDC(hdc_mem);
    ReleaseDC(nullptr, hdc_screen);
    return true;
}
