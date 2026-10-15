# Group 38 — Idle audio-device recovery and bounded scanning

Qt refreshes the available microphone and speaker inventory every 30 seconds while idle, using the existing native `devices` probe. Scans do not run during a voice session or active authorization. A six-second watchdog terminates a hung probe. The watchdog is owned by the session controller instead of a detached single-shot closure, so older scans cannot kill newer ones.

A failed probe does not erase previously selected device identifiers. A successful scan reconciles selections against current endpoints and clears any selections that have disappeared. The user must explicitly choose and authorize a new microphone session; hotplug detection never starts voice or switches an active microphone. Live selected-device disappearance during a call remains handled by Group 37's five-second native polling.

This increment does not provide Windows device-notification callbacks, instant USB hotplug handling, live audio rerouting, or physical microphone verification. Group 37's CI result and hardware tests should be reviewed before production use. Continuous scans are not started while a voice session is active, reducing interference during gaming.
