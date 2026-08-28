# 开发进度 — MyRemote Control

按 PRD 分里程碑实现，均已在本地端到端验证（提交见 git log）。

## 里程碑状态

- [x] **M1** 构建系统：重写 CMake（去 Boost/protobuf/DXGUID，AUTOMOC，静态 CRT + vcpkg OpenSSL）；移除损坏的 ECDH/AES/protobuf 模块
- [x] **M2** 协议与连接：自定义二进制分帧；客户端注册上线、设备硬件标识、心跳 1s、断线指数退避重连；服务端被动监听 + 会话线程 + 3s 离线回收 + 真实设备列表
- [x] **M3** 加密：PSK→HKDF→AES-128-GCM 信封；密钥不匹配被服务端拒绝（GCM 认证失败）
- [x] **M4** 桌面串流：DXGI 采集 → OpenH264 编码 → 传输 → OpenH264 解码 → QImage 渲染（实测远程桌面实时可见）
- [x] **M5** 键鼠控制：INPUT_EVENT 协议；客户端 SendInput 忠实注入（可拖拽）；服务端坐标映射
- [x] **M6** P1：画质三档（实时重配编码器）、全屏、设备备注（QSettings）、心跳优化
- [x] **M7** P2 + 交付：二次控制密码（HMAC challenge-response）、批量断开、操作日志（control_server.log）、开机自启、agent 单文件、windeployqt 打包

## 后续可选优化

- [ ] GPU H.264 编码 + 独立解码线程（提升帧率/降低延迟）
- [ ] 50+ 并发压力测试与自适应码率
- [ ] 传输层接入真实多播/带宽统计 UI
