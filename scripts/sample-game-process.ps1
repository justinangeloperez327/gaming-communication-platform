# Legacy single-process sampler. Prefer capture-desktop-performance.ps1 for GCP.
[CmdletBinding(DefaultParameterSetName='Name')]
param(
    [Parameter(Mandatory=$true,ParameterSetName='Name')][string]$ProcessName,
    [Parameter(Mandatory=$true,ParameterSetName='Id')][ValidateRange(1,2147483647)][int]$ProcessId,
    [ValidateRange(5,3600)][int]$DurationSeconds = 60,
    [ValidateRange(1,60)][int]$IntervalSeconds = 2,
    [Parameter(Mandatory=$true)][string]$OutputCsv
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'Windows is required.' }
if ($DurationSeconds -lt 3 * $IntervalSeconds) { throw 'Duration must allow three sample intervals.' }
Import-Module (Join-Path $PSScriptRoot 'desktop-performance.psm1') -Force
if ($PSCmdlet.ParameterSetName -eq 'Name') {
    if ($ProcessName -notmatch '^[a-zA-Z0-9_.-]{1,80}$') { throw 'Invalid process name' }
    $candidates = @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue)
    try {
        if ($candidates.Count -ne 1) { throw 'Select one exact PID; same-named processes must not be merged.' }
        $ProcessId = $candidates[0].Id
    } finally { foreach ($candidate in $candidates) { $candidate.Dispose() } }
}
$logical = [int](Get-CimInstance -Query "SELECT NumberOfLogicalProcessors FROM Win32_ComputerSystem").NumberOfLogicalProcessors
if ($logical -lt 1) { throw 'Cannot determine logical processor count.' }
$previous = Get-ProcessObservation $ProcessId
$timer = [Diagnostics.Stopwatch]::StartNew(); $previousSeconds = 0.0
$rows = [Collections.Generic.List[object]]::new()
while ($timer.Elapsed.TotalSeconds -lt $DurationSeconds) {
    Start-Sleep -Seconds $IntervalSeconds
    $current = Get-ProcessObservation $ProcessId
    Assert-ProcessContinuity $previous $current
    $seconds = $timer.Elapsed.TotalSeconds
    $cpu = 100.0 * ($current.cpu_seconds - $previous.cpu_seconds) / (($seconds - $previousSeconds) * $logical)
    if ([double]::IsNaN($cpu) -or [double]::IsInfinity($cpu) -or $cpu -lt 0 -or $cpu -gt 100.1) { throw 'Invalid process CPU sample.' }
    $rows.Add([pscustomobject]@{
        ElapsedSeconds = $seconds.ToString('R',[Globalization.CultureInfo]::InvariantCulture)
        ProcessCpuPercentOfMachine = $cpu.ToString('R',[Globalization.CultureInfo]::InvariantCulture)
        PrivateBytes = $current.private_bytes; ProcessCount = 1
        ProcessId = $current.pid; StartTimeUtc = $current.start_utc
    })
    $previous = $current; $previousSeconds = $seconds
}
$path = [IO.Path]::GetFullPath($OutputCsv)
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
# No existing capture is overwritten; a restart/exit produces no result CSV.
$stream = [IO.File]::Open($path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
$writer = [IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false))
try { foreach ($line in ($rows | ConvertTo-Csv -NoTypeInformation)) { $writer.WriteLine($line) } }
finally { $writer.Dispose() }
Write-Host "Saved $($rows.Count) samples for one pinned process; not whole-application qualification."
