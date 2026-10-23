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
                    text: channelDirectory.communityName.length ? channelDirectory.communityName : "Your communities"
                    textFormat: Text.PlainText
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
                    text: "COMMUNITY"
                    color: root.mutedTone
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                }

                IdPicker {
                    id: communityPicker
                    Layout.fillWidth: true
                    enabled: accountSession.signedIn && !channelDirectory.busy && !voiceSession.busy
                    options: channelDirectory.communities
                    selectedId: channelDirectory.communityId
                    emptyLabel: "Choose a community"
                    onActivated: channelDirectory.chooseCommunity(currentValue)
                }
                Text { text: "TEXT CHANNELS"; color: root.mutedTone; font.pixelSize: 11 }
                IdPicker {
                    id: textPicker
                    Layout.fillWidth: true
                    enabled: accountSession.signedIn && !accountSession.busy && channelDirectory.communityId.length > 0 && !channelDirectory.busy
                    options: channelDirectory.textChannels
                    selectedId: channelDirectory.textChannelId
                    emptyLabel: "Choose a text channel"
                    onActivated: channelDirectory.chooseTextChannel(currentValue)
                }
                Text { text: "VOICE CHANNELS"; color: root.mutedTone; font.pixelSize: 11 }
                IdPicker {
                    id: voicePicker
                    Layout.fillWidth: true
                    enabled: accountSession.signedIn && channelDirectory.communityId.length > 0 && !channelDirectory.busy && !voiceSession.busy
                    options: channelDirectory.channels
                    selectedId: channelDirectory.channelId
                    emptyLabel: "Choose a voice channel"
                    onActivated: channelDirectory.chooseVoiceChannel(currentValue)
                }
                Text {
                    Layout.fillWidth: true
                    text: channelDirectory.status
                    color: root.mutedTone
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                }
                Button {
                    text: "Refresh communities"
                    enabled: accountSession.signedIn && !channelDirectory.busy && !voiceSession.busy
                    onClicked: channelDirectory.refresh()
                }
                Text {
                    Layout.fillWidth: true
                    text: "Voice · " + voiceSession.status
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
                            text: accountSession.signedIn ? accountSession.displayName : "Signed out"
                            color: root.textTone
                            font.pixelSize: 12
                        }
                        Text {
                            text: accountSession.status
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

            ScrollView {
                id: mainScroll
                anchors.fill: parent
                contentWidth: availableWidth

            ColumnLayout {
                width: mainScroll.availableWidth
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
                            text: channelDirectory.textChannelName.length ? "# " + channelDirectory.textChannelName :
                                  channelDirectory.channelName.length ? "Voice · " + channelDirectory.channelName :
                                  channelDirectory.communityName.length ? channelDirectory.communityName : "Your communities"
                            textFormat: Text.PlainText
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
                        text: "Sign in, choose a community, and open a text channel or voice call. Screen sharing remains a separate diagnostic."
                        font.pixelSize: 14
                        color: root.mutedTone
                        wrapMode: Text.WordWrap
                    }
                    AccountPanel {
                        Layout.fillWidth: true
                        account: accountSession
                        textTone: root.textTone
                        mutedTone: root.mutedTone
                    }
                    ChatPanel {
                        Layout.fillWidth: true
                        visible: accountSession.signedIn
                        chat: chatSession
                        directory: channelDirectory
                        account: accountSession
                        textTone: root.textTone
                        mutedTone: root.mutedTone
                    }
                    CommunityPanel {
                        Layout.fillWidth: true
                        directory: channelDirectory
                        signedIn: accountSession.signedIn
                        textTone: root.textTone
                        mutedTone: root.mutedTone
                    }
                    VoicePanel {
                        Layout.fillWidth: true
                        voice: voiceSession
                        directory: channelDirectory
                        account: accountSession
                        panel: root.panel
                        borderTone: root.borderTone
                        textTone: root.textTone
                        mutedTone: root.mutedTone
                    }
                }

                Item { Layout.fillHeight: true }


            }
            }
        }
    }
}
