#pragma once

#include "desktop_capture.hpp"
#include <mfapi.h>
#include <mfreadwrite.h>
#include <mfidl.h>
#include <mediaformateutils.h>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mfreadwrite.lib")

// Media Foundation encoder wrapper for H.264
class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();
    
    // Initialize encoder with configuration
    bool initialize(const EncoderConfig& config);
    
    // Encode a frame (RGB32 texture to H.264)
    bool encode_frame(const uint8_t* rgba_data, int width, int height, 
                     CapturedFrame& encoded_frame);
    
    // Force keyframe generation
    void force_keyframe();
    
private:
    IMFSinkWriter* sink_writer_ = nullptr;
    DWORD stream_index_ = 0;
    EncoderConfig config_;
    bool initialized_ = false;
    
    // Initialize Media Foundation
    static bool init_mf();
    
    // Set up H.264 encoding session
    bool setup_h264_encoder();
};
