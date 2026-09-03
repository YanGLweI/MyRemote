# M24-4 prerequisite: put the 1.0.7 dev build where the service points, so the
# bye gate under test is the one that ships in this milestone.
#
# The installed image is 1.0.6 and its stop path predates the gate, so measuring
# the gate against it would measure nothing. The swap is reversible by
# construction: the original exe is renamed in place (never deleted), its sha256
# is recorded before anything moves, and -Restore puts that exact file back and
# compares the bytes.
#
#   elevated:  powershell -File tools/harness/m24_swap.ps1 -Mode swap
#   elevated:  powershell -File tools/harness/m24_swap.ps1 -Mode restore
param(
    [ValidateSet('swap', 'restore', 'status')]
    [string] $Mode = 'status',
    [string] $Repo = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)),
    [string] $Source = '',
    [string] $InstallDir = 'C:\MyRemote\Agent',
    [string] $Service = 'MyRemoteAgent'
)

$ErrorActionPreference = 'Stop'
$work = Join-Path $Repo 'build\scratch\m24_swap'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$journal = Join-Path $work 'journal.txt'
if (-not $Source) { $Source = Join-Path $Repo 'build\bin\Release\agent.exe' }

$installed = Join-Path $InstallDir 'agent.exe'
$aside = Join-Path $InstallDir 'agent-installed-1.0.6.exe'

function Say([string] $s) {
    $s
    Add-Content -LiteralPath $journal -Value (("{0:yyyy-MM-dd HH:mm:ss}  " -f (Get-Date)) + $s)
}

function Sha([string] $p) {
    if (-not (Test-Path -LiteralPath $p)) { return 'absent' }
    (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash
}

function State([string] $name) {
    $q = & sc.exe query $name 2>&1 | Out-String
    if ($q -match 'STATE\s+:\s+\d+\s+(\w+)') { return $Matches[1] }
    return "unknown($($q -join ' '))"
}

function Wait-State([string] $name, [string] $want, [int] $timeoutMs) {
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        if ((State $name) -eq $want) { return $true }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

function Stop-AgentService([string] $name) {
    if ((State $name) -ne 'STOPPED') {
        [void](& sc.exe stop $name 2>&1 | Out-String)
    }
    if (-not (Wait-State $name 'STOPPED' 15000)) {
        throw "FAIL: service $name did not reach STOPPED (state=$(State $name))"
    }
}

$failures = @()
try {
    if ($Mode -eq 'status') {
        Say "status installed=$(Sha $installed) aside=$(Sha $aside)"
        Say "status source=$(Sha $Source)"
        Say "status service state=$(State $Service)"
        return
    }

    if ($Mode -eq 'swap') {
        if (-not (Test-Path -LiteralPath $Source)) { throw "FAIL: no dev build at $Source" }
        if (-not (Test-Path -LiteralPath $installed)) { throw "FAIL: no installed exe at $installed" }
        if (Test-Path -LiteralPath $aside) {
            throw "FAIL: $aside already exists - a previous swap was never restored"
        }
        $before = Sha $installed
        $srcVer = (Get-Item -LiteralPath $Source).VersionInfo.FileVersion
        $instVer = (Get-Item -LiteralPath $installed).VersionInfo.FileVersion
        Say "swap from installed FileVersion=$instVer sha=$($before.Substring(0,16))"
        Say "swap to   source  FileVersion=$srcVer sha=$((Sha $Source).Substring(0,16))"
        $before | Out-File -LiteralPath (Join-Path $work 'installed.sha256') -Encoding ASCII
        Stop-AgentService $Service
        Move-Item -LiteralPath $installed -Destination $aside
        Copy-Item -LiteralPath $Source -Destination $installed
        Say "swap installed now FileVersion=$((Get-Item -LiteralPath $installed).VersionInfo.FileVersion) sha=$((Sha $installed).Substring(0,16))"
        Say "swap aside  kept   FileVersion=$((Get-Item -LiteralPath $aside).VersionInfo.FileVersion)"
        [void](& sc.exe start $Service 2>&1 | Out-String)
        if (-not (Wait-State $Service 'RUNNING' 20000)) {
            throw "FAIL: service did not come up on the swapped-in build (state=$(State $Service))"
        }
        Say "swap service RUNNING on the dev build"
        return
    }

    # restore
    if (-not (Test-Path -LiteralPath $aside)) { throw "FAIL: nothing to restore - $aside is absent" }
    $want = (Get-Content -LiteralPath (Join-Path $work 'installed.sha256') -Raw).Trim()
    Stop-AgentService $Service
    Remove-Item -LiteralPath $installed -Force
    Move-Item -LiteralPath $aside -Destination $installed
    $now = Sha $installed
    $ok = ($now -eq $want)
    Say "restore installed sha=$($now.Substring(0,16)) expected=$($want.Substring(0,16)) byte-identical=$ok"
    [void](& sc.exe start $Service 2>&1 | Out-String)
    if (-not (Wait-State $Service 'RUNNING' 20000)) {
        throw "FAIL: service not RUNNING after restore (state=$(State $Service))"
    }
    Say "restore service RUNNING FileVersion=$((Get-Item -LiteralPath $installed).VersionInfo.FileVersion)"
    if (-not $ok) { throw "FAIL: the restored exe is not the byte-exact original" }
    return
}
catch {
    $failures += $_.Exception.Message
    throw
}
finally {
    Say "done mode=$Mode failures=$($failures.Count)"
    if ($failures.Count) { foreach ($f in $failures) { Say "  failure: $f" } }
}
