# The admin windows M24 still needs.
#
# Everything in here needs a token, and the person at the machine should only be
# asked to grant it once per phase. Each step is a separate script so a failure
# names itself instead of taking the rest of the list down; each of those scripts
# puts the machine back in its own finally block.
#
#   powershell -File tools/harness/m24_admin_window.ps1 -Phase measure
#   ... the F5 human legs run in between, with the service deliberately stopped
#   powershell -File tools/harness/m24_admin_window.ps1 -Phase restore -RerunHung
#   powershell -File tools/harness/m24_admin_window.ps1 -Phase final
#
# -Phase final is the last three legs in one token: the hung leg's reading, the F5
# elevated menu photographed, and the install path put back. They share a window
# because each needs the same admin token, and they are ordered by where they
# disagree - the hung leg wants a running 1.0.7, the menu item only exists while
# the service is down, and the restore takes the 1.0.7 away, so it goes last and
# runs whatever the legs concluded.
#
# Two phases rather than one because the legs disagree about the service: F4 has
# to quit a running 1.0.7, and F5's menu item only appears while it is stopped.
# -RerunHung belongs to the restore phase because the hung leg is the last thing
# that can only be measured while the swapped-in 1.0.7 owns the service's path.
param(
    [ValidateSet('measure', 'restore', 'final')] [string] $Phase = 'measure',
    [switch] $RerunHung,
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

# Not a -replace with '$1' in it: that pattern has no group to fill, and the
# reading came out of the window as the literal "$12   AUTO_START".
function Start-Type([string] $name) {
    $q = & sc.exe qc $name 2>&1 | Out-String
    if ($q -match 'START_TYPE\s+:\s+(\d+)\s+(\w+)') { return "$($Matches[1]) $($Matches[2])" }
    return 'unknown'
}

"phase=$Phase"

if ($Phase -eq 'final') {
    # The three legs M24 still owes, in one token. $script:phaseBad is the phase's
    # own verdict and is deliberately separate from the legs' - the machine has to
    # go back either way.
    $script:phaseBad = $false
    $hungResult = Join-Path $Repo 'build\scratch\m24_f4\f4-hung.txt'
    $controlResult = Join-Path $logDir 'f4-hung-control.txt'

    # a. An instrument that cannot go red produces no readings. One second of
    #    pumping cannot fill a 4 KB buffer, so this control run must fail on
    #    "the buffer was full at quit". A green control says the assertions are
    #    still decorative, and nothing the real leg then reports is evidence.
    Run-Step 'f4-hung-control' 'm24_f4.ps1' @('-Repo', $Repo, '-Case', 'hung', '-PumpLimitSeconds', '1')
    if (Test-Path -LiteralPath $hungResult) {
        Move-Item -LiteralPath $hungResult -Destination $controlResult -Force
    }
    $control = @()
    if (Test-Path -LiteralPath $controlResult) {
        $control = @(Get-Content -LiteralPath $controlResult)
    }
    $ok_full_red = @($control | Where-Object {
        $_ -match 'buffer was full at quit.*-> False' }).Count -gt 0
    "  control: exit=$($script:exits['f4-hung-control']) ok_full_went_false=$ok_full_red"
    if ($script:exits['f4-hung-control'] -eq 0 -or -not $ok_full_red) {
        "  CONTROL DID NOT GO RED - the hung leg cannot be judged this run"
        $script:phaseBad = $true
    }

    # b. The hung leg proper, against a running 1.0.7 on the install path.
    if ((State $Service) -ne 'RUNNING') {
        [void](& sc.exe start $Service 2>&1 | Out-String)
        [void](Wait-State $Service 'RUNNING' 20000)
    }
    Run-Step 'f4-hung' 'm24_f4.ps1' @('-Repo', $Repo, '-Case', 'hung')
    if ($script:exits['f4-hung'] -ne 0) { $script:phaseBad = $true }

    # c. The elevated token's menu, photographed. The item only exists while the
    #    service is down, so this leg needs the opposite of the last one.
    [void](& sc.exe stop $Service 2>&1 | Out-String)
    [void](Wait-State $Service 'STOPPED' 20000)
    $shot = Join-Path $Repo 'build\scratch\m24_menu\menu-elevated.png'
    Run-Step 'f5-elevated-menu' 'm24_menu.ps1' @(
        '-Repo', $Repo, '-Case', 'elevated', '-Shot', $shot,
        '-AnchorX', '1200', '-AnchorY', '400')
    if ($script:exits['f5-elevated-menu'] -ne 0) { $script:phaseBad = $true }

    # d. The install path goes back whatever the legs concluded.
    Run-Step 'restore' 'm24_swap.ps1' @('-Repo', $Repo, '-Mode', 'restore')
    if ($script:exits['restore'] -ne 0) { $script:phaseBad = $true }

    "=== final summary ==="
    "  service state: $(State $Service)"
    "  start type: $(Start-Type $Service)"
    "  installed FileVersion: $((Get-Item -LiteralPath $InstalledExe).VersionInfo.FileVersion)"
    "  install dir: $((Get-ChildItem -LiteralPath (Split-Path -Parent $InstalledExe) -Filter '*.exe' | ForEach-Object { $_.Name }) -join ', ')"
    foreach ($k in ($script:exits.Keys | Sort-Object)) { "  $k exit=$($script:exits[$k])" }
    Stop-Transcript | Out-Null
    if ($script:phaseBad) { "  RESULT: at least one leg did not pass - see the files above"; exit 1 }
    exit 0
}

if ($Phase -eq 'restore') {
    # The hung leg can only be measured while the swapped-in 1.0.7 owns the path
    # the service points at, and what follows takes that away. So it goes first.
    if ($RerunHung) {
        if ((State $Service) -ne 'RUNNING') {
            [void](& sc.exe start $Service 2>&1 | Out-String)
            [void](Wait-State $Service 'RUNNING' 20000)
        }
        Run-Step 'f4-hung' 'm24_f4.ps1' @('-Repo', $Repo, '-Case', 'hung')
    }
    # The installed image goes back, byte-for-byte, and the service goes up on it.
    Run-Step 'restore' 'm24_swap.ps1' @('-Repo', $Repo, '-Mode', 'restore')
    "  service state: $(State $Service)"
    "  start type: $(Start-Type $Service)"
    "  installed FileVersion: $((Get-Item -LiteralPath $InstalledExe).VersionInfo.FileVersion)"
    Stop-Transcript | Out-Null
    $bad = @($script:exits.Keys | Where-Object { $script:exits[$_] -ne 0 })
    if ($bad.Count) { "  failed steps: $($bad -join ', ')"; exit 1 }
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
