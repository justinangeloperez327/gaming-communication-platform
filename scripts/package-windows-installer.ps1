param(
    [Parameter(Mandatory=$true)][string]$BundleDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateSet('0.0.0','0.0.1')][string]$FixtureVersion
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($BundleDirectory)
$output = [IO.Path]::GetFullPath($OutputDirectory)
if ($output -eq $root -or $output.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Installer output must be outside the bundle'
}
& (Join-Path $PSScriptRoot 'verify-windows-test-bundle.ps1') -BundleDirectory $root
$config = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../packaging/windows/version.json') -Raw | ConvertFrom-Json
if ($config.version -notmatch '^\d+\.\d+\.\d+$' -or $config.tag -cne "v$($config.version)-alpha.1" -or
    @($config.version.Split('.') | Where-Object { [long]$_ -gt 65535 }).Count -ne 0) {
    throw 'Invalid alpha release version'
}
if (Test-Path -LiteralPath $output) { throw 'Installer output must be a new directory' }
$compiler = Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6/ISCC.exe'
if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) { throw 'Inno Setup 6 compiler is required' }
New-Item -ItemType Directory -Path $output | Out-Null
$label = $config.tag.Substring(1)
$numeric = $config.version
if ($FixtureVersion) { $numeric = $FixtureVersion; $label = "$FixtureVersion-qualification-fixture" }
& $compiler "/DBundleDir=$root" "/DOutputDirPath=$output" "/DReleaseVersion=$label" "/DNumericVersion=$numeric" (Join-Path $PSScriptRoot '../packaging/windows/gcp.iss')
if ($LASTEXITCODE -ne 0) { throw "Installer compiler failed: $LASTEXITCODE" }
$installer = Join-Path $output "GCP-$label-windows-x64-setup.exe"
if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) { throw 'Installer missing' }
(Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant() |
    Set-Content -LiteralPath "$installer.sha256" -Encoding ascii
Write-Host "Unsigned alpha installer built: $installer"
