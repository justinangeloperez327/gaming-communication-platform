# Exercise a staged, extracted or installed payload with developer search paths removed.
#requires -Version 7.0
param([Parameter(Mandatory=$true)][string]$BundleDirectory)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($BundleDirectory)
$keys = @('PATH','QT_PLUGIN_PATH','QT_QPA_PLATFORM_PLUGIN_PATH','QML2_IMPORT_PATH','QML_IMPORT_PATH',
    'QT_QUICK_CONTROLS_STYLE','QT_QUICK_CONTROLS_CONF','QT_QPA_PLATFORM','QT_FORCE_STDERR_LOGGING','QT_DEBUG_PLUGINS')
$saved = @{}
foreach ($key in $keys) { $saved[$key] = [Environment]::GetEnvironmentVariable($key, 'Process') }
try {
    foreach ($key in $keys) { [Environment]::SetEnvironmentVariable($key, $null, 'Process') }
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    $env:QT_QPA_PLATFORM = 'offscreen'
    $env:QT_FORCE_STDERR_LOGGING = '1'
    $env:QT_DEBUG_PLUGINS = '1'
    foreach ($test in @(@('gcp-desktop.exe','--smoke-qml'), @('gcp-livekit-voice.exe','--self-test'), @('gcp-livekit-screen.exe','--self-test'))) {
        $program = Join-Path $root $test[0]
        if (-not (Test-Path -LiteralPath $program -PathType Leaf)) { throw "Runtime missing: $($test[0])" }
        $logs = Join-Path ([IO.Path]::GetTempPath()) ('gcp-runtime-' + [Guid]::NewGuid().ToString('N'))
        $child = $null
        try {
            $child = Start-Process -FilePath $program -ArgumentList $test[1] -WorkingDirectory $root -PassThru -RedirectStandardOutput "$logs.out" -RedirectStandardError "$logs.err"
            $child | Wait-Process -Timeout 30 -ErrorAction Stop
            $child.Refresh()
            if ($null -eq $child.ExitCode -or $child.ExitCode -ne 0) { throw "Runtime startup failed: $($test[0]) ($($child.ExitCode))" }
        } catch {
            foreach ($log in @("$logs.out", "$logs.err")) { if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Tail 60 | Write-Host } }
            throw
        } finally {
            if ($child) { if (-not $child.HasExited) { $child.Kill($true); $child.WaitForExit(5000) | Out-Null }; $child.Dispose() }
            Remove-Item -LiteralPath "$logs.out", "$logs.err" -Force -ErrorAction SilentlyContinue
        }
    }
    Write-Host 'Qt/QML and native no-device startup passed with isolated runtime search paths; no clean-PC or physical media claim.'
} finally {
    foreach ($key in $keys) { [Environment]::SetEnvironmentVariable($key, $saved[$key], 'Process') }
}
