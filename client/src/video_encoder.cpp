#include "video_encoder.hpp"
#include <mfreadwrite.h>

VideoEncoder::VideoEncoder() {}

VideoEncoder::~VideoEncoder() {
    if (sink_writer_) {
        sink_writer_->Shutdown();
        sink_writer_->Release();
        sink_writer_ = nullptr;
    }
}

bool VideoEncoder::init_mf() {
    return SUCCEEDED(MFStartup(MF_VERSION, MFINITIALIZATION_COMPRESSED));
}

bool VideoEncoder::initialize(const EncoderConfig& config) {
    config_ = config;
    
    // Initialize Media Foundation
    if (!init_mf()) {
        std::cerr << "Failed to initialize Media Foundation" << std::endl;
        return false;
    }
    
    return setup_h264_encoder();
}

bool VideoEncoder::setup_h264_encoder() {
    HRESULT hr = MFCreateSinkWriter(&sink_writer_);
    if (FAILED(hr)) {
        std::cerr << "MFCreateSinkWriter failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }
    
    // Set input media type (ARGB32 -> H.264)
    IMFMediaType* input_type = nullptr;
    hr = MFCreateMediaType(&input_type);
    if (FAILED(hr)) goto cleanup;
    
    hr = input_type->SetGUID(MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    hr = input_type->SetGUID(MF_MT_SUBTYPE, &MFTVideoFormat_ARGB32);
    hr = input_type->SetUINT32(MF_MT_FIXED_SIZE_BITSTREAM, TRUE);
    
    UINT32 width = static_cast<UINT32>(config_.width);
    UINT32 height = static_cast<UINT32>(config_.height);
    
    hr = input_type->SetUINT32(MF_MT_FRAME_SIZE, width | (static_cast<UINT64>(height) << 32));
    
    LONG num = config_.fps;
    LONG den = 1;
    hr = input_type->SetFraction(MF_MT_FRAME_RATE, num, den);
    hr = input_type->SetFraction(MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    
    hr = input_type->SetUINT32(MF_MT_AVG_BITRATE, config_.bitrate_kbps * 1000);
    
    DWORD stream_index = 0;
    hr = sink_writer_->AddStream(input_type, &stream_index);
    if (FAILED(hr)) {
        std::cerr << "AddStream failed: 0x" << std::hex << hr << std::dec << std::endl;
        goto cleanup;
    }
    
    input_type->Release();
    input_type = nullptr;
    
    // Set encoder attributes for quality control
    IMFAttributes* output_attrs = nullptr;
    hr = MFCreateAttributes(&output_attrs, 2);
    if (SUCCEEDED(hr)) {
        hr = output_attrs->SetGUID(MF_OUTPUT_DRIVER_ATTRIBUTE_CLSID, 
                                  &CLSID_MFVideoEncoderH264);
        
        // Quality preset based on configuration
        if (_wcsicmp(config_.preset, L"UltraLowLatency") == 0) {
            hr = output_attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        }
        
        hr = output_attrs->SetUINT32(MFVIDEOENCODERHP_DESIRED_ENCODE_QUALITY, 
                                    config_.quality_level / 100.0);
        
        hr = sink_writer_->SetOutputMediaType(stream_index, output_attrs);
        output_attrs->Release();
    }
    
cleanup:
    if (input_type) input_type->Release();
    
    if (FAILED(hr)) {
        std::cerr << "Setup encoder failed: 0x" << std::hex << hr << std::dec << std::endl;
        if (sink_writer_) {
            sink_writer_->Release();
            sink_writer_ = nullptr;
        }
        return false;
    }
    
    initialized_ = true;
    stream_index_ = stream_index;
    
    return true;
}

bool VideoEncoder::encode_frame(const uint8_t* rgba_data, int width, int height, 
                                CapturedFrame& encoded_frame) {
    if (!initialized_ || !sink_writer_) {
        std::cerr << "Encoder not initialized" << std::endl;
        return false;
    }
    
    if (width != config_.width || height != config_.height) {
        std::cerr << "Resolution mismatch: expected " << config_.width << "x" 
                  << config_.height << ", got " << width << "x" << height << std::endl;
        return false;
    }
    
    // Create IMediaBuffer from RGBA data
    IMFSample* sample = nullptr;
    IMFMediaBuffer* buffer = nullptr;
    
    // Allocate buffer (add overhead)
    UINT32 data_size = width * height * 4;  // ARGB = 4 bytes per pixel
    UINT32 padding = 256;  // Buffer overhead
    
    HRESULT hr = MFCreateMemoryBuffer(data_size + padding, &buffer);
    if (FAILED(hr)) {
        std::cerr << "MFCreateMemoryBuffer failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }
    
    // Copy RGB data into buffer
    BYTE* data_ptr = nullptr;
    hr = buffer->Lock(&data_ptr, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        memcpy(data_ptr, rgba_data, data_size);
        buffer->SetCurrentLength(data_size);
        buffer->Unlock();
    } else {
        buffer->Release();
        std::cerr << "Buffer lock failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }
    
    // Create sample with buffer
    hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) {
        hr = sample->AddBuffer(buffer);
        if (FAILED(hr)) {
            sample->Release();
            buffer->Release();
            std::cerr << "AddBuffer failed: 0x" << std::hex << hr << std::dec << std::endl;
            return false;
        }
    } else {
        buffer->Release();
        std::cerr << "MFCreateSample failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }
    
    buffer->Release();
    
    // Set sample timestamp
    LARGE_INTEGER ts;
    QueryPerformanceCounter(&ts);
    sample->SetSampleTime(ts.QuadPart);
    sample->SetSampleDuration(10000000LL / config_.fps);  // 30 fps = 333ms per frame
    
    // Write sample to sink writer
    hr = sink_writer_->WriteSample(stream_index_, sample);
    
    bool success = SUCCEEDED(hr);
    if (!success) {
        std::cerr << "WriteSample failed: 0x" << std::hex << hr << std::dec << std::endl;
    }
    
    sample->Release();
    
    return success;
}

void VideoEncoder::force_keyframe() {
    // Request an I-frame (keyframe)
    if (sink_writer_) {
        IMFAttributes* attrs = nullptr;
        if (SUCCEEDED(MFCreateAttributes(&attrs, 1))) {
            attrs->SetUINT32(MFTRANSFORM_FORCE_KEYFRAME, TRUE);
            
            // This would typically be done through an internal mechanism
            // For now, we just note that keyframes are requested periodically
            attrs->Release();
        }
    }
}
