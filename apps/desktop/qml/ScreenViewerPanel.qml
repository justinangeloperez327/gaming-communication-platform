import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import Gcp.Media 1.0

Rectangle {
    id: root
    required property var viewer
    required property var account
    required property var directory
    property color textTone: "#e7edf5"
    property color mutedTone: "#9aabba"
    implicitHeight: controls.implicitHeight + 32
    color: "#151e2b"
    radius: 6
    border.color: "#293649"
    ColumnLayout {
        id: controls
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8
        Label { text: "Watch a screen"; color: root.textTone }
        Label {
            Layout.fillWidth: true
            text: root.viewer.status
            textFormat: Text.PlainText
            color: root.mutedTone
            wrapMode: Text.Wrap
        }
        ComboBox {
            id: publishers
            objectName: "screenPublishers"
            Layout.fillWidth: true
            model: root.viewer.publishers
            textRole: "name"
            valueRole: "id"
            currentIndex: -1
            displayText: currentIndex < 0 ? "Choose a channel member's share" : currentText
            enabled: !root.viewer.busy
            onActivated: root.viewer.choosePublisher(currentValue)
            Connections {
                target: root.viewer
                function onChanged() { publishers.currentIndex = publishers.indexOfValue(root.viewer.publisherId) }
            }
            delegate: ItemDelegate { width: publishers.width; text: modelData.name; contentItem: Text { text: modelData.name; textFormat: Text.PlainText; elide: Text.ElideRight; color: root.textTone } }
        }
        RowLayout {
            Button {
                objectName: "screenRefreshShares"
                text: "Refresh shares"
                enabled: root.account.signedIn && root.directory.channelId.length > 0 && !root.directory.busy && !root.account.busy && !root.viewer.busy
                onClicked: root.viewer.refreshPublishers()
            }
            Button {
                objectName: "screenWatch"
                text: "Watch"
                enabled: root.viewer.available && root.viewer.publisherId.length > 0 && root.account.signedIn && !root.account.busy && !root.viewer.busy
                onClicked: root.viewer.watch(true)
            }
            Button {
                objectName: "screenStopWatching"
                text: "Stop watching"
                enabled: root.viewer.busy
                onClicked: root.viewer.stop()
            }
        }
    }
    Connections {
        target: root.viewer
        function onStarted() { videoWindow.show(); videoWindow.raise(); videoWindow.requestActivate() }
        function onEnded() { videoWindow.hide() }
    }
    Window {
        id: videoWindow
        objectName: "screenViewerWindow"
        visible: false
        width: 960
        height: 600
        minimumWidth: 360
        minimumHeight: 240
        color: "#080c12"
        title: "Screen viewer — " + root.viewer.publisherName
        onClosing: root.viewer.stop()
        onVisibilityChanged: {
            if (root.viewer.busy && (visibility === Window.Hidden || visibility === Window.Minimized)) root.viewer.stop()
        }
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 10
            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: root.viewer.publisherName + " · " + root.viewer.scopeLabel
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: root.textTone
                }
                Button { text: "Stop watching"; onClicked: root.viewer.stop() }
            }
            Label { Layout.fillWidth: true; text: root.viewer.status; textFormat: Text.PlainText; color: root.mutedTone; wrapMode: Text.Wrap }
            ScreenVideo {
                objectName: "screenVideo"
                Layout.fillWidth: true
                Layout.fillHeight: true
                source: root.viewer
            }
        }
    }
}
