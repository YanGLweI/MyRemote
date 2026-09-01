// Media Foundation hardware encoder (HEVC / H.264) for the agent.
//
// The Desktop Duplication capturer hands us the freshly acquired D3D11 texture;
// we wrap it as a DXGI surface buffer and feed it straight into a hardware
// encoder MFT, so the pixels never leave the GPU. Output is AVCC
// (length-prefixed NAL units), which the control centre's MF decoder consumes
// natively. A failure anywhere on this path hot-swaps to the OpenH264 software
// encoder instead of dropping the session.

#include "video_encoder.hpp"

#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <codecapi.h>
#include <propvarutil.h>
#include <strmif.h>  // ICodecAPI
#include <wrl/client.h>

#include "log.hpp"
#include "messages.hpp"

using Microsoft::WRL::ComPtr;

namespace {

// Media Foundation must be started before MFTEnumEx works at all; without it
// the platform answers "no MFTs found" even for encoders that are clearly
// registered (Intel QSV, NVENC...). One start per process, ref-counted by MF
// itself, so the matching MFShutdown in the encoder destructor is safe.
bool ensure_mf_started() {
    static bool started = []() {
        HRESULT hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) {
            mlog::error("MFStartup failed: 0x" +
                        std::to_string(static_cast<unsigned long>(hr)));
            return false;
        }
        return true;
    }();
    return started;
}

// Enumerate hardware encoder MFTs that emit `subtype` and activate the first
// one. Drivers register their NVENC/QSV/VCE encoders here, so this works on
// NVIDIA, Intel and AMD without vendor code.
//
// Note: MFT_ENUM_FLAG_SORTANDFILTER is deliberately NOT used. That flag only
// returns MFTs whose registration carries full In/Out media-type information;
// hardware MFTs (Intel QSV, NVENC, VCE) rarely register those, so the filter
// silently hides every GPU encoder while the software ones survive.
bool find_encoder_mft(const GUID& subtype, ComPtr<IMFTransform>& out) {
    if (!ensure_mf_started()) {
        return false;
    }
    MFT_REGISTER_TYPE_INFO output = {MFMediaType_Video, subtype};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                           MFT_ENUM_FLAG_HARDWARE,
                           nullptr, &output, &activates, &count);
    if (FAILED(hr) || count == 0 || !activates) {
        return false;
    }
    hr = activates[0]->ActivateObject(IID_PPV_ARGS(out.GetAddressOf()));
    for (UINT32 i = 0; i < count; ++i) {
        activates[i]->Release();
    }
    CoTaskMemFree(activates);
    return SUCCEEDED(hr) && out;
}

HRESULT make_video_type(const GUID& subtype, int width, int height, int fps,
                        bool input, IMFMediaType** out) {
    HRESULT hr = MFCreateMediaType(out);
    if (FAILED(hr)) return hr;
    (*out)->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    (*out)->SetGUID(MF_MT_SUBTYPE, subtype);
    (*out)->SetUINT64(MF_MT_FRAME_SIZE, Pack2UINT32AsUINT64(
                                            static_cast<UINT32>(width),
                                            static_cast<UINT32>(height)));
    (*out)->SetUINT64(MF_MT_FRAME_RATE,
                      Pack2UINT32AsUINT64(static_cast<UINT32>(fps), 1));
    (*out)->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (input) {
        (*out)->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO,
                          Pack2UINT32AsUINT64(1, 1));
    }
    return hr;
}

