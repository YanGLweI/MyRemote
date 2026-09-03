# 开发交接文档（2026-09-03，M24 代码五笔已进 main，v1.0.7 包已重出，**F6 与 F5 已判账，余三条现场腿**）

> 给下一个会话冷启动用。所有"已验"都指当场有输出/截图/退出码；"待验"都给出台账和操作方法。
> 进度史实在 TASKS.md，行为语义在 README.md，面向用户的说法在 packaging/release-notes.md，
> 本文只写**状态、边界、下一步**。

## 1. 发布与交付现状（2026-09-03 00:30 实测）

| 事项 | 状态 |
| --- | --- |
| GitHub Release | **只有 v1.0.0 / v1.0.4 / v1.0.5 三个**，`latest` = **v1.0.5**（`gh release list` 当场读数）。**v1.0.6 没有 release**——只有 tag。本笔之前这里写的是"v1.0.6 已发布并给出链接"，那句是假账，已按实测更正；写它的时候那个 release 并不存在 |
| tag | `v1.0.0 v1.0.4 v1.0.5 v1.0.6` 四个都在。**v1.0.7 未打**：现场判据没跑完不打 tag（沿用了 1.0.6 那次的教训——tag 比 release 跑得快的结果就是文档没法说清"到底发了什么"） |
| 代码 | `main` = `origin/main` = **`8045fc0`**。M24 五笔：`7f4a792` F1 尾（错误码一路带话）、`6d91ba5` F5（文案自带可供性 + 真弹 UAC）、`3f44e18` F4（bye 等到送达）、`14648ef` 抬版本 + 撤三处假账、`8045fc0` 判据仪器（菜单自报它画了什么） |
| 交付物 | `build\package\dist\` = **1.0.7 四包 + LF `SHA256SUMS.txt`**，00:28 随 `8045fc0` 重出（`stage exit=0`）。从 zip 里取出的 `agent.exe` 与跑判据用的 `build/bin/Release/agent.exe` **sha256 相同**（`85fb3396a6530eb6…`）：判据判的就是包里的东西。**`D:\IT-share\MyRemote-v1.0.7\` 还没放**，等现场三条腿 |
| 版本号 | `CMakeLists.txt` 与 `packaging/common.iss` 都是 **1.0.7** |
| 本机现场 | 装的是 **1.0.6**（`C:\MyRemote\Agent\agent.exe`，服务 binPath 指着它）。服务 **`4 RUNNING`、start type=Auto**——F6 那一跑结束时的恢复态。之前卡了 1 小时 `STOP_PENDING` 的 pid 4936 已经不在，而且**查清了它是谁**：`build\bin\Release\agent-1.0.5-running.exe`，文件名说是 1.0.5，`FileVersion` 实测 **1.0.7**，一个未提交构建的产物 |

**现在的现场条件与判据的关系**：① 服务 RUNNING，会话 1 里有活的宿主（pid 23976）与它的托盘代理（pid 28204），**这两个进程让"再起一个前台探针实例"这条路只能读"有那一项"以外的形状**——见 §6 的 `--force` 那条；② 也正因为宿主活着，F4 的两腿有地方跑了，但**跑的是 1.0.6 的宿主**，量不到新那道门，要跑得先把 dev 构建放进服务指向的那份（改名让位，跑完还原）；③ `sc sdset` 那条一跑就会暂时改掉真服务的 DACL，**跑之前要点头**。


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

### M24 本机判账（2026-09-03 00:0x–00:3x，YEUNG）

判据脚本这一轮重造：`tools/harness/m24_menu.ps1`（F5 两枚文案，`-Case limited|elevated`）、`tools/harness/m24_menu_live.ps1`（弹活的装机代理的菜单并量它的 rect）、`tools/harness/m24_f6.ps1`（服务 start/stop 往返 + `service.log` 是否继续写）。产物在 `build/scratch/`，结果文件都是 UTF-8 带 BOM、中文原样。

| 条 | 判据 | 读数 | 状态 |
|---|------|------|-----|
| F5 受限 | 该项带「需管理员批准」，且同 pid 日志 `Elevation: no` | `wording: needs-administrator` + `service item present`，弹窗 417x114。**判据读的是目标自报的那行日志，不是屏幕上的字**——`-Shot` 像素腿待补 | ✅ PASS（待像素腿加固） |
| F5 提权 | 该项逐字节旧文案、无后缀 | `wording: plain` + `present`，弹窗 333x114。同上 | ✅ PASS（待像素腿加固） |
| 形状 | "多一项"只多一行高 + 文案宽 | **旧的 93px / Δ21px 一条已撤回**（读不出来了，且当时的截图裁剪比例写错）。换成可复现的那枚：活的装机 1.0.6 代理在钉住锚点下 **317x105**，截图 `build/scratch/m24_menu/live-menu.png` 上 4 项 + 1 分隔线逐项可读、**没有那一项**；宽 417−333=84px = 多出的 7 个汉字 | ✅ 读数成立，但主体是 1.0.6 |
| F6 | `sc start`/`sc stop` 往返×3，≤10s 落定且 `service.log` 有后续行 | 起 9/10/11ms、停 10/12/11ms，每轮 stop 后多写 5 行直到 `MyRemote agent service stopped`，`failures=0`，结束恢复 RUNNING/Auto | ✅ PASS（**装在的是 1.0.6**） |
| 交付链 | 判据用的 exe == 包里的 exe | sha256 `85fb3396a6530eb6…` 三处一致（zip 内 / `build/bin/Release` / 探针副本） | ✅ PASS |
| 探针会说谎吗 | 同一串针打在旧镜像上应当全 False | 装机 1.0.6：`F5_suffix`/`F5_cancel`/`Tray menu built`/`item wording`/`bye unconfirmed`/`last state`/`ACCESS_DENIED` 全 False（`F5_label`/`service stopped` 是历史串，True 才对） | ✅ 对照通过 |
| 不变量 | M24 不把外部面变大 | `agent.exe` 三个 pid 拥有 **TCP 监听 0 个**（全系统 52），唯一服务端是管道 `MyRemoteAgent_TrayProxy_v1`；`config.hpp` 对基线 `d2aa7d8` 零改动；管道无新动词（`tools/harness/m24_invariants.ps1`） | ✅ PASS |
| F1 四岔 | `sc sdset` 摘掉读取权 → 首行 `ACCESS_DENIED (...)` 且 `console session:`/`stations:`/`host:` 照打 | 未跑（要改真服务 DACL，需点头） | ⬜ |
| F4 两腿 | `host said bye` + 门不再固定 ≥1100ms；挂起代理 ≤2500ms 且点名 warn | 未跑（要 1.0.7 当宿主；装在的是 1.0.6） | ⬜ |
| 人在机器前 | 同意一次 / 拒绝一次 | 未跑 | ⬜ |

**顺手抓到的一条 F4 反面证据（不用跑就有）**：`C:\ProgramData\MyRemote\tray-1.log` 同一枚代理、两种死法——22:17:29.953 用户点「退出」那次是 `host said bye; exiting`，00:18:36.883 本轮 `sc stop` 那次是 `host pipe closed; exiting`。所以"外部停服务没有 bye 可说"不是推测，是现场读得出来的两行字。

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

1. **M22 未完工，欠的已由 M24 补上代码；F6 与 F5 已判账（见 §3 的 M24 表），还剩三条现场腿**。1.0.7 四包已随 `8045fc0` 重出且与判据用的 exe sha256 一致；未发版、未打 tag、未放 `D:\IT-share`。**按顺序差的**：
   1. ~~清场~~ **已做**：pid 27124 与卡住的 pid 4936 都不在了，服务现在 `4 RUNNING`/Auto（F6 那一跑的恢复态）。
   2. ~~F6 实测~~ **已做**：1.0.6 三趟往返全部 ≤12ms 落定且日志有后续行——**那 23 分钟不是产品缺陷**，见 TASKS 的 M24-1。剩下的对照（1.0.7 停一次）与第 4 条共用同一个前置。
   3. ~~F5 两枚文案 + 形状~~ **已做**（`tools/harness/m24_menu.ps1`、`tools/harness/m24_menu_live.ps1`）。**唯一没做成的判据**是"服务 RUNNING 时这一项自己消失"：会话级单例锁 + `--force` 的组合让前台探针实例在服务运行时根本起不了托盘，见 §6 最后一条。M21-4 当年在 TEST-WIN 上用"同一实例活着时把服务起起来"绕过去了（菜单 239→195px），那条路在这台机器上要走得连着点两次 UAC。
   4. **前置（挡住第 5、6 两条）**：把 dev 构建放进服务指向的那份——`C:\MyRemote\Agent\agent.exe` 改名让位（**别删**，跑完原名放回），拷入 1.0.7 的 `agent.exe`，`sc start`。这样 1.0.7 才真的当宿主，F4 的新门与"停一次 1.0.7"才有东西可量。**这一条会改掉本机在装的那份二进制，跑之前要点头。**
   5. **F1 四岔**：`sc sdshow` 存原 DACL → `sc sdset` 摘掉读取权 → `--service-state` 必须打 `ACCESS_DENIED (...)` 且 `console session:` / `stations:` / `host:` 三行仍在 → 还原并逐字节比对。**这一条会临时改服务安全描述符，跑之前必须跟用户点头。**
   6. **F4 两腿**：健康腿看 `tray-<会话>.log` 记 `host said bye` 且退出耗时不再是固定 ≥1100ms；挂起腿手动起第二代理、`NtSuspendProcess` 住它再退宿主，判据是总耗时 ≤2500ms、warn 点名那个 session、F3 不破。**触发优雅退场只有两条路**：托盘点「退出」，或让代理往管道写 `quit`（`main.cpp:1709` 的 `sink.quit` → `g_quit_requested` → `main.cpp:1863` 的 `trayproxies::stop()`）。`sc stop` 走的是 `shutdown_host()` 的 `TerminateProcess`，永远量不到这道门——已在 §3 用现场日志实证。
   7. 人在机器前两次：同意一次（服务 RUNNING、下一枚菜单里这一项自己消失）、拒绝一次（弹的是「已取消」而不是「需要管理员权限」，事后 2s 内右键仍能弹菜单）。**注意本机 2026-09-01 实测过 `Start-Process -Verb RunAs` 能静默过**，"拒绝"这一腿可能根本没有框可点——先做 §计划里的 R1 探针确认这台机器的行为，别把"没弹框"当成产品缺陷。
   8. 全绿之后再抬交付：`D:\IT-share\MyRemote-v1.0.7\` + 目标端复算哈希；GitHub release 仍等真机通过。
2. 修完 F3 之后才轮到那两条没跑的注入：① 只连不读的宿主 + ④ 挂住代理托盘线程。脚本已经写好并推到 `C:\M20test\tw.ps1`（`-Test t2` 那段现在写的是"先别跑"，F3 修完改掉那句）。④ 还需要一个挂线程的注入形态，目前没有。
3. 发版之后的既有决定：tag 只打在真正交付过的版本上（v1.0.0、v1.0.4），1.0.1~1.0.3 不再补 tag。
4. 收尾清理**已做**：dev 路径那枚 `HKCU\Control Panel\NotifyIconSettings\2890237777820566933`（`…\build\bin\Release\agent.exe`）的 `IsPromoted` 已删，回读 `<absent>`；`C:\MyRemote\Agent\agent.exe` 那枚**留着**——那是装机版该有的状态，删了图标会掉进折叠区。

## 6. 环境与工具备忘（会杀时间的坑）

**2026-09-03 M24 判据这一轮新增，共同点：坑在仪器本身，不在脚本逻辑。判据脚本这一轮从 `build/`（不在版本控制里）挪进了 **`tools/harness/`**，以下条目的脚本名都指那里。**

- **`#32768` 弹出菜单在这台机器上跨进程读不出内容**（Windows 11 24H2 / 10.0.26200，装机版 1.0.6 的活代理 pid 28204、菜单 hwnd 10750298、`visible=True rect=309x93`）：UIA `FromHandle` 给 `children=0 subtree=1 rawwalk=0`；MSAA `AccessibleObjectFromWindow` 对 `OBJID_WINDOW(0)`、`OBJID_CLIENT(-4)`、`OBJID_MENU(-3)` 全部 `hr != 0`；`SendMessage(hwnd, MN_GETHMENU=0x01B1)` 返回 0。**窗口存在、看得见、量得出 rect，就是读不出里面有什么。** 所以判"菜单画了什么"只能让**画它的那段代码自报**（`TrayIcon::ShowMenu` 与 `start_service_label()` 各写一行 ASCII 日志）。自报不等于自证：**必须同时留一枚独立读数**——最好的那枚是**把弹窗那块屏幕截下来**（`-Shot`，见下下条），几何 rect 只是它的弱替代品（417 vs 333，84px 恰是 7 个汉字）。别再拿 UIA 试第二次。
- **截图裁剪比例不许写死**：`m24_shot.ps1` 早期硬编码 `scale 2`（那台面板在 200%），这台是 144/96=**1.5**，于是"菜单的截图"实际截的是偏了 33% 的另一块屏幕——里面满是别的窗口的字。因为图片照样"满而不空"，这个错**看起来像菜单没画出来**，直接把像素判据错怪成了仪器不成立。**现在每次跑都现场量**：声明 DPI 感知**之前**读 `Screen::PrimaryScreen.Bounds`（2560，虚拟化值）、之后读 `GetSystemMetrics(SM_CXSCREEN)`（3840，物理值），比值即比例，并把 `phys=/logical=/sysdpi=` 一起打出来。
- **截图辅助脚本必须另起进程**：`m24_shot.ps1` 会 `SetProcessDPIAware()`。用 `&` 在同一个 powershell 里调它，**持有坐标的那个进程当场变成 DPI 感知**，之后每一枚 `GetWindowRect`/`SetCursorPos` 都换了一套坐标系。判据脚本一律 `powershell.exe -File ...` 起子进程调它，并检查 `$LASTEXITCODE`。
- **弹窗跟着真光标，锚点必须钉**：`tray_icon.cpp:349-352` 是 `GetCursorPos` 然后在那个点 `TrackPopupMenuEx`。不 `SetCursorPos` 就没法复现同一个 rect（同一份菜单读到过 93 与 105 两种高度），而 Windows 还会把工作区边缘的弹窗往里挤。判据脚本先 `SetCursorPos($AnchorX,$AnchorY)` 再 `PostMessage`，并把锚点写进结果文件。
- **上一条差点是假的**：第一次跑 `m24_menu_probe.ps1` 时 `MN_GETHMENU` 传的是 `0x03B1`（真值 `0x01B1`，那条消息没有任何窗口会答）、`OBJID_WINDOW` 传的是 `-1`（那是 `OBJID_SYSMENU`，`OBJID_WINDOW` 是 `0`）。**两个针脚都错，而错的针脚答起来和"OS 拒绝"一模一样**。修好常量重跑才是上面那组读数。教训：**"三个读法都失败"要先证明三个读法都被正确地调用过**——拿一个已知能读到的目标（同进程内的菜单、或一个普通窗口的 MSAA）当阳性对照，比多写一条注释便宜。
- **服务 RUNNING 时，前台实例没有任何办法带着托盘起起来——`--force` 也不行**：`main.cpp:1503` 那支的判据是 `host_owns_machine = !g_session_host && !args.force && !args.background && installed && running`，**`--force` 恰好把它否成 false**，于是走"给别人的托盘 `PostMessage(WM_SHOW_CONFIG)` 然后 `return 0`"那条，且这一支在 `mlog::init` 之前——现象还是那个熟悉的"秒退、code 0、一行日志都不写"。`--force` 真正能过的只有 `:1589` 那支让位，而那支要在**锁是空的**时候才轮得到。结论：**"服务在跑 → 那一项消失"这类判据不能靠新起一个前台实例来读**；要么用 M21-4 那条路（同一实例活着时把服务起起来），要么直接读活的会话代理（`tools/harness/m24_menu_live.ps1`，它的菜单按构造永远不带那一项）。顺带：`:1592` 那句 `Use --force to override.` 在宿主持锁时是一句办不成的承诺——**本轮没改它**，先记在这里。
- **`finally` 会制造假绿**：`m24_f6.ps1` 第一版在 `try` 开头就抛（`Get-Item 'C'` 那种把单元素管道当数组的错），跳到 `finally` 打印 `failures=0`，**看起来像全绿**。凡是"跑完汇报失败数"的判据脚本，`catch` 必须先把中止原因记进同一个列表再 `throw`，`finally` 只负责恢复现场。同理：**恢复动作（把服务重新起起来）放 `finally`，判定放 `try`**。

