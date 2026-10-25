# Collect Windows GPU Video Encode engine utilization, without screen/audio
# content or LiveKit credentials. Run locally during a real test session.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$Output,

    [ValidateRange(10, 120)]
    [int]$Seconds = 30
)

$ErrorActionPreference = "Stop"
if (-not $IsWindows -and $PSVersionTable.PSEdition -eq "Core") {
    throw "GPU Engine counters require Windows."
}
$parent = Split-Path -Parent $Output
if ($parent -and -not (Test-Path -LiteralPath $parent -PathType Container)) {
    throw "Output parent directory does not exist: $parent"
}
$counterPath = '\GPU Engine(*)\Utilization Percentage'
try {
    $records = @(Get-Counter -Counter $counterPath -SampleInterval 1 -MaxSamples $Seconds)
} catch {
    throw "GPU Engine counters are unavailable. Check Windows driver/counter support: $_"
}
$rows = foreach ($sample in $records) {
    foreach ($counter in $sample.CounterSamples) {
        if ($counter.Path -match 'engtype_VideoEncode') {
            if ([double]::IsNaN([double]$counter.CookedValue) -or [double]::IsInfinity([double]$counter.CookedValue)) {
                throw "A GPU Video Encode counter returned a nonfinite value."
            }
            [PSCustomObject]@{
                TimestampUtc = $sample.Timestamp.ToUniversalTime().ToString("o")
                GpuEnginePath = [string]$counter.Path
                UtilizationPercent = [Math]::Round([double]$counter.CookedValue, 4)
            }
        }
    }
}
if (@($rows).Count -eq 0) {
    throw "No GPU Video Encode samples were returned; no file written."
}
$rows | Export-Csv -LiteralPath $Output -NoTypeInformation -Encoding utf8
Write-Host "Wrote $(@($rows).Count) GPU Video Encode rows to $Output."
Write-Host "Counter activity alone does NOT prove that the GCP encoder used the GPU."
