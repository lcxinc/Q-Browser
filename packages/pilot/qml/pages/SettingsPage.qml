import QtQuick
import QtQuick.Layouts
import Company.Design
import "../models" as Models

Item {
    id: root
    property var runtime: null
    readonly property alias model: dataModel
    readonly property alias retryControl: retryButton
    function applyTheme() { Theme.dark = dataModel.themeName === "dark" }
    function loadSettings() { return dataModel.loadSettings() }
    function setTheme(name) { const changed = dataModel.persistTheme(name); if (changed) applyTheme(); return changed }
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
                RowLayout { spacing: Spacing.sm; AppButton { text: "Light"; enabled: !dataModel.laneBusy("settings") && dataModel.themeName !== "light"; accessibleDescription: "Use light theme"; onClicked: root.setTheme("light") } AppButton { text: "Dark"; enabled: !dataModel.laneBusy("settings") && dataModel.themeName !== "dark"; accessibleDescription: "Use dark theme"; onClicked: root.setTheme("dark") } AppStatusBadge { text: dataModel.themeName; status: dataModel.settingsDirty ? "warning" : "neutral"; accessibleDescription: dataModel.settingsDirty ? "Current theme is not persisted" : "Current theme" } }
            }
        }
        Text { Layout.fillWidth: true; text: dataModel.settingsMessage; color: dataModel.settingsError.length > 0 ? Theme.error : Theme.textSecondary; font.family: Typography.family; font.pixelSize: Typography.body; Accessible.name: text; Accessible.role: dataModel.settingsError.length > 0 ? Accessible.AlertMessage : Accessible.StaticText }
        AppButton { id: retryButton; visible: dataModel.settingsDirty && dataModel.settingsError.length > 0; enabled: !dataModel.laneBusy("settings"); text: "Retry save"; accessibleDescription: "Retry saving the selected theme"; onClicked: dataModel.persistTheme(dataModel.themeName) }
        Item { Layout.fillHeight: true }
    }
}
