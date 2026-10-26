# Desktop settings and recovery — Group 10

[PR #88](https://github.com/justinangeloperez327/gaming-communication-platform/pull/88)
is stacked on [Group 9 / #87](https://github.com/justinangeloperez327/gaming-communication-platform/pull/87).
The Qt desktop, Rust API and native media processes retain their architecture.
This group adds no third-party dependency, installer, release or deployment.

## Remembered choices

Audio device IDs and the push-to-talk mode are remembered for this Windows user
profile when changed in the voice panel. They are preferences for the next join;
they never restore a call, microphone permission, transmission or F8 arming.
Every microphone call still requires per-call consent and starts muted. Screen
source selection and viewing consent remain transient.

The account form offers **Remember this server address**. It is off initially,
and the address is saved only after successful sign-in or account creation.
The saved HTTPS origin pre-fills the next launch's form; it never signs in.
Loopback HTTP remains available for local testing. Credentials, query strings,
fragments and non-root paths are rejected. Email is not remembered.

Expand **Settings & recovery** to forget the server address, check the current
session, reload communities/channels, or reset saved settings. Reset is disabled
while voice is active/scanning or an authentication request is pending. It clears
the saved address, device IDs and mode; the next voice join explicitly uses
Default. It does not sign out the current login, erase server data or join media.

Preferences use `preferences.json` under Qt's per-user `AppConfigLocation` for
organization `GCP` and application `Gaming Communication Platform`. They are
shared by this user's portable and installed app, rather than written next to
the executable. The JSON contains exactly version, input/output IDs, PTT mode,
remember-server flag and server origin. These non-secret values are plain text.
No password, bearer/refresh/media token, email, invitation, message, selected
channel, source, consent or live microphone state is written there.

The versioned file is bounded to 16 KiB on read. Device IDs are bounded to 512
characters and reject control characters; the server is validated independently.
Writes replace a small allowlisted snapshot through `QSaveFile`, with direct
non-atomic fallback disabled. Read failures, malformed/type-invalid/newer files
use defaults and display guidance; the original is preserved until explicit
Reset. Write failure displays guidance and keeps current choices usable in
memory. No continuous save, idle disk polling or credential storage is added.
QML startup smoke tests use temporary in-memory preferences.

## Missing devices and returning headsets

A saved specific ID that is missing or not yet verified is displayed as
**Saved device unavailable**, never as Default. A missing speaker blocks both
joins; a missing microphone blocks only microphone publication. Listen only can
still use an available selected speaker. The controller also enforces these
checks before requesting room authorization.

Reconnect the headset and select **Scan audio devices**, or explicitly choose
another available device or Default. A successful scan preserves IDs that have
disappeared. Their return makes them selectable/available without rejoining any
call. A failed scan preserves the previous list and preferences, marks specific
selections unverified and gives retry guidance. Scanning or changing a device
clears the microphone-consent checkbox.

A profile with specific saved IDs gets one startup enumeration. Otherwise scans
are manual. The old recurring 30-second idle probe has been removed. Enumeration
uses the existing bounded native probe and six-second watchdog, without joining
a room or opening microphone capture. It initializes the platform audio stack;
it is not a microphone test. Output is capped at 64 KiB and 128 unique endpoints
per role; malformed/duplicate IDs fail the scan.

Live calls retain the existing native selected-device polling and fail-closed
device-loss handling. There is no live device rerouting, automatic substitute
microphone, OS-default-device pinning, immediate USB notification or test tone.
Selecting Default deliberately leaves endpoint choice to Windows/the SDK. A
device removed after enumeration can still fail in native startup; its strict
ID lookup prevents a silent replacement.

## Connection and session recovery

**Check connection** sends one unauthenticated GET to the entered origin's
`/ready`. It has a five-second absolute event-loop deadline, 1 KiB response bound,
normal TLS verification and blocked redirects. It carries no login credentials
or media token and makes no authentication, SFU or media mutations. Editing the
address or canceling discards the previous check; late responses cannot restore
its result. There is no background probe or automatic retry.

| Outcome | Meaning and action |
| --- | --- |
| Ready (HTTP 204) | API readiness passed. Sign in; voice and screen connectivity still need an actual call. |
| Unavailable (503) | Host is reachable but dependencies/readiness failed. Ask the host to check services. |
| Certificate failure | Check the address, Windows clock and host certificate. Verification stays enabled. |
| Redirect | Enter the final HTTPS origin; redirects are not followed. |
| Timeout/network/size failure | Check the address/network and explicitly retry. |
| Unexpected response | Confirm this is the GCP API origin with the host. |

**Check session** invokes the existing refresh operation once. It rotates the
current account's credentials; it is not a liveness-only request. A changed known
user ID is rejected. Failed, malformed or missing refresh responses clear the
local session, report **Sign in again**, and stop media through the existing
sign-out connections. An ambiguous rotation is never automatically retried with
the old refresh token. Server cleanup/rotation may have committed without a
response; a fresh sign-in is the recovery path. Successful rotation retains the
existing current-access checks and muted SDK recovery policy.

After a failed login/session, sign in again, choose a channel and explicitly
join/share/watch again. For microphone startup errors, check Windows microphone
privacy settings and the selected endpoint, then scan and join manually.
Screen interruption still requires fresh source/view consent. Settings do not
weaken room access, expiry or revocation checks, and do not replay failed writes.
Password reset, email recovery, MFA and persistent login remain unimplemented.

## Evidence and remaining acceptance

The [PR checks](https://github.com/justinangeloperez327/gaming-communication-platform/pull/88/checks)
and PR audit record identify the exact tested head and results.

- Windows and offscreen Linux `desktop_preferences_and_recovery` exercise real
  atomic files, restart/forget/reset, unsafe input, corrupt/oversized/newer files,
  failed writes, actual Qt HTTP buffering/deadlines/redirects/cancellation and
  shipped account/settings QML, login persistence exclusions and session failure.
- Windows voice/QML/subprocess tests cover restored IDs/mode without consent,
  missing input versus output, return without auto-join, native argument identity,
  active-call reset denial, device-loss cleanup, and stopping a transmitting
  protocol child on manual refresh failure. The child opens no real devices.
- Real Qt→Rust→PostgreSQL acceptance checks `/ready` through the production probe
  and explicitly rotates real credentials before continuing existing onboarding,
  messaging and screen-lease flows. It is separate from media acceptance.
- Existing synthetic SDK/SFU, package integrity, staged/extracted/installed startup
  and alpha installation lifecycle checks remain required. No package version or
  publication configuration changes.

Physical two-PC voice, actual picker/GPU publishing-to-viewing, real permission
denial, USB/Bluetooth/default-device behavior, SDK/network outages and long gaming
sessions remain owner-run checks. Restart both portable and installed builds on
a consumer PC; verify remembered choices, missing-device guidance, explicit
Default/reselection, fresh consent and no orphan processes after failures. Check
normal, corrupt and unwritable user-profile settings. No measured performance
improvement or exhaustive leak-free claim follows from the bounded design.

Next is Group 11 gaming qualification using actual matched measurements, then
Group 12 distribution qualification. Complete attribution, owner signing, clean
Windows/version-upgrade evidence and physical product acceptance remain gates
before treating the existing alpha installer as a stable distribution.
