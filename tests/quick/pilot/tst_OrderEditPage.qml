import QtQuick
import QtTest
import "../../../packages/pilot/qml/pages" as Pages
import "../../../packages/pilot/qml/models" as Models
import "../../../packages/pilot/qml/Base64.js" as Base64

TestCase {
    name: "PilotOrderEdit"
    when: windowShown
    Component {
        id: runtimeComponent
        QtObject {
            property int calls: 0
            property string lastRequestId: ""
            property var lastPayload: ({})
            property var requests: ({})
            signal capabilityFinished(string requestId, var response)
            function invoke(capability, operation, payload) {
                ++calls; lastRequestId = "r" + calls; lastPayload = payload
                const next = Object.assign({}, requests); next[lastRequestId] = payload
                requests = next; return lastRequestId
            }
            function finish(response) { capabilityFinished(lastRequestId, response) }
            function finishId(requestId, response) { capabilityFinished(requestId, response) }
        }
    }
    Component { id: pageComponent; Pages.OrderEditPage { width: 640; height: 540; orderId: "ORD-0001" } }
    Component { id: detailComponent; Pages.OrderDetailPage { width: 640; height: 540; orderId: "ORD-0001" } }
    SignalSpy { id: navigationSpy; signalName: "navigateRequested" }

    function init() { navigationSpy.clear() }

    function orderResponse(order) {
        return { ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify(order)) } }
    }

    function test_editLoadsExistingValuesAndIgnoresStaleResponses() {
        const runtime = createTemporaryObject(runtimeComponent, this)
        const page = createTemporaryObject(pageComponent, this, { runtime: runtime })
        verify(runtime && page)
        verify(page.refresh())
        compare(runtime.lastPayload.method, "GET")
        compare(page.model.orderEditState, Models.RuntimeModels.Loading)
        const first = runtime.lastRequestId
        verify(page.refresh())
        const newest = runtime.lastRequestId
        runtime.finishId(first, orderResponse({ id: "ORD-0001", status: "pending",
            priority: "normal", shippingAddress: "Old address", notes: "Old" }))
        compare(page.model.orderEditState, Models.RuntimeModels.Loading)
        runtime.finishId(newest, orderResponse({ id: "ORD-0001", status: "shipped",
            priority: "high", shippingAddress: "120 Market Street", notes: "Dock 3" }))
        tryCompare(page.model, "orderEditState", Models.RuntimeModels.Content)
        compare(page.selectedStatus, "shipped")
        compare(page.selectedPriority, "high")
        compare(page.addressText, "120 Market Street")
        compare(page.notesText, "Dock 3")

        verify(page.refresh())
        runtime.finish({ ok: true, result: { status: 404,
            bodyBase64: Base64.encode(JSON.stringify({ error: { message: "Order missing" } })) } })
        tryCompare(page.model, "orderEditState", Models.RuntimeModels.Error)
        compare(page.model.orderEditServerError, "Order missing")
    }

    function test_clientValidationAndServerErrors() {
        const runtime = createTemporaryObject(runtimeComponent, this)
        const page = createTemporaryObject(pageComponent, this, { runtime: runtime })
        verify(runtime && page)
        navigationSpy.target = page
        verify(!page.save("unsupported", "normal", "", ""))
        verify(page.model.orderEditErrors.status.length > 0)
        compare(runtime.calls, 0)
        verify(page.save("pending", "high", "1 Pilot Way", "Handle carefully"))
        compare(runtime.lastPayload.method, "PATCH")
        compare(JSON.parse(Base64.decode(runtime.lastPayload.bodyBase64)).priority, "high")
        runtime.finish({ ok: true, result: { status: 422,
            bodyBase64: Base64.encode(JSON.stringify({ error: { code: "invalid_order_update",
                                                         message: "Invalid update" } })) } })
        tryCompare(page.model, "orderEditState", Models.RuntimeModels.Error)
        compare(page.model.orderEditServerError, "Invalid update")

        verify(page.save("processing", "normal", "2 Pilot Way", ""))
        runtime.finish({ ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify({ id: "ORD-0001", status: "processing" })) } })
        tryCompare(page.model, "orderEditState", Models.RuntimeModels.Content)
        compare(page.model.order.id, "ORD-0001")
        compare(page.model.orderEditMessage, "Order saved")
        tryCompare(navigationSpy, "count", 1)
        compare(navigationSpy.signalArguments[0][0], "/orders/ORD-0001")
    }

    function test_editGetCannotOverwriteANewerPatchResponse() {
        const runtime = createTemporaryObject(runtimeComponent, this)
        const page = createTemporaryObject(pageComponent, this, { runtime: runtime })
        verify(runtime && page)
        navigationSpy.target = page
        verify(page.refresh())
        const getRequest = runtime.lastRequestId
        verify(page.save("processing", "high", "2 Pilot Way", "Ready"))
        const patchRequest = runtime.lastRequestId
        verify(getRequest !== patchRequest)

        runtime.finishId(getRequest, orderResponse({ id: "ORD-0001", status: "pending",
            priority: "normal", shippingAddress: "Stale address" }))
        compare(page.model.orderEditState, Models.RuntimeModels.Loading)
        compare(page.addressText, "")
        runtime.finishId(patchRequest, orderResponse({ id: "ORD-0001", status: "processing",
            priority: "high", shippingAddress: "2 Pilot Way" }))
        tryCompare(page.model, "orderEditState", Models.RuntimeModels.Content)
        compare(page.model.order.status, "processing")
        compare(page.model.orderEditMessage, "Order saved")
        tryCompare(navigationSpy, "count", 1)
    }

    function test_detailStatusActionKeyboardAccessibilityAndServerError() {
        const runtime = createTemporaryObject(runtimeComponent, this)
        const page = createTemporaryObject(detailComponent, this, { runtime: runtime })
        verify(runtime && page)
        verify(page.refresh())
        compare(page.model.orderDetailState, Models.RuntimeModels.Loading)
        runtime.finish(orderResponse({ id: "ORD-0001", status: "pending",
            customerName: "Acme", shippingAddress: "1 Pilot Way" }))
        tryCompare(page.model, "orderDetailState", Models.RuntimeModels.Content)
        compare(page.statusControl.Accessible.name, "Change order status")
        compare(page.statusControl.Accessible.role, Accessible.PageTabList)

        page.statusControl.forceActiveFocus(Qt.TabFocusReason)
        keyClick(Qt.Key_Right)
        compare(runtime.lastPayload.method, "PATCH")
        compare(JSON.parse(Base64.decode(runtime.lastPayload.bodyBase64)).status, "processing")
        compare(page.model.orderDetailState, Models.RuntimeModels.Loading)
        runtime.finish({ ok: true, result: { status: 422,
            bodyBase64: Base64.encode(JSON.stringify({ error: { message: "Transition rejected" } })) } })
        tryCompare(page.model, "orderDetailState", Models.RuntimeModels.Error)
        compare(page.model.orderDetailError, "Transition rejected")

        page.model.orderDetailState = Models.RuntimeModels.Content
        tryVerify(function() { return page.statusControl.itemAtIndex(2) !== null })
        const shipped = page.statusControl.itemAtIndex(2)
        shipped.clicked()
        compare(JSON.parse(Base64.decode(runtime.lastPayload.bodyBase64)).status, "shipped")
        runtime.finish(orderResponse({ id: "ORD-0001", status: "shipped" }))
        tryCompare(page.model, "orderDetailState", Models.RuntimeModels.Content)
        compare(page.model.order.status, "shipped")
        compare(page.model.orderDetailMessage, "Order status updated")
    }
}
