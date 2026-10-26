# Group 12: Windows distribution qualification

Group 12 integrates the completed Groups 7–11 into a PR targeting `main` and
hardens the existing portable ZIP and per-user alpha installer. It preserves
the Qt/Rust/LiveKit architecture and the requested alpha version. No service,
release, certificate or installer technology is introduced.

## What the checks establish

| Check | Evidence and boundary |
| --- | --- |
| Executable provenance | Every deployed EXE/DLL has an AMD64 PE header and a hash matching a known build input: a GCP executable, the pinned LiveKit release archive, the selected VS x64 CRT, or the installed Qt SDK. Unknown or changed inputs fail staging. This does not authenticate a manually supplied Qt installation. |
| Build inventory | `DISTRIBUTION-INVENTORY.json` binds the source commit, executable hashes/versions/origins, pinned SDK archive and available upstream evidence. All product-acceptance flags remain false. |
| Attribution material | The pinned baseline additionally contains the exact Rust SDK license, WebRTC notice and soxr license at the C++ SDK's Rust submodule revision. Staging collects license/notice files from the pinned SDK archive and available Qt `sbom`/`licenses` directories. These references do not prove which transitive code was compiled or complete licensing obligations. |
| Portable ZIP | All manifest files, including hidden files, are archived and checked after extraction. Directory links, changed files, ambiguous paths and missing notice material fail verification. |
| Runtime isolation | The staged, extracted and installed Qt/QML application (including real TLS-provider availability and zero QML warnings) and native voice/screen self-tests run with developer PATH, Qt plugin and QML overrides cleared. The staged check also removes/restores TLS plugins and requires a specific TLS failure. No remote certificate handshake is claimed. A hosted developer runner remains different from a clean consumer PC. |
| Signature inventory | Authenticode status, signature type, publisher certificate thumbprint and timestamp certificate are observed again during verification. Editing JSON to say `Valid` cannot pass. |
| Installer policy | Fresh current install, an older-version fixture upgrade, repair, shortcut/payload checks and uninstall are exercised. Downgrades, removed previously owned files, relocation and corrupt/unsafe old manifests are rejected before changing the installed payload. |
| User data | A known user-file fixture and a non-secret preference-file fixture retain identical bytes across upgrade, rejection and uninstall. The startup smoke intentionally does not read preferences. Actual owner preference/device behavior is a separate check. |

The older installers use versions `0.0.0-qualification-fixture` and
`0.0.1-qualification-fixture`, wrap the **current real executables**, and change
an existing config; one also owns an obsolete text file. They are not historical
GCP releases. Their tests verify Setup policy, replacement and rejection, not a
migration from the previously published alpha. They are not uploaded as downloads.

## Build and test

