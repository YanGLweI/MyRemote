// Media Foundation decoders (HEVC / H.264) plus the OpenH264 fallback adapter
// for the control centre. One decoder per session; FramePipeline owns it and
// swaps instances when codec negotiation changes the stream.

#include "video_decoder.hpp"

#include <mferror.h>
#include <mftransform.h>

#include <cstring>

#include "h264_decoder.hpp"
#include "log.hpp"

namespace {

// Media Foundation must be started before MFTEnumEx works at all; without it
// the platform answers "no MFTs found" even for decoders that are clearly
// registered. Ref-counted by MF itself, so the matching MFShutdown calls in
// the decoder destructors are safe.
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

// Media Foundation decoders consume AVCC (length-prefixed NAL units). The
// OpenH264 encoder sends Annex-B (start codes), so normalise both on arrival:
// if start codes are present, convert; otherwise the payload is already AVCC.
bool has_start_code(const uint8_t* data, size_t size) {
    for (size_t i = 0; i + 3 < size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) return true;
    }
    return false;
}

bool annexb_to_avcc(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(size + 64);
    size_t i = 0;
    while (i + 3 < size) {
        // Skip to the next start code.
        if (!(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)) {
            ++i;
            continue;
        }
        i += 3;
        if (i < size && data[i] == 0) ++i;  // 00 00 00 01 form
        size_t nal_start = i;
        while (i + 3 <= size) {
            if (data[i] == 0 && data[i + 1] == 0 &&
                (data[i + 2] == 1 || (data[i + 2] == 0 && i + 3 < size &&
                                      data[i + 3] == 1))) {
                break;
            }
            ++i;
        }
        const size_t nal_len = i - nal_start;
        if (nal_len > 0 && nal_start + nal_len <= size) {
            out.push_back(static_cast<uint8_t>((nal_len >> 24) & 0xFF));
            out.push_back(static_cast<uint8_t>((nal_len >> 16) & 0xFF));
            out.push_back(static_cast<uint8_t>((nal_len >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>(nal_len & 0xFF));
            out.insert(out.end(), data + nal_start, data + nal_start + nal_len);
        }
    }
    return !out.empty();
}

// NV12 -> RGB32, same BT.601 math the OpenH264 path uses so both decoders
// agree on colour. The MF decoder hands us a contiguous NV12 buffer.
void nv12_to_qimage(const uint8_t* y, const uint8_t* uv, int y_stride,
                    int uv_stride, int width, int height, QImage& out) {
    out = QImage(width, height, QImage::Format_RGB32);
    for (int row = 0; row < height; ++row) {
        const uint8_t* y_row = y + static_cast<size_t>(row) * y_stride;
        const uint8_t* uv_row = uv + static_cast<size_t>(row / 2) * uv_stride;
        uint32_t* dpx = reinterpret_cast<uint32_t*>(out.scanLine(row));
        for (int col = 0; col < width; ++col) {
            int yy = y_row[col] - 16;
            int uu = uv_row[(col & ~1)] - 128;
            int vv = uv_row[(col & ~1) + 1] - 128;
            int r = (298 * yy + 409 * vv + 128) >> 8;
            int g = (298 * yy - 100 * uu - 208 * vv + 128) >> 8;
            int b = (298 * yy + 516 * uu + 128) >> 8;
            r = r < 0 ? 0 : (r > 255 ? 255 : r);
            g = g < 0 ? 0 : (g > 255 ? 255 : g);
            b = b < 0 ? 0 : (b > 255 ? 255 : b);
            dpx[col] = 0xFF000000u | (static_cast<uint32_t>(r) << 16) |
                       (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// MfDecoder
// ---------------------------------------------------------------------------
MfDecoder::MfDecoder(CodecType type) : type_(type) {}

MfDecoder::~MfDecoder() {
    shutdown();
}

void MfDecoder::shutdown() {
    if (mft_) {
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    mft_.Reset();
    initialized_ = false;
}

bool MfDecoder::init_mft(const GUID& subtype) {
    // The codec (H.264/HEVC) is the decoder's *input* type: it consumes a
    // compressed stream and emits NV12 frames. Constraining the input type is
    // what finds the right MFT; constraining the output would look for a
    // decoder that emits H.264 and find nothing.
    //
    // MFT_ENUM_FLAG_SORTANDFILTER is deliberately NOT used: it hides hardware
    // MFTs whose registration lacks full type info (see find_encoder_mft).
    MFT_REGISTER_TYPE_INFO input = {MFMediaType_Video, subtype};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    if (!ensure_mf_started()) {
        return false;
    }
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                           0, &input, nullptr,
                           &activates, &count);
    if (FAILED(hr) || count == 0 || !activates) {
        return false;
    }
    hr = activates[0]->ActivateObject(IID_PPV_ARGS(mft_.GetAddressOf()));
    for (UINT32 i = 0; i < count; ++i) {
        activates[i]->Release();
    }
    CoTaskMemFree(activates);
    return SUCCEEDED(hr) && mft_;
}

bool MfDecoder::initialize(int width, int height, CodecType type) {
    shutdown();
    type_ = type;
    width_ = width;
    height_ = height;
    if (width <= 0 || height <= 0) {
        mlog::error("MfDecoder: invalid size " + std::to_string(width) + "x" +
                    std::to_string(height));
        return false;
    }

    const GUID* subtype = type == CodecType::CODEC_HEVC ? &MFVideoFormat_HEVC
                                                        : &MFVideoFormat_H264;
    if (!init_mft(*subtype)) {
        mlog::warn(std::string("No Media Foundation decoder for ") +
                   (type == CodecType::CODEC_HEVC ? "HEVC" : "H.264"));
        return false;
    }

    Microsoft::WRL::ComPtr<IMFMediaType> input;
    if (FAILED(MFCreateMediaType(input.GetAddressOf()))) return false;
    input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input->SetGUID(MF_MT_SUBTYPE, *subtype);
    input->SetUINT64(MF_MT_FRAME_SIZE, Pack2UINT32AsUINT64(
                                           static_cast<UINT32>(width),
                                           static_cast<UINT32>(height)));
    input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(mft_->SetInputType(0, input.Get(), 0))) {
        mlog::warn("MF decoder rejected input type");
        shutdown();
        return false;
    }

    // Pick the output type from what the MFT actually offers. Hand-built
    // NV12 types are often rejected: the MFT wants one of the types it
    // advertises through GetOutputAvailableType, and calling ProcessOutput
    // with no output type set fails with MF_E_TRANSFORM_TYPE_NOT_SET.
    Microsoft::WRL::ComPtr<IMFMediaType> output;
    bool output_set = false;
    for (DWORD i = 0; !output_set; ++i) {
        Microsoft::WRL::ComPtr<IMFMediaType> candidate;
        HRESULT hr = mft_->GetOutputAvailableType(0, i, candidate.GetAddressOf());
        if (FAILED(hr)) {
            break;  // ran out of advertised types
        }
        GUID subtype_guid = GUID_NULL;
        if (SUCCEEDED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype_guid)) &&
            subtype_guid == MFVideoFormat_NV12) {
            // Only NV12 is acceptable: drain_decoded assumes NV12 plane
            // layout, so any other advertised type (YUY2, RGB32, P010...)
            // would decode into wrong colours. Better to fall back to
            // OpenH264 than to paint garbage.
            if (SUCCEEDED(mft_->SetOutputType(0, candidate.Get(), 0))) {
                output = candidate;
                output_set = true;
            }
        }
    }
    if (!output_set) {
        mlog::warn("MF decoder has no NV12 output; using OpenH264 instead");
        shutdown();
        return false;
    }

    MFT_OUTPUT_STREAM_INFO info{};
    if (SUCCEEDED(mft_->GetOutputStreamInfo(0, &info))) {
        output_provides_samples_ =
            (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
        output_cb_size_ = info.cbSize;
        if (info.cbAlignment > 0) {
            output_cb_alignment_ = info.cbAlignment;
        }
    }

    if (FAILED(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0))) {
        shutdown();
        return false;
    }
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    initialized_ = true;
    mlog::info(std::string("Media Foundation ") +
               (type == CodecType::CODEC_HEVC ? "HEVC" : "H.264") +
               " decoder ready (" + std::to_string(width) + "x" +
               std::to_string(height) + ")");
    return true;
}

bool MfDecoder::decode(const uint8_t* data, size_t size, QImage& out) {
    if (!initialized_ || !mft_ || !data || size == 0) {
        return false;
    }

    std::vector<uint8_t> avcc;
    const uint8_t* bits = data;
    size_t bit_len = size;
    if (has_start_code(data, size)) {
        if (!annexb_to_avcc(data, size, avcc)) return false;
        bits = avcc.data();
        bit_len = avcc.size();
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(bit_len),
                                    buffer.GetAddressOf()))) {
        return false;
    }
    BYTE* dst = nullptr;
    if (SUCCEEDED(buffer->Lock(&dst, nullptr, nullptr))) {
        memcpy(dst, bits, bit_len);
        buffer->Unlock();
    }
    buffer->SetCurrentLength(static_cast<DWORD>(bit_len));

    Microsoft::WRL::ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(sample.GetAddressOf()))) return false;
    sample->AddBuffer(buffer.Get());

    HRESULT hr = mft_->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr)) {
        mlog::info("MF decoder ProcessInput failed: 0x" +
                    std::to_string(static_cast<unsigned long>(hr)));
        return false;
    }
    return drain_decoded(out);
}

