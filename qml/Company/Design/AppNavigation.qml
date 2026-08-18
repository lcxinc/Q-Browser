pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

Control {
    id: root

    property var model: []
    property int currentIndex: -1
    property int orientation: Qt.Vertical
    property string accessibleName: "Navigation"
    property string accessibleDescription: count + " destinations"
    readonly property bool focusVisible: visualFocus
    readonly property int count: navigationList.count

    signal activated(int index, var value)

    function itemAtIndex(index) {
        return navigationList.itemAtIndex(index)
    }

    function valueAt(index) {
        if (Array.isArray(model))
            return model[index]
        if (typeof model === "number")
            return index
        if (model !== null && model !== undefined && typeof model.get === "function")
            return model.get(index)
        return index
    }

    function navigate(index) {
        if (!enabled || !Number.isInteger(index) || index < 0 || index >= count)
            return false
        currentIndex = index
        activated(index, valueAt(index))
        return true
    }

    implicitWidth: orientation === Qt.Vertical ? Spacing.md * 14 : Spacing.md * 30
    implicitHeight: orientation === Qt.Vertical ? Spacing.md * 20 : Spacing.touchTarget
    activeFocusOnTab: true
    Accessible.name: accessibleName
    Accessible.description: accessibleDescription
    Accessible.role: Accessible.PageTabList
    Accessible.focusable: true

    Keys.onPressed: event => {
        let nextIndex = currentIndex
        if ((orientation === Qt.Vertical && event.key === Qt.Key_Down)
                || (orientation === Qt.Horizontal && event.key === Qt.Key_Right)) {
            nextIndex = Math.min(count - 1, currentIndex + 1)
        } else if ((orientation === Qt.Vertical && event.key === Qt.Key_Up)
                   || (orientation === Qt.Horizontal && event.key === Qt.Key_Left)) {
            nextIndex = Math.max(0, currentIndex - 1)
        } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                   && currentIndex >= 0) {
            nextIndex = currentIndex
        } else {
            return
        }
        if (navigate(nextIndex))
            event.accepted = true
    }

    contentItem: ListView {
        id: navigationList
        orientation: root.orientation
        model: root.model
        currentIndex: root.currentIndex
        reuseItems: true
        boundsBehavior: Flickable.StopAtBounds

        delegate: ItemDelegate {
            id: destination
            required property int index
            required property var modelData
            width: root.orientation === Qt.Vertical ? navigationList.width : implicitWidth
            height: Spacing.touchTarget
            text: typeof modelData === "object" && modelData !== null
                    && typeof modelData.label !== "undefined"
                    ? String(modelData.label) : String(modelData)
            highlighted: root.currentIndex === index
            font.family: Typography.family
            font.pixelSize: Typography.body
            Accessible.name: text
            Accessible.role: Accessible.PageTab
            Accessible.selectable: true
            Accessible.selected: highlighted
            onClicked: root.navigate(index)

            contentItem: Text {
                text: destination.text
                color: destination.enabled ? Theme.textPrimary : Theme.disabled
                font: destination.font
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }

            background: Rectangle {
                color: destination.highlighted ? Theme.surfaceRaised : Theme.surface
                border.width: destination.visualFocus ? Spacing.focusRing : 0
                border.color: Theme.focus
            }
        }
    }

    background: Rectangle {
        color: Theme.surface
        border.width: root.focusVisible ? Spacing.focusRing : Spacing.border
        border.color: root.focusVisible ? Theme.focus : Theme.border
    }
}
