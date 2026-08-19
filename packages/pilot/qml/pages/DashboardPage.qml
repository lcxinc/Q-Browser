import QtQuick
import QtQuick.Layouts
import Company.Design
import "../models" as Models

Item {
    id: root
    property var runtime: null
    readonly property alias model: dataModel
    readonly property alias orderCountText: orderCountValue.text
    readonly property alias customerCountText: customerCountValue.text
    readonly property alias pendingCountText: pendingCountValue.text
    readonly property alias revenueText: revenueValue.text
    readonly property var revenueRows: dataModel.dashboard.revenueByMonth || []
    readonly property string revenueAccessibleName: revenueTable.accessibleName
    function refresh() { dataModel.loadDashboard() }
    function metric(name) {
        const kpis = dataModel.dashboard.kpis || ({})
        return kpis[name] === undefined ? 0 : Number(kpis[name])
    }
    function money(cents) {
        return "$" + (Number(cents || 0) / 100).toLocaleString(Qt.locale("en_US"), "f", 2)
    }
    Accessible.name: "Dashboard"
    Accessible.role: Accessible.Pane
    Models.RuntimeModels { id: dataModel; runtime: root.runtime }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Spacing.lg
        spacing: Spacing.md
        RowLayout {
            Layout.fillWidth: true
            Text { Layout.fillWidth: true; text: "Dashboard"; color: Theme.textPrimary; font.family: Typography.family; font.pixelSize: Typography.heading; font.weight: Typography.boldWeight }
            AppButton { text: "Refresh"; onClicked: root.refresh() }
        }
        StateView {
            Layout.fillWidth: true; Layout.fillHeight: true
            viewState: dataModel.dashboardState
            errorMessage: dataModel.dashboardError
            loadingMessage: "Loading dashboard"
            emptyMessage: "No dashboard activity"
            AppCard {
                anchors.fill: parent
                accessibleName: "Dashboard metrics"
                ColumnLayout {
                    anchors.fill: parent; spacing: Spacing.md
                    GridLayout {
                        Layout.fillWidth: true; columns: 4; columnSpacing: Spacing.md
                        Text { text: "Orders"; color: Theme.textSecondary; font.family: Typography.family; font.pixelSize: Typography.label }
                        Text { text: "Customers"; color: Theme.textSecondary; font.family: Typography.family; font.pixelSize: Typography.label }
                        Text { text: "Pending"; color: Theme.textSecondary; font.family: Typography.family; font.pixelSize: Typography.label }
                        Text { text: "Revenue"; color: Theme.textSecondary; font.family: Typography.family; font.pixelSize: Typography.label }
                        Text { id: orderCountValue; text: String(root.metric("orderCount")); color: Theme.textPrimary; font.family: Typography.family; font.pixelSize: Typography.heading; font.weight: Typography.boldWeight; Accessible.name: "Orders " + text; Accessible.role: Accessible.StaticText }
                        Text { id: customerCountValue; text: String(root.metric("customerCount")); color: Theme.textPrimary; font.family: Typography.family; font.pixelSize: Typography.heading; font.weight: Typography.boldWeight; Accessible.name: "Customers " + text; Accessible.role: Accessible.StaticText }
                        Text { id: pendingCountValue; text: String(root.metric("pendingCount")); color: Theme.textPrimary; font.family: Typography.family; font.pixelSize: Typography.heading; font.weight: Typography.boldWeight; Accessible.name: "Pending orders " + text; Accessible.role: Accessible.StaticText }
                        Text { id: revenueValue; text: root.money(root.metric("revenueCents")); color: Theme.textPrimary; font.family: Typography.family; font.pixelSize: Typography.heading; font.weight: Typography.boldWeight; Accessible.name: "Revenue " + text; Accessible.role: Accessible.StaticText }
                    }
                    RowLayout {
                        Layout.fillWidth: true; Layout.fillHeight: true; spacing: Spacing.md
                        AppTable { id: revenueTable; Layout.fillWidth: true; Layout.fillHeight: true; model: root.revenueRows; accessibleName: "Revenue by month"; emptyText: "No monthly revenue" }
                        AppTable { Layout.fillWidth: true; Layout.fillHeight: true; model: dataModel.dashboard.recentActivity || []; accessibleName: "Recent activity"; emptyText: "No recent activity" }
                    }
                }
            }
        }
    }
}
