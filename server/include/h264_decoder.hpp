#pragma once

#include <QImage>

#include <cstddef>
#include <cstdint>
#include <vector>

// H.264 decoder backed by OpenH264 (mirrors the client's encoder),
// independent of Media Foundation codec availability.
class H264Decoder {
public:
    H264Decoder() = default;
    ~H264Decoder();

    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    bool initialize(int width, int height);
    void shutdown();
    bool decode(const uint8_t* data, size_t size, QImage& out);
    bool is_initialized() const { return initialized_; }

private:
    void* decoder_ = nullptr;  // ISVCDecoder*
    std::vector<uint8_t> yuv_;
    int width_ = 0;
    int height_ = 0;
    bool initialized_ = false;
};
