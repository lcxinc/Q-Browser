import QtQuick
import QtTest
import Company.Design
import "../../../packages/pilot/qml/pages" as Pages
import "../../../packages/pilot/qml/models" as Models
import "../../../packages/pilot/qml/Base64.js" as Base64

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
            function finishId(requestId, response) { capabilityFinished(requestId, response) }
        }
    }
    Component { id: pageComponent; Pages.SettingsPage { width: 640; height: 480 } }
    Component { id: modelComponent; Models.RuntimeModels {} }

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

    function test_saveSupersedesOldLoadAndFailureRemainsRetryable() {
        Theme.dark = false
        const runtime = createTemporaryObject(runtimeComponent, this)
        const page = createTemporaryObject(pageComponent, this, { runtime: runtime })
        verify(runtime && page)
        page.loadSettings()
        const loadId = runtime.lastRequestId
        compare(page.model.settingsLoading, true)
        page.setTheme("dark")
        const saveId = runtime.lastRequestId
        verify(saveId !== loadId)
        compare(page.model.settingsSaving, true)
        compare(page.model.settingsDirty, true)
        runtime.finishId(saveId, { ok: false, error: { code: "storage.failed", message: "Disk busy" } })
        tryCompare(page.model, "settingsSaving", false)
        compare(page.model.themeName, "dark")
        compare(page.model.settingsDirty, true)
        compare(page.model.settingsPersisted, false)
        compare(page.model.settingsError, "Disk busy")

        compare(page.retryControl.Accessible.description, "Retry saving the selected theme")
        page.retryControl.clicked()
        const retryId = runtime.lastRequestId
        runtime.finishId(retryId, { ok: true, result: {} })
        tryCompare(page.model, "settingsPersisted", true)
        compare(page.model.settingsDirty, false)
        runtime.finishId(loadId, { ok: true, result: { value: "light" } })
        wait(0)
        compare(page.model.themeName, "dark")
        compare(page.model.settingsPersisted, true)
    }

    function test_fixedLanesReplacePendingAndWrongPageFailsClosed() {
        const runtime = createTemporaryObject(runtimeComponent, this)
        const model = createTemporaryObject(modelComponent, this, { runtime: runtime })
        verify(runtime && model)
        model.loadOrders("first", "all", 1)
        const firstId = runtime.lastRequestId
        model.loadOrders("second", "pending", 2)
        const secondId = runtime.lastRequestId
        compare(Object.keys(model.pending).length, 1)
        compare(model.pendingCount, 1)
        verify(model.laneBusy("orders"))
        runtime.finishId(firstId, { ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify({ items: [], page: 1, totalPages: 1 })) } })
        wait(0)
        compare(model.ordersState, Models.RuntimeModels.Loading)
        runtime.finishId(secondId, { ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify({ items: [], page: 1, totalPages: 2 })) } })
        tryCompare(model, "ordersState", Models.RuntimeModels.Error)
        compare(model.pendingCount, 0)
        verify(!model.laneBusy("orders"))
    }

    function test_malformedAndWrongEntityResponsesEndInStableErrors() {
        const runtime = createTemporaryObject(runtimeComponent, this)
        const model = createTemporaryObject(modelComponent, this, { runtime: runtime })
        verify(runtime && model)
        model.loadDashboard()
        runtime.finish({ ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify({
                kpis: { orderCount: 1, customerCount: 1, pendingCount: 0, revenueCents: 10 },
                revenueByMonth: [{ month: "2026-08", amountCents: "ten" }],
                recentActivity: []
            })) } })
        tryCompare(model, "dashboardState", Models.RuntimeModels.Error)
        verify(!model.laneBusy("dashboard"))

        model.loadOrder("ORD-EXPECTED")
        runtime.finish({ ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify({ id: "ORD-WRONG", status: "pending" })) } })
        tryCompare(model, "orderDetailState", Models.RuntimeModels.Error)
        model.saveOrder("ORD-EXPECTED", "pending", "normal", "1 Pilot Way", "")
        runtime.finish({ ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify({ id: "ORD-WRONG", status: "pending" })) } })
        tryCompare(model, "orderEditMutationState", Models.RuntimeModels.MutationFailure)

        model.openFile()
        runtime.finish({ ok: true, result: { name: "bad.bin", size: -1 } })
        tryCompare(model, "fileState", Models.RuntimeModels.Error)
        model.loadSettings()
        runtime.finish({ ok: true, result: { value: "remote-script" } })
        tryCompare(model, "settingsLoading", false)
        verify(model.settingsError.length > 0)
        compare(model.pendingCount, 0)
    }

    function test_pageSizeAndPublicArgumentBoundariesFailWithoutThrowing() {
        const runtime = createTemporaryObject(runtimeComponent, this)
        const model = createTemporaryObject(modelComponent, this, { runtime: runtime })
        verify(runtime && model)

        verify(!model.login(null, {}))
        verify(model.emailError.length > 0)
        verify(model.passwordError.length > 0)
        verify(!model.loadOrders(null, {}, "one"))
        verify(!model.loadCustomers([], "one"))
        verify(!model.loadOrder({}))
        verify(!model.loadCustomer(42))
        verify(!model.saveOrder([], {}, [], null, {}))
        compare(runtime.calls, 0)

        verify(model.loadOrders("", "all", 1))
        runtime.finish({ ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify({
                items: [{ id: "ORD-1", customerName: "One" },
                        { id: "ORD-2", customerName: "Two" }],
                page: 1, pageSize: 1, total: 2, totalPages: 2,
                query: "", status: "all"
            })) } })
        tryCompare(model, "ordersState", Models.RuntimeModels.Error)
    }

    function test_errorMessagesAreStringsBoundedTo512Utf16CodeUnits() {
        const runtime = createTemporaryObject(runtimeComponent, this)
        const model = createTemporaryObject(modelComponent, this, { runtime: runtime })
        verify(runtime && model)
        const invalidMessages = [{ injected: true }, ["injected"], "x".repeat(513),
                                 "😀".repeat(257)]
        for (let index = 0; index < invalidMessages.length; ++index) {
            verify(model.loadDashboard())
            runtime.finish({ ok: false, error: {
                code: "transport.failed", message: invalidMessages[index] } })
            tryCompare(model, "dashboardError", "Dashboard failed")
            compare(typeof model.dashboardError, "string")
        }

        verify(model.loadDashboard())
        runtime.finish({ ok: true, result: { status: 500,
            bodyBase64: Base64.encode(JSON.stringify({
                error: { message: { injected: true } }
            })) } })
        tryCompare(model, "dashboardError", "Request failed")

        verify(model.openFile())
        runtime.finish({ ok: false, error: {
            code: "file.cancelled", message: ["injected"] } })
        tryCompare(model, "fileMessage", "No file selected")
        compare(typeof model.fileMessage, "string")

        const exactlyBounded = "😀".repeat(256)
        verify(model.loadSettings())
        runtime.finish({ ok: false, error: {
            code: "storage.failed", message: exactlyBounded } })
        tryCompare(model, "settingsError", exactlyBounded)
        compare(model.settingsError.length, 512)
    }
}
