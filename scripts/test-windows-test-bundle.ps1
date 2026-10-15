# Offline deterministic package integrity checks; no Qt installation required.
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ("gcp-package-test-" + [Guid]::NewGuid().ToString('N'))
try {
    New-Item -Path $root -ItemType Directory | Out-Null
    $names = @('gcp-desktop.exe','gcp-livekit-voice.exe','livekit.dll','livekit_ffi.dll')
    $manifest = @()
    foreach ($name in $names) {
        $path = Join-Path $root $name
        [IO.File]::WriteAllText($path, "synthetic fixture: $name")
        $manifest += [pscustomobject]@{
            path = $name
            sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
            bytes = (Get-Item $path).Length
        }
    }
    $manifest | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $root 'SHA256-MANIFEST.json') -Encoding utf8
    & (Join-Path $PSScriptRoot 'verify-windows-test-bundle.ps1') -BundleDirectory $root
    [IO.File]::AppendAllText((Join-Path $root 'livekit.dll'), 'tamper')
    $failed = $false
    try {
        & (Join-Path $PSScriptRoot 'verify-windows-test-bundle.ps1') -BundleDirectory $root
    } catch { $failed = $true }
    if (-not $failed) { throw 'Tampered package unexpectedly accepted' }
    Write-Host 'Synthetic integrity test passed (tampered bundle rejected).'
} finally {
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}
