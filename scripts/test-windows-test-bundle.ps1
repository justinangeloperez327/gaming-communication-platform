# Offline deterministic package checks; synthetic files prove integrity logic only.
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ("gcp-package-test-" + [Guid]::NewGuid().ToString('N'))
function Write-Manifest($entries) {
    $records = @($entries | Where-Object { $_.path -cne 'GCP-PAYLOAD-PATHS.txt' })
    $list = Join-Path $root 'GCP-PAYLOAD-PATHS.txt'
    [IO.File]::WriteAllLines($list, @((@($records.path) + @('GCP-PAYLOAD-PATHS.txt','SHA256-MANIFEST.json')) | Sort-Object -Unique), [Text.UTF8Encoding]::new($false))
    $records += [pscustomobject]@{ path = 'GCP-PAYLOAD-PATHS.txt'; bytes = (Get-Item -LiteralPath $list).Length
        sha256 = (Get-FileHash -LiteralPath $list -Algorithm SHA256).Hash.ToLowerInvariant() }
    ConvertTo-Json -InputObject @($records) -Depth 4 | Set-Content (Join-Path $root 'SHA256-MANIFEST.json') -Encoding utf8
}
function Expect-Rejected([string]$case) {
    $failed = $false
    try { & (Join-Path $PSScriptRoot 'verify-windows-test-bundle.ps1') -BundleDirectory $root }
    catch { $failed = $true }
    if (-not $failed) { throw "Invalid bundle accepted: $case" }
}
try {
    New-Item -Path $root -ItemType Directory | Out-Null
    $manifest = @()
    foreach ($name in @('gcp-desktop.exe','gcp-livekit-voice.exe','gcp-livekit-screen.exe','livekit.dll','livekit_ffi.dll','Qt6WebSockets.dll','qml/nested/module.dll')) {
        $path = Join-Path $root $name
        New-Item -Path (Split-Path $path) -ItemType Directory -Force | Out-Null
        [IO.File]::WriteAllText($path, "synthetic fixture: $name")
        $manifest += [pscustomobject]@{
            path = $name
            sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
            bytes = (Get-Item $path).Length
        }
    }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../third_party/notices') -Destination (Join-Path $root 'third-party') -Recurse
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $root 'third-party') -Recurse -File) {
        $manifest += [pscustomobject]@{
            path = [IO.Path]::GetRelativePath($root, $file.FullName).Replace('\','/')
            sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            bytes = $file.Length
        }
    }
    Write-Manifest $manifest
    & (Join-Path $PSScriptRoot 'verify-windows-test-bundle.ps1') -BundleDirectory $root
    # Hidden files must survive ZIP creation, not be silently dropped by Compress-Archive.
    $hidden = Join-Path $root '.hidden-required.txt'
    [IO.File]::WriteAllText($hidden, 'hidden fixture')
    (Get-Item -LiteralPath $hidden -Force).Attributes = [IO.FileAttributes]::Hidden
    $hiddenEntry = [pscustomobject]@{ path = '.hidden-required.txt'; bytes = (Get-Item -LiteralPath $hidden -Force).Length
        sha256 = (Get-FileHash -LiteralPath $hidden -Algorithm SHA256).Hash.ToLowerInvariant() }
    Write-Manifest ($manifest + $hiddenEntry)
    & (Join-Path $PSScriptRoot 'package-private-windows-zip.ps1') -BundleDirectory $root -OutputZip ($root + '.zip')
    Expand-Archive -LiteralPath ($root + '.zip') -DestinationPath ($root + '-extracted')
    & (Join-Path $PSScriptRoot 'verify-windows-test-bundle.ps1') -BundleDirectory ($root + '-extracted')
    if (-not (Test-Path -LiteralPath (Join-Path ($root + '-extracted') '.hidden-required.txt'))) { throw 'ZIP omitted a hidden manifest file' }
    Remove-Item -LiteralPath $hidden -Force
    Write-Manifest $manifest
    # Regenerate the general manifest after removing a required notice. The
    # pinned baseline, not a self-consistent payload inventory, must reject it.
    foreach ($name in @('third-party/livekit/NOTICE', 'third-party/README.md')) {
        $path = Join-Path $root $name
        $saved = [IO.File]::ReadAllBytes($path)
        Remove-Item -LiteralPath $path
        Write-Manifest ($manifest | Where-Object { $_.path -ne $name })
        Expect-Rejected 'notice removed with regenerated payload manifest'
        [IO.File]::WriteAllBytes($path, $saved)
    }
    $name = 'third-party/livekit/NOTICE'
    $path = Join-Path $root $name
    $saved = [IO.File]::ReadAllBytes($path)
    $entry = $manifest | Where-Object { $_.path -eq $name }
    $savedHash = $entry.sha256
    $savedLength = $entry.bytes
    [IO.File]::WriteAllBytes($path, [byte[]]@())
    $entry.bytes = 0
    $entry.sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Manifest $manifest
    Expect-Rejected 'empty notice with regenerated payload manifest'
    [IO.File]::WriteAllBytes($path, $saved)
    $entry.bytes = $savedLength
    $entry.sha256 = $savedHash

    $name = 'third-party/catalog.json'
    $path = Join-Path $root $name
    $saved = [IO.File]::ReadAllBytes($path)
    $entry = $manifest | Where-Object { $_.path -eq $name }
    $savedHash = $entry.sha256
    $savedLength = $entry.bytes
    [IO.File]::WriteAllText($path, '{}')
    $entry.bytes = (Get-Item -LiteralPath $path).Length
    $entry.sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Manifest $manifest
    Expect-Rejected 'changed notice catalog with regenerated payload manifest'
    [IO.File]::WriteAllBytes($path, $saved)
    $entry.bytes = $savedLength
    $entry.sha256 = $savedHash
    Write-Manifest $manifest
    & (Join-Path $PSScriptRoot 'verify-windows-test-bundle.ps1') -BundleDirectory $root

    Write-Manifest ($manifest | Where-Object { $_.path -ne 'Qt6WebSockets.dll' })
    Expect-Rejected 'missing WebSockets runtime with regenerated manifest'

    Write-Manifest ($manifest | Where-Object { $_.path -ne 'gcp-livekit-screen.exe' })
    Expect-Rejected 'missing screen publisher with regenerated manifest'

    Write-Manifest ($manifest + $manifest[0])
    Expect-Rejected 'duplicate path'
    Write-Manifest ($manifest | Select-Object -Skip 1)
    Expect-Rejected 'missing required executable'
    $original = $manifest[0].path
    foreach ($unsafe in @('../escape.dll','C:\escape.dll','qml/../../escape.dll','*.dll','qml/./module.dll','qml//module.dll','bad./module.dll')) {
        $manifest[0].path = $unsafe
        Write-Manifest $manifest
        Expect-Rejected 'unsafe path'
    }
    $manifest[0].path = $original
    Write-Manifest $manifest
    $extra = Join-Path $root '.unexpected.dll'
    [IO.File]::WriteAllText($extra, 'unmanifested')
    Expect-Rejected 'unmanifested hidden file'
    Remove-Item -LiteralPath $extra
    # Windows junction creation does not require symlink administrator privileges.
    $link = Join-Path $root 'linked-qml'
    New-Item -ItemType Junction -Path $link -Target (Join-Path $root 'qml') | Out-Null
    Expect-Rejected 'directory junction'
    [IO.Directory]::Delete($link)
    [IO.File]::AppendAllText((Join-Path $root 'livekit.dll'), 'tamper')
    Expect-Rejected 'tampered payload'
    Write-Host 'Synthetic integrity rejection fixtures passed; no runtime qualification.'
} finally {
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
    Remove-Item -LiteralPath ($root + '.zip'), ($root + '-extracted') -Recurse -Force -ErrorAction SilentlyContinue
}
