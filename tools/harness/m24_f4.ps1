# M24-4 leg: does the bye reach the wire, and is the gate bounded?
#
# F4's claim is that trayproxies::stop() waits for the word to be *written*, not
# for it to leave the queue. The only honest way to watch that is from the other
# end of the pipe, so this script IS a proxy client: it connects, sends "quit",
# and times what comes back.
#
#   healthy : reads its pipe. "bye" should arrive in tens of milliseconds and the
#             service should be STOPPED well under the 1100ms the old code spent
#             parked in kByeFlushMs + Sleep(100) on every exit, healthy or not.
#   hung    : never reads. State changes are pumped out of the host (pause/resume
#             is the only host-initiated traffic available on demand, and
#             broadcast_state dedupes, so each toggle is one line) until this
#             connection's inbound buffer is full - PeekNamedPipe is the gauge,
#             and it does not consume. Then the host's writer thread is parked
#             inside WriteFile with the bye still queued, which is the only state
#             that can produce the "bye unconfirmed" warn.
#
# Run elevated: a graceful quit stops the service, and only an admin token can
# put it back afterwards. The finally block does that whatever the leg concluded.
param(
    [ValidateSet('healthy', 'hung')] [string] $Case = 'healthy',
    [string] $Repo = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)),
    [string] $Service = 'MyRemoteAgent',
    [string] $LogDir = 'C:\ProgramData\MyRemote',
    [string] $InstalledExe = 'C:\MyRemote\Agent\agent.exe',
    # hung case only: how long to keep the host talking before giving up.
    [int] $PumpLimitSeconds = 90,
    [int] $StopTimeoutMs = 25000,
    # The pipe's DACL gives the interactive user read/write, so connecting and
    # quitting need no admin - only putting the service back does. Set this to
    # run the leg unelevated and leave the restart to a later admin window.
    [switch] $NoRestart
)

$ErrorActionPreference = 'Stop'
$work = Join-Path $Repo 'build\scratch\m24_f4'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$result = Join-Path $work "f4-$Case.txt"
if (Test-Path -LiteralPath $result) { Remove-Item -LiteralPath $result -Force }

Add-Type -AssemblyName System.Core
Add-Type -AssemblyName System.ServiceProcess
Add-Type -Language CSharp @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public static class PipeGauge {
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool PeekNamedPipe(SafePipeHandle h, byte[] buf, int size,
                                            out int read, out int avail, out int msgLeft);
    // The stream owns the handle, so peek through its accessor without taking a
    // second reference that could close the connection on dispose.
    public static int Available(SafePipeHandle h) {
        int read, avail, left;
        if (!PeekNamedPipe(h, null, 0, out read, out avail, out left)) return -1;
        return avail;
    }
}
'@

function Fold([string] $s) {
    -join ($s.ToCharArray() | ForEach-Object {
        if ([int]$_ -lt 32 -or [int]$_ -ge 127) { '?' } else { $_ }
    })
}

function Tail-From([string] $path, [long] $offset) {
    if (-not (Test-Path -LiteralPath $path)) { return @() }
    $fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
    try {
        if ($fs.Length -le $offset) { return @() }
        [void]$fs.Seek($offset, 'Begin')
        $sr = New-Object IO.StreamReader($fs)
        return @(($sr.ReadToEnd() -split "`r?`n") | Where-Object { $_ -ne '' })
    } finally { $fs.Dispose() }
}

function Size-Of([string] $path) {
    if (Test-Path -LiteralPath $path) { return (Get-Item -LiteralPath $path).Length }
    return 0
}

$ctl = New-Object System.ServiceProcess.ServiceController($Service)
function Get-AgentState { $ctl.Refresh(); return [string]$ctl.Status }

function Wait-Agent([string] $want, [int] $timeoutMs) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        if ((Get-AgentState) -eq $want) { return $sw.ElapsedMilliseconds }
        Start-Sleep -Milliseconds 5
    }
    return -1
}

$hostLog = Join-Path $LogDir 'agent.log'
$proxyLog = Join-Path $LogDir 'tray-1.log'
$pipe = $null
$script:failures = @()
$report = @()
$heard = @()

