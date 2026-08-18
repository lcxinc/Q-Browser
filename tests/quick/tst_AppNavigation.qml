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
}
