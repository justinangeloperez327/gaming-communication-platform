# Read-only helpers shared by the desktop and legacy process samplers. PS 5.1+.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-ProcessObservation {
    param([Parameter(Mandatory=$true)][int]$ProcessId)
    $process = Get-Process -Id $ProcessId -ErrorAction Stop
    try {
        $process.Refresh()
        [pscustomobject]@{
            pid = $process.Id
            start_utc = $process.StartTime.ToUniversalTime().ToString('o')
            cpu_seconds = $process.TotalProcessorTime.TotalSeconds
            private_bytes = $process.PrivateMemorySize64
            working_set_bytes = $process.WorkingSet64
            handles = $process.HandleCount
            threads = $process.Threads.Count
        }
    } finally { $process.Dispose() }
}

function Assert-ProcessContinuity {
    param($Before, $After)
    if ($Before.pid -ne $After.pid -or $Before.start_utc -ne $After.start_utc -or
        $After.cpu_seconds -lt $Before.cpu_seconds) {
        throw 'Process exited, restarted, or its CPU clock moved backwards; capture invalid.'
    }
}

function Get-DesktopProcessSet {
    param([int]$DesktopProcessId, [string]$Scenario, [int]$SessionId)
    # Deliberately never query CommandLine or an environment block.
    $named = @(Get-CimInstance -Query "SELECT Name,ProcessId,ParentProcessId,ExecutablePath FROM Win32_Process WHERE Name LIKE 'gcp-%.exe' AND SessionId=$SessionId")
    if ($Scenario -eq 'baseline') {
        if ($DesktopProcessId -ne 0 -or $named.Count -ne 0) { throw 'Baseline requires all GCP processes in this Windows session closed.' }
        return @()
    }
    $root = @($named | Where-Object { $_.ProcessId -eq $DesktopProcessId -and $_.Name -eq 'gcp-desktop.exe' })
    if ($root.Count -ne 1 -or -not $root[0].ExecutablePath) { throw 'Select the exact running GCP desktop PID.' }
    $directory = Split-Path -Parent $root[0].ExecutablePath
    $voice = 0; $screen = 0
    $result = foreach ($item in $named) {
        if (-not $item.ExecutablePath -or (Split-Path -Parent $item.ExecutablePath) -ne $directory) { throw 'Unrelated GCP process or unreadable executable path; capture invalid.' }
        $role = 'desktop'
        if ($item.ProcessId -ne $DesktopProcessId) {
            if ($item.ParentProcessId -ne $DesktopProcessId) { throw 'Unrelated GCP process in this Windows session; capture invalid.' }
            if ($item.Name -eq 'gcp-livekit-voice.exe') { $voice++; $role = 'voice' }
            elseif ($item.Name -eq 'gcp-livekit-screen.exe') { $screen++; $role = 'screen' }
            else { throw 'Unexpected GCP child; stop diagnostics/device scans before capture.' }
        }
        [pscustomobject]@{ pid = [int]$item.ProcessId; role = $role; name = [string]$item.Name; path = [string]$item.ExecutablePath }
    }
    $counts = @{ idle = @(0,0); voice = @(1,0); voice_publish = @(1,1); voice_view = @(1,1); voice_publish_view = @(1,2) }
    if (-not $counts.ContainsKey($Scenario) -or $voice -ne $counts[$Scenario][0] -or $screen -ne $counts[$Scenario][1]) {
        throw 'Native process counts do not match the declared scenario; start media manually before sampling.'
    }
    return @($result | Sort-Object pid)
}

function Convert-GpuSamples {
    param([object[]]$Samples, [int[]]$ProcessIds)
    $rows = [System.Collections.Generic.List[object]]::new()
    $seen = @{}
    foreach ($sample in $Samples) {
        $instance = [string]$sample.InstanceName
        if ($instance -notmatch '^pid_(\d+)_luid_(0x[0-9a-f]+_0x[0-9a-f]+)_phys_(\d+)_eng_(\d+)_engtype_(.+)$') { continue }
        $processIdValue = [int]$Matches[1]
        if ($ProcessIds -notcontains $processIdValue) { continue }
        $adapter = "$($Matches[2])_phys_$($Matches[3])".ToLowerInvariant()
        $engine = [int]$Matches[4]; $kind = $Matches[5].ToLowerInvariant()
        $key = "$processIdValue/$adapter/$engine"
        $value = [double]$sample.CookedValue
        if ($sample.Status -notin @(0,1) -or [double]::IsNaN($value) -or [double]::IsInfinity($value) -or $value -lt 0 -or $value -gt 100 -or $seen.ContainsKey($key)) {
            throw 'Invalid or duplicate PID-attributed GPU counter.'
        }
        if ($rows.Count -ge 128) { throw 'Too many GPU counter rows.' }
        $seen[$key] = $true
        $rows.Add([pscustomobject]@{ pid = $processIdValue; adapter = $adapter; engine = $engine; kind = $kind; percent = $value })
    }
    return $rows.ToArray()
}

function Get-AttributedGpuObservation {
    param([int[]]$ProcessIds, [switch]$SkipGpu)
    if ($SkipGpu) { return @{ status = 'not_requested'; engines = @() } }
    if ($ProcessIds.Count -eq 0) { return @{ status = 'not_applicable'; engines = @() } }
    try {
        # Counter names are localized. Unsupported/denied counters remain missing,
        # never zero. No elevation, network query or installation is attempted.
        $data = Get-Counter -Counter '\GPU Engine(*)\Utilization Percentage' -MaxSamples 1 -ErrorAction Stop
        $rows = @(Convert-GpuSamples -Samples $data.CounterSamples -ProcessIds $ProcessIds)
        if ($rows.Count -eq 0) { return @{ status = 'no_instances'; engines = @() } }
        return @{ status = 'ok'; engines = $rows }
    } catch { return @{ status = 'unavailable'; engines = @() } }
}

Export-ModuleMember -Function Get-ProcessObservation, Assert-ProcessContinuity, Get-DesktopProcessSet, Convert-GpuSamples, Get-AttributedGpuObservation
