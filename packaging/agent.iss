; MyRemote 被控端（agent）安装包。
;
; 编译：ISCC.exe packaging\agent.iss —— 请走 packaging\stage.ps1。
;
; 静默参数（远程批量推送用，全部选填）：
;   MyRemote-Agent-v1.0.0-setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
;     /SERVERIP=10.0.0.5 /SERVERPORT=7500 /SECRETKEY=只有你知道的串 /TASKS="service"
; 一个参数都不给 = 只装文件和（可选）服务，第一次运行弹配置界面。
#define MyAppName "MyRemote 被控端"
#define MyAppExeName "agent.exe"
#define MyServiceName "MyRemoteAgent"
#include "common.iss"

[Setup]
AppId={{7C1F5A83-9E42-4B7D-A108-3D6F2B47C9E5}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoProductName=MyRemote
DefaultDirName={sd}\MyRemote\Agent
; 与控制端同一个开始菜单组（见 server.iss）。
DefaultGroupName=MyRemote
DisableProgramGroupPage=yes
OutputDir={#MyOutputDir}
OutputBaseFilename=MyRemote-Agent-v{#MyAppVersion}-setup
SetupIconFile={#MySetupIcon}
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
; 不要让 Inno 自己去"关闭应用"：占着 agent.exe 的是 SYSTEM 里那个没有窗口的服务
; 宿主，既没窗口可关，也不该被静默杀掉。真正的停服务在 PrepareToInstall 里。
; 这里也刻意不加 AppMutex——CheckForOtherInstances 会弹一句"请先关闭正在运行的
; 程序"，而机器前的人根本没有程序可关。
CloseApplications=no
RestartIfNeededByRun=no
UninstallDisplayName={#MyAppName} {#MyAppVersion}
; 不写这行，"程序和功能"里就是一块空白磁贴——面板不会去读 exe 自己的图标。
UninstallDisplayIcon={app}\{#MyAppExeName}
WizardStyle=modern

[Languages]
Name: "zh"; MessagesFile: "lang\ChineseSimplified.isl"

[Tasks]
; 服务是推荐形态：LocalSystem + 自启，登录界面也能连（M14/M15 的全部意义）。
; 不勾就是便携用法：登录后由人（或计划任务）把它跑起来。
Name: "service"; Description: "安装并启动系统服务 {#MyServiceName}（开机自启，登录界面也能连）"; GroupDescription: "运行方式:"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[InstallDelete]
; 与 PrepareToInstall 里的"等到真能删为止"互补：Inno 自己那次删除尝试不等待。
Type: files; Name: "{app}\{#MyAppExeName}"

[Files]
Source: "..\build\bin\Release\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
; 刻意不放 deploy\config.json：里面写着一个示例 IP，带上它 agent 会去连那个地址
; 而不是弹配置界面——"没有配置"才是要弹界面的信号（client/src/main.cpp 的默认路径）。

[Icons]
; 桌面这条**不带参数**：前台启动会先弹配置窗、点"保存并运行"才建隧道，所以从托盘
; "退出"之后双击它就能把客户端重新跑起来。带 --config-ui 只会开一个编辑器然后退出，
; 那是开始菜单那条的职责。图标继承 agent.exe 自带的那个（client/resources/agent.rc）。
Name: "{commondesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon
Name: "{group}\{#MyAppName} 配置界面"; Filename: "{app}\{#MyAppExeName}"; Parameters: "--config-ui"; WorkingDir: "{app}"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"

[Run]
; 这里只负责"装好之后要做的事"。**不要**把停服务/taskkill 加回来：[Run] 在 [Files]
; 之后执行，等到它们跑起来时复制早就失败了——v1.0.0 在 TEST-WIN 上覆盖安装报
; "DeleteFile 失败；错误代码 5" 就是这个顺序造成的。要腾出文件请去 PrepareToInstall。
Filename: "{app}\{#MyAppExeName}"; Parameters: "--install-service"; WorkingDir: "{app}"; StatusMsg: "安装并启动服务…"; Flags: runhidden waituntilterminated; Tasks: service
Filename: "{app}\{#MyAppExeName}"; Parameters: "--config-ui"; WorkingDir: "{app}"; Description: "打开配置界面（控制端地址与接入密钥）"; Flags: nowait postinstall skipifsilent; Check: ConfigNotWritten

[Code]
var
  ConnectPage: TInputQueryWizardPage;
  ConfigWritten: Boolean;

// 等到 agent.exe 真的删得动为止。映像只要还被映射就删不掉，所以 DeleteFile 返回
// TRUE 是"锁已解除"的权威证明；FileExists 和"以读方式打开"两种探测都会说谎。
// 删掉本身无害：[Files] 紧接着就会把它重写一遍。
function WaitForAgentExeFree(const Exe: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to 50 do
  begin
    if not FileExists(Exe) then
    begin
      // 已经被别处删掉了，同样说明没有锁
      Result := True;
      Exit;
    end;
    if DeleteFile(Exe) then
    begin
      Result := True;
      Exit;
    end;
    Sleep(200);
  end;
end;

// 覆盖安装（机器上还跑着旧版）真正的关键一步：这一切必须发生在 [Files] 之前。
function FreeAgentExe(const Exe: String): Boolean;
var
  ResultCode: Integer;
begin
  if not FileExists(Exe) then
  begin
    Result := True;   // 全新安装，没什么要腾的
    Exit;
  end;
  // 安装器本身已经提权（PrivilegesRequired=admin），所以 --stop-service 会真的等
  // SCM 停完，不会走"转手提权、立刻返回 0"那条分支；服务本来不存在时它返回非 0，
  // 那也是正常情况，所以忽略退出码。
  Exec(Exe, '--stop-service', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  // 便携形态的残留实例不归服务管，不同路径的旧 agent 也不会被新服务接管。
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#MyAppExeName}', '',
       SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Result := WaitForAgentExeFree(Exe);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Exe: String;
  ResultCode: Integer;
begin
  Result := '';
  Exe := ExpandConstant('{app}\{#MyAppExeName}');
  if FreeAgentExe(Exe) then
    Exit;
  // 走到这里说明刚才已经把服务停了。装不下去就要尽力把它开回来——
  // 一次失败的升级不该把这台机器留在"没人能连"的状态里直到下次重启。
  ResultCode := 0;
  Exec(Exe, '--start-service', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  // 刻意不置 NeedsRestart：那会走"需要重启"分支，为了一个重试就能解决的问题
  // 重启一台可能是服务器的机器。这里也一个 MsgBox 都不弹——静默安装（尤其是被
  // 一次性服务拉起来跑的那种）弹窗就是永久挂住，只返回字符串让退出码非 0。
  Result := '无法替换 agent.exe：它仍在被占用。' + #13#10 +
    '请在该机器上以管理员身份运行 agent.exe --stop-service，或注销正在使用它的会话，然后重试。';
end;

function ParamOr(const Name: String; PageIndex: Integer): String;
begin
  // 命令行优先：静默安装时页面根本不显示，交互安装时页面上填的才算数。
  Result := Trim(ExpandConstant('{param:' + Name + '}'));
  if Result = '' then
    Result := Trim(ConnectPage.Values[PageIndex]);
end;

function ConfigNotWritten: Boolean;
begin
  Result := not ConfigWritten;
end;

procedure InitializeWizard;
begin
  ConnectPage := CreateInputQueryPage(wpSelectTasks,
    '连到哪台控制端', '全部留空也可以：装完第一次运行会弹出配置界面。',
    '填写任意一项就会写入配置文件；留空的项保持机器上原有的设置。' + #13#10 +
    '接入密钥必须与控制端「设置」里那一条完全一致，否则会被拒绝接入。');
  ConnectPage.Add('控制端地址（IP 或主机名）：', False);
  ConnectPage.Add('端口（默认 7500）：', False);
  ConnectPage.Add('接入密钥：', False);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  IP, Port, Key, Params, Exe: String;
  ResultCode: Integer;
begin
  if CurStep <> ssPostInstall then
    Exit;
  ConfigWritten := False;
  IP := ParamOr('SERVERIP', 0);
  Port := ParamOr('SERVERPORT', 1);
  Key := ParamOr('SECRETKEY', 2);
  if (IP = '') and (Port = '') and (Key = '') then
    Exit;  // 什么都没填：不去动配置，让第一次运行弹界面

  // 落盘交给 agent 自己。安装器不猜路径：resolve_paths 认哪一份（ProgramData 优先），
  // --set-server 就写哪一份；它还会先把那份读回来，只覆盖页面上显式给出的项，
  // 于是设备名、控制密码、编码宽度、托盘开关在覆盖安装时原样保住——
  // config.cpp 的 save() 是全量写，从默认值起步会把这些悄悄抹掉。
  // 静默安装（尤其是被 sc.exe 一次性服务拉起来跑的那种）绝不能弹窗：
  // session 0 里没有前台，弹窗会把安装器永久挂住。
  Exe := ExpandConstant('{app}\{#MyAppExeName}');
  Params := '--set-server';
  if IP <> '' then
    Params := Params + ' --ip "' + IP + '"';
  if Port <> '' then
    Params := Params + ' --port ' + Port;
  if Key <> '' then
    Params := Params + ' --key "' + Key + '"';
  if not Exec(Exe, Params, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then begin
    Log('agent.exe --set-server could not run (code ' + IntToStr(ResultCode) + ').');
    Exit;
  end;
  if ResultCode <> 0 then begin
    Log('agent.exe --set-server exited with ' + IntToStr(ResultCode) +
        '; the config was left as it was.');
    if not WizardSilent then
      MsgBox('安装完成，但连接信息没有写入配置（agent.exe --set-server 退出码 ' +
        IntToStr(ResultCode) + '）。' + #13#10 + #13#10 +
        '请从开始菜单打开“配置界面”手工填写。', mbInformation, MB_OK);
    Exit;
  end;
  ConfigWritten := True;
end;

function InitializeUninstall: Boolean;
var
  ResultCode: Integer;
  Exe: String;
begin
  Result := True;  // 无论如何都继续卸载，只是能清的先清掉
  // 必须在任何文件被删之前把服务摘掉：服务 binPath 指的就是 {app}\agent.exe，
  // 先删文件会留下一个指向空路径的僵尸服务（而且 SCM 会尝试重启它）。
  Exe := ExpandConstant('{app}\{#MyAppExeName}');
  if not FileExists(Exe) then
    Exit;
  Exec(Exe, '--uninstall-autostart', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(Exe, '--uninstall-service', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  // --uninstall-service 自己会先停再删；这里只收便携模式下残留的那一份。
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#MyAppExeName}', '',
    SW_HIDE, ewWaitUntilTerminated, ResultCode);
  // 卸载同一样的竞态：不等到锁消失，agent.exe 会被登记成"重启后删除"，
  // 目录里留着一个还在跑的二进制，下一次安装又要跟它抢。
  WaitForAgentExeFree(Exe);
end;
