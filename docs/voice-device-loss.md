# Group 37 — Selected audio endpoint removal

When a user explicitly selects a microphone or speaker device, native voice checks for the selected endpoint at most once every five seconds during the call. If a selected endpoint is no longer enumerated, the native process emits a fixed `GCP_VOICE_EVENT:device_lost` message and exits its call loop; the existing cleanup mutes the microphone and disconnects the room. Qt also terminates the process with its two-second fallback and shows a persistent device-removal reason instead of a generic disconnect.

Users must rescan and manually join with freshly issued backend authorization. The client never automatically switches to another microphone, and explicit consent is required before publishing on any new call.

Limitations: detection is polling-based, not instant. It only covers **explicitly selected** device IDs; OS-default device loss and automatic device replacement are not reliably detectable here. Enumeration errors alone do not trigger a false removal. Audio-stack exceptions, unplug/replug races and actual stop time must be verified on physical Windows hardware. This is not a guarantee of continuous microphone privacy under all hardware faults. Performance overhead is a bounded device enumeration every five seconds for selected-device calls.
