import QtQuick
import QtQuick.Layouts
import Company.Design
import "../models" as Models

Item {
    id: root
    property var runtime: null
    property string orderId: ""
    property string selectedStatus: "pending"
    property string selectedPriority: "normal"
    readonly property alias model: dataModel
    signal navigateRequested(string route)
    function save(status, priority, address, notes) { return dataModel.saveOrder(orderId, status, priority, address, notes) }
    Accessible.name: "Edit order " + orderId
    Accessible.role: Accessible.Pane
    Models.RuntimeModels { id: dataModel; runtime: root.runtime }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: Spacing.lg; spacing: Spacing.md
        Text { text: "Edit " + root.orderId; color: Theme.textPrimary; font.family: Typography.family; font.pixelSize: Typography.heading; font.weight: Typography.boldWeight }
        AppNavigation { Layout.fillWidth: true; Layout.preferredHeight: Spacing.touchTarget; orientation: Qt.Horizontal; model: ["pending", "processing", "shipped", "delivered", "cancelled"]; currentIndex: 0; accessibleName: "Order status"; accessibleDescription: dataModel.orderEditErrors.status; onActivated: (index, value) => root.selectedStatus = String(value) }
        AppNavigation { Layout.fillWidth: true; Layout.preferredHeight: Spacing.touchTarget; orientation: Qt.Horizontal; model: ["normal", "high"]; currentIndex: 0; accessibleName: "Order priority"; accessibleDescription: dataModel.orderEditErrors.priority; onActivated: (index, value) => root.selectedPriority = String(value) }
        AppTextField { id: address; Layout.fillWidth: true; label: "Shipping address"; required: true; maximumLength: 500; externalError: dataModel.orderEditErrors.shippingAddress }
        AppTextField { id: notes; Layout.fillWidth: true; label: "Notes"; maximumLength: 500 }
        Text { Layout.fillWidth: true; visible: dataModel.orderEditServerError.length > 0; text: dataModel.orderEditServerError; color: Theme.error; font.family: Typography.family; font.pixelSize: Typography.body; wrapMode: Text.Wrap; Accessible.name: text; Accessible.role: Accessible.AlertMessage }
        AppButton { text: dataModel.orderEditState === Models.RuntimeModels.Loading ? "Saving" : "Save"; enabled: dataModel.orderEditState !== Models.RuntimeModels.Loading; onClicked: root.save(root.selectedStatus, root.selectedPriority, address.text, notes.text) }
    }
}
