# Every top-level window owned by an agent-ish process: class, pid, visibility, title.
#
# Two reasons this is a separate script rather than an inline one-liner: the
# EnumWindows callback has to live on the C# side (a PowerShell scriptblock
# marshalled as a function pointer gets collected while still in use), and
# `bash -> powershell -Command` eats the quoting on any of it.
#
#   powershell -File tools/harness/m24_windows.ps1                    # all agent windows
#   powershell -File tools/harness/m24_windows.ps1 -OwnerPid 27124    # one process
#   powershell -File tools/harness/m24_windows.ps1 -All               # every window, any exe
#
# Not "-Pid": $PID is a read-only automatic variable and binding a parameter
# over it fails before the script body runs.
param(
    [int] $OwnerPid = 0,
    [switch] $All
)

$ErrorActionPreference = 'Stop'

Add-Type -Language CSharp @'
using System;
using System.Text;
using System.Runtime.InteropServices;
using System.Collections.Generic;

public static class WindowList {
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] static extern int GetClassName(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern int GetWindowTextW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll")] static extern int GetWindowTextLength(IntPtr h);
    delegate bool EnumProc(IntPtr h, IntPtr l);

    // One C# callback, results collected in a list: returning a string per
    // window through PowerShell is what made the previous attempt unusable.
    public static List<string> Grab(uint onlyPid, bool everyWindow) {
        var outLines = new List<string>();
        EnumWindows((h, l) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (onlyPid != 0 && pid != onlyPid) return true;
            var cls = new StringBuilder(128);
            GetClassName(h, cls, cls.Capacity);
            string c = cls.ToString();
            bool interesting = everyWindow
                || c == "MyRemoteAgentTray" || c == "MyRemoteConfigWnd"
                || c == "#32768" || c == "CabinetWClass";
            if (!interesting) return true;
            var title = new StringBuilder(256);
            GetWindowTextW(h, title, title.Capacity);
            outLines.Add(string.Format("{0} pid={1} visible={2} hwnd={3} title=[{4}]",
                c, pid, IsWindowVisible(h), h.ToInt64(), title.ToString()));
            return true;
        }, IntPtr.Zero);
        return outLines;
    }
}
'@

$found = [WindowList]::Grab([uint32]$OwnerPid, [bool]$All)
if ($found.Count -eq 0) {
    if ($OwnerPid) { "no matching windows for pid $OwnerPid" } else { "no matching windows" }
}
foreach ($f in $found) { $f }
