import QtQuick
import QtQuick.Layouts
import Company.Design
import "../models" as Models

Item {
    id: root
    property var runtime: null
    property string customerId: ""
    readonly property alias model: dataModel
    readonly property alias retryControl: retryButton
    signal navigateRequested(string route)
    function refresh() { dataModel.loadCustomer(customerId) }
    Accessible.name: "Customer " + customerId; Accessible.role: Accessible.Pane
    Models.RuntimeModels { id: dataModel; runtime: root.runtime }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: Spacing.lg; spacing: Spacing.md
        Text { text: "Customer " + root.customerId; color: Theme.textPrimary; font.family: Typography.family; font.pixelSize: Typography.heading; font.weight: Typography.boldWeight }
        StateView { Layout.fillWidth: true; Layout.fillHeight: true; viewState: dataModel.customerDetailState; errorMessage: dataModel.customerDetailError; emptyMessage: "Load customer details"; AppCard { anchors.fill: parent; accessibleName: "Customer details and related orders"; ColumnLayout { anchors.fill: parent; spacing: Spacing.sm; Text { text: dataModel.customer.name || ""; color: Theme.textPrimary; font.family: Typography.family; font.pixelSize: Typography.headingSmall } Text { text: dataModel.customer.email || ""; color: Theme.textSecondary; font.family: Typography.family; font.pixelSize: Typography.body } AppTable { Layout.fillWidth: true; Layout.fillHeight: true; model: dataModel.customer.orders || []; accessibleName: "Related orders"; emptyText: "No related orders" } } } }
        Text { Layout.fillWidth: true; visible: dataModel.customerDetailState === Models.RuntimeModels.Error; text: dataModel.customerDetailError; color: Theme.error; font.family: Typography.family; font.pixelSize: Typography.body; wrapMode: Text.Wrap; Accessible.name: text; Accessible.role: Accessible.AlertMessage }
        AppButton { id: retryButton; visible: dataModel.customerDetailState === Models.RuntimeModels.Error; enabled: !dataModel.laneBusy("customerDetail"); text: "Retry"; accessibleDescription: "Retry loading customer details"; onClicked: root.refresh() }
    }
}
