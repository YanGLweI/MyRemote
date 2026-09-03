# M24-2 leg: make OpenService genuinely fail and read what the tool says.
#
# F1's tail claim is that a caller who cannot open the service still gets the
# whole report - the first line names ACCESS_DENIED as the cause, and `console
# session:`, `stations:` and `host:` are printed anyway. That cannot be reached by
# asking nicely; the service's DACL has to actually refuse.
#
# This runs elevated (it edits a service DACL). It does NOT run the CLI itself:
# the CLI has to fail as a *limited* token, and a child of an elevated process is
# elevated. So the two halves rendezvous through files:
#
#   here   : save sdshow -> drop the interactive-user ACE -> write reduced.txt
#   caller : runs agent.exe --service-state unelevated, saves cli-state.txt,
#            writes cli-done
#   here   : restore the original descriptor, re-read it and byte-compare
#
# The restore is in a finally and the wait is bounded, so a crashed caller cannot
# leave this machine with a service nobody can query.
param(
    [string] $Repo = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)),
    [string] $Service = 'MyRemoteAgent',
    [int] $WaitSeconds = 240
)

$ErrorActionPreference = 'Stop'
$work = Join-Path $Repo 'build\scratch\m24_f1'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$reduced = Join-Path $work 'reduced.txt'
$done = Join-Path $work 'cli-done'
$cliOut = Join-Path $work 'cli-state.txt'
$origFile = Join-Path $work 'sdshow-original.txt'
$result = Join-Path $work 'f1-acl.txt'
foreach ($f in @($reduced, $done, $cliOut, $result)) {
    if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -Force }
}

$report = @()
$failures = @()
$original = $null

function Get-Sd {
    $out = & sc.exe sdshow $Service 2>&1 | Out-String
    return ($out -split "`r?`n" | Where-Object { $_ -match '^D:' } | Select-Object -First 1)
}

try {
    $original = Get-Sd
    if (-not $original) { throw "FAIL: sdshow returned no descriptor: $original" }
    $original | Set-Content -LiteralPath $origFile -Encoding ASCII
    $report += "original: $original"

    # Drop the ACE that grants the interactive user, whatever rights it happens to
    # carry. Editing the right bits by hand would be a guess about SDDL service
    # aliases; removing the whole grant is not.
    $reducedSd = ($original -replace '\(A;[^;]*;[^;]*;;;IU\)', '')
    if ($reducedSd -eq $original) {
        throw "FAIL: no (…;;;IU) ACE found, so this leg cannot make OpenService refuse"
    }
    $applied = & sc.exe sdset $Service $reducedSd 2>&1 | Out-String
    $report += "sdset reduced: $($applied.Trim())"
    if ($applied -notmatch '120|SUCCESS') {
        # 0 = success; 120 = "service paused/pending" is fine too. Anything else
        # and the DACL never changed, so the leg would prove nothing.
        throw "FAIL: sc sdset did not take: $applied"
    }
    $report += "reduced: $reducedSd"
    'reduced; run the limited CLI now' | Set-Content -LiteralPath $reduced -Encoding ASCII

    $deadline = (Get-Date).AddSeconds($WaitSeconds)
    while ((Get-Date) -lt $deadline -and -not (Test-Path -LiteralPath $done)) {
        Start-Sleep -Milliseconds 200
    }
    if (-not (Test-Path -LiteralPath $done)) {
        $failures += "caller never produced cli-done within ${WaitSeconds}s"
    } elseif (Test-Path -LiteralPath $cliOut) {
        foreach ($l in (Get-Content -LiteralPath $cliOut)) { $report += "cli: $l" }
        $text = Get-Content -LiteralPath $cliOut -Raw
        $ok_denied = ($text -match 'ACCESS_DENIED')
        $ok_rest = ($text -match 'console session:' -and $text -match 'stations:' -and
                     $text -match 'host:')
        $report += "ASSERT the refusal names ACCESS_DENIED -> $ok_denied"
        $report += "ASSERT the rest of the report is still printed -> $ok_rest"
        if (-not $ok_denied) { $failures += 'no ACCESS_DENIED in the limited output' }
        if (-not $ok_rest) { $failures += 'the report stopped at the refusal' }
    } else {
        $failures += "cli-done present but cli-state.txt missing"
    }
}
catch {
    $failures += $_.Exception.Message
    $report += "ERROR: $($_.Exception.Message)"
}
finally {
    if ($original) {
        $back = & sc.exe sdset $Service $original 2>&1 | Out-String
        $report += "restored: $($back.Trim())"
        Start-Sleep -Milliseconds 300
        $now = Get-Sd
        $same = ($now -eq $original)
        $report += "ASSERT the descriptor is back byte-for-byte -> $same"
        if (-not $same) {
            $report += "  now: $now"
            $failures += 'the service DACL did not come back identical'
        }
    } else {
        $report += "restore skipped - nothing was ever changed"
    }
    [IO.File]::WriteAllText($result, ($report -join "`r`n") + "`r`n",
        (New-Object Text.UTF8Encoding($true)))
    foreach ($l in $report) { "  | $l" }
    "RESULT=$result"
    if ($failures.Count) {
        foreach ($f in $failures) { "FAIL: $f" }
        throw "F1 ACL leg did not pass ($($failures.Count) failure(s))"
    }
}
