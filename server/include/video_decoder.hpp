#pragma once

#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <QImage>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class H264Decoder;  // existing OpenH264 software decoder (h264_decoder.hpp)

// Which bitstream the agent is sending. Both encoders emit Annex-B / AVCC
// variants of their codec; the decoder created for a session must match.
enum class CodecType {
    CODEC_H264 = 0,
    CODEC_HEVC = 1,
};

// Decoder interface used by FramePipeline; one instance per session.
class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;
    virtual bool initialize(int width, int height, CodecType type) = 0;
    virtual bool decode(const uint8_t* data, size_t size, QImage& out) = 0;
    virtual CodecType codec_type() const = 0;
};

// Media Foundation decoder (HEVC / H.264, hardware accelerated where the OS
// offers it). Accepts Annex-B from the OpenH264 encoder and AVCC from the MF
// encoder by normalising to AVCC before ProcessInput.
class MfDecoder : public IVideoDecoder {
public:
    explicit MfDecoder(CodecType type);
    ~MfDecoder() override;

    bool initialize(int width, int height, CodecType type) override;
    bool decode(const uint8_t* data, size_t size, QImage& out) override;
    CodecType codec_type() const override { return type_; }

private:
    bool init_mft(const GUID& subtype);
    bool drain_decoded(QImage& out);
    void shutdown();

    CodecType type_ = CodecType::CODEC_H264;
    Microsoft::WRL::ComPtr<IMFTransform> mft_;
    int width_ = 0;
    int height_ = 0;
    bool initialized_ = false;
    bool output_provides_samples_ = false;
    // Output buffer sizing from MFT_OUTPUT_STREAM_INFO: when the MFT does not
    // provide samples itself, the caller must attach a buffer of cbSize (aligned
    // to cbAlignment) to the sample it hands to ProcessOutput.
    DWORD output_cb_size_ = 0;
    DWORD output_cb_alignment_ = 1;
};

// Adapter over the existing OpenH264 decoder so FramePipeline only has to know
// IVideoDecoder. Only used for H.264 (OpenH264 has no HEVC support).
class OpenH264DecoderAdapter : public IVideoDecoder {
public:
    ~OpenH264DecoderAdapter() override;

    bool initialize(int width, int height, CodecType type) override;
    bool decode(const uint8_t* data, size_t size, QImage& out) override;
    CodecType codec_type() const override { return CodecType::CODEC_H264; }

private:
    std::unique_ptr<H264Decoder> inner_;
};

// Picks the decoder for a session: MF (hardware) first, OpenH264 for H.264 as
// the final fallback. HEVC without an MF decoder means the agent must encode
// H.264 instead, which the codec negotiation handles.
class DecoderFactory {
public:
    static bool is_mf_decoder_available(CodecType type);
    static std::unique_ptr<IVideoDecoder> create(CodecType type, int width,
                                                 int height);
};
