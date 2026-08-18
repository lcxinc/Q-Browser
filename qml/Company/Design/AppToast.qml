import QtQuick
import QtQuick.Controls.Basic

Control {
    id: root

    property string message: ""
    property int duration: 4000
    property bool error: false
    property string accessibleName: message
    readonly property bool shown: message.length > 0 && dismissTimer.running

    signal announcementRequested(string message, int politeness)

    function announceMessage(text) {
        const politeness = error ? Accessible.Assertive : Accessible.Polite
        announcementRequested(text, politeness)
        Accessible.announce(text, politeness)
    }

    function show(text, timeout) {
        message = text
        if (typeof timeout === "number" && timeout > 0)
            duration = timeout
        visible = true
        dismissTimer.restart()
        announceMessage(text)
    }

    function dismiss() {
        dismissTimer.stop()
        visible = false
    }

    visible: false
    implicitWidth: Math.max(Spacing.md * 15,
                            toastText.implicitWidth + leftPadding + rightPadding)
    implicitHeight: toastText.implicitHeight + topPadding + bottomPadding
    padding: Spacing.md

    Accessible.name: accessibleName
    Accessible.description: error ? "Error notification" : "Notification"
    Accessible.role: Accessible.StaticText

    contentItem: Text {
        id: toastText
        text: root.message
        color: root.error ? Theme.error : Theme.textPrimary
        font.family: Typography.family
        font.pixelSize: Typography.body
        wrapMode: Text.Wrap
    }

    background: Rectangle {
        radius: Spacing.radius
        color: root.error ? Theme.errorSurface : Theme.surfaceRaised
        border.width: Spacing.border
        border.color: root.error ? Theme.error : Theme.border
    }

    Timer {
        id: dismissTimer
        interval: root.duration
        onTriggered: root.visible = false
    }
}
