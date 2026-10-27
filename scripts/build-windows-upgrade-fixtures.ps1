# Compile older-version policy fixtures from the actual payload, never release assets.
#requires -Version 7.0
param([Parameter(Mandatory=$true)][string]$BundleDirectory,
      [Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'windows-distribution.psm1') -Force
$root = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $root) { throw 'Upgrade fixtures require a new output directory' }
$bundle = [IO.Path]::GetFullPath($BundleDirectory)
if ($root.StartsWith($bundle + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Fixtures must be outside the payload' }
& (Join-Path $PSScriptRoot 'verify-windows-test-bundle.ps1') -BundleDirectory $bundle
New-Item -ItemType Directory -Path $root | Out-Null
foreach ($case in @('compatible','obsolete')) {
    $payload = Join-Path $root $case
    Copy-Item -LiteralPath $bundle -Destination $payload -Recurse
    # Changing an existing config proves replacement, not just a version label.
    [IO.File]::AppendAllText((Join-Path $payload 'qt.conf'), "`n; older installer policy fixture`n")
    $version = '0.0.1'
    if ($case -eq 'obsolete') {
        $version = '0.0.0'
        [IO.File]::WriteAllText((Join-Path $payload 'gcp-obsolete-fixture.txt'),'Old installer-owned file, absent in the next payload.')
    }
    Write-WindowsPayloadManifest $payload
    & (Join-Path $PSScriptRoot 'package-windows-installer.ps1') -BundleDirectory $payload -OutputDirectory (Join-Path $root ($case + '-installer')) -FixtureVersion $version
}
Write-Host 'Older-version installer policy fixtures built from current binaries. These are not historical releases or qualified migration evidence.'
