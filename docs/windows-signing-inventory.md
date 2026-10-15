# Group 55 — Windows executable signature inventory

The Windows CI packaging job now inspects all packaged `.exe` and `.dll` files for SHA-256 digests and Windows Authenticode signature states using `Get-AuthenticodeSignature`. The report `windows-signature-inventory.json` is uploaded separately from the unsigned test bundle.

The script first runs the existing manifest integrity verifier. It **does not sign or alter** any file and intentionally does not fail the test bundle merely because an executable is unsigned. An executable can be third-party-signed while the overall application remains an **unsigned, not production-qualified test bundle**.

Review the signature inventory before any external distribution. Production release will require publisher signing, certificate custody, executable provenance, redistributable license review, clean-machine Windows compatibility, physical voice/security tests and real gaming benchmarks. Authenticode `Valid` on its own does not guarantee that code is safe or approved for release.
