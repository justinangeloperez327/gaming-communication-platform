import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    required property var screen
    required property var directory
    required property var account
    property color textTone: "#e7edf5"
    property color mutedTone: "#9aabba"
    implicitHeight: controls.implicitHeight + 32
    color: "#151e2b"
    radius: 6
    border.color: root.screen.sharing ? "#e38755" : "#293649"
    ColumnLayout {
        id: controls
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8
        Label { text: root.screen.sharing ? "Screen sharing active" : "Screen sharing"; color: root.textTone }
        Label {
            Layout.fillWidth: true
            objectName: "screenStatus"
            text: root.screen.status
            textFormat: Text.PlainText
            color: root.screen.sharing ? "#ffbd91" : root.mutedTone
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            text: "Share one SDR window or display, up to 1920×1080 at 30 fps. No system audio. Channel members with a compatible viewer can watch and record."
            textFormat: Text.PlainText
            color: root.mutedTone
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
        RowLayout {
            Button {
                objectName: "screenChoose"
                text: "Choose source…"
                enabled: root.screen.available && !root.screen.busy && root.account.signedIn && !root.account.busy && !root.directory.busy && root.directory.channelId.length > 0
                onClicked: root.screen.chooseSource()
            }
            Button {
                objectName: "screenStop"
                text: "Stop sharing"
                enabled: root.screen.busy
                onClicked: root.screen.stop()
            }
        }
        Label {
            Layout.fillWidth: true
            visible: !root.screen.available
            text: "Screen publisher is unavailable in this build."
            color: root.mutedTone
            wrapMode: Text.Wrap
        }
    }
    Connections {
        target: root.screen
        function onConsentRequested() { consent.checked = false; confirmation.open() }
        function onConsentInvalidated() { consent.checked = false; confirmation.close() }
    }
    Dialog {
        id: confirmation
        objectName: "screenConfirmation"
        title: "Confirm screen sharing"
        modal: true
        closePolicy: Popup.NoAutoClose
        width: Math.min(520, root.width)
        contentItem: ColumnLayout {
            spacing: 12
            Label {
                Layout.fillWidth: true
                text: "Source: " + root.screen.sourceName + " (" + root.screen.sourceWidth + "×" + root.screen.sourceHeight + ")"
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                text: "Channel: " + root.screen.scopeLabel + ". Members can see everything in this source, including private content and notifications."
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
            }
            CheckBox {
                id: consent
                objectName: "screenConsent"
                Layout.fillWidth: true
                text: "I authorize sharing this source with this channel"
                checked: false
            }
            RowLayout {
                Button {
                    objectName: "screenConfirm"
                    text: "Start sharing"
                    enabled: consent.checked && root.screen.awaitingConsent
                    onClicked: root.screen.confirmShare(consent.checked)
                }
                Button {
                    objectName: "screenCancel"
                    text: "Cancel"
                    onClicked: root.screen.confirmShare(false)
                }
            }
        }
    }
}
