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
    readonly property alias addressText: address.text
    readonly property alias notesText: notes.text
    signal navigateRequested(string route)
    function refresh() { return dataModel.loadOrderForEdit(orderId) }
    function save(status, priority, address, notes) { return dataModel.saveOrder(orderId, status, priority, address, notes) }
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
            errorMessage: dataModel.orderEditServerError
            ColumnLayout {
                anchors.fill: parent; spacing: Spacing.md
                AppNavigation { Layout.fillWidth: true; Layout.preferredHeight: Spacing.touchTarget; orientation: Qt.Horizontal; model: ["pending", "processing", "shipped", "delivered", "cancelled"]; currentIndex: model.indexOf(root.selectedStatus); accessibleName: "Order status"; accessibleDescription: dataModel.orderEditErrors.status; onActivated: (index, value) => root.selectedStatus = String(value) }
                AppNavigation { Layout.fillWidth: true; Layout.preferredHeight: Spacing.touchTarget; orientation: Qt.Horizontal; model: ["normal", "high"]; currentIndex: model.indexOf(root.selectedPriority); accessibleName: "Order priority"; accessibleDescription: dataModel.orderEditErrors.priority; onActivated: (index, value) => root.selectedPriority = String(value) }
                AppTextField { id: address; Layout.fillWidth: true; label: "Shipping address"; required: true; maximumLength: 500; externalError: dataModel.orderEditErrors.shippingAddress }
                AppTextField { id: notes; Layout.fillWidth: true; label: "Notes"; maximumLength: 500 }
                AppButton { text: "Save"; onClicked: root.save(root.selectedStatus, root.selectedPriority, address.text, notes.text) }
            }
        }
        AppToast { id: savedToast; Layout.alignment: Qt.AlignHCenter; accessibleName: dataModel.orderEditMessage }
    }
}
