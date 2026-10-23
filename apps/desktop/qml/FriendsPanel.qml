import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: panel
    required property var social
    required property var account
    property color textTone: "#e7edf5"
    property color mutedTone: "#9aabba"
    property string action: ""
    property string actionUser: ""
    property string actionSession: ""
    color: "#151e2b"
    radius: 6
    implicitHeight: contents.implicitHeight + 32

    function ask(kind, id) {
        action = kind
        actionUser = id
        actionSession = account.userId
        confirmation.open()
    }
    function clearAction() {
        confirmation.close()
        action = ""
        actionUser = ""
    }
    Connections {
        target: panel.account
        function onSignedOut() { panel.clearAction(); handleInput.text = "" }
    }
    Connections {
        target: panel.social
        function onRelationshipLost(peer) { if (panel.actionUser === peer) panel.clearAction() }
    }
    ColumnLayout {
        id: contents
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10
        RowLayout {
            Text { text: "Friends and private messages"; color: panel.textTone; font.pixelSize: 17; font.bold: true; Layout.fillWidth: true }
            Button { text: "Refresh"; objectName: "friendsRefresh"; enabled: panel.account.signedIn && !panel.social.busy; onClicked: panel.social.refresh() }
            Button { text: "Cancel request"; objectName: "friendsCancelOperation"; visible: panel.social.busy; onClicked: panel.social.cancel() }
        }
        TextEdit {
            Layout.fillWidth: true
            text: panel.account.handle.length ? "Your handle: @" + panel.account.handle : "Ask your friend for their exact account handle."
            textFormat: TextEdit.PlainText
            readOnly: true
            selectByMouse: true
            color: panel.mutedTone
        }
        Label { text: panel.social.status; textFormat: Text.PlainText; wrapMode: Text.Wrap; Layout.fillWidth: true; color: panel.mutedTone }
        RowLayout {
            TextField {
                id: handleInput
                objectName: "friendHandle"
                Layout.fillWidth: true
                placeholderText: "Exact friend handle (without @)"
                maximumLength: 32
                enabled: panel.account.signedIn && !panel.social.busy
                onAccepted: { if (requestButton.enabled) panel.social.sendRequest(text.trim()) }
            }
            Button {
                id: requestButton
                objectName: "friendSendRequest"
                text: "Send request"
                enabled: panel.social.verified && !panel.social.busy && handleInput.text.trim().length >= 3
                onClicked: panel.social.sendRequest(handleInput.text.trim())
            }
        }
        TabBar {
            id: tabs
            objectName: "friendsTabs"
            Layout.fillWidth: true
            TabButton { text: "Friends (" + panel.social.friends.length + ")" }
            TabButton { text: "Requests (" + panel.social.requests.length + ")" }
            TabButton { text: "Blocked (" + panel.social.blocks.length + ")" }
            TabButton { text: "Conversations" }
        }
        ListView {
            id: list
            objectName: "friendsList"
            Layout.fillWidth: true
            Layout.preferredHeight: 230
            clip: true
            spacing: 6
            boundsBehavior: Flickable.StopAtBounds
            model: tabs.currentIndex === 0 ? panel.social.friends : tabs.currentIndex === 1 ? panel.social.requests : tabs.currentIndex === 2 ? panel.social.blocks : panel.social.conversations
            ScrollBar.vertical: ScrollBar {}
            Label { anchors.centerIn: parent; visible: list.count === 0; text: panel.social.busy ? "Loading…" : "Nothing to show here."; color: panel.mutedTone }
            delegate: Rectangle {
                id: contact
                required property var modelData
                readonly property bool incoming: modelData.receiver_id === panel.account.userId
                width: list.width - 12
                implicitHeight: row.implicitHeight + 16
                radius: 4
                color: "#111925"
                RowLayout {
                    id: row
                    anchors.fill: parent
                    anchors.margins: 8
                    Text {
                        Layout.fillWidth: true
                        text: (contact.modelData.display_name || "Private conversation") + (contact.modelData.handle ? " · @" + contact.modelData.handle : "") + (tabs.currentIndex === 1 ? contact.incoming ? " · incoming" : " · outgoing" : "")
                        textFormat: Text.PlainText
                        color: panel.textTone
                        elide: Text.ElideRight
                    }
                    Button { text: "Message"; visible: tabs.currentIndex === 0; enabled: panel.social.verified && !panel.social.busy; onClicked: panel.social.openConversation(contact.modelData.user_id) }
                    Button { text: "Remove"; visible: tabs.currentIndex === 0; enabled: panel.social.verified && !panel.social.busy; onClicked: panel.ask("remove", contact.modelData.user_id) }
                    Button { text: "Accept"; visible: tabs.currentIndex === 1 && contact.incoming; enabled: panel.social.verified && !panel.social.busy; onClicked: panel.social.acceptRequest(contact.modelData.id) }
                    Button { text: "Decline"; visible: tabs.currentIndex === 1 && contact.incoming; enabled: panel.social.verified && !panel.social.busy; onClicked: panel.social.declineRequest(contact.modelData.id) }
                    Button { text: "Cancel"; visible: tabs.currentIndex === 1 && !contact.incoming; enabled: panel.social.verified && !panel.social.busy; onClicked: panel.social.cancelRequest(contact.modelData.id) }
                    Button { text: "Block"; visible: tabs.currentIndex <= 1; enabled: panel.social.verified && !panel.social.busy; onClicked: panel.ask("block", contact.modelData.user_id) }
                    Button { text: "Unblock"; visible: tabs.currentIndex === 2; enabled: panel.social.verified && !panel.social.busy; onClicked: panel.ask("unblock", contact.modelData.user_id) }
                    Button { text: "Open"; visible: tabs.currentIndex === 3; enabled: panel.social.verified && !panel.social.busy; onClicked: panel.social.chooseConversation(contact.modelData.id) }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Text { Layout.fillWidth: true; text: panel.social.conversationId.length ? "Private chat: " + panel.social.peerName : "Open a friend's private conversation to message."; textFormat: Text.PlainText; color: panel.mutedTone; elide: Text.ElideRight }
            Button { text: "Close chat"; objectName: "closeDirectChat"; visible: panel.social.conversationId.length > 0; onClicked: panel.social.closeConversation() }
        }
    }
    Dialog {
        id: confirmation
        objectName: "friendConfirmation"
        title: panel.action === "block" ? "Block this user?" : panel.action === "remove" ? "Remove this friend?" : "Unblock this user?"
        modal: true
        width: Math.max(240, Math.min(420, panel.width - 32))
        anchors.centerIn: parent
        standardButtons: Dialog.Ok | Dialog.Cancel
        contentItem: Label {
            text: panel.action === "unblock" ? "This allows new requests. Friendship and private-message access are not restored automatically." : "Both users will lose access to this private conversation. Restoring access requires a new accepted friend request. Shared community channels are unaffected."
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }
        onAccepted: {
            if (panel.account.signedIn && panel.account.userId === panel.actionSession && panel.social.verified && !panel.social.busy) {
                if (panel.action === "block") panel.social.blockUser(panel.actionUser)
                else if (panel.action === "remove") panel.social.removeFriend(panel.actionUser)
                else if (panel.action === "unblock") panel.social.unblockUser(panel.actionUser)
            }
            panel.action = ""
            panel.actionUser = ""
        }
        onRejected: { panel.action = ""; panel.actionUser = "" }
    }
}
