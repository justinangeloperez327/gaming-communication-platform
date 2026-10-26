# Verify unsigned desktop test-bundle files against its SHA-256 manifest.
param([Parameter(Mandatory=$true)][string]$BundleDirectory)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($BundleDirectory)
# Reject directory links as well as leaf links; a safe-looking relative path
# must not traverse a junction to files outside the bundle.
if (((Get-Item -LiteralPath $root -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'Bundle root links are not permitted'
}
foreach ($item in Get-ChildItem -LiteralPath $root -Recurse -Force) {
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'Bundle links are not permitted'
    }
}
$manifestPath = Join-Path $root 'SHA256-MANIFEST.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw 'Missing SHA-256 manifest' }
if ((Get-Item -LiteralPath $manifestPath).Length -gt 8388608) { throw 'Manifest exceeds 8 MiB' }
$raw = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$items = @($raw)
if ($items.Count -lt 4 -or $items.Count -gt 20000) { throw 'Unexpected manifest file count' }
$expected = @{}
foreach ($entry in $items) {
    $relative = [string]$entry.path
    if ([string]::IsNullOrWhiteSpace($relative) -or [IO.Path]::IsPathRooted($relative) -or
        $relative -match '(^|[\\/])\.\.?([\\/]|$)' -or $relative.Contains(':') -or $relative -match '[*?\[\]\x00-\x1f]' -or
        $relative.Contains('\') -or $relative -match '//|[ .](/|$)' -or $relative.EndsWith('/') -or
        $relative -ieq 'SHA256-MANIFEST.json') {
        throw 'Unsafe manifest path'
    }
    if ($entry.sha256 -cnotmatch '^[a-f0-9]{64}$' -or $entry.bytes -isnot [long] -and $entry.bytes -isnot [int] -or $entry.bytes -lt 0) {
        throw 'Invalid manifest hash or byte count'
    }
    $normalized = $relative.Replace('/', [IO.Path]::DirectorySeparatorChar)
    $path = [IO.Path]::GetFullPath((Join-Path $root $normalized))
    if (-not $path.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Manifest path escapes bundle'
    }
    if ($expected.ContainsKey($path)) { throw 'Duplicate manifest entry' }
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing file: $relative" }
    $file = Get-Item -LiteralPath $path
    if (($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Bundle links are not permitted' }
    if ($file.Length -ne [int64]$entry.bytes) { throw "File size mismatch: $relative" }
    $digest = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($digest -ne [string]$entry.sha256) { throw "Checksum mismatch: $relative" }
    $expected[$path] = $true
}
foreach ($name in @('gcp-desktop.exe','gcp-livekit-voice.exe','gcp-livekit-screen.exe','livekit.dll','livekit_ffi.dll','Qt6WebSockets.dll','GCP-PAYLOAD-PATHS.txt')) {
    if (-not $expected.ContainsKey((Join-Path $root $name))) { throw "Missing required package entry: $name" }
}
$pathsFile = Join-Path $root 'GCP-PAYLOAD-PATHS.txt'
if ((Get-Item -LiteralPath $pathsFile).Length -gt 2097152) { throw 'Payload path list exceeds 2 MiB' }
$listed = @(Get-Content -LiteralPath $pathsFile)
$actual = @($items | ForEach-Object { [string]$_.path }) + @('SHA256-MANIFEST.json')
if ($listed.Count -ne $actual.Count -or @(Compare-Object ($listed | Sort-Object -CaseSensitive) ($actual | Sort-Object -CaseSensitive) -CaseSensitive).Count -ne 0) {
    throw 'Installer payload path list does not match the bundle'
}
foreach ($file in Get-ChildItem -LiteralPath $root -Recurse -Force -File) {
    if ($file.FullName -eq $manifestPath) { continue }
    if (-not $expected.ContainsKey($file.FullName)) { throw "Unmanifested file: $($file.Name)" }
}
& (Join-Path $PSScriptRoot 'verify-windows-notices.ps1') -BundleDirectory $root
Write-Host "Package file sizes and SHA-256 hashes verified ($($expected.Count) files)."
