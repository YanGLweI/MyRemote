# The other half of the F1 ACL leg - run this UN-elevated, in a normal window.
#
# m24_f1_acl.ps1 lowers the service DACL from an elevated window and has to
# restore it again; a child of that window would be elevated too, and the leg is
# about what a limited token sees. So the two halves meet through files:
#
#   elevated : lowers the DACL, writes reduced.txt, waits for cli-done
#   here     : watches for reduced.txt, runs the CLI, saves its output, writes
#              cli-done
#   elevated : restores the descriptor and byte-compares it
#
# Start it before the elevated window reaches step 3. It exits on its own if the
# DACL never drops.
#
#   powershell -File tools/harness/m24_f1_caller.ps1
param(
    [string] $Repo = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)),
    [string] $Exe = '',
    [int] $WaitSeconds = 300
)

$ErrorActionPreference = 'Continue'
$work = Join-Path $Repo 'build\scratch\m24_f1'
$reduced = Join-Path $work 'reduced.txt'
$done = Join-Path $work 'cli-done'
$cliOut = Join-Path $work 'cli-state.txt'
if (-not $Exe) { $Exe = Join-Path $Repo 'build\bin\Release\agent.exe' }

if (-not (Test-Path -LiteralPath $work)) {
    "FAIL: no rendezvous directory at $work - run the elevated leg first"
    exit 2
}
foreach ($f in @($done, $cliOut)) {
    if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -Force }
}
"waiting up to ${WaitSeconds}s for $reduced (pid=$PID, unelevated)"
$deadline = (Get-Date).AddSeconds($WaitSeconds)
while ((Get-Date) -lt $deadline -and -not (Test-Path -LiteralPath $reduced)) {
    Start-Sleep -Milliseconds 200
}
if (-not (Test-Path -LiteralPath $reduced)) {
    "FAIL: the DACL was never lowered within ${WaitSeconds}s - nothing to measure"
    exit 3
}
"reduced descriptor is in place; running $Exe --service-state as this token"
$out = & $Exe --service-state 2>&1 | Out-String
[System.IO.File]::WriteAllText($cliOut, $out, (New-Object Text.UTF8Encoding($true)))
foreach ($l in ($out -split "`r?`n")) { "  | $l" }
"DACLED=$(Get-Date -Format 'HH:mm:ss.fff')" | Set-Content -LiteralPath $done -Encoding ASCII
"saved $cliOut, wrote $done"
