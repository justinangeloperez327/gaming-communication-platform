# Package the verified unsigned desktop payload used by ZIP and alpha installer.
param(
    [Parameter(Mandatory=$true)][string]$BuildDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$CommitSha,
    [string]$LiveKitArchive
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'windows-distribution.psm1') -Force
if (-not $CommitSha) {
    $CommitSha = & git -C (Join-Path $PSScriptRoot '..') rev-parse HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Pass the full source CommitSha for an exported source tree' }
}
if ($CommitSha -cnotmatch '^[0-9a-f]{40}$') { throw 'Expected full source CommitSha' }
$build = [IO.Path]::GetFullPath($BuildDirectory)
if (-not $LiveKitArchive) { $LiveKitArchive = Join-Path (Split-Path $build -Parent) '_downloads/livekit-sdk-windows-x64-1.12.2.zip' }
$output = [IO.Path]::GetFullPath($OutputDirectory)
if ($output.StartsWith($build + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Output must be outside the build tree'
}
if (-not (Test-Path -LiteralPath $build -PathType Container)) { throw "Build directory missing" }
$required = @('gcp-desktop.exe', 'gcp-livekit-voice.exe','gcp-livekit-screen.exe', 'livekit.dll', 'livekit_ffi.dll')
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
# qt.conf defines relocation. Keep Qt DLL bytes identical to the installed SDK
# and use only the explicitly inventoried app-local CRT, not a redist installer.
& $deploy.Source --release --no-patchqt --no-compiler-runtime --qmldir (Join-Path $PSScriptRoot '..\apps\desktop\qml') (Join-Path $output 'gcp-desktop.exe')
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE" }
# The regular Windows platform plugin is deployed by windeployqt. Our no-display
# startup check also needs the offscreen plugin, without a developer Qt fallback.
$qtRoot = Split-Path (Split-Path $deploy.Source -Parent) -Parent
$offscreen = Join-Path $qtRoot 'plugins/platforms/qoffscreen.dll'
if (-not (Test-Path -LiteralPath $offscreen -PathType Leaf)) { throw 'Qt offscreen startup-test plugin missing' }
Copy-Item -LiteralPath $offscreen -Destination (Join-Path $output 'platforms/qoffscreen.dll')
@'
[Paths]
Prefix=.
Plugins=.
QmlImports=qml
Translations=translations
'@ | Set-Content -LiteralPath (Join-Path $output 'qt.conf') -Encoding ascii
# Hosted builds do not necessarily export VCINSTALLDIR for windeployqt. Include
# the VS 2022 x64 CRT app-locally instead of depending on the host's global runtime.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall = & $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($vsInstall)) { throw 'Cannot locate Visual Studio 2022 CRT redistributables' }
$versions = @(Get-ChildItem -LiteralPath (Join-Path $vsInstall 'VC/Redist/MSVC') -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+(\.\d+)?$' -and
        (Test-Path -LiteralPath (Join-Path $_.FullName 'x64/Microsoft.VC143.CRT')) } |
    Sort-Object { [version]$_.Name } -Descending)
if ($versions.Count -eq 0) { throw 'VS 2022 x64 CRT redistributables missing' }
$crt = Join-Path $versions[0].FullName 'x64/Microsoft.VC143.CRT'
foreach ($name in @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $crt $name) -PathType Leaf)) { throw "Missing CRT runtime: $name" }
}
Get-ChildItem -LiteralPath $crt -Filter '*.dll' -File | Copy-Item -Destination $output
Copy-Item -LiteralPath $noticeSource -Destination (Join-Path $output 'third-party') -Recurse
& (Join-Path $PSScriptRoot 'verify-windows-notices.ps1') -BundleDirectory $output
foreach ($file in ($required + @('Qt6WebSockets.dll'))) {
    if (-not (Test-Path -LiteralPath (Join-Path $output $file) -PathType Leaf)) { throw "Bundle missing $file" }
}
& (Join-Path $PSScriptRoot 'write-windows-distribution-inventory.ps1') -BundleDirectory $output -QtDirectory $qtRoot -CrtDirectory $crt -LiveKitArchive $LiveKitArchive -CommitSha $CommitSha
Write-WindowsPayloadManifest $output
& (Join-Path $PSScriptRoot 'verify-windows-distribution-inventory.ps1') -BundleDirectory $output -ExpectedCommit $CommitSha
Write-Host "Unsigned test bundle prepared in $output. NOT production qualified."
