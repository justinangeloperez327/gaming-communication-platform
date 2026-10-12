import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    visible: true
    title: "Gaming Communication Platform"
    width: 1180
    height: 760
    minimumWidth: 850
    minimumHeight: 580
    color: "#0e141d"

    property string selectedChannel: "general"
    property bool microphonePreviewMuted: false
    readonly property color panel: "#151e2b"
    readonly property color borderTone: "#293649"
    readonly property color textTone: "#e7edf5"
    readonly property color mutedTone: "#9aabba"
    readonly property color accent: "#5e9fea"

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.preferredWidth: 68
            Layout.fillHeight: true
            color: "#0a1018"

            Column {
                width: parent.width
                spacing: 16
                topPadding: 20

                Rectangle {
                    width: 42
                    height: 42
                    radius: 8
                    color: root.accent
                    anchors.horizontalCenter: parent.horizontalCenter
                    Text {
                        anchors.centerIn: parent
                        text: "G"
                        color: "#0a1018"
                        font.bold: true
                        font.pixelSize: 22
                    }
                }
                Rectangle {
                    width: 32
                    height: 1
                    color: root.borderTone
                    anchors.horizontalCenter: parent.horizontalCenter
                }
                Text {
                    text: "TEAM"
                    color: root.mutedTone
                    anchors.horizontalCenter: parent.horizontalCenter
                    font.pixelSize: 10
                    font.bold: true
                }
            }
        }

        Rectangle {
            Layout.preferredWidth: 246
            Layout.fillHeight: true
            color: root.panel

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12

                Text {
                    Layout.fillWidth: true
                    text: "Gaming Community"
                    font.pixelSize: 17
                    font.weight: Font.DemiBold
                    color: root.textTone
                    elide: Text.ElideRight
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: root.borderTone
                }
                Text {
                    text: "TEXT CHANNELS"
                    color: root.mutedTone
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                }

                Repeater {
                    model: ["general", "gaming", "announcements"]
                    delegate: Rectangle {
                        required property string modelData
                        Layout.fillWidth: true
                        Layout.preferredHeight: 38
                        radius: 5
                        color: root.selectedChannel === modelData ? "#253953" : "transparent"

                        Text {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            verticalAlignment: Text.AlignVCenter
                            text: "#  " + modelData
                            color: root.selectedChannel === modelData ? root.textTone : root.mutedTone
                            font.pixelSize: 13
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: root.selectedChannel = modelData
                        }
                    }
                }

                Text {
                    Layout.topMargin: 12
                    text: "VOICE CHANNELS"
                    color: root.mutedTone
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                }

                Text {
                    Layout.fillWidth: true
                    text: "◖  Team Voice (not connected)"
                    color: root.mutedTone
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }

                Item { Layout.fillHeight: true }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: root.borderTone
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Rectangle {
                        Layout.preferredWidth: 32
                        Layout.preferredHeight: 32
                        radius: 5
                        color: "#30425a"
                        Text {
                            anchors.centerIn: parent
                            text: "U"
                            color: root.textTone
                            font.bold: true
                        }
                    }
                    ColumnLayout {
                        spacing: 1
                        Text {
                            text: "Local Preview"
                            color: root.textTone
                            font.pixelSize: 12
                        }
                        Text {
                            text: "Offline"
                            color: root.mutedTone
                            font.pixelSize: 11
                        }
                    }
                    Item { Layout.fillWidth: true }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: root.color

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 64
                    color: "#111925"

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 24
                        anchors.rightMargin: 24
                        spacing: 8
                        Text {
                            text: "# " + root.selectedChannel
                            color: root.textTone
                            font.weight: Font.DemiBold
                            font.pixelSize: 17
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: "Foundation preview"
                            color: root.mutedTone
                            font.pixelSize: 12
                        }
                    }
                }

                Item { Layout.fillHeight: true }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 28
                    Layout.rightMargin: 28
                    spacing: 14

                    Text {
                        Layout.fillWidth: true
                        text: "Your gaming space starts here."
                        font.pixelSize: 24
                        font.weight: Font.DemiBold
                        color: root.textTone
                        wrapMode: Text.WordWrap
                    }
                    Text {
                        Layout.fillWidth: true
                        text: "The native desktop shell is running. Messaging, voice, and screen sharing will be connected in later implementations."
                        font.pixelSize: 14
                        color: root.mutedTone
                        wrapMode: Text.WordWrap
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 96
                        radius: 6
                        color: root.panel
                        border.color: root.borderTone

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            Text {
                                text: "Local UI controls"
                                color: root.textTone
                                font.pixelSize: 12
                            }
                            RowLayout {
                                spacing: 12
                                Button {
                                    text: root.microphonePreviewMuted ? "Unmute preview" : "Mute preview"
                                    onClicked: root.microphonePreviewMuted = !root.microphonePreviewMuted
                                }
                                Text {
                                    text: "No microphone or call is connected"
                                    color: root.mutedTone
                                    font.pixelSize: 12
                                }
                            }
                        }
                    }
                }

                Item { Layout.fillHeight: true }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 74
                    color: "#111925"
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 22
                        anchors.rightMargin: 22
                        TextField {
                            Layout.fillWidth: true
                            enabled: false
                            placeholderText: "Messaging not connected yet"
                        }
                    }
                }
            }
        }
    }
}
