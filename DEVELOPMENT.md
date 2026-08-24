# 开发指南 - MyRemote Control

本文档详细说明各个模块的实现细节、扩展方法和优化策略。

## 目录结构详解

### 公共库 (`common/`)

#### `aes_gcm.hpp/.cpp` - AES-128-GCM 加密模块
```cpp
// 使用示例:
#include "aes_gcm.hpp"
#include <array>

int main() {
    std::array<uint8_t, 16> key = {};  // 填充密钥
    
    AESGCM encryptor(key);
    std::vector<uint8_t> plaintext = {'H', 'e', 'l', 'l', 'o'};
    
    auto encrypted = encryptor.encrypt(plaintext);
    // 输出：nonce[12] | ciphertext | mac[16]
    
    AESGCM decryptor(key);
    auto decrypted = decryptor.decrypt(encrypted);
}
```

#### `ecdh.hpp/.cpp` - ECDH 密钥交换
```cpp
ECDHExchange alice;
alice.generate_keys();
auto public_key = alice.get_public_key();

ECDHExchange bob;
bob.generate_keys();
bob.set_peer_public_key(public_key);

// Derive same AES key on both sides
auto shared = alice.derive_shared_secret();  // Alice
auto shared = bob.derive_shared_secret();     // Bob (same)
```

#### `protocol.proto` - 通信协议定义
Protocol Buffers IDL 文件。编译命令:
```bash
protoc --cpp_out=./src/ common/include/protocol.proto
```

生成的头文件: `protocol.pb.h`  
生成的源文件：`protocol.pb.cc`

### 客户端模块 (`client/`)

#### `connection.hpp/.cpp` - 主动连接管理器

**线程模型**:
```
MainThread -> connect() -> start_recv_thread()
                           recv_thread_ -> receive_loop()
                                              while connected:
                                                  recv(socket)
                                                  process_message()
```

**关键 API**:
```cpp
Connection conn;
bool ok = conn.connect("192.168.1.100", 7500);

conn.set_receive_callback([&](const std::vector<uint8_t>& data) {
    // Parse and handle incoming message
    parse_encrypted_message(data);
});

// Wait for connection to close
std::this_thread::sleep_for(std::chrono::seconds(30));
```

#### `desktop_capture.hpp/.cpp` - DXGI Desktop Duplication

**完整实现步骤**（需补充）:
```cpp
DesktopCapturer capturer;

if (!capturer.initialize()) {
    throw std::runtime_error("Failed to init");
}

EncoderConfig cfg;
cfg.fps = 30;
cfg.width = 1920;
cfg.height = 1080;
capturer.configure(cfg);

while (true) {
    CapturedFrame frame;
    if (capturer.capture_frame(frame)) {
        // Send frame to server via connection
        send_to_server(frame.h264_data);
    }
    Sleep(1000 / 30);  // 30fps limit
}
```

**DXGI Duplication 内部流程**:
1. Create D3D11 device → `D3D11CreateDevice()`
2. Get IDXGIOutput1 from monitor → `EnumOutputs()`
3. Duplicate output → `IDXGIOutput1::DuplicateOutput()`
4. Acquire frame → `IDirectXGIOutputDuplication::AcquireNextFrame()`
5. Lock desktop surface → `Map()`
6. Copy to texture → `CopyResource()`
7. Encode H.264 → Media Foundation
8. Free resource → `ReleaseFrame()`

**降级方案**: 如果 DXGI 失败，回退到 BitBlt:
```cpp
bool DesktopCapturer::capture_with_bitblt(CapturedFrame& frame) {
    HDC hdc_screen = GetDC(nullptr);
    HDC hdc_mem = CreateCompatibleDC(hdc_screen);
    
    BITMAPINFO bmi{
        .bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32}
    };
    
    void* pixels;
    HBITMAP hbmp = CreateDIBSection(hdc_mem, &bmi, DIB_RGB_COLORS, 
                                    &pixels, nullptr, 0);
    SelectObject(hdc_mem, hbmp);
    BitBlt(hdc_mem, 0, 0, width, height, hdc_screen, 0, 0, SRCCOPY);
    
    ReleaseDC(nullptr, hdc_screen);
    
    // Pass RGBA buffer to encoder
    encode_rgba(pixels, frame);
    
    DeleteDC(hdc_mem);
    DeleteObject(hbmp);
}
```

#### `input_simulator.hpp/.cpp` - Windows 输入注入

