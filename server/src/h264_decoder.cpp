#include "h264_decoder.hpp"

#include "log.hpp"

H264Decoder::~H264Decoder() {
    shutdown();
}

bool H264Decoder::initialize() {
    // TODO(M4): MFTEnumEx for the H.264 decoder MFT, wire ProcessInput/Output.
    mlog::warn("H264Decoder not yet implemented (M4)");
    initialized_ = false;
    return initialized_;
}

void H264Decoder::shutdown() {
    initialized_ = false;
}

bool H264Decoder::decode(const uint8_t* data, size_t size, QImage& out) {
    (void)data;
    (void)size;
    (void)out;
    return false;
}
