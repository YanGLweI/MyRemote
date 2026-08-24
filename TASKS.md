# 开发任务清单 - MyRemote Control

## ✅ 已完成 (Phase 1: 框架搭建)

### 基础设施
- [x] CMakeLists.txt 构建配置
- [x] 项目目录结构建立
- [x] 公共库模块框架

### 核心网络模块
- [x] `connection.cpp` - 客户端主动连接实现
- [x] `listener.cpp` - 服务端 TCP 监听器  
- [x] `heartbeat.cpp` - 心跳保活机制
- [x] `auto_reconnect.cpp` - 断线重连逻辑

### 加密安全
- [x] `aes_gcm.cpp/.hpp` - AES-128-GCM 加密实现
- [x] `ecdh.cpp/.hpp` - ECDH 密钥交换实现

### GUI 框架 (服务端)
- [x] Qt6 主窗口骨架
- [x] DeviceListWidget 基础类
- [x] DisplayRenderer OpenGL Widget 框架

### 文档
- [x] README.md 项目说明
- [x] DEVELOPMENT.md 详细开发指南
- [x] build.bat Windows 构建脚本

---

## 🚧 进行中/待完成 (Phase 2: 核心功能完善)

### P0 - 必须实现的关键功能

#### 1. DesktopCapturer (高危优先级) ⭐⭐⭐
**文件**: `client/src/desktop_capture.cpp`
**依赖头文件**: `desktop_capture.hpp`

**需实现内容**:
```cpp
bool DesktopCapturer::initialize(int monitor_index) {
    // Step 1: Create D3D11 device
    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    
    HRESULT hr = D3D11CreateDevice(
        nullptr,                          // Default adapter
        D3D_DRIVER_TYPE_HARDWARE,         // Hardware accelerated
        nullptr,                          // No software renderer
        0,                                // No flags
        feature_levels,                   // Feature levels
        ARRAYSIZE(feature_levels),
        D3D11_SDK_VERSION,
        &d3d_device_,                     // Output device
        nullptr,                          // Feature level out
        &immediate_ctx_                   // Context output
    );
    
    if (FAILED(hr)) return false;
    
    // Step 2: Get IDXGIDevice and enumerate outputs
    dxgi_device_.as(&dxgi_device_);
    IDXGIAdapter* adapter = nullptr;
    dxgi_device_->GetAdapter(&adapter);
    
    IDXGIOutput* output = nullptr;
    adapter->EnumOutputs(monitor_index, &output);
    adapter->Release();
    
    // Step 3: Query for IDXCIOutput1 (needed for duplication)
    output->QueryInterface(IID_PPV_ARGS(&output_));
    output->Release();
    
    if (!output_) return false;
    
    // Step 4: Duplicate the output
    hr = output_->DuplicateOutput1(d3d_device_.Get(), 0, 
                                   IID_PPV_ARGS(&duplication_));
    return SUCCEEDED(hr);
}
```

**测试点**:
- [ ] Win7 compatibility (should fail gracefully, fallback to BitBlt)
- [ ] Multi-monitor support
- [ ] Remote desktop session detection
- [ ] Gaming fullscreen mode (DWM composition off)

**替代方案**: 
如果 DXGI 不可用，实现 BitBlt 降级路径:
```cpp
// client/src/desktop_capture_bitblt.cpp
bool DesktopCapturer::capture_with_bitblt(CapturedFrame& frame) {
    HDC hdc_screen = GetDC(nullptr);
    HDC hdc_mem = CreateCompatibleDC(hdc_screen);
    
    // Create bitmap buffer
    BITMAPINFOHEADER bih{sizeof(BITMAPINFOHEADER), width, -height};
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    
    void* pixels = nullptr;
    HBITMAP hbmp = CreateDIBSection(hdc_mem, 
                                    reinterpret_cast<BITMAPINFO*>(&bih), 
                                    DIB_RGB_COLORS, &pixels, nullptr, 0);
    
    SelectObject(hdc_mem, hbmp);
    BitBlt(hdc_mem, 0, 0, width, height, hdc_screen, 0, 0, SRCCOPY);
    
    // Pass RGBA buffer to encoder
    encode_rgba(static_cast<uint8_t*>(pixels), frame);
    
    DeleteDC(hdc_mem);
    ReleaseDC(nullptr, hdc_screen);
    DeleteObject(hbmp);
    
    return true;
}
```

