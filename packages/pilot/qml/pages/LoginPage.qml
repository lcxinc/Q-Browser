import QtQuick
import QtQuick.Layouts
import Company.Design
import "../models" as Models

Item {
    id: root
    property var runtime: null
    readonly property alias model: dataModel
    signal navigateRequested(string route)
    function submitCredentials(email, password) { return dataModel.login(email, password) }

    Accessible.name: "Sign in"
    Accessible.role: Accessible.Pane

    Models.RuntimeModels { id: dataModel; runtime: root.runtime }
    Connections {
        target: dataModel
        function onAuthenticatedChanged() {
            if (dataModel.authenticated) root.navigateRequested("/dashboard")
        }
    }

    AppCard {
        anchors.centerIn: parent
        width: Math.min(parent.width - Spacing.xl, Spacing.md * 28)
        accessibleName: "Pilot sign in"
        ColumnLayout {
            anchors.fill: parent
            spacing: Spacing.md
            Text { text: "Pilot Console"; color: Theme.textPrimary; font.family: Typography.family; font.pixelSize: Typography.heading; font.weight: Typography.boldWeight }
            AppTextField { id: email; Layout.fillWidth: true; enabled: !dataModel.laneBusy("login"); label: "Email"; accessibleName: "Email"; required: true; externalError: dataModel.emailError; inputMethodHints: Qt.ImhEmailCharactersOnly; onAccepted: password.inputControl.forceActiveFocus(Qt.TabFocusReason) }
            AppTextField { id: password; Layout.fillWidth: true; enabled: !dataModel.laneBusy("login"); label: "Password"; accessibleName: "Password"; required: true; externalError: dataModel.passwordError; echoMode: TextInput.Password; onAccepted: if (!dataModel.laneBusy("login")) root.submitCredentials(email.text, password.text) }
            Text { Layout.fillWidth: true; visible: dataModel.serverError.length > 0; text: dataModel.serverError; color: Theme.error; font.family: Typography.family; font.pixelSize: Typography.body; wrapMode: Text.Wrap; Accessible.name: text; Accessible.role: Accessible.AlertMessage }
            AppButton { Layout.fillWidth: true; enabled: !dataModel.laneBusy("login"); text: dataModel.laneBusy("login") ? "Signing in" : "Sign in"; accessibleDescription: "Submit credentials"; onClicked: root.submitCredentials(email.text, password.text) }
        }
    }
}