**2026-09-02 出 1.0.7 判据脚本这一轮新增，同样每一条都真咬过一次：**

- **`powershell -File x.ps1 -Arr a,b,c` 不会给你三个元素**：`-File` 只交一个 argv，`[string[]]` 把它绑成**长度 1 的数组**，于是脚本拿 `"a,b,c"` 整串去匹配，条条报"缺失"——这是最像"修复没生效"的一种假 FAIL。重复 `-Arr a -Arr b` 也不行（`ParameterAlreadyBound`）。判据脚本的数组入参要么设计成**逗号串 + 脚本内 `-split`**，要么走 `-Command`。
- **`-Pid` 这个参数名不存在**：`$PID` 是 PowerShell 只读自动变量，`param([int] $Pid)` 在脚本体执行**之前**就抛"无法覆盖变量 PID"。改名 `-OwnerPid`。
- **`[char]0x9700 + [char]0x7BA1` 是加法，不是拼接**：得到的是一个整数和再转成的单个字符。非 ASCII 串一律 `-join (@(0x9700,0x7BA1,...) | ForEach-Object { [char]$_ })`。
- **整文件按 UTF-16 解码只解一次会漏一半**：`.rdata` 里的字面量落在**奇数字节偏移**时，offset 0 的解码看不见它 → 又是一个假 FAIL。要么两对齐都解，要么用字节级搜索。另外 **PS 5.1 的 `String.Contains` 走当前区域**，对 CJK 针不是"有没有这段字节"的意思，必须 `IndexOf($needle, [StringComparison]::Ordinal)`。
- **码点针脚写错一个字，"存在"就读成"不存在"**：`需管理员批准` 我少写了 `员`(U+5458)，于是 1.0.7 里明明有的 F5 后缀被判成没做。**判据必须能真的失败**：同一支脚本对**旧装机镜像（1.0.6）**跑一遍，那些应当全是 False——两边一对照，探针自己在不在说谎当场就出来了。这次就是这么发现我自己的针错了，不是二进制错。
- **bash 会把 `-File build\x.ps1` 的反斜杠吃掉**（变成 `buildx.ps1`）。走 `-File` 的路径用正斜杠。
- **会话级单例锁让"起一个探针实例"这条路必须先清场**：`main.cpp:1477` 对非提权、无 `--takeover` 的实例只试 **1 次** `CreateMutexW(MyRemoteAgent_SingleInstance)`；拿不到就走 1499 那支——给**别人的**托盘窗 `PostMessage(WM_SHOW_CONFIG)` 然后 `return 0`。两个后果：① 它早于 1538 的 `mlog::init`，所以**一行日志都不写**，脚本只看到"秒退且 code 0"；② 那次 `WM_SHOW_CONFIG` 若是 Medium→High/ SYSTEM，UIPI 会**悄悄丢掉**（`PostMessage` 照样返回 TRUE）。所以"探针实例起不来"永远先查谁会话里已经有一个 agent；本次跑之前必须先让用户的实例退掉。

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
- 截图脚本必须先 `SetProcessDPIAware()`。**本机的缩放按实测读，别按记忆乘**：`HKCU\Control Panel\Desktop\WindowMetrics\AppliedDPI = 144`（=150%），物理 `3840x2160`，逻辑 `2194x1234`；换算系数取 `GetDpiForSystem()/96`。上一轮这里写的是"200%/3400 宽"，照着它截出来的框偏了一整档。
- 取证文件的根**这台机器已经换了**：M21-6 把服务配置落到 `%ProgramData%\MyRemote\` 之后，YEUNG 上 `agent.log`/`service.log`/`tray-<会话>.log`/`host.status` **全部在 `C:\ProgramData\MyRemote\`**，`C:\MyRemote\Agent\*.log` 实测不存在（2026-09-03 00:20 当场 `ls` 过）。但两 root 都扫的规则**不要撤**——装过老版本、或用便携形态跑的机器仍会把日志写在 exe 旁边；判据脚本要按"两处都在、取 mtime 最新的那枚"来选（`tools/harness/m24_f6.ps1` 就是这么写的）。
- 关键脚本：`build/tw_m20.ps1`（**现场判账那一支**，`-Test t0/t1/t3/t4/t5`，推到目标机改名 `C:\M20test\tw.ps1`；`-AppDir` 默认 `C:\MyRemote\Agent`）、`build/m21_cli_regression.ps1`、`build/m21_probe_config.ps1`、`build/m21_hide_probe.ps1`、`build/m21_menu_shape.ps1`、`build/m21_window.ps1`（把需要停服务的那几跑收进一个提权窗口）、`build/m21_set_server.ps1`、`build/scratch/m21_pipe_smoke.ps1`（不碰产品、只练管道读法）、`build/scratch/m21_cleanup.ps1`（提权收残留）。**这一批 2026-09-03 逐个 `[ -f ]` 实测：一个都不在**，`tw_m20.ps1` 也没了；`build/*.ps1` 只剩 `m23_run_stage.ps1`、`probe_binary.ps1` 与本轮新写的 `m24_*.ps1`。这些名字留着只为说明"当年那批判据查过什么"，**别去找它们跑**——要跑得照本节那些坑重造。这就是 `build/` 不在版本控制里的代价（见 §7 的 git 条）。取证文件两个根都要扫，规则见上一条。
- 远端取证走 `\\10.60.254.153\c$\...`，**ADMIN$ 是可写的**：脚本推过去、转录与截图从同一处拉回来，比来回截图快一个量级。bash 命令层拒绝内联 UNC——写进 `.ps1` 再执行。TEST-WIN 两份配置在 2026-09-01 现场判账之后实测都还是 `10.60.1.188`，权威那份是 `%ProgramData%\MyRemote\config.json`（`tray_icon=true`，mtime 停在装机那晚的 22:59:29，整晚没被写过）。

## 7. 机器快照（2026-09-03 00:30，本会话结束时）

- **YEUNG（本机）**：装的是 **1.0.6**，服务 **`4 RUNNING` / start type=Auto**，binPath=`"C:\MyRemote\Agent\agent.exe" --service`。会话 1 里三个进程：服务 pid 7572（session 0）、宿主 pid 23976、托盘代理 pid 28204（`"C:\MyRemote\Agent\agent.exe" --tray-proxy --no-elevate --config "C:\ProgramData\MyRemote\config.json"`）。**取证根这台机器是 `C:\ProgramData\MyRemote\`**（`agent.log`/`service.log`/`tray-1.log`/`host.status`/`worker.log` 都在这一处，`C:\MyRemote\Agent\*.log` 不存在）。控制端 `10.60.1.188:7500`  reachable，宿主日志 `Registered with server`。桌面 `3840x2160`，编码 1920x1080，`No GPU input format negotiated; hardware encoder disabled` → OpenH264 软编。
- **本机桌面上留了什么**：探针的三枚实例（`build\scratch\m24_menu\agent-{limited,elevated}.exe`）都已随脚本结束退出；两次弹过的 `#32768` 都已实测关闭（`menu_closed=True`）。`build\bin\Release\` 里有两枚**来历要认清**的旧文件：`agent-1.0.5-running.exe`（**FileVersion 实测 1.0.7**，就是卡 23 分钟那个未提交构建，删不删等 §5 第 4 条跑完再定）与 `agent.exe.old`（1.0.6 装机前的备份，不是本轮产物）。
- **TEST-WIN**（`\\10.60.254.153\c$`，只有被控端）：最后一次实测是 2026-09-01 覆盖安装 **1.0.5**，服务 Running/Automatic；M22 现场判账（F1/F2/F4/菜单形状）日志齐全。`C:\M20test\tw.ps1`、证据与 portable 仍保留便于追溯。**这两天的状态本机未复核**。
- **构建产物**：`build/bin/Release` = **1.0.7** 全量（`agent.exe` sha256 `85fb3396a6530eb6…`）；`build/package/dist` = **1.0.7 四包 + LF `SHA256SUMS.txt`**（00:28 重出），1.0.6 四包同目录并存。**`D:\IT-share\MyRemote-v1.0.7\` 仍空**，等 §5 剩下三条腿。
- **git**：`main` = `origin/main` = `8045fc0`。`build/` 整体在 `.gitignore` 里，**这一轮重造的判据脚本（`m24_menu.ps1`、`m24_menu_live.ps1`、`m24_menu_probe.ps1`、`m24_f6.ps1`、`m24_shot.ps1`、`m24_windows.ps1`）都不在版本控制里**——换一台机器或清一次 `build/` 就全没了。要么挪进受跟踪的 `tools/harness/`，要么接受每轮重造（M21/M22 的脚本就是这么丢的）。
