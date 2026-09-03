# Throwaway experiment: a #32768 popup is on screen and UIA reports 0 children.
# Which reader actually sees the items?
#
#   powershell -File tools/harness/m24_menu_probe.ps1 -OwnerPid 24748 -TrayHwnd 6950268
param(
    [Parameter(Mandatory)][int] $OwnerPid,
    [Parameter(Mandatory)][long] $TrayHwnd,
    [int] $HoldMs = 1200,
    [string] $Shot = '',
    [string] $FullShot = ''
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes

Add-Type -Language CSharp @'
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class Probe {
    public delegate bool EnumWindowsProc(IntPtr h, IntPtr lp);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr menu);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetMenuStringW(IntPtr menu, uint item, StringBuilder s, int max, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);

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

    // MN_GETHMENU = 0x01B1. An earlier run of this probe passed 0x03B1, which is
    // no message any window answers, so its "-> 0" was the harness lying rather
    // than the OS refusing. The documented way to reach a popup menu's HMENU.
    public static string[] Items(IntPtr menuHwnd, int max) {
        IntPtr hMenu = SendMessageW(menuHwnd, 0x01B1, IntPtr.Zero, IntPtr.Zero);
        System.Collections.Generic.List<string> hits = new System.Collections.Generic.List<string>();
        if (hMenu == IntPtr.Zero) { hits.Add("MN_GETHMENU -> 0"); return hits.ToArray(); }
        int n = GetMenuItemCount(hMenu);
        hits.Add("hmenu=" + hMenu.ToInt64() + " count=" + n);
        for (int i = 0; i < n && i < max; i++) {
            StringBuilder sb = new StringBuilder(256);
            GetMenuStringW(hMenu, (uint)i, sb, sb.Capacity, 0x0400 /* MF_BYPOSITION */);
            hits.Add("[" + i + "] " + sb.ToString());
        }
        return hits.ToArray();
    }
    // Every window of a class for the pid, annotated. A popup menu window can
    // outlive its menu: if Find() returns the first match, it may be handing
    // back a dead one whose item list is legitimately empty.
    public static string[] DumpClass(uint targetPid, string className) {
        System.Collections.Generic.List<string> hits = new System.Collections.Generic.List<string>();
        EnumWindows((h, lp) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid != targetPid) return true;
            StringBuilder sb = new StringBuilder(64);
            GetClassName(h, sb, sb.Capacity);
            if (sb.ToString() != className) return true;
            RECT r; GetWindowRect(h, out r);
            hits.Add("hwnd=" + h.ToInt64() + " visible=" + IsWindowVisible(h) +
                     " rect=" + r.Left + "," + r.Top + "," + (r.Right - r.Left) +
                     "x" + (r.Bottom - r.Top));
            return true;
        }, IntPtr.Zero);
        return hits.ToArray();
    }
}
'@

function Show([string] $tag, $lines) {
    "${tag}: " + (($lines | ForEach-Object { "$_" }) -join ' ;; ')
}

[void][Probe]::PostMessage([IntPtr]$TrayHwnd, [uint32]0x8002, [IntPtr]::Zero, [IntPtr]0x0205)

