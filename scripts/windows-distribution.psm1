# Build-time helpers. No application processes, credentials or signing keys are used.
#requires -Version 7.0
Set-StrictMode -Version Latest

function Write-NewJsonFile {
    param([string]$Path, $Value)
    $pathName = [IO.Path]::GetFullPath($Path)
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($pathName)) | Out-Null
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes(($Value | ConvertTo-Json -Depth 12) + "`n")
    $stream = [IO.File]::Open($pathName, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $stream.Write($bytes, 0, $bytes.Length) } finally { $stream.Dispose() }
}

function Write-WindowsPayloadManifest {
    param([string]$BundleDirectory)
    $root = [IO.Path]::GetFullPath($BundleDirectory)
    foreach ($item in @((Get-Item -LiteralPath $root -Force)) + @(Get-ChildItem -LiteralPath $root -Recurse -Force)) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Bundle links are not permitted' }
    }
    # This list is embedded in Setup for a read-only upgrade preflight. It never
    # authorizes deletion of old files or user data.
    $names = @('GCP-PAYLOAD-PATHS.txt', 'SHA256-MANIFEST.json') + @(Get-ChildItem -LiteralPath $root -Recurse -Force -File |
        ForEach-Object { [IO.Path]::GetRelativePath($root, $_.FullName).Replace('\', '/') } |
        Where-Object { $_ -notin @('GCP-PAYLOAD-PATHS.txt', 'SHA256-MANIFEST.json') })
    [IO.File]::WriteAllLines((Join-Path $root 'GCP-PAYLOAD-PATHS.txt'), @($names | Sort-Object -Unique), [Text.UTF8Encoding]::new($false))
    $manifestPath = Join-Path $root 'SHA256-MANIFEST.json'
    $rows = @(Get-ChildItem -LiteralPath $root -Recurse -Force -File | Where-Object { $_.FullName -ine $manifestPath } | ForEach-Object {
        [ordered]@{ path = [IO.Path]::GetRelativePath($root, $_.FullName).Replace('\', '/'); bytes = $_.Length
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    })
    $json = ConvertTo-Json -InputObject @($rows | Sort-Object { $_.path }) -Depth 4
    [IO.File]::WriteAllText((Join-Path $root 'SHA256-MANIFEST.json'), $json + "`n", [Text.UTF8Encoding]::new($false))
}

function Assert-WindowsX64PE {
    param([string]$Path)
    $reader = [IO.BinaryReader]::new([IO.File]::OpenRead($Path))
    try {
        if ($reader.BaseStream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5a4d) { throw 'Invalid PE file' }
        $reader.BaseStream.Position = 0x3c; $offset = $reader.ReadUInt32()
        if ($offset -gt $reader.BaseStream.Length - 26) { throw 'Invalid PE header offset' }
        $reader.BaseStream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) { throw 'Expected Windows x64 PE header' }
    } finally { $reader.Dispose() }
}

function Get-WindowsSigningObservation {
    param([string]$Path)
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    [ordered]@{
        signature_status = [string]$signature.Status
        signature_type = [string]$signature.SignatureType
        signer_subject = if ($signature.SignerCertificate) { [string]$signature.SignerCertificate.Subject } else { $null }
        signer_thumbprint = if ($signature.SignerCertificate) { [string]$signature.SignerCertificate.Thumbprint } else { $null }
        timestamp_subject = if ($signature.TimeStamperCertificate) { [string]$signature.TimeStamperCertificate.Subject } else { $null }
        timestamp_thumbprint = if ($signature.TimeStamperCertificate) { [string]$signature.TimeStamperCertificate.Thumbprint } else { $null }
    }
}

function Assert-PublisherSignature {
    param($Observation, [string]$ExpectedThumbprint)
    if ($ExpectedThumbprint -cnotmatch '^[0-9A-Fa-f]{40}$') { throw 'Supply the exact owner certificate SHA-1 thumbprint (40 hex characters)' }
    if ($Observation.signature_status -cne 'Valid' -or $Observation.signature_type -cne 'Authenticode' -or
        $Observation.signer_thumbprint -ine $ExpectedThumbprint -or
        [string]::IsNullOrWhiteSpace($Observation.timestamp_thumbprint)) {
        throw 'Publisher signature must be valid, embedded, timestamped and match the expected certificate'
    }
}

Export-ModuleMember -Function Write-NewJsonFile, Write-WindowsPayloadManifest, Get-WindowsSigningObservation, Assert-PublisherSignature, Assert-WindowsX64PE
