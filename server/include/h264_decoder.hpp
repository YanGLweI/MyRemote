#pragma once

#include <QImage>

#include <cstddef>
#include <cstdint>

// H.264 decoder backed by Media Foundation (implemented in M4).
class H264Decoder {
public:
    H264Decoder() = default;
    ~H264Decoder();

    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    bool initialize();
    void shutdown();

    // Feed one packet; returns true when a decoded frame is produced.
    bool decode(const uint8_t* data, size_t size, QImage& out);

    bool is_initialized() const { return initialized_; }

private:
    bool initialized_ = false;
};
