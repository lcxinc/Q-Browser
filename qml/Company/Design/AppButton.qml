import QtQuick
import QtQuick.Controls.Basic

Button {
    id: root

    property string accessibleName: text
    property string accessibleDescription: ""
    readonly property bool focusVisible: visualFocus

    implicitWidth: Math.max(Spacing.md * 6,
                            contentItem.implicitWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(Spacing.controlHeight,
                             contentItem.implicitHeight + topPadding + bottomPadding)
    leftPadding: Spacing.md
    rightPadding: Spacing.md
    topPadding: Spacing.sm
    bottomPadding: Spacing.sm
    activeFocusOnTab: true

    Accessible.name: accessibleName
    Accessible.description: accessibleDescription
    Accessible.role: Accessible.Button

    Keys.onReturnPressed: event => {
        if (enabled) {
            clicked()
            event.accepted = true
        }
    }
    Keys.onEnterPressed: event => {
        if (enabled) {
            clicked()
            event.accepted = true
        }
    }

    contentItem: Text {
        text: root.text
        color: root.enabled ? Theme.textOnPrimary : Theme.surfaceRaised
        font.family: Typography.family
        font.pixelSize: Typography.body
        font.weight: Typography.mediumWeight
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        radius: Spacing.radius
        color: !root.enabled ? Theme.disabled
                             : root.down ? Theme.primaryPressed : Theme.primary
        border.width: root.focusVisible ? Spacing.focusRing : 0
        border.color: Theme.focus
    }
}
