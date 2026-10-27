# Windows desktop test packaging

The current desktop payload includes `gcp-desktop.exe`, native voice and screen
executables, Qt Quick/QML and WebSockets modules, `livekit.dll`, `livekit_ffi.dll`
and app-local VS 2022 x64 CRT DLLs. The package still requires a separate
configured backend/database/SFU host.

`scripts/package-windows-test.ps1` stages a new directory using the pinned Qt
6.8.3 and LiveKit C++ SDK 1.12.2. It preserves upstream DLL bytes, records executable
provenance against the actual input trees/archive, collects available upstream
attribution material, and writes the payload list plus SHA-256/size manifest.
Unknown input binaries, changed SDK pins and existing output directories fail.
`qt.conf` supplies relative runtime paths. Packaging requires PowerShell 7 and the
normal developer SDKs; an end user does not need these build tools.

```powershell
./scripts/package-windows-test.ps1 -BuildDirectory build/desktop/Release -OutputDirectory dist/gcp-windows-test -CommitSha (git rev-parse HEAD)
./scripts/test-windows-runtime.ps1 -BundleDirectory dist/gcp-windows-test
```

The portable ZIP includes hidden manifest files. Staged, extracted and installed
startup checks clear developer Qt/QML/plugin and PATH overrides. Hash and source
records do not substitute for owner signing or a separate consumer Windows PC.

See [distribution qualification](distribution-qualification.md) for the complete
build/test commands, guarded installer upgrade policy, owner signing order and
private testing handoff, and [notice coverage](windows-third-party-notices.md) for
the remaining exact-build attribution/source reconciliation. No measured gaming
saving, physical two-PC quality, consumer-PC acceptance or stable release approval
is implied by these tools. Group 12 does not publish or deploy a service.
