# Package an unsigned Windows desktop test bundle, never a production installer.
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
if (Test-Path -LiteralPath $output) { throw 'Use a new, empty output directory (refusing overwrite)' }
New-Item -ItemType Directory -Path $output -Force | Out-Null
foreach ($file in $required) { Copy-Item -LiteralPath (Join-Path $build $file) -Destination $output }
& $deploy.Source --release --qmldir (Join-Path $PSScriptRoot '..\apps\desktop\qml') (Join-Path $output 'gcp-desktop.exe')
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE" }
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
