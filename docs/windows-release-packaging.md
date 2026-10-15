# Group 46 — Windows desktop test packaging

Group 45's CI and benchmark tooling tests passed and PR #48 was merged. The Windows desktop presently requires `gcp-desktop.exe`, the separate `gcp-livekit-voice.exe` executable, Qt Quick/QML runtime files, and the LiveKit native DLL dependencies. A compiled application alone does not establish a portable Windows package.

The `scripts/package-windows-test.ps1` staging script packages an **unsigned test bundle**, checking that the desktop, native voice executable, `livekit.dll` and `livekit_ffi.dll` exist before copying anything. It invokes Qt's `windeployqt` to stage Qt/QML runtime dependencies and emits a relative-path SHA-256 manifest for post-copy inspection. The tool refuses overwriting an existing output directory.

Example on a Windows developer machine after a Release build and after making `windeployqt` accessible in PATH:

```powershell
pwsh -File scripts/package-windows-test.ps1 -BuildDirectory build/desktop/Release -OutputDirectory dist/gcp-windows-test
```

This script intentionally does not publish an installer or tag a release, does not sign binaries, does not verify Windows SmartScreen or antivirus outcomes, does not attest third-party licensing completeness, and does not prove actual audio hardware compatibility.

## Release acceptance

- Test the staged directory on a separate clean Windows installation without developer Qt installations.
- Confirm Qt platform and QML modules, LiveKit DLL loading, and native voice process startup.
- Verify microphone and listen-only device consent, account logout, community/channel switching, network recovery, and SFU revocation.
- Measure idle, listen-only and publishing CPU/RAM on gaming hardware while collecting matched frame-time data.
- Review dependency licenses, provenance, code signing, installer upgrade/uninstall behavior and rollback.
- Produce signed, versioned release artifacts only after all gates are satisfied.