#### 2. VideoEncoder (高危优先级) ⭐⭐⭐
**文件**: `client/src/video_encoder.cpp`
**依赖头文件**: `video_encoder.hpp`

**需实现内容**:
```cpp
bool VideoEncoder::initialize(const EncoderConfig& config) {
    config_ = config;
    
    // Initialize Media Foundation
    HRESULT hr = MFInitialize(MF_VERSION, MFINITIALIZATION_COMPRESSED);
    if (FAILED(hr)) {
        init_mf();  // Lazy initialization
    }
    
    return setup_h264_encoder();
}

bool VideoEncoder::setup_h264_encoder() {
    // Step 1: Create sink writer
    IMFMediaType* input_type = nullptr;
    IMFMediaType* output_type = nullptr;
    
    hr = MFCreateMediaType(&input_type);
    hr = MFCreateMediaType(&output_type);
    
    // Set video format (ARGB32 -> H.264)
    hr = input_type->SetGUID(MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    hr = input_type->SetGUID(MF_MT_SUBTYPE, &MFTVideoFormat_ARGB32);
    hr = input_type->SetUINT32(MF_MT_FIXED_SIZE_BITSTREAM, 1);
    
    UINT32 width = config_.width;
    UINT32 height = config_.height;
    hr = input_type->SetUINT32(MF_MT_FRAME_SIZE, width | (height << 32));
    
    // Frame rate: 30 fps
    LONG num = 30, den = 1;
    hr = input_type->SetFraction(MF_MT_FRAME_RATE, num, den);
    hr = input_type->SetFraction(MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    
    // Create video stream on sink writer
    IMFAttributes* attributes = nullptr;
    MFCreateAttributes(&attributes, 1);
    
    hr = sink_writer_->AddStream(output_type, &stream_index_);
    
    // Step 2: Configure encoder attributes
    attributes->SetGUID(MF_TRANSFORM_ATTR_CLSID, &CLSID_MFVideoEncoderH264);
    
    // Quality/Latency preset based on mode
    if (config_.preset == L"UltraLowLatency") {
        attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
    }
    
    // Target bitrate
    hr = sink_writer_->SetOutputMediaType(stream_index_, output_type);
    
    // Step 3: Begin writing
    hr = sink_writer_->BeginWriting();
    return SUCCEEDED(hr);
}
```

**关键 API**:
- `IMFSinkWriter` - Primary encoding interface
- `MFCreateSinkWriterFromURL()` / `MFCreateSinkWriterFromOutput()`
- `MFGetService()` - Retrieve encoder MFT

**降级策略**:
如果硬件编码器失败:
1. Try NVENC (NVIDIA GPU) → `CLSID_NVH264Enc_hw`
2. Try AMD VCE → `CLSID_AMDHWVIDEOCODEC_H264ENCODER`
3. Fallback to x264 software encoder (slower but universal)

#### 3. Protocol Buffers Wiring ⭐⭐
**集成步骤**:
1. Download protobuf compiler: https://github.com/protocolbuffers/protobuf/releases
2. Compile protocol.proto:
   ```bash
   protoc --cpp_out=./src/ common/include/protocol.proto
   ```
3. Update `CMakeLists.txt`:
   ```cmake
   find_package(Protobuf REQUIRED)
   include_directories(${PROTOBUF_INCLUDE_DIRS})
   
   add_library(common_lib STATIC
       common/src/protocol.pb.cc
       ...
   )
   target_link_libraries(common_lib ${PROTOBUF_LIBRARIES})
   ```

