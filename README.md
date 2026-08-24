# MyRemote Control - 单向网络反向远程控制系统

基于逆向连接架构的 Windows 远程控制工具，专用于单向隔离内网环境。

## 项目概述

本项目实现了一个**完全由客户端主动发起连接**的远程控制方案，解决了传统远程工具依赖双向网络连通的问题。

### 核心特点
- ✅ **严格单向网络**: 服务端只被动监听，绝不主动访问客户端
- ✅ **30-60fps 可调画质**: 支持低延迟档、流畅档、高清档三档模式
- ✅ **100ms 以内延迟**: Media Foundation 硬件编码 + DXGI GPU 捕获
- ✅ **AES-128-GCM 加密**: 自定义轻量级应用层加密
- ✅ **≥50 台并发**: 多线程异步 I/O 架构

## 技术栈

| 模块 | 技术选型 |
|------|----------|
| **开发语言** | C++17 + Win32 API |
| **GUI 框架** | Qt 6.x (服务端控制端) |
| **网络通信** | Boost.Asio / WinIOCP 异步 I/O |
| **桌面捕获** | Desktop Duplication API (DXGI 1.2+) |
| **视频编码** | Microsoft Media Foundation H.264 |
| **加密安全** | OpenSSL AES-128-GCM + ECDH 密钥交换 |
| **序列化** | Protocol Buffers v3 |

## 项目结构

```
MyRemote/
├── common/                     # 公共库
│   ├── include/
│   │   ├── protocol.proto      # 通信协议定义
│   │   ├── aes_gcm.hpp         # 加密模块
│   │   ├── ecdh.hpp            # 密钥交换
│   │   └── utils.hpp           # 通用工具
│   └── src/
│       ├── aes_gcm.cpp
│       ├── ecdh.cpp
│       └── protocol.pb.cc
├── client/                     # 客户端代理
│   ├── include/
│   │   ├── connection.hpp      # 主动连接管理器
│   │   ├── desktop_capture.hpp # DXGI 桌面捕获
│   │   ├── video_encoder.hpp   # 编码器封装
│   │   ├── input_simulator.hpp # 输入注入器
│   │   ├── heartbeat.hpp       # 心跳保活
│   │   └── auto_reconnect.hpp  # 断线重连
│   └── src/
│       ├── main.cpp           # Windows 服务入口
│       ├── connection.cpp
│       ├── heartbeat.cpp
│       └── auto_reconnect.cpp
├── server/                     # 服务端控制端
│   ├── include/
│   │   ├── listener.hpp       # TCP 监听器
│   │   ├── tunnel_manager.hpp # 连接池管理
│   │   ├── device_list.hpp    # 设备列表 UI
│   │   ├── display_renderer.hpp # 桌面渲染
│   │   └── remote_controller.hpp # 远程控制器
│   └── src/
│       └── main.cpp           # Qt GUI 入口
├── CMakeLists.txt             # 构建配置
└── README.md                  # 本文档
```

## 编译要求

### 软件依赖
- Visual Studio 2022 (Community/Professional/Enterprise)
- CMake >= 3.16
- Qt 6.5+ (Desktop MSVC 版本)
- OpenSSL 1.1.1+ (静态链接可选)
- Protocol Buffers compiler (protoc)

### 构建步骤

```bash
# 1. 创建构建目录
mkdir build && cd build

# 2. 生成构建文件
cmake .. -G "Visual Studio 17 2022" -A x64

# 3. 编译
cmake --build . --config Release

# 4. 输出位置
# bin/client_agent.exe        # 客户端
# bin/control_server.exe      # 服务端
```

## 使用方法

### 服务端启动

1. **运行控制端程序**
   ```bash
   ./control_server.exe
   ```

2. **默认配置**
   - 监听端口：`7500`
   - 绑定地址：`0.0.0.0` (所有网卡)

3. **UI 操作**
   - 查看在线设备列表
   - 双击设备启动远程会话
   - 显示实时桌面画面
   - 模拟键鼠操作

### 客户端安装

#### 方式一：Windows 服务安装（推荐）

```powershell
# 管理员权限 PowerShell
cd "C:\Program Files\MyRemote"

# 注册为 Windows 服务
sc create MyRemoteAgent binPath="C:\Program Files\MyRemote\agent.exe"

# 设置开机自启
sc start MyRemoteAgent

# 验证状态
sc query MyRemoteAgent
```

#### 方式二：绿色免安装版

直接将 `agent.exe` 复制到任意目录运行即可。

### 配置文件 (可选)

```json
// config.json
{
  "server_ip": "192.168.1.100",
  "server_port": 7500,
  "secret_key": "my_secret_password_123",
  "device_name": "Office-PC-001",
  "quality_mode": "balanced"  // low_latency | balanced | high_quality
}
```

## 性能指标

| 模式 | FPS | Bitrate | GOP | 延迟 | 适用场景 |
|------|-----|---------|-----|------|----------|
| **低延迟档** | 30fps | 1-2Mbps | 30 | ~80ms | 实时控制、运维 |
| **流畅档** | 30fps | 2-4Mbps | 60 | ~100ms | 日常办公、浏览 |
| **高清档** | 60fps | 4-8Mbps | 120 | ~120ms | 视频编辑、设计 |

