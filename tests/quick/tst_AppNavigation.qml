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

    function test_darkThemeStylesActualDelegateAndSelectionSemantics() {
        const originalDark = Theme.dark
        Theme.dark = true
        const navigation = createTemporaryObject(navigationComponent, this)
        verify(navigation)
        tryVerify(function() { return navigation.itemAtIndex(0) !== null })
        verify(navigation.navigate(0))
        const destination = navigation.itemAtIndex(0)

        compare(destination.contentItem.color.toString(), Theme.textPrimary.toString())
        compare(destination.background.color.toString(), Theme.surfaceRaised.toString())
        verify(destination.Accessible.selectable)
        verify(destination.Accessible.selected)
        Theme.dark = originalDark
    }
}
