import QtQuick
import QtQuick.Controls.Basic

Dialog {
    id: root

    property string accessibleName: title.length > 0 ? title : "Dialog"
    property string accessibleDescription: ""

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    padding: Spacing.lg
    standardButtons: Dialog.Ok | Dialog.Cancel

    background: Rectangle {
        radius: Spacing.radius
        color: Theme.surface
        border.width: Spacing.border
        border.color: Theme.border
        Accessible.name: root.accessibleName
        Accessible.description: root.accessibleDescription
        Accessible.role: Accessible.Dialog
    }
}
