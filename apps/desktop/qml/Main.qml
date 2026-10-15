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
    property bool microphoneConsent: false
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
                    text: "COMMUNITY"
                    color: root.mutedTone
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                }

                ComboBox {
                    id: communityPicker
                    Layout.fillWidth: true
                    enabled: accountSession.signedIn && !channelDirectory.busy && !voiceSession.busy
                    model: channelDirectory.communities
                    textRole: "name"
                    valueRole: "id"
                    onActivated: channelDirectory.chooseCommunity(currentValue)
                    Component.onCompleted: currentIndex = -1
                }
                Text { text: "VOICE CHANNELS"; color: root.mutedTone; font.pixelSize: 11 }
                ComboBox {
                    id: voicePicker
                    Layout.fillWidth: true
                    enabled: accountSession.signedIn && channelDirectory.communityId.length > 0 && !channelDirectory.busy && !voiceSession.busy
                    model: channelDirectory.channels
                    textRole: "name"
                    valueRole: "id"
                    onActivated: channelDirectory.chooseVoiceChannel(currentValue)
                    Component.onCompleted: currentIndex = -1
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
                        text: "Sign in to select a community and join experimental voice calls of up to five minutes. Desktop messaging and screen sharing are not connected yet."
                        font.pixelSize: 14
                        color: root.mutedTone
                        wrapMode: Text.WordWrap
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: accountSession.signedIn ? 120 : 270
                        radius: 6
                        color: root.panel
                        border.color: root.borderTone
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 12
                            spacing: 5
                            Text { text: "Account"; color: root.textTone; font.pixelSize: 14 }
                            Text { text: accountSession.status; color: root.mutedTone }
                            TextField { id: serverOrigin; Layout.fillWidth: true; visible: !accountSession.signedIn; placeholderText: "https://your-gcp-server" }
                            TextField { id: loginEmail; Layout.fillWidth: true; visible: !accountSession.signedIn; placeholderText: "Email" }
                            TextField { id: loginPassword; Layout.fillWidth: true; visible: !accountSession.signedIn; placeholderText: "Password"; echoMode: TextInput.Password }
                            Button {
                                text: accountSession.signedIn ? "Sign out" : "Sign in"
                                enabled: !accountSession.busy
                                onClicked: {
                                    if (accountSession.signedIn) accountSession.signOut()
                                    else {
                                        accountSession.signIn(serverOrigin.text, loginEmail.text, loginPassword.text)
                                        loginPassword.text = ""
                                    }
                                }
                            }
                        }
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 340
                        radius: 6
                        color: root.panel
                        border.color: root.borderTone
                        ColumnLayout {
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
                                text: voiceSession.status
                                color: root.mutedTone
                                wrapMode: Text.Wrap
                                font.pixelSize: 12
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Button {
                                    text: voiceSession.scanning ? "Scanning…" : "Scan audio devices"
                                    enabled: !voiceSession.busy && !voiceSession.scanning
                                    onClicked: voiceSession.scanDevices()
                                }
                                Text { text: "Device choices apply on next join"; color: root.mutedTone; font.pixelSize: 11 }
                            }
                            ComboBox {
                                id: microphoneDevice
                                Layout.fillWidth: true
                                enabled: !voiceSession.busy && !voiceSession.scanning
                                model: [{ "name": "Default microphone", "id": "" }].concat(voiceSession.inputs)
                                textRole: "name"
                                valueRole: "id"
                                onActivated: voiceSession.selectInput(currentValue)
                            }
                            ComboBox {
                                id: speakerDevice
                                Layout.fillWidth: true
                                enabled: !voiceSession.busy && !voiceSession.scanning
                                model: [{ "name": "Default speaker", "id": "" }].concat(voiceSession.outputs)
                                textRole: "name"
                                valueRole: "id"
                                onActivated: voiceSession.selectOutput(currentValue)
                            }
                            CheckBox {
                                id: micPermission
                                text: "I authorize microphone capture for this call"
                                checked: root.microphoneConsent
                                enabled: !voiceSession.busy
                                onToggled: root.microphoneConsent = checked
                            }
                            RowLayout {
                                spacing: 8
                                Button {
                                    text: "Listen only"
                                    enabled: voiceSession.available && accountSession.signedIn && channelDirectory.channelId.length > 0 && !voiceSession.busy
                                    onClicked: voiceSession.joinListenOnly()
                                }
                                Button {
                                    text: "Join with mic"
                                    enabled: voiceSession.available && accountSession.signedIn && channelDirectory.channelId.length > 0 && !voiceSession.busy && root.microphoneConsent
                                    onClicked: {
                                        voiceSession.joinWithMicrophone(root.microphoneConsent)
                                        root.microphoneConsent = false
                                    }
                                }
                                Button {
                                    text: "Leave"
                                    enabled: voiceSession.busy
                                    onClicked: voiceSession.leave()
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
}
