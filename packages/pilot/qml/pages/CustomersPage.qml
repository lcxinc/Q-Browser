import QtQuick
import QtQuick.Layouts
import Company.Design
import "../models" as Models

Item {
    id: root
    property var runtime: null
    readonly property alias model: dataModel
    signal navigateRequested(string route)
    function search(query, page) { dataModel.loadCustomers(query, page) }
    function openCustomer(id) { if (typeof id !== "string" || id.length === 0) return false; navigateRequested("/customers/" + encodeURIComponent(id)); return true }
    function openSelectedCustomer(index) {
        if (!Number.isInteger(index) || index < 0 || index >= dataModel.customers.length)
            return false
        return openCustomer(dataModel.customers[index].id)
    }
    Accessible.name: "Customers"; Accessible.role: Accessible.Pane
    Models.RuntimeModels { id: dataModel; runtime: root.runtime }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: Spacing.lg; spacing: Spacing.md
        Text { text: "Customers"; color: Theme.textPrimary; font.family: Typography.family; font.pixelSize: Typography.heading; font.weight: Typography.boldWeight }
        RowLayout { Layout.fillWidth: true; spacing: Spacing.sm; AppTextField { id: query; Layout.fillWidth: true; label: "Search"; accessibleName: "Search customers"; onAccepted: root.search(text, 1) } AppButton { text: "Search"; onClicked: root.search(query.text, 1) } }
        StateView { Layout.fillWidth: true; Layout.fillHeight: true; viewState: dataModel.customersState; errorMessage: dataModel.customersError; emptyMessage: "No matching customers"; AppTable { id: customerTable; anchors.fill: parent; model: dataModel.customers; accessibleName: "Customer results"; emptyText: "No matching customers" } }
        AppButton { text: "Open selected"; enabled: customerTable.currentIndex >= 0; onClicked: root.openSelectedCustomer(customerTable.currentIndex) }
    }
}