// MF encoders emit AVCC (4-byte big-endian length prefix per NAL unit), but
// the wire protocol is Annex-B (00 00 00 01 start codes) because that is what
// the OpenH264 decoder consumes. Rewrite every length prefix into a start
// code so either decoder on the control side can handle the stream.
void avcc_to_annex_b(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(size + size / 8);
    size_t i = 0;
    while (i + 4 <= size) {
        const uint32_t nal_len =
            (static_cast<uint32_t>(data[i]) << 24) |
            (static_cast<uint32_t>(data[i + 1]) << 16) |
            (static_cast<uint32_t>(data[i + 2]) << 8) |
            static_cast<uint32_t>(data[i + 3]);
        i += 4;
        if (nal_len == 0 || i + nal_len > size) {
            break;  // corrupt tail: hand the remainder over as-is
        }
        out.push_back(0);
        out.push_back(0);
        out.push_back(0);
        out.push_back(1);
        out.insert(out.end(), data + i, data + i + nal_len);
        i += nal_len;
    }
    if (i < size) {
        out.insert(out.end(), data + i, data + size);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// MfHardwareEncoder
// ---------------------------------------------------------------------------
class MfHardwareEncoder : public IStreamEncoder {
public:
    MfHardwareEncoder(EncoderBackend backend, const EncoderConfig& config)
        : backend_(backend), config_(config) {}
    ~MfHardwareEncoder() override;

    bool initialize(const EncoderConfig& config) override;
    bool encode_frame(CapturedFrame& frame) override;
    void force_keyframe() override;
    bool is_initialized() const override { return initialized_; }
    uint64_t exchange_skips() override { return skips_.exchange(0); }
    EncoderBackend backend() const override { return backend_; }
    std::string backend_name() const override {
        return backend_ == EncoderBackend::HARDWARE_HEVC_MF
                   ? "HEVC 硬编 (GPU)"
                   : "H.264 硬编 (GPU)";
    }

private:
    bool init_mft(const GUID& subtype);
    void configure_codec_api();
    bool submit_frame(ID3D11Texture2D* texture, uint64_t timestamp_us);
    bool drain_output(CapturedFrame& frame);
    void request_keyframe_locked();

    EncoderBackend backend_;
    EncoderConfig config_;
    ComPtr<IMFTransform> mft_;
    ComPtr<ICodecAPI> codec_api_;
    bool output_provides_samples_ = false;
    // Output buffer sizing from MFT_OUTPUT_STREAM_INFO; same contract as the
    // decoder: when the MFT does not provide samples, ours must carry a
    // buffer of cbSize aligned to cbAlignment.
    DWORD output_cb_size_ = 0;
    DWORD output_cb_alignment_ = 1;
    bool initialized_ = false;
    bool keyframe_requested_ = false;
    std::mutex mutex_;
    std::atomic<uint64_t> skips_{0};
};

MfHardwareEncoder::~MfHardwareEncoder() {
    if (mft_) {
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    mft_.Reset();
}

bool MfHardwareEncoder::initialize(const EncoderConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    if (mft_) {
        mft_.Reset();
        codec_api_.Reset();
    }
    initialized_ = false;

    const GUID* subtype = backend_ == EncoderBackend::HARDWARE_HEVC_MF
                              ? &MFVideoFormat_HEVC
                              : &MFVideoFormat_H264;
    if (!init_mft(*subtype)) {
        mlog::warn(std::string("Hardware encoder unavailable for ") +
                   (backend_ == EncoderBackend::HARDWARE_HEVC_MF ? "HEVC"
                                                                 : "H.264"));
        return false;
    }

    // Input: the capturer's GPU texture. The format is negotiated through
    // EncoderConfig.gpu_input_format (1 = NV12 via VideoProcessor); 0 means
    // the capturer cannot produce a GPU frame and the caller must fall back
    // to the software encoder.
    if (config_.gpu_input_format != 1) {
        mlog::warn("No GPU input format negotiated; hardware encoder disabled");
        return false;
    }
    ComPtr<IMFMediaType> input_type;
    if (FAILED(make_video_type(MFVideoFormat_NV12, config_.width, config_.height,
                               config_.fps, true, input_type.GetAddressOf()))) {
        mlog::error("Failed to build encoder input type");
        return false;
    }
    input_type->SetUINT32(MF_SA_D3D11_AWARE, TRUE);
    if (FAILED(mft_->SetInputType(0, input_type.Get(), 0))) {
        mlog::warn("Hardware encoder rejected the input format, falling back");
        return false;
    }

    // Output: the requested bitstream subtype.
    ComPtr<IMFMediaType> output_type;
    if (FAILED(make_video_type(*subtype, config_.width, config_.height,
                               config_.fps, false, output_type.GetAddressOf()))) {
        mlog::error("Failed to build encoder output type");
        return false;
    }
    output_type->SetUINT32(MF_MT_AVG_BITRATE,
                           static_cast<UINT32>(config_.bitrate_kbps) * 1000);
    if (FAILED(mft_->SetOutputType(0, output_type.Get(), 0))) {
        mlog::warn("Hardware encoder rejected output type, falling back");
        return false;
    }

    configure_codec_api();

    MFT_OUTPUT_STREAM_INFO out_info{};
    if (SUCCEEDED(mft_->GetOutputStreamInfo(0, &out_info))) {
        output_provides_samples_ =
            (out_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
        output_cb_size_ = out_info.cbSize;
        if (out_info.cbAlignment > 0) {
            output_cb_alignment_ = out_info.cbAlignment;
        }
    }

    if (FAILED(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0))) {
        mlog::error("Encoder refused to begin streaming");
        return false;
    }
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    initialized_ = true;
    keyframe_requested_ = false;
    mlog::info(backend_name() + " initialized (" + std::to_string(config_.width) +
               "x" + std::to_string(config_.height) + ", " +
               std::to_string(config_.fps) + "fps, " +
               std::to_string(config_.bitrate_kbps) + "kbps)");
    return true;
}

bool MfHardwareEncoder::init_mft(const GUID& subtype) {
    if (!find_encoder_mft(subtype, mft_)) {
        return false;
    }
    // DXGI surface buffers only work when the MFT knows it may receive them.
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(mft_.As(&attrs))) {
        attrs->SetUINT32(MF_SA_D3D11_AWARE, TRUE);
        attrs->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_RENDER_TARGET);
    }
    mft_.As(&codec_api_);
    return true;
}

void MfHardwareEncoder::configure_codec_api() {
    if (!codec_api_) return;
    VARIANT v;
    if (SUCCEEDED(InitVariantFromBoolean(TRUE, &v))) {
        // Low-latency mode is what keeps a remote session feeling instant:
        // the encoder stops buffering frames in the name of quality.
        if (FAILED(codec_api_->SetValue(&CODECAPI_AVLowLatencyMode, &v))) {
            mlog::info("CODECAPI_AVLowLatencyMode refused by encoder");
        }
        VariantClear(&v);
    }
    if (SUCCEEDED(InitVariantFromUInt32(eAVEncCommonRateControlMode_CBR, &v))) {
        codec_api_->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
        VariantClear(&v);
    }
    if (SUCCEEDED(InitVariantFromUInt32(
            static_cast<UINT32>(config_.bitrate_kbps) * 1000, &v))) {
        codec_api_->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);
        VariantClear(&v);
    }
}

