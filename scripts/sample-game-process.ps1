# Windows-only, read-only GCP process sampler. No microphone or media capture.
param(
    [Parameter(Mandatory=$true)][string]$ProcessName,
    [ValidateRange(5,3600)][int]$DurationSeconds = 60,
    [ValidateRange(1,60)][int]$IntervalSeconds = 2,
    [Parameter(Mandatory=$true)][string]$OutputCsv
)
$ErrorActionPreference = 'Stop'
if ($ProcessName -notmatch '^[a-zA-Z0-9_.-]{1,80}$') { throw 'Invalid process name' }
$rows = [System.Collections.Generic.List[object]]::new()
$timer = [System.Diagnostics.Stopwatch]::StartNew()
$previous = $null
while ($timer.Elapsed.TotalSeconds -lt $DurationSeconds) {
    $processes = @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue)
    if ($processes.Count -eq 0) { throw "Process '$ProcessName' not running; benchmark invalid" }
    $cpu = ($processes | Measure-Object -Property CPU -Sum).Sum
    $private = ($processes | Measure-Object -Property PrivateMemorySize64 -Sum).Sum
    if ($null -ne $previous) {
        $delta = $timer.Elapsed.TotalSeconds - $previous.Seconds
        if ($delta -gt 0) {
            $machineCpu = 100.0 * ($cpu - $previous.Cpu) / ($delta * [Environment]::ProcessorCount)
            $rows.Add([pscustomobject]@{
                ElapsedSeconds = [math]::Round($timer.Elapsed.TotalSeconds, 3)
                ProcessCpuPercentOfMachine = [math]::Round([math]::Max(0,$machineCpu),3)
                PrivateBytes = [int64]$private
                ProcessCount = $processes.Count
            })
        }
    }
    $previous = @{ Seconds = $timer.Elapsed.TotalSeconds; Cpu = $cpu }
    Start-Sleep -Seconds $IntervalSeconds
}
if ($rows.Count -lt 2) { throw 'Insufficient sampling duration' }
$path = [System.IO.Path]::GetFullPath($OutputCsv)
$directory = [System.IO.Path]::GetDirectoryName($path)
if (-not [System.IO.Directory]::Exists($directory)) { [System.IO.Directory]::CreateDirectory($directory) | Out-Null }
$rows | Export-Csv -Path $path -NoTypeInformation -Encoding UTF8
Write-Host "Saved $($rows.Count) process samples to $path"
