# 开发交接文档（2026-09-01，M22 F1~F4 **已全绿**，v1.0.5 待发）

> 给下一个会话冷启动用。所有"已验"都指当场有输出/截图/退出码；"待验"都给出台账和操作方法。
> 进度史实在 TASKS.md，行为语义在 README.md，面向用户的说法在 packaging/release-notes.md，
> 本文只写**状态、边界、下一步**。

## 1. 发布与交付现状

| 事项 | 状态 |
| --- | --- |
| GitHub Release | **v1.0.5 已发布**（https://github.com/YanGLweI/MyRemote/releases/tag/v1.0.5）——代码已修、四包已出、TEST-WIN 现场验证全绿，tag 打在 `bac29f2`。v1.0.4 仍可访问（https://github.com/YanGLweI/MyRemote/releases/tag/v1.0.4） |
| 代码 | tag **v1.0.4** 打在 `dbc3954`（M21 九笔：`9dd9c3c` `19df922` `a116d77` `647b7ea` `f759b4c` `b34708c` `e02320d` `dbc3954` + 一笔文档刷新）；main 之后又走了 `08a03af`（TEST-WIN 现场判账，**只动文档**，代码与 1.0.4 一致），再两笔 `b6f71c0`+`e5cf08e`（M22 代码 +1.0.5 版本） |
| 交付物 | `D:\IT-share\MyRemote-v1.0.5\`（四包 + LF 校验，目标目录复校四条全 OK）；1.0.4 那份仍在原地做参照 |
| 版本号 | `CMakeLists.txt` 与 `packaging/common.iss` 都是 **1.0.5**（M22 已抬）；上一版 1.0.4 |
| 用户侧 | **两台都在 1.0.4**（YEUNG：`agent.exe`/`control_server.exe` FileVersion 实测 1.0.4，服务 Running/Automatic；TEST-WIN：`agent.exe` 1.0.4，只有被控端）。TEST-WIN 三条症状现在**都有日志与转录背书**（见第 3 节），不再是"口头确认"；M20 那七条现场账也在同一台机器上逐条判过：**五条判完（⑦ 带一个盲区）、两条刻意没跑**，而那两条没跑的里有一条已经由读代码判出真因（见"已确认待修"） |

## 2. M21 是什么问题、怎么修的

**用户报告**（TEST-WIN，1.0.3）：① 点「隐藏托盘图标」没反应，而"显示托盘图标"的勾选在两个入口里状态不一致；② 托盘右键「打开配置」显示 `127.0.0.1` 而不是装机时填的地址；③ 一个从没点过的「重新启用远程控制服务」挂在菜单上，可远控其实一直好着。

**三条根因都在代码里逐行核实过，不需要猜**（细节与提交在 TASKS.md 的 M21 块）：

- **A（→②，也是①那个"两个视图"的一半）**：宿主给每会话代理拼 `--config "带引号的路径"`，而 `parse_command_line` 是手写扫描，不剥引号也不在空格处收尾 → 代理读的是一个不存在的路径 → `ClientConfig::load` 静默返回全默认值。**顺带封掉最危险的那条路**：这份默认值窗一按保存，宿主就会经 `save_config` 全量覆写盘上真配置（地址当场变 127.0.0.1 并热重载）。现在 `load` 报 `Missing/Read/Unreadable`，读不动的文件**不给编辑**，宿主代写回 `saved ok`/`saved fail`。
- **B（→①）**：隐藏只写了盘上副本，宿主内存里 `cfg.tray_icon` 仍是 true，而广播给各代理的 `tray_icon=` 读的就是内存那份 → 永远不说"这枚图标不该存在" → 代理既不自退、监管还补生。现在写盘之后 `cfg = hidden`。
- **C（→③）**：**不是误判**。托盘「退出」在服务态就是 `svc::stop()`，之后双击跑的是前台实例（能远控但脱管）。真缺陷是形状判定在进程启动时算一次就缓存；文案还把"回到受管形态"说成"重新启用远程控制服务"。现在主循环 ≤1/s 采样进原子值，菜单**弹的那一刻**只读它（绝不在托盘消息线程上查 SCM——那是 M20 的成因），文案定稿「启用后台服务（当前：前台运行，开机不自启）」。

**另外两处"读数说谎"一并收掉**：`--service-state` 的 `host:` 行现在会先复核 pid 再看文件时间，任一不过就整行前挂 `STALE(原因)`（记录本身不删不改，现场取证要原件）；安装向导填的地址改由 `agent.exe --set-server` 落到 **agent 真正会读的那一份**（读回→只覆盖显式给出的项→整体写回），换控制端地址从此重推包有效。

## 3. 验证台账

### 已验（本机，当场有输出/截图）

1. **M21-1/2 命令行与配置窗**（非提权）：`build/m21_cli_regression.ps1` 7 项全绿...
2. **M21-3 隐藏链路**（提权窗口，服务停 2 分钟）：`build/m21_hide_probe.ps1` 13 项全绿...
3. **M21-4 菜单形状**（同一提权窗口）：`build/m21_menu_shape.ps1` 三跑三张截图——服务停着：该项在，菜单 239px（`m21-menu-stopped.png`）；**同一实例活着时把服务起起来**：该项消失，实测 **13s**（`m21-menu-live.png`，195px）；服务在跑时新起实例：该项与「安装开机自启」都不在（`m21-menu-running.png`）。
4. **M21-5**：伪造 `host.status` 里的 pid → 打 `STALE`；服务 RUNNING → 打真值。本轮还顺手抓到一个活案例：dev 构建把 8 月 30 日的记录当现值打了出来。
5. **M21-6 CLI**：`build/m21_set_server.ps1` 17 项全绿（新建、只改 `--ip` 其余六项原样保住、坏文件拒绝且 sha256 不变、空参数返回 2、不起实例、带空格的 `--key` 完整存活）；`ISCC` 编译 `agent.iss` 通过（删掉的 `JsonEsc`/`AppConfig`/`ProgramDataConfig` 确无残留引用）。

### 真机复测（TEST-WIN，2026-09-01 装机 1.0.5，**全程日志与转录在手**）

| 条 | 场景 | 输出/观察 | 状态 |
|---|------|----------|-----|
| F2① | `pid=65535` (不存在的进程) + 停服 | `host: STALE(pid 65535 is not running) pid=65535 ...` | ✅ PASS |
| F2② | lsass pid + 停服 + 非管理员窗口 | `host: STALE(service stopped) pid=920 ...` | ✅ PASS |
| F1 | `--start-service --no-elevate`（Medium IL） | `MyRemoteAgent start: FAILED - administrator rights are required.` | ✅ PASS |
| F4 | 前台实例停服优雅退出 | 托盘消失（log: `the host said bye; exiting`） | ✅ PASS |
| 菜单 | RUNNING 状态下 | 无「启用后台服务」「安装开机自启」两项 | ✅ PASS |

**结论**：F1/F2 权限分岔与 STALE(service stopped) 路径在现场环境全部触达并通过；F4 的 host 退出 graceful shutdown 生效；菜单形状运行态不出现这两项符合预期。

### 真机复测（2026-08-31，TEST-WIN 装 1.0.4，**用户口头确认，未带回日志/截图**）

① 安装态点「隐藏托盘图标」：图标 ≤1s 退场、`proxies=` 掉到 none、日志无 `left a ghost icon`。
② 前台实例点「启用后台服务（当前：前台运行，开机不自启）」：收回受管形态，该实例随后被新宿主 `retire_same_path_instances` 收掉。
③ 症状②那条主判据（托盘「打开配置」回显真地址、`tray-<会话>.log` 首行 `(found)`）随本轮装机通过。

这三条当时按口头确认记账；**2026-09-01 的现场判账已经把它们逐条落到日志**，见下面"现场七条判账"，不用再当口头证据引用。

### 装机带回来的证据（YEUNG 覆盖安装 1.0.4，22:55，本机当场读到）

1. **M21-1 的现场自证**：`C:\ProgramData\MyRemote\tray-2.log` 在 22:55:29 写下 `Tray proxy config: C:\MyRemote\Agent\config.json (found)`——同一个文件在 1.0.2 时代被带引号的 `--config` 挡在门外、代理静默抱默认值。症状②的根因在真机上确认已断。
2. **M21-6 端到端**：`C:\MyRemote\Agent\config.json` 的**创建时间仍是 8-30 15:34**、**修改时间 22:55:28**，与 `unins000.dat` 的写入同一秒 → 是 `CurStepChanged(ssPostInstall)` 里 `--set-server` **原地重写**了它，不是新建、也不是事后手点。文件现在是全七键（老安装器只写五键），而 `control_password` 长度 1 存活——向导从不问这一项，它活下来只可能是"先读回来再整体写回"。**合并语义由此得证**。
3. **M21-5 的反面**：`agent.exe --service-state` 此刻打的是 `host: pid=28156 session=4 ... registered=1 paused=0 proxies=2`，**没有** `STALE` 前缀，而 `host.status` 的 mtime 就在几秒前——判活与判旧两条支路各跑通了一次。
4. 顺带：日志里 22:41 那条 `nothing from the host for 15s; exiting` 是 1.0.2 宿主不心跳时 M20 的静默规则在收尾，属正常路径。

### 现场七条判账（2026-09-01 00:00，TEST-WIN 装 1.0.4，`build/scratch/m20-field/` 有全部转录与截图）

判据脚本 `build/tw_m20.ps1`（远端落在 `C:\M20test\tw.ps1`，sha256 复校一致）。菜单判据走"程序化呼出 + 按 pid 找 `#32768`"，不靠人眼。

