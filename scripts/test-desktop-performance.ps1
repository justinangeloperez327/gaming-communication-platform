# Policy fixtures and a real Windows process-only smoke. No game/media/capture.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'desktop-performance.psm1') -Force
function Require($condition, [string]$message) { if (-not $condition) { throw $message } }
function MustFail([scriptblock]$action) { $failed = $false; try { & $action } catch { $failed = $true }; Require $failed 'Expected fail-closed rejection.' }
$before = [pscustomobject]@{ pid = 101; start_utc = '2026-01-01T00:00:00Z'; cpu_seconds = 1.0 }
Assert-ProcessContinuity $before $before
MustFail { Assert-ProcessContinuity $before ([pscustomobject]@{pid=102;start_utc=$before.start_utc;cpu_seconds=2}) }
MustFail { Assert-ProcessContinuity $before ([pscustomobject]@{pid=101;start_utc='2026-01-02T00:00:00Z';cpu_seconds=2}) }
MustFail { Assert-ProcessContinuity $before ([pscustomobject]@{pid=101;start_utc=$before.start_utc;cpu_seconds=0}) }
$samples = @(
    [pscustomobject]@{ InstanceName = 'pid_101_luid_0x00000000_0x00000123_phys_0_eng_0_engtype_3D'; Status=0; CookedValue=25.0 },
    [pscustomobject]@{ InstanceName = 'pid_999_luid_0x00000000_0x00000123_phys_0_eng_0_engtype_3D'; Status=0; CookedValue=99.0 }
)
$gpu = @(Convert-GpuSamples -Samples $samples -ProcessIds @(101))
Require ($gpu.Count -eq 1 -and $gpu[0].percent -eq 25 -and $gpu[0].pid -eq 101) 'GPU attribution mixed unrelated PIDs.'
MustFail { Convert-GpuSamples -Samples @($samples[0],$samples[0]) -ProcessIds @(101) }
$samples[0].CookedValue = [double]::NaN
MustFail { Convert-GpuSamples -Samples $samples -ProcessIds @(101) }
$samples[0].CookedValue = 101
MustFail { Convert-GpuSamples -Samples $samples -ProcessIds @(101) }
$own = Get-ProcessObservation $PID
Require ($own.pid -eq $PID -and $own.private_bytes -gt 0 -and $own.working_set_bytes -gt 0) 'Real process observation failed.'
Assert-ProcessContinuity $own (Get-ProcessObservation $PID)

