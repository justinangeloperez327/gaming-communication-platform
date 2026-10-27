# Verify an owner-signed payload/installer. This never signs or accesses a private key.
#requires -Version 7.0
param([Parameter(Mandatory=$true)][string]$BundleDirectory,
      [Parameter(Mandatory=$true)][string]$ExpectedThumbprint,
      [Parameter(Mandatory=$true)][string]$Installer)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'windows-distribution.psm1') -Force
& (Join-Path $PSScriptRoot 'verify-windows-test-bundle.ps1') -BundleDirectory $BundleDirectory
foreach ($path in @((Join-Path $BundleDirectory 'gcp-desktop.exe'), (Join-Path $BundleDirectory 'gcp-livekit-voice.exe'),
        (Join-Path $BundleDirectory 'gcp-livekit-screen.exe'), [IO.Path]::GetFullPath($Installer))) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw 'Signed executable missing' }
    Assert-PublisherSignature (Get-WindowsSigningObservation $path) $ExpectedThumbprint
}
Write-Host 'All three GCP executables and Setup have valid embedded timestamped signatures from the expected publisher; product acceptance remains separate.'
