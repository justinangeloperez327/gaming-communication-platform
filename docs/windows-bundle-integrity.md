# Group 47 — Windows bundle file integrity

This change is stacked on the still-open Group 46 packaging PR.

Run `pwsh -File scripts/verify-windows-test-bundle.ps1 -BundleDirectory dist/gcp-windows-test` after packaging. The verifier checks all manifest file sizes and SHA-256 hashes, disallows absolute/traversal entries and duplicate paths, requires the desktop/native LiveKit binaries, rejects unmanifested files and symlink entries, and fails if any packaged file differs from the manifest.

`scripts/test-windows-test-bundle.ps1` creates temporary synthetic files, verifies a correct bundle, changes one file and confirms tampering causes verification failure. The Windows workflow tests this integrity logic without requiring actual hardware.

**Scope:** SHA-256 manifest verification detects accidental or post-manifest changes, not authenticity; an attacker who can modify both files and the unsigned manifest could forge a matching package. This does not replace code signing, clean-machine deployment, antivirus checking, dependency review, or real Windows headset testing. No installer or release is created.
