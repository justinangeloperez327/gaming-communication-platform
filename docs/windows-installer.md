# Windows alpha installer

The existing alpha installer installs the Qt desktop and native LiveKit voice and
screen processes for the current Windows user under
`%LOCALAPPDATA%\Programs\GCP-Alpha`. It requests no administrator rights, creates
no service or firewall exception, deploys no backend and starts no microphone or
screen capture. It creates a Start menu shortcut and offers an optional desktop
shortcut. A configured GCP backend, PostgreSQL and LiveKit host remain separate.
The packaged app includes Qt/QML, LiveKit and app-local VS x64 runtime DLLs; friends
do not install the developer tools.

Current source implements onboarding, community/channel management, text,
friends/DMs, voice, the Windows screen picker/publisher, explicit Watch and private
settings. Physical two-PC media and gaming qualification remain pending. The
already published `v0.1.0-alpha.1` is an older preview and does not gain these
features when new source is merged. Use the exact successful workflow artifact
for private testing of newer source.

Setup keeps its stable alpha AppId. Compatible upgrades and same-version repair
preserve unrelated files and preferences. Downgrades, relocation, removed old
payload files and unreadable/unsafe manifests require an explicit uninstall
before installation. No old-manifest path is used to delete user files. Windows
Settings > Apps removes the installed payload and shortcuts while leaving user
files and per-profile preferences. Close the desktop and both native processes
before replacing a build. Automatic rollback and historical release migration
remain unqualified.

See [Group 12 distribution qualification](distribution-qualification.md) for the
build commands, disposable-account upgrade tests, byte-bound inventory, owner
signing order and clean-PC handoff. The tests exercise older-version wrappers
around current binaries and non-secret preference fixtures; they are not proof
of historical migration or physical media acceptance.

## Publication

`packaging/windows/version.json` still selects the existing alpha version.
Ordinary pushes and PRs only run checks/upload workflow artifacts. A repository
owner must explicitly dispatch CI on `main` with `publish_preview: true` to invoke
the existing publish job after every required check succeeds. Published tags and
assets remain immutable; reruns can resume a matching unpublished draft only.
No release or deployment is performed by Group 12.

CI remains unsigned. Checksums verify integrity against a trusted source; owner
publisher signing is separate. Full build-specific third-party attribution,
consumer-PC, real media, historical upgrade and gameplay acceptance remain open;
read the [audit](stabilization-status.md) and
[notice coverage](windows-third-party-notices.md).
