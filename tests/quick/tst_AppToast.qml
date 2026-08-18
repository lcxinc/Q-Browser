import QtQuick
import QtTest
import Company.Design

TestCase {
    name: "AppToast"
    when: windowShown

    Component {
        id: toastComponent

        AppToast {
            duration: 160
        }
    }

    SignalSpy {
        id: announcementSpy
        signalName: "announcementRequested"
    }

    function init() {
        announcementSpy.clear()
    }

    function test_normalAndErrorAnnouncementsUseExpectedPriority() {
        const toast = createTemporaryObject(toastComponent, this)
        verify(toast)
        announcementSpy.target = toast

        toast.show("Order saved", 500)
        verify(toast.shown)
        compare(toast.Accessible.name, "Order saved")
        compare(toast.Accessible.role, Accessible.StaticText)
        compare(announcementSpy.count, 1)
        compare(announcementSpy.signalArguments[0][0], "Order saved")
        compare(announcementSpy.signalArguments[0][1], Accessible.Polite)

        toast.error = true
        toast.show("Save failed", 500)
        compare(announcementSpy.count, 2)
        compare(announcementSpy.signalArguments[1][0], "Save failed")
        compare(announcementSpy.signalArguments[1][1], Accessible.Assertive)
    }

    function test_showRestartsTimeoutAndDismissStopsIt() {
        const toast = createTemporaryObject(toastComponent, this)
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
        wait(550)
        verify(!toast.shown)
    }
}
