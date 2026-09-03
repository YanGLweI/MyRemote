# M24-5 leg 3, live half.
#
# Legs 1 and 2 read a probe copy's menu with the service item present. This reads
# the menu of the tray proxy that is actually on screen right now - the one the
# installed service's session host spawned - which by construction has no service
# item at all. Its rect is the "item absent" number, taken from a real desktop
# rather than from what the code says it would draw.
#
# Nothing here writes: the popup is opened, measured and closed.
param(
    [int] $ProxyPid = 0,
    [string] $Repo = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)),
    [int] $MenuWaitMs = 4000,
    # The menu is tracked at the real cursor position (tray_icon.cpp:350 does
    # GetCursorPos then TrackPopupMenuEx at that point), and Windows clamps a
    # popup to the work area - so an unpinned cursor gives a different rect for
    # the same menu, and a rect near an edge reports a clipped height. Every
    # geometry reading here parks the cursor on one anchor first.
    [int] $AnchorX = 1200,
    [int] $AnchorY = 400,
    [string] $Shot = ''
)

$ErrorActionPreference = 'Stop'
$work = Join-Path $Repo 'build\scratch\m24_menu'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$result = Join-Path $work 'menu-live.txt'
if (Test-Path -LiteralPath $result) { Remove-Item -LiteralPath $result -Force }

Add-Type -Language CSharp @'
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class WinLive {
    public delegate bool EnumWindowsProc(IntPtr h, IntPtr lp);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);

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
}
'@

function Fold([string] $s) {
    -join ($s.ToCharArray() | ForEach-Object {
        if ([int]$_ -lt 32 -or [int]$_ -ge 127) { '?' } else { $_ }
    })
}

function Close-MenuLive([IntPtr] $menu) {
    if ($menu -eq [IntPtr]::Zero) { return }
    [void][WinLive]::PostMessage($menu, $script:WM_KEYDOWN, [IntPtr]$script:VK_ESCAPE, [IntPtr]::Zero)
    [void][WinLive]::PostMessage($menu, $script:WM_CANCELMODE, [IntPtr]::Zero, [IntPtr]::Zero)
}

function Finish([string[]] $lines) {
    [IO.File]::WriteAllText($result, ($lines -join "`r`n") + "`r`n",
        (New-Object Text.UTF8Encoding($true)))
    foreach ($l in $lines) { "  | $(Fold $l)" }
    "RESULT=$result"
}

$WM_APP = 0x8000
$WM_RBUTTONUP = 0x0205
$WM_KEYDOWN = 0x0100
$WM_CANCELMODE = 0x01F6
$VK_ESCAPE = 0x1B

if ($ProxyPid -le 0) {
    $found = @(Get-CimInstance Win32_Process -Filter "Name='agent.exe'" |
        Where-Object { $_.CommandLine -match '--tray-proxy' })
    if ($found.Count -ne 1) {
        throw "FAIL: expected exactly one --tray-proxy agent.exe, found $($found.Count)"
    }
    $ProxyPid = [int]$found[0].ProcessId
}

$proc = Get-Process -Id $ProxyPid
$exe = $proc.Path
$ver = $proc.MainModule.FileVersionInfo.FileVersion
$sits = $proc.SessionId

$script:menu = [IntPtr]::Zero
try {
    $tray = [WinLive]::Find([uint32]$ProxyPid, 'MyRemoteAgentTray')
    if ($tray -eq [IntPtr]::Zero) {
        throw "FAIL: pid $ProxyPid owns no MyRemoteAgentTray window"
    }

    [void][WinLive]::SetCursorPos($AnchorX, $AnchorY)
    Start-Sleep -Milliseconds 120
    [void][WinLive]::PostMessage($tray, ($WM_APP + 2), [IntPtr]::Zero, [IntPtr]$WM_RBUTTONUP)

    $deadline = (Get-Date).AddMilliseconds($MenuWaitMs)
    while ((Get-Date) -lt $deadline) {
        $script:menu = [WinLive]::Find([uint32]$ProxyPid, '#32768')
        if ($script:menu -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 100
    }
    if ($script:menu -eq [IntPtr]::Zero) {
        throw "FAIL: no #32768 popup owned by pid $ProxyPid in ${MenuWaitMs}ms"
    }

    $box = New-Object WinLive+RECT
    $deadline = (Get-Date).AddMilliseconds(2000)
    do {
        [void][WinLive]::GetWindowRect($script:menu, [ref]$box)
        Start-Sleep -Milliseconds 50
    } while ((($box.Right - $box.Left) -le 0 -or ($box.Bottom - $box.Top) -le 0) -and
             (Get-Date) -lt $deadline)

    $w = $box.Right - $box.Left
    $h = $box.Bottom - $box.Top

    # Everything about the popup has to be read while it exists. GetDpiForWindow
    # on a destroyed hwnd answers 0, which reads like the OS refusing rather
    # than like the harness asking too late.
    $menuDpi = [WinLive]::GetDpiForWindow($script:menu)
    if ($Shot) {
        # Out of process on purpose: m24_shot.ps1 declares itself DPI-aware, and
        # that must not happen to the process holding the coordinates.
        $shotArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                      (Join-Path $PSScriptRoot 'm24_shot.ps1'),
                      '-Left', $box.Left, '-Top', $box.Top,
                      '-Width', $w, '-Height', $h, '-Out', $Shot)
        $shotOut = & powershell.exe @shotArgs 2>&1
        if ($LASTEXITCODE -ne 0) { throw "FAIL: screenshot exited $LASTEXITCODE :: $shotOut" }
        "  | $(Fold ($shotOut -join ' / '))"
    }

    Close-MenuLive $script:menu
    $deadline = (Get-Date).AddMilliseconds(2000)
    while ((Get-Date) -lt $deadline -and [WinLive]::IsWindow($script:menu)) {
        Start-Sleep -Milliseconds 50
    }
    $closed = -not [WinLive]::IsWindow($script:menu)

    $dpi = (Get-ItemProperty 'HKCU:\Control Panel\Desktop\WindowMetrics' `
                -Name AppliedDPI -ErrorAction SilentlyContinue).AppliedDPI
    $lines = @(
        "pid=$ProxyPid tray_hwnd=$([long]$tray)",
        "exe=$exe FileVersion=$ver",
        "session=$sits",
        "anchor=$AnchorX,$AnchorY",
        "menu_dpi=$menuDpi",
        "applied_dpi=$dpi",
        "popup=${w}x${h} at $($box.Left),$($box.Top)",
        "ASSERT menu closed -> $closed"
    )
    Finish $lines
    if (-not $closed) { throw "FAIL: the popup was left open on the user's desktop" }
}
catch {
    Close-MenuLive $script:menu
    throw
}
