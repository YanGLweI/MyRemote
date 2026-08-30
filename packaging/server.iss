; MyRemote 控制端（服务端）安装包。
;
; 编译：ISCC.exe packaging\server.iss
; 但请走 packaging\stage.ps1 —— 它负责 build + windeployqt + 断言版本号一致。
;
; 这个安装器刻意**不管配置**：端口、密钥、日志文件都在管理台的「设置」里改
; （server_config.json 就在 exe 旁边，设置页会重写它）。安装器只做四件事：
; 摆文件、放行防火墙、给快捷方式、卸载时清干净。
#define MyAppName "MyRemote 远程控制管理台"
#define MyAppExeName "control_server.exe"
; 防火墙规则按**程序**放行而不是按端口：管理台以后换监听端口不用再回来加规则。
#define MyFwRule "MyRemote 远程控制管理台"
#include "common.iss"

[Setup]
; AppId 一旦发布就不能改：升级时靠它认出已装的旧版本，改了就会装出第二份。
AppId={{B4E7A1C9-2D63-4F8A-9C05-7A1E6D8F3B22}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoProductName=MyRemote
; 配置文件和日志都写在 exe 旁边（server/src/app_paths.cpp:20），装进 {autopf}
; 会让非提权运行的设置页保存失败，所以默认目录固定在一个可写位置。
DefaultDirName={sd}\MyRemote\Server
; 开始菜单固定用 MyRemote 这一个组：两端都装时是同一个文件夹，而不是两个以全名命名的长文件夹。
DefaultGroupName=MyRemote
DisableProgramGroupPage=yes
OutputDir={#MyOutputDir}
OutputBaseFilename=MyRemote-Server-v{#MyAppVersion}-setup
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
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "firewall"; Description: "在 Windows 防火墙里放行本程序入站连接（被控端要连上来）"; GroupDescription: "网络:"

[Files]
Source: "..\build\bin\Release\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\bin\Release\*.dll"; DestDir: "{app}"; Flags: ignoreversion
; Qt 插件目录逐个写死。曾经写成 "{platforms,styles,...}\*" 的花括号展开：编译不报错，
; 但一条都没匹配上，装出来的控制端缺 platforms\qwindows.dll 根本起不来。
; 也不再写 skipifsourcedoesntexist——缺目录就该编译失败，而不是悄悄少装。
Source: "..\build\bin\Release\platforms\*"; DestDir: "{app}\platforms"; Flags: ignoreversion
Source: "..\build\bin\Release\styles\*"; DestDir: "{app}\styles"; Flags: ignoreversion
Source: "..\build\bin\Release\imageformats\*"; DestDir: "{app}\imageformats"; Flags: ignoreversion
Source: "..\build\bin\Release\tls\*"; DestDir: "{app}\tls"; Flags: ignoreversion
Source: "..\build\bin\Release\networkinformation\*"; DestDir: "{app}\networkinformation"; Flags: ignoreversion
Source: "..\build\bin\Release\generic\*"; DestDir: "{app}\generic"; Flags: ignoreversion
; 只在没有的时候放模板：操作者改过端口/密钥之后，重装不该把它冲掉。
Source: "..\deploy\server_config.json"; DestDir: "{app}"; Flags: onlyifdoesntexist
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{commondesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent

[Code]
// 卸载一开始就删规则：InitializeUninstall 在任何文件被删之前执行，
// 而 [UninstallRun] 的时机不保证这一点，规则名一旦丢了就再也删不掉。
procedure RemoveFirewallRule;
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\netsh.exe'),
    'advfirewall firewall delete rule name="' + '{#MyFwRule}' + '"',
    '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('firewall') then begin
    // 先删后加：同名规则重复添加会叠出好几条，重装一次就多一条。
    RemoveFirewallRule;
    Exec(ExpandConstant('{sys}\netsh.exe'),
      'advfirewall firewall add rule name="' + '{#MyFwRule}' + '" dir=in action=allow protocol=TCP' +
      ' program="' + ExpandConstant('{app}\{#MyAppExeName}') + '" enable=yes profile=any',
      '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;
end;

function InitializeUninstall: Boolean;
begin
  RemoveFirewallRule;
  Result := True;  // False 会让卸载直接中止
end;
