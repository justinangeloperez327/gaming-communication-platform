# Run only on a disposable Windows account: this changes its per-user registration.
#requires -Version 7.0
param([Parameter(Mandatory=$true)][string]$Installer,
      [Parameter(Mandatory=$true)][string]$BundleDirectory,
      [Parameter(Mandatory=$true)][string]$FixtureDirectory,
      [Parameter(Mandatory=$true)][string]$SettingsProbe,
      [Parameter(Mandatory=$true)][string]$PreviousInstaller,
      [Parameter(Mandatory=$true)][string]$CommitSha,
      [Parameter(Mandatory=$true)][string]$OutputFile)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'windows-distribution.psm1') -Force
$registry = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{F6CA3831-848C-4148-831E-804B1F6DC06B}_is1'
if (Test-Path -LiteralPath $registry) { throw 'Use a disposable account without an existing GCP Alpha install' }
if (Test-Path -LiteralPath $OutputFile) { throw 'Installer acceptance output must not exist' }
if ($CommitSha -cnotmatch '^[0-9a-f]{40}$') { throw 'Expected full source commit SHA' }
$installerPath = (Resolve-Path -LiteralPath $Installer).Path
$historical = (Resolve-Path -LiteralPath $PreviousInstaller).Path
$previousRelease = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../packaging/windows/previous-alpha.json') -Raw | ConvertFrom-Json
if ((Get-Item -LiteralPath $historical).Length -ne $previousRelease.installer_bytes -or
    (Get-FileHash -LiteralPath $historical -Algorithm SHA256).Hash.ToLowerInvariant() -cne $previousRelease.installer_sha256) { throw 'Historical alpha installer bytes do not match the pin' }
$compatible = (Resolve-Path -LiteralPath (Join-Path $FixtureDirectory 'compatible-installer/GCP-0.0.1-qualification-fixture-windows-x64-setup.exe')).Path
$obsolete = (Resolve-Path -LiteralPath (Join-Path $FixtureDirectory 'obsolete-installer/GCP-0.0.0-qualification-fixture-windows-x64-setup.exe')).Path
$config = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../packaging/windows/version.json') -Raw | ConvertFrom-Json
$label = $config.tag.Substring(1)
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('gcp-installer-' + [Guid]::NewGuid().ToString('N'))
$appDir = Join-Path $testRoot 'Application With Spaces'
$shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'Gaming Communication Platform Alpha/Gaming Communication Platform Alpha.lnk'
# Production QStandardPaths::AppConfigLocation with its organization/name.
# A test-only helper reads it through DesktopSettings; --smoke-qml uses memory only.
$profileDir = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'GCP/Gaming Communication Platform'
$preferences = Join-Path $profileDir 'preferences.json'
if (Test-Path -LiteralPath $preferences) { throw 'Use an account without existing GCP preferences' }
$profileExisted = Test-Path -LiteralPath $profileDir
$preferenceBytes = [Text.UTF8Encoding]::new($false).GetBytes('{"version":1,"remember_server":true,"server_origin":"https://settings-fixture.invalid","input_id":"fixture-input","output_id":"fixture-output","push_to_talk":true}')
$userRelative = 'User Notes/user-keep.txt'
$userText = 'Installer acceptance preservation fixture'
$snapshotCount = 0
New-Item -ItemType Directory -Path $testRoot | Out-Null

