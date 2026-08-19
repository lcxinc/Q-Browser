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
                text: "Settings"
                color: Theme.textPrimary
                font.family: Typography.family
                font.pixelSize: Typography.heading
                font.weight: Typography.boldWeight
                wrapMode: Text.Wrap
                Accessible.name: text
                Accessible.role: Accessible.StaticText
                // Source: index.html:1:88
            }
            AppCard {
                Layout.fillWidth: true
                accessibleName: "Appearance Use dark theme"
                ColumnLayout {
                    anchors.fill: parent
                    spacing: Spacing.md
                    Text {
                        Layout.fillWidth: true
                        text: "Appearance"
                        color: Theme.textPrimary
                        font.family: Typography.family
                        font.pixelSize: Typography.heading
                        font.weight: Typography.boldWeight
                        wrapMode: Text.Wrap
                        Accessible.name: text
                        Accessible.role: Accessible.StaticText
                        // Source: index.html:1:114
                    }
                    AppCard {
                        Layout.fillWidth: true
                        accessibleName: "Theme preview"
                        Text {
                            anchors.centerIn: parent
                            text: "[Image] Theme preview"
                            color: Theme.textSecondary
                            font.family: Typography.family
                            font.pixelSize: Typography.body
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: "Use dark theme"
                        color: Theme.textPrimary
                        font.family: Typography.family
                        font.pixelSize: Typography.body
                        font.weight: Typography.normalWeight
                        wrapMode: Text.Wrap
                        Accessible.name: text
                        Accessible.role: Accessible.StaticText
                        // Source: index.html:1:205
                    }
                }
            }
        }
    }
}
