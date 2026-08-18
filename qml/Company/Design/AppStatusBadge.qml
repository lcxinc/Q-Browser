import QtQuick
import QtQuick.Controls.Basic

Control {
    id: root

    property string text: ""
    property string status: "neutral"
    property string accessibleName: text.length > 0 ? text : normalizedStatus
    property string accessibleDescription: normalizedStatus + " status"
    readonly property string normalizedStatus: ["neutral", "success", "warning", "error"]
                                                   .includes(status) ? status : "neutral"
    readonly property color badgeBackground: normalizedStatus === "success"
                                                  ? Theme.statusSuccessBackground
                                                  : normalizedStatus === "warning"
                                                    ? Theme.statusWarningBackground
                                                    : normalizedStatus === "error"
                                                      ? Theme.statusErrorBackground
                                                      : Theme.statusNeutralBackground
    readonly property color badgeText: normalizedStatus === "success"
                                            ? Theme.statusSuccessText
                                            : normalizedStatus === "warning"
                                              ? Theme.statusWarningText
                                              : normalizedStatus === "error"
                                                ? Theme.statusErrorText
                                                : Theme.statusNeutralText
    readonly property color badgeBorder: normalizedStatus === "success"
                                              ? Theme.statusSuccessBorder
                                              : normalizedStatus === "warning"
                                                ? Theme.statusWarningBorder
                                                : normalizedStatus === "error"
                                                  ? Theme.statusErrorBorder
                                                  : Theme.statusNeutralBorder

    implicitWidth: contentItem.implicitWidth + leftPadding + rightPadding
    implicitHeight: Math.max(Spacing.controlHeight / 2,
                             contentItem.implicitHeight + topPadding + bottomPadding)
    leftPadding: Spacing.sm
    rightPadding: Spacing.sm
    topPadding: Spacing.xs
    bottomPadding: Spacing.xs

    Accessible.name: accessibleName
    Accessible.description: accessibleDescription
    Accessible.role: Accessible.StaticText

    contentItem: Text {
        text: root.text.length > 0 ? root.text : root.normalizedStatus
        color: root.badgeText
        font.family: Typography.family
        font.pixelSize: Typography.label
        font.weight: Typography.mediumWeight
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        radius: Spacing.radius
        color: root.badgeBackground
        border.width: Spacing.border
        border.color: root.badgeBorder
    }
}