**消息处理示例**:
```cpp
class MessageCodec {
public:
    // Encode message to encrypted bytes
    std::vector<uint8_t> encode_and_encrypt(const ClientHello& msg, 
                                            const std::array<uint8_t, 16>& key) {
        // Serialize
        std::string str;
        msg.SerializeToString(&str);
        
        // Encrypt
        AESGCM crypto(key);
        auto plaintext = std::vector<uint8_t>(str.begin(), str.end());
        return crypto.encrypt(plaintext);
    }
    
    // Decode decrypted bytes into message object
    bool decode_and_decrypt(const std::vector<uint8_t>& encrypted,
                           ServerHello& msg,
                           const std::array<uint8_t, 16>& key) {
        AESGCM crypto(key);
        try {
            auto decrypted = crypto.decrypt(encrypted);
            
            std::string str(decrypted.begin(), decrypted.end());
            return msg.ParseFromString(str);
        } catch (...) {
            return false;
        }
    }
};
```

#### 4. Input Simulator Full Implementation ⭐⭐
**文件**: `client/src/input_simulator.cpp`

**需实现鼠标事件**:
```cpp
void InputSimulator::simulate_mouse(const mouse::Event& event) {
    INPUT inputs[2];
    ZeroMemory(inputs, sizeof(INPUT));
    
    DWORD dwFlags = 0;
    
    if (event.pressed) {
        switch (event.button) {
            case LEFT_BUTTON:
                inputs[0].type = INPUT_MOUSE;
                inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                
                Sleep(50);  // Simulate button press duration
                
                inputs[1].type = INPUT_MOUSE;
                inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
                
                SendInput(2, inputs, sizeof(INPUT));
                break;
                
            case RIGHT_BUTTON:
                inputs[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
                inputs[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
                SendInput(2, inputs, sizeof(INPUT));
                break;
                
            default:
                return;  // Unsupported button
        }
    } else {
        // Handle button release only
        inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTUP;  // Or appropriate flag
        SendInput(1, inputs, sizeof(INPUT));
    }
    
    // Move cursor (absolute coordinates: 0-65535 range)
    if (event.cursor_x != 0 || event.cursor_y != 0) {
        ZeroMemory(inputs, sizeof(INPUT));
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        inputs[0].mi.dx = event.cursor_x * 65535 / screen_width_;
        inputs[0].mi.dy = event.cursor_y * 65535 / screen_height_;
        
        SendInput(1, inputs, sizeof(INPUT));
    }
}
```

**键盘事件**:
```cpp
void InputSimulator::simulate_keyboard(const keyboard::Event& event) {
    INPUT inputs[1];
    ZeroMemory(inputs, sizeof(INPUT));
    
    UINT scanCode = MapVirtualKey(event.virtual_key, MAPVK_VK_TO_VSC);
    
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = static_cast<WORD>(event.virtual_key);
    inputs[0].ki.wScan = static_cast<WORD>(scanCode);
    
    if (event.type == KEY_PRESS) {
        inputs[0].ki.dwFlags = 0;
    } else {
        inputs[0].ki.dwFlags = KEYEVENTF_KEYUP;
    }
    
    if (event.extended) {
        inputs[0].ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    
    SendInput(1, inputs, sizeof(INPUT));
}
```

#### 5. TunnelManager Full Implementation ⭐⭐
**文件**: `server/src/tunnel_manager.cpp`

需完整实现所有连接池管理方法，包括:
- Client registration with device info extraction
- Active/inactive state tracking
- Graceful disconnect handling
- Broadcast mechanism

### P1 - 优化与增强功能

#### 实时桌面渲染 ⭐⭐
**文件**: `server/src/display_renderer.cpp`

