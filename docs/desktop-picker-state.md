# Desktop selection state

Community, voice-channel and audio dropdowns share the `IdPicker.qml` identity
mapping. Their displayed row follows the controller's selected ID when models
are replaced or reordered. Loading a community/channel list does not select its
first row: the dropdown displays Choose a community/voice channel until the user
selects one. Refresh and sign-out clear these choices; joining still requires an
explicit authorized channel ID.

Audio dropdowns keep a Default row through `AudioDevicePicker.qml`. An empty
device ID means the SDK default, unlike an empty channel ID which disables join.
The [audio selection guide](desktop-audio-devices.md) describes scan behavior.

Offscreen tests load the actual shared and audio QML components. Local HTTP and
subprocess fixtures also check that logout aborts a pending channel request,
clears directory data, permits a fresh login/retry, and refresh leaves no implicit
selection. These tests initialize no audio or media peers; they do not verify
visual rendering, remote room membership or physical audio behavior.
