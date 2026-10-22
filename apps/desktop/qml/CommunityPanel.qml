import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: panel
    required property var directory
    required property bool signedIn
    property color textTone: "#e7edf5"
    property color mutedTone: "#9aabba"
    property string contextId: ""
    property string editChannelId: ""
    property string formError: ""
    property string pendingAction: ""
    property string pendingCommunity: ""
    property string pendingId: ""
    property string pendingName: ""
    property string pendingRole: ""
    color: "#151e2b"
    border.color: "#293649"
    radius: 6
    visible: signedIn
    implicitHeight: fields.implicitHeight + 24

    function clearEditor() {
        editChannelId = ""
        channelEditName.text = ""
        channelEditPosition.value = 0
    }
    function reconcile() {
        if (directory.busy) formError = ""
        if (contextId !== directory.communityId) {
            contextId = directory.communityId
            clearEditor()
            rename.text = directory.communityName
            management.checked = false
            joinCode.text = ""
            formError = ""
            confirmation.close()
        }
        if (editChannelId.length && !directory.allChannels.some(row => row.id === editChannelId)) clearEditor()
    }
    function submitCreate() {
        formError = ""
        directory.createCommunity(newCommunity.text.trim())
        if (directory.busy) newCommunity.text = ""
    }
    function submitJoin() {
        formError = ""
        directory.joinCommunity(joinCode.text.trim())
        if (directory.busy) joinCode.text = ""
    }
    function selectChannel(id) {
        for (const row of directory.allChannels) {
            if (row.id === id) {
                editChannelId = id
                channelEditName.text = row.name
                channelEditPosition.value = row.position
                return
            }
        }
        clearEditor()
    }
    function ask(action, id, name, role) {
        formError = ""
        pendingAction = action
        pendingCommunity = directory.communityId
        pendingId = id
        pendingName = name
        pendingRole = role || ""
        confirmName.text = ""
        confirmation.open()
    }
    function confirmAction() {
        if (!signedIn || directory.busy || pendingCommunity !== directory.communityId) {
            formError = "The community changed. Review the action again."
            return
        }
        if (pendingAction === "deleteCommunity") {
            if (confirmName.text !== pendingName || directory.communityName !== pendingName) {
                formError = "The community name did not match. Nothing was deleted."
                return
            }
            directory.deleteCommunity()
        } else if (pendingAction === "leave") directory.leaveCommunity()
        else if (pendingAction === "deleteChannel") directory.deleteChannel(pendingId)
        else if (pendingAction === "removeMember") directory.removeMember(pendingId)
        else if (pendingAction === "role") directory.setMemberRole(pendingId, pendingRole)
        pendingAction = ""
        confirmName.text = ""
    }
    onSignedInChanged: {
        if (!signedIn) {
            joinCode.text = ""
            newCommunity.text = ""
            clearEditor()
            confirmation.close()
            pendingAction = ""
        }
    }
    Connections {
        target: panel.directory
        function onChanged() { panel.reconcile() }
    }
    ColumnLayout {
        id: fields
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 12
        spacing: 8
        Text { text: "Communities"; color: panel.textTone; font.pixelSize: 15 }
        Text {
            Layout.fillWidth: true
            text: panel.formError.length ? panel.formError : panel.directory.status
            textFormat: Text.PlainText; color: panel.mutedTone; wrapMode: Text.Wrap
        }
        TextField {
            id: newCommunity; objectName: "newCommunityName"
            Layout.fillWidth: true; maximumLength: 200; enabled: !panel.directory.busy
            placeholderText: "New community name"; Accessible.name: "New community name"
            onAccepted: panel.submitCreate()
        }
        Button { text: "Create community"; objectName: "createCommunity"; enabled: !panel.directory.busy; onClicked: panel.submitCreate() }
        TextField {
            id: joinCode; objectName: "communityJoinCode"
            Layout.fillWidth: true; maximumLength: 64; enabled: !panel.directory.busy
            placeholderText: "Community invitation code"; Accessible.name: "Community invitation code"
            inputMethodHints: Qt.ImhNoPredictiveText
            onAccepted: panel.submitJoin()
        }
        Button { text: "Join community"; objectName: "joinCommunity"; enabled: !panel.directory.busy; onClicked: panel.submitJoin() }
        Text {
            Layout.fillWidth: true
            text: "Community codes are for existing GCP accounts. Account invitations are separate. Changing the selected room leaves the current call."
            color: panel.mutedTone; wrapMode: Text.Wrap
        }
        Button { text: "Cancel request"; visible: panel.directory.busy; onClicked: panel.directory.cancel() }
        Button {
            id: management; objectName: "communityManagement"
            visible: panel.directory.communityId.length > 0
            text: checked ? "Hide community details" : "Community details and management"
            checkable: true; enabled: !panel.directory.busy
            onCheckedChanged: {
                if (checked) {
                    rename.text = panel.directory.communityName
                    panel.directory.loadManagement()
                }
            }
        }
        ColumnLayout {
            visible: management.checked && panel.directory.communityId.length > 0
            Layout.fillWidth: true; spacing: 8
            Text {
                Layout.fillWidth: true
                text: panel.directory.communityName + " · " + panel.directory.communityRole
                textFormat: Text.PlainText; color: panel.textTone; wrapMode: Text.Wrap
            }
            Button { text: "Refresh details"; enabled: !panel.directory.busy; onClicked: panel.directory.loadManagement() }
            ColumnLayout {
                objectName: "communityManagerControls"
                visible: panel.directory.canManage
                Layout.fillWidth: true; spacing: 8
                TextField {
                    id: rename; objectName: "renameCommunityName"
                    Layout.fillWidth: true; maximumLength: 200; enabled: !panel.directory.busy
                    placeholderText: "Community name"; Accessible.name: "Rename community"
                }
                Button { text: "Rename community"; objectName: "renameCommunity"; enabled: !panel.directory.busy; onClicked: panel.directory.renameCommunity(rename.text.trim()) }
                Text { text: "Create a channel"; color: panel.textTone }
                TextField {
                    id: channelCreateName; objectName: "newChannelName"
                    Layout.fillWidth: true; maximumLength: 80; enabled: !panel.directory.busy
                    placeholderText: "team-voice"; Accessible.name: "New channel name"
                }
                RowLayout {
                    Layout.fillWidth: true
                    ComboBox { id: channelKind; objectName: "newChannelKind"; model: ["voice", "text"]; enabled: !panel.directory.busy; Accessible.name: "Channel type" }
                    Text { text: "Position"; color: panel.mutedTone }
                    SpinBox { id: channelCreatePosition; objectName: "newChannelPosition"; from: 0; to: 2147483647; editable: true; enabled: !panel.directory.busy; Accessible.name: "New channel position" }
                }
                Button {
                    objectName: "createChannel"; text: "Create channel"; enabled: !panel.directory.busy
                    onClicked: {
                        panel.directory.createChannel(channelCreateName.text.trim(), channelKind.currentText, channelCreatePosition.value)
                        if (panel.directory.busy) channelCreateName.text = ""
                    }
                }
                Text { text: "Edit a channel"; color: panel.textTone }
                IdPicker {
                    objectName: "manageChannelPicker"
                    Layout.fillWidth: true; enabled: !panel.directory.busy
                    options: panel.directory.allChannels; selectedId: panel.editChannelId
                    emptyLabel: "Choose a text or voice channel"
                    onActivated: panel.selectChannel(currentValue)
                }
                TextField {
                    id: channelEditName; objectName: "editChannelName"
                    Layout.fillWidth: true; maximumLength: 80; enabled: !panel.directory.busy && panel.editChannelId.length > 0
                    placeholderText: "Channel name"; Accessible.name: "Rename channel"
                }
                RowLayout {
                    Text { text: "Position"; color: panel.mutedTone }
                    SpinBox { id: channelEditPosition; objectName: "editChannelPosition"; from: 0; to: 2147483647; editable: true; enabled: !panel.directory.busy && panel.editChannelId.length > 0; Accessible.name: "Channel position" }
                }
                RowLayout {
                    Button { text: "Save channel"; objectName: "saveChannel"; enabled: !panel.directory.busy && panel.editChannelId.length > 0; onClicked: panel.directory.updateChannel(panel.editChannelId, channelEditName.text.trim(), channelEditPosition.value) }
                    Button { text: "Delete channel"; enabled: !panel.directory.busy && panel.editChannelId.length > 0; onClicked: panel.ask("deleteChannel", panel.editChannelId, channelEditName.text) }
                }
                Button { text: "Generate community code"; objectName: "generateCommunityCode"; enabled: !panel.directory.busy; onClicked: panel.directory.createInvitation() }
                Text {
                    Layout.fillWidth: true; visible: panel.directory.invitationToken.length > 0
                    text: "One use, valid for 24 hours from creation. Share privately with a signed-in friend."
                    color: panel.mutedTone; wrapMode: Text.Wrap
                }
                TextField {
                    id: generatedCode; objectName: "communityInvitationToken"
                    Layout.fillWidth: true; visible: panel.directory.invitationToken.length > 0
                    text: panel.directory.invitationToken; readOnly: true; selectByMouse: true
                    Accessible.name: "Generated community invitation code"
                }
                RowLayout {
                    visible: panel.directory.invitationToken.length > 0
                    Button { text: "Copy code"; onClicked: { generatedCode.selectAll(); generatedCode.copy() } }
                    Button { text: "Revoke this code"; enabled: !panel.directory.busy; onClicked: panel.directory.revokeInvitation(panel.directory.invitationId) }
                }
                Repeater {
                    model: management.checked ? panel.directory.invitations : []
                    RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        Text { Layout.fillWidth: true; text: "Pending code " + modelData.id.slice(0, 8) + " · " + Math.ceil(modelData.expires_in / 60) + " min at last refresh"; textFormat: Text.PlainText; color: panel.mutedTone; wrapMode: Text.Wrap }
                        Button { text: "Revoke"; enabled: !panel.directory.busy; onClicked: panel.directory.revokeInvitation(modelData.id) }
                    }
                }
            }
            Text { text: "Members"; color: panel.textTone }
            Repeater {
                model: management.checked ? panel.directory.members : []
                ColumnLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    Text { Layout.fillWidth: true; text: modelData.display_name + " (@" + modelData.handle + ") · " + modelData.role; textFormat: Text.PlainText; color: panel.textTone; wrapMode: Text.Wrap }
                    RowLayout {
                        visible: modelData.role !== "owner"
                        Button {
                            visible: panel.directory.communityRole === "owner"
                            text: modelData.role === "admin" ? "Make member" : "Make admin"; enabled: !panel.directory.busy
                            onClicked: panel.ask("role", modelData.user_id, modelData.handle, modelData.role === "admin" ? "member" : "admin")
                        }
                        Button {
                            visible: panel.directory.canManage && (panel.directory.communityRole === "owner" || modelData.role === "member")
                            text: "Remove member"; enabled: !panel.directory.busy
                            onClicked: panel.ask("removeMember", modelData.user_id, modelData.handle)
                        }
                    }
                }
            }
            Text { visible: panel.directory.managementLoaded && panel.directory.members.length === 200; Layout.fillWidth: true; text: "Showing the first 200 members."; color: panel.mutedTone; wrapMode: Text.Wrap }
            Button { text: "Leave community"; visible: panel.directory.communityRole === "admin" || panel.directory.communityRole === "member"; enabled: !panel.directory.busy; onClicked: panel.ask("leave", "", panel.directory.communityName) }
            Button { text: "Delete community"; visible: panel.directory.communityRole === "owner"; enabled: !panel.directory.busy; onClicked: panel.ask("deleteCommunity", panel.directory.communityId, panel.directory.communityName) }
        }
    }
    Dialog {
        id: confirmation; objectName: "communityConfirmation"
        parent: Overlay.overlay
        x: parent ? Math.max(0, (parent.width - width) / 2) : 0
        y: parent ? Math.max(0, (parent.height - height) / 2) : 0
        width: Math.min(420, panel.width)
        modal: true; title: "Confirm community change"
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: panel.confirmAction()
        onRejected: { panel.pendingAction = ""; confirmName.text = "" }
        contentItem: ColumnLayout {
            Text {
                Layout.fillWidth: true; textFormat: Text.PlainText; color: confirmation.palette.windowText; wrapMode: Text.Wrap
                text: panel.pendingAction === "deleteCommunity" ? "Delete " + panel.pendingName + " and all of its channels and messages? Type its name to confirm." :
                      panel.pendingAction === "leave" ? "Leave " + panel.pendingName + "? A new community invitation will be needed to rejoin." :
                      panel.pendingAction === "deleteChannel" ? "Delete channel " + panel.pendingName + " and its message history? An active call in this channel will stop." :
                      panel.pendingAction === "role" ? "Change " + panel.pendingName + " to " + panel.pendingRole + "? Admins can manage channels, invitations and ordinary members." :
                      "Remove " + panel.pendingName + " from this community?"
            }
            TextField { id: confirmName; objectName: "confirmCommunityName"; Layout.fillWidth: true; visible: panel.pendingAction === "deleteCommunity"; maximumLength: 200; placeholderText: "Community name"; Accessible.name: "Confirm community name" }
        }
    }
}
