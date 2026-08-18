import QtQuick
import QtTest
import Company.Design

TestCase {
    name: "AppCard"
    when: windowShown

    Component {
        id: cardComponent

        AppCard {
            accessibleName: "Order summary"
            accessibleDescription: "Three pending items"
        }
    }

    function test_accessibilityAndThemeFollowTokens() {
        const originalDark = Theme.dark
        for (const dark of [false, true]) {
            Theme.dark = dark
            const card = createTemporaryObject(cardComponent, this)
            verify(card)
            compare(card.Accessible.name, "Order summary")
            compare(card.Accessible.description, "Three pending items")
            compare(card.Accessible.role, Accessible.Grouping)
            compare(card.background.color.toString(), Theme.surface.toString())
            compare(card.background.border.color.toString(), Theme.border.toString())
        }
        Theme.dark = originalDark
    }
}
