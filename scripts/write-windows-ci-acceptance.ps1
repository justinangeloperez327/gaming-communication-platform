# Write a factual acceptance record only after successful CI packaging and runtime checks.
param(
    [Parameter(Mandatory=$true)][string]$BundleDirectory,
    [Parameter(Mandatory=$true)][string]$OutputFile,
    [Parameter(Mandatory=$true)][string]$CommitSha
)
$ErrorActionPreference = 'Stop'
if ($CommitSha -notmatch '^[a-fA-F0-9]{40}$') { throw 'Expected full Git commit SHA' }
$root = [IO.Path]::GetFullPath($BundleDirectory)
$manifest = Join-Path $root 'SHA256-MANIFEST.json'
if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) { throw 'Bundle manifest missing' }
# Re-run the verifier to ensure the artifact still matches at record creation.
& (Join-Path $PSScriptRoot 'verify-windows-test-bundle.ps1') -BundleDirectory $root
$record = [ordered]@{
    schema_version = 1
    commit_sha = $CommitSha.ToLowerInvariant()
    timestamp_utc = [DateTimeOffset]::UtcNow.ToString('o')
    artifact_kind = 'unsigned_windows_test_bundle'
    automated_checks = [ordered]@{
        native_windows_build = 'passed_in_preceding_ci_step'
        ctest = 'passed_in_preceding_ci_step'
        bundle_sha256_integrity = 'passed'
        packaged_qml_smoke = 'passed_in_preceding_ci_step'
        packaged_livekit_self_test = 'passed_in_preceding_ci_step'
    }
    release_checks = [ordered]@{
        clean_windows_machine = 'not_tested'
        two_participant_real_audio = 'not_tested'
        microphone_privacy_hardware = 'not_tested'
        gaming_cpu_gpu_ram_fps = 'not_tested'
        credential_revocation = 'not_tested'
        code_signing = 'not_completed'
    }
    production_qualified = $false
}
$path = [IO.Path]::GetFullPath($OutputFile)
$folder = [IO.Path]::GetDirectoryName($path)
if (-not (Test-Path -LiteralPath $folder)) { New-Item -ItemType Directory -Path $folder -Force | Out-Null }
$record | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $path -Encoding utf8
Write-Host "CI-only package acceptance report written to $path (not production qualified)."
