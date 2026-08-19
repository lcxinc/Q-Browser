import QtQuick
import QtQuick.Layouts
import Company.Design
import "../models" as Models

Item {
    id: root
    property var runtime: null
    property string orderId: ""
    readonly property alias model: dataModel
    readonly property alias statusControl: statusNavigation
    readonly property alias mutationErrorControl: mutationError
    readonly property bool detailVisible: dataModel.orderDetailState === Models.RuntimeModels.Content
    readonly property bool mutationSaving: dataModel.orderStatusMutationState
                                           === Models.RuntimeModels.MutationSaving
    signal navigateRequested(string route)
    function refresh() {
        if (orderId.length === 0) return false
        dataModel.loadOrder(orderId); return true
    }
    function changeStatus(status) { return dataModel.changeOrderStatus(orderId, status) }
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
        AppNavigation {
            id: statusNavigation
            Layout.fillWidth: true; Layout.preferredHeight: Spacing.touchTarget
            orientation: Qt.Horizontal
            model: ["pending", "processing", "shipped", "delivered", "cancelled"]
            currentIndex: model.indexOf(String(dataModel.order.status || ""))
            enabled: dataModel.orderDetailState === Models.RuntimeModels.Content
                     && !root.mutationSaving
            accessibleName: "Change order status"
            accessibleDescription: dataModel.orderStatusError.length > 0
                                   ? dataModel.orderStatusError : "Select the new order status"
            onActivated: (index, value) => root.changeStatus(String(value))
        }
        Text {
            id: mutationError
            Layout.fillWidth: true
            visible: dataModel.orderStatusMutationState === Models.RuntimeModels.MutationFailure
                     && text.length > 0
            text: dataModel.orderStatusError
            color: Theme.error
            font.family: Typography.family; font.pixelSize: Typography.body
            wrapMode: Text.Wrap
            Accessible.name: text; Accessible.role: Accessible.AlertMessage
        }
        AppToast {
            Layout.alignment: Qt.AlignHCenter
            message: dataModel.orderDetailMessage
            visible: message.length > 0
            accessibleName: message
        }
    }
}
