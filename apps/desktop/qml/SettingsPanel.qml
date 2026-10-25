import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    required property var preferences
    required property var voice
    required property var account
    required property var directory
    property bool expanded: false
    color: "#151e2b"
    border.color: "#293649"
    radius: 6
    implicitHeight: controls.implicitHeight + 24
    ColumnLayout {
        id: controls
        anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
        anchors.margins: 12
        spacing: 8
        Button {
            objectName: "settingsExpand"
            text: root.expanded ? "Hide settings & recovery" : "Settings & recovery"
            onClicked: root.expanded = !root.expanded
        }
        ColumnLayout {
            visible: root.expanded
            Layout.fillWidth: true
            spacing: 8
            Text {
                Layout.fillWidth: true
                text: "Audio device choices and push-to-talk mode are saved for this Windows profile. Microphone/screen consent, passwords, tokens and messages are never saved in preferences."
                textFormat: Text.PlainText; color: "#9aabba"; wrapMode: Text.Wrap
            }
            Text {
                Layout.fillWidth: true
                text: root.preferences.status
                visible: text.length > 0
                textFormat: Text.PlainText; color: "#9aabba"; wrapMode: Text.Wrap
            }
            Text {
                Layout.fillWidth: true
                text: root.preferences.serverOrigin.length ? "Saved server: " + root.preferences.serverOrigin : "Server address is not saved."
                textFormat: Text.PlainText; color: "#e7edf5"; wrapMode: Text.Wrap
            }
            Button {
                objectName: "settingsForgetServer"
                text: "Forget server address"
                enabled: root.preferences.rememberServer
                onClicked: root.preferences.rememberServer = false
            }
            Button {
                objectName: "settingsCheckSession"
                text: "Check session"
                enabled: root.account.signedIn && !root.account.busy && !root.account.invitationBusy
                onClicked: root.account.refreshSession()
            }
            Button {
                objectName: "settingsRefreshDirectory"
                text: "Reload communities and channels"
                enabled: root.account.signedIn && !root.account.busy && !root.directory.busy && !root.voice.busy
                onClicked: root.directory.refresh()
            }
            Text {
                Layout.fillWidth: true
                text: "A session check rotates your current login once. If it fails or its reply is lost, sign in again. Media is never rejoined automatically.\nFor microphone errors, check Windows Settings → Privacy & security → Microphone and allow desktop apps, then scan devices and join manually. A readiness check does not verify voice, screen capture or remote audio."
                textFormat: Text.PlainText; color: "#9aabba"; wrapMode: Text.Wrap
            }
            Button {
                objectName: "settingsReset"
                text: "Reset saved settings"
                enabled: !root.voice.busy && !root.voice.scanning && !root.account.busy
                onClicked: root.voice.resetSavedSettings()
            }
        }
    }
}
