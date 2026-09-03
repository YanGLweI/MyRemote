# Leg 7 invariant: M24 must not teach the agent to listen on a TCP port. It is an
# outbound-only client plus one named pipe, so "no agent process owns a listener"
# is the reading that shows nothing was added. ASCII-only, per the standing rule.
$ErrorActionPreference = 'Stop'
$listen = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue)
$agentPids = @(Get-CimInstance Win32_Process -Filter "Name='agent.exe'" |
    ForEach-Object { [int]$_.ProcessId })
"agent.exe pids = $($agentPids -join ', ')"
$hits = @($listen | Where-Object { $agentPids -contains [int]$_.OwningProcess })
if ($hits.Count -eq 0) {
    "agent TCP listeners = 0  (assertion holds)"
} else {
    foreach ($h in $hits) {
        "agent TCP listener pid=$($h.OwningProcess) $($h.LocalAddress):$($h.LocalPort)"
    }
    "ASSERTION FAILED: agent owns $( $hits.Count ) listener(s)"
}
# Named pipes owned by agent: the tray-proxy pipe is the one thing it does serve,
# so print it rather than pretend there is no server side at all.
$pipes = @(Get-ChildItem '\\.\pipe\' -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -like '*MyRemote*' } | ForEach-Object { $_.Name })
"agent named pipes = $($pipes -join ', ')"
"total system listeners = $($listen.Count)"