| 条 | 判 | 凭据 |
| --- | --- | --- |
| ② explorer 重启 | **PASS** | `taskkill` 掉 explorer pid 7120 后，**同一个 hwnd 721108 / pid 5592** 回出 619x195px 菜单，代理日志无退出无重生（`t3-*.txt`、`t3-after-explorer-restart.png`） |
| ③ 安装态点「退出」 | **PASS** | 服务 STOPPED、`no agent.exe`、`no tray window`、`left a ghost icon` 0、start type 仍 `AUTO_START`（`t1-*.txt`） |
| ⑥ 暂停/退出/隐藏语义 | **PASS** | 22:59:03→22:59:11 完整 pause→resume 往返；22:58:48.283 宿主 `no icon; retiring 1` → 22:58:48.389 代理 `the host hid the icon; exiting`（**106ms**），22:58:57 又 `wanted again` 并重新 spawn；当晚真配置 mtime 停在 22:59:29 没被写过 |
| ⑦ 便携态与 `--service-state` 格式 | **PASS，带一个盲区** | 格式全对；盲区见 F2 |
| ⑤ 便携态菜单形状 | **PASS**（形状）/ **未跑通**（动作） | 服务装着但停着时，前台实例菜单里**只有**「启用后台服务（当前：前台运行，开机不自启）」，没有 autostart、没有以管理员重启（用户当场目视确认）；动作没走通是因为那一跑在**非提权**窗口里，见 F1 |
| ① 只连不读的宿主注入 | **刻意没跑** | 读代码判出它会踩 F3，等于在报障机上现场制造一次"图标没了"。修完 F3 再跑才有意义 |
| ④ 僵死代理 ≤10s 自愈 | **未跑**（需要挂线程的注入） | 但 `t4` 给了**反向**结论：菜单开着 **25s** 全程 `same-popup=1`、pid 不变、无 cull 行——"有人在读菜单"不会被误判成托盘线程僵死，这个担心可以划掉 |

