import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: panel
    required property var chat
    required property var account
    property color textTone: "#e7edf5"
    property color mutedTone: "#9aabba"
    property string editingId: ""
    property string deleteId: ""
    property string actionScope: ""
    color: "#151e2b"
    radius: 6
    implicitHeight: contents.implicitHeight + 32

    function clearActions() {
        editingId = ""
        editBody.text = ""
        deleteId = ""
        confirmDelete.close()
    }
    function currentAction() {
        return actionScope === chat.scopeKey
    }
    function beginEdit(id, body) {
        actionScope = chat.scopeKey
        editingId = id
        editBody.text = body
        editBody.forceActiveFocus()
    }
    function askDelete(id) {
        actionScope = chat.scopeKey
        deleteId = id
        confirmDelete.open()
    }
    Connections {
        target: panel.account
        function onSignedOut() { panel.clearActions() }
    }
    Connections {
        target: panel.chat
        function onScopeChanged() { panel.clearActions() }
        function onDraftChanged() { composer.text = panel.chat.draft }
        function onHistoryReset() { Qt.callLater(function() { history.positionViewAtEnd() }) }
        function onAccessRevoked() { panel.clearActions() }
        function onMutationFinished(id, success) { if (success && panel.editingId === id) panel.clearActions() }
    }
    ColumnLayout {
        id: contents
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10
        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: panel.chat.title
                textFormat: Text.PlainText
                color: panel.textTone
                font.pixelSize: 17
                font.bold: true
                elide: Text.ElideRight
            }
            Label { text: panel.chat.live ? "Live" : "Reconnecting"; color: panel.mutedTone }
            Button { text: "Refresh"; objectName: "chatRefresh"; enabled: panel.chat.scopeId.length > 0; onClicked: panel.chat.refreshLatest() }
        }
        Label {
            Layout.fillWidth: true
            text: panel.chat.status
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: panel.mutedTone
        }
        RowLayout {
            Button { text: "Older messages"; objectName: "chatOlder"; enabled: panel.chat.hasOlder && !panel.chat.loading; onClicked: panel.chat.loadOlder() }
            Button {
                text: panel.chat.newerAvailable ? "New messages · back to latest" : "Back to latest"
                objectName: "chatLatest"
                visible: panel.chat.browsingOlder || panel.chat.newerAvailable
                onClicked: panel.chat.refreshLatest()
            }
            Item { Layout.fillWidth: true }
            BusyIndicator { running: panel.chat.loading; visible: running; implicitWidth: 24; implicitHeight: 24 }
        }
        ListView {
            id: history
            objectName: "chatHistory"
            Layout.fillWidth: true
            Layout.preferredHeight: 330
            clip: true
            spacing: 6
            model: panel.chat.messages
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            Label {
                anchors.centerIn: parent
                visible: history.count === 0 && !panel.chat.loading
                text: panel.chat.scopeId.length ? "No messages to show." : (panel.chat.direct ? "Choose a private conversation to start." : "Choose a text channel to start.")
                color: panel.mutedTone
            }
            delegate: Rectangle {
                id: message
                required property string messageId
                required property string authorId
                required property string authorName
                required property string bodyText
                required property double createdAtMs
                required property bool edited
                required property bool deleted
                width: history.width - 12
                implicitHeight: rowContents.implicitHeight + 16
                radius: 4
                color: "#111925"
                ColumnLayout {
                    id: rowContents
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 5
                    RowLayout {
                        Layout.fillWidth: true
                        Text { Layout.fillWidth: true; text: message.authorName; textFormat: Text.PlainText; color: panel.textTone; font.bold: true; elide: Text.ElideRight }
                        Text { text: new Date(message.createdAtMs).toLocaleString(Qt.locale(), Locale.ShortFormat) + (message.edited ? " · edited" : ""); color: panel.mutedTone; font.pixelSize: 10 }
                        Button {
                            text: "Edit"
                            visible: !message.deleted && message.authorId === panel.account.userId
                            enabled: panel.chat.canMutate
                            onClicked: panel.beginEdit(message.messageId, message.bodyText)
                        }
                        Button {
                            text: "Delete"
                            visible: !message.deleted && (message.authorId === panel.account.userId || panel.chat.canModerate)
                            enabled: panel.chat.canMutate
                            onClicked: panel.askDelete(message.messageId)
                        }
                    }
                    TextEdit {
                        Layout.fillWidth: true
                        text: message.bodyText
                        textFormat: TextEdit.PlainText
                        wrapMode: TextEdit.Wrap
                        readOnly: true
                        selectByMouse: true
                        color: message.deleted ? panel.mutedTone : panel.textTone
                        font.pixelSize: 13
                    }
                }
            }
        }
        ColumnLayout {
            visible: panel.editingId.length > 0
            Layout.fillWidth: true
            Label { text: "Edit your message"; color: panel.textTone }
            TextArea {
                id: editBody
                objectName: "chatEditBody"
                Layout.fillWidth: true
                Layout.preferredHeight: 80
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.Wrap
                enabled: !panel.chat.sending
                onTextChanged: { if (length > 4000) remove(4000, length) }
            }
            RowLayout {
                Button { text: "Save edit"; objectName: "chatSaveEdit"; enabled: panel.chat.canMutate && panel.currentAction() && panel.chat.utf8Bytes(editBody.text) <= 4000 && editBody.text.trim().length > 0; onClicked: panel.chat.editMessage(panel.editingId, editBody.text) }
                Button { text: "Cancel edit"; enabled: !panel.chat.sending; onClicked: panel.clearActions() }
            }
        }
        TextArea {
            id: composer
            objectName: "chatComposer"
            Layout.fillWidth: true
            Layout.preferredHeight: 80
            text: panel.chat.draft
            textFormat: TextEdit.PlainText
            wrapMode: TextEdit.Wrap
            enabled: panel.account.signedIn && panel.chat.scopeId.length > 0 && !panel.chat.sending && !panel.chat.retrySend
            placeholderText: "Write a message…"
            onTextChanged: {
                if (length > 4000) remove(4000, length)
                if (!panel.chat.setDraft(text) && text !== panel.chat.draft) text = panel.chat.draft
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Button { text: panel.chat.sending ? "Sending…" : panel.chat.retrySend ? "Retry send" : "Send"; objectName: "chatSend"; enabled: panel.chat.canSubmit; onClicked: panel.chat.sendDraft() }
            Button { text: "Clear draft"; objectName: "chatClearDraft"; enabled: !panel.chat.sending && panel.chat.draft.length > 0; onClicked: panel.chat.clearDraft() }
            Item { Layout.fillWidth: true }
            Label { text: panel.chat.draftBytes + "/4000 bytes"; color: panel.chat.draftBytes > 4000 ? "#f28c8c" : panel.mutedTone }
        }
    }
    Dialog {
        id: confirmDelete
        objectName: "chatDeleteConfirmation"
        title: "Delete this message?"
        modal: true
        width: Math.max(240, Math.min(400, panel.width - 32))
        anchors.centerIn: parent
        standardButtons: Dialog.Ok | Dialog.Cancel
        contentItem: Label { text: "The text will be replaced with a deleted marker."; wrapMode: Text.Wrap }
        onAccepted: { if (panel.currentAction() && panel.chat.canMutate) panel.chat.deleteMessage(panel.deleteId); panel.deleteId = "" }
        onRejected: panel.deleteId = ""
    }
}
