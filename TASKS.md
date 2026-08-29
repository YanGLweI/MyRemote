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

- [x] **M8** 单实例互斥 + 系统托盘 + 配置热生效
- [x] **M9** 帧率：编码长边封顶缩放（默认 1920）+ 多线程缩放/色彩转换 + 4 slice 并行编码 + 服务端独立解码线程
- [x] **M10** 杂项小修（分帧队列 O(n²) → deque、开机自启注册表路径转义等）
- [x] **M11** 提权运行（托盘一键管理员重启 + 最高权限计划任务自启 + 设备列表〔受限〕标注）、画质档位可控编码分辨率（远程指针本地回显已撤销：每次移动触发整帧重绘，操作变迟钝）
- [x] **M12** 交互启动自动请求 UAC；RDP 客户端关闭/会话切换后自动恢复桌面采集（`try_recover_dxgi`，DXGI 丢失期间走 BitBlt）
- [x] **M13** 显示尺寸变化实时上报（DisplayChanged）修正鼠标映射偏移；服务端跟随重设渲染尺寸并请求关键帧；“Attach to console”把脱离物理控制台的会话拉回来
- [x] **M14** SYSTEM 服务 + 控制台会话宿主：登录界面可查看与远程登录
  - M14-0 崩溃与热循环防护：`init_duplication` 后重算缩放表（会话切换后陈旧表越界读是 agent 无故消失的根因）、边界钳制、BitBlt 路径帧率节流、关键帧标志改由编码器填写、未处理异常落日志、配置保存不再冲掉编码上限
  - M14-1 `win32util` 公共层：会话解析四级阶梯、`DesktopFollower`（跟随输入桌面，绝不调用 SwitchDesktop）、ProgramData 路径回退、宽字符路径统一（中文用户名下 load/save 不再指向不同文件）
  - M14-2 服务半边：Session 0 只做监管不开 socket、SYSTEM 令牌投放宿主、SESSIONCHANGE 唤醒、重启指数退避；`--install-service/--uninstall-service/--start-service/--stop-service/--service-state` 自带提权与可见结论
  - M14-3 宿主半边：无托盘无对话框、输入注入改由采集线程排空（与捕获同一桌面）、桌面切换时重建采集链+强制关键帧、无桌面时不再退出、host.status sidecar、跨会话的配置 mtime 热更新
  - M14-4 能力上报：Register flags（服务宿主/SYSTEM/控制台拥有者/可跟随安全桌面/登录界面）+ 实时 StateReport；控制端〔服务〕〔登录界面〕〔非控制台〕徽章与“返回登录界面”；手工启动的 agent 让位给服务
  - M14-5 文档：修正 README“不使用服务”的旧结论、COMPILATION.md 里 `sc create` 的错误教程、开发循环与能力边界

## 后续可选优化

- [ ] GPU 缩放 + GPU H.264 编码（提升弱机帧率/降低延迟；独立解码线程已完成）
- [ ] 50+ 并发压力测试与自适应码率
- [ ] 传输层接入真实多播/带宽统计 UI
- [ ] 隧道改由服务持有 + 命名管道转发给宿主（消除会话切换时约 2 秒的设备行闪断；当前宿主崩溃/重启由服务在 ≤2s 内拉起）
- [ ] 真·Ctrl+Alt+Del（`SendSAS`/TAP COM，依赖 OSK 组件，Home 与 Server Core 不保证可用）
- [ ] 多监视器选择；安全桌面上的多显示器
- [ ] 会话列表选择器（查看/接管非控制台会话）
