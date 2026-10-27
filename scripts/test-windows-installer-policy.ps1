# Fast regression of the actual Setup script with synthetic, never-executed files.
# No Qt/SDK, application startup, media, network, signing key or physical claim.
#requires -Version 7.0
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'windows-distribution.psm1') -Force
$registry = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{F6CA3831-848C-4148-831E-804B1F6DC06B}_is1'
if (Test-Path -LiteralPath $registry) { throw 'Use a disposable account without an existing GCP installation' }
$compiler = Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6/ISCC.exe'
if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) { throw 'Inno Setup 6 compiler missing' }
$root = Join-Path ([IO.Path]::GetTempPath()) ('gcp-setup-policy-' + [Guid]::NewGuid().ToString('N'))
$app = Join-Path $root 'Application With Spaces'
New-Item -ItemType Directory -Path $root | Out-Null
$legacy = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../packaging/windows/previous-alpha.json') -Raw | ConvertFrom-Json
$userFile = Join-Path $app 'User Notes/keep.txt'

function Invoke-SetupProcess([string]$Program, [string[]]$Arguments, [bool]$Reject = $false) {
    $log = Join-Path $root ([Guid]::NewGuid().ToString('N'))
    $process = $null
    try {
        $process = Start-Process -FilePath $Program -ArgumentList $Arguments -PassThru -Wait:$false -WorkingDirectory (Split-Path $Program) -RedirectStandardOutput "$log.out" -RedirectStandardError "$log.err"
        $process | Wait-Process -Timeout 60 -ErrorAction Stop
        $process.Refresh()
        if ($null -eq $process.ExitCode -or ($Reject -and $process.ExitCode -eq 0) -or (-not $Reject -and $process.ExitCode -ne 0)) {
            Get-ChildItem -LiteralPath $root -Filter 'setup-*.log' -File | ForEach-Object { Get-Content -LiteralPath $_.FullName -Tail 35 | Write-Host }
            throw "Unexpected Setup result ($($process.ExitCode)); rejection required: $Reject"
        }
    } finally {
        if ($process) {
            if (-not $process.HasExited) { $process.Kill($true); $process.WaitForExit(5000) | Out-Null }
            $process.Dispose()
        }
    }
}
function Install([string]$Path, [bool]$Reject = $false) {
    $log = Join-Path $root ('setup-' + [Guid]::NewGuid().ToString('N') + '.log')
    Invoke-SetupProcess $Path @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/SP-',"/DIR=`"$app`"","/LOG=`"$log`"") $Reject
}
function Fingerprint {
    return (Get-Item -LiteralPath $registry).GetValue('GCPPayloadManifestSHA256', $null)
}
function Snapshot {
    return (@(Get-ChildItem -LiteralPath $app -Recurse -Force -File | Sort-Object FullName | ForEach-Object {
        [IO.Path]::GetRelativePath($app, $_.FullName) + ':' + (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    }) -join "`n")
}
function Check-Payload([string]$Payload, [string]$Version) {
    $expected = (Get-FileHash -LiteralPath (Join-Path $Payload 'SHA256-MANIFEST.json') -Algorithm SHA256).Hash
    if ((Get-ItemProperty -LiteralPath $registry).DisplayVersion -cne "$Version-qualification-fixture" -or
        (Fingerprint) -ine $expected -or
        (Get-FileHash -LiteralPath (Join-Path $app 'SHA256-MANIFEST.json') -Algorithm SHA256).Hash -cne $expected) { throw 'Installed version/fingerprint/manifest mismatch' }
    foreach ($name in @('gcp-desktop.exe','qt.conf','payload-a.txt','payload-b.txt')) {
        if ((Get-FileHash -LiteralPath (Join-Path $Payload $name) -Algorithm SHA256).Hash -cne
            (Get-FileHash -LiteralPath (Join-Path $app $name) -Algorithm SHA256).Hash) { throw 'Installed fixture bytes differ' }
    }
    if ([IO.File]::ReadAllText($userFile) -cne 'preserve-user-data') { throw 'Setup changed user data' }
}
function Reject-Unchanged([string]$Installer) {
    $before = Snapshot
    $version = (Get-ItemProperty -LiteralPath $registry).DisplayVersion
    $fingerprint = Fingerprint
    Install $Installer $true
    $after = Snapshot
    $afterVersion = (Get-ItemProperty -LiteralPath $registry).DisplayVersion
    $afterFingerprint = Fingerprint
    if ($after -cne $before -or $afterVersion -cne $version -or $afterFingerprint -cne $fingerprint) {
        Write-Host "Rejected installer: $Installer"
        Write-Host "Version before=[$version] after=[$afterVersion]; fingerprint before=[$fingerprint] after=[$afterFingerprint]"
        Compare-Object ($before -split "`n") ($after -split "`n") | Format-Table -AutoSize | Out-Host
        Get-ChildItem -LiteralPath $root -Filter 'setup-*.log' -File | Sort-Object LastWriteTime | Select-Object -Last 1 | ForEach-Object { Get-Content -LiteralPath $_.FullName -Tail 60 | Write-Host }
        throw 'Rejected Setup changed installed state'
    }
}
function Uninstall {
    Invoke-SetupProcess (Join-Path $app 'unins000.exe') @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART')
    # Inno's detached cleanup can outlive the process we waited for. Do not
    # reinstall into this directory until its old uninstaller has disappeared:
    # otherwise that cleanup can delete the newly installed unins000.exe.
    $cleanupDeadline = [DateTime]::UtcNow.AddSeconds(20)
    while ((Test-Path -LiteralPath (Join-Path $app 'unins000.exe')) -or
           (Test-Path -LiteralPath (Join-Path $app 'unins000.dat'))) {
        if ([DateTime]::UtcNow -ge $cleanupDeadline) { throw 'Uninstaller cleanup did not finish before reinstall' }
        Start-Sleep -Milliseconds 100
    }
    if ((Test-Path -LiteralPath $registry) -or (Test-Path -LiteralPath (Join-Path $app 'gcp-desktop.exe')) -or
        (Test-Path -LiteralPath (Join-Path $app 'SHA256-MANIFEST.json'))) { throw 'Uninstall did not remove registration/payload' }
    if ([IO.File]::ReadAllText($userFile) -cne 'preserve-user-data') { throw 'Uninstall changed user data' }
}
try {
    $installers = @{}; $payloads = @{}
    foreach ($version in @('0.0.0','0.0.1','0.0.2')) {
        $payload = Join-Path $root "payload-$version"
        New-Item -ItemType Directory -Path $payload | Out-Null
        foreach ($name in @('gcp-desktop.exe','qt.conf','payload-a.txt','payload-b.txt')) {
            [IO.File]::WriteAllText((Join-Path $payload $name), "Synthetic fixture $version $name; never executed.")
        }
        if ($version -eq '0.0.0') { [IO.File]::WriteAllText((Join-Path $payload 'obsolete-owned.txt'), 'retired fixture') }
        Write-WindowsPayloadManifest $payload
        $sha = (Get-FileHash -LiteralPath (Join-Path $payload 'SHA256-MANIFEST.json') -Algorithm SHA256).Hash.ToLowerInvariant()
        $output = Join-Path $root "setup-$version"
        New-Item -ItemType Directory -Path $output | Out-Null
        & $compiler "/DBundleDir=$payload" "/DOutputDirPath=$output" "/DReleaseVersion=$version-qualification-fixture" "/DNumericVersion=$version" "/DPayloadManifestSha256=$sha" "/DLegacyManifestSha256=$($legacy.manifest_sha256)" "/DLegacyReleaseLabel=$($legacy.tag.Substring(1))" (Join-Path $PSScriptRoot '../packaging/windows/gcp.iss') | Out-Host
        if ($LASTEXITCODE -ne 0) { throw 'Policy fixture compilation failed' }
        $installers[$version] = Join-Path $output "GCP-$version-qualification-fixture-windows-x64-setup.exe"
        $payloads[$version] = $payload
    }
    New-Item -ItemType Directory -Path (Split-Path $userFile) -Force | Out-Null
    [IO.File]::WriteAllText($userFile, 'preserve-user-data')
    Install $installers['0.0.1']; Check-Payload $payloads['0.0.1'] '0.0.1'
    Remove-Item -LiteralPath (Join-Path $app 'payload-a.txt')
    [IO.File]::WriteAllText((Join-Path $app 'qt.conf'), 'damaged fixture')
    Install $installers['0.0.1']; Check-Payload $payloads['0.0.1'] '0.0.1'
    Install $installers['0.0.2']; Check-Payload $payloads['0.0.2'] '0.0.2'
    Reject-Unchanged $installers['0.0.1']
    $savedFingerprint = Fingerprint
    try {
        Remove-ItemProperty -LiteralPath $registry -Name GCPPayloadManifestSHA256
        Reject-Unchanged $installers['0.0.2']
    } finally { Set-ItemProperty -LiteralPath $registry -Name GCPPayloadManifestSHA256 -Value $savedFingerprint }
    $manifest = Join-Path $app 'SHA256-MANIFEST.json'
    $saved = [IO.File]::ReadAllBytes($manifest)
    try {
        [IO.File]::WriteAllText($manifest, '[]')
        Reject-Unchanged $installers['0.0.2']
    } finally { [IO.File]::WriteAllBytes($manifest, $saved) }
    Uninstall
    Install $installers['0.0.0']; Check-Payload $payloads['0.0.0'] '0.0.0'
    Reject-Unchanged $installers['0.0.2']
    Uninstall
    Write-Host 'Actual Setup script: fingerprint registration/readback, repair, upgrade, guarded rejection, preservation and uninstall passed with synthetic files. No product/hardware qualification.'
} finally {
    if (Test-Path -LiteralPath $registry) {
        $location = (Get-ItemProperty -LiteralPath $registry).InstallLocation
        if ($location.TrimEnd('\') -ine $app.TrimEnd('\')) { throw 'Refusing cleanup of an unrelated installation' }
        $uninstaller = Join-Path $app 'unins000.exe'
        if (Test-Path -LiteralPath $uninstaller) { Invoke-SetupProcess $uninstaller @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART') }
    }
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
