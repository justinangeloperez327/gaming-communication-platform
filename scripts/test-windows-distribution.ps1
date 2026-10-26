# Synthetic metadata/PE-header fixtures; no product, signing key or physical qualification.
#requires -Version 7.0
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'windows-distribution.psm1') -Force
$root = Join-Path ([IO.Path]::GetTempPath()) ('gcp-distribution-tests-' + [Guid]::NewGuid().ToString('N'))
$commit = '1' * 40
function Rejected([scriptblock]$Action, [string]$Name) {
    $failed = $false
    try { & $Action } catch { $failed = $true }
    if (-not $failed) { throw "Invalid distribution data accepted: $Name" }
}
try {
    New-Item -ItemType Directory -Path $root | Out-Null
    $fixture = [byte[]]::new(128); $fixture[0] = 0x4d; $fixture[1] = 0x5a; $fixture[0x3c] = 64
    $fixture[64] = 0x50; $fixture[65] = 0x45; $fixture[68] = 0x64; $fixture[69] = 0x86
    foreach ($name in @('gcp-desktop.exe','gcp-livekit-voice.exe','gcp-livekit-screen.exe','livekit.dll','livekit_ffi.dll','Qt6WebSockets.dll')) {
        [IO.File]::WriteAllBytes((Join-Path $root $name),$fixture)
    }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../third_party/notices') -Destination (Join-Path $root 'third-party') -Recurse
    $catalog = Get-Content -LiteralPath (Join-Path $root 'third-party/catalog.json') -Raw | ConvertFrom-Json
    $rows = @(Get-ChildItem -LiteralPath $root -File | ForEach-Object {
        $kind = if ($_.Name.StartsWith('gcp-')) { 'gcp-build' } elseif ($_.Name.StartsWith('livekit')) { 'livekit-release-archive' } else { 'qt-installation' }
        $sha = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        [ordered]@{ path = $_.Name; bytes = $_.Length; sha256 = $sha; source_sha256 = $sha; machine = 'amd64'
            source_kind = $kind; source_reference = if ($kind -eq 'gcp-build') { $commit } else { 'fixture' } }
    })
    $report = [ordered]@{ schema_version = 1; commit_sha = $commit; artifact_kind = 'windows_distribution_build_inventory'
        qt_version = $catalog.qt_version; livekit_sdk_version = $catalog.livekit_sdk_version; livekit_rust_revision = $catalog.livekit_rust_revision
        livekit_archive_sha256 = $catalog.livekit_windows_archive_sha256; qt_provenance = 'matched_installed_sdk_not_authenticated_upstream'
        executable_count = $rows.Count; executables = $rows; upstream_evidence = @()
        attribution_complete = $false; publisher_signing_complete = $false; clean_consumer_windows_verified = $false
        physical_media_verified = $false; gaming_performance_verified = $false; production_qualified = $false }
    function Save-Report {
        [IO.File]::WriteAllText((Join-Path $root 'DISTRIBUTION-INVENTORY.json'), ($report | ConvertTo-Json -Depth 10), [Text.UTF8Encoding]::new($false))
        Write-WindowsPayloadManifest $root
    }
    function Verify-Report { & (Join-Path $PSScriptRoot 'verify-windows-distribution-inventory.ps1') -BundleDirectory $root -ExpectedCommit $commit }
    Save-Report; Verify-Report
    foreach ($claim in @($true,'False',0,$null)) {
        $report.production_qualified = $claim; Save-Report
        Rejected { Verify-Report } 'false claim must be a boolean'
    }
    $report.production_qualified = $false
    $report.commit_sha = '2' * 40; Save-Report; Rejected { Verify-Report } 'wrong source commit'; $report.commit_sha = $commit
    $report.executables = $rows + $rows[0]; Save-Report; Rejected { Verify-Report } 'duplicate executable'; $report.executables = $rows
    $rows[0].source_sha256 = '0' * 64; Save-Report; Rejected { Verify-Report } 'source hash mismatch'; $rows[0].source_sha256 = $rows[0].sha256
    $oldKind = $rows[0].source_kind; $rows[0].source_kind = 'unknown'; Save-Report; Rejected { Verify-Report } 'unclassified dependency'; $rows[0].source_kind = $oldKind
    Save-Report
    $pe = Join-Path $root 'Qt6WebSockets.dll'
    $fixture[69] = 0; [IO.File]::WriteAllBytes($pe,$fixture)
    Rejected { Assert-WindowsX64PE $pe } 'wrong PE architecture'
    $fixture[69] = 0x86; [IO.File]::WriteAllBytes($pe,$fixture)
    Save-Report
    $signatures = $root + '-signatures.json'
    & (Join-Path $PSScriptRoot 'inventory-windows-signatures.ps1') -BundleDirectory $root -OutputFile $signatures
    & (Join-Path $PSScriptRoot 'verify-windows-signature-inventory.ps1') -BundleDirectory $root -InventoryFile $signatures
    Rejected { & (Join-Path $PSScriptRoot 'inventory-windows-signatures.ps1') -BundleDirectory $root -OutputFile $signatures } 'existing inventory output'
    $saved = [IO.File]::ReadAllText($signatures)
    $signing = $saved | ConvertFrom-Json
    $signing.executables[0].signature_status = 'Valid'
    $signing.executables_without_valid_signature = @($signing.executables | Where-Object { $_.signature_status -ne 'Valid' }).Count
    [IO.File]::WriteAllText($signatures, ($signing | ConvertTo-Json -Depth 10))
    Rejected { & (Join-Path $PSScriptRoot 'verify-windows-signature-inventory.ps1') -BundleDirectory $root -InventoryFile $signatures } 'forged signature observation'
    $validPolicyFixture = @{ signature_status = 'Valid'; signature_type = 'Authenticode'; signer_thumbprint = ('A' * 40); timestamp_thumbprint = ('B' * 40) }
    Assert-PublisherSignature $validPolicyFixture ('A' * 40)
    Rejected { Assert-PublisherSignature $validPolicyFixture ('C' * 40) } 'wrong publisher'
    $validPolicyFixture.timestamp_thumbprint = $null
    Rejected { Assert-PublisherSignature $validPolicyFixture ('A' * 40) } 'missing timestamp'
    Rejected { & (Join-Path $PSScriptRoot 'verify-windows-publisher-signatures.ps1') -BundleDirectory $root -ExpectedThumbprint ('A' * 40) -Installer (Join-Path $root 'gcp-desktop.exe') } 'unsigned fixture cannot pass owner signing'
    Write-NewJsonFile ($root + '-new.json') @{ value = 1 }
    Rejected { Write-NewJsonFile ($root + '-new.json') @{ value = 2 } } 'new evidence refuses overwrite'
    Write-Host 'Distribution provenance/schema, real signature observation, owner policy and no-overwrite fixtures passed; no real publisher signing or product qualification.'
} finally {
    Remove-Item -LiteralPath $root, ($root + '-signatures.json'), ($root + '-new.json') -Recurse -Force -ErrorAction SilentlyContinue
}
