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
- [x] **M15** TEST-WIN 实测后的修正：死守物理控制台 + 断线自动恢复 + 删除 Attach to console
  - 实测结论：无人登录开机 → 宿主进 `Winlogon`（BitBlt，DXGI 被拒 0x80070005）→ 远程输密码真的登录成功（`WTS_SESSION_LOGON`），且全程无 `UNHANDLED EXCEPTION`（M14-0 的崩溃根因确认已修）
  - M15-0 会话解析只认物理控制台：`win32util::console_session()` 以 `WinStationName=="Console"` 为首选（不看连接状态），`RDP-*` 站点硬否决，解析不出就返回 false 让宿主原地不动。旧实现用 `WTSGetActiveConsoleSessionId()`，控制台一旦被 RDP 顶成断开就返回 0xFFFFFFFF，于是落到"最大编号的 active 会话"= 刚建起来的 RDP 会话，宿主被迁走并杀掉唯一隧道
  - M15-1 监管去抖：迁移需要同一目标稳定 5 秒且宿主已满 15 秒；`SESSIONCHANGE` 只在可能改变控制台归属的事件（1/2/3/4/6）上唤醒，登录/锁定/解锁不再插手；`--service-state` 打印站点表与判定来源
  - M15-2 端到端删除 Attach to console：`0x0C` 退役不复用，agent 的 `tscon` 分支与控制端按钮一并去掉，只保留 `〔非控制台〕` 提示
  - M15-3 控制端自动恢复：设备重新注册后自动重新鉴权并恢复画面，60 秒内最多 5 次，密码错误立刻放弃；操作者主动停止才会解除
  - M15-4 `--force` 不再清理服务宿主（否则与监管互相残杀），宿主仍清理用户态重复实例
  - M15-5 取消服务延迟自启（实测开机后有 2 分 26 秒的离线空窗），文档同步
  - 顺带修好：`ControlService(STOP)` 传 `nullptr` 出参会以 `ERROR_INVALID_ADDRESS(487)` 失败，因此 `--stop-service`/`--uninstall-service` 从来没真的停过服务

