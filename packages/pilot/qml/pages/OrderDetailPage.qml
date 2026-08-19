import QtQuick
import QtQuick.Layouts
import Company.Design
import "../models" as Models

Item {
    id: root
    property var runtime: null
    property string orderId: ""
    readonly property alias model: dataModel
    signal navigateRequested(string route)
    function refresh() { dataModel.loadOrder(orderId) }
    Accessible.name: "Order " + orderId
    Accessible.role: Accessible.Pane
    Models.RuntimeModels { id: dataModel; runtime: root.runtime }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: Spacing.lg; spacing: Spacing.md
        RowLayout { Layout.fillWidth: true; Text { Layout.fillWidth: true; text: "Order " + root.orderId; color: Theme.textPrimary; font.family: Typography.family; font.pixelSize: Typography.heading; font.weight: Typography.boldWeight } AppButton { text: "Edit"; onClicked: root.navigateRequested("/orders/" + encodeURIComponent(root.orderId) + "/edit") } }
        StateView {
            Layout.fillWidth: true; Layout.fillHeight: true; viewState: dataModel.orderDetailState; errorMessage: dataModel.orderDetailError; emptyMessage: "Load order details"
            AppCard { anchors.fill: parent; accessibleName: "Order details"; ColumnLayout { anchors.fill: parent; spacing: Spacing.sm; Text { text: dataModel.order.customerName || ""; color: Theme.textPrimary; font.family: Typography.family; font.pixelSize: Typography.headingSmall } AppStatusBadge { text: dataModel.order.status || "unknown"; status: dataModel.order.status === "delivered" ? "success" : "neutral" } Text { text: dataModel.order.shippingAddress || ""; color: Theme.textSecondary; font.family: Typography.family; font.pixelSize: Typography.body; wrapMode: Text.Wrap } } }
        }
    }
}