Build-time packaging uses PowerShell 7, the pinned Qt/LiveKit SDKs, VS 2022 x64
redistributables and Inno Setup 6. Setup targets x64 Windows 10 1903 (18362) or later / Windows 11. The monitor
capture interop API needs that Windows minimum; ARM64 emulation is not qualified
and Setup refuses it. See the [Microsoft API requirement](https://learn.microsoft.com/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createformonitor).

A friend running the resulting app needs none
of these build tools. The shared host still needs the separately configured
[server stack](server-stack.md).

After the normal Release build, in a disposable Windows account:

```powershell
$commit = git rev-parse HEAD
./scripts/package-windows-test.ps1 -BuildDirectory build/desktop/Release -OutputDirectory build/gcp-windows-test -CommitSha $commit
./scripts/test-windows-runtime.ps1 -BundleDirectory build/gcp-windows-test
./scripts/package-private-windows-zip.ps1 -BundleDirectory build/gcp-windows-test -OutputZip build/gcp-private-windows-x64.zip
./scripts/package-windows-installer.ps1 -BundleDirectory build/gcp-windows-test -OutputDirectory build/installer
./scripts/build-windows-upgrade-fixtures.ps1 -BundleDirectory build/gcp-windows-test -OutputDirectory build/installer-policy-fixtures
./scripts/test-windows-installer.ps1 -Installer build/installer/GCP-0.1.0-alpha.1-windows-x64-setup.exe -BundleDirectory build/gcp-windows-test -FixtureDirectory build/installer-policy-fixtures -CommitSha $commit -OutputFile build/windows-installer-acceptance.json
```

The installer test changes the current user's actual uninstall registration and
creates an isolated preference fixture. It refuses an existing GCP install,
existing preference file or existing output record. Do not run it on your normal
account. Build/archive outputs also require new directories or filenames.
The `-FixtureVersion` option accepts only the two reserved test versions; leave
it unset for the actual alpha package.

For a custom build location, pass `-LiveKitArchive` pointing at the exact pinned
SDK ZIP. Its SHA-256 must match both the notice catalog and the CMake SDK pin.
Qt is deployed without patching DLL bytes; `qt.conf` supplies relocation. Only
the explicitly inventoried app-local CRT is included, not a global redist setup.

CI uploads `gcp-windows-distribution-acceptance` containing the build inventory
and successful installer-policy record. That record is written only after the
checks and cleanup finish. The normal portable ZIP, unsigned installer and
signature inventory remain separate artifacts from the same run and source head.
The published `v0.1.0-alpha.1` is an older build; it is not updated by this work.

## Upgrade and recovery policy

Close GCP and both native media processes before replacing it. A compatible
in-place upgrade replaces the installed payload and keeps unrelated files and
per-profile preferences. Repair reinstalls the selected version's exact files.
Setup compares the prior manifest with the fingerprint compiled into its installer
and recorded in its per-user uninstall key. A missing fingerprint (including the
older published alpha) requires uninstall/reinstall. It refuses a downgrade or a
payload that omits a previously installed file,
rather than leaving obsolete runtime DLLs or deleting paths from an editable old
manifest. It also refuses unknown/corrupt manifests and changing the registered
installation folder during an upgrade.

For a refused upgrade or intentional rollback, explicitly uninstall the old
version, retain a backup of preferences, then install the chosen verified build.
Uninstall removes its own registered files and shortcuts; preferences and
unrelated files remain. Automatic rollback, interrupted-install recovery and
migration from the actual published alpha are not qualified by the fixture test.
Never turn a failed or missing test into a successful acceptance record.

## Owner signing

CI produces unsigned GCP previews. Signing and its publisher identity belong to
the owner. No certificate, password, token or private key is added to this repo.
Use an owner-selected timestamp service and a certificate available through the
Windows signing provider. Microsoft documents [SignTool](https://learn.microsoft.com/windows/win32/seccrypto/signtool).

Sign the three GCP executables **in a separate copy of the Release build before
staging**, using SHA-256 and a trusted timestamp; preserve third-party DLLs.
For example, after setting the real certificate thumbprint and timestamp URL:

```powershell
foreach ($name in @('gcp-desktop.exe','gcp-livekit-voice.exe','gcp-livekit-screen.exe')) {
    signtool sign /sha1 $ExpectedThumbprint /fd SHA256 /tr $TimestampUrl /td SHA256 (Join-Path $SignedBuildDirectory $name)
    if ($LASTEXITCODE -ne 0) { throw 'Publisher signing failed' }
}
```

Stage that signed build into a **new** bundle, supplying its source commit and
original pinned `-LiveKitArchive`, then rerun runtime, ZIP and installer tests.
The package manifest/inventory must describe the signed bytes. Build Setup from
that verified bundle, sign Setup, then regenerate its external checksum:

```powershell
signtool sign /sha1 $ExpectedThumbprint /fd SHA256 /tr $TimestampUrl /td SHA256 $Installer
if ($LASTEXITCODE -ne 0) { throw 'Installer signing failed' }
(Get-FileHash -LiteralPath $Installer -Algorithm SHA256).Hash.ToLowerInvariant() | Set-Content -LiteralPath ($Installer + '.sha256') -Encoding ascii
./scripts/verify-windows-publisher-signatures.ps1 -BundleDirectory $SignedBundleDirectory -Installer $Installer -ExpectedThumbprint $ExpectedThumbprint
```

Before shipping a signed build, rerun installed-startup/repair/uninstall checks
against that exact signed Setup.

The read-only verifier requires all three GCP executables and Setup to have valid
embedded, timestamped signatures from that exact certificate. It neither signs
nor approves vendor DLL licensing, SmartScreen reputation, physical media or
product acceptance. Keep refreshed checksums, observations and source identity
with the actual files shared. Rebuild on app-local CRT security updates; no
automatic updater is implemented.

## Private test handoff and remaining gates

1. Choose the successful CI run for the exact reviewed source head. Download its
   portable/installer artifact and checksum together; verify the extracted file's
   SHA-256 with `Get-FileHash` before running it. An unsigned checksum establishes
   integrity relative to that trusted source, not publisher authentication.
2. On a separate supported Windows 10/11 x64 gaming PC without Qt/VS development tools, test
   startup, invited sign-in, messaging/DMs, device choices, explicit consent,
   Leave/Stop/Watch cleanup, relogin and network/device interruptions. Neither
   microphone nor screen capture should start during installation.
3. Use two real accounts/PCs for audible voice and capture-to-viewer acceptance,
   and record actual [matched gaming measurements](desktop-gaming-qualification.md).
   The owner chooses budgets and reviews quality. No performance saving is claimed.
4. Reconcile exact Qt/transitive, LiveKit FFI/WebRTC/codecs and Microsoft payload
   attributions, upstream source/build material and the chosen Qt licensing route.
   Available SBOMs/notices and file origins are evidence inputs, not approval.
5. Test actual historical-version upgrade/uninstall/reinstall, settings recovery,
   interrupted installation, expected signing identity and consumer-PC outcomes.

Group 12's automated distribution tooling can be reviewed independently of
these outstanding owner/hardware/attribution gates. A stable distribution is
not qualified until those checks have evidence. Publication is manual and
explicitly opted in; ordinary `main` pushes and PRs do not publish previews.
