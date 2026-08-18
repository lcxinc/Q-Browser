import QtQuick
import QtTest
import Company.Design

TestCase {
    name: "AppTextField"
    when: windowShown

    Component {
        id: fieldComponent

        AppTextField {
            label: "Email"
            required: true
            requiredMessage: "Email is required"
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

    function test_requiredValidationIsDeterministic() {
        const field = createTemporaryObject(fieldComponent, this)
        verify(field)

        verify(!field.validate())
        verify(field.hasError)
        compare(field.validationMessage, "Email is required")

        field.text = "person@example.test"
        verify(field.validate())
        verify(!field.hasError)
        compare(field.validationMessage, "")
    }

    function test_serverValidationCanBeShownAndCleared() {
        const field = createTemporaryObject(fieldComponent, this,
                                             { "text": "duplicate@example.test" })
        verify(field)

        field.externalError = "Account already exists"
        verify(!field.validate())
        compare(field.validationMessage, "Account already exists")

        field.externalError = ""
        verify(field.validate())
        compare(field.validationMessage, "")
    }

    function test_labelProvidesAccessibleName() {
        const field = createTemporaryObject(fieldComponent, this)
        verify(field)

        verify(field.Accessible.ignored)
        compare(field.inputControl.Accessible.name, "Email")
        compare(field.inputControl.Accessible.role, Accessible.EditableText)
        verify(field.inputControl.Accessible.focusable)

        verify(!field.validate())
        compare(field.inputControl.Accessible.description, "Email is required")
    }

    function test_normalBoundaryHasThreeToOneContrastInBothThemes() {
        const originalDark = Theme.dark
        for (const dark of [false, true]) {
            Theme.dark = dark
            const field = createTemporaryObject(fieldComponent, this)
            verify(field)
            verify(contrast(field.inputControl.background.border.color,
                            field.inputControl.background.color) >= 3.0)
        }
        Theme.dark = originalDark
    }

    function test_disabledFieldIsDistinctAndReadableInBothThemes() {
        const originalDark = Theme.dark
        for (const dark of [false, true]) {
            Theme.dark = dark
            const field = createTemporaryObject(fieldComponent, this,
                                                 { "enabled": false,
                                                   "text": "Unavailable" })
            verify(field)
            verify(!field.inputControl.enabled)
            verify(field.inputControl.background.color.toString()
                   !== Theme.surface.toString())
            verify(contrast(field.inputControl.color,
                            field.inputControl.background.color) >= 4.5)
            verify(contrast(field.inputControl.background.border.color,
                            field.inputControl.background.color) >= 3.0)
        }
        Theme.dark = originalDark
    }
}