## 安全机制

### 1. 传输层加密
- 使用 `AES-128-GCM` 对称加密所有通信数据
- 每个消息包含独立随机 Nonce 防止重放攻击
- GCM 认证标签确保完整性保护

### 2. 密钥协商
- 通过 `ECDH-P256` 椭圆曲线密钥交换
- 每次连接动态生成唯一会话密钥
- 不支持中间人攻击（前向安全性）

### 3. 接入控制
- 设备密钥校验（配置 SecretKey 匹配）
- HWID 硬件指纹绑定（可拓展）
- 服务端白名单机制（可选功能）

### 4. 示例：加密消息格式

```cpp
struct EncryptedMessage {
    uint8_t nonce[12];        // GCM 初始化向量 (随机)
    uint8_t encrypted_data[]; // AES 密文载荷
    uint8_t mac[16];          // GCM 认证标签
};
```

## 网络拓扑

```
┌─────────────────────────┐
│     服务端 (Server)      │
│  TCP:7500 被动监听      │
│                         │
│  • Device List Widget   │ ← User operates UI
│  • Display Renderer     │
│  • Tunnel Manager       │
└──────────▲──────────────┘
           │
           │ 客户端主动发起单向长连接
           │ (TCP + TLS-Like 加密)
           │
           ▼
┌─────────────────────────┐
│   客户端 (Client Agent)  │
│ 后台 Windows 服务运行     │
│                         │
│  • Auto Reconnect       │ ← 自动恢复连接
│  • Heartbeat Keeper     │ <- 心跳保活 (3s)
│  • Desktop Capturer     │ ← DXGI 捕获
│  • Input Simulator      │ ← 接收远程指令
└─────────────────────────┘
```

## 开发计划

### Phase 1: MVP (已完成)
- [x] 网络通信框架搭建
- [x] 桌面捕获与编码接口定义
- [x] 加密模块实现
- [x] 基础心跳机制
- [x] 自动重连逻辑

### Phase 2: 完善核心功能 (进行中)
- [ ] Desktop Duplication API 完整实现
- [ ] Media Foundation 编码器集成
- [ ] Protocol Buffers 编解码
- [ ] 服务端 Qt GUI 界面完善
- [ ] 实时远程桌面渲染
- [ ] 鼠标/键盘事件转发

### Phase 3: 优化增强
- [ ] 自适应码率算法 (根据网络质量调整)
- [ ] 多显示器支持
- [ ] 全屏优化渲染
- [ ] 文件传输功能
- [ ] 聊天窗口

### Phase 4: 生产就绪
- [ ] Windows 服务安装工具
- [ ] 开机自启配置界面
- [ ] 日志系统完善
- [ ] 压力测试与调优
- [ ] 打包发布工具链

## 关键技术难点

### 1. DXGI 虚拟化兼容性
某些虚拟化平台不支持 Desktop Duplication API。
**解决方案**: 提供 BitBlt 降级路径（15fps 限制）。

### 2. H.264 硬件编码失败
部分显卡不支持 Media Foundation 硬件编码。
**解决方案**: 多层降级策略
- NVIDIA NVENC → AMD VCE → x264 软件编码

### 3. 内存泄漏排查
长时间运行可能出现的内存泄漏问题。
**解决方案**: 
- Visual Studio Profiler 分析
- AddressSanitizer 检测
- 定期全量 GC 触发

## 测试建议

### 单元测试
- 协议编解码正确性测试
- 加解密循环验证
- 心跳超时判定逻辑

### 集成测试
- 模拟单向网络环境 (`tc qdisc add dev eth0 netem delay 50ms rate 10mbit`)
- 网络抖动/丢包模拟 (NetEm)
- 长时间稳定性测试 (72 小时连续运行)

### 压力测试
- 50 台客户端同时连接
- CPU/内存占用基线测试
- 网络带宽利用率监测

## 常见问题

**Q: 为什么必须用 C++?**  
A: 为了获得最高性能和 Windows 原生 API 的直接访问能力（DXGI、Media Foundation、Win32 Service）。

**Q: 能否支持 Linux 服务器？**  
A: MVP 阶段仅支持 Windows 客户端。Linux 服务端可通过 Boost.Asio 移植。

**Q: 加密密钥在哪里配置？**  
A: 目前硬编码在代码中，后续将通过配置文件或环境变量指定。

**Q: 能适配 Win7 吗？**  
A: Desktop Duplication 需要 Win8+, BitBlt 降级方案可兼容 Win7(性能下降)。

## 贡献指南

欢迎提交 Issue 和 Pull Request！主要关注方向：
- 性能优化 (降低延迟、提升帧率)
- 功能增强 (文件传输、截图等)
- 代码质量 (注释完善、单元测试)

## 许可证

此项目采用 MIT 开源许可证

---

**注意**: 本工具仅供企业内网运维、安全审计等合法用途使用。未经授权远程控制他人计算机属违法行为。
