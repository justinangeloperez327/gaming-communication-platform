# Verify the notice baseline against the catalog in this source revision.
param([Parameter(Mandatory=$true)][string]$BundleDirectory)
$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot '../third_party/notices'
$root = Join-Path $BundleDirectory 'third-party'
$catalogPath = Join-Path $root 'catalog.json'
if (-not (Test-Path -LiteralPath $catalogPath -PathType Leaf)) { throw 'Missing third-party notice catalog' }
$expectedCatalog = Join-Path $source 'catalog.json'
if ((Get-FileHash -LiteralPath $catalogPath -Algorithm SHA256).Hash -ne
    (Get-FileHash -LiteralPath $expectedCatalog -Algorithm SHA256).Hash) {
    throw 'Notice catalog does not match this source revision'
}
$catalog = Get-Content -LiteralPath $expectedCatalog -Raw | ConvertFrom-Json
foreach ($entry in $catalog.files) {
    $path = Join-Path $root $entry.path
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing notice: $($entry.path)" }
    if ((Get-Item -LiteralPath $path).Length -ne $entry.bytes -or $entry.bytes -le 0 -or
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry.sha256) {
        throw "Invalid notice: $($entry.path)"
    }
}
$readme = Join-Path $root 'README.md'
if (-not (Test-Path -LiteralPath $readme -PathType Leaf) -or
    (Get-FileHash -LiteralPath $readme -Algorithm SHA256).Hash -ne
    (Get-FileHash -LiteralPath (Join-Path $source 'README.md') -Algorithm SHA256).Hash) {
    throw 'Missing or changed third-party notice coverage statement'
}
Write-Host 'Pinned source notices verified; build-specific attribution and source-delivery review remain pending.'
