import QtQuick
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
        }
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
        Theme.dark = originalDark
    }
}
