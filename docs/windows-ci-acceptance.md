# Group 53 — Windows CI package acceptance evidence

The Windows CI pipeline emits `gcp-windows-ci-acceptance`, a seven-day artifact containing a JSON report identifying the exact build commit and the checks performed on the **unsigned test bundle**.

The acceptance record is generated only after the build, CTest, bundle SHA-256 verification, packaged QML smoke test and native LiveKit self-test steps complete successfully. It reruns bundle verification and writes the report **outside** the bundle directory to preserve the bundle's checksum manifest.

The report explicitly marks clean-machine compatibility, physical two-user voice, microphone privacy on hardware, real gaming CPU/GPU/RAM/FPS measurements, active-token revocation and code signing as **not tested or not completed**, with `production_qualified: false`.

This is a CI provenance summary, not cryptographic attestation. The commit ID is drawn from GitHub Actions; the report is not signed and should not be used to make unsupported production-readiness claims. A signed Windows installer and public release are out of scope.
