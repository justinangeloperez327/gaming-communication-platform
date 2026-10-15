# Inventory signatures for all executable components. Unsigned test bundles remain unsigned.
param([Parameter(Mandatory=$true)][string]$BundleDirectory,
      [Parameter(Mandatory=$true)][string]$OutputFile)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($BundleDirectory)
& (Join-Path $PSScriptRoot 'verify-windows-test-bundle.ps1') -BundleDirectory $root
$executables = @(Get-ChildItem -LiteralPath $root -Recurse -File | Where-Object {
    $_.Extension -in @('.exe', '.dll')
})
if ($executables.Count -lt 4) { throw 'No executable dependency inventory available' }
$rows = foreach ($item in $executables) {
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Executable links are not permitted' }
    $signature = Get-AuthenticodeSignature -LiteralPath $item.FullName
    [pscustomobject]@{
        path = [IO.Path]::GetRelativePath($root, $item.FullName).Replace('\\', '/')
        sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        signature_status = [string]$signature.Status
        signer_subject = if ($signature.SignerCertificate) { [string]$signature.SignerCertificate.Subject } else { $null }
    }
}
$unsignedCount = @($rows | Where-Object { $_.signature_status -ne 'Valid' }).Count
$report = [ordered]@{
    artifact_kind = 'unsigned_windows_test_bundle'
    production_qualified = $false
    executable_count = $executables.Count
    executables_without_valid_signature = $unsignedCount
    executables = @($rows | Sort-Object path)
}
$target = [IO.Path]::GetFullPath($OutputFile)
$folder = [IO.Path]::GetDirectoryName($target)
if (-not [IO.Directory]::Exists($folder)) { [IO.Directory]::CreateDirectory($folder) | Out-Null }
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $target -Encoding utf8
Write-Host "Executable signature inventory written. $unsignedCount executable(s) lack a valid Authenticode signature."