**使用 FFmpeg libavcodec 解码 H.264**:
```cpp
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

void DisplayRenderer::render_frame(const uint8_t* h264_data, size_t len) {
    AVPacket pkt;
    av_packet_from_data(&pkt, (uint8_t*)h264_data, len);
    
    int ret = avcodec_send_packet(codec_ctx_, &pkt);
    if (ret < 0) {
        std::cerr << "Failed to send packet: " << ret << std::endl;
        return;
    }
    
    AVFrame* frame = av_frame_alloc();
    ret = avcodec_receive_frame(codec_ctx_, frame);
    if (ret < 0) {
        av_frame_free(&frame);
        return;  // No frame available yet
    }
    
    // Convert YUV to RGB and upload texture
    convert_and_upload(frame);
    
    av_frame_free(&frame);
}
```

**FFmpeg Setup Code**:
```cpp
// Call once during initialization
av_register_all();
AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
codec_ctx_ = avcodec_alloc_context3(codec);
avcodec_open2(codec_ctx_, codec, nullptr);
```

#### Qt UI Enhancement ⭐
- Connect device list selection to remote control trigger
- Add quality mode dropdown selector
- Show FPS/bitrate statistics overlay
- Implement mouse capture widget (disable cursor from leaving window)

### P2 - 拓展功能 (可选)

- [ ] File transfer capability
- [ ] Clipboard synchronization  
- [ ] Chat messaging between endpoints
- [ ] Session recording
- [ ] Multiple monitors support

---

## 📋 测试计划

### Unit Tests
- [ ] Connection connect/disconnect cycle
- [ ] Encryption/Decryption roundtrip
- [ ] Heartbeat timer accuracy
- [ ] Auto-reconnect exponential backoff

### Integration Tests
- [ ] End-to-end: Client connects → sends frames → Server receives
- [ ] Mouse/keyboard events correctly inject
- [ ] Quality presets effect verification

### Performance Tests
- [ ] 1920×1080 @ 30fps sustained (CPU usage target < 40%)
- [ ] 2560×1440 @ 60fps stress test (max 2 hours continuous)
- [ ] Network latency measurement (ping to server IP + RTT)
- [ ] Concurrent connection test (50+ clients simulated)

### Stability Tests
- [ ] Memory leak detection (Valgrind/Visual Leak Detector)
- [ ] Long-running server (72h+ without restart)
- [ ] Rapid connect/disconnect cycles (stress network layer)

---

## 🔧 工具链准备

### Required SDKs
1. **Windows 10/11 SDK** (for DX11/Media Foundation APIs)
   - Download: https://developer.microsoft.com/en-us/windows/downloads/windows-sdk/
   
2. **Qt 6.x MSVC 版本**
   - Download: https://download.qt.io/official_releases/qt/
   - Install components: Qt6.5 MSVC2019_64

3. **OpenSSL 1.1.1** (for encryption)
   - Prebuilt binaries: https://wiki.openssl.org/index.php/Binaries
   - Or compile from source (recommended for custom builds)

4. **Protocol Buffers 21.0+**
   - Download: https://github.com/protocolbuffers/protobuf/releases
   - Extract and add bin/ to PATH

5. **FFmpeg Libraries** (optional, for decoder)
   - Build or download: https://www.gyan.dev/ffmpeg/builds/

### Development Environment Checklist
- Visual Studio 2022 installed with "Desktop development with C++" workload
- CMake 3.16+ configured in VS settings
- Windows Debugger set up
- Optional: Git LFS for binary assets

---

## 💡 下一步行动建议

**立即开始 (本周)**:
1. Complete `DesktopCapturer` implementation using DXGI
2. Integrate `VideoEncoder` with Media Foundation
3. Wire up Protocol Buffers messages end-to-end

**下一周**:
1. Implement InputSimulator with real test
2. Build complete data path: Capture→Encode→Send→Decode→Render
3. Add basic performance monitoring

**第二周**:
1. Polish Qt GUI with real-time updates
2. Add quality mode switching
3. Conduct load testing

**第三周**:
1. Debug and optimize performance bottlenecks
2. Implement remaining P2 features (optional)
3. Prepare first beta release

---

如需任何技术细节讨论或遇到实现障碍，请及时提交 Issue 或联系团队!
