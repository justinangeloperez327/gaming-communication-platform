# Group 58 — Private portable Windows distribution

This project targets a small trusted group of friends, not the Microsoft Store or a public Windows installer.

After Windows build, CTest, Qt/QML smoke, LiveKit self-test and package-integrity checks succeed, CI produces a portable ZIP `gcp-private-windows-x64.zip` with a companion SHA-256 digest. CI expands the ZIP into an independent temporary directory and reruns the full file-manifest verifier against its extracted contents before uploading the artifacts (retention: seven days).

## Using with friends

Download the `gcp-private-windows-x64` artifact from a successful GitHub Actions CI run, verify the ZIP's SHA-256 against `gcp-private-windows-x64.sha256`, unzip it, and run `gcp-desktop.exe`. Do not run individual executables without the bundled dependencies. Share the archive only through a trusted channel; an unsigned executable may trigger Windows reputation warnings.

There is **no publisher certificate requirement** for this private testing workflow. No unsigned ZIP should be described as authenticated, production-qualified or independently trustworthy. Avoid sharing tokens, credentials and participant data in support screenshots.

## Acceptance still needed

Two-user voice tests, screen sharing, privacy controls, credential revocation, clean Windows startup, gaming CPU/GPU/RAM/FPS testing, and local firewall/network behavior remain real acceptance tasks. Code signing is optional for this private audience. The workflow does not automatically publish a GitHub Release or deploy any service.
