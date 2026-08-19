import QtQuick
import QtQuick.Layouts
import Company.Design
import "../models" as Models

Item {
    id: root
    property var runtime: null
    readonly property alias model: dataModel
    readonly property alias statusControl: statusNavigation
    readonly property alias tableControl: orderTable
    readonly property alias openControl: openButton
    property string selectedStatus: "all"
    signal navigateRequested(string route)
    function search(query, status, page) {
        orderTable.clearSelection()
        dataModel.loadOrders(query, status, page)
    }
    function openOrder(id) {
        if (typeof id !== "string" || id.length === 0) return false
        navigateRequested("/orders/" + encodeURIComponent(id)); return true
    }
    function openSelectedOrder(index) {
        if (dataModel.laneBusy("orders") || !Number.isInteger(index)
                || index < 0 || index >= dataModel.orders.length)
            return false
        return openOrder(dataModel.orders[index].id)
    }
    Accessible.name: "Orders"
    Accessible.role: Accessible.Pane
    Models.RuntimeModels { id: dataModel; runtime: root.runtime }

    ColumnLayout {
        anchors.fill: parent; anchors.margins: Spacing.lg; spacing: Spacing.md
        Text { text: "Orders"; color: Theme.textPrimary; font.family: Typography.family; font.pixelSize: Typography.heading; font.weight: Typography.boldWeight }
        RowLayout {
            Layout.fillWidth: true; spacing: Spacing.sm
            AppTextField { id: query; Layout.fillWidth: true; enabled: !dataModel.laneBusy("orders"); label: "Search"; accessibleName: "Search orders"; placeholderText: "Order or customer"; onAccepted: if (!dataModel.laneBusy("orders")) root.search(text, root.selectedStatus, 1) }
            AppButton { text: "Search"; enabled: !dataModel.laneBusy("orders"); onClicked: root.search(query.text, root.selectedStatus, 1) }
        }
        AppNavigation {
            id: statusNavigation
            Layout.fillWidth: true; Layout.preferredHeight: Spacing.touchTarget
            orientation: Qt.Horizontal
            model: ["all", "pending", "processing", "shipped", "delivered", "cancelled"]
            currentIndex: 0
            enabled: !dataModel.laneBusy("orders")
            accessibleName: "Order status filter"
            onActivated: (index, value) => {
                root.selectedStatus = String(value)
                root.search(dataModel.ordersQuery, root.selectedStatus, 1)
            }
        }
        StateView {
            Layout.fillWidth: true; Layout.fillHeight: true
            viewState: dataModel.ordersState; emptyMessage: "No matching orders"; errorMessage: dataModel.ordersError
            AppTable { id: orderTable; anchors.fill: parent; model: dataModel.orders; accessibleName: "Order results"; emptyText: "No matching orders" }
        }
        RowLayout {
            Layout.fillWidth: true
            AppButton { text: "Previous"; enabled: !dataModel.laneBusy("orders") && dataModel.ordersPage > 1; onClicked: root.search(dataModel.ordersQuery, dataModel.ordersStatus, dataModel.ordersPage - 1) }
            Text { Layout.fillWidth: true; text: "Page " + dataModel.ordersPage + " of " + dataModel.ordersTotalPages; color: Theme.textSecondary; font.family: Typography.family; font.pixelSize: Typography.body; horizontalAlignment: Text.AlignHCenter; Accessible.name: text; Accessible.role: Accessible.StaticText }
            AppButton { id: openButton; text: "Open selected"; enabled: !dataModel.laneBusy("orders") && orderTable.currentIndex >= 0; onClicked: root.openSelectedOrder(orderTable.currentIndex) }
            AppButton { text: "Next"; enabled: !dataModel.laneBusy("orders") && dataModel.ordersPage < dataModel.ordersTotalPages; onClicked: root.search(dataModel.ordersQuery, dataModel.ordersStatus, dataModel.ordersPage + 1) }
        }
    }
}
