# 开发交接文档（2026-08-31 深夜，M21 收口时点）

> 给下一个会话冷启动用。所有"已验"都指当场有输出/截图/退出码；"待验"都给出台账和操作方法。
> 进度史实在 TASKS.md，行为语义在 README.md，面向用户的说法在 packaging/release-notes.md，
> 本文只写**状态、边界、下一步**。

## 1. 发布与交付现状

| 事项 | 状态 |
| --- | --- |
| GitHub Release | 仍停在 **v1.0.0**（tag 与资产一字未动），1.0.1~1.0.4 都**未发布** |
| 代码 | main 已推到 M21-6，共六笔：`9dd9c3c`(M21-1) `19df922`(M21-2) `a116d77`(M21-5) `647b7ea`(M21-3) `f759b4c`(M21-4) `b34708c`(M21-6)；**本文件所在的第七笔（M21-7 文档+抬版本）收尾后需 `git push`** |
| 交付物 | `D:\IT-share\MyRemote-v1.0.3\`：四包 + `SHA256SUMS.txt`，仍是**当前交给用户的那一份**（1.0.3，带这三个 bug） |
| 本轮出包 | **没出**。1.0.4 只抬了版本号（`CMakeLists.txt` + `packaging/common.iss` 一致，`stage.ps1` 的断言过得去），四包等下一次决定 |
| dist 目录 | `build/package/dist/` 里 1.0.3 的 **agent** setup 曾被本轮一次 `ISCC` 试编译覆盖成"M21 内容、1.0.3 文件名"，已挪走改名为 `build/scratch/agent-setup-m21-probe.exe`；dist 里现在没有任何 1.0.4 产物，权威 1.0.3 只在 D:\IT-share |
| 用户侧 | **TEST-WIN 装的是 1.0.3**（本轮三条症状的来源）；**YEUNG 装的仍是 1.0.2**（实测 `C:\MyRemote\Agent\agent.exe --version` → 1.0.2，从未做过 1.0.3 的提权装机）——注意 1.0.2 同样有引号那个病，本机代理命令行里就是 `--config "C:\MyRemote\Agent\config.json"` |

## 2. M21 是什么问题、怎么修的

**用户报告**（TEST-WIN，1.0.3）：① 点「隐藏托盘图标」没反应，而"显示托盘图标"的勾选在两个入口里状态不一致；② 托盘右键「打开配置」显示 `127.0.0.1` 而不是装机时填的地址；③ 一个从没点过的「重新启用远程控制服务」挂在菜单上，可远控其实一直好着。

**三条根因都在代码里逐行核实过，不需要猜**（细节与提交在 TASKS.md 的 M21 块）：

- **A（→②，也是①那个"两个视图"的一半）**：宿主给每会话代理拼 `--config "带引号的路径"`，而 `parse_command_line` 是手写扫描，不剥引号也不在空格处收尾 → 代理读的是一个不存在的路径 → `ClientConfig::load` 静默返回全默认值。**顺带封掉最危险的那条路**：这份默认值窗一按保存，宿主就会经 `save_config` 全量覆写盘上真配置（地址当场变 127.0.0.1 并热重载）。现在 `load` 报 `Missing/Read/Unreadable`，读不动的文件**不给编辑**，宿主代写回 `saved ok`/`saved fail`。
- **B（→①）**：隐藏只写了盘上副本，宿主内存里 `cfg.tray_icon` 仍是 true，而广播给各代理的 `tray_icon=` 读的就是内存那份 → 永远不说"这枚图标不该存在" → 代理既不自退、监管还补生。现在写盘之后 `cfg = hidden`。
- **C（→③）**：**不是误判**。托盘「退出」在服务态就是 `svc::stop()`，之后双击跑的是前台实例（能远控但脱管）。真缺陷是形状判定在进程启动时算一次就缓存；文案还把"回到受管形态"说成"重新启用远程控制服务"。现在主循环 ≤1/s 采样进原子值，菜单**弹的那一刻**只读它（绝不在托盘消息线程上查 SCM——那是 M20 的成因），文案定稿「启用后台服务（当前：前台运行，开机不自启）」。

**另外两处"读数说谎"一并收掉**：`--service-state` 的 `host:` 行现在会先复核 pid 再看文件时间，任一不过就整行前挂 `STALE(原因)`（记录本身不删不改，现场取证要原件）；安装向导填的地址改由 `agent.exe --set-server` 落到 **agent 真正会读的那一份**（读回→只覆盖显式给出的项→整体写回），换控制端地址从此重推包有效。

## 3. 验证台账

### 已验（本机，当场有输出/截图）

1. **M21-1/2 命令行与配置窗**（非提权）：`build/m21_cli_regression.ps1` 7 项全绿（`--version`、`--service-state`≠`--service`、`--config-ui` 读带引号路径、`--flag=value`、非 ASCII 路径、garbage/零字节两份都拒绝开窗）；`build/m21_probe_config.ps1 -Case space` 下代理配置窗按控件 id 回显 `10.77.77.77`、路径标签不含引号、文件 sha256 不变（脚本从不点保存）。
2. **M21-3 隐藏链路**（提权窗口，服务停 2 分钟）：`build/m21_hide_probe.ps1` 13 项全绿——手起 `--session-host` + 真 `--tray-proxy` + 第二个管道客户端；空载荷回 `saved fail` 且一字不写、真载荷回 `saved ok` 且落盘；`hide` 之后客户端收到 `state ... tray_icon=0`、代理**自己**退出且理由 `the host hid the icon`、宿主落了 `no icon; retiring`，`left a ghost icon` 保持 0 次（M20 的判据没被本轮改坏）。
3. **M21-4 菜单形状**（同一提权窗口）：`build/m21_menu_shape.ps1` 三跑三张截图——服务停着：该项在，菜单 239px（`m21-menu-stopped.png`）；**同一实例活着时把服务起起来**：该项消失，实测 **13s**（`m21-menu-live.png`，195px）；服务在跑时新起实例：该项与「安装开机自启」都不在（`m21-menu-running.png`）。
4. **M21-5**：伪造 `host.status` 里的 pid → 打 `STALE`；服务 RUNNING → 打真值。本轮还顺手抓到一个活案例：dev 构建把 8 月 30 日的记录当现值打了出来。
5. **M21-6 CLI**：`build/m21_set_server.ps1` 17 项全绿（新建、只改 `--ip` 其余六项原样保住、坏文件拒绝且 sha256 不变、空参数返回 2、不起实例、带空格的 `--key` 完整存活）；`ISCC` 编译 `agent.iss` 通过（删掉的 `JsonEsc`/`AppConfig`/`ProgramDataConfig` 确无残留引用）。

### 待验——只能真机（下一个装机窗口，TEST-WIN 优先）

① 安装态点「隐藏托盘图标」：图标 ≤1s 退场、`--service-state` 的 `proxies=` 掉到 none、日志无 `left a ghost icon`。
② 前台实例点「启用后台服务（当前：前台运行，开机不自启）」：这台真的收回受管形态（该实例随后被新宿主 `retire_same_path_instances` 收掉）。
③ **M21-6 端到端那一跑**：`/VERYSILENT /TASKS="!service" /SERVERIP=...` 装到临时目录 → ProgramData 那份为该 IP 且 `device_name`/`control_password`/`tray_icon` 仍在；再装一次不同 IP → 被覆盖；不带参数 → 不动已有配置。**刻意没在 YEUNG 做**：安装包与机器上那份同 `AppId` 同版本，Inno 会先把 `C:\MyRemote\Agent` 的现有安装静默卸掉——为验 12 行 Pascal 把这台机器的受管形态拆了不值。TEST-WIN 上做这件事没有这个代价。
④ M20 那七条（假宿主故障注入、explorer 重启、退出无残留、僵死代理 ≤10s 自愈、暂停/退出/隐藏语义、1.0.2→1.0.3 覆盖安装）——**至今一条都没在真机上跑过**，账还在 TASKS.md 的 M20 块末尾。

用户侧复测脚本 = 他撞出 bug 的原序列：暂停→再右键→恢复→退出→双击→再右键→隐藏→勾回显示。

## 4. 可能还有什么问题（按可信度排序）

1. **菜单跟随延迟 = 一轮主循环**。控制端连不上时那一轮里含 10s 连接超时，实测 13s（隧道正常时 ≤1.2s）。这不是缓存 bug，是循环结构；要压到 1s 内只能把形状采样挪出主循环（新线程或独立定时器线程），本轮刻意没做——M20 的全部教训就是往消息线程上放慢调用。
2. **`state` 行刻意没加 `config_ok=0/1`**：修好引号之后，"代理读到的是不是真配置"已经由代理自己那行 `Tray proxy config: <path> (found|missing)` 与开窗/拒绝行为说清，再加一个字段只是多一处会各自说谎的读数。若现场仍判断不了，再补。
3. **管道 DACL 是全机交互用户**（`(A;;GRGW;;;IU)`），不是"该会话用户"——文档本轮已改口。所以 `secret_key`/`control_password` 绝不推上管道；`save_config` 走的是宿主自己那份真配置。任何本地进程仍能连管道发 `pause/hide/quit`，这是定案（托盘是操作入口，不是权威凭据）。
4. **`pause`/`resume` 文案的乐观翻转**（老账）：菜单文案按本地缓存决定，宿主恰好同毫秒改状态会差一拍，症状轻，真机复测时留意。
5. **混合版本共存**：1.0.4 的代理与 1.0.2/1.0.3 宿主共存时，`saved ok/fail` 应答老宿主不会发 → 代理配置窗等 3s 后按"没落盘"报失败（保守方向，不会说谎说成功）。覆盖安装的瞬间窗口期没测过。
6. 老账（TASKS.md 末节）：隧道随宿主重启闪断 ~2s、弱机帧率、多显示器。

## 5. 下一步工作（按顺序）

1. **TEST-WIN 装 1.0.4**（要先出包：`packaging/stage.ps1`）→ 跑第 3 节 ①②③，顺路把 M20 那七条一起结。跨机动作，**必须先经用户点头**。
2. 全绿之后再谈抬 tag / 发 Release（当前决定仍是**不发版**，1.0.1~1.0.4 只在交付目录里）。
3. 收尾清理：删测试期间为 dev 路径写的两个 `HKCU\Control Panel\NotifyIconSettings\<key>\IsPromoted=1`（本机，`build/m19_promote_icon.ps1` 当年写进去的）。
4. YEUNG 这台还停在 1.0.2：要么随下一次真机验证一起抬，要么明确不抬（它现在能远控、受管形态正常）。

## 6. 环境与工具备忘（会杀时间的坑）

**本轮新增的，全是脚本自己的坑，每一条都制造过一次假 FAIL 或假 PASS：**

- **`attach_parent_console()` 会让 PowerShell 抓不到子进程 stdout**：`$out = & exe ...` 拿到空，`$LASTEXITCODE` 甚至是空的。凡是要读 agent 的打印或退出码，一律 `Start-Process -Wait -PassThru -RedirectStandardOutput <file>`。
- **孤儿检测正则 `build..bin..Release` 永远匹配不上** `build\..\build\bin\Release`（两个点各只代一个字符）——它给出的"无残留"是**假 PASS**。要写 `build.*bin.*Release`。
- **管道客户端读法**：`NamedPipeClientStream` 的方向枚举没有 `ReadWrite`（只有 `In/Out/InOut`）；客户端句柄上 `ReadTimeout` 直接抛"此流上不支持超时"；`ReadAvailable` 在客户端不可信。正解和代理自己一样：P/Invoke `CreateFileW` + `PeekNamedPipe` + `ReadFile`。
- **P/Invoke 的 DWORD**：PS 里 `0xC0000000` 是负的 Int64，绑不上 `uint` 参数 → 签名里声明 `int`；`out uint` 的 `[ref]` 变量必须先 `[uint32]0` 声明。
- **断言要消费掉匹配过的行**：`Wait-Line` 从头扫累积缓冲区时，上一条 `saved fail` 会把下一个"应该收到 saved ok"的断言判成 FAIL。
- **宿主主循环会被 10s 连接超时占满**：凡是"等宿主把话传给代理"的断言，超时给到 20~25s，否则测的是网络不是产品。
- **提权**：本机 `Start-Process -Verb RunAs` 能静默过（不需要有人点 UAC，也不需要显示器）；但 `-Verb RunAs` 与 `-RedirectStandardOutput` **不能同时用**（参数集冲突）→ 在被拉起的那个脚本里 `Start-Transcript`。
- **harness 参数少写一个 flag 会放出真实例**：`--set-server` 漏掉时 agent 就以前台身份起、自己提权、弹一个模态配置窗留在用户桌面上。判据脚本一律带 `--no-elevate`，失败路径统一走 `Bail()`：先按类名 `WM_CLOSE` 自己的窗，再停进程。
- **`Get-Process agent` 看不到别的会话/提权进程的 Path 与命令行**：查残留要用 `Get-CimInstance Win32_Process`（且从提权侧查才看得见提权那个）。

**沿用的老坑：**

- **bash 里没有 cmake**：绝对路径 `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`；`build.bat` 依赖 PATH 里的 cmake。**ISCC 在** `C:\Users\YLW\AppData\Local\Programs\Inno Setup 6\ISCC.exe`。
- **测试脚本一律纯 ASCII**（PS 5.1 按 ANSI 解码无 BOM UTF-8，中文字面量匹配必挂）；窗口识别走类名（`MyRemoteAgentTray`/`MyRemoteConfigWnd`/`#32768`），中文按钮文案在 C# 侧用 `(char)0x4FDD` 拼；`EnumWindows` 回调整体放 C# 侧；PS 单结果管道要 `@()` 包；`$w` 会撞 `$W`。
- 截图脚本必须先 `SetProcessDPIAware()`（本机 200%/3400 宽物理）。
- 关键脚本：`build/m21_cli_regression.ps1`、`build/m21_probe_config.ps1`、`build/m21_hide_probe.ps1`、`build/m21_menu_shape.ps1`、`build/m21_window.ps1`（把需要停服务的那几跑收进一个提权窗口）、`build/m21_set_server.ps1`、`build/scratch/m21_pipe_smoke.ps1`（不碰产品、只练管道读法）、`build/scratch/m21_cleanup.ps1`（提权收残留）。取证目录 `C:\ProgramData\MyRemote\tray-<会话>.log` 与 `C:\MyRemote\Agent\*.log`。
- 远端只读取证走 `\\10.60.254.153\c$\...`，bash 命令层拒绝内联 UNC——写进 `.ps1` 再执行。TEST-WIN 的两份配置本轮实测都是 `10.60.1.188`，**没被 127.0.0.1 覆盖过**。

## 7. 机器快照（本会话结束时）

- **YEUNG**：安装态 **1.0.2** 服务 RUNNING（本轮为验证停过两次、每次约 2 分钟，均已起回），宿主在控制台会话 4，会话 2（RDP，无显示器）有代理；管道正常；`build.*bin.*Release` 残留 = 0；桌面上本轮误放的一个配置窗已 `WM_CLOSE` 收掉（那个实例随后自己退出）。`%ProgramData%\MyRemote\config.json` 与 `{app}\config.json` 都还是这台自己的真值，本轮所有写测试都走 `--config <scratch>`。
- **构建产物**：`build/bin/Release` = **1.0.4** dev 全量（含 windeployqt 产物）；`build/package/dist` 无 1.0.4 包；`build/scratch/` 下本轮留下 `m21_window.log`（提权窗口全程转录）、`m21_pipe_trace.txt`（管道逐行）、`m21-menu-{stopped,live,running}.png`、`m21set/`、`m21host/`、`m21menu/` 与上面那批脚本。
- **git**：工作区应只剩 M21-7 这一笔待推（README / release-notes / TASKS / HANDOFF / 两处版本号）。`.qoderignore` 是未跟踪的本地文件，不属于任何里程碑。
