# The one admin window M24 still needs.
#
# Everything in here needs a token, and the person at the machine should only be
# asked to grant it once. Each step is a separate script so a failure names
# itself instead of taking the rest of the list down; each of those scripts puts
# the machine back in its own finally block.
#
#   powershell -File tools/harness/m24_admin_window.ps1     (elevated)
param(
    [string] $Repo = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)),
    [string] $Service = 'MyRemoteAgent',
    [string] $InstalledExe = 'C:\MyRemote\Agent\agent.exe'
)

$ErrorActionPreference = 'Continue'
$script:exits = @{}

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

# 1. The rebuilt 1.0.7 goes into the path the service points at. The service is
#    stopped at this point, which is the only moment the file can be replaced.
if ((State $Service) -ne 'STOPPED') {
    "SKIP: service is $(State $Service), not STOPPED - refusing to overwrite a running exe"
    exit 2
}
$src = Join-Path $Repo 'build\bin\Release\agent.exe'
Copy-Item -LiteralPath $src -Destination $InstalledExe -Force
"copied $src -> $InstalledExe sha=$((Get-FileHash -LiteralPath $InstalledExe -Algorithm SHA256).Hash.Substring(0,16))"
& sc.exe start $Service | Out-Null
Start-Sleep -Milliseconds 800
"service after start: $(State $Service)"

# 2. Both F4 legs. Each one quits the agent on purpose and puts the service back.
Run-Step 'f4-healthy' 'm24_f4.ps1' @('-Repo', $Repo, '-Case', 'healthy')
Run-Step 'f4-hung'    'm24_f4.ps1' @('-Repo', $Repo, '-Case', 'hung')

# 3. F1's tail: this step waits for a limited-token CLI run started from the
#    other side, then restores the descriptor whatever happens.
Run-Step 'f1-acl' 'm24_f1_acl.ps1' @('-Repo', $Repo, '-Service', $Service)

# 4. The installed image goes back, byte-for-byte.
Run-Step 'restore' 'm24_swap.ps1' @('-Repo', $Repo, '-Mode', 'restore')

"=== window summary ==="
foreach ($k in $script:exits.Keys) { "  $k exit=$($script:exits[$k])" }
"  service state: $(State $Service)"
"  installed FileVersion: $((Get-Item -LiteralPath $InstalledExe).VersionInfo.FileVersion)"
