import QtQuick
import QtTest
import Company.Design

TestCase {
    name: "AppNavigation"
    when: windowShown

    Component {
        id: navigationComponent

        AppNavigation {
            width: 240
            height: 180
            model: ["Dashboard", "Orders", "Settings"]
            accessibleName: "Primary navigation"
        }
    }

    SignalSpy {
        id: activatedSpy
        signalName: "activated"
    }

    ListModel {
        id: destinationModel
        ListElement { label: "Dashboard"; route: "/dashboard" }
        ListElement { label: "Orders"; route: "/orders" }
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

    function init() {
        activatedSpy.clear()
    }

    function test_keyboardNavigationAndFocusVisibility() {
        const navigation = createTemporaryObject(navigationComponent, this)
        verify(navigation)
        activatedSpy.target = navigation
        navigation.forceActiveFocus(Qt.TabFocusReason)

        verify(navigation.focusVisible)
        keyClick(Qt.Key_Down)
        compare(navigation.currentIndex, 0)
        keyClick(Qt.Key_Down)
        compare(navigation.currentIndex, 1)
        keyClick(Qt.Key_Up)
        compare(navigation.currentIndex, 0)
        compare(activatedSpy.count, 3)
        compare(navigation.Accessible.name, "Primary navigation")
        compare(navigation.Accessible.role, Accessible.PageTabList)
    }

    function test_disabledNavigationCannotActivate() {
        const navigation = createTemporaryObject(navigationComponent, this,
                                                 { "enabled": false })
        verify(navigation)
        activatedSpy.target = navigation

        verify(!navigation.navigate(0))
        compare(navigation.currentIndex, -1)
        compare(activatedSpy.count, 0)
    }

    function test_integerAndListModelsFollowListViewContract() {
        const integerNavigation = createTemporaryObject(navigationComponent, this,
                                                        { "model": 3 })
        verify(integerNavigation)
        tryCompare(integerNavigation, "count", 3)
        verify(integerNavigation.navigate(2))
        compare(integerNavigation.currentIndex, 2)

        const objectNavigation = createTemporaryObject(navigationComponent, this,
                                                       { "model": destinationModel })
        verify(objectNavigation)
        activatedSpy.target = objectNavigation
        tryCompare(objectNavigation, "count", 2)
        verify(objectNavigation.navigate(1))
        compare(activatedSpy.signalArguments[0][1].route, "/orders")
    }

    function test_bothThemesStyleActualDelegateAndVisibleSelection() {
        const originalDark = Theme.dark
        for (const dark of [false, true]) {
            Theme.dark = dark
            const navigation = createTemporaryObject(navigationComponent, this)
            verify(navigation)
            tryVerify(function() { return navigation.itemAtIndex(0) !== null })
            verify(navigation.navigate(0))
            const destination = navigation.itemAtIndex(0)

            compare(destination.contentItem.color.toString(), Theme.textPrimary.toString())
            compare(destination.background.color.toString(), Theme.surfaceRaised.toString())
            verify(destination.background.border.width >= Spacing.focusRing)
            verify(contrast(destination.background.border.color,
                            destination.background.color) >= 3.0)
            verify(destination.Accessible.selectable)
            verify(destination.Accessible.selected)
        }
        Theme.dark = originalDark
    }
}
