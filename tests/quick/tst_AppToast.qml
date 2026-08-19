import QtQuick
import QtQuick.Window
import QtTest
import Company.Design

TestCase {
    name: "AppToast"
    when: windowShown

    Component {
        id: toastComponent

        Window {
            width: 360
            height: 160
            visible: true

            property alias toast: toastControl
            property alias toastMessage: toastControl.message
            property alias toastVisible: toastControl.visible

            AppToast {
                id: toastControl
                duration: 160
            }
        }
    }

    function init() {
        AccessibilityRecorder.clear()
    }

    function createToast(initialProperties) {
        const host = createTemporaryObject(toastComponent, null,
                                           initialProperties || {})
        verify(host)
        verify(host.visible)
        return host.toast
    }

    function test_normalAndErrorAnnouncementsUseExpectedPriority() {
        const toast = createToast()
        verify(toast)
        toast.show("Order saved", 500)
        verify(toast.shown)
        compare(toast.Accessible.name, "Order saved")
        compare(toast.Accessible.role, Accessible.AlertMessage)
        compare(AccessibilityRecorder.count, 1)
        compare(AccessibilityRecorder.lastMessage, "Order saved")
        compare(AccessibilityRecorder.lastPoliteness, Accessible.Polite)

        toast.error = true
        toast.show("Save failed", 500)
        compare(AccessibilityRecorder.count, 2)
        compare(AccessibilityRecorder.lastMessage, "Save failed")
        compare(AccessibilityRecorder.lastPoliteness, Accessible.Assertive)
        compare(toast.Accessible.role, Accessible.AlertMessage)
    }

    function test_shownTracksVisibilityAndMessage() {
        const toast = createToast({
                                      "toastMessage": "Declarative",
                                      "toastVisible": true
                                  })
        verify(toast)
        verify(toast.shown)

        toast.visible = false
        verify(!toast.shown)

        toast.visible = true
        toast.message = ""
        verify(!toast.shown)
    }

    function test_emptyMessageIsNotShownOrAnnounced() {
        const toast = createToast()
        verify(toast)

        toast.show("", 500)
        verify(!toast.shown)
        verify(!toast.visible)
        compare(AccessibilityRecorder.count, 0)
    }

    function test_showRestartsTimeoutAndDismissStopsIt() {
        const toast = createToast()
        verify(toast)

        toast.show("First", 160)
        wait(100)
        toast.show("Updated", 160)
        wait(100)
        verify(toast.shown)
        tryVerify(function() { return !toast.shown }, 1000)

        toast.show("Dismiss me", 500)
        verify(toast.shown)
        toast.dismiss()
        verify(!toast.shown)
        verify(!toast.visible)
        wait(550)
        verify(!toast.shown)
    }
}
