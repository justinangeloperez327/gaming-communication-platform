# Validate the signature inventory against the staged bundle manifest.
param([Parameter(Mandatory=$true)][string]$BundleDirectory,
      [Parameter(Mandatory=$true)][string]$InventoryFile)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'windows-distribution.psm1') -Force
$root = [IO.Path]::GetFullPath($BundleDirectory)
& (Join-Path $PSScriptRoot 'verify-windows-test-bundle.ps1') -BundleDirectory $root
$inventory = Get-Content -LiteralPath $InventoryFile -Raw | ConvertFrom-Json
if ($inventory.schema_version -ne 2 -or $inventory.artifact_kind -cne 'unsigned_windows_test_bundle' -or
    $inventory.production_qualified -isnot [bool] -or $inventory.production_qualified -ne $false) { throw 'Unexpected inventory qualification' }
$files = @(Get-ChildItem -LiteralPath $root -Recurse -Force -File | Where-Object { $_.Extension -in @('.exe','.dll') })
$entries = @($inventory.executables)
if ($entries.Count -ne $files.Count -or [int]$inventory.executable_count -ne $files.Count) {
    throw 'Executable inventory count mismatch'
}
$byPath = @{}
foreach ($entry in $entries) {
    $name = [string]$entry.path
    if ($name -match '(^|[\\/])\.\.([\\/]|$)' -or [IO.Path]::IsPathRooted($name) -or $name.Contains(':')) {
        throw 'Unsafe inventory path'
    }
    if ($byPath.ContainsKey($name)) { throw 'Duplicate executable path' }
    if ([string]$entry.sha256 -notmatch '^[0-9a-f]{64}$') { throw 'Invalid digest' }
    if ([string]::IsNullOrWhiteSpace([string]$entry.signature_status)) { throw 'Missing signature status' }
    $byPath[$name] = $entry
}
foreach ($item in $files) {
    $relative = [IO.Path]::GetRelativePath($root,$item.FullName).Replace('\','/')
    if (-not $byPath.ContainsKey($relative)) { throw "Executable not inventoried: $relative" }
    $hash = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -cne [string]$byPath[$relative].sha256) { throw "Executable digest mismatch: $relative" }
    # A self-consistent JSON count is not evidence that a signature is valid.
    $observed = Get-WindowsSigningObservation $item.FullName
    foreach ($key in $observed.Keys) {
        if ($byPath[$relative].$key -cne $observed[$key]) { throw "Signature observation mismatch: $relative ($key)" }
    }
}
$unsigned = @($entries | Where-Object { $_.signature_status -ne 'Valid' }).Count
if ($unsigned -ne [int]$inventory.executables_without_valid_signature) {
    throw 'Signature count mismatch'
}
Write-Host "Executable signature inventory consistent with package: $($files.Count) files; unsigned/untrusted: $unsigned."
