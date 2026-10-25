import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: panel
    required property var account
    property var preferences: null
    property var probe: null
    property bool registering: false
    property string formError: ""
    property color textTone: "#e7edf5"
    property color mutedTone: "#9aabba"
    color: "#151e2b"
    border.color: "#293649"
    radius: 6
    implicitHeight: fields.implicitHeight + 24

    function clearSecrets() {
        password.text = ""
        confirmation.text = ""
        invitation.text = ""
    }
    function submit() {
        formError = ""
        if (account.signedIn) {
            account.signOut()
            return
        }
        if (registering && password.text !== confirmation.text) {
            formError = "Passwords do not match."
            return
        }
        if (registering)
            account.createAccount(origin.text.trim(), handle.text, displayName.text,
                                  email.text.trim(), password.text, invitation.text.trim())
        else
            account.signIn(origin.text.trim(), email.text.trim(), password.text)
        if (account.busy) clearSecrets()
    }
    Connections {
        target: panel.account
        function onSignedOut() { panel.clearSecrets(); friendEmail.text = "" }
        function onChanged() {
            if (panel.account.signedIn) {
                panel.clearSecrets()
                handle.text = ""
                displayName.text = ""
            }
        }
    }
    ColumnLayout {
        id: fields
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 12
        spacing: 8
        Text { text: "Account"; color: panel.textTone; font.pixelSize: 15 }
        Text {
            Layout.fillWidth: true
            text: panel.formError.length ? panel.formError : panel.account.status
            textFormat: Text.PlainText
            color: panel.mutedTone
            wrapMode: Text.Wrap
            Accessible.role: Accessible.StaticText
        }
        RowLayout {
            visible: !panel.account.signedIn
            Button { text: "Sign in"; checkable: true; checked: !panel.registering; enabled: !panel.account.busy; onClicked: { panel.registering = false; panel.formError = ""; panel.clearSecrets() } }
            Button { text: "Create account"; checkable: true; checked: panel.registering; enabled: !panel.account.busy; onClicked: { panel.registering = true; panel.formError = ""; panel.clearSecrets() } }
        }
        Text {
            visible: !panel.account.signedIn && panel.registering
            Layout.fillWidth: true
            text: "Use the server address and one-use invitation code shared by the owner. The email must match your invitation."
            color: panel.mutedTone
            wrapMode: Text.Wrap
        }
        TextField {
            id: origin; objectName: "accountOrigin"
            Layout.fillWidth: true; visible: !panel.account.signedIn
            enabled: !panel.account.busy; maximumLength: 2048
            placeholderText: "https://your-gcp-server"; Accessible.name: "Server address"
            Component.onCompleted: if (panel.preferences) text = panel.preferences.serverOrigin
            onTextEdited: if (panel.probe) panel.probe.clear()
        }
        CheckBox {
            objectName: "accountRememberServer"
            visible: panel.preferences !== null && !panel.account.signedIn
            text: "Remember this server address on this Windows profile"
            checked: panel.preferences ? panel.preferences.rememberServer : false
            enabled: !panel.account.busy
            onToggled: if (panel.preferences) panel.preferences.rememberServer = checked
        }
        RowLayout {
            visible: panel.probe !== null && !panel.account.signedIn
            Button {
                objectName: "accountCheckConnection"
                text: panel.probe && panel.probe.busy ? "Checking…" : "Check connection"
                enabled: panel.probe !== null && !panel.probe.busy && !panel.account.busy
                onClicked: panel.probe.check(origin.text.trim())
            }
            Button {
                objectName: "accountCancelCheck"
                visible: panel.probe !== null && panel.probe.busy
                text: "Cancel check"
                onClicked: panel.probe.clear()
            }
        }
        Text {
            Layout.fillWidth: true
            visible: panel.probe !== null && !panel.account.signedIn && text.length > 0
            text: panel.probe ? panel.probe.status : ""
            textFormat: Text.PlainText; color: panel.mutedTone; wrapMode: Text.Wrap
        }
        Text {
            Layout.fillWidth: true
            visible: panel.account.reauthenticationRequired
            text: "Your call and screen sessions have stopped. Sign in again, choose your channel and explicitly join or share again."
            textFormat: Text.PlainText; color: panel.mutedTone; wrapMode: Text.Wrap
        }
        TextField {
            id: email; objectName: "accountEmail"
            Layout.fillWidth: true; visible: !panel.account.signedIn
            enabled: !panel.account.busy; maximumLength: 320
            placeholderText: "Email"; Accessible.name: "Email"
            inputMethodHints: Qt.ImhEmailCharactersOnly | Qt.ImhNoPredictiveText
        }
        TextField {
            id: handle; objectName: "accountHandle"
            Layout.fillWidth: true; visible: !panel.account.signedIn && panel.registering
            enabled: !panel.account.busy; maximumLength: 32
            placeholderText: "Handle: 3–32 lowercase letters, digits or _"; Accessible.name: "Handle"
        }
        TextField {
            id: displayName; objectName: "accountDisplayName"
            Layout.fillWidth: true; visible: !panel.account.signedIn && panel.registering
            enabled: !panel.account.busy; maximumLength: 80
            placeholderText: "Display name"; Accessible.name: "Display name"
        }
        TextField {
            id: password; objectName: "accountPassword"
            Layout.fillWidth: true; visible: !panel.account.signedIn
            enabled: !panel.account.busy; maximumLength: 128; echoMode: TextInput.Password
            placeholderText: panel.registering ? "Password: 12–128 UTF-8 bytes" : "Password"
            Accessible.name: "Password"; onAccepted: panel.submit()
        }
        TextField {
            id: confirmation; objectName: "accountConfirmation"
            Layout.fillWidth: true; visible: !panel.account.signedIn && panel.registering
            enabled: !panel.account.busy; maximumLength: 128; echoMode: TextInput.Password
            placeholderText: "Confirm password"; Accessible.name: "Confirm password"
        }
        TextField {
            id: invitation; objectName: "accountInvitation"
            Layout.fillWidth: true; visible: !panel.account.signedIn && panel.registering
            enabled: !panel.account.busy; maximumLength: 64
            placeholderText: "Invitation code"; Accessible.name: "Invitation code"
            inputMethodHints: Qt.ImhNoPredictiveText
        }
        Text {
            visible: panel.account.signedIn
            text: panel.account.displayName + (panel.account.serverOwner ? " · Server owner" : "")
            textFormat: Text.PlainText; color: panel.textTone
        }
        Button {
            objectName: "accountSubmit"
            text: panel.account.signedIn ? "Sign out" : panel.registering ? "Create account" : "Sign in"
            enabled: !panel.account.busy
            onClicked: panel.submit()
        }
        Button {
            visible: panel.account.busy
            text: "Cancel request"
            onClicked: panel.account.signOut()
        }
        ColumnLayout {
            Layout.fillWidth: true
            visible: panel.account.signedIn && panel.account.serverOwner
            spacing: 8
            Text { text: "Invite a friend"; color: panel.textTone; font.pixelSize: 14 }
            TextField {
                id: friendEmail; objectName: "friendEmail"
                Layout.fillWidth: true; enabled: !panel.account.invitationBusy
                maximumLength: 320; placeholderText: "Friend's email"; Accessible.name: "Friend's email"
            }
            RowLayout {
                Button { text: "Generate code"; enabled: !panel.account.invitationBusy && !panel.account.busy; onClicked: panel.account.inviteFriend(friendEmail.text.trim()) }
                Button { text: "Refresh pending"; enabled: !panel.account.invitationBusy; onClicked: panel.account.loadInvitations() }
            }
            Text {
                Layout.fillWidth: true; text: panel.account.invitationStatus
                textFormat: Text.PlainText; color: panel.mutedTone; wrapMode: Text.Wrap
            }
            Text {
                Layout.fillWidth: true; visible: panel.account.invitationCode.length > 0
                text: "Code for " + panel.account.invitationEmail + " · one use, expires in 24 hours"
                textFormat: Text.PlainText; color: panel.mutedTone; wrapMode: Text.Wrap
            }
            TextField {
                id: generatedCode; objectName: "generatedInvitation"
                Layout.fillWidth: true; visible: panel.account.invitationCode.length > 0
                text: panel.account.invitationCode; readOnly: true; selectByMouse: true
                Accessible.name: "Generated invitation code"
            }
            RowLayout {
                visible: panel.account.invitationCode.length > 0
                Button { text: "Copy code"; onClicked: { generatedCode.selectAll(); generatedCode.copy() } }
                Button { text: "Revoke this code"; enabled: !panel.account.invitationBusy; onClicked: panel.account.revokeInvitation() }
            }
            Repeater {
                model: panel.account.invitations
                RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    Text { Layout.fillWidth: true; text: modelData.email; textFormat: Text.PlainText; color: panel.textTone; elide: Text.ElideRight }
                    Button { text: "Revoke"; enabled: !panel.account.invitationBusy; onClicked: panel.account.revokePendingInvitation(modelData.id) }
                }
            }
        }
    }
}
