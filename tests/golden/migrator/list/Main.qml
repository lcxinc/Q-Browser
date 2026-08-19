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
            spacing: 12
            Text {
                Layout.fillWidth: true
                text: "Orders"
                color: Theme.textPrimary
                font.family: Typography.family
                font.pixelSize: Typography.heading
                font.weight: Typography.boldWeight
                wrapMode: Text.Wrap
                Accessible.name: text
                Accessible.role: Accessible.StaticText
                // Source: index.html:1:88
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Spacing.sm
                Text {
                    Layout.fillWidth: true
                    text: "• Draft order"
                    color: Theme.textPrimary
                    font.family: Typography.family
                    font.pixelSize: Typography.body
                    font.weight: Typography.normalWeight
                    wrapMode: Text.Wrap
                    Accessible.name: text
                    Accessible.role: Accessible.StaticText
                    // Source: index.html:1:103
                }
                Text {
                    Layout.fillWidth: true
                    text: "• Paid order"
                    color: Theme.textPrimary
                    font.family: Typography.family
                    font.pixelSize: Typography.body
                    font.weight: Typography.normalWeight
                    wrapMode: Text.Wrap
                    Accessible.name: text
                    Accessible.role: Accessible.StaticText
                    // Source: index.html:1:103
                }
            }
            AppTable {
                Layout.fillWidth: true
                Layout.preferredHeight: Spacing.lg * 8
                model: ["Number Status", "Q-100 Paid"]
                accessibleName: "Migrated table"
            }
        }
    }
}
