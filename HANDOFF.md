# 开发交接文档（2026-09-03 16:4x，M24 三条欠腿全部有账：**F4 挂起腿量到了门（2500ms 花完 + 1ms 交接 + 2596ms 落定）、F5 提权侧到像素、装机已逐字节还原 1.0.6**；同一跑逼出一条**新缺陷，已确诊**——一个客户端的读写共用同一个非重叠管道句柄，对端不读则该客户端的命令一律进不来，`quit` 随之被当作超时丢弃；1.0.7 仍未发版——**发版卡在"1.0.7 从没上过 TEST-WIN"**）

> 给下一个会话冷启动用。所有"已验"都指当场有输出/截图/退出码；"待验"都给出台账和操作方法。
> 进度史实在 TASKS.md，行为语义在 README.md，面向用户的说法在 packaging/release-notes.md，
> 本文只写**状态、边界、下一步**。

## 1. 发布与交付现状（2026-09-03 15:0x 实测）

| 事项 | 状态 |
| --- | --- |
| GitHub Release | **只有 v1.0.0 / v1.0.4 / v1.0.5 三个**，`latest` = **v1.0.5**（`gh release list` 当场读数）。**v1.0.6 没有 release**——只有 tag。本笔之前这里写的是"v1.0.6 已发布并给出链接"，那句是假账，已按实测更正；写它的时候那个 release 并不存在 |
| tag | `v1.0.0 v1.0.4 v1.0.5 v1.0.6` 四个都在。**v1.0.7 未打**：现场判据没跑完不打 tag（沿用了 1.0.6 那次的教训——tag 比 release 跑得快的结果就是文档没法说清"到底发了什么"） |
| 代码 | `main` 此刻 = **`93d3cfb`**（push 之后与 `origin/main` 一致）。`a96653e` 之后这四笔都在**判据侧**，产品代码一行未动：`21e0391` 交接文档按 15:1x 实测重写、`6208adb` **挂起腿那条恒真断言**（`$ok_unread` 量的 `$byeMs` 在 hung 支从不赋值 → 换成 `ok_full`/`ok_nodrain` 两条真能红的）、`51010f1` `-Phase final`（一窗三棒 + 反向对照）并把"服务必须停着"从 `-Click` 专属升成整支前置、`93d3cfb` **日志毫秒只有 2 位时 `ParseExact(.fff)` 直接抛**（那条异常把 warn→交接 的差值测量整段跳掉了）+ `start type` 读数打成 `$12   AUTO_START` 的 `-replace '$1'` 无组可填 |
| 交付物 | `build\package\dist\` = **1.0.7 四包 + LF `SHA256SUMS.txt`**（实测 `CR=0 LF=4`），**10:20–10:21 随 flush 修复重出**。15:0x 那次"包内 exe == `build/bin/Release/agent.exe` == 装机那份"的三处一致（`846E52EE3AB891FE…`）**判的就是包里那份带 flush 修复的镜像**；16:20 装机路径已还原成 1.0.6，那串 `846E52EE…` 此刻仍能在两处复算：`build/bin/Release/agent.exe` 与 `C:\MyRemote\Agent\agent-swapped-1.0.7.exe`（挪出来没删的那份）。`MyRemote-Agent-v1.0.7-setup.exe` 的 `FileVersion` 实测 **1.0.7**。**`D:\IT-share\MyRemote-v1.0.7\` 还没放**——拦着的不是腿，是 §4 第一条那个新缺陷要不要一起交付 |
| 版本号 | `CMakeLists.txt` 与 `packaging/common.iss` 都是 **1.0.7** |
| 本机现场 | **已还原**：`C:\MyRemote\Agent\agent.exe` = **1.0.6**、`Get-FileHash` 现算 `D0279D07733DA71A…` 与 `build\scratch\m24_swap\installed.sha256` **逐字节相同**，服务 **`4 RUNNING` / `2 AUTO_START`**，托盘代理 16:20:40 重连（`tray-1.log` 有新首行，图标回来了）。换进来的 dev 构建**没删**，`m24_swap -Mode restore` 把它挪成同目录 `agent-swapped-1.0.7.exe`（5493760B）。这一跑出自 `tools/harness/m24_admin_window.ps1 -Phase final`（16:19:47 投窗、16:20:39 收尾，48 秒）。留档：之前卡 1 小时 `STOP_PENDING` 的 pid 4936 = `build\bin\Release\agent-1.0.5-running.exe`，文件名说 1.0.5、`FileVersion` 实测 1.0.7，一个未提交构建的产物 |

**现在的现场条件与判据的关系**：① **装机路径已回到 1.0.6**，`trayproxies::stop()` 的新门在此刻的装机镜像上**没有东西可量**——要再量 F4 任何一支，得先 `m24_swap -Mode swap` 把 dev 构建换回去（一枚提权框），跑完再还原；`build\bin\Release\agent.exe` 与 `C:\MyRemote\Agent\agent-swapped-1.0.7.exe` 两份 `846E52EE…` 都还在，换回去不需要重新构建。② 挂起腿的读数**已进账**（§3 那两行：门花完 2500ms、1ms 后交接、2596ms 落定），10:47 那份 `f4-hung.txt` 已改名 `f4-hung.stale-1047.txt` 留作"坏仪器长什么样"的对照。③ **一枚框就够一整窗**：16:19:47 从非提权 shell `Start-Process -Verb RunAs` 投窗，3 秒起来；窗内 `m24_menu.ps1` 再对探针 `-Verb RunAs`（`:290`）**没有第二次弹框**——已提权的父进程起提权子进程不再问。这条把 §6 里"本机 `-Verb RunAs` 能静默过"和"那枚框被取消过"两句之间的含糊抹掉了：**框只在投窗那一次**。④ **托盘腿仍然必须在人看着的那个会话里跑**：这台机器的 console 槽是空的 session 3、人在 RDP session 1；本轮探针（pid 12424）落在 session 1 才拍得到照片。详见 §6 与 §7 快照。


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

### M24 本机判账（2026-09-03 00:0x 起、至 16:2x，YEUNG）

判据脚本全部在 **`tools/harness/`**（受版本跟踪）：`m24_menu.ps1`（F5 文案 + `-Click approve|decline` 那条人要点的腿）、`m24_menu_live.ps1`（弹活的装机代理的菜单并量它的 rect）、`m24_shot.ps1`（另起进程截屏、现场量 DPI 比例）、`m24_f6.ps1`（服务 start/stop 往返 + `service.log` 是否继续写）、`m24_f4.ps1`（自己当一枚代理客户端，健康腿与挂起腿）、`m24_f1_acl.ps1`（提权半：改 DACL 并回读）+ `m24_f1_caller.ps1`（非提权半：等信号后跑 `--service-state`）、`m24_swap.ps1`（把 dev 构建换进装机路径 / 原名还原）、`m24_invariants.ps1`（外部面）。**需要令牌的那几跑收在 `m24_admin_window.ps1` 的三段里**：`-Phase measure`（换镜像→起服务→f4-healthy→f4-hung→f1-acl→故意留服务停着给 F5 腿）、`-Phase restore -RerunHung`（复测挂起腿→还原 1.0.6）、**`-Phase final`（16:19 那一跑用的就是它：反向对照→挂起腿→停服务拍提权菜单→还原，四棒共用一枚框；哪一棒红都照样还原，但整支 `exit 1`）**。产物在 `build/scratch/`，结果文件都是 UTF-8 带 BOM、中文原样。

| 条 | 判据 | 读数 | 状态 |
|---|------|------|-----|
| F5 受限 | 该项带「需管理员批准」，且同 pid 日志 `Elevation: no` | 日志：`Tray menu service item wording: needs-administrator (clicking it asks UAC)` + `Tray menu built: 6 entries, service item present`，同 pid 首行 `Elevation: no`。**像素腿已补**：`build/scratch/m24_menu/menu-limited-approve.png`（钉住锚点 1200,400）上逐项可读 `打开配置 / 停止远程控制（本机不再被远控） / — / 启用后台服务（当前：前台运行，开机不自启；需管理员批准） / 隐藏托盘图标 / 退出`——**后缀是屏幕上读出来的，不是自报的**。弹窗 rect 三跑三种读数（417x114 / 425x128 / 412x107，同一枚 exe、同一锚点）——**原因是这台机器的会话 DPI 按连接变（144→192），不是噪声**，见 §6；所以承重的读数是那张照片，不是几何 | ✅ PASS |
| F5 提权 | 该项逐字节旧文案、无后缀 | **像素腿已补**（16:20，`-Phase final` 的 c 棒，探针 pid 12424、跑的是 `build\bin\Release\agent.exe` = `846E52EE…` 那份带 flush 修复的镜像）：`build/scratch/m24_menu/menu-elevated.png`（512x192，截图仪器现场量到 `phys=3840 logical=2560 sysdpi=144 scale 1.5`）上逐项可读 `打开配置 / 停止远程控制（本机不再被远控） / — / 启用后台服务（当前：前台运行，开机不自启） / 隐藏托盘图标 / 退出`——**后缀在屏幕上读不出来，它就不在**。同 pid 日志 `Elevation: yes` + `item wording: plain` + `Tray menu built: 6 entries, service item present`，报告里带 `service_state_at_start=STOPPED`（那一项存在的前提，从 16:20 起由脚本自己写在账上）。与受限侧那张（同一锚点、同一镜像、**多**「需管理员批准」）正好成对 | ✅ PASS |
| 形状 | "多一项"只多一行高 + 文案宽 | **旧的 93px / Δ21px 一条已撤回**（读不出来了，且当时的截图裁剪比例写错）。换成可复现的那枚：活的装机 1.0.6 代理在钉住锚点下 **317x105**，截图 `build/scratch/m24_menu/live-menu.png` 上 4 项 + 1 分隔线逐项可读、**没有那一项**。**"417−333=84px 恰是 7 个汉字"那句算术一并撤回**——同一枚受限菜单三跑的 rect 是 417/425/412，差值本身就比要量的东西大，形状判据只能靠"两张照片上逐项可读 + 一张有那一项一张没有" | ✅ 读数成立，但主体是 1.0.6 |
| F6 | `sc start`/`sc stop` 往返×3，≤10s 落定且 `service.log` 有后续行 | 起 9/10/11ms、停 10/12/11ms，每轮 stop 后多写 5 行直到 `MyRemote agent service stopped`，`failures=0`，结束恢复 RUNNING/Auto | ✅ PASS（**装在的是 1.0.6**） |
| 交付链 | 判据用的 exe == 包里的 exe | **15:0x 复算：sha256 `846E52EE3AB891FE…` 三处一致**（`MyRemote-Agent-v1.0.7-portable.zip` 里取出的一份 / `build/bin/Release/agent.exe` / 装机路径 `C:\MyRemote\Agent\agent.exe`）。包是 10:21 重出的 → **这条链上的镜像带 flush 修复**；凌晨那组 `85fb3396…` 的读数作废（那是修复前的构建） | ✅ PASS |
| 探针会说谎吗 | 同一串针打在旧镜像上应当全 False | 装机 1.0.6：`F5_suffix`/`F5_cancel`/`Tray menu built`/`item wording`/`bye unconfirmed`/`last state`/`ACCESS_DENIED` 全 False（`F5_label`/`service stopped` 是历史串，True 才对） | ✅ 对照通过 |
| 不变量 | M24 不把外部面变大 | `agent.exe` 三个 pid 拥有 **TCP 监听 0 个**（全系统 52），唯一服务端是管道 `MyRemoteAgent_TrayProxy_v1`；`config.hpp` 对基线 `d2aa7d8` 零改动；管道无新动词（`tools/harness/m24_invariants.ps1`） | ✅ PASS |
| F1 四岔 | `sc sdset` 摘掉读取权 → 首行 `ACCESS_DENIED (...)` 且 `console session:`/`stations:`/`host:` 照打 | 原 DACL 含 `(A;;CCLCSWLOCRRC;;;IU)`；`sdset` 后**回读**该 IU ACE 已消失（断言读的是 `sc sdshow` 的回读，不读 `sc` 那句话——本机中文系统打的是「成功」，认 `SUCCESS` 会假红）；受限 token 跑 `--service-state` 打 `MyRemoteAgent: ACCESS_DENIED (the service may be installed; this token cannot read it)`，而 `console session: 1 (console station)`、`stations: 0 Services(4); 1 Console(0); 65536 RDP-Tcp(6)`、`host: STALE(...)` **三行照打**；跑完描述符**逐字节还原 = True**。反面基线（完整 DACL 时同一条命令打 `MyRemoteAgent: STOPPED` + binPath + start type）也已跑，否则这条判据是空转 | ✅ PASS |
| F4 健康腿 | `host said bye` + 门不再固定 ≥1100ms | 仪器自己当一枚代理客户端连上真宿主（1.0.7）：`bye` 到线上 **201ms**、SCM `STOPPED` 在 `quit` 后 **166ms**、真代理 `tray-1.log` 打 `Tray proxy: host said bye; exiting`，四条断言全 True，收尾服务 0ms 回 Running | ✅ PASS |
| F4 挂起腿（门） | 挂住不读的代理：门花完 2500ms、之后立刻交接、总耗时框住 | **入账，读数是反向对照那一跑给的**（16:19:51，`-PumpLimitSeconds 1` → 缓冲区只到 `avail=525/4096`，可这支客户端**照样一次都没读过管道**，而门看的就是"有没有人确认"）：`16:19:56.085 WARN Tray proxies: session 1 bye unconfirmed at 2500ms (still connected); its tray log will read "host pipe closed"` → `16:19:56.086 INFO cutting the pipe to session 1 (host shutting down)` = **1ms 交接**；`service STOPPED 2596ms after quit`（一把秒表从 `quit` 走到 SCM `Stopped`）。断言四条全 True（`ok_full` 这一条**应当** False，见下一行）。注意 warn 的**死因文案与 10:47 那跑不同**：这里是 `(still connected)`，10:47 是 `(write failed)` | ✅ PASS |
| F4 灌满分支 | 把对端缓冲区真灌满（`avail=4139/4096`）会怎样 | **仪器能红，产品翻车**：16:20:02 那一跑 `ok_full True`、`ok_nodrain True`（4139→4139 一次没泄），但 **`quit` 根本没被宿主处理**——`16:20:17.929 cutting the pipe to session 1 (no pong)` + `session 1 closed (reason=we cut it, supervisor reaped it)`，监管线程 6 秒后把这个客户端当死的收了，**SCM 在 25000ms 内始终没到 STOPPED**（`stoppedMs=-1`）。同一支脚本 10:47 灌满时却走到了 `bye unconfirmed at 2500ms (write failed)` + 同毫秒 `cutting the pipe`。（这一跑的报告里还夹着一行 `ERROR: ...ParseExact...`——修在 `93d3cfb`，晚于这一跑；它跳过的是 delta，与这一行的两条 False 无关） **两条当时下的结论已于 16:4x 撤回**：①"不是确定性的"错——两跑的差别只是 `quit` 落在灌满之前还是之后；②"25s 停不下来"里的 25s 是脚本的等待上限，不是产品门限。真机制与三阶段复判见 §4 第一条（跑在装机的 1.0.6 上，**不是 M24 带来的**）。这条不在 M24 的判据里 | ⚠️ 新缺陷，已确诊，未修 |
| 人在机器前 | 同意一次 / 拒绝一次 | **拒绝**：11:06 那一跑他点了「否」→ 同 pid 新增 `Service enable declined at the consent prompt; nothing changed`、SCM 全程 `STOPPED`、且屏幕上真出现了 `已取消，后台服务没有启动。` 那枚框（340x187，`notice-decline.png` 里逐字可读）。**同意**：14:50 那一跑 → +5667ms `START_PENDING`、再 +262ms `RUNNING`，同 pid 打 `The background service is running; this instance is handing the machine over`，**不弹框**。两跑之间：14:34 那次他按了「否」而腿判 `FAIL: the menu item was there but did not do what it says (click=approve)`——**这条 FAIL 是对的**，那枚腿就是要在"点了否却没办成"时变红；更早还有一跑 `waited 180188ms … noticed=False`，因为人当时不在机器前 | ✅ PASS（两向都有像素） |

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

1. **一个客户端的读与写共用同一个非重叠管道句柄：对端一旦不读，这个客户端的命令就再也进不来**（2026-09-03 16:4x 确诊，仪器 `tools/harness/m24_wedge.ps1`，报告 `build\scratch\m24_wedge\wedge.txt`）。
   - **机制**：`client/src/tray_proxies.cpp:344-345` 给每个客户端起 `reader_loop` 与 `writer_loop` 两个线程，**共用一个 `c->pipe`（`PIPE_WAIT`，没有 OVERLAPPED）**。对端不读 → 管道缓冲写满 → 写线程停在 `WriteFile`（`:205`）→ **同一句柄上的 `PeekNamedPipe`/`ReadFile`（`:260`/`:268`）一起被钉住**。这正是这个文件开头承诺不会发生的事（"nothing on the host's own threads may block waiting for a proxy"），也与 `queue_line` 上那句"一个不排空的代理只该拖住它自己的队列"相反。
   - **三阶段读数**（同一台机器、同一个宿主，一个变量＝这条连接的入站缓冲满不满）：**A** 缓冲 760B 时发 8 次 toggle → 宿主日志 **8/8**，另发的 1 条垃圾标记也读到（标记只要读侧就答复，所以它兼作"这条查询能报非 0"的证明）；**B** 灌到 4165B、先 settle 1.5s，再发 **6 次 toggle + 3 条标记** → 宿主日志 **0 toggle、0 标记**——不是 pause 处理块住，是**一个字节都没读走**；**C** 把缓冲排空 → **6/6 toggle + 3/3 标记两秒内全数落地，顺序完好** → 命令一直躺在管道里，读者只是被钉住，松开就来。
   - **后果**：这条连接上已写下却没人读的 `quit`（现场就是托盘点"退出"那一下），会在 `kPongTimeoutMs=8000` 之后被监管按 `no pong` 收走，**连同积压的命令一起丢**。用户看到的是"点了退出，图标闪一下又回来（宿主马上又拉起一个代理），服务照旧在跑"。16:20 那一跑的 `stoppedMs=-1` 就是这个形状：**不是停得慢，是根本没轮到停**——25s 是脚本自己的等待上限，先前写成"服务 25s 停不下来"是把仪器当成了产品。同理，"同一前提两种结局/不是确定性的"也撤回：两跑的区别只是 `quit` 落在灌满之前还是之后，机制本身是确定的（B 阶段两次都是 0）。
   - **还开着的边界**：① **不是 M24 带来的**——本次跑在装机的 **1.0.6** 上复现，且这段读/写线程结构自 `edf3cfa`（M20-2）起未变、`git show v1.0.6` 里逐字相同，M24-4 只动了 `stop()` 的等待；② **真实代理会不会把缓冲灌满，没量过**——代理侧是 `PeekNamedPipe`+`ReadFile`+`Sleep(150)` 的轮询循环（`tray_proxy.cpp:251-308`），只要它在轮询就灌不满，所以要它**同时**卡在别处（模态菜单、配置框、CPU 饿死）**且**宿主话多（隧道反复掉线让 `broadcast_state` 停不下来）。窄，但不是零，而且失败方式是"静默吞掉一次用户点击"；③ 修法要先定形：给写线程单独 `DuplicateHandle` 一份句柄（三行，且本条腿可直接复判），还是两端改重叠 I/O（动整个文件的 I/O 形状）。
2. **菜单跟随延迟 = 一轮主循环**。控制端连不上时那一轮里含 10s 连接超时，实测 13s（隧道正常时 ≤1.2s）。这不是缓存 bug，是循环结构；要压到 1s 内只能把形状采样挪出主循环（新线程或独立定时器线程），本轮刻意没做——M20 的全部教训就是往消息线程上放慢调用。
3. **`state` 行刻意没加 `config_ok=0/1`**：修好引号之后，"代理读到的是不是真配置"已经由代理自己那行 `Tray proxy config: <path> (found|missing)` 与开窗/拒绝行为说清，再加一个字段只是多一处会各自说谎的读数。若现场仍判断不了，再补。
4. **管道 DACL 是全机交互用户**（`(A;;GRGW;;;IU)`），不是"该会话用户"——文档本轮已改口。所以 `secret_key`/`control_password` 绝不推上管道；`save_config` 走的是宿主自己那份真配置。任何本地进程仍能连管道发 `pause/hide/quit`，这是定案（托盘是操作入口，不是权威凭据）。
5. **`pause`/`resume` 文案的乐观翻转**（老账）：菜单文案按本地缓存决定，宿主恰好同毫秒改状态会差一拍，症状轻，真机复测时留意。
6. **混合版本共存**：1.0.4 的代理与 1.0.2/1.0.3 宿主共存时，`saved ok/fail` 应答老宿主不会发 → 代理配置窗等 3s 后按"没落盘"报失败（保守方向，不会说谎说成功）。覆盖安装的瞬间窗口期没测过。
7. 老账（TASKS.md 末节）：隧道随宿主重启闪断 ~2s、弱机帧率、多显示器。

## 5. 下一步工作（按顺序）

1. **M24 的现场腿全部有账，机器已还原**。16:19:47→16:20:39 那一枚提权窗（`m24_admin_window.ps1 -Phase final`）把欠的三条一起收掉，**产品代码一行未动**（见 §1 代码行那四笔）。**按顺序差的**：
   1. ~~清场~~ **已做**：卡住的 pid 4936 与 27124 都不在了。
   2. ~~F6 实测~~ **已做**：1.0.6 三趟往返全部 ≤12ms 落定且日志有后续行——**那 23 分钟不是产品缺陷**，见 TASKS 的 M24-1。**1.0.7 的对照没单独跑 `sc stop`**，但 F4 健康腿实际上就是"让 1.0.7 宿主退场"，166ms 落定。
   3. ~~F5 两枚文案 + 形状~~ **两侧都到像素了**：受限侧 `menu-limited-approve.png`（带「需管理员批准」）、提权侧 `menu-elevated.png`（16:20，同一锚点、同一镜像，**没有**那串后缀），两张放一起就是这条判据的全部。**"服务 RUNNING 时这一项自己消失"这一判据本机依旧没做成**：会话级单例锁 + `--force` 的组合让前台探针在服务运行时起不了托盘，见 §6 那条；能做的替代是 `m24_menu_live.ps1` 读活的会话代理（它的菜单按构造永远不带那一项，且那台是 1.0.6）。
   4. ~~前置：把 dev 构建放进服务指向的那份 + 还原~~ **两头都做完**：16:20:39 `m24_swap -Mode restore` 打印 `installed sha=D0279D07733DA71A expected=D0279D07733DA71A byte-identical=True`、`failures=0`，服务回 `RUNNING`/`AUTO_START`，dev 构建挪成 `agent-swapped-1.0.7.exe` 留在同目录（**没删**），托盘代理 16:20:40 重连。
   5. ~~F1 四岔~~ **已做**（`m24_f1_acl.ps1` + 非提权那半 `m24_f1_caller.ps1`），描述符已逐字节还原，不用再点头。
   6. ~~F4 挂起腿复测~~ **已做，而且比预期多问出一件事**：门本身入账了（16:19 那一跑：`bye unconfirmed at 2500ms (still connected)` → 1ms 后 `cutting the pipe` → `STOPPED 2596ms`，见 §3）。**同一跑在"缓冲区真灌满"那一岔里翻出来一条新缺陷**——宿主在这条连接上的读侧一起被钉住，`quit` 丢失。**16:4x 已确诊**（`tools/harness/m24_wedge.ps1` 三阶段 A/B/C，跑在装机的 1.0.6 上，不用提权、跑完服务仍 RUNNING、`paused=0`、`proxies=1`）：机制是 `reader_loop`/`writer_loop` 共用一个非重叠句柄，**不是 M24 带来的**，详见 §4 第一条。**这一条要不要在发版前处理，是个决定，不是个任务**。
   7. ~~人在机器前两次~~ **已做**：同意一次（`START_PENDING`→`RUNNING` + 交接那行日志）、拒绝一次（「已取消」框有照片 + `declined at the consent prompt` + SCM 全程 STOPPED）。判据现在会在"点了否却没办成"时真变红（14:34 那一跑就是）。
   8. **剩下的只有两件，都不欠提权框**：① `D:\IT-share\MyRemote-v1.0.7\`（四包 + `SHA256SUMS.txt`，版式照 `MyRemote-v1.0.6\`）+ 目标端复算哈希——**点头才放**，且放之前先读 §4 第一条；② **1.0.7 上 TEST-WIN 跑一轮现场腿**——这才是 `v1.0.7` tag 与 GitHub release 的门槛（1.0.5 之后那台机器再没复核过，见 §7）。
2. 修完 F3 之后才轮到那两条没跑的注入：① 只连不读的宿主 + ④ 挂住代理托盘线程。**"只连不读"这一形状今天从代理那一侧已经跑过一次**（16:20 挂起腿：客户端连上、一次都不读、还把缓冲灌满），结果就是 §4 第一条——所以①剩下的半边是**宿主作为客户端去连、不读**那一侧，④ 还需要一个挂线程的注入形态，目前没有。脚本已经写好并推到 `C:\M20test\tw.ps1`（`-Test t2` 那段现在写的是"先别跑"，F3 修完改掉那句）。
3. 发版之后的既有决定：tag 只打在真正交付过的版本上（v1.0.0、v1.0.4），1.0.1~1.0.3 不再补 tag。
4. 收尾清理**已做**：dev 路径那枚 `HKCU\Control Panel\NotifyIconSettings\2890237777820566933`（`…\build\bin\Release\agent.exe`）的 `IsPromoted` 已删，回读 `<absent>`；`C:\MyRemote\Agent\agent.exe` 那枚**留着**——那是装机版该有的状态，删了图标会掉进折叠区。

## 6. 环境与工具备忘（会杀时间的坑）

**2026-09-03 16:4x（确诊 §4 第一条，新腿 `tools/harness/m24_wedge.ps1`）三条：**

- **想判"有没有人读我的字节"，就发一条宿主会原样复述的垃圾**：`dispatch()` 对认不出的命令打 `unknown command from session N: <line>`——这行**只需要读侧**就能出现，不经过任何命令处理器。于是"1 条 toggle 都没落地"到底是读者没读、还是读完卡在下游，一次跑就分得开（本次：0 toggle + **0 标记** = 读者一个字节都没读走）。顺手还满足了"期望 0 的查询要先证明它能报非 0"：同一条标记在 A 阶段报 1/1。
- **取日志 offset 之前先 settle**：灌满那一瞬，pump 自己最后一发 toggle 还在路上，不等一下就把这条**不属于灌满窗口**的落地算进去，读数成了 `2/6`（真值 `0/6`）。这类偏差**一律往"温和"的方向偏**，也就是最容易放走真缺陷的方向。修完第二次跑就是 0/6。
- **"同一前提两种结局"要先怀疑前提没对齐**，别急着记成"不确定性"：16:20 与 10:47 两跑结论不同，当时的解释是"这一岔不是确定性的"，而真正的差别只是 `quit` 落在缓冲灌满**之前还是之后**。同理，**仪器自己的等待上限不是产品门限**——`$StopTimeoutMs=25000` 写出来的"25s 停不下来"读起来像产品有个 25s 的等待，其实产品那侧的界是 `kPongTimeoutMs=8000`。
- 顺带两条便宜事实：① **这条腿不用提权**——管道 DACL 给交互用户读写，只要不发 `quit`，就能在装机的 1.0.6 上直接量活宿主；凡是能花 0 枚框量到的东西，就不要预订那一枚框。② PowerShell 函数里 `$tick += 1` 造的是**局部变量**（要 `$script:tick`），于是每次都发 `pong tray=0`，宿主按 `tray pump not turning` 收人——测的就不是产品而是作用域规则了。

**2026-09-03 傍晚这一轮（`-Phase final`，16:19–16:20）新增。共同点还是老一句：坑在仪器身上，而这一轮的四条都是"看着像量过了"：**

- **又一处恒真断言**：`m24_f4.ps1` 挂起支的 `$ok_unread = ($byeMs -lt 0)`——`$byeMs` 在 `:149` 初始化成 `-1` 之后**只在 healthy 支赋过值**，挂起支根本不读管道，所以这条永远 True，"这个客户端没读到 bye"从 M24-4 立那条判据起**一次都没被量过**。换成两条能真红的：`ok_full`（quit 时 `avail>=4000`，原来只写一句 NOTE 的"缓冲没灌满"升成硬前置）+ `ok_nodrain`（quit 后再 `PeekNamedPipe` 一次，avail 不许掉——peek 不消费，所以既证伪不了挂死又能量到"没人读过"）。
- **异常会伪装成跑完**：日志时间戳的毫秒位 `log.cpp:41-42` 只补一个 0（`.83` = 83 毫秒，永远凑不满 3 位），而 `ParseExact` 用的是 `'yyyy-MM-dd HH:mm:ss.fff'` → **两位的行必抛**。抛在 delta 那段里，于是 warn→`cutting the pipe` 的差值**整段没算**，可报告里四条 `ASSERT ... -> True/False` 照样打印，只在中间留一行 `ERROR: ???3???????ParseExact...`（GBK 控制台把中文咬成一串问号）。**看结果文件时先扫有没有 `ERROR:` 行混在 ASSERT 中间**，别数 ASSERT 的个数。修完拿现成的 `f4-hung-control.txt` 离线复算：`16:19:56.085 → .086` = **1ms**。
- **`-replace` 里写 `'$1'` 而模式没捕获组** = 把替换串原样打进读数：窗口那两行 `start type:` 打出来是 **`$12   AUTO_START`**。改成 `if ($q -match 'START_TYPE\s+:\s+(\d+)\s+(\w+)') { "$($Matches[1]) $($Matches[2])" }`。**读数长得像坏了的格式串，就是读数在说自己没被正确算过**——别猜，直接看代码那行。
- **反向对照要放在真跑之前，而且它的 FAIL 就是判据**：`-PumpLimitSeconds 1` 那一跑不可能灌满缓冲，所以 `ok_full` 必须 False、整棒必须 `exit=1`；`m24_admin_window.ps1 -Phase final` 把"对照没红"直接写成整窗失败（`CONTROL DID NOT GO RED`），因为**一件不会失败的仪器会把后面两棒一起染成假绿**。顺带：这一跑顺手量到了门的读数（见 §3），"故意做不到的那一跑"不只能自证仪器，还常常是最干净的一次测量。
- **一枚框够一整窗**（把 §6 里"能静默过"与"那枚框被取消过"两句之间的含糊抹掉）：16:19:47 从非提权 shell `Start-Process -Verb RunAs` 投窗，3 秒起来；窗内 `m24_menu.ps1:290` 对探针再 `-Verb RunAs` **没有第二次弹框**。所以"要人点框"的腿该合并成一窗，别按腿数预约时间。
- **"红也照样还原"的代价是重试要再花一枚框**：`-Phase final` 把还原放在最后一棒且无条件执行（机器不该留在换进去的 dev 镜像上过夜），于是任何一棒红了想原地重试，都得重新约一次提权窗。**先修仪器再投框**——本轮那两条如果留在窗里发现，就是两枚框的钱。

**2026-09-03 白天这一轮（需要令牌的那几腿）新增，每一条都真咬过一次：**

- **后台任务可能几小时之后才开始跑**：一支 `run_in_background` 的判据实际启动时刻比投出去晚了 **~3.4 小时**。凡是"要人在机器前点一下"的腿，**一律前台跑**——不然你喊他看屏幕、框三小时后才弹。
- **会话拓扑决定托盘腿能不能量**：这台机器人在 **RDP session 1**，**console 槽是一个空的 session 3**，宿主此刻就落在 session 3（pid 28068）而它照样给 session 1 生代理（pid 5604）。agent 自己就把这句打在首行：`Session 1; console runs session 3 - this agent is not in it`。**"看不见图标"先查会话，别查产品**。
- **bash 管道会吞掉 PowerShell 的退出码**：`powershell ... | tr -d '\r'` 让一支抛了 `throw` 的腿报 `exit=0`——**假绿**。判据跑完要么读它自己写的结果文件，要么把 `$LASTEXITCODE` 落进文件再看。
- **断言必须能真的失败，这一条本轮抓出两处**：① `m24_f4` 挂起腿先"按住"再"等"，于是量的是**自己的轮询**而不是那道门，读数 `stopped 1ms after quit`（而 warn 实际在 2500ms 才响）——改成**一把秒表从 `quit` 走到 `STOPPED`**，并把"花完 deadline"与"deadline 后 ≤200ms 交接"拆成两条、后者从宿主日志时间戳算；② `m24_f1_acl` 认 `sc` 打印的 `SUCCESS|120`，中文系统打的是「成功」→ **改判效果**：`sc sdshow` 回读那条 IU ACE 是否真没了。
- **还原动作不许删掉"原主"的替身**：`m24_swap -Mode restore` 第一版对装机路径那份直接 `Remove-Item`，而它正被服务当镜像开着 → `访问被拒绝`，整跑中止、机器留在"装着 1.0.7"的状态。现在是 **move-aside**：把换进来的那份改名挪走（同名先挪），再把原主放回原名。**删不是还原，挪才是。**
- **UAC 同意框在安全桌面上：别的进程看不见、拍不到、按不动**。所以"人在机器前"那条腿**不能判框**，只能：把菜单留着 → 等 → 判**后果**。后果要读两样：目标自己那份日志**新增的字节**（记下 `log_bytes` 偏移再 `Tail-From`，别整个文件重扫——历史行会让任何断言恒真），以及 SCM **落定后**的状态（`START_PENDING` 不是答案：第一版把"不再是 STOPPED"当成成功，会在服务还没起来时就报绿）。
- **这台机器的 DPI 会按连接变，于是"逻辑像素"跨跑不可比**：同一枚受限菜单、同一锚点、同一 exe，三跑 rect = 417x114 / 425x128 / 412x107。原因查出来了——截图仪器每次现场量的比例不一样：11:0x 两跑是 `sysdpi=144 phys=3840 logical=2560 scale 1.5`，14:34 那跑是 `sysdpi=192 phys=3400 logical=1700 scale 2`，而 `HKCU\Control Panel\Desktop\WindowMetrics\AppliedDPI` 至今仍读 **144**。**RDP 会话的 DPI 是按连接协商的，不读注册表那一枚**。结论：① 任何"宽差 = 几个字"这类跨跑算术都不许进判据；② 裁剪比例**必须每跑现场量**（`m24_shot.ps1` 已经这么做了，这就是为什么照片一直是对的）；③ 文案这种判据只有**照片**够格承重。

**2026-09-03 M24 判据这一轮新增，共同点：坑在仪器本身，不在脚本逻辑。判据脚本这一轮从 `build/`（不在版本控制里）挪进了 **`tools/harness/`**，以下条目的脚本名都指那里。**

- **`#32768` 弹出菜单在这台机器上跨进程读不出内容**（Windows 11 24H2 / 10.0.26200，装机版 1.0.6 的活代理 pid 28204、菜单 hwnd 10750298、`visible=True rect=309x93`）：UIA `FromHandle` 给 `children=0 subtree=1 rawwalk=0`；MSAA `AccessibleObjectFromWindow` 对 `OBJID_WINDOW(0)`、`OBJID_CLIENT(-4)`、`OBJID_MENU(-3)` 全部 `hr != 0`；`SendMessage(hwnd, MN_GETHMENU=0x01B1)` 返回 0。**窗口存在、看得见、量得出 rect，就是读不出里面有什么。** 所以判"菜单画了什么"只能让**画它的那段代码自报**（`TrayIcon::ShowMenu` 与 `start_service_label()` 各写一行 ASCII 日志）。自报不等于自证：**必须同时留一枚独立读数**，而这台机器上唯一够格的独立读数就是**把弹窗那块屏幕截下来**（`-Shot`，见下下条）。当时把"几何 rect"当弱替代品用（"417 vs 333，84px 恰是 7 个汉字"），**那句已撤回**——同一枚菜单三跑三种 rect，几何根本不复现，别再拿它当读数。也别再拿 UIA 试第二次。
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

