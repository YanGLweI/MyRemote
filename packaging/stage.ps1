# 打四个包：控制端与被控端各一个安装版 setup.exe + 一个便携 zip，外加校验值。
#
#   powershell -File packaging\stage.ps1              # 先 build.bat 再打包
#   powershell -File packaging\stage.ps1 -SkipBuild   # 用现成的 build\bin\Release
#
# 产物全部落在 build\package\ 下（build/ 已被 git 忽略，仓库里只留脚本）。
param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$bin  = Join-Path $root 'build\bin\Release'
$pkg  = Join-Path $root 'build\package'
$dist = Join-Path $pkg 'dist'
New-Item -ItemType Directory -Force -Path $pkg, $dist | Out-Null

# ---- 版本号：CMakeLists 与 common.iss 必须是同一个串 ----
# 不一致时最坏的结果是"文件名 v1.0.1、属性页 1.0.0"，那种包发出去没人能判断装了什么。
$cmVer  = [regex]::Match((Get-Content (Join-Path $root 'CMakeLists.txt') -Raw),
                'project\s*\(\s*MyRemoteControl\s+VERSION\s+([0-9.]+)').Groups[1].Value
$issVer = [regex]::Match((Get-Content (Join-Path $PSScriptRoot 'common.iss') -Raw),
                '#define\s+MyAppVersion\s+"([^"]+)"').Groups[1].Value
if (-not $cmVer -or ($cmVer -ne $issVer)) {
    throw "版本不一致：CMakeLists.txt=$cmVer common.iss=$issVer"
}
$ver = $cmVer

# ---- Inno Setup 编译器：winget 是按用户装的，所以先找 per-user 路径 ----
$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
          "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
          "$env:ProgramFiles\Inno Setup 6\ISCC.exe") |
       Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) {
    throw "找不到 ISCC.exe。先装：winget install --id JRSoftware.InnoSetup -e"
}

# ---- 构建 ----
if (-not $SkipBuild) {
    "build       : build.bat（日志 build\package\build.log）"
    $blog = Join-Path $pkg 'build.log'
    cmd /c "`"$root\build.bat`" > `"$blog`" 2>&1"
    if ($LASTEXITCODE) { throw "build.bat 失败，见 $blog" }
}

# ---- 产物完整性：宁可现在失败，也别发一个点开就报错的包 ----
foreach ($f in 'control_server.exe', 'agent.exe', 'Qt6Core.dll', 'platforms\qwindows.dll') {
    if (-not (Test-Path (Join-Path $bin $f))) {
        throw "缺少 build\bin\Release\$f —— 去掉 -SkipBuild 重跑（windeployqt 那一步）"
    }
}
# server.iss 里每个插件目录都是一条 [Files]，缺任何一个都会让控制端起不来
# （没有 platforms\qwindows.dll 就是一句"找不到 Qt platform plugin"）。
foreach ($d in 'platforms', 'styles', 'imageformats', 'tls') {
    if (-not (Test-Path (Join-Path $bin $d))) {
        throw "缺少 build\bin\Release\$d\ —— windeployqt 没跑全，不能出包"
    }
}
foreach ($n in 'agent', 'control_server') {
    $pv = (Get-Item (Join-Path $bin "$n.exe")).VersionInfo.ProductVersion
    if ($pv -ne $ver) { throw "$n.exe ProductVersion='$pv'，应为 $ver（版本资源没进去？）" }
}

# ---- 摆便携版目录 ----
$serverDir = Join-Path $pkg 'server-portable'
$agentDir  = Join-Path $pkg 'agent-portable'
Remove-Item $serverDir, $agentDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $serverDir, $agentDir | Out-Null

Copy-Item (Join-Path $bin 'control_server.exe') $serverDir
Copy-Item (Join-Path $bin '*.dll') $serverDir
# 插件目录整层名字必须保住：Qt 找的是 <exe 目录>\platforms\qwindows.dll。
foreach ($d in 'platforms', 'styles', 'imageformats', 'tls', 'networkinformation', 'generic') {
    $src = Join-Path $bin $d
    if (Test-Path $src) { Copy-Item $src (Join-Path $serverDir $d) -Recurse -Force }
}
Copy-Item (Join-Path $root 'deploy\server_config.json') $serverDir
Copy-Item (Join-Path $root 'README.md') $serverDir
Copy-Item (Join-Path $PSScriptRoot 'portable\unblock.cmd') $serverDir

