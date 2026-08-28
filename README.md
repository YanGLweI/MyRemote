# MyRemote Control — 单向网络反向远程控制

Windows 内网远程控制工具，专用于**单向隔离网络**（被控端可出站访问控制端，控制端无法反向访问被控端）。所有通信隧道由客户端主动发起，服务端仅被动监听，全程无反向连接。

> 本工具仅用于企业内网运维、安全审计等合法用途。未经授权远程控制他人计算机属违法行为。

## 核心特性

- ✅ 严格单向网络：服务端只 `accept`，绝不主动连接客户端（已代码审查佐证）
- ✅ 客户端主动注册上线、设备唯一硬件标识（MAC+主机名）
- ✅ 实时远程桌面：DXGI 桌面复制采集 → H.264 编码 → 传输 → 解码渲染
- ✅ 键鼠反向控制：绝对坐标映射，支持移动/按下/抬起（可拖拽）、滚轮、键盘
- ✅ AES-128-GCM 全链路加密（PSK 经 HKDF 派生），密钥校验拒绝非法设备
- ✅ 断线自动重连（指数退避），心跳 1s、秒级上下线检测
- ✅ 画质三档、全屏、设备备注、二次控制密码、操作日志、批量管理
- ✅ 客户端绿色单文件（OpenSSL/OpenH264/CRT 静态链接），支持开机自启

## 技术栈

| 模块 | 选型 |
|------|------|
| 语言 | C++17 + Win32 |
| 服务端 GUI | Qt 6 (MSVC) |
| 采集 | Desktop Duplication API (DXGI)，BitBlt 降级 |
| 编解码 | OpenH264（自包含，不依赖系统 Media Foundation 编码器） |
| 加密 | OpenSSL AES-128-GCM + HKDF + HMAC-SHA256 |
| 传输 | Winsock2 + 自定义二进制分帧协议 |

## 通信协议

帧格式：`[4B 总长(大端)][1B 类型][载荷]`；除心跳外载荷均为 AES-128-GCM 信封
`[12B nonce][密文][16B tag]`，内层再带 `[1B 真实类型][业务载荷]`。

消息类型：Register / RegisterAck / Heartbeat / StartStream / StopStream /
VideoFrame / InputEvent / RequestKeyframe / AuthChallenge / AuthResponse。

## 构建

前置：VS2022（Build Tools 亦可）、Qt6 MSVC、vcpkg：

```
vcpkg install openssl:x64-windows-static openh264:x64-windows-static
```

然后运行 `build.bat`（需按本机路径设置 VCPKG / QTDIR），产物：

```
build/bin/Release/agent.exe            # 客户端，单文件
build/bin/Release/control_server.exe   # 服务端，随附 Qt 运行时（windeployqt）
```

手动 cmake：

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_PREFIX_PATH=C:/Qt/6.7.3/msvc2019_64 ^
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
```

## 使用

1. 服务端：将 `deploy/server_config.json` 放到 `control_server.exe` 同目录（可省略，使用默认端口 7500 / 默认密钥），运行。
2. 客户端：把 `agent.exe` 拷到目标电脑，**直接双击运行**即打开图形配置界面，填写服务器地址/端口/连接密钥，可点“测试连接并注册”验证；点“保存并后台运行”后 agent 自动转入后台并注册到服务端。仅改配置不启动：`agent.exe --config-ui`。开机自启：`agent.exe --install-autostart`（卸载 `--uninstall-autostart`）注册**最高权限**计划任务，登录后即以管理员权限静默运行（需在管理员命令行下执行一次）。调试窗口：`agent.exe --console --background`。
3. 控制端双击在线设备 → 输入该设备的控制密码（客户端未设置则留空）→ 查看/操作远程桌面。

> 任务管理器、UAC 弹窗、管理员控制台等提权窗口只接受来自提权进程的注入输入（UIPI）。以普通权限运行的 agent 在设备列表中标注 `〔受限〕`：可远程操作普通窗口，但点不动提权窗口。在目标机托盘图标右键选“以管理员身份重启”即可接管。

客户端与服务端的 `secret_key` 必须一致，否则注册被拒。

## 部署形态

- 客户端普通进程 + 最高权限登录计划任务自启 + 无控制台窗口（后台静默）
- 不使用 Windows 服务（Session 0 无法捕获交互桌面）
- 客户端仅出站连接，本机不监听任何端口

## 已知限制

- 串流为 CPU 软件编码，帧率上限取决于被控机自身开销：同一 3400×1812 桌面，1920 档实测本机 30fps（采集 20ms+编码 10ms）、弱机仅 2fps（采集 170ms+编码 250ms），画质档位里的 1280 上限即为此准备；后续可换 GPU 缩放/编码
- 服务端解码已移出 GUI 线程（独立解码线程 + 单槽最新帧），帧率显示区分 NET（收包）与 DEC（解码）
- DXGI 复制的纹理不含硬件指针，因此远程画面里的箭头由控制端本地回显；远端有人自己移动鼠标时该箭头不会跟随
- DXGI 桌面复制在部分虚拟机/远程会话下不可用时自动降级 BitBlt
