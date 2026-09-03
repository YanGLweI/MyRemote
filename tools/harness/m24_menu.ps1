# M24-5 legs 1/2/3: pop the tray menu of one specific agent instance and read
# what it actually says, item by item, with geometry.
#
# Why this exists: F5's whole claim is that one menu item changes its wording and
# then really does something. The last time this project claimed a tray fix had
# shipped, the item was unchanged on the machine - so the judgement has to come
# from the rendered menu, not from a commit message.
#
# Constraints, each of which has cost an hour before:
#   * ASCII-only source. PS 5.1 decodes a no-BOM UTF-8 script as ANSI, so a CJK
#     literal in here would never match. Chinese is compared by code point and
#     written to a UTF-8 result file for the human pass.
#   * The limited-token case must run NON-elevated: UIPI blocks PostMessage from
#     high IL into a medium IL window, so an elevated harness turns every
#     limited leg into a false red. The elevated case therefore runs this whole
#     script elevated and the caller only reads the file it leaves behind.
#   * The target gets its own copy of agent.exe and its own config.json:
#     retire_same_path_instances matches on full path, so an isolated copy can
#     neither kill the instance already on the user's tray nor be killed by it.
#   * TrackPopupMenuEx is modal to the tray thread. Every exit path must close
#     the menu it opened, or that thread sits in it (one such menu was left
#     hanging for 8 minutes on 2026-09-01).
param(
    [string] $Repo = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)),
    [string] $Source = '',          # exe to copy; default build\bin\Release\agent.exe
    [string] $Case = 'limited',     # limited | elevated
    [int] $StartupWaitMs = 30000,
    [int] $MenuWaitMs = 5000,
    # The popup is tracked at the real cursor (tray_icon.cpp:350), so an unpinned
    # cursor moves the thing being measured between runs.
    [int] $AnchorX = 1200,
    [int] $AnchorY = 400,
    # Where to leave the picture of the rendered menu. Without it the only
    # evidence about the item's wording is the target's own log line about
    # itself, which is a claim, not a reading.
    [string] $Shot = '',
    # '' | approve | decline. When set, the menu stays open and a person at the
    # machine picks the start-service item and answers the consent box. The click is not
    # simulated: the box appears on the secure desktop, where no other process
    # can see or reach it - which is the whole point of the leg.
    [ValidateSet('', 'approve', 'decline')] [string] $Click = '',
    [int] $ClickWaitMs = 180000,
    [string] $Service = 'MyRemoteAgent',
    [switch] $KeepAlive   # leave the instance and its icon behind for a follow-up probe
)

$ErrorActionPreference = 'Stop'

if (-not $Source) { $Source = Join-Path $Repo 'build\bin\Release\agent.exe' }
if (-not (Test-Path -LiteralPath $Source)) { throw "FAIL: no such source exe: $Source" }

$work = Join-Path $Repo 'build\scratch\m24_menu'
$exe = Join-Path $work ('agent-' + $Case + '.exe')
$cfg = Join-Path $work ('config-' + $Case + '.json')
$result = Join-Path $work ('menu-' + $Case + '.txt')

New-Item -ItemType Directory -Force -Path $work | Out-Null
Copy-Item -LiteralPath $Source -Destination $exe -Force
# A dead endpoint on purpose: this instance must never reach a live server, or
# the run would add a device row to somebody's console. 127.0.0.1:1 refuses at once.
'{ "server_ip": "127.0.0.1", "server_port": 1, "secret_key": "m24-probe",' +
' "device_name": "M24-PROBE", "control_password": "", "tray_icon": true }' |
    Set-Content -LiteralPath $cfg -Encoding ASCII
if (Test-Path -LiteralPath $result) { Remove-Item -LiteralPath $result -Force }


