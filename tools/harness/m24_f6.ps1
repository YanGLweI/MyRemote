# M24-1 / F6: can the installed 1.0.6 service actually be stopped?
#
# Why this is a measurement and not a code read: a host process sat in
# STOP_PENDING for 23 minutes on 2026-09-02 while service.log wrote nothing
# after "Service stop requested". The committed shutdown_host() is bounded, so
# the wedge was either an uncommitted build or something the code read cannot
# see. Only start/stop round trips on the machine settle it.
#
# Run elevated (one prompt) - a limited token cannot control the service:
#   powershell -Command "Start-Process powershell -Verb RunAs -Wait -ArgumentList
#     '-NoProfile','-ExecutionPolicy','Bypass','-File','<this file>','-Out','<txt>'"
#
# The machine is briefly uncontrolled while this runs. It ends by starting the
# service again and asserting RUNNING, so a failure in the middle still leaves
# the box reachable.
param(
    [int] $Rounds = 3,
    [int] $SettleMs = 10000,
    [string] $ServiceName = 'MyRemoteAgent',
    [string] $Out = ''
)

$ErrorActionPreference = 'Stop'
$filt = "Name='$ServiceName'"

$lines = @()
function Say([string] $s) {
    $script:lines += $s
    $s
}

function Get-ServiceState([string] $name) {
    $q = sc.exe query $name 2>&1 | Out-String
    if ($q -match 'STATE\s+:\s+(\d+)\s+(\w+)') {
        return @{ code = [int]$Matches[1]; name = $Matches[2] }
    }
    return @{ code = -1; name = 'UNKNOWN' }
}

function Wait-State([string] $name, [int] $want, [int] $budget) {
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalMilliseconds -lt $budget) {
        $st = Get-ServiceState $name
        if ($st.code -eq $want) {
            return @{ ok = $true; ms = [int]((Get-Date) - $t0).TotalMilliseconds; state = $st.name }
        }
        Start-Sleep -Milliseconds 200
    }
    $st = Get-ServiceState $name
    return @{ ok = $false; ms = $budget; state = $st.name }
}

# Two log roots in the wild: the installed agent writes next to its own files,
# ProgramData wins when it exists. Newest file is the live one.
$logCandidates = @(
    'C:\ProgramData\MyRemote\service.log',
    'C:\MyRemote\Agent\service.log'
) | Where-Object { Test-Path -LiteralPath $_ }
$log = $null
if ($logCandidates) {
    # @() because a one-item pipeline is a scalar here, and [0] on a string is
    # its first character - which is exactly how service.log became "C".
    $log = @($logCandidates | Sort-Object { (Get-Item -LiteralPath $_).LastWriteTime } -Descending)[0]
}

$exe = 'C:\MyRemote\Agent\agent.exe'
Say "started=$(Get-Date -Format 'HH:mm:ss') service=$ServiceName rounds=$Rounds settle_ms=$SettleMs"
if (Test-Path -LiteralPath $exe) {
    Say "installed exe FileVersion=$((Get-Item -LiteralPath $exe).VersionInfo.FileVersion)"
}
Say "binPath=$((Get-CimInstance Win32_Service -Filter $filt).PathName)"
Say "service.log=$log"
Say "before: $(Get-ServiceState $ServiceName | ForEach-Object { "$($_.code) $($_.name)" })"

$failures = @()
try {
    for ($r = 1; $r -le $Rounds; $r++) {
        $st = Get-ServiceState $ServiceName
        if ($st.code -ne 1) {
            # Asking first: waiting for STOPPED without sending a stop can only
            # ever time out, and a timeout here is a finding, not a formality.
            [void](sc.exe stop $ServiceName 2>&1 | Out-String)
            $s = Wait-State $ServiceName 1 $SettleMs
            Say "round $r pre-clean stop -> $($s.state) in $($s.ms)ms ok=$($s.ok)"
            if (-not $s.ok) { $failures += "round $r pre-clean stop did not land" }
        }
        $logLen = 0
        if ($log) { $logLen = (Get-Item -LiteralPath $log).Length }

        [void](sc.exe start $ServiceName 2>&1 | Out-String)
        $up = Wait-State $ServiceName 4 $SettleMs
        Say "round $r start -> $($up.state) in $($up.ms)ms ok=$($up.ok)"
        if (-not $up.ok) { $failures += "round $r did not reach RUNNING in ${SettleMs}ms" }

        [void](sc.exe stop $ServiceName 2>&1 | Out-String)
        $down = Wait-State $ServiceName 1 $SettleMs
        Say "round $r stop -> $($down.state) in $($down.ms)ms ok=$($down.ok)"
        if (-not $down.ok) { $failures += "round $r did not reach STOPPED in ${SettleMs}ms" }

        # The log is the part a state number cannot tell you: a service that
        # reports STOPPED without ever writing past "stop requested" stopped for
        # the wrong reason - or was killed by something else.
        if ($log) {
            $tail = ((Get-Item -LiteralPath $log).Length - $logLen)
            $after = @()
            if ($tail -gt 0) {
                $fs = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
                try {
                    [void]$fs.Seek($logLen, 'Begin')
                    $sr = New-Object IO.StreamReader($fs)
                    $after = @($sr.ReadToEnd() -split "`r?`n" | Where-Object { $_ })
                } finally { $fs.Dispose() }
            }
            $stop_line = ($after | Where-Object { $_ -match 'stop requested' }).Count
            $past = @($after | Where-Object { $_ -notmatch 'stop requested' })
            Say "round $r log grew $tail bytes, $($after.Count) lines, 'stop requested'=$stop_line, later lines=$($past.Count)"
            foreach ($p in $past | Select-Object -First 6) { Say "    | $p" }
            if ($stop_line -eq 0) { $failures += "round $r wrote no 'stop requested' line" }
            if ($past.Count -eq 0) { $failures += "round $r log ends at 'stop requested' - the shutdown path never finished talking" }
        } else {
            Say "round $r service.log not found - cannot judge what the shutdown said"
            $failures += 'no service.log to read'
        }
    }
} catch {
    # Without this, an abort lands in the cleanup with an empty $failures and the
    # report says failures=0 about a run that never finished.
    $failures += "aborted: $($_.Exception.Message)"
    throw
} finally {
    # Leave the box controllable whatever happened above.
    [void](sc.exe start $ServiceName 2>&1 | Out-String)
    $final = Wait-State $ServiceName 4 $SettleMs
    Say "restored: $($final.state) in $($final.ms)ms ok=$($final.ok)"
    Say "start type=$((Get-CimInstance Win32_Service -Filter $filt).StartMode)"
    Say "failures=$($failures.Count)"
    foreach ($f in $failures) { Say "  FAIL $f" }
    if ($Out) {
        [IO.File]::WriteAllText($Out, ($lines -join "`r`n") + "`r`n",
                                (New-Object Text.UTF8Encoding($true)))
    }
}
if ($failures.Count) { throw "F6: $($failures.Count) assertion(s) failed - see $Out" }