void MfHardwareEncoder::force_keyframe() {
    std::lock_guard<std::mutex> lock(mutex_);
    keyframe_requested_ = true;
}

void MfHardwareEncoder::request_keyframe_locked() {
    if (!codec_api_ || !keyframe_requested_) return;
    VARIANT v;
    if (SUCCEEDED(InitVariantFromBoolean(TRUE, &v))) {
        codec_api_->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
        VariantClear(&v);
    }
    keyframe_requested_ = false;
}

bool MfHardwareEncoder::submit_frame(ID3D11Texture2D* texture,
                                     uint64_t timestamp_us) {
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture, 0,
                                           FALSE, buffer.GetAddressOf());
    if (FAILED(hr)) {
        mlog::error("MFCreateDXGISurfaceBuffer failed: 0x" +
                    std::to_string(static_cast<unsigned long>(hr)));
        return false;
    }
    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(sample.GetAddressOf()))) return false;
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(static_cast<LONGLONG>(timestamp_us / 10));  // 100ns
    sample->SetSampleDuration(10000000LL / (config_.fps > 0 ? config_.fps : 30));
    return SUCCEEDED(mft_->ProcessInput(0, sample.Get(), 0));
}

bool MfHardwareEncoder::drain_output(CapturedFrame& frame) {
    std::vector<uint8_t> avcc;
    avcc.reserve(64 * 1024);
    frame.is_keyframe = false;
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER out{};
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> out_buffer;
        if (!output_provides_samples_) {
            if (FAILED(MFCreateSample(sample.GetAddressOf()))) return false;
            // Same contract as the decoder: an empty sample fails ProcessOutput.
            // Give the MFT a buffer of the size it advertised at init.
            const DWORD size = output_cb_size_ > 0 ? output_cb_size_ : 1;
            if (FAILED(MFCreateAlignedMemoryBuffer(size, output_cb_alignment_,
                                                   out_buffer.GetAddressOf()))) {
                return false;
            }
            if (FAILED(sample->AddBuffer(out_buffer.Get()))) return false;
            out.pSample = sample.Get();
        }
        DWORD status = 0;
        HRESULT hr = mft_->ProcessOutput(0, 1, &out, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            break;
        }
        if (FAILED(hr)) {
            mlog::info("Encoder ProcessOutput failed: 0x" +
                        std::to_string(static_cast<unsigned long>(hr)));
            return false;
        }
        if (!out.pSample) continue;
        // MF marks key frames on the sample; propagate it to the frame so the
        // control centre knows it can resync from this access unit.
        UINT32 clean_point = 0;
        if (SUCCEEDED(out.pSample->GetUINT32(MFSampleExtension_CleanPoint,
                                             &clean_point))) {
            frame.is_keyframe = clean_point != 0;
        }
        ComPtr<IMFMediaBuffer> contiguous;
        if (SUCCEEDED(out.pSample->ConvertToContiguousBuffer(
                contiguous.GetAddressOf()))) {
            BYTE* data = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(contiguous->Lock(&data, nullptr, &len))) {
                avcc.insert(avcc.end(), data, data + len);
                contiguous->Unlock();
            }
        }
        if (status & MFT_OUTPUT_DATA_BUFFER_INCOMPLETE) {
            continue;  // more output for the same input frame
        }
        break;
    }
    if (avcc.empty()) {
        frame.h264_data.clear();
        return false;
    }
    // Normalise to Annex-B so both the MF decoder and the OpenH264 fallback
    // on the control side can decode this frame.
    avcc_to_annex_b(avcc.data(), avcc.size(), frame.h264_data);
    return !frame.h264_data.empty();
}

