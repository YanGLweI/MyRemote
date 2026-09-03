# Binary string probe: does an exe image contain a given string, in ASCII or UTF-16?
#
# Why this exists: half the "the fix is not in the package" false alarms on this
# project came from grepping an ASCII-only view of a binary whose user-visible
# text is L"..." UTF-16. So the probe always answers for both encodings.
#
# ASCII-only source, per the standing rule for harness scripts: non-ASCII probes
# are spelled as code points and built with -join, never as literals. ([char]+[char]
# is integer addition in PowerShell, not concatenation.)
#
#   powershell -File tools/harness/probe_binary.ps1 -Path build/bin/Release/agent.exe `
#       -Probe F5_suffix,F5_label,'bye unconfirmed' -Label NEW
param(
    [Parameter(Mandatory = $true)][string] $Path,
    # Comma-separated rather than [string[]]: `powershell -File` hands over one
    # argv element and binds it as a single-element array, so "-Probe a,b,c"
    # would silently search for the literal "a,b,c" and report everything as
    # missing. Repeating -Probe is rejected outright (ParameterAlreadyBound).
    [Parameter(Mandatory = $true)][string] $Probe,
    [string] $Label,
    # Dump mode: a "False" answer is only useful if you can see what is there
    # instead. Prints the code units from the first hit of an anchor probe.
    [string] $DumpFrom,
    [int] $DumpChars = 40
)

$ErrorActionPreference = 'Stop'

# Named code-point probes. Add to this table rather than passing raw chars around:
# a probe that reads "F5_suffix" in the output is auditable, one that prints
# mojibake is not.
$probes = @{
    # Check every spelling below against the source text before trusting a False.
    # This one originally omitted U+5458 and reported a string that was present
    # as missing - a code-point needle has no typos to catch you.
    'F5_suffix'  = @(0x9700, 0x7BA1, 0x7406, 0x5458, 0x6279, 0x51C6)  # xu-guan-li-yuan-pi-zhun (needs admin approval)
    'F5_label'   = @(0x542F, 0x7528, 0x540E, 0x53F0, 0x670D, 0x52A1)  # qi-yong-hou-tai-fu-wu (enable background service)
    'F5_cancel'  = @(0x5DF2, 0x53D6, 0x6D88)  # yi-qu-xiao (cancelled)
    'F5_admin'   = @(0x7BA1, 0x7406, 0x5458, 0x6743, 0x9650)  # guan-li-yuan-quan-xian (administrator permission)
    # ASCII needles that contain a space live here too: passing one through a
    # shell is how it gets word-split into two arguments and silently missed.
    'F4_warn'    = $null
    'F1_timeout' = $null
    'F1_denied'  = $null
    'F2_stale'   = $null
}
$ascii_probes = @{
    'F4_warn'    = 'bye unconfirmed'
    'F1_timeout' = 'last state'
    'F1_denied'  = 'ACCESS_DENIED'
    'F2_stale'   = 'service stopped'
}
foreach ($k in $ascii_probes.Keys) { $probes[$k] = [int[]] @($ascii_probes[$k].ToCharArray() | ForEach-Object { [int]$_ }) }

$names = @($Probe -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })

$full = (Resolve-Path $Path).Path
$bytes = [IO.File]::ReadAllBytes($full)
$ascii = [Text.Encoding]::ASCII.GetString($bytes)
# Both alignments. A UTF-16 literal that happens to start at an odd file offset
# is invisible to a single offset-0 decode, and "not found" from a probe that
# cannot see half the file is the exact false alarm this script exists to kill.
$uni0 = [Text.Encoding]::Unicode.GetString($bytes)
$uni1 = [Text.Encoding]::Unicode.GetString($bytes, 1, $bytes.Length - 1)

# Ordinal, not Contains: in Windows PowerShell 5.1 String.Contains compares with
# the current culture, which for CJK needles is not the question being asked.
function Test-Here([string[]] $haystacks, [string] $needle) {
    foreach ($h in $haystacks) {
        if ($h.IndexOf($needle, [StringComparison]::Ordinal) -ge 0) { return $true }
    }
    return $false
}

$info = (Get-Item $full).VersionInfo
"${Label}: $full"
"  ProductVersion = $($info.ProductVersion)  size = $($bytes.Length)  mtime = $((Get-Item $full).LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))"

foreach ($name in $names) {
    if ($probes.ContainsKey($name)) {
        $needle = -join ($probes[$name] | ForEach-Object { [char]$_ })
        # Print the code points, not the characters: the console encoding here is
        # not guaranteed, and a verdict nobody can read is not a verdict.
        $spell = ($probes[$name] | ForEach-Object { 'U+{0:X4}' -f $_ }) -join ' '
        "  probe $name [$spell] unicode=$(Test-Here @($uni0, $uni1) $needle) ascii=$($ascii.IndexOf($needle, [StringComparison]::Ordinal) -ge 0)"
    } else {
        "  probe $name [literal] unicode=$(Test-Here @($uni0, $uni1) $name) ascii=$($ascii.IndexOf($name, [StringComparison]::Ordinal) -ge 0)"
    }
}

if ($DumpFrom) {
    $anchor = $DumpFrom
    if ($probes.ContainsKey($DumpFrom)) {
        $anchor = -join ($probes[$DumpFrom] | ForEach-Object { [char]$_ })
    }
    # Every hit, not just the first: the two variants of the tray label share
    # their whole prefix, so the first match can be the one that proves nothing.
    $shown = 0
    foreach ($h in @($uni0, $uni1)) {
        $from = 0
        while ($shown -lt 6) {
            $i = $h.IndexOf($anchor, $from, [StringComparison]::Ordinal)
            if ($i -lt 0) { break }
            $take = [Math]::Min($DumpChars, $h.Length - $i)
            $region = $h.Substring($i, $take)
            $shown++
            "  dump $DumpFrom #$shown ($take code units):"
            "    " + ((($region.ToCharArray() | ForEach-Object { 'U+{0:X4}' -f [int]$_ })) -join ' ')
            $from = $i + $anchor.Length
        }
    }
    if ($shown -eq 0) { "  dump: anchor '$DumpFrom' not found - nothing to show" }
}