$folder = Join-Path ([IO.Path]::GetTempPath()) ('gcp-perf-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($folder) | Out-Null
$started = [Collections.Generic.List[object]]::new()
try {
    # Built-in Windows .NET Framework compiler; fixture binaries are never shipped.
    $compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    Require (Test-Path -LiteralPath $compiler) 'Windows fixture compiler is missing.'
    $source = @'
using System; using System.Diagnostics; using System.IO; using System.Threading;
class Fixture {
 static void Main(string[] args) {
  if(args.Length > 0 && args[0] == "desktop") {
   using(var child = Process.Start(new ProcessStartInfo(Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"gcp-livekit-voice.exe"),"sleep") { UseShellExecute=false })) {
    Thread.Sleep(30000); if(!child.HasExited) child.Kill();
   }
  } else Thread.Sleep(30000);
 }
}
'@
    $sourcePath = Join-Path $folder 'Fixture.cs'; [IO.File]::WriteAllText($sourcePath,$source)
    $desktopPath = Join-Path $folder 'gcp-desktop.exe'
    & $compiler /nologo "/out:$desktopPath" $sourcePath
    Require ($LASTEXITCODE -eq 0) 'Fixture compile failed.'
    Copy-Item -LiteralPath $desktopPath -Destination (Join-Path $folder 'gcp-livekit-voice.exe')
    Copy-Item -LiteralPath $desktopPath -Destination (Join-Path $folder 'fixture-game.exe')
    $desktop = Start-Process -FilePath $desktopPath -ArgumentList 'desktop' -PassThru; $started.Add($desktop)
    $game = Start-Process -FilePath (Join-Path $folder 'fixture-game.exe') -PassThru; $started.Add($game)
    $children = @(); $deadline = [Diagnostics.Stopwatch]::StartNew()
    while ($children.Count -eq 0 -and $deadline.Elapsed.TotalSeconds -lt 5) {
        $children = @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$($desktop.Id) AND Name='gcp-livekit-voice.exe'")
        if ($children.Count -eq 0) { Start-Sleep -Milliseconds 50 }
    }
    Require ($children.Count -eq 1) 'Fixture voice child did not start.'
    $child = Get-Process -Id $children[0].ProcessId; $started.Add($child)
    $output = Join-Path $folder 'capture.jsonl'
    & (Join-Path $PSScriptRoot 'capture-desktop-performance.ps1') -GameProcessId $game.Id -DesktopProcessId $desktop.Id -Scenario voice -DurationSeconds 5 -IntervalSeconds 1 -SkipGpu -OutputFile $output
    $records = @(Get-Content -LiteralPath $output | ForEach-Object { $_ | ConvertFrom-Json })
    Require ($records[0].processes.Count -eq 2 -and $records[-1].complete -and $records[-1].samples -ge 3) 'Process-only smoke did not capture complete desktop/native set.'
    Require ($records[1].gpu.status -eq 'not_requested') 'Unavailable GPU was invented as zero.'
    MustFail { & (Join-Path $PSScriptRoot 'capture-desktop-performance.ps1') -GameProcessId $game.Id -DesktopProcessId $desktop.Id -Scenario voice -DurationSeconds 5 -IntervalSeconds 1 -SkipGpu -OutputFile $output }
    MustFail { Get-DesktopProcessSet $desktop.Id baseline $desktop.SessionId }
    MustFail { Get-DesktopProcessSet $desktop.Id voice_view $desktop.SessionId }
    # Legacy CLI must reject ambiguity, pin a single PID and retain identity.
    $secondGame = Start-Process -FilePath (Join-Path $folder 'fixture-game.exe') -PassThru; $started.Add($secondGame)
    MustFail { & (Join-Path $PSScriptRoot 'sample-game-process.ps1') -ProcessName 'fixture-game' -DurationSeconds 5 -IntervalSeconds 1 -OutputCsv (Join-Path $folder 'ambiguous.csv') }
    $csv = Join-Path $folder 'process.csv'
    & (Join-Path $PSScriptRoot 'sample-game-process.ps1') -ProcessId $game.Id -DurationSeconds 5 -IntervalSeconds 1 -OutputCsv $csv
    $legacy = @(Import-Csv -LiteralPath $csv)
    Require ($legacy.Count -ge 3 -and $legacy[0].ProcessId -eq [string]$game.Id -and $legacy[0].StartTimeUtc) 'Legacy process identity missing.'
    $badOutput = Join-Path $folder 'interrupted.jsonl'
    $shellPath = (Get-Process -Id $PID).Path
    $captureScript = Join-Path $PSScriptRoot 'capture-desktop-performance.ps1'
    $captureArgs = @('-NoProfile','-File',('"'+$captureScript+'"'),'-GameProcessId',[string]$game.Id,'-DesktopProcessId',[string]$desktop.Id,'-Scenario','voice','-DurationSeconds','20','-IntervalSeconds','1','-SkipGpu','-OutputFile',('"'+$badOutput+'"'))
    $interrupted = Start-Process -FilePath $shellPath -ArgumentList $captureArgs -PassThru -RedirectStandardError (Join-Path $folder 'capture-error.txt') -RedirectStandardOutput (Join-Path $folder 'capture-output.txt'); $started.Add($interrupted)
    $wait = [Diagnostics.Stopwatch]::StartNew(); $recordCount = 0
    while ($recordCount -lt 3 -and $wait.Elapsed.TotalSeconds -lt 10) {
        if (Test-Path -LiteralPath $badOutput) { $recordCount = @(Get-Content -LiteralPath $badOutput).Count }
        if ($recordCount -lt 3) { Start-Sleep -Milliseconds 100 }
    }
    Require ($recordCount -ge 3) 'Interrupted capture did not reach live process sampling.'
    Stop-Process -Id $child.Id -Force; $child.WaitForExit(3000) | Out-Null
    Require ($interrupted.WaitForExit(7000)) 'Interrupted collector did not terminate.'
    $interrupted.Refresh()
    $lastRecord = (Get-Content -LiteralPath $badOutput | Select-Object -Last 1) | ConvertFrom-Json
    Require ($interrupted.ExitCode -ne 0 -and $lastRecord.complete -eq $false) 'Process loss was accepted as complete evidence.'
    MustFail { Get-ProcessObservation $child.Id }
    Write-Host 'PowerShell identity/GPU policy and real Windows process-only collector smoke passed. No gaming or physical media qualification.'
} finally {
    foreach ($process in $started) {
        try { $process.Refresh(); if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit(3000) | Out-Null } } catch {}
        $process.Dispose()
    }
    Remove-Item -LiteralPath $folder -Recurse -Force -ErrorAction SilentlyContinue
}
