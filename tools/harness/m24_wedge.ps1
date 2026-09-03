# Is a proxy that stops draining also stopped being heard from?
#
# tray_proxies.cpp gives every client two threads, reader_loop and writer_loop,
# and hands BOTH of them the same non-overlapped pipe handle (c->pipe). The header
# of that file promises the opposite of what that can cost: "nothing on the host's
# own threads may block waiting for a proxy". A peer that stops reading parks the
# writer inside a synchronous WriteFile; if the reader shares that handle's I/O
# context, it parks too - and every command that proxy already sent, including a
# "quit" from a tray click, waits in the input buffer until the 8s pong timeout
# throws the connection away with the commands still in it.
#
# Three phases, one variable: how full this connection's inbound buffer is.
#   A control : buffer low   -> the toggles must show up in the host log
#   B wedge   : buffer full  -> the same toggles must NOT show up
#   C causal  : drain it     -> the backlog must show up within milliseconds
# A is the proof that the counting query can report non-zero; C is the proof that
# B measured a coupling and not a dead thread.
#
# No elevation: this leg never sends "quit", so the service is not asked to stop
# and the pipe DACL (which grants the interactive user read/write) is enough. It
# does pause and resume remote control on this machine ~40 times in ~30 seconds.
#
# The leg cleans up after itself whatever it concludes: drain, resume, close.
param(
    [string] $Repo = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)),
    [string] $Service = 'MyRemoteAgent',
    [string] $LogDir = 'C:\ProgramData\MyRemote',
    [string] $PipeName = 'MyRemoteAgent_TrayProxy_v1',
    # Named only to report which image is on the other end of the pipe; this leg
    # talks to the running host and never launches an exe.
    [string] $InstalledExe = 'C:\MyRemote\Agent\agent.exe',
    # This connection counts as full at this many bytes waiting to be read, and
    # the pump gives up after this many seconds so a build that drains itself
    # cannot spin here forever.
    [int] $FillBytes = 4000,
    [int] $PumpLimitSeconds = 60,
    [int] $ControlToggles = 4,
    [int] $WedgeToggles = 3
)

$ErrorActionPreference = 'Stop'
$work = Join-Path $Repo 'build\scratch\m24_wedge'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$result = Join-Path $work 'wedge.txt'
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
    // Peek only. Reading here would drain the buffer the whole leg is about.
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

function Size-Of([string] $path) {
    if (Test-Path -LiteralPath $path) { return (Get-Item -LiteralPath $path).Length }
    return 0
}

# Read the host log from a byte offset the way a proxy would: what is new is what
# just happened. Rescanning the whole file would let last week's toggles answer
# this leg, which is the oldest way there is to write an assertion that cannot be
# False.
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

function Count-Toggles([long] $offset) {
    $lines = @(Tail-From $hostLog $offset |
               Where-Object { $_ -match 'Remote control (paused|resumed) from a session tray' })
    return $lines.Count
}

# A line the host does not recognise is echoed straight into the log by dispatch()
# before any command handler exists, so this counts what the READER consumed - as
# opposed to the toggles above, which need the pause/resume path to run as well.
# Sending both is what separates "nobody read my bytes" from "read them, then got
# stuck downstream".
function Count-Marker([long] $offset, [string] $tag) {
    $lines = @(Tail-From $hostLog $offset |
               Where-Object { $_.Contains($tag) })
    return $lines.Count
}

$hostLog = Join-Path $LogDir 'agent.log'
$ctl = New-Object System.ServiceProcess.ServiceController($Service)
function Get-AgentState { $ctl.Refresh(); return [string]$ctl.Status }

$pipe = $null
$report = @()
$script:failures = @()
$script:heard = @()
# Kept so the cleanup can report whether the machine is left with remote control
# working - this leg pauses it on purpose, and a leg that ends paused is a leg
# that owes somebody a phone call.
$script:leftPaused = $true

