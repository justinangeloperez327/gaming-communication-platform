# Validate that CI-only package acceptance never claims physical production qualification.
param(
    [Parameter(Mandatory=$true)][string]$RecordFile,
    [Parameter(Mandatory=$true)][string]$ExpectedCommit
)
$ErrorActionPreference = 'Stop'
if ($ExpectedCommit -notmatch '^[a-fA-F0-9]{40}$') { throw 'Invalid expected SHA' }
$report = Get-Content -LiteralPath $RecordFile -Raw | ConvertFrom-Json
if ($report.schema_version -ne 1) { throw 'Unknown acceptance schema' }
if ($report.commit_sha -cne $ExpectedCommit.ToLowerInvariant()) { throw 'Commit SHA mismatch' }
if ($report.artifact_kind -cne 'unsigned_windows_test_bundle') { throw 'Unexpected artifact kind' }
if ($report.production_qualified -cne $false) { throw 'CI must never assert production qualification' }
$checks = @('native_windows_build','ctest','bundle_sha256_integrity','packaged_qml_smoke','packaged_livekit_self_test')
foreach ($name in $checks) {
    $value = [string]$report.automated_checks.$name
    if ($value -notin @('passed','passed_in_preceding_ci_step')) { throw "Automated check missing: $name" }
}
$unverified = @('clean_windows_machine','two_participant_real_audio',
    'microphone_privacy_hardware','gaming_cpu_gpu_ram_fps','credential_revocation','code_signing')
foreach ($name in $unverified) {
    $value = [string]$report.release_checks.$name
    if ($value -notin @('not_tested','not_completed')) { throw "Unsupported release claim: $name" }
}
Write-Host 'Unsigned Windows acceptance record validated; physical release gates remain unverified.'
