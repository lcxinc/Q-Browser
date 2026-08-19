pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import Company.Design

Item {
    id: root
    implicitWidth: 1024
    implicitHeight: 720
    Accessible.name: "Migrated index.html"
    Accessible.role: Accessible.Pane

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Spacing.lg
        spacing: Spacing.md
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Spacing.md
            Text {
                Layout.fillWidth: true
                text: "Edit order"
                color: Theme.textPrimary
                font.family: Typography.family
                font.pixelSize: Typography.heading
                font.weight: Typography.boldWeight
                wrapMode: Text.Wrap
                Accessible.name: text
                Accessible.role: Accessible.StaticText
                // Source: index.html:1:88
            }
            GridLayout {
                Layout.fillWidth: true
                columns: 1
                rowSpacing: 8
                columnSpacing: 16
                AppTextField {
                    Layout.fillWidth: true
                    label: "Customer"
                    required: true
                }
                AppTextField {
                    Layout.fillWidth: true
                    label: "Notes"
                    maximumLength: 200
                }
                AppButton {
                    text: "Save"
                }
            }
        }
    }
}