Add-Type -Language CSharp @'
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class Win {
    public delegate bool EnumWindowsProc(IntPtr h, IntPtr lp);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumWindowsProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);

    static string TextOf(IntPtr h) {
        StringBuilder tb = new StringBuilder(512);
        GetWindowText(h, tb, tb.Capacity);
        return tb.ToString();
    }

    public static IntPtr Find(uint targetPid, string className) {
        IntPtr hit = IntPtr.Zero;
        EnumWindows((h, lp) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid != targetPid) return true;
            StringBuilder sb = new StringBuilder(64);
            GetClassName(h, sb, sb.Capacity);
            if (sb.ToString() == className) { hit = h; return false; }
            return true;
        }, IntPtr.Zero);
        return hit;
    }

    // parent/child lookup by class and, when given, exact visible text. Text is
    // how the save button is picked: its control id comes from a C++ enum that
    // reorders whenever someone adds a field, and a harness that clicks the
    // wrong button silently cancels the instance instead.
    public static IntPtr FindChild(uint targetPid, IntPtr parent, string className, string text) {
        IntPtr hit = IntPtr.Zero;
        EnumChildWindows(parent, (h, lp) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid != targetPid) return true;
            StringBuilder sb = new StringBuilder(64);
            GetClassName(h, sb, sb.Capacity);
            if (className != null && sb.ToString() != className) return true;
            if (text != null && TextOf(h) != text) return true;
            hit = h; return false;
        }, IntPtr.Zero);
        return hit;
    }

    public static string[] ChildLines(uint targetPid, IntPtr parent, string className) {
        System.Collections.Generic.List<string> hits = new System.Collections.Generic.List<string>();
        EnumChildWindows(parent, (h, lp) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid != targetPid) return true;
            StringBuilder sb = new StringBuilder(64);
            GetClassName(h, sb, sb.Capacity);
            if (className != null && sb.ToString() != className) return true;
            string t = TextOf(h);
            if (t.Length > 0) hits.Add(t);
            return true;
        }, IntPtr.Zero);
        return hits.ToArray();
    }

    public static string[] TopLines(uint targetPid) {
        System.Collections.Generic.List<string> hits = new System.Collections.Generic.List<string>();
        EnumWindows((h, lp) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid != targetPid) return true;
            StringBuilder sb = new StringBuilder(64);
            GetClassName(h, sb, sb.Capacity);
            hits.Add(sb.ToString() + " hwnd=" + h.ToInt64());
            return true;
        }, IntPtr.Zero);
        return hits.ToArray();
    }

    public static string[] ChildIds(uint targetPid, IntPtr parent) {
        System.Collections.Generic.List<string> hits = new System.Collections.Generic.List<string>();
        EnumChildWindows(parent, (h, lp) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid != targetPid) return true;
            StringBuilder sb = new StringBuilder(64);
            GetClassName(h, sb, sb.Capacity);
            hits.Add(sb.ToString() + " id=" + GetDlgCtrlID(h) +
                     " textlen=" + TextOf(h).Length);
            return true;
        }, IntPtr.Zero);
        return hits.ToArray();
    }

    // Handles, not descriptions: a notice box is dismissed by clicking the one
    // button it has, and which control id that button carries is the dialog
    // template's business, not the harness's.
    public static IntPtr[] ChildHandles(uint targetPid, IntPtr parent, string className) {
        System.Collections.Generic.List<IntPtr> hits = new System.Collections.Generic.List<IntPtr>();
        EnumChildWindows(parent, (h, lp) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid != targetPid) return true;
            StringBuilder sb = new StringBuilder(64);
            GetClassName(h, sb, sb.Capacity);
            if (className != null && sb.ToString() != className) return true;
            hits.Add(h);
            return true;
        }, IntPtr.Zero);
        return hits.ToArray();
    }
}
'@

$WM_APP = 0x8000
$WM_RBUTTONUP = 0x0205
$WM_KEYDOWN = 0x0100
$WM_CANCELMODE = 0x01F6
$VK_ESCAPE = 0x1B
$BM_CLICK = 0x00F5

