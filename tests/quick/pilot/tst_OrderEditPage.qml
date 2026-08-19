import QtQuick
import QtTest
import "../../../packages/pilot/qml/pages" as Pages
import "../../../packages/pilot/qml/models" as Models
import "../../../packages/pilot/qml/Base64.js" as Base64

TestCase {
    name: "PilotOrderEdit"
    Component {
        id: runtimeComponent
        QtObject {
            property int calls: 0
            property string lastRequestId: ""
            property var lastPayload: ({})
            signal capabilityFinished(string requestId, var response)
            function invoke(capability, operation, payload) {
                ++calls; lastRequestId = "r" + calls; lastPayload = payload; return lastRequestId
            }
            function finish(response) { capabilityFinished(lastRequestId, response) }
        }
    }
    Component { id: pageComponent; Pages.OrderEditPage { width: 640; height: 540; orderId: "ORD-0001" } }

    function test_clientValidationAndServerErrors() {
        const runtime = createTemporaryObject(runtimeComponent, this)
        const page = createTemporaryObject(pageComponent, this, { runtime: runtime })
        verify(runtime && page)
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
    }
}
