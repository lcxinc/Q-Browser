import QtQuick
import QtQuick.Layouts
import Company.Design
import "../models" as Models

Item {
    id: root
    property var runtime: null
    readonly property alias model: dataModel
    function applyTheme() { Theme.dark = dataModel.themeName === "dark" }
    function loadSettings() { dataModel.loadSettings() }
    function setTheme(name) { dataModel.persistTheme(name); applyTheme() }
    Accessible.name: "Settings"; Accessible.role: Accessible.Pane
    Models.RuntimeModels {
        id: dataModel; runtime: root.runtime
        onThemeNameChanged: root.applyTheme()
    }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: Spacing.lg; spacing: Spacing.md
        Text { text: "Settings"; color: Theme.textPrimary; font.family: Typography.family; font.pixelSize: Typography.heading; font.weight: Typography.boldWeight }
        AppCard {
            Layout.fillWidth: true; accessibleName: "Appearance settings"
            ColumnLayout {
                anchors.fill: parent; spacing: Spacing.sm
                Text { text: "Theme"; color: Theme.textPrimary; font.family: Typography.family; font.pixelSize: Typography.headingSmall }
                RowLayout { spacing: Spacing.sm; AppButton { text: "Light"; enabled: dataModel.themeName !== "light"; accessibleDescription: "Use light theme"; onClicked: root.setTheme("light") } AppButton { text: "Dark"; enabled: dataModel.themeName !== "dark"; accessibleDescription: "Use dark theme"; onClicked: root.setTheme("dark") } AppStatusBadge { text: dataModel.themeName; status: "neutral"; accessibleDescription: "Current theme" } }
            }
        }
        Text { Layout.fillWidth: true; text: dataModel.settingsMessage; color: Theme.textSecondary; font.family: Typography.family; font.pixelSize: Typography.body; Accessible.name: text; Accessible.role: Accessible.StaticText }
        Item { Layout.fillHeight: true }
    }
}
