import QtQuick
import QtQuick.Controls.Basic

Pane {
    id: root

    property string accessibleName: ""
    property string accessibleDescription: ""

    padding: Spacing.md
    Accessible.name: accessibleName
    Accessible.description: accessibleDescription
    Accessible.role: Accessible.Grouping

    background: Rectangle {
        radius: Spacing.radius
        color: Theme.surface
        border.width: Spacing.border
        border.color: Theme.border
    }
}
