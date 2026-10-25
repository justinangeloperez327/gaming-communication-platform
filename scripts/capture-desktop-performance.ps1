# Explicit, read-only Windows process/counter observation. Never starts media.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateRange(1,2147483647)][int]$GameProcessId,
    [ValidateRange(0,2147483647)][int]$DesktopProcessId = 0,
    [Parameter(Mandatory=$true)][ValidateSet('baseline','idle','voice','voice_publish','voice_view','voice_publish_view')][string]$Scenario,
    [ValidateRange(5,3600)][int]$DurationSeconds = 120,
    [ValidateRange(1,10)][int]$IntervalSeconds = 1,
    [Parameter(Mandatory=$true)][string]$OutputFile,
    [switch]$SkipGpu
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'Windows is required.' }
if ($DurationSeconds -lt 3 * $IntervalSeconds) { throw 'Duration must allow at least three sample intervals.' }
if (-not [Diagnostics.Stopwatch]::IsHighResolution) { throw 'QPC clock is required for frame-time alignment.' }
Import-Module (Join-Path $PSScriptRoot 'desktop-performance.psm1') -Force
$sessionIdValue = (Get-Process -Id $PID).SessionId
$set = @(Get-DesktopProcessSet $DesktopProcessId $Scenario $sessionIdValue)
$appIds = @($set | ForEach-Object { $_.pid })
if ($appIds -contains $GameProcessId -or $GameProcessId -eq $PID) { throw 'Game PID must be distinct from GCP and the observer.' }
$game = Get-ProcessObservation $GameProcessId
$gameProcess = Get-Process -Id $GameProcessId
try { $gameName = $gameProcess.ProcessName; $gameHash = (Get-FileHash -LiteralPath $gameProcess.Path -Algorithm SHA256).Hash.ToLowerInvariant() }
finally { $gameProcess.Dispose() }
$initial = @{}; $metadata = @()
foreach ($item in $set) {
    $observation = Get-ProcessObservation $item.pid; $initial[$item.pid] = $observation
    $metadata += @{ pid = $item.pid; role = $item.role; name = $item.name; start_utc = $observation.start_utc; sha256 = (Get-FileHash -LiteralPath $item.path -Algorithm SHA256).Hash.ToLowerInvariant() }
}
$system = Get-CimInstance -Query "SELECT NumberOfLogicalProcessors,TotalPhysicalMemory FROM Win32_ComputerSystem"
$osInfo = Get-CimInstance -Query "SELECT LastBootUpTime,BuildNumber FROM Win32_OperatingSystem"
$cpuNames = @((Get-CimInstance -Query "SELECT Name FROM Win32_Processor").Name | Sort-Object)
$gpus = @(Get-CimInstance -Query "SELECT Name,DriverVersion FROM Win32_VideoController" | ForEach-Object { @{ name = $_.Name; driver = $_.DriverVersion } })
$sha = [Security.Cryptography.SHA256]::Create()
try { $boot = ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes([Environment]::MachineName + '|' + $osInfo.LastBootUpTime.ToUniversalTime().ToString('o'))))).Replace('-','').ToLowerInvariant() }
finally { $sha.Dispose() }
$logical = [int]$system.NumberOfLogicalProcessors
if ($logical -lt 1 -or $logical -gt 1024) { throw 'Invalid logical processor count.' }
$path = [IO.Path]::GetFullPath($OutputFile)
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
# CreateNew rejects old captures and partial files instead of overwriting evidence.
$stream = [IO.File]::Open($path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
$writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false))
$bytesWritten = 0
function Write-Record($value) {
    $line = ConvertTo-Json -InputObject $value -Compress -Depth 10
    $size = [Text.Encoding]::UTF8.GetByteCount($line)
    if ($size -gt 65536 -or $script:bytesWritten + $size -gt 67108864) { throw 'Capture storage limit exceeded.' }
    $script:bytesWritten += $size + 1
    $writer.WriteLine($line); $writer.Flush()
}
$observer = Get-ProcessObservation $PID
$observerPeak = $observer.private_bytes
$start = [Diagnostics.Stopwatch]::GetTimestamp(); $count = 0; $last = $start
try {
    Write-Record @{ type = 'header'; schema = 1; collector = 'gcp-desktop-performance'; scenario = $Scenario; started_utc = [DateTime]::UtcNow.ToString('o'); boot_fingerprint = $boot;
        qpc_frequency = [Diagnostics.Stopwatch]::Frequency; interval_seconds = $IntervalSeconds; requested_seconds = $DurationSeconds; skip_gpu = [bool]$SkipGpu;
        hardware = @{ cpu = $cpuNames; gpu = $gpus; logical_processors = $logical; ram_bytes = [long]$system.TotalPhysicalMemory; windows_build = [string]$osInfo.BuildNumber };
        game = @{ pid = $game.pid; start_utc = $game.start_utc; name = $gameName; sha256 = $gameHash }; processes = @($metadata) }
    while ($true) {
        $iterationStart = [Diagnostics.Stopwatch]::GetTimestamp()
        $current = @(Get-DesktopProcessSet $DesktopProcessId $Scenario $sessionIdValue)
        if (($appIds -join ',') -ne ((@($current | ForEach-Object { $_.pid })) -join ',')) { throw 'GCP process set changed; capture invalid.' }
        $currentGame = Get-ProcessObservation $GameProcessId; Assert-ProcessContinuity $game $currentGame
        $observations = @()
        foreach ($item in $current) {
            $next = Get-ProcessObservation $item.pid; Assert-ProcessContinuity $initial[$item.pid] $next
            $initial[$item.pid] = $next; $observations += $next
        }
        $gpu = Get-AttributedGpuObservation -ProcessIds $appIds -SkipGpu:$SkipGpu
        $last = [Diagnostics.Stopwatch]::GetTimestamp()
        Write-Record @{ type = 'sample'; index = $count; qpc = $last; game = @{ pid = $currentGame.pid; start_utc = $currentGame.start_utc }; processes = @($observations); gpu = $gpu }
        $count++
        $observerNow = Get-ProcessObservation $PID; $observerPeak = [Math]::Max($observerPeak, $observerNow.private_bytes)
        if (($last - $start) / [Diagnostics.Stopwatch]::Frequency -ge $DurationSeconds) { break }
        $remainingMs = $IntervalSeconds * 1000 - 1000 * ([Diagnostics.Stopwatch]::GetTimestamp() - $iterationStart) / [double][Diagnostics.Stopwatch]::Frequency
        if ($remainingMs -gt 0) { Start-Sleep -Milliseconds ([int][Math]::Ceiling($remainingMs)) }
    }
    # A change immediately after the final sample also invalidates the capture.
    $finalSet = @(Get-DesktopProcessSet $DesktopProcessId $Scenario $sessionIdValue)
    if (($appIds -join ',') -ne ((@($finalSet | ForEach-Object { $_.pid })) -join ',')) { throw 'GCP process set changed at completion.' }
    foreach ($item in $set) {
        Assert-ProcessContinuity $initial[$item.pid] (Get-ProcessObservation $item.pid)
        $expected = @($metadata | Where-Object { $_.pid -eq $item.pid })[0].sha256
        if ((Get-FileHash -LiteralPath $item.path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) { throw 'GCP executable changed during capture.' }
    }
    Assert-ProcessContinuity $game (Get-ProcessObservation $GameProcessId)
    $elapsed = ($last - $start) / [double][Diagnostics.Stopwatch]::Frequency
    Write-Record @{ type = 'end'; complete = $true; samples = $count; qpc = $last; observer = @{ cpu_percent_machine = 100 * ($observerNow.cpu_seconds - $observer.cpu_seconds) / ($elapsed * $logical); peak_private_bytes_sampled = $observerPeak } }
} catch {
    try { Write-Record @{ type = 'end'; complete = $false; samples = $count; reason = 'capture_failed_or_process_changed' } } catch {}
    throw
} finally { $writer.Dispose() }
Write-Host "Saved $count process/counter samples. This is not physical media or gaming qualification: $path"