**实现逻辑**:
```cpp
void InputSimulator::simulate_mouse(const mouse::Event& event) {
    DWORD dwFlags = 0;
    
    if (event.pressed) {
        switch (event.button) {
            case LEFT_BUTTON:
                dwFlags = MOUSEEVENTF_LEFTDOWN;
                break;
            case RIGHT_BUTTON:
                dwFlags = MOUSEEVENTF_RIGHTDOWN;
                break;
            default:
                return;
        }
        
        SendInput(1, &input_event, sizeof(INPUT));
        Sleep(50);  // Simulate hold time
        
        input_event.type = INPUT_MOUSE;
        input_event.mi.dwFlags = MOUSEEVENTF_LEFTUP;  // Release
        SendInput(1, &input_event, sizeof(INPUT));
    } else {
        // Handle button release directly
    }
    
    // Move cursor
    if (dx != 0 || dy != 0) {
        input_event.type = INPUT_MOUSE;
        input_event.mi.dx = dx;
        input_event.mi.dy = dy;
        input_event.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        SendInput(1, &input_event, sizeof(INPUT));
    }
}
```

**键盘事件模拟**:
```cpp
void InputSimulator::simulate_keyboard(const keyboard::Event& event) {
    UINT scanCode = MapVirtualKey(event.virtual_key, MAPVK_VK_TO_VSC);
    
    INPUT inputs[2];
    ZeroMemory(inputs, sizeof(inputs));
    
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = event.virtual_key;
    inputs[0].ki.wScan = scanCode;
    inputs[0].ki.dwFlags = event.type == KEY_PRESS ? 0 : KEYEVENTF_KEYUP;
    
    // Extended key handling
    if (event.extended) {
        inputs[0].ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    
    SendInput(2, inputs, sizeof(inputs));
}
```

### 服务端模块 (`server/`)

#### `tunnel_manager.cpp` - 连接池管理

```cpp
TunnelManager::TunnelManager() {
    listener_.set_connected_callback([this](SOCKET socket) {
        register_client(socket, "");  // Will be filled with hello message
    });
    
    listener_.set_disconnected_callback([this](SOCKET socket) {
        unregister_client(socket);
    });
}

SOCKET TunnelManager::register_client(SOCKET socket, const std::string& device_id) {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    auto session = std::make_shared<ClientSession>();
    session->socket = socket;
    session->device_id = device_id;
    session->active = true;
    session->connect_time = time(nullptr);
    
    client_pool_[socket] = session;
    device_id_to_socket_[device_id] = socket;
    
    client_count_++;
    return socket;
}

std::vector<std::shared_ptr<ClientSession>> TunnelManager::get_all_clients() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    std::vector<std::shared_ptr<ClientSession>> result;
    
    for (const auto& [sock, session] : client_pool_) {
        if (session->active) {
            result.push_back(session);
        }
    }
    
    return result;
}
```

#### `display_renderer.cpp` - OpenGL 桌面渲染

**关键实现**:
```cpp
void DisplayRenderer::render_frame(const uint8_t* h264_data, size_t size) {
    GLuint tex_id;
    
    if (decode_h264_frame(h264_data, size, tex_id)) {
        // Upload decoded YUV/RGB texture
        QOpenGLFramebufferObject fbo(size_width_, size_height_);
        
        bind();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex_id);
        draw_texture_quad();
        
        last_fps_update_ = std::chrono::steady_clock::now();
        update_fps();
    }
}

bool DisplayRenderer::decode_h264_frame(const uint8_t* data, size_t size, 
                                        GLuint& tex_out) {
    // Use FFmpeg libavcodec decoder
    AVPacket pkt;
    av_packet_from_data(&pkt, (uint8_t*)data, size);
    
    int ret = avcodec_send_packet(codec_ctx_, &pkt);
    if (ret < 0) return false;
    
    AVFrame* frame = av_frame_alloc();
    ret = avcodec_receive_frame(codec_ctx_, frame);
    if (ret < 0) return false;
    
    // Convert YUV to RGB and upload to OpenGL texture
    convert_yuv_to_rgb(frame, tex_out);
    
    av_frame_free(&frame);
    return true;
}
```

**FFmpeg 集成要点**:
1. Add `libavcodec`, `libavformat`, `libswscale` libraries
2. Initialize codec context: `avcodec_find_decoder(AV_CODEC_ID_H264)`
3. Parse H.264 NALU units (Annex B format)
4. Decode I/P/B frames correctly

### Protocol Buffers 消息处理

