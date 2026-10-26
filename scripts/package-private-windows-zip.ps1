# Create a privately shareable portable ZIP only after CI runtime checks succeed.
param(
    [Parameter(Mandatory=$true)][string]$BundleDirectory,
    [Parameter(Mandatory=$true)][string]$OutputZip
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($BundleDirectory)
$zip = [IO.Path]::GetFullPath($OutputZip)
if (-not (Test-Path -LiteralPath $root -PathType Container)) { throw 'Bundle directory missing' }
if ($zip.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'ZIP must be outside bundle root'
}
if (Test-Path -LiteralPath $zip) { throw 'Refusing to overwrite an existing ZIP' }
& (Join-Path $PSScriptRoot 'verify-windows-test-bundle.ps1') -BundleDirectory $root
$folder = [IO.Path]::GetDirectoryName($zip)
if (-not (Test-Path -LiteralPath $folder)) { New-Item -ItemType Directory -Path $folder -Force | Out-Null }
# Compress-Archive silently omits hidden files on Windows. Archive the entire
# already verified directory so every manifest entry survives extraction.
[IO.Compression.ZipFile]::CreateFromDirectory($root, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
if (-not (Test-Path -LiteralPath $zip -PathType Leaf)) { throw 'ZIP creation failed' }
$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "Private unsigned portable ZIP: $zip"
Write-Host "SHA256: $hash"
