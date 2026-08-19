import QtQuick
import QtQuick.Window
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
    Component {
        id: editWindowComponent
        Window {
            id: editWindow
            property var runtime: null
            readonly property alias page: editPage
            width: 720; height: 620; visible: true
            Pages.OrderEditPage {
                id: editPage
                anchors.fill: parent
                runtime: editWindow.runtime
                orderId: "ORD-0001"
            }
        }
    }
    Component {
        id: detailWindowComponent
        Window {
            id: detailWindow
            property var runtime: null
            readonly property alias page: detailPage
            width: 720; height: 620; visible: true
            Pages.OrderDetailPage {
                id: detailPage
                anchors.fill: parent
                runtime: detailWindow.runtime
                orderId: "ORD-0001"
            }
        }
    }
    SignalSpy { id: navigationSpy; signalName: "navigateRequested" }

    function init() { navigationSpy.clear() }

    function activate(window, control) {
        window.requestActivate()
        tryVerify(function() { return window.active })
        control.forceActiveFocus(Qt.TabFocusReason)
        tryVerify(function() { return control.activeFocus })
    }

    function orderResponse(order) {
        const completeOrder = Object.assign({
            id: "ORD-0001", customerId: "CUS-001", customerName: "Acme",
            createdAt: "2026-08-18T09:30:00.000Z",
            updatedAt: "2026-08-18T09:30:00.000Z",
            status: "pending", totalCents: 125000, currency: "USD",
            priority: "normal", shippingAddress: "1 Pilot Way", notes: ""
        }, order)
        return { ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify(completeOrder)) } }
    }

    function errorResponse(message) {
        return { ok: true, result: { status: 422,
            bodyBase64: Base64.encode(JSON.stringify({ error: { message: message } })) } }
    }

    function test_editInitialGetLoadingErrorAndStaleResponse() {
        const runtime = createTemporaryObject(runtimeComponent, this)
        const window = createTemporaryObject(editWindowComponent, this, { runtime: runtime })
        verify(runtime && window)
        const page = window.page
        verify(page.refresh())
        compare(runtime.lastPayload.method, "GET")
        compare(page.model.orderEditState, Models.RuntimeModels.Loading)
        verify(!page.formVisible)
        const first = runtime.lastRequestId
        verify(page.refresh())
        const newest = runtime.lastRequestId
        runtime.finishId(first, orderResponse({ id: "ORD-0001", status: "pending",
            priority: "normal", shippingAddress: "Stale address", notes: "Old" }))
        compare(page.model.orderEditState, Models.RuntimeModels.Loading)
        verify(!page.formVisible)
        runtime.finishId(newest, errorResponse("Order missing"))
        tryCompare(page.model, "orderEditState", Models.RuntimeModels.Error)
        compare(page.model.orderEditLoadError, "Order missing")
        verify(!page.formVisible)

        tryVerify(function() { return page.retryControl.visible })
        compare(page.retryControl.Accessible.description, "Retry loading order for editing")
        activate(window, page.retryControl)
        keyClick(Qt.Key_Return)
        compare(page.model.orderEditState, Models.RuntimeModels.Loading)
        verify(!page.retryControl.enabled)
        runtime.finish(orderResponse({ id: "ORD-0001", status: "shipped",
            priority: "high", shippingAddress: "120 Market Street", notes: "Dock 3" }))
        tryCompare(page.model, "orderEditState", Models.RuntimeModels.Content)
        tryVerify(function() { return page.formVisible })
        compare(page.selectedStatus, "shipped")
        compare(page.selectedPriority, "high")
        compare(page.addressControl.text, "120 Market Street")
        compare(page.notesControl.text, "Dock 3")
        compare(page.model.orderEditMutationState, Models.RuntimeModels.MutationIdle)
    }

    function test_editVisibleFormClientErrorServerErrorAndKeyboardRetry() {
        const runtime = createTemporaryObject(runtimeComponent, this)
        const window = createTemporaryObject(editWindowComponent, this, { runtime: runtime })
        verify(runtime && window)
        const page = window.page
        navigationSpy.target = page
        verify(page.refresh())
        runtime.finish(orderResponse({ id: "ORD-0001", status: "pending",
            priority: "normal", shippingAddress: "1 Pilot Way", notes: "" }))
        tryVerify(function() { return page.formVisible })

        page.addressControl.text = ""
        mouseClick(page.saveControl, page.saveControl.width / 2, page.saveControl.height / 2)
        compare(runtime.calls, 1)
        verify(page.model.orderEditErrors.shippingAddress.length > 0)
        verify(page.formVisible)

        page.addressControl.text = "1 Pilot Way"
        mouseClick(page.saveControl, page.saveControl.width / 2, page.saveControl.height / 2)
        compare(runtime.lastPayload.method, "PATCH")
        compare(page.model.orderEditMutationState, Models.RuntimeModels.MutationSaving)
        verify(page.formVisible)
        verify(!page.saveControl.enabled)
        verify(!page.statusControl.enabled)
        verify(!page.addressControl.enabled)
        runtime.finish(errorResponse("Invalid update"))
        tryCompare(page.model, "orderEditMutationState", Models.RuntimeModels.MutationFailure)
        compare(page.model.orderEditState, Models.RuntimeModels.Content)
        verify(page.formVisible)
        verify(page.saveControl.enabled)
        verify(page.statusControl.enabled)
        verify(page.addressControl.enabled)
        tryVerify(function() { return page.mutationErrorControl.visible })
        compare(page.mutationErrorControl.text, "Invalid update")
        compare(page.mutationErrorControl.Accessible.name, "Invalid update")
        compare(page.mutationErrorControl.Accessible.role, Accessible.AlertMessage)

        activate(window, page.addressControl.inputControl)
        keyClick(Qt.Key_A, Qt.ControlModifier)
        keyClick(Qt.Key_2)
        activate(window, page.saveControl)
        keyClick(Qt.Key_Return)
        compare(page.model.orderEditMutationState, Models.RuntimeModels.MutationSaving)
        compare(JSON.parse(Base64.decode(runtime.lastPayload.bodyBase64)).shippingAddress,
                "2")
        runtime.finish(orderResponse({ id: "ORD-0001", status: "pending",
            priority: "normal", shippingAddress: "2" }))
        tryCompare(page.model, "orderEditMutationState", Models.RuntimeModels.MutationSuccess)
        compare(page.model.orderEditMessage, "Order saved")
        tryCompare(navigationSpy, "count", 1)
        compare(navigationSpy.signalArguments[0][0], "/orders/ORD-0001")
    }

    function test_detailVisibleStatusErrorAndKeyboardRetry() {
        const runtime = createTemporaryObject(runtimeComponent, this)
        const window = createTemporaryObject(detailWindowComponent, this, { runtime: runtime })
        verify(runtime && window)
        const page = window.page
        verify(page.refresh())
        compare(page.model.orderDetailState, Models.RuntimeModels.Loading)
        verify(!page.detailVisible)
        runtime.finish(errorResponse("Order missing"))
        tryCompare(page.model, "orderDetailState", Models.RuntimeModels.Error)
        compare(page.model.orderDetailError, "Order missing")
        verify(!page.detailVisible)
        verify(!page.statusControl.enabled)
        tryVerify(function() { return page.retryControl.visible })
        compare(page.retryControl.Accessible.description, "Retry loading order details")
        activate(window, page.retryControl)
        keyClick(Qt.Key_Return)
        compare(page.model.orderDetailState, Models.RuntimeModels.Loading)
        runtime.finish(orderResponse({ id: "ORD-0001", status: "pending",
            customerName: "Acme", shippingAddress: "1 Pilot Way" }))
        tryCompare(page.model, "orderDetailState", Models.RuntimeModels.Content)
        tryVerify(function() { return page.detailVisible })
        compare(page.statusControl.Accessible.name, "Change order status")
        compare(page.statusControl.Accessible.role, Accessible.PageTabList)

        tryVerify(function() { return page.statusControl.itemAtIndex(1) !== null })
        const processing = page.statusControl.itemAtIndex(1)
        tryVerify(function() { return processing.visible })
        mouseClick(processing, processing.width / 2, processing.height / 2)
        compare(runtime.lastPayload.method, "PATCH")
        compare(JSON.parse(Base64.decode(runtime.lastPayload.bodyBase64)).status, "processing")
        compare(page.model.orderStatusMutationState, Models.RuntimeModels.MutationSaving)
        compare(page.model.orderDetailState, Models.RuntimeModels.Content)
        verify(page.detailVisible)
        verify(!page.statusControl.enabled)
        runtime.finish(errorResponse("Transition rejected"))
        tryCompare(page.model, "orderStatusMutationState", Models.RuntimeModels.MutationFailure)
        compare(page.model.orderDetailState, Models.RuntimeModels.Content)
        verify(page.detailVisible)
        verify(page.statusControl.enabled)
        tryVerify(function() { return page.mutationErrorControl.visible })
        compare(page.mutationErrorControl.text, "Transition rejected")
        compare(page.mutationErrorControl.Accessible.name, "Transition rejected")
        compare(page.mutationErrorControl.Accessible.role, Accessible.AlertMessage)

        activate(window, page.statusControl)
        keyClick(Qt.Key_Return)
        compare(page.model.orderStatusMutationState, Models.RuntimeModels.MutationSaving)
        verify(!page.statusControl.enabled)
        runtime.finish(orderResponse({ id: "ORD-0001", status: "processing" }))
        tryCompare(page.model, "orderStatusMutationState", Models.RuntimeModels.MutationSuccess)
        compare(page.model.order.status, "processing")
        compare(page.model.orderDetailMessage, "Order status updated")
    }

    function test_detailRefreshMakesOlderMutationResponseStale() {
        const runtime = createTemporaryObject(runtimeComponent, this)
        const window = createTemporaryObject(detailWindowComponent, this, { runtime: runtime })
        verify(runtime && window)
        const page = window.page
        verify(page.refresh())
        runtime.finish(orderResponse({ id: "ORD-0001", status: "pending" }))
        tryCompare(page.model, "orderDetailState", Models.RuntimeModels.Content)
        verify(page.statusControl.navigate(1))
        const mutationRequest = runtime.lastRequestId
        verify(page.refresh())
        const refreshRequest = runtime.lastRequestId
        runtime.finishId(mutationRequest, errorResponse("Stale mutation"))
        compare(page.model.orderDetailState, Models.RuntimeModels.Loading)
        compare(page.model.orderStatusError, "")
        runtime.finishId(refreshRequest, orderResponse({ id: "ORD-0001", status: "shipped" }))
        tryCompare(page.model, "orderDetailState", Models.RuntimeModels.Content)
        compare(page.model.order.status, "shipped")
        compare(page.model.orderStatusMutationState, Models.RuntimeModels.MutationIdle)
    }

    function test_saveRejectsSameIdWithMismatchedSubmittedFields() {
        const runtime = createTemporaryObject(runtimeComponent, this)
        const window = createTemporaryObject(editWindowComponent, this, { runtime: runtime })
        verify(runtime && window)
        const page = window.page
        verify(page.refresh())
        runtime.finish(orderResponse({}))
        tryVerify(function() { return page.formVisible })
        page.addressControl.text = "2 Correct Street"
        mouseClick(page.saveControl, page.saveControl.width / 2, page.saveControl.height / 2)
        runtime.finish(orderResponse({ shippingAddress: "tampered" }))
        tryCompare(page.model, "orderEditMutationState",
                   Models.RuntimeModels.MutationFailure)
        verify(page.formVisible)
        verify(page.saveControl.enabled)
    }
}
