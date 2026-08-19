import QtQuick
import QtTest
import Company.Design
import "../../../packages/pilot/qml/pages" as Pages

TestCase {
    name: "PilotSettings"
    Component {
        id: runtimeComponent
        QtObject {
            property int calls: 0
            property string lastRequestId: ""
            property string lastCapability: ""
            property string lastOperation: ""
            property var lastPayload: ({})
            signal capabilityFinished(string requestId, var response)
            function invoke(capability, operation, payload) {
                ++calls; lastRequestId = "r" + calls; lastCapability = capability
                lastOperation = operation; lastPayload = payload; return lastRequestId
            }
            function finish(response) { capabilityFinished(lastRequestId, response) }
        }
    }
    Component { id: pageComponent; Pages.SettingsPage { width: 640; height: 480 } }

    function test_themeLoadsAndPersistsThroughStorageCapability() {
        Theme.dark = false
        const runtime = createTemporaryObject(runtimeComponent, this)
        const page = createTemporaryObject(pageComponent, this, { runtime: runtime })
        verify(runtime && page)
        page.loadSettings()
        compare(runtime.lastCapability, "storage")
        compare(runtime.lastOperation, "get")
        compare(runtime.lastPayload.key, "theme")
        runtime.finish({ ok: true, result: { value: "dark" } })
        tryCompare(page.model, "themeName", "dark")
        compare(Theme.dark, true)

        page.setTheme("light")
        compare(Theme.dark, false)
        compare(runtime.lastOperation, "set")
        compare(runtime.lastPayload.value, "light")
        runtime.finish({ ok: true, result: {} })
        tryCompare(page.model, "settingsMessage", "Settings saved")
    }
}