function Invoke-CheckedProcess([string]$Program, [string[]]$Arguments, [bool]$MustReject = $false) {
    $logBase = Join-Path $testRoot ([Guid]::NewGuid().ToString('N'))
    $child = $null
    try {
        $child = Start-Process -FilePath $Program -ArgumentList $Arguments -WorkingDirectory (Split-Path $Program) -RedirectStandardOutput "$logBase.stdout" -RedirectStandardError "$logBase.stderr" -PassThru
        $child | Wait-Process -Timeout 90 -ErrorAction Stop
        $child.Refresh()
        if ($null -eq $child.ExitCode -or ($MustReject -and $child.ExitCode -eq 0) -or (-not $MustReject -and $child.ExitCode -ne 0)) {
            throw "Unexpected installer result: $Program ($($child.ExitCode)), rejection required: $MustReject"
        }
    } catch {
        foreach ($log in @("$logBase.stdout", "$logBase.stderr")) { if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Tail 60 | Write-Host } }
        Get-ChildItem -LiteralPath $testRoot -Filter 'setup-*.log' -File | ForEach-Object { Get-Content -LiteralPath $_.FullName -Tail 30 | Write-Host }
        throw
    } finally {
        if ($child) { if (-not $child.HasExited) { $child.Kill($true); $child.WaitForExit(5000) | Out-Null }; $child.Dispose() }
    }
}
function Install([string]$Program, [bool]$Reject = $false, [string]$Directory = $appDir) {
    $log = Join-Path $testRoot ('setup-' + [Guid]::NewGuid().ToString('N') + '.log')
    Invoke-CheckedProcess $Program @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/SP-',"/DIR=`"$Directory`"","/LOG=`"$log`"") $Reject
}
function Check-Preservation {
    if ([IO.File]::ReadAllText((Join-Path $appDir $userRelative)) -cne $userText -or
        [Convert]::ToHexString([IO.File]::ReadAllBytes($preferences)) -cne [Convert]::ToHexString($preferenceBytes)) { throw 'Installer changed user data or preferences' }
    & $probe --verify-fixture
    if ($LASTEXITCODE -ne 0) { throw 'Production settings reader could not restore the preserved fixture' }
}
function Check-Payload([string]$ExpectedBundle, [string]$Version) {
    if ((Get-ItemProperty -LiteralPath $registry).DisplayVersion -cne $Version) { throw 'Installed version registration mismatch' }
    if ((Get-FileHash -LiteralPath (Join-Path $appDir 'SHA256-MANIFEST.json') -Algorithm SHA256).Hash -cne
        (Get-FileHash -LiteralPath (Join-Path $ExpectedBundle 'SHA256-MANIFEST.json') -Algorithm SHA256).Hash) { throw 'Installed manifest does not match the selected installer payload' }
    if ((Get-ItemProperty -LiteralPath $registry).GCPPayloadManifestSHA256 -ine (Get-FileHash -LiteralPath (Join-Path $appDir 'SHA256-MANIFEST.json') -Algorithm SHA256).Hash) { throw 'Installed manifest fingerprint registration mismatch' }
    $script:snapshotCount++
    $snapshot = Join-Path $testRoot "snapshot-$script:snapshotCount"
    New-Item -ItemType Directory -Path $snapshot | Out-Null
    foreach ($file in Get-ChildItem -LiteralPath $appDir -Recurse -Force -File) {
        $relative = [IO.Path]::GetRelativePath($appDir,$file.FullName).Replace('\','/')
        if ($relative -in @('unins000.exe','unins000.dat',$userRelative)) { continue }
        $destination = Join-Path $snapshot $relative
        New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($destination)) -Force | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $destination
    }
    & (Join-Path $PSScriptRoot 'verify-windows-test-bundle.ps1') -BundleDirectory $snapshot
    if (-not (Test-Path -LiteralPath (Join-Path $appDir 'unins000.exe'))) { throw 'Uninstaller missing' }
    if (-not (Test-Path -LiteralPath $shortcut)) { throw 'Start menu shortcut missing' }
    $shell = New-Object -ComObject WScript.Shell
    $link = $null
    try {
        $link = $shell.CreateShortcut($shortcut)
        if ($link.TargetPath -ine (Join-Path $appDir 'gcp-desktop.exe') -or $link.WorkingDirectory -ine $appDir) { throw 'Shortcut target or working directory mismatch' }
    } finally {
        if ($link) { [Runtime.InteropServices.Marshal]::FinalReleaseComObject($link) | Out-Null }
        [Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell) | Out-Null
    }
    foreach ($name in @('msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll')) { if (-not (Test-Path -LiteralPath (Join-Path $appDir $name))) { throw 'App-local CRT missing' } }
    Check-Preservation
}
function Installed-State {
    return (@(Get-ChildItem -LiteralPath $appDir -Recurse -Force -File | Sort-Object FullName | ForEach-Object {
        [IO.Path]::GetRelativePath($appDir,$_.FullName) + ':' + (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    }) -join "`n")
}
function Reject-Unchanged([string]$Program, [string]$Directory = $appDir) {
    $state = Installed-State
    $version = (Get-ItemProperty -LiteralPath $registry).DisplayVersion
    $fingerprint = (Get-ItemProperty -LiteralPath $registry).GCPPayloadManifestSHA256
    Install $Program $true $Directory
    if ((Installed-State) -cne $state -or (Get-ItemProperty -LiteralPath $registry).DisplayVersion -cne $version -or
        (Get-ItemProperty -LiteralPath $registry).GCPPayloadManifestSHA256 -cne $fingerprint) { throw 'Rejected upgrade changed installed state' }
    Check-Preservation
}
function Uninstall {
    $payload = @(Get-Content -LiteralPath (Join-Path $appDir 'SHA256-MANIFEST.json') -Raw | ConvertFrom-Json)
    Invoke-CheckedProcess (Join-Path $appDir 'unins000.exe') @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART')
    foreach ($name in @($payload.path) + @('SHA256-MANIFEST.json')) { if (Test-Path -LiteralPath (Join-Path $appDir $name)) { throw "Payload remains after uninstall: $name" } }
    if ((Test-Path -LiteralPath $registry) -or (Test-Path -LiteralPath $shortcut)) { throw 'Registration or shortcut remains after uninstall' }
    Check-Preservation
}
try {
    $probeFolder = Join-Path $testRoot 'Preference Probe'
    New-Item -ItemType Directory -Path $probeFolder | Out-Null
    $probe = Join-Path $probeFolder 'gcp-installed-preferences-check.exe'
    Copy-Item -LiteralPath (Resolve-Path -LiteralPath $SettingsProbe).Path -Destination $probe
    Get-ChildItem -LiteralPath $BundleDirectory -Filter '*.dll' -File | Copy-Item -Destination $probeFolder
    $actualPreferences = & $probe --path
    if ($LASTEXITCODE -ne 0 -or [IO.Path]::GetFullPath(($actualPreferences -join '').Trim()) -ine [IO.Path]::GetFullPath($preferences)) { throw 'Unexpected production Qt preference location' }
    New-Item -ItemType Directory -Path (Join-Path $appDir 'User Notes') -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $appDir $userRelative),$userText)
    [IO.Directory]::CreateDirectory($profileDir) | Out-Null
    [IO.File]::WriteAllBytes($preferences,$preferenceBytes)
    Install $obsolete
    Check-Payload (Join-Path $FixtureDirectory 'obsolete') '0.0.0-qualification-fixture'
    Reject-Unchanged $installerPath
    Uninstall
    # Old-version wrappers use current binaries: this verifies Setup policy,
    # not migration from a historical published release.
    Install $compatible
    Check-Payload (Join-Path $FixtureDirectory 'compatible') '0.0.1-qualification-fixture'
    & (Join-Path $PSScriptRoot 'test-windows-runtime.ps1') -BundleDirectory $appDir
    Install $installerPath
    Check-Payload $BundleDirectory $label
    Remove-Item -LiteralPath (Join-Path $appDir 'gcp-livekit-screen.exe')
    [IO.File]::WriteAllText((Join-Path $appDir 'Qt6WebSockets.dll'),'corrupted repair fixture')
    Install $installerPath
    Check-Payload $BundleDirectory $label
    Reject-Unchanged $compatible
    Reject-Unchanged $installerPath (Join-Path $testRoot 'Different Location')
    $savedFingerprint = (Get-ItemProperty -LiteralPath $registry).GCPPayloadManifestSHA256
    try {
        Remove-ItemProperty -LiteralPath $registry -Name GCPPayloadManifestSHA256
        Reject-Unchanged $installerPath
    } finally { Set-ItemProperty -LiteralPath $registry -Name GCPPayloadManifestSHA256 -Value $savedFingerprint }
    try {
        Set-ItemProperty -LiteralPath $registry -Name DisplayVersion -Value 'unknown-fixture'
        Reject-Unchanged $installerPath
    } finally { Set-ItemProperty -LiteralPath $registry -Name DisplayVersion -Value $label }
    $manifestPath = Join-Path $appDir 'SHA256-MANIFEST.json'
    $savedManifest = [IO.File]::ReadAllBytes($manifestPath)
    try {
        foreach ($invalid in @('[]', "[`n {`"path`": `"../escape.dll`"}`n]")) {
            [IO.File]::WriteAllText($manifestPath,$invalid)
            Reject-Unchanged $installerPath
        }
    } finally { [IO.File]::WriteAllBytes($manifestPath,$savedManifest) }
    & (Join-Path $PSScriptRoot 'test-windows-runtime.ps1') -BundleDirectory $appDir
    Uninstall
    # The current release label is unchanged: this exercises a real historical
    # same-version refresh, separately from the controlled version-change fixtures.
    Install $historical
    if ((Get-FileHash -LiteralPath (Join-Path $appDir 'SHA256-MANIFEST.json') -Algorithm SHA256).Hash.ToLowerInvariant() -cne $previousRelease.manifest_sha256) { throw 'Historical installed manifest differs from pinned release' }
    Install $installerPath
    Check-Payload $BundleDirectory $label
    & (Join-Path $PSScriptRoot 'test-windows-runtime.ps1') -BundleDirectory $appDir
    Uninstall
    Install $installerPath
    Check-Payload $BundleDirectory $label
    Uninstall
} finally {
    $uninstaller = Join-Path $appDir 'unins000.exe'
    if ((Test-Path -LiteralPath $registry) -and (Test-Path -LiteralPath $uninstaller)) {
        $location = (Get-ItemProperty -LiteralPath $registry).InstallLocation
        if ($location.TrimEnd('\') -ine $appDir.TrimEnd('\')) { throw 'Refusing cleanup of an unrelated registered install' }
        Invoke-CheckedProcess $uninstaller @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART')
    }
    Remove-Item -LiteralPath $preferences -Force -ErrorAction SilentlyContinue
    if (-not $profileExisted -and (Test-Path -LiteralPath $profileDir) -and @(Get-ChildItem -LiteralPath $profileDir -Force).Count -eq 0) { Remove-Item -LiteralPath $profileDir -Force }
    Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
}
$report = [ordered]@{ schema_version = 1; commit_sha = $CommitSha; artifact_kind = 'hosted_windows_installer_policy_acceptance'
    installer_sha256 = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash.ToLowerInvariant()
    compatible_fixture_sha256 = (Get-FileHash -LiteralPath $compatible -Algorithm SHA256).Hash.ToLowerInvariant()
    obsolete_fixture_sha256 = (Get-FileHash -LiteralPath $obsolete -Algorithm SHA256).Hash.ToLowerInvariant()
    historical_installer_sha256 = $previousRelease.installer_sha256
    checks = @('fresh_current_install','older_fixture_to_current_upgrade','same_version_repair','exact_payload_and_shortcuts',
        'downgrade_rejected_unchanged','stale_owned_file_upgrade_rejected_unchanged','relocation_rejected_unchanged',
        'corrupt_unsafe_manifest_rejected_unchanged','missing_fingerprint_unknown_version_rejected_unchanged',
        'isolated_installed_runtime','user_file_and_preference_fixture_preserved','uninstall_cleanup',
        'production_settings_location_and_read','missing_and_corrupt_file_repair','pinned_published_alpha_same_version_refresh')
    historical_same_version_refresh_verified = $true
    historical_release_migration_verified = $false; clean_consumer_windows_verified = $false
    physical_media_verified = $false; publisher_signing_verified = $false; production_qualified = $false }
Write-NewJsonFile $OutputFile $report
Write-Host 'Hosted installer policy and pinned historical refresh checks passed, including real preference reads and repair. Historical cross-version migration and consumer PCs remain unqualified.'
