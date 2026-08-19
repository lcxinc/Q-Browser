pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

Control {
    id: root

    property var model: []
    property Component rowDelegate
    property string accessibleName: "Table"
    property string accessibleDescription: empty ? emptyText : count + " rows"
    property string emptyText: "No items"
    property int rowHeight: Spacing.touchTarget
    property int cacheBuffer: rowHeight * 2
    readonly property int count: listView.count
    readonly property bool empty: count === 0
    readonly property bool emptyVisible: empty
    readonly property int currentIndex: listView.currentIndex
    readonly property int instantiatedItemCount: listView.contentItem
                                                     ? listView.contentItem.children.length : 0
    readonly property bool virtualizationEnabled: listView.reuseItems

    function select(index) {
        if (!Number.isInteger(index) || index < 0 || index >= count)
            return false
        listView.currentIndex = index
        return true
    }

    function clearSelection() {
        listView.currentIndex = -1
    }

    function itemAtIndex(index) {
        return listView.itemAtIndex(index)
    }

    implicitWidth: Spacing.md * 20
    implicitHeight: Spacing.lg * 10
    activeFocusOnTab: true

    Accessible.name: accessibleName
    Accessible.description: accessibleDescription
    Accessible.role: Accessible.Table
    Accessible.focusable: true

    Keys.onUpPressed: event => {
        if (count > 0) {
            select(Math.max(0, currentIndex - 1))
            event.accepted = true
        }
    }
    Keys.onDownPressed: event => {
        if (count > 0) {
            select(Math.min(count - 1, currentIndex + 1))
            event.accepted = true
        }
    }

    contentItem: Item {
        ListView {
            id: listView
            anchors.fill: parent
            visible: !root.empty
            clip: true
            model: root.model
            currentIndex: -1
            reuseItems: true
            cacheBuffer: root.cacheBuffer
            boundsBehavior: Flickable.StopAtBounds
            delegate: root.rowDelegate ? root.rowDelegate : defaultRowDelegate
        }

        Text {
            id: emptyLabel
            anchors.centerIn: parent
            visible: root.empty
            text: root.emptyText
            color: Theme.textSecondary
            font.family: Typography.family
            font.pixelSize: Typography.body
            Accessible.name: text
            Accessible.role: Accessible.StaticText
        }
    }

    background: Rectangle {
        radius: Spacing.radius
        color: Theme.surface
        border.width: root.visualFocus ? Spacing.focusRing : Spacing.border
        border.color: root.visualFocus ? Theme.focus : Theme.border
    }

    Component {
        id: defaultRowDelegate

        ItemDelegate {
            id: row
            required property int index
            required property var modelData
            width: ListView.view.width
            height: root.rowHeight
            text: typeof modelData === "object" && modelData !== null
                      && typeof modelData.display !== "undefined"
                      ? String(modelData.display) : String(modelData)
            highlighted: root.currentIndex === index
            font.family: Typography.family
            font.pixelSize: Typography.body
            Accessible.name: text
            Accessible.role: Accessible.Row
            Accessible.selectable: true
            Accessible.selected: highlighted
            onClicked: root.select(index)


            contentItem: Text {
                text: row.text
                color: row.enabled ? Theme.textPrimary : Theme.disabled
                font: row.font
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }

            background: Rectangle {
                color: row.highlighted ? Theme.surfaceRaised : Theme.surface
                border.width: row.highlighted || row.visualFocus
                              ? Spacing.focusRing : 0
                border.color: Theme.focus
            }
        }
    }
}
