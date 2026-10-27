# Inventory signatures for all executable components. Unsigned test bundles remain unsigned.
param([Parameter(Mandatory=$true)][string]$BundleDirectory,
      [Parameter(Mandatory=$true)][string]$OutputFile)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'windows-distribution.psm1') -Force
$root = [IO.Path]::GetFullPath($BundleDirectory)
& (Join-Path $PSScriptRoot 'verify-windows-test-bundle.ps1') -BundleDirectory $root
$executables = @(Get-ChildItem -LiteralPath $root -Recurse -Force -File | Where-Object {
    $_.Extension -in @('.exe', '.dll')
})
if ($executables.Count -lt 4) { throw 'No executable dependency inventory available' }
$rows = foreach ($item in $executables) {
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Executable links are not permitted' }
    $row = [ordered]@{
        path = [IO.Path]::GetRelativePath($root, $item.FullName).Replace('\', '/')
        sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $observation = Get-WindowsSigningObservation $item.FullName
    foreach ($key in $observation.Keys) { $row[$key] = $observation[$key] }
    [pscustomobject]$row
}
$unsignedCount = @($rows | Where-Object { $_.signature_status -ne 'Valid' }).Count
$report = [ordered]@{
    schema_version = 2
    artifact_kind = 'unsigned_windows_test_bundle'
    production_qualified = $false
    executable_count = $executables.Count
    executables_without_valid_signature = $unsignedCount
    executables = @($rows | Sort-Object path)
}
$target = [IO.Path]::GetFullPath($OutputFile)
if ($target.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Signing inventory must be outside the bundle' }
$folder = [IO.Path]::GetDirectoryName($target)
if (-not [IO.Directory]::Exists($folder)) { [IO.Directory]::CreateDirectory($folder) | Out-Null }
Write-NewJsonFile $target $report
Write-Host "Executable signature inventory written. $unsignedCount executable(s) lack a valid Authenticode signature."
