# Windows alpha installer and release

The first requested release is `v0.1.0-alpha.1`. It packages the existing verified
desktop payload as a portable ZIP and an unsigned Inno Setup installer. No Qt,
Rust or Visual Studio installation is needed to run that payload. A configured
GCP backend, PostgreSQL and LiveKit server are still required separately. Desktop
messaging and screen sharing remain incomplete.

Download the `GCP-0.1.0-alpha.1-windows-x64-setup.exe` release asset. Run it under
your normal Windows account. It installs to `%LOCALAPPDATA%\Programs\GCP-Alpha`,
adds a Start menu shortcut and offers an optional desktop shortcut. It requests
no administrator rights, creates no service, adds no firewall exception, installs
no backend and starts no microphone/screen capture. Launch GCP from the Start
menu, sign in and select a voice channel. Mic calls require consent and start
muted. Leave closes the audio session.

Use Windows Settings > Apps to uninstall. Only installed files and shortcuts
are removed; unrelated user files are preserved. The alpha has a separate,
stable installer AppId. CI tests a fresh install and same-version repair, then
installed Qt/QML/SDK startup and uninstall including preservation of a user file.
Cross-version upgrades, downgrades and rollback are not qualified by this first
installer. Close GCP and its native voice process before replacing a version.

## Build and verification

Windows CI uses its installed Inno Setup 6 compiler. After producing and verifying
the normal Qt/LiveKit bundle, it runs:

```powershell
./scripts/package-windows-installer.ps1 -BundleDirectory build/gcp-windows-test -OutputDirectory build/installer
./scripts/test-windows-installer.ps1 -Installer build/installer/GCP-0.1.0-alpha.1-windows-x64-setup.exe
```

The smoke test installs into a temporary directory and changes the current user's
uninstall registration. Run it only on a disposable Windows test account/runner.
The installer wraps every manifest-listed runtime and baseline notice file.
Installed payload checks allow only Inno's exact uninstall metadata pair beyond
the original manifest. Unsigned installer and portable hashes accompany the
release; checksums are integrity evidence, not publisher authentication.

## Publication

`packaging/windows/version.json` is the explicit request for one alpha version.
On a main push, publication runs only after both Rust and Windows CI jobs pass,
including the installer checks. It downloads artifacts from that same run,
checks their SHA-256 files, creates a draft prerelease targeting that commit,
uploads both downloads, hashes and BUILD.json provenance, then publishes the
prerelease. PR builds only upload workflow artifacts. Existing published tags
and assets are never replaced; reruns may resume a draft only for the same commit.
Changing the version request is an explicit request for a future preview.

This publication was requested by the owner after the stabilization audit.
Installer availability does not close its physical two-user audio, clean-PC,
screen-sharing, gaming-performance or transitive attribution limitations. Read
the [audit](stabilization-status.md) and [notice coverage](windows-third-party-notices.md).
Publisher code signing remains the owner's responsibility. The preview notice
inside Setup and release notes disclose the incomplete functionality and evidence.
