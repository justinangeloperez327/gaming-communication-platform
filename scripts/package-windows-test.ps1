# Package the verified unsigned desktop payload used by ZIP and alpha installer.
param(
    [Parameter(Mandatory=$true)][string]$BuildDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$build = [IO.Path]::GetFullPath($BuildDirectory)
$output = [IO.Path]::GetFullPath($OutputDirectory)
if ($output.StartsWith($build + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Output must be outside the build tree'
}
if (-not (Test-Path -LiteralPath $build -PathType Container)) { throw "Build directory missing" }
$required = @('gcp-desktop.exe', 'gcp-livekit-voice.exe', 'livekit.dll', 'livekit_ffi.dll')
foreach ($file in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $build $file) -PathType Leaf)) { throw "Missing bundle dependency: $file" }
}
$deploy = Get-Command windeployqt -ErrorAction Stop
$noticeSource = Join-Path $PSScriptRoot '../third_party/notices'
$catalog = Get-Content -LiteralPath (Join-Path $noticeSource 'catalog.json') -Raw | ConvertFrom-Json
$qtVersion = & $deploy.Source --version
if ($LASTEXITCODE -ne 0 -or ($qtVersion -join ' ') -notmatch '(?<![0-9])([0-9]+\.[0-9]+\.[0-9]+)(?![0-9])') {
    throw 'Cannot determine Qt deployment version for notice baseline'
}
if ($Matches[1] -ne $catalog.qt_version) { throw 'Qt deployment version does not match notice baseline' }
$sdkConfig = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../cmake/GcpLiveKit.cmake') -Raw
if ($sdkConfig -notmatch 'set\(GCP_LIVEKIT_VERSION\s+"([^"]+)"\)' -or
    $Matches[1] -ne $catalog.livekit_sdk_version) { throw 'LiveKit SDK version does not match notice baseline' }
if (Test-Path -LiteralPath $output) { throw 'Use a new, empty output directory (refusing overwrite)' }
New-Item -ItemType Directory -Path $output -Force | Out-Null
foreach ($file in $required) { Copy-Item -LiteralPath (Join-Path $build $file) -Destination $output }
& $deploy.Source --release --qmldir (Join-Path $PSScriptRoot '..\apps\desktop\qml') (Join-Path $output 'gcp-desktop.exe')
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE" }
Copy-Item -LiteralPath $noticeSource -Destination (Join-Path $output 'third-party') -Recurse
& (Join-Path $PSScriptRoot 'verify-windows-notices.ps1') -BundleDirectory $output
foreach ($file in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $output $file) -PathType Leaf)) { throw "Bundle missing $file" }
}
$manifest = foreach ($item in Get-ChildItem -LiteralPath $output -Recurse -File) {
    [pscustomobject]@{
        path = [IO.Path]::GetRelativePath($output, $item.FullName).Replace('\','/')
        sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        bytes = $item.Length
    }
}
@($manifest) | Sort-Object path | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $output 'SHA256-MANIFEST.json') -Encoding utf8
Write-Host "Unsigned test bundle prepared in $output. NOT production qualified."