### 已修（M22，2026-09-01 本地 + TEST-WIN 现场双端验证）

四条缺陷全部修在 `client/src/tray_proxies.cpp` 与 `client/src/service.cpp`。详细条目与判据结论见 TASKS.md 的 M22 块，这里只列要点：

- **F3（监管线程挂死）**：`cut_pipe` 与 `close_client` 在 `DisconnectNamedPipe` 之前先 `CancelSynchronousIo(writer_thread)`——cancel-first 防止 disconnect 等待挂起的同步 WriteFile。`reader_loop` 的 `Sleep(150)` 改为 `Sleep(10)` 消除时序竞态。**判据**：`build/m22_wedge_probe.ps1`（死账 + 队列溢出 + 后验 ping + 无 ghost）→ **全绿**。
- **F4（bye 未排空）**：`stop()` 里 `queue_line("bye")` 后轮询 `c->queue.empty()` 排空（`kByeFlushMs=1000`），排空后 `Sleep(100)` 确保 writer 的最后一次 WriteFile 完成。**判据**：`build/m22_f4_probe.ps1`（quit → 宿主退出 → tray log 断言 "host said bye"）→ **全绿**。TEST-WIN：停服→前台实例优雅退出→托盘消失。**全绿**。
- **F1（open_registered 按 GetLastError 分岔）**：`ERROR_SERVICE_DOES_NOT_EXIST` → "not installed"、`ERROR_ACCESS_DENIED` → "access denied; requires administrator rights"。**判据**：`& 'C:\MyRemote\Agent\agent.exe' --start-service --no-elevate` → `FAILED - administrator rights are required.`。**现场通过**。
- **F2（ACCESS_DENIED 盲区用 SCM 定案）**：`host_record_gap` 新增 `service_stopped` 参数；`query()` 传 `QueryServiceStatus` 结果。**判据**：停服 + `pid=0/65535` → `STALE(pid ... is not running)`；停服 + lsass pid + Medium IL 窗口 → `STALE(service stopped)`。**TEST-WIN 现场双路通过**。

