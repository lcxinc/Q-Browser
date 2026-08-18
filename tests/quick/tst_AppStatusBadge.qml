import QtQuick
import QtTest
import Company.Design

TestCase {
    name: "AppStatusBadge"
    when: windowShown

    Component {
        id: badgeComponent

        AppStatusBadge {
            text: "Pending"
        }
    }

    function linearChannel(channel) {
        const normalized = channel / 255.0
        return normalized <= 0.04045 ? normalized / 12.92
                                       : Math.pow((normalized + 0.055) / 1.055, 2.4)
    }

    function luminance(color) {
        const hex = color.toString().substring(1)
        return 0.2126 * linearChannel(parseInt(hex.substring(0, 2), 16))
                + 0.7152 * linearChannel(parseInt(hex.substring(2, 4), 16))
                + 0.0722 * linearChannel(parseInt(hex.substring(4, 6), 16))
    }

    function contrast(first, second) {
        const firstLuminance = luminance(first)
        const secondLuminance = luminance(second)
        return (Math.max(firstLuminance, secondLuminance) + 0.05)
                / (Math.min(firstLuminance, secondLuminance) + 0.05)
    }

    function test_allStatusesAreAccessibleAndReadableInBothThemes() {
        const originalDark = Theme.dark
        for (const dark of [false, true]) {
            Theme.dark = dark
            for (const status of ["neutral", "success", "warning", "error"]) {
                const badge = createTemporaryObject(badgeComponent, this,
                                                    { "status": status,
                                                      "text": status + " status" })
                verify(badge)
                compare(badge.Accessible.name, status + " status")
                compare(badge.Accessible.role, Accessible.StaticText)
                verify(contrast(badge.contentItem.color,
                                badge.background.color) >= 4.5)
                verify(contrast(badge.background.border.color,
                                badge.background.color) >= 3.0)
            }
        }
        Theme.dark = originalDark
    }
}
