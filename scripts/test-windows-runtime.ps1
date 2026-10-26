# Exercise a payload with developer search paths removed, without devices/network.
#requires -Version 7.0
param([Parameter(Mandatory=$true)][string]$BundleDirectory,
      [switch]$CheckMissingTlsRejection)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($BundleDirectory)
$keys = @('PATH','QT_PLUGIN_PATH','QT_QPA_PLATFORM_PLUGIN_PATH','QML2_IMPORT_PATH','QML_IMPORT_PATH',
    'QT_QUICK_CONTROLS_STYLE','QT_QUICK_CONTROLS_CONF','QT_QPA_PLATFORM','QT_FORCE_STDERR_LOGGING','QT_DEBUG_PLUGINS')
$saved = @{}
foreach ($key in $keys) { $saved[$key] = [Environment]::GetEnvironmentVariable($key, 'Process') }
function Invoke-RuntimeProbe([string]$Name, [string]$Argument, [bool]$MissingTls = $false) {
    $program = Join-Path $root $Name
    if (-not (Test-Path -LiteralPath $program -PathType Leaf)) { throw "Runtime missing: $Name" }
    $logs = Join-Path ([IO.Path]::GetTempPath()) ('gcp-runtime-' + [Guid]::NewGuid().ToString('N'))
    $child = $null
    try {
        $child = Start-Process -FilePath $program -ArgumentList $Argument -WorkingDirectory $root -PassThru -RedirectStandardOutput "$logs.out" -RedirectStandardError "$logs.err"
        $child | Wait-Process -Timeout 30 -ErrorAction Stop
        $child.Refresh()
        $stderr = [IO.File]::ReadAllText("$logs.err")
        if ($MissingTls) {
            if ($child.ExitCode -ne 1 -or -not $stderr.Contains('GCP Qt TLS backend unavailable')) { throw 'Missing TLS plugin did not produce the specific rejected startup outcome' }
        } elseif ($null -eq $child.ExitCode -or $child.ExitCode -ne 0) {
            throw "Runtime startup failed: $Name ($($child.ExitCode))"
        } elseif ($Name -eq 'gcp-desktop.exe' -and -not $stderr.Contains('GCP Qt TLS backend:')) {
            throw 'Desktop did not report an actual TLS backend'
        }
        foreach ($line in $stderr.Split("`n") | Where-Object { $_.StartsWith('GCP Qt TLS backend') }) { Write-Host $line.Trim() }
    } catch {
        foreach ($log in @("$logs.out", "$logs.err")) { if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Tail 60 | Write-Host } }
        throw
    } finally {
        if ($child) { if (-not $child.HasExited) { $child.Kill($true); $child.WaitForExit(5000) | Out-Null }; $child.Dispose() }
        Remove-Item -LiteralPath "$logs.out", "$logs.err" -Force -ErrorAction SilentlyContinue
    }
}
try {
    foreach ($key in $keys) { [Environment]::SetEnvironmentVariable($key, $null, 'Process') }
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    $env:QT_QPA_PLATFORM = 'offscreen'
    $env:QT_FORCE_STDERR_LOGGING = '1'
    $env:QT_DEBUG_PLUGINS = '1'
    Invoke-RuntimeProbe 'gcp-desktop.exe' '--smoke-qml'
    Invoke-RuntimeProbe 'gcp-livekit-voice.exe' '--self-test'
    Invoke-RuntimeProbe 'gcp-livekit-screen.exe' '--self-test'
    if ($CheckMissingTlsRejection) {
        $tls = Join-Path $root 'tls'
        if (-not (Test-Path -LiteralPath $tls -PathType Container)) { throw 'Expected packaged Qt TLS plugins' }
        $backup = Join-Path ([IO.Path]::GetTempPath()) ('gcp-tls-plugins-' + [Guid]::NewGuid().ToString('N'))
        Move-Item -LiteralPath $tls -Destination $backup
        try { Invoke-RuntimeProbe 'gcp-desktop.exe' '--smoke-qml' $true }
        finally { Move-Item -LiteralPath $backup -Destination $tls }
        & (Join-Path $PSScriptRoot 'verify-windows-test-bundle.ps1') -BundleDirectory $root
    }
    Write-Host 'Qt/QML/TLS and native no-device startup passed with isolated runtime search paths; no clean-PC or physical media claim.'
} finally {
    foreach ($key in $keys) { [Environment]::SetEnvironmentVariable($key, $saved[$key], 'Process') }
}
