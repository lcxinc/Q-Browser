import QtQuick
import QtQuick.Controls.Basic

Dialog {
    id: root

    property string accessibleName: title.length > 0 ? title : "Dialog"
    property string accessibleDescription: ""
    readonly property alias accessibilityContainer: accessibleContent

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    padding: Spacing.lg
    standardButtons: Dialog.Ok | Dialog.Cancel
    dim: true

    palette.window: Theme.surface
    palette.windowText: Theme.textPrimary
    palette.base: Theme.surface
    palette.text: Theme.textPrimary
    palette.button: Theme.primary
    palette.buttonText: Theme.textOnPrimary
    palette.highlight: Theme.focusOnPrimary
    palette.highlightedText: Theme.textOnPrimary

    Overlay.modal: Rectangle {
        color: Theme.scrim
    }

    contentItem: Item {
        id: accessibleContent
        Accessible.name: root.accessibleName
        Accessible.description: root.accessibleDescription
        Accessible.role: Accessible.Dialog
    }

    background: Rectangle {
        radius: Spacing.radius
        color: Theme.surface
        border.width: Spacing.border
        border.color: Theme.border
    }
}
