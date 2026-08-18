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
}
