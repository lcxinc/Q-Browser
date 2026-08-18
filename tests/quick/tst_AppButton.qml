import QtQuick
import QtTest
import Company.Design

TestCase {
    name: "AppButton"
    when: windowShown

    Component {
        id: buttonComponent

        AppButton {
            text: "Save"
        }
    }

    SignalSpy {
        id: clickedSpy
        signalName: "clicked"
    }

    function init() {
        clickedSpy.clear()
    }

    function linearChannel(channel) {
        const normalized = channel / 255.0
        return normalized <= 0.04045 ? normalized / 12.92
                                       : Math.pow((normalized + 0.055) / 1.055, 2.4)
    }

    function luminance(color) {
        const hex = color.toString().substring(1)
        const red = parseInt(hex.substring(0, 2), 16)
        const green = parseInt(hex.substring(2, 4), 16)
        const blue = parseInt(hex.substring(4, 6), 16)
        return 0.2126 * linearChannel(red)
                + 0.7152 * linearChannel(green)
                + 0.0722 * linearChannel(blue)
    }

    function contrast(first, second) {
        const firstLuminance = luminance(first)
        const secondLuminance = luminance(second)
        const lighter = Math.max(firstLuminance, secondLuminance)
        const darker = Math.min(firstLuminance, secondLuminance)
        return (lighter + 0.05) / (darker + 0.05)
    }

    function test_keyboardActivation() {
        const button = createTemporaryObject(buttonComponent, this)
        verify(button)
        clickedSpy.target = button
        button.forceActiveFocus()

        keyClick(Qt.Key_Return)
        compare(clickedSpy.count, 1)

        keyClick(Qt.Key_Space)
        compare(clickedSpy.count, 2)
    }

    function test_disabledButtonCannotActivate() {
        const button = createTemporaryObject(buttonComponent, this,
                                             { "enabled": false })
        verify(button)
        clickedSpy.target = button
        button.forceActiveFocus()

        keyClick(Qt.Key_Return)
        keyClick(Qt.Key_Space)
        compare(clickedSpy.count, 0)
    }

    function test_keyboardFocusIsVisibleAndAccessible() {
        const button = createTemporaryObject(buttonComponent, this,
                                             { "accessibleName": "Save order" })
        verify(button)
        button.forceActiveFocus(Qt.TabFocusReason)

        verify(button.focusVisible)
        compare(button.Accessible.name, "Save order")
        compare(button.Accessible.role, Accessible.Button)
    }

    function test_lightAndDarkTokensAreDistinct() {
        const originalDark = Theme.dark
        Theme.dark = false
        const lightSurface = Theme.surface.toString()
        const lightText = Theme.textPrimary.toString()
        Theme.dark = true

        verify(Theme.surface.toString() !== lightSurface)
        verify(Theme.textPrimary.toString() !== lightText)
        Theme.dark = originalDark
    }

    function test_primaryTextContrastMeetsWcagInBothThemes() {
        const originalDark = Theme.dark
        for (const dark of [false, true]) {
            Theme.dark = dark
            verify(contrast(Theme.primary, Theme.textOnPrimary) >= 4.5)
            verify(contrast(Theme.primaryPressed, Theme.textOnPrimary) >= 4.5)
        }
        Theme.dark = originalDark
    }
}
