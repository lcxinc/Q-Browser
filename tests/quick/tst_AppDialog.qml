import QtQuick
import QtQuick.Controls.Basic
import QtTest
import Company.Design

TestCase {
    name: "AppDialog"
    when: windowShown

    Component {
        id: dialogComponent

        AppDialog {
            title: "Confirm order"
            accessibleDescription: "Confirm the pending order"
            property alias selectionField: selectionField

            TextField {
                id: selectionField
                text: "Selectable dialog text"
            }
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

    function test_dialogOwnsAccessibleSemanticsAndDarkPalette() {
        const originalDark = Theme.dark
        Theme.dark = true
        const dialog = createTemporaryObject(dialogComponent, this)
        verify(dialog)

        compare(dialog.accessibilityContainer.Accessible.name, "Confirm order")
        compare(dialog.accessibilityContainer.Accessible.description,
                "Confirm the pending order")
        compare(dialog.accessibilityContainer.Accessible.role, Accessible.Dialog)
        compare(dialog.palette.window.toString(), Theme.surface.toString())
        compare(dialog.palette.windowText.toString(), Theme.textPrimary.toString())
        compare(dialog.palette.button.toString(), Theme.primary.toString())
        compare(dialog.palette.buttonText.toString(), Theme.textOnPrimary.toString())
        compare(dialog.selectionField.selectionColor.toString(),
                Theme.focusOnPrimary.toString())
        compare(dialog.selectionField.selectedTextColor.toString(),
                Theme.textOnFocus.toString())
        verify(contrast(dialog.selectionField.selectionColor,
                        dialog.selectionField.selectedTextColor) >= 4.5)

        dialog.open()
        tryVerify(function() { return dialog.opened })
        const acceptButton = dialog.standardButton(Dialog.Ok)
        verify(acceptButton)
        acceptButton.forceActiveFocus()
        verify(acceptButton.activeFocus)
        keyClick(Qt.Key_Tab)
        keyClick(Qt.Key_Backtab)
        tryVerify(function() { return acceptButton.activeFocus })
        verify(acceptButton.activeFocus)
        verify(acceptButton.visualFocus)
        compare(acceptButton.background.border.color.toString(),
                Theme.focusOnPrimary.toString())
        verify(contrast(acceptButton.background.border.color,
                        acceptButton.background.color) >= 3.0)
        dialog.close()
        Theme.dark = originalDark
    }
}