# U+4FDD U+5B58 U+5E76 U+540E U+53F0 U+8FD0 U+884C ("save and run in background")
# and its U+4FDD U+5B58 ("save") fallback.
$saveRun = -join (@(0x4FDD, 0x5B58, 0x5E76, 0x540E, 0x53F0, 0x8FD0, 0x884C) | ForEach-Object { [char]$_ })
$saveOnly = -join (@(0x4FDD, 0x5B58) | ForEach-Object { [char]$_ })

# Foreground startup is configure-then-run (main.cpp:1611): the tray window does
# not exist until somebody confirms the first-run dialog. This clicks the real
# button rather than passing a flag, so the leg still walks the path a user walks.
#
# PostMessage, not SendMessage: a successful save ends inside MessageBoxW on the
# target's only UI thread, so a sent click would never come back.
function Confirm-ConfigDialog([uint32] $ownerPid, [IntPtr] $dlg) {
    $btn = [Win]::FindChild($ownerPid, $dlg, 'Button', $saveRun)
    if ($btn -eq [IntPtr]::Zero) { $btn = [Win]::FindChild($ownerPid, $dlg, 'Button', $saveOnly) }
    if ($btn -eq [IntPtr]::Zero) {
        $all = [Win]::ChildLines($ownerPid, $dlg, 'Button') -join ' | '
        throw "FAIL: config dialog offers no save button. buttons: $all"
    }
    [void][Win]::PostMessage($btn, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
}

$script:noticeWarn = @()

# The modal notice a save leaves behind. Its text is kept as evidence: "written"
# and "not written" are different stories about the same run.
#
# Clicked by handle, not by control id: the single button of a message box on
# this build reports id=2, so an IDOK guard would leave the box up and the whole
# instance parked behind it. More than one button is never clicked - guessing on
# a dialog is how you press the destructive one.
function Dismiss-Notice([uint32] $ownerPid, [IntPtr] $box) {
    $text = ([Win]::ChildLines($ownerPid, $box, 'Static') -join ' / ')
    $btns = [Win]::ChildHandles($ownerPid, $box, 'Button')
    if ($btns.Count -eq 1) {
        [void][Win]::PostMessage($btns[0], $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
    } else {
        $script:noticeWarn += "box had $($btns.Count) buttons, left alone: " +
                              (([Win]::ChildLines($ownerPid, $box, 'Button')) -join ' | ')
    }
    $text
}

$script:proc = $null
$script:menu = [IntPtr]::Zero
$script:clickFailed = $false

function Close-Menu([IntPtr] $h) {
    if ($h -ne [IntPtr]::Zero -and [Win]::IsWindow($h)) {
        [void][Win]::PostMessage($h, $WM_KEYDOWN, [IntPtr]$VK_ESCAPE, [IntPtr]::Zero)
        [void][Win]::PostMessage($h, $WM_CANCELMODE, [IntPtr]::Zero, [IntPtr]::Zero)
    }
}

function Fold([string] $s) {
    -join ($s.ToCharArray() | ForEach-Object {
        if ([int]$_ -lt 32 -or [int]$_ -ge 127) { '?' } else { $_ }
    })
}

# The SCM's own word, asked from a limited token: whether the menu item did
# anything is not this process's opinion to hold.
function Get-AgentServiceState {
    $q = & sc.exe query $Service 2>&1 | Out-String
    if ($q -match 'STATE\s+:\s+\d+\s+(\w+)') { return $Matches[1] }
    return 'unknown'
}

# Only the bytes gained since $offset, so a verdict cannot come from a previous
# run's line sitting at the top of the same file.
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

function Finish([string[]] $lines) {
    [IO.File]::WriteAllText($result, ($lines -join "`r`n") + "`r`n",
        (New-Object Text.UTF8Encoding($true)))
    foreach ($l in $lines) { "  | $(Fold $l)" }
    "RESULT=$result"
}

try {
    $svcAtStart = Get-AgentServiceState
    # The item this leg reads only exists while the background service is down, so
    # a RUNNING service made "service item present -> False" a reading about the
    # setup rather than the product. Say what the setup owes, and say it first.
    if ($svcAtStart -ne 'STOPPED') {
        throw "FAIL: $Service is $svcAtStart, not STOPPED - the menu item this leg reads only exists " +
              "while the background service is down (sc.exe stop $Service first; click=$Click needs " +
              "STOPPED to RUNNING as its verdict)"
    }
    # Fresh log or no log: reading "Elevation: no" only proves anything if the
    # file cannot be holding last run's verdict.
    $log = Join-Path $work 'agent.log'
    if (Test-Path -LiteralPath $log) { Remove-Item -LiteralPath $log -Force }
    $childArgs = @('--no-elevate', '--config', $cfg)
    if ($Case -eq 'elevated') {
        $script:proc = Start-Process -FilePath $exe -ArgumentList $childArgs -Verb RunAs -PassThru -WindowStyle Hidden
    } else {
        $script:proc = Start-Process -FilePath $exe -ArgumentList $childArgs -PassThru -WindowStyle Hidden
    }
    $childPid = $script:proc.Id

    $tray = [IntPtr]::Zero
    $notices = @()
    $touched = @{}
    $deadline = (Get-Date).AddMilliseconds($StartupWaitMs)
    while ((Get-Date) -lt $deadline) {
        if ($script:proc.HasExited) {
            throw "FAIL: instance exited early (code $($script:proc.ExitCode)) - nothing to read"
        }
        $tray = [Win]::Find([uint32]$childPid, 'MyRemoteAgentTray')
        if ($tray -ne [IntPtr]::Zero) { break }

        # Same-IL notice from a save; it is modal to the dialog thread, so it has
        # to go away before the tray can be created.
        $msgbox = [Win]::Find([uint32]$childPid, '#32770')
        if ($msgbox -ne [IntPtr]::Zero -and -not $touched.ContainsKey([long]$msgbox)) {
            $touched[[long]$msgbox] = $true
            $notices += (Dismiss-Notice ([uint32]$childPid) $msgbox)
            Start-Sleep -Milliseconds 150
            continue
        }

        $dlg = [Win]::Find([uint32]$childPid, 'MyRemoteConfigWnd')
        if ($dlg -ne [IntPtr]::Zero -and -not $touched.ContainsKey([long]$dlg)) {
            $touched[[long]$dlg] = $true
            Confirm-ConfigDialog ([uint32]$childPid) $dlg
            Start-Sleep -Milliseconds 150
            continue
        }
        Start-Sleep -Milliseconds 200
    }
    if ($tray -eq [IntPtr]::Zero) {
        $tops = [Win]::TopLines([uint32]$childPid)
        $why = @("FAIL: no MyRemoteAgentTray window for pid $childPid in ${StartupWaitMs}ms",
                 "  windows owned: $($tops -join ', ')")
        foreach ($n in $notices) { $why += "  save notice: $n" }
        foreach ($w in $tops) {
            if ($w -notlike '#32770*' -and $w -notlike 'MyRemoteConfigWnd*') { continue }
            $hwnd = [IntPtr][long](($w -split 'hwnd=')[1])
            $cls = ($w -split ' hwnd=')[0]
            $why += "  children of ${cls}: $([Win]::ChildIds([uint32]$childPid, $hwnd) -join ' | ')"
        }
        Finish $why
        throw ($why[0])
    }

    [void][Win]::SetCursorPos($AnchorX, $AnchorY)
    Start-Sleep -Milliseconds 120
    [void][Win]::PostMessage($tray, ($WM_APP + 2), [IntPtr]::Zero, [IntPtr]$WM_RBUTTONUP)

    $deadline = (Get-Date).AddMilliseconds($MenuWaitMs)
    while ((Get-Date) -lt $deadline) {
        $script:menu = [Win]::Find([uint32]$childPid, '#32768')
        if ($script:menu -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 100
    }
    if ($script:menu -eq [IntPtr]::Zero) {
        throw "FAIL: no #32768 popup owned by pid $childPid in ${MenuWaitMs}ms"
    }

    $box = New-Object Win+RECT
    [void][Win]::GetWindowRect($script:menu, [ref]$box)

    # The item text cannot be pulled out of another process's #32768: UIAutomation,
    # MSAA and MN_GETHMENU all answer empty on this OS (measured 2026-09-02). The
    # pixels can. So the picture is the reading and the log lines below are only
    # what the target claims about itself - keep them, do not trust them alone.
    if ($Shot) {
        $shotArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                      (Join-Path $PSScriptRoot 'm24_shot.ps1'),
                      '-Left', $box.Left, '-Top', $box.Top,
                      '-Width', ($box.Right - $box.Left), '-Height', ($box.Bottom - $box.Top),
                      '-Out', $Shot)
        $shotOut = & powershell.exe @shotArgs 2>&1
        if ($LASTEXITCODE -ne 0) { throw "FAIL: screenshot exited $LASTEXITCODE :: $shotOut" }
        "  | $(Fold ($shotOut -join ' / '))"
    }

    $menu_lines = @()
    $deadline = (Get-Date).AddMilliseconds(3000)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path -LiteralPath $log) {
            $menu_lines = @(Get-Content -LiteralPath $log |
                Where-Object { $_ -match 'Tray menu' })
        }
        if ($menu_lines.Count -ge 2) { break }
        Start-Sleep -Milliseconds 100
    }

    $lines = @()
    $lines += "case=$Case pid=$childPid tray_hwnd=$([long]$tray)"
    $lines += "service_state_at_start=$svcAtStart"
    $lines += "exe=$exe"
    $lines += "popup=$($box.Right - $box.Left)x$($box.Bottom - $box.Top) at $($box.Left),$($box.Top)"
    $lines += "anchor=$AnchorX,$AnchorY"
    $lines += "shot=$Shot"
    foreach ($n in $notices) { $lines += "save-notice: $n" }
    foreach ($w in $script:noticeWarn) { $lines += "notice-warning: $w" }
    if (Test-Path -LiteralPath $log) {
        foreach ($l in (Get-Content -LiteralPath $log)) {
            if ($l -match 'Elevation:|running limited|running elevated|Config file:|Tray menu') {
                $lines += "log: $l"
            }
        }
    } else {
        $lines += "log: MISSING at $log"
    }

    $built = @($menu_lines | Where-Object { $_ -match 'Tray menu built' }) | Select-Object -Last 1
    $wording = @($menu_lines | Where-Object { $_ -match 'item wording' }) | Select-Object -Last 1
    $want = if ($Case -eq 'elevated') { 'wording: plain' } else { 'wording: needs-administrator' }
    $ok_wording = [bool]($wording -and $wording.Contains($want))
    $ok_present = [bool]($built -and $built.Contains('service item present'))
    $lines += "ASSERT wording '$want' -> $ok_wording"
    $lines += "ASSERT service item present -> $ok_present"

    # ---- the human half -------------------------------------------------------
    # The consent box is drawn on the secure desktop: no other process can read
    # it, photograph it or press it, and that refusal is exactly what F5 claims.
    # So the verdict has to come from what is observable from here - the service's
    # own state as the SCM reports it, and whatever notice box the instance puts
    # up on the normal desktop afterwards.
    if ($Click) {
        $logAt = (Get-Item -LiteralPath $log).Length
        $svc_before = Get-AgentServiceState
        $lines += "click=$Click service_before=$svc_before log_bytes=$logAt"
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $svc_after = $svc_before
        $noticed = $false
        while ($sw.ElapsedMilliseconds -lt $ClickWaitMs) {
            $box = [Win]::Find([uint32]$childPid, '#32770')
            if ($box -ne [IntPtr]::Zero -and -not $touched.ContainsKey([long]$box)) {
                $touched[[long]$box] = $true
                $b = New-Object Win+RECT
                [void][Win]::GetWindowRect($box, [ref]$b)
                $noticeShot = Join-Path $work ("notice-$Click.png")
                $shotArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                              (Join-Path $PSScriptRoot 'm24_shot.ps1'),
                              '-Left', $b.Left, '-Top', $b.Top,
                              '-Width', ($b.Right - $b.Left), '-Height', ($b.Bottom - $b.Top),
                              '-Out', $noticeShot)
                $shotOut = & powershell.exe @shotArgs 2>&1
                $lines += "notice box=$([long]$box) $($b.Right - $b.Left)x$($b.Bottom - $b.Top) " +
                          "at $($b.Left),$($b.Top) after=$($sw.ElapsedMilliseconds)ms " +
                          "shot=$noticeShot exit=$LASTEXITCODE"
                # The box's text is CJK, so it lands folded: the picture is the
                # reading, this line only says a box was there and how it was
                # built - one button, which is the only thing safe to click.
                $statics = @([Win]::ChildLines([uint32]$childPid, $box, 'Static'))
                $lines += "notice static: $(Fold ($statics -join ' / '))"
                $btns = [Win]::ChildHandles([uint32]$childPid, $box, 'Button')
                if ($btns.Count -eq 1) {
                    [void][Win]::PostMessage($btns[0], $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
                } else {
                    $lines += "notice-warning: $($btns.Count) buttons, left alone"
                }
                $noticed = $true
            }
            $svc_after = Get-AgentServiceState
            if ($noticed -or ($svc_after -ne $svc_before)) { break }
            Start-Sleep -Milliseconds 200
        }
        $lines += "waited $($sw.ElapsedMilliseconds)ms for the click, service_after=$svc_after noticed=$noticed"
        # Two things settle later than the loop lets go: the instance writes its
        # verdict line up to a second after the SCM moves (it polls at 200ms
        # itself), and the SCM reports START_PENDING before it reports RUNNING. So
        # the expected line is waited for, and the state is re-read afterwards.
        $expect = if ($Click -eq 'decline') { 'declined at the consent prompt' } else { 'background service is running' }
        $sw2 = [Diagnostics.Stopwatch]::StartNew()
        $fresh = ''
        while ($sw2.ElapsedMilliseconds -lt 10000) {
            $fresh = (Tail-From $log $logAt) -join "`n"
            $svc_after = Get-AgentServiceState
            if (($fresh -match $expect) -and ($svc_after -eq 'RUNNING' -or $svc_after -eq 'STOPPED')) { break }
            Start-Sleep -Milliseconds 200
        }
        $lines += "waited a further $($sw2.ElapsedMilliseconds)ms, service_after=$svc_after"
        foreach ($l in (Tail-From $log $logAt)) { $lines += "new-log: $(Fold $l)" }
        if ($Click -eq 'decline') {
            $ok_log = ($fresh -match 'declined at the consent prompt')
            $ok_state = ($svc_after -eq $svc_before)
            $lines += "ASSERT the instance logged the refusal -> $ok_log"
            $lines += "ASSERT the service is still $svc_before -> $ok_state"
        } else {
            $ok_log = ($fresh -match 'background service is running')
            $ok_state = ($svc_after -eq 'RUNNING')
            $lines += "ASSERT the instance logged the service coming up -> $ok_log"
            $lines += "ASSERT the SCM now says RUNNING -> $ok_state"
        }
        if (-not ($ok_log -and $ok_state)) {
            $script:clickFailed = $true
        }
    }
    # ---- end of the human half ------------------------------------------------

    Close-Menu $script:menu
    $script:menu = [IntPtr]::Zero

    Finish $lines

    if (-not ($ok_wording -and $ok_present)) {
        throw "FAIL: $Case-token menu did not read as expected (wording=$ok_wording present=$ok_present)"
    }
    if ($script:clickFailed) {
        throw "FAIL: the menu item was there but did not do what it says (click=$Click)"
    }
    if (-not $KeepAlive) {
        Stop-Process -Id $childPid -Force -ErrorAction SilentlyContinue
        $script:proc = $null
    } else {
        "keepalive pid=$childPid"
    }
} catch {
    Close-Menu $script:menu
    if ($script:proc -and -not $script:proc.HasExited) {
        Stop-Process -Id $script:proc.Id -Force -ErrorAction SilentlyContinue
    }
    throw
}