function Drain-All([int] $limitMs) {
    # Reading here is the point of phase C: it is what releases a write parked
    # against us. Returns bytes taken, and keeps every state line it sees because
    # that is the only reading this leg has of the host's pause flag.
    $bytes = 0
    $sw = [Diagnostics.Stopwatch]::StartNew()
    try {
        while ($sw.ElapsedMilliseconds -lt $limitMs) {
            if ([PipeGauge]::Available($gauge) -le 0) { break }
            $buf = New-Object byte[] 8192
            $n = $pipe.Read($buf, 0, $buf.Length)
            if ($n -le 0) { break }
            $bytes += $n
            foreach ($raw in (([Text.Encoding]::ASCII.GetString($buf, 0, $n) -split "`n"))) {
                $line = $raw.TrimEnd("`r")
                if ($line -like 'state *') { $script:heard += $line }
            }
            Start-Sleep -Milliseconds 20
        }
    } catch {
        $script:heard += "DRAIN STOPPED: $($_.Exception.Message)"
    }
    return $bytes
}

function Send-Toggles([int] $count, [int] $spacingMs) {
    for ($i = 0; $i -lt $count; $i++) {
        $w.WriteLine('pause'); Start-Sleep -Milliseconds $spacingMs
        $w.WriteLine('resume'); Start-Sleep -Milliseconds $spacingMs
        # script scope on purpose: a function-local $tick would repeat
        # "pong tray=0" forever, the host counts that as a stalled pump, and this
        # leg would get reaped for "tray pump not turning" instead of measuring.
        $script:tick += 1
        $w.WriteLine("pong tray=$($script:tick)")
    }
}

