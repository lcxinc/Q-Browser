import QtQuick
import QtQuick.Controls.Basic

Control {
    id: root

    property string label: ""
    property string accessibleName: label
    property string accessibleDescription: validationMessage
    property alias text: editor.text
    property alias placeholderText: editor.placeholderText
    property alias echoMode: editor.echoMode
    property alias inputMethodHints: editor.inputMethodHints
    property alias maximumLength: editor.maximumLength
    property alias readOnly: editor.readOnly
    readonly property alias inputControl: editor
    property bool required: false
    property string requiredMessage: label.length > 0 ? label + " is required"
                                                       : "This field is required"
    property string externalError: ""
    property string helperText: ""
    readonly property string validationMessage: externalError.length > 0
                                                   ? externalError : clientError
    readonly property bool hasError: validationMessage.length > 0
    readonly property bool focusVisible: editor.activeFocus
    property string clientError: ""

    signal accepted()

    function validate() {
        clientError = required && text.trim().length === 0 ? requiredMessage : ""
        return !hasError
    }

    implicitWidth: Math.max(Spacing.md * 14,
                            contentColumn.implicitWidth + leftPadding + rightPadding)
    implicitHeight: contentColumn.implicitHeight + topPadding + bottomPadding
    leftPadding: 0
    rightPadding: 0
    topPadding: 0
    bottomPadding: 0

    Accessible.ignored: true

    contentItem: Column {
        id: contentColumn
        spacing: Spacing.xs

        Text {
            visible: root.label.length > 0
            text: root.label
            color: Theme.textPrimary
            font.family: Typography.family
            font.pixelSize: Typography.label
            font.weight: Typography.mediumWeight
        }

        TextField {
            id: editor
            width: parent.width
            implicitHeight: Math.max(Spacing.controlHeight, contentHeight + topPadding + bottomPadding)
            activeFocusOnTab: true
            color: Theme.textPrimary
            placeholderTextColor: Theme.textSecondary
            selectByMouse: true
            font.family: Typography.family
            font.pixelSize: Typography.body
            Accessible.name: root.accessibleName
            Accessible.description: root.accessibleDescription
            Accessible.role: Accessible.EditableText
            Accessible.focusable: true
            onAccepted: root.accepted()
            onTextEdited: root.clientError = ""

            background: Rectangle {
                radius: Spacing.radius
                color: Theme.surface
                border.width: editor.activeFocus ? Spacing.focusRing : Spacing.border
                border.color: root.hasError ? Theme.error
                                                : editor.activeFocus ? Theme.focus : Theme.border
            }
        }

        Text {
            visible: text.length > 0
            text: root.hasError ? root.validationMessage : root.helperText
            color: root.hasError ? Theme.error : Theme.textSecondary
            font.family: Typography.family
            font.pixelSize: Typography.label
            wrapMode: Text.Wrap
        }
    }
}
