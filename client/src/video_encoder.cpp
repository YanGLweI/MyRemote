#include "video_encoder.hpp"

#include "log.hpp"

VideoEncoder::~VideoEncoder() {
    shutdown();
}

bool VideoEncoder::initialize(const EncoderConfig& config) {
    config_ = config;
    // TODO(M4): create IMFTransform H.264 encoder (hardware probe + software fallback).
    mlog::warn("VideoEncoder not yet implemented (M4); streaming disabled until then");
    initialized_ = false;
    return initialized_;
}

void VideoEncoder::shutdown() {
    initialized_ = false;
}

bool VideoEncoder::encode_frame(const uint8_t* bgra_data, int width, int height,
                                CapturedFrame* frame_out) {
    (void)bgra_data;
    (void)width;
    (void)height;
    (void)frame_out;
    return false;
}

void VideoEncoder::force_keyframe() {
    keyframe_requested_ = true;
}
