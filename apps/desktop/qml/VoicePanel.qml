import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    required property var voice
    required property var directory
    required property var account
    signal leaveRequested()
    property bool microphoneConsent: false
    property color panel: "#151e2b"
    property color borderTone: "#293649"
    property color textTone: "#e7edf5"
    property color mutedTone: "#9aabba"
    implicitHeight: controls.implicitHeight + 32

    Connections {
        target: root.directory
        function onSelectionChanged() { root.microphoneConsent = false }
    }
    Connections {
        target: root.account
        function onSignedOut() { root.microphoneConsent = false }
    }
    Connections {
        target: root.voice
        function onRunningChanged() { root.microphoneConsent = false }
    }
    radius: 6
    color: root.panel
    border.color: root.borderTone
    ColumnLayout {
        id: controls
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8
        Text {
            text: "Voice connection · experimental"
            color: root.textTone
            font.pixelSize: 14
        }
        Text {
            Layout.fillWidth: true
            text: root.voice.status
            color: root.mutedTone
            wrapMode: Text.Wrap
            font.pixelSize: 12
        }
        RowLayout {
            Layout.fillWidth: true
            Button {
                text: root.voice.scanning ? "Scanning…" : "Scan audio devices"
                enabled: !root.voice.busy && !root.voice.scanning
                onClicked: root.voice.scanDevices()
            }
            Text { text: "Device choices apply on next join"; color: root.mutedTone; font.pixelSize: 11 }
        }
        AudioDevicePicker {
            id: microphoneDevice
            Layout.fillWidth: true
            enabled: !root.voice.busy && !root.voice.scanning
            devices: root.voice.inputs
            selectedDeviceId: root.voice.inputId
            defaultLabel: "Default microphone"
            onActivated: root.voice.selectInput(currentValue)
        }
        AudioDevicePicker {
            id: speakerDevice
            Layout.fillWidth: true
            enabled: !root.voice.busy && !root.voice.scanning
            devices: root.voice.outputs
            selectedDeviceId: root.voice.outputId
            defaultLabel: "Default speaker"
            onActivated: root.voice.selectOutput(currentValue)
        }
        CheckBox {
            text: "Push-to-talk · hold F8 (works outside this window)"
            checked: root.voice.pushToTalk
            enabled: !root.voice.busy || root.voice.connected
            objectName: "voicePushToTalk"
            onCheckedChanged: root.voice.setPushToTalk(checked)
        }
        RowLayout {
            Button {
                objectName: "voiceMute"
                text: root.voice.muted ? "Enable mic" : "Mute mic"
                enabled: root.voice.connected && root.voice.microphoneEnabled
                onClicked: root.voice.setMuted(!root.voice.muted)
            }
            Text {
                text: !root.voice.microphoneEnabled ? "Listen only" :
                      root.voice.transmitting ? "Microphone transmitting" :
                      root.voice.muted ? "Microphone muted" : "PTT armed · hold F8"
                color: root.mutedTone
            }
        }
        Text {
            Layout.fillWidth: true
            text: "Mic calls start muted. Enable mic after joining. Muting stops transmission; the audio device may remain open until Leave."
            color: root.mutedTone
            wrapMode: Text.Wrap
            font.pixelSize: 11
        }
        CheckBox {
            objectName: "voiceMicrophoneConsent"
            text: "I authorize microphone capture for this call"
            checked: root.microphoneConsent
            enabled: root.account.signedIn && root.directory.channelId.length > 0 && !root.directory.busy && !root.voice.busy && !root.voice.scanning
            onCheckedChanged: root.microphoneConsent = checked
        }
        RowLayout {
            spacing: 8
            Button {
                objectName: "voiceListenOnly"
                text: "Listen only"
                enabled: root.voice.available && root.account.signedIn && root.directory.channelId.length > 0 && !root.directory.busy && !root.voice.busy && !root.voice.scanning
                onClicked: {
                    root.microphoneConsent = false
                    root.voice.joinListenOnly()
                }
            }
            Button {
                objectName: "voiceJoinMicrophone"
                text: "Join with mic"
                enabled: root.voice.available && root.account.signedIn && root.directory.channelId.length > 0 && !root.directory.busy && !root.voice.busy && !root.voice.scanning && root.microphoneConsent
                onClicked: {
                    const consent = root.microphoneConsent
                    root.microphoneConsent = false
                    root.voice.joinWithMicrophone(consent)
                }
            }
            Button {
                objectName: "voiceLeave"
                text: "Leave"
                enabled: root.voice.busy
                onClicked: {
                    root.microphoneConsent = false
                    root.leaveRequested()
                    root.voice.leave()
                }
            }
        }
        Text {
            Layout.fillWidth: true
            text: "Fetches a short-lived voice token from the Rust backend for each join. Choose your community and voice channel above before joining."
            color: root.mutedTone
            font.pixelSize: 11
            wrapMode: Text.Wrap
        }
    }
}
