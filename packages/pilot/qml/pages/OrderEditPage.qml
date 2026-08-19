import QtQuick
import QtQuick.Layouts
import Company.Design
import "../models" as Models

Item {
    id: root
    property var runtime: null
    property string orderId: ""
    property string selectedStatus: ""
    property string selectedPriority: ""
    readonly property alias model: dataModel
    readonly property alias statusControl: statusNavigation
    readonly property alias priorityControl: priorityNavigation
    readonly property alias addressControl: address
    readonly property alias notesControl: notes
    readonly property alias saveControl: saveButton
    readonly property alias mutationErrorControl: mutationError
    readonly property alias retryControl: retryButton
    readonly property bool formVisible: dataModel.orderEditState === Models.RuntimeModels.Content
    readonly property bool mutationSaving: dataModel.orderEditMutationState
                                           === Models.RuntimeModels.MutationSaving
    signal navigateRequested(string route)
    function refresh() { return dataModel.loadOrderForEdit(orderId) }
    Accessible.name: "Edit order " + orderId
    Accessible.role: Accessible.Pane
    Models.RuntimeModels { id: dataModel; runtime: root.runtime }
    Connections {
        target: dataModel
        function onOrderEditLoaded() {
            root.selectedStatus = dataModel.editStatus
            root.selectedPriority = dataModel.editPriority
            address.text = dataModel.editShippingAddress
            notes.text = dataModel.editNotes
        }
        function onOrderSaved(savedId) {
            savedToast.show(dataModel.orderEditMessage)
            root.navigateRequested("/orders/" + encodeURIComponent(savedId || root.orderId))
        }
    }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: Spacing.lg; spacing: Spacing.md
        Text { text: "Edit " + root.orderId; color: Theme.textPrimary; font.family: Typography.family; font.pixelSize: Typography.heading; font.weight: Typography.boldWeight }
        StateView {
            Layout.fillWidth: true; Layout.fillHeight: true
            viewState: dataModel.orderEditState
            loadingMessage: "Loading order"
            emptyMessage: "Load order before editing"
            errorMessage: dataModel.orderEditLoadError
            ColumnLayout {
                id: editForm
                anchors.fill: parent; spacing: Spacing.md
                AppNavigation { id: statusNavigation; Layout.fillWidth: true; Layout.preferredHeight: Spacing.touchTarget; orientation: Qt.Horizontal; model: ["pending", "processing", "shipped", "delivered", "cancelled"]; currentIndex: model.indexOf(root.selectedStatus); enabled: !root.mutationSaving; accessibleName: "Order status"; accessibleDescription: dataModel.orderEditErrors.status; onActivated: (index, value) => root.selectedStatus = String(value) }
                AppNavigation { id: priorityNavigation; Layout.fillWidth: true; Layout.preferredHeight: Spacing.touchTarget; orientation: Qt.Horizontal; model: ["normal", "high"]; currentIndex: model.indexOf(root.selectedPriority); enabled: !root.mutationSaving; accessibleName: "Order priority"; accessibleDescription: dataModel.orderEditErrors.priority; onActivated: (index, value) => root.selectedPriority = String(value) }
                AppTextField { id: address; Layout.fillWidth: true; enabled: !root.mutationSaving; label: "Shipping address"; required: true; maximumLength: 500; externalError: dataModel.orderEditErrors.shippingAddress }
                AppTextField { id: notes; Layout.fillWidth: true; enabled: !root.mutationSaving; label: "Notes"; maximumLength: 500 }
                Text {
                    id: mutationError
                    Layout.fillWidth: true
                    visible: dataModel.orderEditMutationState === Models.RuntimeModels.MutationFailure
                             && text.length > 0
                    text: dataModel.orderEditServerError
                    color: Theme.error
                    font.family: Typography.family; font.pixelSize: Typography.body
                    wrapMode: Text.Wrap
                    Accessible.name: text; Accessible.role: Accessible.AlertMessage
                }
                AppButton {
                    id: saveButton
                    text: root.mutationSaving ? "Saving" : "Save"
                    enabled: !root.mutationSaving
                    onClicked: dataModel.saveOrder(root.orderId, root.selectedStatus,
                                                   root.selectedPriority, address.text, notes.text)
                }
            }
        }
        Text { Layout.fillWidth: true; visible: dataModel.orderEditState === Models.RuntimeModels.Error; text: dataModel.orderEditLoadError; color: Theme.error; font.family: Typography.family; font.pixelSize: Typography.body; wrapMode: Text.Wrap; Accessible.name: text; Accessible.role: Accessible.AlertMessage }
        AppButton { id: retryButton; visible: dataModel.orderEditState === Models.RuntimeModels.Error; enabled: !dataModel.laneBusy("orderEditFlow"); text: "Retry"; accessibleDescription: "Retry loading order for editing"; onClicked: root.refresh() }
        AppToast { id: savedToast; Layout.alignment: Qt.AlignHCenter; accessibleName: dataModel.orderEditMessage }
    }
}
