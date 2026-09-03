# The admin windows M24 still needs.
#
# Everything in here needs a token, and the person at the machine should only be
# asked to grant it once per phase. Each step is a separate script so a failure
# names itself instead of taking the rest of the list down; each of those scripts
# puts the machine back in its own finally block.
#
#   powershell -File tools/harness/m24_admin_window.ps1 -Phase measure
#   ... the F5 human legs run in between, with the service deliberately stopped
#   powershell -File tools/harness/m24_admin_window.ps1 -Phase restore
#
# Two phases rather than one because the legs disagree about the service: F4 has
# to quit a running 1.0.7, and F5's menu item only appears while it is stopped.
param(
    [ValidateSet('measure', 'restore')] [string] $Phase = 'measure',
    [string] $Repo = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)),
    [string] $Service = 'MyRemoteAgent',
    [string] $InstalledExe = 'C:\MyRemote\Agent\agent.exe'
)

$ErrorActionPreference = 'Continue'
$script:exits = @{}

# An elevated Start-Process cannot redirect its stdout, so the window records
# itself or the run leaves no trace of the steps between the child scripts.
$logDir = Join-Path $Repo 'build\scratch\m24_admin'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
Start-Transcript -Path (Join-Path $logDir "window-$Phase.txt") -Force | Out-Null

function Run-Step([string] $name, [string] $file, [string[]] $argv) {
    "=== $name ==="
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot $file) @argv
    $script:exits[$name] = $LASTEXITCODE
    "exit=$LASTEXITCODE for $name"
}

function State([string] $name) {
    $q = & sc.exe query $name 2>&1 | Out-String
    if ($q -match 'STATE\s+:\s+\d+\s+(\w+)') { return $Matches[1] }
    return 'unknown'
}

function Wait-State([string] $name, [string] $want, [int] $timeoutMs) {
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        if ((State $name) -eq $want) { return $true }
        Start-Sleep -Milliseconds 150
    }
    return $false
}

"phase=$Phase"

if ($Phase -eq 'restore') {
    # The installed image goes back, byte-for-byte, and the service goes up on it.
    Run-Step 'restore' 'm24_swap.ps1' @('-Repo', $Repo, '-Mode', 'restore')
    "  service state: $(State $Service)"
    "  start type: $((& sc.exe qc $Service 2>&1 | Out-String) -replace '(?s).*START_TYPE\s+:\s+', '$1' -replace '\r?\n.*', '')"
    "  installed FileVersion: $((Get-Item -LiteralPath $InstalledExe).VersionInfo.FileVersion)"
    Stop-Transcript | Out-Null
    if ($script:exits['restore'] -ne 0) { exit 1 }
    exit 0
}

# measure
# 1. The rebuilt 1.0.7 goes into the path the service points at. Only a stopped
#    service lets that file be replaced.
$src = Join-Path $Repo 'build\bin\Release\agent.exe'
$srcSha = (Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash
$curSha = 'absent'
if (Test-Path -LiteralPath $InstalledExe) {
    $curSha = (Get-FileHash -LiteralPath $InstalledExe -Algorithm SHA256).Hash
}
"installed sha=$($curSha.Substring(0,16)) dev sha=$($srcSha.Substring(0,16)) same=$($curSha -eq $srcSha)"
if ($curSha -ne $srcSha) {
    if ((State $Service) -ne 'STOPPED') {
        [void](& sc.exe stop $Service 2>&1 | Out-String)
        if (-not (Wait-State $Service 'STOPPED' 20000)) {
            "SKIP: service would not stop (state=$(State $Service)) - refusing to overwrite a running exe"
            Stop-Transcript | Out-Null
            exit 2
        }
    }
    Copy-Item -LiteralPath $src -Destination $InstalledExe -Force
    "copied $src -> $InstalledExe"
}
[void](& sc.exe start $Service 2>&1 | Out-Null)
if (-not (Wait-State $Service 'RUNNING' 20000)) {
    "SKIP: service is $(State $Service), not RUNNING - nothing downstream can measure"
    Stop-Transcript | Out-Null
    exit 2
}
"service RUNNING, FileVersion=$((Get-Item -LiteralPath $InstalledExe).VersionInfo.FileVersion)"

# 2. Both F4 legs. Each one quits the agent on purpose and puts the service back.
Run-Step 'f4-healthy' 'm24_f4.ps1' @('-Repo', $Repo, '-Case', 'healthy')
Run-Step 'f4-hung'    'm24_f4.ps1' @('-Repo', $Repo, '-Case', 'hung')

# 3. F1's tail: this step waits for a limited-token CLI run started from the
#    other side (m24_f1_caller.ps1 in the un-elevated session), then restores the
#    descriptor whatever happens.
Run-Step 'f1-acl' 'm24_f1_acl.ps1' @('-Repo', $Repo, '-Service', $Service)

# 4. Leave the service down for the F5 legs, which need the menu item that only
#    exists while the background service is stopped. The restore phase puts it
#    back up; this is a deliberate hand-off, not an accident.
[void](& sc.exe stop $Service 2>&1 | Out-String)
[void](Wait-State $Service 'STOPPED' 20000)

"=== window summary ==="
foreach ($k in ($script:exits.Keys | Sort-Object)) { "  $k exit=$($script:exits[$k])" }
"  service state: $(State $Service) (left stopped on purpose for the F5 legs)"
"  installed FileVersion: $((Get-Item -LiteralPath $InstalledExe).VersionInfo.FileVersion)"
Stop-Transcript | Out-Null