## 7. 机器快照（2026-09-03 16:2x，本会话结束时）

- **YEUNG（本机）**：装机路径**已还原**——`C:\MyRemote\Agent\agent.exe` = **1.0.6**、`Get-FileHash` 现算 `D0279D07733DA71A…` = `build\scratch\m24_swap\installed.sha256`（逐字节），服务 **`4 RUNNING` / `2 AUTO_START`**，binPath 不变。同目录另有 **`agent-swapped-1.0.7.exe`**（5493760B，`846E52EE…`，换进来又被挪出去的那份，**没删**）与 `unins000.exe`；`agent-installed-1.0.6.exe` 这个名字**已经没有了**（它回到了原名）。
- **会话拓扑与 15:1x 那份快照不同，别照旧账推**：`agent.exe --service-state` 现读 `console session: 1 (console station)`、`stations: 0 Services(4); 1 Console(0); 65536 RDP-Tcp(6)`、`host: pid=13872 session=1 desktop=Default capture=dxgi size=1920x1080 registered=1 paused=0 proxies=1`——**console 槽此刻就是人所在的那个活动会话，不再是空 session 3**；进程树当场读数：服务 **pid 28200（s0）** → 会话宿主 **pid 13872（s1）** → 托盘代理 **pid 7396（s1）**。`query session`：`console YLW 1 活动`、`services 0 断开`、`rdp-tcp 65536 监听`。抓屏尺寸也从 3840x2160 变成 **1920x1080**。**"看不见图标先查会话"这条规则不变，但每次都要现查**——15:1x 那份快照（console=空 session 3、人在 rdp-tcp#0）到 16:2x 已经不成立。
- **取证根**仍是 `C:\ProgramData\MyRemote\`（`agent.log`/`service.log`/`tray-1.log`/`host.status`）。控制端 `10.60.1.188:7500` 可达（`registered=1`）。
- **本机桌面上留了什么**：16:20 那一跑的探针（`build\scratch\m24_menu\agent-elevated.exe`，pid 12424）随脚本退出，`#32768` 由脚本自己关掉并回读过；`build\bin\Release\` 里那两枚来历不明的旧文件仍在（`agent-1.0.5-running.exe`，FileVersion 实测 1.0.7；`agent.exe.old`，1.0.6 装机前的备份），**都不是本轮产物，删不删等 §5.8 那两件定了再说**。
- **TEST-WIN**（`\\10.60.254.153\c$`，只有被控端）：最后一次实测仍是 2026-09-01 覆盖安装 **1.0.5**；本轮从非提权 shell `Test-Connection` 通、`Test-Path \\…\c$\M20test` = **False**——**这条 False 不能当"够不到"记账**（ADMIN$ 要提权侧才看得见，见 §6 那条）。**1.0.7 从没上过这台机器**，这就是 §5.8 ② 那道门槛。
- **构建产物**：`build/bin/Release/agent.exe` = **1.0.7**，sha256 `846E52EE3AB891FE…`；`build/package/dist` = **1.0.7 四包（Agent/Server × portable/setup）+ LF `SHA256SUMS.txt`**（10:20–10:21 重出）+ 1.0.6 四包同目录并存。**`D:\IT-share\` 上至今只有到 `MyRemote-v1.0.6\` 为止**——没有 `MyRemote-v1.0.7`，也没有记忆里那两个 `1.0.7/1.0.8` 文件夹（本轮 `ls` 实测它们**不存在**，那条记忆已撤）。
- **git**：`main` = `origin/main` = 本笔（§1 代码行那四笔之后）。判据脚本都在受跟踪的 `tools/harness/`；`build/scratch/` 下的产物仍不跟踪，所以**要留证据就往 `tools/harness/` 旁放，或直接引用日志里的时间戳**——本轮的三份承重证据是 `build/scratch/m24_menu/menu-elevated.png`、`build/scratch/m24_admin/f4-hung-control.txt`、`build/scratch/m24_swap/journal.txt`，它们**不在版本控制里**，下一个会话若要看得趁早。
