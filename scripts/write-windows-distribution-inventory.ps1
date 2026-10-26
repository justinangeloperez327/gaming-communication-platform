# Bind every shipped PE file to its actual build input; collect upstream evidence.
#requires -Version 7.0
param([Parameter(Mandatory=$true)][string]$BundleDirectory,
      [Parameter(Mandatory=$true)][string]$QtDirectory,
      [Parameter(Mandatory=$true)][string]$CrtDirectory,
      [Parameter(Mandatory=$true)][string]$LiveKitArchive,
      [Parameter(Mandatory=$true)][string]$CommitSha)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'windows-distribution.psm1') -Force
if ($CommitSha -cnotmatch '^[0-9a-f]{40}$') { throw 'Expected full source commit SHA' }
$root = [IO.Path]::GetFullPath($BundleDirectory)
$qt = [IO.Path]::GetFullPath($QtDirectory)
$crt = [IO.Path]::GetFullPath($CrtDirectory)
$catalog = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../third_party/notices/catalog.json') -Raw | ConvertFrom-Json
$sdkHash = (Get-FileHash -LiteralPath $LiveKitArchive -Algorithm SHA256).Hash.ToLowerInvariant()
if ($sdkHash -cne $catalog.livekit_windows_archive_sha256) { throw 'SDK release archive does not match the pinned digest' }
$archive = [IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($LiveKitArchive))
$evidence = [Collections.Generic.List[object]]::new()
$sdkHashes = @{}
function Get-StreamHash($Stream) {
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { return [Convert]::ToHexString($hasher.ComputeHash($Stream)).ToLowerInvariant() } finally { $hasher.Dispose() }
}
function Add-Evidence([string]$Source, [string]$Relative, [string]$Kind) {
    if ($Relative -match '(^|/)\.\.?(/|$)|[:\\\x00-\x1f]' -or $Relative.StartsWith('/')) { throw 'Unsafe attribution source path' }
    if ((Get-Item -LiteralPath $Source).Length -gt 20971520) { throw 'Attribution source exceeds 20 MiB' }
    $target = Join-Path $root ('third-party/build/' + $Kind + '/' + $Relative)
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
    [IO.File]::Copy($Source, $target, $false)
    $evidence.Add([ordered]@{ path = [IO.Path]::GetRelativePath($root, $target).Replace('\','/'); kind = $Kind
        bytes = (Get-Item -LiteralPath $target).Length; sha256 = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() })
}
try {
    foreach ($name in @('livekit.dll','livekit_ffi.dll')) {
        $matches = @($archive.Entries | Where-Object { $_.Name -ieq $name })
        if ($matches.Count -ne 1) { throw "SDK archive must contain exactly one $name" }
        $stream = $matches[0].Open()
        try { $sdkHashes[$name] = Get-StreamHash $stream } finally { $stream.Dispose() }
    }
    foreach ($entry in $archive.Entries | Where-Object { $_.Name -match '^(LICENSE|LICENCE|NOTICE|THIRD.PARTY.NOTICES)(\..*)?$' }) {
        $relative = $entry.FullName
        if ($relative -match '(^|/)\.\.?(/|$)|[:\\\x00-\x1f]' -or $relative.StartsWith('/') -or $entry.Length -gt 20971520) { throw 'Unsafe archive attribution entry' }
        $target = Join-Path $root ('third-party/build/livekit-archive/' + $relative)
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
        $inputStream = $entry.Open(); $outputStream = $null
        try { $outputStream = [IO.File]::Open($target, [IO.FileMode]::CreateNew); $inputStream.CopyTo($outputStream) }
        finally { if ($outputStream) { $outputStream.Dispose() }; $inputStream.Dispose() }
        $evidence.Add([ordered]@{ path = [IO.Path]::GetRelativePath($root,$target).Replace('\','/'); kind = 'livekit-archive'
            bytes = $entry.Length; sha256 = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() })
    }
} finally { $archive.Dispose() }

foreach ($folder in @('sbom','licenses')) {
    $source = Join-Path $qt $folder
    if (Test-Path -LiteralPath $source -PathType Container) {
        $files = @(Get-ChildItem -LiteralPath $source -Recurse -File | Where-Object { $_.Extension -in @('.json','.spdx','.txt','.md') })
        if ($files.Count -gt 1000) { throw 'Unexpected Qt attribution inventory size' }
        foreach ($file in $files) { Add-Evidence $file.FullName ([IO.Path]::GetRelativePath($source,$file.FullName).Replace('\','/')) ('qt-' + $folder) }
    }
}

$rows = [Collections.Generic.List[object]]::new()
foreach ($item in Get-ChildItem -LiteralPath $root -Recurse -Force -File | Where-Object { $_.Extension -in @('.exe','.dll') }) {
    $relative = [IO.Path]::GetRelativePath($root,$item.FullName).Replace('\','/')
    Assert-WindowsX64PE $item.FullName
    $digest = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $kind = ''; $origin = ''; $sourceDigest = ''
    if ($relative -cin @('gcp-desktop.exe','gcp-livekit-voice.exe','gcp-livekit-screen.exe')) {
        $kind = 'gcp-build'; $origin = $CommitSha; $sourceDigest = $digest
    } elseif ($sdkHashes.ContainsKey($relative)) {
        $kind = 'livekit-release-archive'; $origin = $catalog.livekit_sdk_version; $sourceDigest = $sdkHashes[$relative]
    } elseif (Test-Path -LiteralPath (Join-Path $crt $relative) -PathType Leaf) {
        $kind = 'microsoft-vc-redist'; $origin = Split-Path (Split-Path (Split-Path $crt -Parent) -Parent) -Leaf
        $sourceDigest = (Get-FileHash -LiteralPath (Join-Path $crt $relative) -Algorithm SHA256).Hash.ToLowerInvariant()
    } else {
        $candidates = @($relative, ('plugins/' + $relative))
        if (-not $relative.Contains('/')) { $candidates += 'bin/' + $relative }
        foreach ($candidate in $candidates) {
            $source = Join-Path $qt $candidate
            if ((Test-Path -LiteralPath $source -PathType Leaf) -and
                (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() -ceq $digest) {
                $kind = 'qt-installation'; $origin = $candidate; $sourceDigest = $digest; break
            }
        }
    }
    if (-not $kind -or $sourceDigest -cne $digest) { throw "Executable does not match a known build input: $relative" }
    $version = $item.VersionInfo
    $rows.Add([ordered]@{ path = $relative; bytes = $item.Length; sha256 = $digest; machine = 'amd64'
        source_kind = $kind; source_reference = $origin; source_sha256 = $sourceDigest
        file_version = $version.FileVersion; product_version = $version.ProductVersion; company = $version.CompanyName })
}
if ($rows.Count -lt 9) { throw 'Incomplete executable dependency inventory' }
$report = [ordered]@{
    schema_version = 1; commit_sha = $CommitSha; artifact_kind = 'windows_distribution_build_inventory'
    qt_version = $catalog.qt_version; livekit_sdk_version = $catalog.livekit_sdk_version
    livekit_rust_revision = $catalog.livekit_rust_revision; livekit_archive_sha256 = $sdkHash
    qt_provenance = 'matched_installed_sdk_not_authenticated_upstream'
    executable_count = $rows.Count; executables = @($rows | Sort-Object { $_.path })
    upstream_evidence = @($evidence | Sort-Object { $_.path })
    attribution_complete = $false; publisher_signing_complete = $false; clean_consumer_windows_verified = $false
    physical_media_verified = $false; gaming_performance_verified = $false; production_qualified = $false
    remaining_review = @('Qt module/transitive attribution and licensing/source-delivery route',
        'LiveKit FFI Rust/WebRTC/codecs build-specific transitive notice reconciliation',
        'Microsoft VC/Qt-bundled Microsoft runtime redistribution material',
        'Owner publisher signing, clean consumer-PC and real media/gameplay acceptance')
}
Write-NewJsonFile (Join-Path $root 'DISTRIBUTION-INVENTORY.json') $report
Write-Host "Matched $($rows.Count) Windows x64 executable files to build inputs; collected $($evidence.Count) upstream evidence file(s). Attribution review remains open."