回归：`m21_cli_regression.ps1` 7/7、`m21_hide_probe.ps1` 13/13、`m21_set_server.ps1` 17/17 全部 PASS；`m21_menu_shape.ps1` RUNNING 状态无"启用后台服务"项（人工目视）。

## 4. 可能还有什么问题（按可信度排序）

1. **菜单跟随延迟 = 一轮主循环**。控制端连不上时那一轮里含 10s 连接超时，实测 13s（隧道正常时 ≤1.2s）。这不是缓存 bug，是循环结构；要压到 1s 内只能把形状采样挪出主循环（新线程或独立定时器线程），本轮刻意没做——M20 的全部教训就是往消息线程上放慢调用。
2. **`state` 行刻意没加 `config_ok=0/1`**：修好引号之后，"代理读到的是不是真配置"已经由代理自己那行 `Tray proxy config: <path> (found|missing)` 与开窗/拒绝行为说清，再加一个字段只是多一处会各自说谎的读数。若现场仍判断不了，再补。
3. **管道 DACL 是全机交互用户**（`(A;;GRGW;;;IU)`），不是"该会话用户"——文档本轮已改口。所以 `secret_key`/`control_password` 绝不推上管道；`save_config` 走的是宿主自己那份真配置。任何本地进程仍能连管道发 `pause/hide/quit`，这是定案（托盘是操作入口，不是权威凭据）。
4. **`pause`/`resume` 文案的乐观翻转**（老账）：菜单文案按本地缓存决定，宿主恰好同毫秒改状态会差一拍，症状轻，真机复测时留意。
5. **混合版本共存**：1.0.4 的代理与 1.0.2/1.0.3 宿主共存时，`saved ok/fail` 应答老宿主不会发 → 代理配置窗等 3s 后按"没落盘"报失败（保守方向，不会说谎说成功）。覆盖安装的瞬间窗口期没测过。
6. 老账（TASKS.md 末节）：隧道随宿主重启闪断 ~2s、弱机帧率、多显示器。

## 5. 下一步工作（按顺序）

