#include "video_encoder.hpp"

#include "wels/codec_api.h"
#include "wels/codec_def.h"

#include "log.hpp"

namespace {
constexpr int kI420 = videoFormatI420;
}

VideoEncoder::~VideoEncoder() {
    shutdown();
}

bool VideoEncoder::initialize(const EncoderConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;

    ISVCEncoder* encoder = nullptr;
    if (WelsCreateSVCEncoder(&encoder) != 0 || !encoder) {
        mlog::error("WelsCreateSVCEncoder failed");
        return false;
    }

    SEncParamExt params{};
    encoder->GetDefaultParams(&params);
    params.iUsageType = CAMERA_VIDEO_REAL_TIME;
    params.iPicWidth = config_.width;
    params.iPicHeight = config_.height;
    params.fMaxFrameRate = static_cast<float>(config_.fps > 0 ? config_.fps : 30);
    params.iTargetBitrate = config_.bitrate_kbps * 1000;
    params.iMaxBitrate = params.iTargetBitrate * 12 / 10;
    params.bEnableFrameSkip = true;

    if (encoder->InitializeExt(&params) != 0) {
        mlog::error("OpenH264 InitializeExt failed");
        WelsDestroySVCEncoder(encoder);
        return false;
    }

    encoder_ = encoder;
    y_plane_.resize(static_cast<size_t>(config_.width) * config_.height);
    u_plane_.resize(static_cast<size_t>(config_.width / 2) *
                    ((config_.height + 1) / 2));
    v_plane_.resize(u_plane_.size());
    initialized_ = true;
    mlog::info("OpenH264 encoder initialized (" + std::to_string(config_.width) +
               "x" + std::to_string(config_.height) + ")");
    return true;
}

void VideoEncoder::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (encoder_) {
        WelsDestroySVCEncoder(static_cast<ISVCEncoder*>(encoder_));
        encoder_ = nullptr;
    }
    initialized_ = false;
}

void VideoEncoder::force_keyframe() {
    keyframe_requested_ = true;
}

void VideoEncoder::bgra_to_i420(const uint8_t* bgra, int width, int height,
                                uint8_t* y, uint8_t* u, uint8_t* v) {
    for (int row = 0; row < height; ++row) {
        const uint8_t* src = bgra + static_cast<size_t>(row) * width * 4;
        uint8_t* dst = y + static_cast<size_t>(row) * width;
        for (int col = 0; col < width; ++col) {
            int b = src[col * 4 + 0];
            int g = src[col * 4 + 1];
            int r = src[col * 4 + 2];
            int yy = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            dst[col] = static_cast<uint8_t>(yy < 0 ? 0 : (yy > 255 ? 255 : yy));
        }
    }
    int cw = (width + 1) / 2;
    int ch = (height + 1) / 2;
    for (int j = 0; j < ch; ++j) {
        for (int i = 0; i < cw; ++i) {
            int sx = i * 2 < width ? i * 2 : width - 1;
            int sy = j * 2 < height ? j * 2 : height - 1;
            int b = bgra[static_cast<size_t>(sy) * width * 4 + sx * 4 + 0];
            int g = bgra[static_cast<size_t>(sy) * width * 4 + sx * 4 + 1];
            int r = bgra[static_cast<size_t>(sy) * width * 4 + sx * 4 + 2];
            int uu = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
            int vv = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
            u[static_cast<size_t>(j) * cw + i] = static_cast<uint8_t>(uu < 0 ? 0 : (uu > 255 ? 255 : uu));
            v[static_cast<size_t>(j) * cw + i] = static_cast<uint8_t>(vv < 0 ? 0 : (vv > 255 ? 255 : vv));
        }
    }
}

bool VideoEncoder::encode_frame(const uint8_t* bgra_data, int width, int height,
                                CapturedFrame* frame_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || !frame_out || width != config_.width ||
        height != config_.height) {
        return false;
    }

    bgra_to_i420(bgra_data, width, height, y_plane_.data(), u_plane_.data(),
                 v_plane_.data());

    if (keyframe_requested_) {
        static_cast<ISVCEncoder*>(encoder_)->ForceIntraFrame(true);
        keyframe_requested_ = false;
    }

    SSourcePicture pic{};
    pic.iColorFormat = kI420;
    pic.iPicWidth = width;
    pic.iPicHeight = height;
    pic.iStride[0] = width;
    pic.iStride[1] = width / 2;
    pic.iStride[2] = width / 2;
    pic.pData[0] = y_plane_.data();
    pic.pData[1] = u_plane_.data();
    pic.pData[2] = v_plane_.data();
    pic.uiTimeStamp = static_cast<long long>(frame_out->timestamp_us / 1000);

    SFrameBSInfo bs{};
    int rv = static_cast<ISVCEncoder*>(encoder_)->EncodeFrame(&pic, &bs);
    if (rv != 0 || bs.eFrameType == videoFrameTypeSkip) {
        static int warn_count = 0;
        if (warn_count++ < 3) {
            mlog::warn("EncodeFrame rv=" + std::to_string(rv) +
                       " frameType=" + std::to_string((int)bs.eFrameType));
        }
        return false;
    }

    frame_out->h264_data.clear();
    for (int l = 0; l < bs.iLayerNum; ++l) {
        const SLayerBSInfo& layer = bs.sLayerInfo[l];
        int offset = 0;
        for (int n = 0; n < layer.iNalCount; ++n) {
            int nal_len = layer.pNalLengthInByte[n];
            frame_out->h264_data.insert(frame_out->h264_data.end(),
                                        layer.pBsBuf + offset,
                                        layer.pBsBuf + offset + nal_len);
            offset += nal_len;
        }
    }
    return !frame_out->h264_data.empty();
}
