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
}
