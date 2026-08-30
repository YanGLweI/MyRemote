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
- ✅ 控制端多标签会话（最多 4 路同时）、设备搜索与备注、批量断开、深色运维控制台配色
- ✅ 键盘只在点过画面之后送到对端，连按两次 Esc 或按释放热键交还；Alt+F4 与 Win 永远留在本机
- ✅ 画质三档（流畅 / 均衡 / 清晰）、全屏、二次控制密码
- ✅ 事件日志抽屉（同一行文本既进日志文件也进窗口）、设置页、记住窗口布局与默认画质
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
VideoFrame / InputEvent / RequestKeyframe / AuthChallenge / AuthResponse /
DisplayChanged / StateReport（能力 flags + 桌面尺寸）/ LockWorkstation /
Ping-Pong（延迟读数＝一次网络往返，与画面是否在动无关）/ QueryDisplayModes →
DisplayModes（当前分辨率 + 可选模式清单，兼作切换后的回执）→ SetDisplayMode
（真的改对端桌面分辨率，只作用于本次会话，不写注册表）。

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

## 安装与部署

正式版发四个包（`SHA256SUMS.txt` 是它们的 SHA-256），下载在
[Releases · v1.0.0](https://github.com/YanGLweI/MyRemote/releases/tag/v1.0.0)；本地跑 `packaging\stage.ps1` 也能重新产出到 `build\package\dist\`：

| 给谁 | 安装版 | 绿色版 |
| --- | --- | --- |
| 控制端（服务端，运维坐的那台） | `MyRemote-Server-v1.0.0-setup.exe` | `MyRemote-Server-v1.0.0-portable.zip` |
| 被控端（客户端，要被远控的机器） | `MyRemote-Agent-v1.0.0-setup.exe` | `MyRemote-Agent-v1.0.0-portable.zip` |

- **控制端装在 `C:\MyRemote\Server`，不要装进 `Program Files`。** `server_config.json` 和日志都写在 exe 旁边，设置页每次保存都要重写它；非提权进程写不进受保护目录，表现就是状态栏那句"设置没能写进文件"。
- 控制端安装版加一条**按程序**放行的入站防火墙规则（不按端口，所以以后改监听端口不用回来补规则），卸载时删掉。被控端**只出站**，不需要任何放行；用绿色版则要自己放行 TCP 7500。
- 被控端安装版默认勾选"安装并启动系统服务"——这是唯一能在无人登录时工作的形态。不想装服务就取消勾选，或用绿色版里的 `install-service.bat`。
- 卸载删掉自己装进去的文件与服务，**配置和日志都留在原地**：`%ProgramData%\MyRemote\` 整目录不动，运行期写出来的 `config.json` / `agent.log` / `host.status` 因为不是安装器放进去的，也照样留在安装目录里。误删不丢现场，重装即恢复。
- 包**没有代码签名**，首次运行会吃 SmartScreen：点"更多信息 → 仍要运行"。

远程批量推送（不依赖 WinRM，走管理共享即可）：

```bat
MyRemote-Agent-v1.0.0-setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART ^
  /TASKS=service /SERVERIP=10.0.0.5 /SERVERPORT=7500 /SECRETKEY=与控制端一致的串
```

三个参数都是选填：**一个都不填**就什么都不写（实测退出码 0、目录里没有 `config.json`、已有服务没被碰），之后再填有两个入口——开始菜单的"配置界面"快捷方式，或直接双击 `agent.exe`（前台启动先弹配置窗，保存后才开始连；静默安装不会弹这个窗）。**填了任意一项**就把整份 `config.json` 写到 exe 旁边——除非 `%ProgramData%\MyRemote\config.json` 已经存在（agent 优先读它），那种情况安装器只往安装日志里记一行、不动那个文件，因此**换控制端地址要改那个文件而不是重推包**。卸载：`unins000.exe /VERYSILENT`。

现场先跑这两条：`agent.exe --version` / `control_server.exe --version` 报版本号，`agent.exe --service-state` 打印服务状态、会话解析结果与宿主实时状态。

## 使用

1. 服务端：将 `deploy/server_config.json` 放到 `control_server.exe` 同目录（可省略，使用默认端口 7500 / 默认密钥），运行。控制端窗口右下角的“设置”能改同一份文件里的监听端口、绑定地址、最大接入数与日志文件——**日志文件立刻生效，其余三项要重启控制端**；连接密钥只在这份文件里改（改它会让所有已注册设备一起失效，不该做成一个按钮）。
2. 客户端：把 `agent.exe` 拷到目标电脑，**直接双击运行**即打开图形配置界面，填写服务器地址/端口/连接密钥，可点“测试连接并注册”验证；点“保存并后台运行”后 agent 自动转入后台并注册到服务端。仅改配置不启动：`agent.exe --config-ui`。调试窗口：`agent.exe --console --background`。
3. **推荐安装为服务**（唯一能在无人登录时工作的形态）：`agent.exe --install-service`。服务以 LocalSystem 开机即自启，在控制台会话里托管一个采集/注入进程，因此机器停在登录界面时也能远程查看并远程输入密码登录 Windows。配置读写位置改为 `%ProgramData%\MyRemote\config.json`（普通用户可写，便于现场改配置）；装了服务之后再双击 `agent.exe` 只会打开这份配置后退出，不会建立第二条隧道（排障需要强行接管用 `--force`）。配套命令：`--uninstall-service`、`--start-service`、`--stop-service`、`--service-state`（打印服务状态、会话解析结果与宿主实时状态）。
4. 不能装服务的机器可退回登录计划任务：`agent.exe --install-autostart`（卸载 `--uninstall-autostart`）注册**最高权限**计划任务，登录后以管理员权限静默运行；它做不到服务能做的“无人登录时可用”。`--install-service` 会自动删掉这个遗留任务，避免两个 agent 抢同一个设备号。
5. 控制端双击在线设备 → 输入该设备的控制密码（客户端未设置则留空）→ 查看/操作远程桌面。

> 任务管理器、UAC 弹窗、管理员控制台等提权窗口只接受来自提权进程的注入输入（UIPI）。以普通权限运行的 agent 会在设备行上标一枚 `受限` 小牌：可远程操作普通窗口，但点不动提权窗口。在目标机托盘图标右键选“以管理员身份重启”即可接管；**装成服务后不存在这个问题**（宿主以 SYSTEM 运行，高于任何提权窗口）。
>
> 其余几枚小牌：`服务`由 MyRemoteAgent 服务托管；`登录界面`对端此刻正停在登录/凭据界面，可以直接远程输密码；`非控制台`该会话此刻没接在物理显示器上（典型原因是有人正用 RDP 操作这台机器）——隧道不会因此断开，RDP 一放手画面就自己回来，无需人工干预。**设备行里它们是色块小牌，标签页标题和会话页眉里放不下色块，就写成同一批词：`TEST-WIN · 服务`。** 想主动回到登录界面输密码，点“返回登录界面”（锁定工作站）。注意 **Ctrl+Alt+Del 无法被注入**：若该机策略要求先按 Ctrl+Alt+Del 才出现登录框（`DisableCAD=0`），这一页远程点不动，请将该值设为 1。

客户端与服务端的 `secret_key` 必须一致，否则注册被拒。

## 部署形态

- **两个进程，一个二进制**：`agent.exe --service`（LocalSystem，Session 0）只做监管——不联网、不碰 GDI、不弹界面；它把唯一一个 `agent.exe --session-host` 用复制的 SYSTEM 令牌投放到**当前持有物理控制台的那个会话**里（`Winsta0\Default`）。
- 宿主按名字跟随真实输入桌面（`Default` ⇄ `Winlogon` ⇄ `SAC-Desktop`），所以登录界面与 UAC 同意框都能看、能打；它从不调用 `SwitchDesktop`——那会把显示器从现场用户手里抢走。
- 隧道与设备身份归**宿主**：服务从不建立连接，手工启动的 agent 检测到服务已装就让位，因此一台机器永远只有一行设备。
- 仍然只出站：客户端不监听、不 accept，控制端地址由被控机主动连出。
- Session 0 本身确实无法捕获交互桌面，早期因此放弃了服务形态；正确解法不是不用服务，而是让服务只当_launcher_，采集放在控制台会话里。

## 开发循环

服务装在哪个路径就跑哪个 exe（`binPath` 取自 `GetModuleFileNameW`），所以可以直接从 build 树安装、改完就重装：

```bat
build\bin\Release\agent.exe --stop-service       REM 先释放 agent.exe，否则 LNK1168
cmake --build build --config Release --target agent
build\bin\Release\agent.exe --install-service    REM ChangeServiceConfigW 重指 binPath 并启动
build\bin\Release\agent.exe --service-state
```

只改宿主逻辑时可以完全绕开服务：`agent.exe --background --console`。

## 已知限制

- 串流为 CPU 软件编码，帧率上限取决于被控机自身开销：同一 3400×1812 桌面，1920 档实测本机 30fps（采集 20ms+编码 10ms）、弱机仅 2fps（采集 170ms+编码 250ms），画质档位里的 1280 上限即为此准备；后续可换 GPU 缩放/编码
- 服务端解码已移出 GUI 线程（独立解码线程 + 单槽最新帧），帧率显示区分 NET（收包）与 DEC（解码）
- DXGI 复制的纹理不含硬件指针，远程画面里看不到对端的箭头；曾试过在控制端本地回显，但每次鼠标移动都触发整帧重绘、操作明显变迟钝，已撤销
- 登录界面相关（M14）的能力边界：
  - 预启动/BitLocker PIN/BSOD/固件层不可达——服务还没开始运行，需要 iDRAC/iLO/虚拟机控制台
  - Ctrl+Alt+Del **无法**被注入（系统硬性限制）。开机后/锁屏后的欢迎界面本身就是安全桌面且已聚焦密码框，远程输密码登录不需要它；但若策略 `DisableCAD=0` 要求先按 Ctrl+Alt+Del，那一页远程点不动，请设为 1
  - 开机后设备要等系统启动完成、网络就绪才上线（服务是自启的，不再叠加延迟自启的 1~2 分钟；宿主连不上服务器时按 1s→30s 退避重试）
  - 登录/注销/快速用户切换/宿主崩溃时设备行会闪断，服务在 ≤2 秒内拉起新宿主；控制窗口会自动重新鉴权并恢复画面（60 秒内最多尝试 5 次，超过后需在状态栏提示时重新双击）
  - 他人正用 RDP 操作这台机器时，控制台会话处于“已断开”状态：宿主仍然守在控制台会话上，因此画面会静止或发黑（输入仍落进该会话），这是 Windows 的限制而不是掉线；RDP 断开后画面自动回来。要真把桌面搬回物理显示器，只能在目标机上注销或断开那个 RDP 会话——“Attach to console” 按钮已随服务形态一起删除
  - 设置了 `WDA_EXCLUDEFROMCAPTURE` 的窗口与硬件保护视频流仍捕获为黑色；指纹/人脸/安全密钥类 Windows Hello 需要本地硬件
  - 多显示器只采集主屏；登录界面固定在主屏，因此不是新增的退化
  - `%ProgramData%\MyRemote\config.json` 允许本机普通用户写（保住“双击改配置”的流程），代价是任何本地用户都能改服务器地址与控制密码——这与 M14 之前“任何人都能跑 agent.exe --config”的暴露面相同。若要收紧，改成仅管理员可写并让配置框自提权
- DXGI 桌面复制在部分虚拟机/远程会话下不可用时自动降级 BitBlt