try {
    $installed = 'unknown'
    if (Test-Path -LiteralPath $InstalledExe) {
        $installed = (Get-Item -LiteralPath $InstalledExe).VersionInfo.FileVersion
    }
    $report += "measured host = the running service, image $InstalledExe FileVersion=$installed"
    $report += "agent.log = $hostLog"
    $report += "service at start = $(Get-AgentState)"
    if ((Get-AgentState) -ne 'Running') {
        throw 'FAIL: the service is not Running, so there is no host to talk to.' +
              ' This leg needs a live session host; it does not start one.'
    }

    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(
        '.', $PipeName, [System.IO.Pipes.PipeDirection]::InOut)
    $pipe.Connect(3000)
    $w = New-Object IO.StreamWriter($pipe)
    # Bare LF, not CRLF: reader_loop splits on '\n' and leaves the '\r' inside the
    # command, so the .NET default newline arrives as "pause\r" and is answered
    # "unknown command from session N". Real proxies send LF.
    $w.NewLine = "`n"
    $w.AutoFlush = $true
    $gauge = $pipe.SafePipeHandle
    $script:tick = 0
    $report += "connected as a proxy client, pid=$PID"

    # ---------- A: control, buffer low ----------
    # A unique tag per run, so the marker count needs no offset bookkeeping and
    # cannot pick up another leg's leftovers.
    $tag = "wedgeprobe$PID"
    $offA = Size-Of $hostLog
    $w.WriteLine("$tag-A")
    Send-Toggles $ControlToggles 250
    Start-Sleep -Milliseconds 1500
    $seenA = Count-Toggles $offA
    $markersA = Count-Marker $offA $tag
    $availA = [PipeGauge]::Available($gauge)
    $wantA = $ControlToggles * 2
    $report += "A control: sent $wantA toggles with the buffer at avail=$availA," +
               " host logged $seenA of them; $markersA/1 junk markers read"
    $ok_control = ($seenA -eq $wantA -and $markersA -ge 1)

    # ---------- B: fill this connection, then talk again ----------
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $avail = $availA
    $pumped = 0
    while ($sw.ElapsedMilliseconds -lt ($PumpLimitSeconds * 1000)) {
        $w.WriteLine('pause'); Start-Sleep -Milliseconds 230
        $w.WriteLine('resume'); Start-Sleep -Milliseconds 230
        $script:tick += 1
        $w.WriteLine("pong tray=$($script:tick)")
        $pumped += 1
        $avail = [PipeGauge]::Available($gauge)
        if ($avail -ge $FillBytes) { break }
    }
    $filled = ($avail -ge $FillBytes)
    $report += "B pump: $pumped toggles in $($sw.ElapsedMilliseconds)ms to get avail=$avail"
    if (-not $filled) {
        # Without a full buffer there is nothing parked and the rest of the leg
        # would be measuring an experiment that never ran.
        $report += "PRECONDITION FAILED: the buffer never reached $FillBytes bytes," +
                   " so nothing was parked; B and C are meaningless this run"
    } else {
        # Settle before the offset: the pump's last toggle was written while the
        # buffer was still under the limit, and letting it land inside B's window
        # counted a non-wedged toggle as a wedged one (read 2/6 on a run whose
        # after-full count was really 0).
        Start-Sleep -Milliseconds 1500
        $offB = Size-Of $hostLog
        for ($i = 1; $i -le $WedgeToggles; $i++) {
            $w.WriteLine("$tag-B$i"); Start-Sleep -Milliseconds 200
            $w.WriteLine('pause'); Start-Sleep -Milliseconds 400
            $w.WriteLine('resume'); Start-Sleep -Milliseconds 400
            $script:tick += 1
            $w.WriteLine("pong tray=$($script:tick)")
        }
        $afterB = [PipeGauge]::Available($gauge)
        Start-Sleep -Milliseconds 2000
        $seenB = Count-Toggles $offB
        $markersB = Count-Marker $offB $tag
        $wantB = $WedgeToggles * 2
        $report += "B wedge: sent $wantB toggles and $WedgeToggles junk markers with" +
                   " the buffer at avail=$afterB; host logged $seenB toggles and" +
                   " $markersB markers"
        # Two readings, and the pair is the mechanism: markers are answered by the
        # reader alone, toggles only if the pause path also ran.
        $ok_wedged = ($seenB -eq 0)
        $reader_read = ($markersB -gt 0)

        # ---------- C: drain, and see whether the same commands then land -------
        # Counted from $offB rather than from a fresh offset: the backlog was
        # written before the drain, and an offset taken after draining starts would
        # skip the very lines this phase is looking for.
        $drained = Drain-All 3000
        Start-Sleep -Milliseconds 2000
        $seenC = Count-Toggles $offB
        $markersC = Count-Marker $offB $tag
        $report += "C causal: drained $drained bytes, then the host logged $seenC of" +
                   " the $wantB backlog toggles and $markersC of the $WedgeToggles markers" +
                   " within 2s"
        $ok_causal = ($seenC -ge 2)
        if ($seenB -eq 0 -and $markersB -eq 0) {
            $report += "MECHANISM: the reader consumed nothing at all while our side of the" +
                       " pipe was full - the parked write holds the read on the same handle"
        } elseif ($markersB -gt 0) {
            $report += "MECHANISM: the reader DID consume the stream ($markersB markers)" +
                       " while the pause path did not run - the block is downstream of read"
        } else {
            $report += "MECHANISM: inconclusive this run ($seenB toggles, $markersB markers)"
        }

        # ---------- leave the machine as found ----------
        # The last word has to be "resume", and it has to be heard: send it, drain
        # again so the reader gets to it, then read back the state line.
        $w.WriteLine('resume')
        Start-Sleep -Milliseconds 800
        [void](Drain-All 2000)
        $last = ''
        foreach ($l in @($script:heard | Where-Object { $_ -like 'state *' } |
                         Select-Object -Last 1)) { $last = $l }
        $script:leftPaused = ($last -notmatch 'paused=0')
        $report += "last state heard on this connection: $(if ($last) { $last } else { 'none' })"
        $stalled = @($script:heard | Where-Object { $_ -like 'DRAIN STOPPED*' })
        if ($stalled.Count) { $report += "note: $($stalled[0]) - the host had already reaped this connection" }
        $report += "service at end = $(Get-AgentState)"

        $report += "ASSERT the host heard the toggles while the buffer was low -> $ok_control ($seenA/$wantA)"
        $report += "ASSERT the host stopped hearing them once it was full -> $ok_wedged ($seenB/$wantB)"
        $report += "ASSERT draining the buffer brought them back -> $ok_causal ($seenC >= 2)"
        foreach ($p in @($ok_control, $ok_wedged, $ok_causal)) {
            if (-not $p) { $script:failures += 'assertion failed' }
        }
        if (-not $filled) { $script:failures += 'precondition: buffer never filled' }
    }
}
catch {
    $script:failures += $_.Exception.Message
    $report += "ERROR: $($_.Exception.Message)"
}
finally {
    try { if ($pipe) { $pipe.Dispose() } } catch { }
    $report += "left remote control paused: $($script:leftPaused) - if True, resume it from the tray or a later leg"
    [IO.File]::WriteAllText($result, ($report -join "`r`n") + "`r`n",
        (New-Object Text.UTF8Encoding($true)))
    foreach ($l in $report) { "  | $(Fold $l)" }
    "RESULT=$result"
    if ($script:failures.Count) {
        foreach ($f in $script:failures) { "FAIL: $(Fold $f)" }
        throw "leg did not pass ($($script:failures.Count) failure(s))"
    }
}