$menu = [IntPtr]::Zero
$deadline = (Get-Date).AddMilliseconds(5000)
while ((Get-Date) -lt $deadline) {
    $menu = [Probe]::Find([uint32]$OwnerPid, '#32768')
    if ($menu -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 50
}
if ($menu -eq [IntPtr]::Zero) { "no menu for pid $OwnerPid"; exit 1 }
"menu hwnd=$([long]$menu)"; [Probe]::DumpClass([uint32]$OwnerPid, "#32768") | ForEach-Object { "  $_" }

if ($Shot) {
    # Read the rect before anything else can dismiss it, and hand it to the
    # physical-pixel screencap: if the menu is drawn full and the APIs say it is
    # empty, the readers are the problem, not the build.
    $r = New-Object Probe+RECT
    [void][Probe]::GetWindowRect($menu, [ref]$r)
    "shot rect: $($r.Left),$($r.Top) $($r.Right - $r.Left)x$($r.Bottom - $r.Top)"
    # Separate process: m24_shot.ps1 declares itself DPI-aware, which must not
    # happen to the process holding these coordinates.
    $shotArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                  (Join-Path $PSScriptRoot 'm24_shot.ps1'),
                  '-Left', $r.Left, '-Top', $r.Top,
                  '-Width', ($r.Right - $r.Left), '-Height', ($r.Bottom - $r.Top),
                  '-Out', $Shot)
    & powershell.exe @shotArgs
    if ($LASTEXITCODE -ne 0) { throw "screenshot exited $LASTEXITCODE" }
    if ($FullShot) {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
            (Join-Path $PSScriptRoot 'm24_shot.ps1') -Out $FullShot -Full
        if ($LASTEXITCODE -ne 0) { throw "full screenshot exited $LASTEXITCODE" }
    }
}

$root = [System.Windows.Automation.AutomationElement]::FromHandle($menu)
"uia root null=$($null -eq $root)"
if ($root) {
    "uia name='$($root.Current.Name)' type=$($root.Current.ControlType.ProgrammaticName) class='$($root.Current.ClassName)'"
    $kids = $root.FindAll([System.Windows.Automation.TreeScope]::Children,
                          [System.Windows.Automation.Condition]::TrueCondition)
    $desc = $root.FindAll([System.Windows.Automation.TreeScope]::Subtree,
                          [System.Windows.Automation.Condition]::TrueCondition)
    "uia children=$($kids.Count) subtree=$($desc.Count)"
    $walker = [System.Windows.Automation.TreeWalker]::RawViewWalker
    $c = $walker.GetFirstChild($root)
    $n = 0
    while ($c -ne $null -and $n -lt 20) { $n++; $c = $walker.GetNextSibling($c) }
    "uia rawwalk children=$n"
}

Start-Sleep -Milliseconds $HoldMs
"after ${HoldMs}ms:"
foreach ($l in [Probe]::Items($menu, 12)) { "  $l" }

# Third candidate: classic menus expose their items through MSAA as accessible
# children, not as child windows, so neither UIA-by-window nor GetMenuString on
# a window handle is guaranteed to reach them.
Add-Type -Language CSharp @'
using System;
using System.Runtime.InteropServices;

public static class Msaa {
    [DllImport("oleacc.dll")]
    public static extern int AccessibleObjectFromWindow(
        IntPtr hwnd, int id, ref Guid iid,
        [MarshalAs(UnmanagedType.IUnknown)] out object acc);

    public static object From(IntPtr hwnd, int id) {
        Guid iid = new Guid("618736E3-3C3D-11CF-810C-00AA00389B71");
        object acc;
        if (AccessibleObjectFromWindow(hwnd, id, ref iid, out acc) != 0) return null;
        return acc;
    }
}
'@
# Real MSAA ids: OBJID_WINDOW is 0, not -1 (-1 is OBJID_SYSMENU). A wrong id
# answers "no object here", which reads exactly like the OS refusing.
foreach ($spec in @(@('WINDOW', 0), @('CLIENT', -4), @('MENU', -3))) {
    $acc = [Msaa]::From($menu, [int]$spec[1])
    if (-not $acc) { "  msaa $($spec[0]): hr!=0" ; continue }
    try {
        $n = $acc.accChildCount
        $names = @()
        for ($i = 1; $i -le $n; $i++) { $names += "#$i='$($acc.accName($i))'" }
        "  msaa $($spec[0]): children=$n $($names -join ' ')"
    } catch { "  msaa $($spec[0]): late bind failed: $($_.Exception.Message)" }
}

[void][Probe]::PostMessage($menu, [uint32]0x0100, [IntPtr]0x1B, [IntPtr]::Zero)
[void][Probe]::PostMessage($menu, [uint32]0x01F6, [IntPtr]::Zero, [IntPtr]::Zero)
"menu closed"