- [x] **M16** 控制端 UI/UX 重做（M15 实测后反馈"能自动恢复，但体感效果不好"）：多标签、深色运维控制台、事件日志、设置与记住布局
  - M16-0 `8a14d07` MainWindow 从 main.cpp 拆出（纯重构，为后面每一步留出可改的边界）
  - M16-1 `e077609` **设备行在本次运行内永不消失**：隧道断了行还在（变成"重连中"），只有离线太久或被手动移除才走；顺带修掉服务端 remove-after-add（新设备注册会被上一行的移除逻辑吃掉）
  - M16-2 `c39f0dc` 一机一标签，最多 4 路并发，标签自带自绘关闭钮；关标签才是结束会话（以前必须"选列表 + 点断开"）
  - M16-3 `c98c0fa` 输入捕获模型：点画面才接管键盘，连按两次 Esc 或释放热键交还，Alt+F4/Win/Alt+Esc 永远留本机；视图出口不再依赖列表选中
  - M16-4 `9e3d31f` 设备列表重写为 model + 自绘 delegate + 搜索（名称/IP/设备号/备注）+ 右键菜单；`4347445` **文本宽度不能决定布局最小值**（fps 标签每秒撑宽一次窗口 = 画面每秒重缩放，改为按最宽文案定宽）；`a85d23a` 会话页眉两行随栏位自适应；`3f328f6` 侧栏声明 `setMinimumWidth(260)`，不允许被分隔条饿死
  - M16-5 `9afc0a8` 深色运维控制台：**颜色走显式 QPalette、形状走 QSS 且只写可交互控件**（以前的"深色"只是借了 Windows 深色模式，换台机器就浅底黑字）；图标全部 QPainter 现画（两个 Qt kit 都没有 Svg 模块，也没有 .qrc）；`e158370` 徽章改成行内记法（设备行是色块小牌，标签页/页眉只能放词：`TEST-WIN · 服务`——上文 M11/M14/M15 里的 `〔〕` 写法到此为止）；`f741d16` 自绘下拉箭头（样式表只能引用文件，于是把 PNG 现画到临时目录）+ 只留一个诚实的帧率
  - M16-5e `c40242e` + `7a3b3ba` **延迟读数＝一次网络往返**：以前从"最后一幅画的年龄"推延迟，既要两台机器时钟对过表，又在画面静止时没有样本；`Pong` 塌回只回显 8 字节时间戳，控制端单钟测量，老 agent 不认识就还是 `--`。测的过程中揪出更值的一个 bug：固定 1 秒节拍 + 严格比对时间戳＝"回得比 1 秒慢就算没回"，会话开头必然连吃 5 次然后永久 `--`，改成 stop-and-wait（只允许一个在途、慢答案照收、超时才算丢失）；两机实测通过
  - M16-6a `51ac4dc` + `d5c4d6b` 事件日志抽屉：`mlog` 加 sink（在文件锁**外**投递，UI 不能拖累写日志），同一行文本既进文件也进窗口；只读 `QPlainTextEdit` + `setMaximumBlockCount(500)`（不封顶就是跟着运行时长长的内存泄漏）；出口是状态栏一个词，`事件日志 · N` 只数警告与错误
  - M16-6b `83a5701` + `44647b5` 设置页：`log_file` 与释放热键以前都只是"写在文件里的承诺"，现在 `mlog::init` 能在运行中重定向（打不开就留在原文件并 WARN），`ServerConfig::save` 补上，热键改完立刻发给所有在跑的标签；Qt 自带的英文 OK/Cancel 用一只只认按钮词的 `QTranslator` 解决，不引入 `.qm` 部署文件
  - M16-6c `06b2d93` 记住布局：窗口几何、两条分隔条、抽屉开合、默认画质在关闭时写、启动时读；全屏故意不记（那是一次会话的心情，不是明天该醒成的样子）
  - M16-6d 文档：README 的徽章写法、协议消息清单（补齐 StateReport / Ping-Pong）、控制端"设置"改的是什么文件；本条即补上的 M16 进度
  - M16-7 `9880f27` **清晰档真的到原生分辨率**：StartStream 第三字段过去"缺失当 0"，agent 又把 0 当"回落配置上限"，宽桌面上清晰的"设备原生"永远够不着；parse 改 `std::optional`，只有字段真缺失（老控制端 3 字节包）才回落，显式 0 走原生。顺带揪出第二处：新会话首包发的是控制器私有 {30,2048,0}，不是下拉显示的那档——视图在构造时用显示档初始化控制器。YEUNG 4K 实测：清晰 cap=0→Encode 3840x2160、均衡 cap=1920→1920x1080、新会话首包继承持久化默认；TEST-WIN 老 agent 收到 cap=0 仍回落配置上限，兼容如旧；`1531c18` 档位说明改视频词汇 **720p / 1080p / 原画**（"长边 1280"这种编码器话术操作者读不懂），TEST-WIN 新 agent 三档全验：流畅 cap=1280→Encode 1280x800、均衡/清晰在 1480 桌面上走原生
  - M16-8 `1e22fc6` **分辨率下拉真的改对端桌面**：画质档只缩编码，虚拟机默认 1480x925 里操作者只能干坐着。新增 QueryDisplayModes / DisplayModes / SetDisplayMode 三条消息走既有隧道（不开端口、不加监听），agent 用 EnumDisplaySettingsW 枚举、ChangeDisplaySettingsExW 先 CDS_TEST 再用普通 flags 落地——只影响本次会话，绝不写 CDS_UPDATEREGISTRY，重启即还给机器自己的选择；DisplayModes 兼作每次切换后的回执，老 agent 不认这个询问就显示 `--` 而不是编一个值。验证时又揪出两处谎报：roster 存的始终是"注册那一刻"的尺寸，页眉、设备行与新会话解码器都按旧值读（隧道现在把实时尺寸写回 roster），以及下拉只留了 8 个字宽，1920x1080 显示成 `1920x10`。TEST-WIN 实测 1480x925 → 1920x1080 → 1480x925，每步 agent 日志都留下 `Display mode set to ... (was ...)`，约 300ms 后回抛 DisplayChanged，页眉与设备行当场跟上；1920x1080 撑过了控制端重启，说明对端桌面确实变了
  - M17 `168c2f8` + `c15ed77` + `1aa1c27` **两端各出安装版与绿色版**：交付从"拷 build 目录 + 人手 `--install-service`"变成四个包（Server/Agent × setup/zip）+ `SHA256SUMS.txt`。`packaging/stage.ps1` 先断言 CMakeLists 的 `PROJECT_VERSION` == `common.iss` 的 `MyAppVersion`、windeployqt 产物齐全、两个 exe 的 ProductVersion 真是这个号，才压 zip、编 setup。**zip 必须用系统 bsdtar**：`Compress-Archive` 与 .NET `ZipFile` 都把目录分隔符写成反斜杠，7-Zip/unzip 会解出一个名叫 `platforms\qwindows.dll` 的文件。向导中文界面靠 `packaging/lang/ChineseSimplified.isl`，钉在 issrc tag `is-6_7_3` 并把校验值记进 `SOURCES.md`（它不随 Inno 安装包一起发，本机装了 Inno 也没有）。`168c2f8` 先补版本资源——此前两个 exe 属性页全空，装完无法回答"这台是哪一版"——并加 `--version`，且必须在服务分发/单实例互斥/自动提权/路径解析**之前**返回：查个版本不该留下 `agent.log`。
  - M17-3 第一次真装就抓到四个错：① `{platforms,styles,...}\*` 的花括号展开在 `[Files]` 里一条都没匹配上，ISCC 不报错，装出来的控制端缺 `platforms\qwindows.dll` 根本起不来 → 逐目录写死、去掉 `skipifsourcedoesntexist`、stage 加断言；② "%ProgramData% 已有配置" 的提示是裸 `MsgBox`，`/VERYSILENT`（尤其被一次性服务拉起来跑）时 session 0 没有前台，弹窗会把安装器永久挂住 → 一律写安装日志，只有非静默才弹；③ `SaveStringsToUTF8File` 必带 BOM，写出的 `config.json` 与配置界面写的那份不逐字节可比 → 值全是 ASCII，直接写字节；④ 开始菜单组与字段冒号不统一。
  - M17 真机证据：YEUNG 静默安装 → 写出无 BOM 的 `C:\MyRemote\Agent\config.json`、服务 `binPath` 重指、**设备号不变**重新注册；卸载后服务没了、`agent.exe` 与卸载器自己也没了，而运行期写出来的文件留在原地（`%ProgramData%\MyRemote\service.log` 与 `{app}` 里的 `config.json`/`agent.log`/`host.status` 都是）——Inno 只删自己装进去的东西，所以"目录在不在"取决于有没有运行期文件。TEST-WIN 走管理共享 + 一次性服务推送（SCM 报 1053 是预期：安装器本来就不进服务分发器），装→卸→再装全通过，护栏按预期只记一行日志不挂住。打包后的两端开真实会话 `1920x1080 · 帧率 15`，分辨率下拉照旧列出模式。便携服务端 zip 解压并**人为打上 Zone.Identifier** 之后仍能启动并写日志——所以 `unblock.cmd` 是排障手段，不是我复现得到的缺陷。
  - M17-4 发版前把 release note 里两处"应该"按掉：`/SERVERIP` 等**一个都不给**的分支拿 `/TASKS= /DIR=C:\MyRemote\AgentTest` 真装一次（退出码 0、没有 `config.json`、正在跑的服务 `binPath` 一字未动），卸载后 scratch 目录整体消失；"卸载保留配置"改写成实测口径并顺带发现一件事——**同一台机器上两份 agent 安装共用一个服务名 `MyRemoteAgent`，卸载任意一份都会把服务摘掉**（这条进已知限制）。YEUNG 的 `%ProgramData%\MyRemote\config.json` 其实不存在（配置在 `{app}`），所以护栏那一支只在 TEST-WIN 验过；两处措辞都按"哪台验的"写实。
  - 量测口径：空列表最小跟踪尺寸 `794x491` → 抽屉出口 `794x497` → **抽屉打开时竖向地板变成 747**（小屏笔记本上要留意）；开着会话时是 1400x769，尚未针对四路标签与小屏复核

## 后续可选优化

- [ ] GPU 缩放 + GPU H.264 编码（提升弱机帧率/降低延迟；独立解码线程已完成）
- [ ] 50+ 并发压力测试与自适应码率
- [ ] 传输层接入真实多播/带宽统计 UI
- [ ] 隧道改由服务持有 + 命名管道转发给宿主（消除会话切换时约 2 秒的设备行闪断；当前宿主崩溃/重启由服务在 ≤2s 内拉起）
- [ ] 真·Ctrl+Alt+Del（`SendSAS`/TAP COM，依赖 OSK 组件，Home 与 Server Core 不保证可用）
- [ ] 多监视器选择；安全桌面上的多显示器
- [ ] 会话列表选择器（查看/接管非控制台会话）
- [ ] 安装器把 `config.json` 写进 `%ProgramData%\MyRemote\`（agent 真正优先读的那份），并让**显式给出的** `/SERVERIP` 等参数覆盖已有配置：现在换控制端地址只能改文件，重推包不生效
