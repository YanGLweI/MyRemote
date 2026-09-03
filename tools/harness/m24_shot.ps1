# Capture a screen rectangle at true physical pixels.
#
# This exists because window rectangles reported by a DPI-unaware process (like
# powershell.exe, which is what runs the probes) are virtualised, while the
# screenshot is taken after this script calls SetProcessDPIAware. Multiply one by
# the other wrong and you crop a region with nothing to do with the window you
# measured - the image still comes back full of plausible pixels, so the failure
# is silent. An earlier draft hardcoded 2 because the panel once sat at 200%;
# this machine is at 144/96 and the crop has been landing 33% off ever since.
#
# The multiplier is therefore measured per run: the logical desktop width read
# BEFORE the process declares itself aware, divided into the physical width read
# AFTER. Single-monitor assumption stated, not silently made.
#
#   powershell -File tools/harness/m24_shot.ps1 -Left 538 -Top 1038 -Width 417 -Height 114 -Out build\scratch\m24_menu\shot.png
param(
    [int] $Left = 0,
    [int] $Top = 0,
    [int] $Width = 0,
    [int] $Height = 0,
    [Parameter(Mandatory)][string] $Out,
    # 0 = derive it. Only set it to argue with the derivation.
    [double] $Scale = 0,
    [switch] $Full
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type -Language CSharp @'
using System;
using System.Runtime.InteropServices;
public static class Dpi {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool IsProcessDPIAware();
    [DllImport("user32.dll")] public static extern uint GetDpiForSystem();
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
}
'@

# ...aware, so this is the virtualised size the probes' coordinates live in.
$logicalW = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds.Width
$monCount = [System.Windows.Forms.Screen]::AllScreens.Count
[void][Dpi]::SetProcessDPIAware()
# ...and this is the panel's real pixel width.
$physW = [Dpi]::GetSystemMetrics(0)
$derived = if ($logicalW -gt 0) { $physW / [double]$logicalW } else { [Dpi]::GetDpiForSystem() / 96.0 }
$s = if ($Scale -gt 0) { $Scale } else { $derived }
if ($monCount -gt 1) { "NOTE $monCount monitors - one system scale applied to all" }
$scaleNote = "scale $s (phys=$physW logical=$logicalW sysdpi=$([Dpi]::GetDpiForSystem()) explicit=$($Scale -gt 0))"

if ($Full) {
    $vs = [System.Windows.Forms.SystemInformation]::VirtualScreen
    "virtual screen (physical): $($vs.Width)x$($vs.Height) at $($vs.Left),$($vs.Top)"
    $bmp = New-Object Drawing.Bitmap($vs.Width, $vs.Height)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($vs.Left, $vs.Top, 0, 0, (New-Object Drawing.Size($vs.Width, $vs.Height)))
    $g.Dispose()
    $dir = Split-Path -Parent $Out
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $bmp.Save($Out, [Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    "saved full screen $Out"
    return
}

$w = [int]($Width * $s)
$h = [int]($Height * $s)
$bmp = New-Object Drawing.Bitmap($w, $h)
$g = [Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen([int]($Left * $s), [int]($Top * $s), 0, 0, (New-Object Drawing.Size($w, $h)))
$g.Dispose()
$dir = Split-Path -Parent $Out
if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
$bmp.Save($Out, [Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
"saved $Out at $($w)x$($h) - physical $([int]($Left * $s)),$([int]($Top * $s)) - $scaleNote"
