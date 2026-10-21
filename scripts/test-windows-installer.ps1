param([Parameter(Mandatory=$true)][string]$Installer)
$ErrorActionPreference = 'Stop'
$registry = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{F6CA3831-848C-4148-831E-804B1F6DC06B}_is1'
if (Test-Path -LiteralPath $registry) { throw 'Use a disposable account without an existing GCP Alpha install' }
$installerPath = (Resolve-Path -LiteralPath $Installer).Path
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('gcp-installer-' + [Guid]::NewGuid().ToString('N'))
$appDir = Join-Path $testRoot 'Application With Spaces'
New-Item -ItemType Directory -Path $testRoot | Out-Null
function Invoke-CheckedProcess([string]$Program, [string[]]$Arguments, [int]$Timeout = 90) {
    $logBase = Join-Path $testRoot ([Guid]::NewGuid().ToString('N'))
    $child = Start-Process -FilePath $Program -ArgumentList $Arguments -WorkingDirectory (Split-Path $Program) -RedirectStandardOutput "$logBase.stdout" -RedirectStandardError "$logBase.stderr" -PassThru
    try {
        $child | Wait-Process -Timeout $Timeout -ErrorAction Stop
        $child.Refresh()
        if ($null -eq $child.ExitCode -or $child.ExitCode -ne 0) { throw "Process failed: $Program ($($child.ExitCode))" }
    } catch {
        foreach ($log in @("$logBase.stdout", "$logBase.stderr")) {
            if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Tail 80 | Write-Host }
        }
        throw
    } finally {
        if (-not $child.HasExited) { Stop-Process -Id $child.Id -Force -ErrorAction SilentlyContinue }
        $child.Dispose()
    }
}
function Test-InstalledPayload([string]$Snapshot) {
    New-Item -ItemType Directory -Path $Snapshot | Out-Null
    foreach ($file in Get-ChildItem -LiteralPath $appDir -Recurse -Force -File) {
        $relative = [IO.Path]::GetRelativePath($appDir, $file.FullName)
        # Only Inno's exact uninstall pair is outside the original bundle manifest.
        if ($relative -in @('unins000.exe', 'unins000.dat')) { continue }
        $destination = Join-Path $Snapshot $relative
        New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($destination)) -Force | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $destination
    }
    & (Join-Path $PSScriptRoot 'verify-windows-test-bundle.ps1') -BundleDirectory $Snapshot
    if (-not (Test-Path -LiteralPath (Join-Path $appDir 'unins000.exe'))) { throw 'Uninstaller missing' }
}
try {
    # Fresh install and same-version repair must preserve the exact packaged payload.
    foreach ($iteration in 1..2) {
        Invoke-CheckedProcess $installerPath @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', "/DIR=`"$appDir`"", "/LOG=`"$testRoot/install-$iteration.log`"")
        Test-InstalledPayload (Join-Path $testRoot "snapshot-$iteration")
    }
    if (-not (Test-Path -LiteralPath $registry)) { throw 'Per-user uninstall registration missing' }
    $shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'Gaming Communication Platform Alpha/Gaming Communication Platform Alpha.lnk'
    if (-not (Test-Path -LiteralPath $shortcut)) { throw 'Start menu shortcut missing' }
    $payload = @(Get-Content -LiteralPath (Join-Path $appDir 'SHA256-MANIFEST.json') -Raw | ConvertFrom-Json)
    foreach ($runtime in @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
        if (-not (Test-Path -LiteralPath (Join-Path $appDir $runtime) -PathType Leaf)) { throw "App-local CRT missing: $runtime" }
    }
    # Avoid resolving Qt imports/plugins or compiler DLLs through build-tool paths.
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    Remove-Item Env:QT_PLUGIN_PATH, Env:QML2_IMPORT_PATH, Env:QML_IMPORT_PATH -ErrorAction SilentlyContinue
    $env:QT_QPA_PLATFORM = 'offscreen'
    $env:QT_FORCE_STDERR_LOGGING = '1'
    $env:QT_DEBUG_PLUGINS = '1'
    Invoke-CheckedProcess (Join-Path $appDir 'gcp-desktop.exe') @('--smoke-qml') 20
    Invoke-CheckedProcess (Join-Path $appDir 'gcp-livekit-voice.exe') @('--self-test') 20
    # User-created files must survive uninstall; remove only installed payload files.
    Set-Content -LiteralPath (Join-Path $appDir 'user-keep.txt') -Value 'preserve'
    Invoke-CheckedProcess (Join-Path $appDir 'unins000.exe') @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=`"$testRoot/uninstall.log`"")
    if (Test-Path -LiteralPath (Join-Path $appDir 'gcp-desktop.exe')) { throw 'Installed executable remains after uninstall' }
    foreach ($entry in $payload) {
        if (Test-Path -LiteralPath (Join-Path $appDir $entry.path)) { throw "Payload remains after uninstall: $($entry.path)" }
    }
    if (Test-Path -LiteralPath $shortcut) { throw 'Start menu shortcut remains after uninstall' }
    if (Test-Path -LiteralPath $registry) { throw 'Uninstall registration remains' }
    if (-not (Test-Path -LiteralPath (Join-Path $appDir 'user-keep.txt'))) { throw 'Uninstaller removed user file' }
    Write-Host 'Installer install, repair, payload, runtime and uninstall checks passed on hosted Windows.'
} finally {
    # Retry cleanup after test failures; never install on the developer machine by default.
    $uninstaller = Join-Path $appDir 'unins000.exe'
    # Inno can leave its executable briefly while arranging self-deletion.
    # Only retry while registration still says an installation exists.
    if ((Test-Path -LiteralPath $registry) -and (Test-Path -LiteralPath $uninstaller)) {
        Invoke-CheckedProcess $uninstaller @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')
    }
    Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
}