Copy-Item (Join-Path $bin 'agent.exe') $agentDir
# 故意叫 config.example.json：真叫 config.json 的话 agent 会去连里面那个示例 IP，
# 而不是弹配置界面——"没有配置"才是弹界面的信号。
Copy-Item (Join-Path $root 'deploy\config.json') (Join-Path $agentDir 'config.example.json')
Copy-Item (Join-Path $root 'README.md') $agentDir
foreach ($h in 'unblock.cmd', 'install-service.bat', 'uninstall-service.bat') {
    Copy-Item (Join-Path $PSScriptRoot "portable\$h") $agentDir
}

# ---- 压缩 ----
# 用系统自带的 bsdtar（Win10 1803+ 就有），不用 Compress-Archive：后者和 .NET 的
# ZipFile 都把目录分隔符写成反斜杠，违反 ZIP 规范——资源管理器能解开，7-Zip 或
# macOS 的 unzip 会解出一个名字叫 "platforms\qwindows.dll" 的文件，控制端就起不来。
# 逐项传顶层名字，条目里就不会多出 "./" 前缀。
# 刚复制出来的 exe 常被杀软扫一遍而短暂占用，第一次会报"正由另一进程使用"，重试即可。
function Zip-Dir {
    param([string]$SourceDir, [string]$Destination)
    $tar = Join-Path $env:SystemRoot 'System32\tar.exe'
    $items = Get-ChildItem -LiteralPath $SourceDir | ForEach-Object { $_.Name }
    if (Test-Path $Destination) { Remove-Item $Destination -Force }
    for ($try = 1; $try -le 6; $try++) {
        if (Test-Path $tar) {
            & $tar -a -cf $Destination -C $SourceDir -- $items 2>$null
            if ((-not $LASTEXITCODE) -and (Test-Path $Destination)) { return }
        } else {
            try {
                Compress-Archive -Path (Join-Path $SourceDir '*') -DestinationPath $Destination -Force -ErrorAction Stop
                Write-Warning "没有 System32\tar.exe，退回 Compress-Archive：条目分隔符是反斜杠"
                return
            } catch { }
        }
        Start-Sleep -Milliseconds 800
    }
    throw "打包 $Destination 失败（重试 6 次）"
}

$zipServer = Join-Path $dist "MyRemote-Server-v$ver-portable.zip"
$zipAgent  = Join-Path $dist "MyRemote-Agent-v$ver-portable.zip"
Zip-Dir $serverDir $zipServer
Zip-Dir $agentDir  $zipAgent

# ---- 编译安装器 ----
foreach ($iss in 'server.iss', 'agent.iss') {
    "iscc        : $iss"
    & $iscc /Qp (Join-Path $PSScriptRoot $iss) | Out-Null
    if ($LASTEXITCODE) { throw "ISCC 编译 $iss 失败（退出码 $LASTEXITCODE）" }
}

# ---- 校验值 ----
$setupServer = Join-Path $dist "MyRemote-Server-v$ver-setup.exe"
$setupAgent  = Join-Path $dist "MyRemote-Agent-v$ver-setup.exe"
foreach ($f in $setupServer, $setupAgent) {
    if (-not (Test-Path $f)) { throw "没产出 $f" }
}
$sums = Join-Path $dist 'SHA256SUMS.txt'
$lines = foreach ($f in @($setupServer, $setupAgent, $zipServer, $zipAgent)) {
    '{0}  {1}' -f (Get-FileHash -LiteralPath $f -Algorithm SHA256).Hash.ToLower(), (Split-Path -Leaf $f)
}
Set-Content -LiteralPath $sums -Value $lines -Encoding ascii

''
"version      : $ver"
"artifacts    :"
foreach ($f in @($setupServer, $zipServer, $setupAgent, $zipAgent, $sums)) {
    $i = Get-Item -LiteralPath $f
    '  {0,9:N0} KB  {1}' -f ($i.Length / 1KB), $i.Name
}
