# Deploy packages to D:\IT-share\MyRemote-v1.0.6\ and verify SHA256SUMS.txt

$target = "D:\IT-share\MyRemote-v1.0.6\"
$version = "1.0.6"

if (Test-Path $target) {
    Remove-Item $target -Recurse -Force | Out-Null
}
New-Item -ItemType Directory -Force -Path $target | Out-Null

$dist = Join-Path $PSScriptRoot "build\package\dist"

# Copy only the four packages + SHA256SUMS.txt for version $version
Copy-Item (Join-Path $dist "MyRemote-Server-v$version-setup.exe") `
          (Join-Path $target "MyRemote-Server-v$version-setup.exe") | Out-Null
Copy-Item (Join-Path $dist "MyRemote-Server-v$version-portable.zip") `
          (Join-Path $target "MyRemote-Server-v$version-portable.zip") | Out-Null
Copy-Item (Join-Path $dist "MyRemote-Agent-v$version-setup.exe") `
          (Join-Path $target "MyRemote-Agent-v$version-setup.exe") | Out-Null
Copy-Item (Join-Path $dist "MyRemote-Agent-v$version-portable.zip") `
          (Join-Path $target "MyRemote-Agent-v$version-portable.zip") | Out-Null
Copy-Item (Join-Path $dist "SHA256SUMS.txt") `
          (Join-Path $target "SHA256SUMS.txt") | Out-Null

Write-Host "Deployed to ${target}:"
Get-ChildItem -Path $target | Select-Object Name, @{Name="Size(KB)";Expression={[math]::Round($_.Length/1KB,2)}}

# Verify using PowerShell's Get-FileHash
Write-Host ""
Write-Host "Verifying SHA256SUMS.txt..."
$passed = $true
foreach ($line in (Get-Content (Join-Path $target "SHA256SUMS.txt"))) {
    if ([string]::IsNullOrWhiteSpace($line)) { continue }
    $parts = $line -split '  +'
    $hash = $parts[0]
    $name = $parts[1]
    $file = Join-Path $target $name
    if (-not (Test-Path $file)) {
        Write-Host "MISSING: $name" -ForegroundColor Red
        $passed = $false
        continue
    }
    $actual = (Get-FileHash $file -Algorithm SHA256).Hash.ToLower()
    if ($hash -eq $actual) {
        Write-Host "$name OK"
    } else {
        Write-Host "$name MISMATCH: got $actual expected $hash" -ForegroundColor Red
        $passed = $false
    }
}
Write-Host ""
if ($passed) {
    Write-Host "All four files verified OK!" -ForegroundColor Green
} else {
    throw "Verification failed!"
}
