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
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Spacing.md
                Text {
                    Layout.fillWidth: true
                    text: "Customer"
                    color: Theme.textPrimary
                    font.family: Typography.family
                    font.pixelSize: Typography.body
                    font.weight: Typography.normalWeight
                    wrapMode: Text.Wrap
                    Accessible.name: text
                    Accessible.role: Accessible.StaticText
                    // Source: index.html:1:113
                }
                AppTextField {
                    Layout.fillWidth: true
                    label: "customer"
                    required: true
                }
                Text {
                    Layout.fillWidth: true
                    text: "Notes"
                    color: Theme.textPrimary
                    font.family: Typography.family
                    font.pixelSize: Typography.body
                    font.weight: Typography.normalWeight
                    wrapMode: Text.Wrap
                    Accessible.name: text
                    Accessible.role: Accessible.StaticText
                    // Source: index.html:1:211
                }
                AppTextField {
                    Layout.fillWidth: true
                    label: ""
                    maximumLength: 200
                }
                AppButton {
                    text: "Save"
                }
            }
        }
    }
}