bool MfHardwareEncoder::encode_frame(CapturedFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || !frame.d3d11_texture) {
        skips_.fetch_add(1);
        return false;
    }
    if (keyframe_requested_) {
        request_keyframe_locked();
    }
    if (!submit_frame(frame.d3d11_texture.Get(), frame.timestamp_us)) {
        skips_.fetch_add(1);
        return false;
    }
    return drain_output(frame);
}

// ---------------------------------------------------------------------------
// EncoderFactory
// ---------------------------------------------------------------------------
CodecInfo EncoderFactory::probe_capabilities() {
    CodecInfo caps;
    ComPtr<IMFTransform> probe;
    if (find_encoder_mft(MFVideoFormat_HEVC, probe)) {
        caps.hardware_hevc_available = 1;
        caps.supported_codecs |= proto::kCodecMaskHEVC_Hardware;
        probe.Reset();
    }
    if (find_encoder_mft(MFVideoFormat_H264, probe)) {
        caps.hardware_h264_available = 1;
        caps.supported_codecs |= proto::kCodecMaskH264_Hardware;
        probe.Reset();
    }
    caps.supported_codecs |= proto::kCodecMaskH264_Software;  // OpenH264 always
    mlog::info("Encoder probe: HEVC-HW=" +
               std::to_string(caps.hardware_hevc_available) +
               ", H264-HW=" + std::to_string(caps.hardware_h264_available));
    return caps;
}

std::unique_ptr<IStreamEncoder> EncoderFactory::create_selected(
    const EncoderConfig& config) {
    // Preferred order: HEVC hardware, H.264 hardware, OpenH264 software.
    for (EncoderBackend candidate : {EncoderBackend::HARDWARE_HEVC_MF,
                                     EncoderBackend::HARDWARE_H264_MF,
                                     EncoderBackend::SOFTWARE_OPENH264}) {
        auto encoder = create_backend(candidate, config);
        if (encoder) {
            return encoder;
        }
    }
    mlog::error("No video encoder could be created");
    return nullptr;
}

std::unique_ptr<IStreamEncoder> EncoderFactory::create_backend(
    EncoderBackend backend, const EncoderConfig& config) {
    if (backend == EncoderBackend::SOFTWARE_OPENH264) {
        auto soft = std::make_unique<VideoEncoder>();
        if (soft->initialize(config)) {
            return soft;
        }
        mlog::error("OpenH264 encoder failed to initialise");
        return nullptr;
    }
    auto hard = std::make_unique<MfHardwareEncoder>(backend, config);
    if (hard->initialize(config)) {
        return hard;
    }
    return nullptr;  // caller walks down the preference list
}
