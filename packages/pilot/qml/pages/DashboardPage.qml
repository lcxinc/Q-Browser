import QtQuick
import QtQuick.Layouts
import Company.Design
import "../models" as Models

Item {
    id: root
    property var runtime: null
    readonly property alias model: dataModel
    function refresh() { dataModel.loadDashboard() }
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
            emptyMessage: "Dashboard is ready to load"
            AppCard {
                anchors.fill: parent
                accessibleName: "Dashboard metrics"
                ColumnLayout {
                    anchors.fill: parent; spacing: Spacing.md
                    Text { text: "Pending orders"; color: Theme.textSecondary; font.family: Typography.family; font.pixelSize: Typography.label }
                    Text { text: dataModel.dashboard.kpis && dataModel.dashboard.kpis.pendingCount !== undefined ? String(dataModel.dashboard.kpis.pendingCount) : "0"; color: Theme.textPrimary; font.family: Typography.family; font.pixelSize: Typography.headingLarge; font.weight: Typography.boldWeight }
                    AppTable { Layout.fillWidth: true; Layout.fillHeight: true; model: dataModel.dashboard.recentActivity || []; accessibleName: "Recent activity"; emptyText: "No recent activity" }
                }
            }
        }
    }
}
