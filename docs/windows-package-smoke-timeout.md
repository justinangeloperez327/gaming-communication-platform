# Group 52 — Bounded packaged GUI smoke test

The unsigned packaged Qt/QML smoke test now has a 20-second deadline. The CI runner starts the GUI-subsystem executable as a tracked process, waits for completion, verifies a zero exit code, and force-stops it if it hangs. This prevents a nonterminating GUI from holding the Windows build indefinitely and retains fail-closed artifact validation.

Group 51's CI was still running when this change was authored; this is preventive hardening rather than confirmation that packaging passed. A successful hosted-runner smoke test does not establish clean-machine compatibility, physical voice communication, code-signing trust or acceptable in-game resource usage.