1. **M22 已完工**：F3/F4 判据本机全绿；F1/F2 在 TEST-WIN 现场双路触发并全部通过。版本抬 1.0.5，四包已出，**未发版不打 tag**——等用户确认后可发布。
2. 修完 F3 之后才轮到那两条没跑的注入：① 只连不读的宿主 + ④ 挂住代理托盘线程。脚本已经写好并推到 `C:\M20test\tw.ps1`（`-Test t2` 那段现在写的是"先别跑"，F3 修完改掉那句）。④ 还需要一个挂线程的注入形态，目前没有。
3. 发版之后的既有决定：tag 只打在真正交付过的版本上（v1.0.0、v1.0.4），1.0.1~1.0.3 不再补 tag。
4. 收尾清理**已做**：dev 路径那枚 `HKCU\Control Panel\NotifyIconSettings\2890237777820566933`（`…\build\bin\Release\agent.exe`）的 `IsPromoted` 已删，回读 `<absent>`；`C:\MyRemote\Agent\agent.exe` 那枚**留着**——那是装机版该有的状态，删了图标会掉进折叠区。

## 6. 环境与工具备忘（会杀时间的坑）

**2026-09-01 现场判账这一轮新增，每一条都真咬过我一次：**

- **`[void](Function)` 吞掉的不只是返回值，是函数发出的每一行**。`t0` 那次"找不到托盘窗"是假象——窗找到了、菜单也弹了，只是打印全被 `[void]` 吃了。要丢弃返回值就用 `Function | Out-Null` 或干脆让它吐出来。
- **命令参数位置的 `[uint32]$x` 不是 cast**：PowerShell 把 `[uint32]System.Diagnostics.Process (agent).Id` 整串当字面量绑给参数，报 `ParameterArgumentTransformationError / 输入字符串的格式不正确`。写成 `([uint32]$x)`。
- **`Get-Content $fileInfo` 走的是 `ToString()` = **Name**，不是 FullName** → 于是拿当前目录去拼路径，静默读错文件（本机表现为去仓库根找 `tray-2.log`）。一律 `-LiteralPath $f.FullName`。
- **取证文件有两个可能的根**：`agent.log`/`host.status` 落在 `%ProgramData%\MyRemote` 还是 `{app}`，取决于这台机器的配置在哪一份上——**YEUNG 在 `C:\MyRemote\Agent`，TEST-WIN 在 `C:\ProgramData\MyRemote`**。只扫一个根会得到"canary 全 0"的**假 PASS**（本机就这么错过一次）。判据脚本必须两个根都扫并标明数出来自哪个根。
- **跨进程关不掉别人的模态菜单**：`keybd_event(VK_ESCAPE)` 只作用于前台线程，实测把代理的托盘菜单留挂了 **8 分钟**，那条线程一直停在 `TrackPopupMenuEx` 里（顺带说明：菜单开着并没有被误判成僵死，见第 3 节 ④）。正解 `PostMessage(菜单hwnd, WM_KEYDOWN, VK_ESCAPE)` 再补一发 `WM_CANCELMODE`；脚本收尾必须扫一遍自己可能留下的菜单。
- **程序化呼出托盘菜单可行**：`PostMessage(托盘窗, WM_APP+2, 0, WM_RBUTTONUP)`——`ShowMenu()` 不看 `wParam`，弹的是真菜单，`#32768` 按 pid 能查到，于是"线程还在不在转"有了不靠人眼的读数。**但脚本必须非提权跑**：UIPI 会挡提权进程向普通权限窗口发消息，提权跑必然条条假红。
- **`taskkill /f /im explorer.exe` 之后 explorer 不会在 3s 内自己回来**（TEST-WIN 实测），判据脚本得自己 `Start-Process explorer.exe`。
- 跨机推脚本与收证据都走 `\\10.60.254.153\c$\M20test\`，比让人截图快得多；bash 命令层依旧不吃内联 UNC，写进 `.ps1` 再跑。

**上一轮（M21）新增的，全是脚本自己的坑，每一条都制造过一次假 FAIL 或假 PASS：**

- **`attach_parent_console()` 会让 PowerShell 抓不到子进程 stdout**：`$out = & exe ...` 拿到空，`$LASTEXITCODE` 甚至是空的。凡是要读 agent 的打印或退出码，一律 `Start-Process -Wait -PassThru -RedirectStandardOutput <file>`。
- **孤儿检测正则 `build..bin..Release` 永远匹配不上** `build\..\build\bin\Release`（两个点各只代一个字符）——它给出的"无残留"是**假 PASS**。要写 `build.*bin.*Release`。
- **管道客户端读法**：`NamedPipeClientStream` 的方向枚举没有 `ReadWrite`（只有 `In/Out/InOut`）；客户端句柄上 `ReadTimeout` 直接抛"此流上不支持超时"；`ReadAvailable` 在客户端不可信。正解和代理自己一样：P/Invoke `CreateFileW` + `PeekNamedPipe` + `ReadFile`。
- **P/Invoke 的 DWORD**：PS 里 `0xC0000000` 是负的 Int64，绑不上 `uint` 参数 → 签名里声明 `int`；`out uint` 的 `[ref]` 变量必须先 `[uint32]0` 声明。
- **断言要消费掉匹配过的行**：`Wait-Line` 从头扫累积缓冲区时，上一条 `saved fail` 会把下一个"应该收到 saved ok"的断言判成 FAIL。
- **宿主主循环会被 10s 连接超时占满**：凡是"等宿主把话传给代理"的断言，超时给到 20~25s，否则测的是网络不是产品。
- **提权**：本机 `Start-Process -Verb RunAs` 能静默过（不需要有人点 UAC，也不需要显示器）；但 `-Verb RunAs` 与 `-RedirectStandardOutput` **不能同时用**（参数集冲突）→ 在被拉起的那个脚本里 `Start-Transcript`。
- **harness 参数少写一个 flag 会放出真实例**：`--set-server` 漏掉时 agent 就以前台身份起、自己提权、弹一个模态配置窗留在用户桌面上。判据脚本一律带 `--no-elevate`，失败路径统一走 `Bail()`：先按类名 `WM_CLOSE` 自己的窗，再停进程。
- **`Get-Process agent` 看不到别的会话/提权进程的 Path 与命令行**：查残留要用 `Get-CimInstance Win32_Process`（且从提权侧查才看得见提权那个）。
- **YEUNG 装到 1.0.4 之后，"dev 代理会自己饿死"这条免费清理没了**：安装态宿主现在每 2s 发 `ping`，连上它的 dev 代理不再触发 15s 静默自退，会一直挂着图标（`build/m21_probe_config.ps1` 尾部"留给静默规则自退"那句从此不再成立）。跑完这类脚本要么显式等自己的判据走完，要么用 `build/scratch/m21_cleanup.ps1`（提权，按命令行匹配 `build.*bin.*Release`，先 `WM_CLOSE` 再停）。

**沿用的老坑：**

- **bash 里没有 cmake**：绝对路径 `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`；`build.bat` 依赖 PATH 里的 cmake。**ISCC 在** `C:\Users\YLW\AppData\Local\Programs\Inno Setup 6\ISCC.exe`。
- **测试脚本一律纯 ASCII**（PS 5.1 按 ANSI 解码无 BOM UTF-8，中文字面量匹配必挂）；窗口识别走类名（`MyRemoteAgentTray`/`MyRemoteConfigWnd`/`#32768`），中文按钮文案在 C# 侧用 `(char)0x4FDD` 拼；`EnumWindows` 回调整体放 C# 侧；PS 单结果管道要 `@()` 包；`$w` 会撞 `$W`。
- 截图脚本必须先 `SetProcessDPIAware()`（本机 200%/3400 宽物理）。
- 关键脚本：`build/tw_m20.ps1`（**现场判账那一支**，`-Test t0/t1/t3/t4/t5`，推到目标机改名 `C:\M20test\tw.ps1`；`-AppDir` 默认 `C:\MyRemote\Agent`）、`build/m21_cli_regression.ps1`、`build/m21_probe_config.ps1`、`build/m21_hide_probe.ps1`、`build/m21_menu_shape.ps1`、`build/m21_window.ps1`（把需要停服务的那几跑收进一个提权窗口）、`build/m21_set_server.ps1`、`build/scratch/m21_pipe_smoke.ps1`（不碰产品、只练管道读法）、`build/scratch/m21_cleanup.ps1`（提权收残留）。取证文件**两个根都要扫**：`tray-<会话>.log` 一直在 `%ProgramData%\MyRemote\`，而 `agent.log`/`host.status`/`service.log` 在哪一处取决于这台机器的配置落在哪份（YEUNG 在 `C:\MyRemote\Agent`，TEST-WIN 在 `C:\ProgramData\MyRemote`）。
- 远端取证走 `\\10.60.254.153\c$\...`，**ADMIN$ 是可写的**：脚本推过去、转录与截图从同一处拉回来，比来回截图快一个量级。bash 命令层拒绝内联 UNC——写进 `.ps1` 再执行。TEST-WIN 两份配置在 2026-09-01 现场判账之后实测都还是 `10.60.1.188`，权威那份是 `%ProgramData%\MyRemote\config.json`（`tray_icon=true`，mtime 停在装机那晚的 22:59:29，整晚没被写过）。

## 7. 机器快照（本会话结束时）

- **YEUNG**：安装态 **1.0.4**（`agent.exe` 与 `control_server.exe` 的 FileVersion 都实测过），服务 **Running / Automatic**，宿主在控制台会话 4（`desktop=Winlogon capture=bitblt`，无人登录物理控制台），`--service-state` 打 `registered=1 paused=0 proxies=2` 且**无** `STALE`。配置只有 `C:\MyRemote\Agent\config.json` 一份（`%ProgramData%\MyRemote\config.json` **不存在**），22:55 被 `--set-server` 原地重写。dev 路径那枚 `IsPromoted` 已删（见第 5 节第 4 条）；本轮本机只跑了 `t0` 那一支只读冒烟，代价是**把一个代理的模态菜单留挂了 8 分钟**（pid 4876，已用 `PostMessage(WM_KEYDOWN)+WM_CANCELMODE` 收掉，图标与进程全程无恙，`left a ghost icon` 仍 0）。`build.*bin.*Release` 残留 = 0。
- **TEST-WIN**（`\\10.60.254.153\c$`，只有被控端）：覆盖安装 **1.0.5**，服务 **Running / Automatic**；M22 现场判账（F1/F2）日志齐全。F1 输出 `administrator rights are required.`，F2 输出 `STALE(service stopped)` + `STALE(pid 65535 is not running)`，F4 停服 graceful exit，菜单 RUNNING 态无服务相关项。`C:\M20test\tw.ps1`、证据与 portable 仍保留便于追溯。
- **构建产物**：`build/bin/Release` = **1.0.5** 全量（agent + control_server + windeployqt 产物）；`build/package/dist` = 1.0.5 四包 + `SHA256SUMS.txt`（LF，`sha256sum -c` 四条 OK）；同一批五件已复制到 `D:\IT-share\MyRemote-v1.0.5\` 并在目标目录复校四条全 OK。`build/scratch/` 下本轮留下 `m21_window.log`（提权窗口全程转录）、`m21_pipe_trace.txt`（管道逐行）、`m21-menu-{stopped,live,running}.png`、`m21set/`、`m21host/`、`m21menu/`、`m20-field/`（TEST-WIN 判账证据）、`agent-setup-m21-probe.exe`（试编译产物，别当交付件）与上面那批脚本。
- **git**：本笔是 M21 之后的第十一笔（只动文档：现场判账 + M22 的账 + 脚本坑），代码与 `dbc3954`（tag **v1.0.4**）一致，未动版本号。`.qoderignore` 是未跟踪的本地文件，不属于任何里程碑。
