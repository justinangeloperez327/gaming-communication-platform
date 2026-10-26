# Verify the build inventory is complete, byte-bound and never asserts owner acceptance.
#requires -Version 7.0
param([Parameter(Mandatory=$true)][string]$BundleDirectory,
      [Parameter(Mandatory=$true)][string]$ExpectedCommit)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'windows-distribution.psm1') -Force
& (Join-Path $PSScriptRoot 'verify-windows-test-bundle.ps1') -BundleDirectory $BundleDirectory
$root = [IO.Path]::GetFullPath($BundleDirectory)
$report = Get-Content -LiteralPath (Join-Path $root 'DISTRIBUTION-INVENTORY.json') -Raw | ConvertFrom-Json
$catalog = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../third_party/notices/catalog.json') -Raw | ConvertFrom-Json
if ($ExpectedCommit -cnotmatch '^[0-9a-f]{40}$' -or $report.commit_sha -cne $ExpectedCommit -or
    $report.schema_version -ne 1 -or $report.artifact_kind -cne 'windows_distribution_build_inventory' -or
    $report.qt_version -cne $catalog.qt_version -or $report.livekit_sdk_version -cne $catalog.livekit_sdk_version -or
    $report.livekit_archive_sha256 -cne $catalog.livekit_windows_archive_sha256 -or $report.livekit_rust_revision -cne $catalog.livekit_rust_revision -or
    $report.qt_provenance -cne 'matched_installed_sdk_not_authenticated_upstream') {
    throw 'Distribution inventory source identity mismatch'
}
foreach ($key in @('attribution_complete','publisher_signing_complete','clean_consumer_windows_verified','physical_media_verified','gaming_performance_verified','production_qualified')) {
    if ($report.$key -isnot [bool] -or $report.$key -ne $false) { throw "Unsupported distribution claim: $key" }
}
$files = @(Get-ChildItem -LiteralPath $root -Recurse -Force -File | Where-Object { $_.Extension -in @('.exe','.dll') })
$rows = @($report.executables)
if ($rows.Count -ne $files.Count -or $report.executable_count -ne $files.Count) { throw 'Distribution executable count mismatch' }
$byPath = @{}
foreach ($row in $rows) {
    if ($byPath.ContainsKey([string]$row.path) -or $row.source_kind -cnotin @('gcp-build','livekit-release-archive','microsoft-vc-redist','qt-installation') -or
        $row.machine -cne 'amd64' -or $row.sha256 -cnotmatch '^[a-f0-9]{64}$' -or $row.source_sha256 -cne $row.sha256 -or
        [string]::IsNullOrWhiteSpace($row.source_reference)) { throw 'Invalid executable provenance entry' }
    $byPath[[string]$row.path] = $row
}
foreach ($file in $files) {
    Assert-WindowsX64PE $file.FullName
    $relative = [IO.Path]::GetRelativePath($root,$file.FullName).Replace('\','/')
    $row = $byPath[$relative]
    if ($null -eq $row -or $row.bytes -ne $file.Length -or $row.sha256 -cne (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()) {
        throw "Distribution inventory digest mismatch: $relative"
    }
}
foreach ($name in @('gcp-desktop.exe','gcp-livekit-voice.exe','gcp-livekit-screen.exe')) {
    if ($byPath[$name].source_kind -cne 'gcp-build' -or $byPath[$name].source_reference -cne $ExpectedCommit) { throw 'GCP build provenance missing' }
}
foreach ($name in @('livekit.dll','livekit_ffi.dll')) {
    if ($byPath[$name].source_kind -cne 'livekit-release-archive') { throw 'Pinned LiveKit binary provenance missing' }
}
$seen = @{}
foreach ($row in $report.upstream_evidence) {
    $relative = [string]$row.path
    if (-not $relative.StartsWith('third-party/build/', [StringComparison]::Ordinal) -or $relative -match '(^|/)\.\.?(/|$)|[:\\\x00-\x1f]' -or $seen.ContainsKey($relative)) { throw 'Unsafe or duplicate upstream evidence' }
    $path = Join-Path $root $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -ne $row.bytes -or
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $row.sha256) { throw 'Upstream evidence digest mismatch' }
    $seen[$relative] = $true
}
$evidenceDirectory = Join-Path $root 'third-party/build'
if (Test-Path -LiteralPath $evidenceDirectory -PathType Container) {
    foreach ($file in Get-ChildItem -LiteralPath $evidenceDirectory -Recurse -Force -File) {
        if (-not $seen.ContainsKey([IO.Path]::GetRelativePath($root,$file.FullName).Replace('\','/'))) { throw 'Upstream attribution file missing from evidence inventory' }
    }
}
Write-Host "Distribution inventory verified for $($files.Count) PE files; owner acceptance and complete attribution remain pending."