**编解码示例**:
```cpp
class MessageCodec {
public:
    std::vector<uint8_t> encode(const ClientHello& msg) {
        std::vector<uint8_t> buffer;
        auto bytes = msg.SerializeToString();
        buffer.insert(buffer.end(), bytes.begin(), bytes.end());
        return buffer;
    }
    
    bool decode(const std::vector<uint8_t>& data, ServerHello& msg) {
        return msg.ParseFromArray(data.data(), data.size());
    }
};

// Usage in connection callback:
client.set_receive_callback([&](const std::vector<uint8_t>& data) {
    ServerHello response;
    if (message_codec.decode(data, response)) {
        // Process response
    }
});
```

## 性能优化策略

### 1. 降低延迟技巧
- **零拷贝**: DXGI texture → Encoder 输入直接传递指针
- **双缓冲**: Capture thread encodes while render thread displays
- **低延迟 preset**: Use `ULTRA_LOW_LATENCY` encoding preset
- **减少 GOP**: GOP size 2-5 seconds for fast seek/response

### 2. 提升帧率
- **垂直同步关闭**: Disable VSync in DXGI swap chain
- **异步队列**: Separate capture/encode/transmit threads
- **GPU memory pooling**: Reuse DirectX textures

### 3. 内存管理
- **智能指针**: Use `ComPtr` for COM interfaces
- **RAII wrapper**: Wrap raw sockets with RAII class
- **Static allocators**: Avoid frequent heap allocations

## 调试技巧

### 网络抓包分析
```bash
# Wireshark filter for port 7500
tcp.port == 7500

# Check encryption overhead
stats > io > graph > windowing
```

### 性能 profiling
Visual Studio Profiler setup:
```xml
<!-- .vsproj file configuration -->
<PropertyGroup>
  <LocalDebuggerCommandLine>agent.exe</LocalDebuggerCommandLine>
  <DebuggerFlavor>WindowsLocalDebugger</DebuggerFlower>
</PropertyGroup>

// Then Profile menu → Launch performance wizard
```

## 常见陷阱与解决方案

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| DXGI returns ERROR_INVALID_CALL | Direct3D not initialized first | Call `D3D11CreateDevice()` before any DXGI operations |
| Frame drops at high resolution | Encoder cannot keep up | Drop frames selectively (every Nth frame) or lower quality |
| Audio sync issues | No audio stream yet | Focus on video only (simpler MVP scope) |
| UI freezes during remote control | Blocking network send | Use async/await pattern or fire-and-forget threads |
| Memory leak in Win32 Service | Forgot to call WSAleanup() | Always pair initialization/finalization calls |

## 测试用例

### Unit Test Example
```cpp
TEST(AESGCMTesetBasicEncryptDecrypt) {
    std::array<uint8_t, 16> key{};
    key.fill('x');
    
    AESGCM crypto(key);
    std::vector<uint8_t> original = {'A', 'd', 'v', 'a', 'n', 'c', 'e'};
    
    auto encrypted = crypto.encrypt(original);
    auto decrypted = crypto.decrypt(encrypted);
    
    EXPECT_EQ(original, decrypted);
}

TEST(ConnectionTestConnectivity) {
    Connection c1, c2;
    Listener listener;
    listener.start(7501);
    
    EXPECT_TRUE(c1.connect("127.0.0.1", 7501));
    EXPECT_TRUE(c1.is_connected());
    
    Sleep(1000);
    listener.stop();
    
    EXPECT_FALSE(c1.is_connected());
}
```

### Integration Test
Simulate isolated network scenario:
```python
#!/usr/bin/env python3
import subprocess
import time

# Start server
server_proc = subprocess.Popen(["./control_server"])
time.sleep(2)

# Start client
client_proc = subprocess.Popen(["./agent"], env={"SERVER_IP": "host.docker.internal"})
time.sleep(5)

# Verify connection established
time.sleep(10)  # Allow heartbeat cycle

# Kill network between host and container
subprocess.run(["tc", "qdisc", "add", "dev", "eth0", "root", "netem", "rate", "0bit"])

# Client should reconnect automatically
time.sleep(10)
subprocess.run(["tc", "qdisc", "del", "dev", "eth0"])

# Monitor logs for reconnection success
```

## 下一步开发任务清单

### P0: 必须功能
- [ ] DesktopCapturer implementation completion
- [ ] VideoEncoder MediaFoundation integration
- [ ] Protocol encoder/decoder wiring
- [ ] Display renderer real-time playback

### P1: 优化功能
- [ ] Quality mode selector (low_latency/balanced/high_quality)
- [ ] FPS/bitrate monitoring widget
- [ ] Mouse/keyboard event forwarding
- [ ] Cursor position overlay

### P2: 拓展功能
- [ ] Multi-monitor support
- [ ] Clipboard synchronization
- [ ] File transfer capability
- [ ] Chat messaging

---

如有任何技术问题，请提交 Issue 或联系开发团队。