bool MfDecoder::drain_decoded(QImage& out) {
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER ob{};
        Microsoft::WRL::ComPtr<IMFSample> sample;
        Microsoft::WRL::ComPtr<IMFMediaBuffer> out_buffer;
        if (!output_provides_samples_) {
            if (FAILED(MFCreateSample(sample.GetAddressOf()))) return false;
            // An empty sample makes ProcessOutput fail immediately: the MFT
            // needs somewhere to write the decoded frame. Size the buffer from
            // the stream info the MFT gave us at init.
            const DWORD size = output_cb_size_ > 0 ? output_cb_size_ : 1;
            if (FAILED(MFCreateAlignedMemoryBuffer(size, output_cb_alignment_,
                                                   out_buffer.GetAddressOf()))) {
                return false;
            }
            if (FAILED(sample->AddBuffer(out_buffer.Get()))) return false;
            ob.pSample = sample.Get();
        }
        DWORD status = 0;
        HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return false;
        if (FAILED(hr)) {
            mlog::warn("MF decoder ProcessOutput failed: 0x" +
                       std::to_string(static_cast<unsigned long>(hr)));
            return false;
        }
        if (!ob.pSample) continue;

        Microsoft::WRL::ComPtr<IMFMediaBuffer> contiguous;
        if (FAILED(ob.pSample->ConvertToContiguousBuffer(
                contiguous.GetAddressOf()))) {
            return false;
        }
        Microsoft::WRL::ComPtr<IMF2DBuffer> buf2d;
        if (SUCCEEDED(contiguous.As(&buf2d))) {
            BYTE* y = nullptr;
            LONG y_stride = 0;
            if (SUCCEEDED(buf2d->Lock2D(&y, &y_stride))) {
                const int h = height_ > 0 ? height_ : 1;
                const int w = width_ > 0 ? width_ : 1;
                const BYTE* uv = y + static_cast<size_t>(y_stride) * h;
                nv12_to_qimage(y, uv, y_stride, y_stride, w, h, out);
                buf2d->Unlock2D();
                return true;
            }
        }
        return false;
    }
}

