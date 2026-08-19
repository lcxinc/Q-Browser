import QtQuick
import QtQuick.Layouts
import Company.Design
import "../models" as Models

Item {
    id: root
    property var runtime: null
    readonly property alias model: dataModel
    function openFile() { return dataModel.openFile() }
    Accessible.name: "Files"; Accessible.role: Accessible.Pane
    Models.RuntimeModels { id: dataModel; runtime: root.runtime }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: Spacing.lg; spacing: Spacing.md
        Text { text: "Files"; color: Theme.textPrimary; font.family: Typography.family; font.pixelSize: Typography.heading; font.weight: Typography.boldWeight }
        Text { Layout.fillWidth: true; text: "Files are opened only through the brokered picker."; color: Theme.textSecondary; font.family: Typography.family; font.pixelSize: Typography.body; wrapMode: Text.Wrap }
        AppButton { text: dataModel.laneBusy("file") ? "Opening file" : "Open file"; enabled: !dataModel.laneBusy("file"); onClicked: root.openFile() }
        StateView { Layout.fillWidth: true; Layout.fillHeight: true; viewState: dataModel.fileState; loadingMessage: "Opening file"; emptyMessage: dataModel.fileMessage; errorMessage: dataModel.fileMessage; AppCard { anchors.fill: parent; accessibleName: "Selected file metadata"; ColumnLayout { anchors.fill: parent; spacing: Spacing.sm; Text { text: dataModel.fileMetadata.name || ""; color: Theme.textPrimary; font.family: Typography.family; font.pixelSize: Typography.headingSmall } Text { text: dataModel.fileMetadata.size !== undefined ? dataModel.fileMetadata.size + " bytes" : ""; color: Theme.textSecondary; font.family: Typography.family; font.pixelSize: Typography.body } } } }
    }
}
