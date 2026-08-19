import QtQuick
import QtTest
import "../../../packages/pilot/qml/pages" as Pages
import "../../../packages/pilot/qml/models" as Models
import "../../../packages/pilot/qml/Base64.js" as Base64

TestCase {
    name: "PilotLoginDashboard"

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
                ++calls
                lastRequestId = "request-" + calls
                lastCapability = capability
                lastOperation = operation
                lastPayload = payload
                return lastRequestId
            }
            function finish(response) { capabilityFinished(lastRequestId, response) }
        }
    }

    Component { id: loginComponent; Pages.LoginPage { width: 640; height: 480 } }
    Component { id: dashboardComponent; Pages.DashboardPage { width: 640; height: 480 } }

    function test_loginClientAndServerValidation() {
        const runtime = createTemporaryObject(runtimeComponent, this)
        const page = createTemporaryObject(loginComponent, this, { runtime: runtime })
        verify(runtime && page)
        verify(!page.submitCredentials("", ""))
        verify(page.model.emailError.length > 0)
        verify(page.model.passwordError.length > 0)
        compare(runtime.calls, 0)
        verify(!page.submitCredentials("not-an-email", "long-enough"))
        verify(page.model.emailError.length > 0)
        compare(runtime.calls, 0)

        verify(page.submitCredentials("pilot@example.com", "wrong-pass"))
        compare(runtime.lastCapability, "network")
        compare(runtime.lastOperation, "request")
        compare(runtime.lastPayload.method, "POST")
        compare(runtime.lastPayload.url, "http://127.0.0.1:4173/api/login")
        compare(JSON.parse(Base64.decode(runtime.lastPayload.bodyBase64)).email,
                "pilot@example.com")
        runtime.finish({ ok: true, result: { status: 401,
            bodyBase64: Base64.encode(JSON.stringify({ error: {
                code: "invalid_credentials", message: "The email or password is incorrect." } })) } })
        tryVerify(function() { return page.model.serverError.length > 0 })

        verify(page.submitCredentials("pilot@example.com", "pilot-pass"))
        runtime.finish({ ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify({ token: "session", user: { name: "Lin Chen" } })) } })
        tryCompare(page.model, "authenticated", true)
        compare(page.model.user.name, "Lin Chen")
    }

    function test_dashboardLoadingErrorAndSuccess() {
        const runtime = createTemporaryObject(runtimeComponent, this)
        const page = createTemporaryObject(dashboardComponent, this, { runtime: runtime })
        verify(runtime && page)
        page.refresh()
        compare(page.model.dashboardState, Models.RuntimeModels.Loading)
        runtime.finish({ ok: false, error: { code: "network.timeout", message: "Timed out" } })
        tryCompare(page.model, "dashboardState", Models.RuntimeModels.Error)
        compare(page.model.dashboardError, "Timed out")
        page.refresh()
        runtime.finish({ ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify({
                kpis: { orderCount: 12, pendingCount: 2, customerCount: 4, revenueCents: 477725 },
                revenueByMonth: [
                    { month: "2026-07", amountCents: 117275 },
                    { month: "2026-08", amountCents: 120000 }
                ],
                recentActivity: [{ id: "act-001", text: "Order updated" }]
            })) } })
        tryCompare(page.model, "dashboardState", Models.RuntimeModels.Content)
        compare(page.orderCountText, "12")
        compare(page.customerCountText, "4")
        compare(page.pendingCountText, "2")
        compare(page.revenueText, "$4,777.25")
        compare(page.revenueRows.length, 2)
        compare(page.revenueRows[1].display, "2026-08 — $1,200.00")
        compare(page.revenueAccessibleName, "Revenue by month")
        compare(page.model.dashboard.recentActivity[0].display, "act-001 — Order updated")

        page.refresh()
        compare(page.model.dashboardState, Models.RuntimeModels.Loading)
        runtime.finish({ ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify({
                kpis: { orderCount: 0, pendingCount: 0, customerCount: 0, revenueCents: 0 },
                revenueByMonth: [], recentActivity: []
            })) } })
        tryCompare(page.model, "dashboardState", Models.RuntimeModels.Empty)
    }
}
