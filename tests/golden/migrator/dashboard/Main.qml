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
        AppNavigation {
            Layout.fillWidth: true
            model: ["Dashboard", "Orders"]
            accessibleName: "Primary"
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Spacing.md
            Text {
                Layout.fillWidth: true
                text: "Operations dashboard"
                color: Theme.textPrimary
                font.family: Typography.family
                font.pixelSize: Typography.heading
                font.weight: Typography.boldWeight
                wrapMode: Text.Wrap
                Accessible.name: text
                Accessible.role: Accessible.StaticText
                // Source: index.html:4:7
            }
            AppCard {
                Layout.fillWidth: true
                accessibleName: "Revenue $42,000 Orders 128"
                GridLayout {
                    anchors.fill: parent
                    columns: 2
                    rowSpacing: 16
                    columnSpacing: 16
                    AppCard {
                        Layout.fillWidth: true
                        accessibleName: "Revenue $42,000"
                        ColumnLayout {
                            anchors.fill: parent
                            spacing: Spacing.md
                            Text {
                                Layout.fillWidth: true
                                text: "Revenue"
                                color: Theme.textPrimary
                                font.family: Typography.family
                                font.pixelSize: Typography.heading
                                font.weight: Typography.boldWeight
                                wrapMode: Text.Wrap
                                Accessible.name: text
                                Accessible.role: Accessible.StaticText
                                // Source: index.html:4:70
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: Spacing.md
                                Text {
                                    Layout.fillWidth: true
                                    text: "$42,000"
                                    color: Theme.textPrimary
                                    font.family: Typography.family
                                    font.pixelSize: Typography.body
                                    font.weight: Typography.normalWeight
                                    wrapMode: Text.Wrap
                                    Accessible.name: text
                                    Accessible.role: Accessible.StaticText
                                    // Source: index.html:4:89
                                }
                            }
                        }
                    }
                    AppCard {
                        Layout.fillWidth: true
                        accessibleName: "Orders 128"
                        ColumnLayout {
                            anchors.fill: parent
                            spacing: Spacing.md
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
                                // Source: index.html:4:119
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: Spacing.md
                                Text {
                                    Layout.fillWidth: true
                                    text: "128"
                                    color: Theme.textPrimary
                                    font.family: Typography.family
                                    font.pixelSize: Typography.body
                                    font.weight: Typography.normalWeight
                                    wrapMode: Text.Wrap
                                    Accessible.name: text
                                    Accessible.role: Accessible.StaticText
                                    // Source: index.html:4:137
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