try {
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(
        '.', 'MyRemoteAgent_TrayProxy_v1', [System.IO.Pipes.PipeDirection]::InOut)
    $ver = (Get-Item -LiteralPath $InstalledExe).VersionInfo.FileVersion
    $report += "case=$Case exe=$InstalledExe FileVersion=$ver"
    if ($ver -ne '1.0.7') {
        throw "FAIL: this leg measures the 1.0.7 gate; $InstalledExe is $ver. Run m24_swap.ps1 -Mode swap first."
    }
    if ((Get-AgentState) -ne 'Running') { throw "FAIL: service is $(Get-AgentState), not Running" }
    $report += "service=$([string]$ctl.Status) host_log=$(Size-Of $hostLog) proxy_log=$(Size-Of $proxyLog)"
    $logAt = Size-Of $hostLog
    $proxyAt = Size-Of $proxyLog

    $pipe.Connect(3000)
    $w = New-Object IO.StreamWriter($pipe)
    # Bare LF, not CRLF: reader_loop splits on '\n' and does not trim the '\r', so
    # a .NET default newline arrives as the command "quit\r" and is answered
    # "unknown command from session N" (measured 2026-09-03 09:57 in this leg's own
    # host-log tail). Real proxies send LF.
    $w.NewLine = "`n"
    $w.AutoFlush = $true
    $gauge = $pipe.SafePipeHandle
    # No ReadTimeout on a pipe stream, and a blocking ReadLine would park this leg
    # forever if the host simply had nothing to say. So: ask how much is there,
    # and only read when the answer is non-zero.
    $script:leftover = ''
    $script:readBuf = New-Object byte[] 8192
    function Read-Any {
        $lines = @()
        $avail = [PipeGauge]::Available($gauge)
        if ($avail -le 0) { return $lines }
        $n = $pipe.Read($script:readBuf, 0, $script:readBuf.Length)
        if ($n -le 0) { return $lines }
        $script:leftover += [Text.Encoding]::ASCII.GetString($script:readBuf, 0, $n)
        while ($script:leftover.Contains("`n")) {
            $i = $script:leftover.IndexOf("`n")
            $line = $script:leftover.Substring(0, $i).TrimEnd("`r")
            $script:leftover = $script:leftover.Substring($i + 1)
            if ($line -ne '') { $lines += $line }
        }
        return $lines
    }
    $tick = 0
    $report += "connected as a proxy client, pid=$PID"

    $byeMs = -1
    $stoppedMs = -1

    if ($Case -eq 'healthy') {
        # Warm up: drain the hello line the host queues on accept and keep
        # answering pings, so this client is the healthy kind a real proxy is.
        # Breaking on the first line would leave later pongs unsent and get this
        # client reaped for "no pong" mid-leg.
        $sw = [Diagnostics.Stopwatch]::StartNew()
        while ($sw.ElapsedMilliseconds -lt 1200) {
            foreach ($line in (Read-Any)) {
                $heard += $line
                if ($line -eq 'ping') { $tick++; $w.WriteLine("pong tray=$tick") }
            }
            Start-Sleep -Milliseconds 20
        }
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $w.WriteLine('quit')
        while ($sw.ElapsedMilliseconds -lt 5000) {
            foreach ($line in (Read-Any)) {
                $heard += $line
                if ($line -eq 'ping') { $tick++; $w.WriteLine("pong tray=$tick"); continue }
                if ($line -eq 'bye') { $byeMs = $sw.ElapsedMilliseconds }
            }
            if ($byeMs -ge 0) { break }
        }
        $stoppedMs = Wait-Agent 'Stopped' $StopTimeoutMs
    }
    else {
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $avail = 0
        $toggles = 0
        while ($sw.ElapsedMilliseconds -lt ($PumpLimitSeconds * 1000)) {
            [void]$w.WriteLine('pause')
            Start-Sleep -Milliseconds 230
            [void]$w.WriteLine('resume')
            Start-Sleep -Milliseconds 230
            $tick += 2
            [void]$w.WriteLine("pong tray=$tick")   # writing is independent of reading
            $toggles += 1
            $avail = [PipeGauge]::Available($gauge)
            if ($avail -ge 4000) { break }
        }
        $pumped = $sw.ElapsedMilliseconds
        $report += "pumped $toggles toggles for ${pumped}ms, inbound buffer now avail=$avail/4096"
        if ($avail -lt 4000) {
            $report += "NOTE the buffer never filled (avail=$avail) - the hung leg cannot be built this run"
        }
        # Stop talking: the host's writer is parked in WriteFile, so the bye below
        # can only sit in the queue.
        Start-Sleep -Milliseconds 300
        $sw = [Diagnostics.Stopwatch]::StartNew()
        [void]$w.WriteLine('quit')
        # Stay alive and keep answering for the length of the gate - a dead pid or
        # a shut door would settle it early for the wrong reason.
        $hold = $sw.ElapsedMilliseconds + 4200
        while ($sw.ElapsedMilliseconds -lt $hold) {
            $tick++
            try { $w.WriteLine("pong tray=$tick") } catch { break }
            Start-Sleep -Milliseconds 250
        }
        $stoppedMs = Wait-Agent 'Stopped' $StopTimeoutMs
    }

    $report += "bye on the wire after=${byeMs}ms"
    $report += "service STOPPED ${stoppedMs}ms after quit"
    if ($heard.Count) { $report += "heard: $(($heard | Select-Object -First 8) -join ' | ')" }

    Start-Sleep -Milliseconds 500
    $tail = Tail-From $hostLog $logAt
    $ptail = Tail-From $proxyLog $proxyAt
    foreach ($l in ($tail | Select-Object -Last 40)) { $report += "host-log: $l" }
    foreach ($l in ($ptail | Select-Object -Last 12)) { $report += "proxy-log: $l" }

    $unconfirmed = @($tail | Where-Object { $_ -match 'bye unconfirmed' })
    if ($Case -eq 'healthy') {
        $ok_bye = ($byeMs -ge 0 -and $byeMs -lt 1100)
        $ok_clean = ($unconfirmed.Count -eq 0)
        $ok_proxy = (@($ptail | Where-Object { $_ -match 'host said bye' }).Count -gt 0)
        $ok_stop = ($stoppedMs -ge 0 -and $stoppedMs -lt 1100)
        $report += "ASSERT bye reached this client in <1100ms -> $ok_bye (${byeMs}ms)"
        $report += "ASSERT no 'bye unconfirmed' in the host log -> $ok_clean"
        $report += "ASSERT the real proxy logged 'host said bye' -> $ok_proxy"
        $report += "ASSERT service stopped <1100ms after quit -> $ok_stop (${stoppedMs}ms)"
        foreach ($p in @($ok_bye, $ok_clean, $ok_proxy, $ok_stop)) {
            if (-not $p) { $script:failures += 'assertion failed' }
        }
    }
    else {
        $ok_named = ($unconfirmed.Count -ge 1)
        $ok_bound = ($stoppedMs -ge 2400 -and $stoppedMs -le 5000)
        $report += "ASSERT the gate named a session ('bye unconfirmed') -> $ok_named"
        $report += "ASSERT the wait was bounded by the 2500ms deadline -> $ok_bound (${stoppedMs}ms)"
        foreach ($p in @($ok_named, $ok_bound)) {
            if (-not $p) { $script:failures += 'assertion failed' }
        }
    }
}
catch {
    $script:failures += $_.Exception.Message
    $report += "ERROR: $($_.Exception.Message)"
}
finally {
    try { $pipe.Dispose() } catch { }
    if ($NoRestart) {
        $report += "restart skipped by request - service left as $(Get-AgentState); an admin window owes a start"
    }
    elseif ((Get-AgentState) -ne 'Running') {
        [void](& sc.exe start $Service 2>&1 | Out-String)
        $back = Wait-Agent 'Running' 20000
        $report += "restart service -> $(Get-AgentState) after ${back}ms"
    } else {
        $report += "restart service: still Running, nothing to do"
    }
    [IO.File]::WriteAllText($result, ($report -join "`r`n") + "`r`n",
        (New-Object Text.UTF8Encoding($true)))
    foreach ($l in $report) { "  | $(Fold $l)" }
    "RESULT=$result"
    if ($script:failures.Count) {
        foreach ($f in $script:failures) { "FAIL: $f" }
        throw "leg did not pass ($($script:failures.Count) failure(s))"
    }
}
