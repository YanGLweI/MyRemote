#include "video_encoder.hpp"

#include "wels/codec_api.h"
#include "wels/codec_def.h"

#include <algorithm>

#include "log.hpp"

namespace {
// Slice parallelism is how OpenH264 spreads a frame over cores; the thread
// count has to match it or the encoder silently stays single-threaded.
constexpr int kSliceCount = 4;

long long bitrate_floor_bps(int width, int height, int fps) {
    return static_cast<long long>(width) * height * fps * 5 / 100;  // ~0.05 bpp
}
}  // namespace

VideoEncoder::~VideoEncoder() {
    shutdown();
}

bool VideoEncoder::initialize(const EncoderConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    return initialize_locked();
}

bool VideoEncoder::initialize_locked() {
    if (encoder_) {
        WelsDestroySVCEncoder(static_cast<ISVCEncoder*>(encoder_));
        encoder_ = nullptr;
    }
    initialized_ = false;
    if (config_.width < kMinEncodeDimension || config_.height < kMinEncodeDimension) {
        mlog::error("Encoder size too small: " + std::to_string(config_.width) + "x" +
                    std::to_string(config_.height));
        return false;
    }

    ISVCEncoder* encoder = nullptr;
    if (WelsCreateSVCEncoder(&encoder) != 0 || !encoder) {
        mlog::error("WelsCreateSVCEncoder failed");
        return false;
    }

    SEncParamExt params{};
    encoder->GetDefaultParams(&params);
    int fps = config_.fps > 0 ? config_.fps : 30;
    params.iUsageType = CAMERA_VIDEO_REAL_TIME;
    params.iPicWidth = config_.width;
    params.iPicHeight = config_.height;
    params.fMaxFrameRate = static_cast<float>(fps);
    params.iRCMode = RC_BITRATE_MODE;
    // The server's preset bitrate (e.g. 2 Mbps) is far below what a desktop
    // needs; RC then starves and fps collapses. Floor the target at ~0.05 bpp
    // (capped at 8 Mbps, fine on LAN) and never drop frames to hold it.
    long long target = std::min<long long>(
        8000000LL, std::max<long long>(config_.bitrate_kbps * 1000LL,
                                       bitrate_floor_bps(config_.width,
                                                         config_.height, fps)));
    params.iTargetBitrate = static_cast<int>(target);
    params.iMaxBitrate = static_cast<int>(target * 13 / 10);
    params.bEnableFrameSkip = false;

    params.iSpatialLayerNum = 1;
    params.iMultipleThreadIdc = kSliceCount;
    params.sSpatialLayers[0].sSliceArgument.uiSliceMode = SM_FIXEDSLCNUM_SLICE;
    params.sSpatialLayers[0].sSliceArgument.uiSliceNum = kSliceCount;
    params.uiIntraPeriod = static_cast<unsigned int>(fps) * 5;
    // Whole-frame analysis passes that buy nothing on a screen capture.
    params.bEnableBackgroundDetection = false;
    params.bEnableAdaptiveQuant = false;
    params.bEnableDenoise = false;

    if (encoder->InitializeExt(&params) != 0) {
        mlog::error("OpenH264 InitializeExt failed");
        WelsDestroySVCEncoder(encoder);
        return false;
    }

    encoder_ = encoder;
    initialized_ = true;
    keyframe_requested_ = false;
    mlog::info("OpenH264 encoder initialized (" + std::to_string(config_.width) + "x" +
               std::to_string(config_.height) + ", " + std::to_string(fps) + "fps, " +
               std::to_string(target / 1000) + "kbps, " +
               std::to_string(kSliceCount) + " slices)");
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

bool VideoEncoder::encode_frame(CapturedFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (frame.i420.empty() || frame.width <= 0 || frame.height <= 0) {
        return false;
    }
    if (!initialized_ || frame.width != config_.width ||
        frame.height != config_.height) {
        // Desktop resolution changed: follow the capturer instead of dropping
        // every frame until the agent restarts.
        config_.width = frame.width;
        config_.height = frame.height;
        if (!initialize_locked()) {
            return false;
        }
    }

    if (keyframe_requested_) {
        static_cast<ISVCEncoder*>(encoder_)->ForceIntraFrame(true);
        keyframe_requested_ = false;
    }

    const int w = frame.width;
    const int h = frame.height;
    const uint8_t* base = frame.i420.data();
    SSourcePicture pic{};
    pic.iColorFormat = videoFormatI420;
    pic.iPicWidth = w;
    pic.iPicHeight = h;
    pic.iStride[0] = w;
    pic.iStride[1] = w / 2;
    pic.iStride[2] = w / 2;
    pic.pData[0] = const_cast<uint8_t*>(base);
    pic.pData[1] = const_cast<uint8_t*>(base + static_cast<size_t>(w) * h);
    pic.pData[2] = const_cast<uint8_t*>(base + static_cast<size_t>(w) * h * 5 / 4);
    pic.uiTimeStamp = static_cast<long long>(frame.timestamp_us / 1000);

    SFrameBSInfo bs{};
    int rv = static_cast<ISVCEncoder*>(encoder_)->EncodeFrame(&pic, &bs);
    if (rv != 0 || bs.eFrameType == videoFrameTypeSkip) {
        skips_.fetch_add(1);
        return false;
    }
    frame.is_keyframe = bs.eFrameType == videoFrameTypeIDR ||
                        bs.eFrameType == videoFrameTypeI;

    size_t total = 0;
    for (int l = 0; l < bs.iLayerNum; ++l) {
        const SLayerBSInfo& layer = bs.sLayerInfo[l];
        for (int n = 0; n < layer.iNalCount; ++n) {
            total += static_cast<size_t>(layer.pNalLengthInByte[n]);
        }
    }
    frame.h264_data.clear();
    frame.h264_data.reserve(total);
    for (int l = 0; l < bs.iLayerNum; ++l) {
        const SLayerBSInfo& layer = bs.sLayerInfo[l];
        int offset = 0;
        for (int n = 0; n < layer.iNalCount; ++n) {
            int nal_len = layer.pNalLengthInByte[n];
            frame.h264_data.insert(frame.h264_data.end(), layer.pBsBuf + offset,
                                   layer.pBsBuf + offset + nal_len);
            offset += nal_len;
        }
    }
    return !frame.h264_data.empty();
}
