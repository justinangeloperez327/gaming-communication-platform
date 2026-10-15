# Group 51 — Windows GUI smoke-process reliability

Group 50 restored the Windows bundle verifier and the QML smoke mode. In its CI run, native builds, CTest, SHA-256 verification of the staged bundle and the independent package-integrity workflow succeeded, but the packaged Qt/QML startup check failed with an empty PowerShell `$LASTEXITCODE` value.

The desktop is compiled using the Windows **GUI subsystem** (`WIN32_EXECUTABLE`), so invoking it using PowerShell's call operator is not a reliable way to wait for completion and read its exit status. Windows CI now uses `Start-Process -Wait -PassThru` to launch the actual staged executable and inspect the returned `ExitCode`.

The executable's `--smoke-qml` mode loads the QML module and should terminate after approximately 2.5 seconds. This change fixes the test harness and does not alter audio handling or application release behavior.

If Windows CI still fails, inspect the actual GUI process exit code and the packaged QML dependencies; do not skip the startup test to publish artifacts. Physical Windows, microphone, two-user voice, and gaming-performance testing remain release blockers.