// ---------------------------------------------------------------------------
// OpenH264DecoderAdapter
// ---------------------------------------------------------------------------
OpenH264DecoderAdapter::~OpenH264DecoderAdapter() = default;

bool OpenH264DecoderAdapter::initialize(int width, int height, CodecType type) {
    inner_ = std::make_unique<H264Decoder>();
    return inner_->initialize(width, height);
}

bool OpenH264DecoderAdapter::decode(const uint8_t* data, size_t size,
                                    QImage& out) {
    return inner_ && inner_->decode(data, size, out);
}

// ---------------------------------------------------------------------------
// DecoderFactory
// ---------------------------------------------------------------------------
bool DecoderFactory::is_mf_decoder_available(CodecType type) {
    // Same input-type constraint as MfDecoder::init_mft: the codec is what the
    // decoder consumes, not what it produces. No SORTANDFILTER: hardware MFTs
    // would be hidden by it.
    MFT_REGISTER_TYPE_INFO input = {
        MFMediaType_Video,
        type == CodecType::CODEC_HEVC ? MFVideoFormat_HEVC : MFVideoFormat_H264};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    if (!ensure_mf_started()) {
        return false;
    }
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                           0, &input, nullptr,
                           &activates, &count);
    if (activates) {
        for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
        CoTaskMemFree(activates);
    }
    return SUCCEEDED(hr) && count > 0;
}

std::unique_ptr<IVideoDecoder> DecoderFactory::create(CodecType type, int width,
                                                      int height) {
    if (type == CodecType::CODEC_HEVC) {
        auto decoder = std::make_unique<MfDecoder>(CodecType::CODEC_HEVC);
        if (decoder->initialize(width, height, CodecType::CODEC_HEVC)) {
            return decoder;
        }
        mlog::warn("HEVC decoder unavailable; the agent must fall back to H.264");
        return nullptr;
    }
    // H.264: try MF first, then OpenH264.
    auto decoder = std::make_unique<MfDecoder>(CodecType::CODEC_H264);
    if (decoder->initialize(width, height, CodecType::CODEC_H264)) {
        return decoder;
    }
    mlog::info("MF H.264 decoder unavailable, using OpenH264");
    auto soft = std::make_unique<OpenH264DecoderAdapter>();
    if (soft->initialize(width, height, CodecType::CODEC_H264)) {
        return soft;
    }
    return nullptr;
}
