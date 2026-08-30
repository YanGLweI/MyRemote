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
UninstallDisplayName={#MyAppName} {#MyAppVersion}
WizardStyle=modern

[Languages]
Name: "zh"; MessagesFile: "lang\ChineseSimplified.isl"

[Tasks]
; 服务是推荐形态：LocalSystem + 自启，登录界面也能连（M14/M15 的全部意义）。
; 不勾就是便携用法：登录后由人（或计划任务）把它跑起来。
Name: "service"; Description: "安装并启动系统服务 {#MyServiceName}（开机自启，登录界面也能连）"; GroupDescription: "运行方式:"

[Files]
Source: "..\build\bin\Release\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
; 刻意不放 deploy\config.json：里面写着一个示例 IP，带上它 agent 会去连那个地址
; 而不是弹配置界面——"没有配置"才是要弹界面的信号（client/src/main.cpp 的默认路径）。

[Icons]
Name: "{group}\{#MyAppName} 配置界面"; Filename: "{app}\{#MyAppExeName}"; Parameters: "--config-ui"; WorkingDir: "{app}"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"

[Run]
; 顺序有意义：先让旧服务退出（SCM 会在 2 秒内把宿主重新拉起，不停就是占着旧 exe），
; 再收掉残留的便携实例（不同路径的旧 agent 不会被新服务自动接管），最后装新的。
; 三条都忽略退出码：服务本来不存在、或没有残留进程，都是正常情况。
Filename: "{app}\{#MyAppExeName}"; Parameters: "--stop-service"; WorkingDir: "{app}"; StatusMsg: "停止已有的服务…"; Flags: runhidden waituntilterminated; Tasks: service
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM {#MyAppExeName}"; StatusMsg: "结束残留的便携实例…"; Flags: runhidden waituntilterminated; Tasks: service
Filename: "{app}\{#MyAppExeName}"; Parameters: "--install-service"; WorkingDir: "{app}"; StatusMsg: "安装并启动服务…"; Flags: runhidden waituntilterminated; Tasks: service
Filename: "{app}\{#MyAppExeName}"; Parameters: "--config-ui"; WorkingDir: "{app}"; Description: "打开配置界面（控制端地址与接入密钥）"; Flags: nowait postinstall skipifsilent; Check: ConfigNotWritten

[Code]
var
  ConnectPage: TInputQueryWizardPage;
  ConfigWritten: Boolean;

function ParamOr(const Name: String; PageIndex: Integer): String;
begin
  // 命令行优先：静默安装时页面根本不显示，交互安装时页面上填的才算数。
  Result := Trim(ExpandConstant('{param:' + Name + '}'));
  if Result = '' then
    Result := Trim(ConnectPage.Values[PageIndex]);
end;

function JsonEsc(const S: String): String;
var
  I: Integer;
begin
  Result := '';
  for I := 1 to Length(S) do
  begin
    if (S[I] = '\') or (S[I] = '"') then
      Result := Result + '\';
    Result := Result + S[I];
  end;
end;

function ProgramDataConfig: String;
begin
  Result := ExpandConstant('{commonappdata}\MyRemote\config.json');
end;

function AppConfig: String;
begin
  Result := ExpandConstant('{app}\config.json');
end;

function ConfigNotWritten: Boolean;
begin
  Result := not ConfigWritten;
end;

procedure InitializeWizard;
begin
  ConnectPage := CreateInputQueryPage(wpSelectTasks,
    '连到哪台控制端', '全部留空也可以：装完第一次运行会弹出配置界面。',
    '填写任意一项就会写入配置文件，留空的项用默认值。' + #13#10 +
    '接入密钥必须与控制端「设置」里那一条完全一致，否则会被拒绝接入。');
  ConnectPage.Add('控制端地址（IP 或主机名）：', False);
  ConnectPage.Add('端口（默认 7500）：', False);
  ConnectPage.Add('接入密钥：', False);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  IP, Port, Key, Text: String;
begin
  if CurStep <> ssPostInstall then
    Exit;
  ConfigWritten := False;
  IP := ParamOr('SERVERIP', 0);
  Port := ParamOr('SERVERPORT', 1);
  Key := ParamOr('SECRETKEY', 2);
  if (IP = '') and (Port = '') and (Key = '') then
    Exit;  // 什么都没填：不去动配置，让第一次运行弹界面

  if FileExists(ProgramDataConfig) then begin
    // agent 找配置的顺序是 ProgramData 优先（client/src/desktop.cpp 的 resolve_paths），
    // 写 {app}\config.json 会被无视，所以这里只提醒、不假装成功。
    // 静默安装（尤其是被 sc.exe 一次性服务拉起来跑的那种）绝不能弹窗：
    // session 0 里没有前台，弹窗会把安装器永久挂住。
    Log('A config already exists in %ProgramData%\MyRemote; it was left untouched.');
    if not WizardSilent then
      MsgBox('检测到已有 %ProgramData%\MyRemote\config.json，agent 会优先读它，' +
        '安装器没有覆盖这个文件。' + #13#10 + #13#10 +
        '要改连接信息，请编辑那个文件，或先用配置界面保存。', mbInformation, MB_OK);
    Exit;
  end;

  if Port = '' then
    Port := '7500';
  if Key = '' then
    Key := 'default_secret_key_12345';
  // 字段顺序与 deploy\config.json 一致；common/src/config.cpp 是逐键扫描，顺序不影响读取。
  // 写成不带 BOM：SaveStringsToUTF8File 总会加一个 BOM，而这个文件应该和
  // deploy\config.json、配置界面写出来的那份逐字节可比。这里的值全是 ASCII
  // （device_name 固定留空），所以过一遍 AnsiString 不会丢字——以后要加中文字段，
  // 必须先换回带 BOM 的 UTF-8 写法并确认解析器读得动。
  Text := '{' + #13#10 +
    '  "server_ip": "' + JsonEsc(IP) + '",' + #13#10 +
    '  "server_port": ' + Port + ',' + #13#10 +
    '  "secret_key": "' + JsonEsc(Key) + '",' + #13#10 +
    '  "device_name": "",' + #13#10 +
    '  "control_password": ""' + #13#10 + '}' + #13#10;
  SaveStringToFile(AppConfig, Text, False);
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
end;
