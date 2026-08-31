# 开发交接文档（2026-08-31 晚，M20 收口时点）

> 给下一个会话冷启动用。所有"已验"都指有当场证据；"待验"都给出台账和操作方法。
> 进度史实在 TASKS.md，行为语义在 README.md，本文只写**状态、边界、下一步**。

## 1. 发布与交付现状

| 事项 | 状态 |
| --- | --- |
| GitHub Release | 仍停在 **v1.0.0**（tag 与资产一字未动），1.0.1/1.0.2/1.0.3 都**未发布** |
| 代码 | main 已推到 `36407b5`（M20-1~M20-5 六笔：`8b2b4c9` `edf3cfa` `210b975` `cedec4b` `23feb36` `36407b5`） |
| 交付物 | `D:\IT-share\MyRemote-v1.0.3\`：四包 + `SHA256SUMS.txt`（LF），**目标目录复校四条全 OK** |
| 版本号 | `CMakeLists.txt` 与 `packaging/common.iss` 都是 1.0.3，`stage.ps1` 每次出包都会断言一致，两个 exe 的 ProductVersion 也断言 |
| 用户侧 | TEST-WIN 装了 1.0.2 并撞出 M20 的全部症状；YEUNG 也装着 1.0.2（等 1.0.3 提权装机） |

## 2. M20 是什么问题、怎么修的

**用户报告**：托盘右键点「停止远程控制」或「退出」后，图标还在但右键永远呼不出菜单，必须杀进程；重开（便携态）又冒出「安装开机自启」，点一下短暂正常、随后再次失灵。

**现场取证**（`\\10.60.254.153\c$\ProgramData\MyRemote\`）：`tray-1.log` 5 次 connected 只有 1 次 exiting（其余世代被杀，而 `NIM_DELETE` 只在退出尾部跑 → 幽灵图标）；`agent.log` 零条 `Remote control paused...`（命令没到宿主）；`service.log` 多次启动前没有 stop（服务被硬终止，管道实例挂死 → 老代理连在僵尸宿主上永不退出）。两条路殊途同归：**托盘线程卡死在无超时 `WriteFile` 上**，或**图标属主被杀没删图标**。

**修复八条（D1~D8）→ 落地（P1~P7）**，细节在 TASKS.md 的 M20 块，此处只留判据：

- 菜单动作一律进工作线程；写管道两端 500ms 封顶、宿主锁内快照锁外写（钉不死监管线程）。
- 退休靠 `bye`/`die` 自退；**只有等不动才允许 `TerminateProcess` 并落一条 `left a ghost icon` 的 error——正常情况下这条日志永远不该出现，它就是回归判据**。
- 心跳双向：宿主 5s `beat` + `ping`，代理托盘线程回 `pong tray=<liveness>`；监管 1s 一轮，8s 无 pong 或 pong 计数不涨 → 拔管道重拉。代理 15s 收不到任何话自己退（连僵尸宿主的场景一起消灭）。
- 清扫三态 `NotProxy/IsProxy/Unknown`，**认不出的一律放过**；宿主的孩子按父 pid 豁免。
- 托盘后备窗改**隐藏顶层工具窗**（1.0.3 起 `EnumWindows` 可枚举、广播可达、`FindWindowW(类+标题)` 可命中）。
- P7：便携态装了服务就不给「安装开机自启」「以管理员身份重启」；服务装了但停着给「重新启用远程控制服务」。

## 3. 验证台账

### 已验（本机、当场有输出/截图）

1. 1.0.3 代理图标 ~3s 无人工干预出现、右键按 pid 硬归属应答、连点不失灵（`build/scratch/tl/tl-03.png`、`m20_icon_hunt.ps1` 运行输出）。
2. 15s 静默规则：代理自退且**自己删图标，无幽灵**（`tl-17.png`；对真实 1.0.2 宿主也成立）。
3. 唤醒路径两通道（`build/scratch/m20_wake_v2.ps1`，代理+便携）：第三进程 `FindWindowW`/`EnumWindows` 命中顶层托盘窗 → `WM_SHOW_CONFIG` → 配置窗在正确 pid 弹出；第二个 `--force` 实例走真实叫醒代码 exit 0 且只有目标弹窗；清理全程走属主自己的 `NIM_DELETE`。
4. 出包链：`stage.ps1` 版本断言、图标断言、windeployqt 产物齐全，四包落盘且哈希复校通过。

### 待验——必须一次提权装机（清单也是 TASKS.md M20 最后一条）

① 故障注入：停服务后跑"只连不读"的假宿主（`build/m19_proxy_hold.ps1` 改造：accept 后不 ReadFile），起 `agent.exe --tray-proxy`，点「停止远程控制」→ **菜单必须还能再呼出**（改前必挂的正是这条）。注意 `kPipeName` 机器全局，假宿主只有在真服务停了才拿得到管道。
② `taskkill /f /im explorer.exe` 让它自重启 → 图标回来**且右键仍弹菜单**（验 D5 修 + 广播路径）。
③ 安装态点「退出」→ `tasklist` 无残留、`agent.log`/`tray-*.log` 搜 `left a ghost icon` 必须为空。
④ 让代理托盘线程僵住 → ≤10s 内 `--service-state` 的 `proxies=` 换号自愈。
⑤ 服务装了但停 → 便携态菜单只给该给的，「重新启用远程控制服务」真能回来（起服务需要提权，正好在同一窗口验）。
⑥ 语义不回归：暂停只关隧道（`sc query` 仍 RUNNING）、退出后启动类型仍自动、隐藏落盘 `tray_icon:false`。
⑦ 顺带补 M18-2 欠的真机账：**1.0.2→1.0.3 覆盖安装**（`PrepareToInstall` 停服务等解锁的正向支）。

用户侧复测脚本 = 他撞出 bug 的原序列：暂停→再右键→恢复→退出→双击→再右键。

### 本机新复现的产品级病理（1.0.2）

几轮代理连入/退出后 1.0.2 宿主管道卡死：新连接报 `no host pipe; giving up and exiting`、`proxies=` 停在旧值，只有重启服务能解。这就是 ①②④ 要在 1.0.3 上终结的东西，现在有 YEUNG 的本地复现记录（`C:\MyRemote\Agent\agent.log` 与 `tray-2.log`，16:26~17:18 时段）。

## 4. 可能还有什么问题（按可信度排序）

1. **`pause`/`resume` 文案的乐观翻转**：菜单文案在点击那一刻按本地缓存决定，若宿主恰好同毫秒改了状态会差一拍（概率极低，症状轻，提权复测时留意）。
2. **混合版本共存噪音**：1.0.3 宿主发 `beat`/`ping`，老代理不认只会 unknown-command 告警；反向老宿主不心跳，1.0.3 代理 15s 自退、由新宿主补回。同一台机器同一 exe 不会真混跑，但**覆盖安装的瞬间窗口期**没测过。
3. **等不动才 `TerminateProcess` 的那一支**最坏让退出慢 ~5s，只影响宿主进程收尾路径，无真机证据说它被走到过。
4. **托盘暴露面不变**（定案如此，不是缺陷）：同会话同用户的任何进程都能连管道发命令、也能画一枚假图标——托盘是操作入口，**不是**"这台被谁控制"的权威凭据；权威读数是 `--service-state` 的 `paused=`/`registered=`/`proxies=`。
5. 1.0.3 的**控制端半边**只随包重编，无行为改动，风险≈0。
6. 老账（后续可选优化清单里）：隧道随宿主重启闪断 ~2s、弱机帧率、多显示器、`config.json` 该进 ProgramData 等，见 TASKS.md 末节。

## 5. 下一步工作（按顺序）

1. **拿到一个有人点 UAC 的窗口**（就在 YEUNG）：装 1.0.3 setup → 跑第 3 节 ①~⑦。
2. 收尾清理：删测试期间为 dev 路径写的两个 `HKCU\Control Panel\NotifyIconSettings\<key>\IsPromoted=1`（本机，`build/m19_promote_icon.ps1` 当年写进去的）。
3. **TEST-WIN 复推 1.0.3——必须先经用户点头**。配方：管理共享拷 setup → 一次性服务静默装（先 `--stop-service` + 杀残留僵尸，安装器的 `PrepareToInstall` 也会停服务等解锁）→ 判据读该机 `service.log`/`--service-state`。之后按用户原序列远程复测。
4. 全部通过后再谈抬 tag / 发 Release（当前决定：**不发版**）。
5. 未发版的既有决定：GitHub 上 v1.0.0 之外的三个版本号都只在交付目录里。

## 6. 环境与工具备忘（会杀时间的坑）

- **bash 里没有 cmake**：用绝对路径 `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`；`build.bat` 依赖 PATH 里的 cmake，直接跑会"不是内部或外部命令"，替代顺序＝绝对路径 configure+build+windeployqt，再 `packaging/stage.ps1 -SkipBuild`。
- **测试脚本一律纯 ASCII**（PS 5.1 按 ANSI 解码无 BOM UTF-8，中文字面量匹配必挂）；窗口识别走类名（`MyRemoteAgentTray`/`MyRemoteConfigWnd`/`#32768`），中文按钮文案在 C# 侧用 `(char)0x4FDD` 拼；`EnumWindows` 回调整体放 C# 侧；PS 单结果管道要 `@()` 包（否则字符串被索引成"首字符"——本轮真实踩过）；`$w` 会撞 `$W`（大小写不敏感——也踩过）。
- **GUI 子系统 exe 在 PS 管道里不写 stdout**：`--version` 要走真实 console。
- 截图脚本必须先 `SetProcessDPIAware()`（本机 200%/3400 宽物理）。
- 关键脚本：`build/m19_proxy_hold.ps1`（假宿主）、`build/m20_icon_hunt.ps1`（扫图标+右键+点菜单行，已修 `$rw`）、`build/scratch/m20_wake_v2.ps1`（唤醒路径两通道，本次新写、判据最全）、取证目录 `C:\ProgramData\MyRemote\tray-<会话>.log` 与 `C:\MyRemote\Agent\*.log`。
- 远端只读取证走 `\\10.60.254.153\c$\...`，但 bash 命令层拒绝内联 UNC——写进 `.ps1` 再执行。

## 7. 机器快照（本会话结束时）

- YEUNG：1.0.2 服务 RUNNING（17:2x 重启过一次，宿主在控制台会话 4，session 2/4 各有代理），管道已恢复正常；无 dev 代理残留、`m20_offline.json` 已删、无幽灵。
- 构建产物：`build/bin/Release` = 1.0.3 全量（含 windeployqt），`build/package/dist` 里 1.0.0~1.0.3 四代包共存（dist 是累加的，交付目录只有 1.0.3 五个文件）。
