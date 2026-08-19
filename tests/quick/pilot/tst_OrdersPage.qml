import QtQuick
import QtTest
import "../../../packages/pilot/qml/pages" as Pages
import "../../../packages/pilot/qml/models" as Models
import "../../../packages/pilot/qml/Base64.js" as Base64
import "../../../packages/pilot/qml" as Pilot

TestCase {
    name: "PilotOrdersCustomersFiles"

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
    Component { id: ordersComponent; Pages.OrdersPage { width: 800; height: 600 } }
    Component { id: customersComponent; Pages.CustomersPage { width: 800; height: 600 } }
    Component { id: customerDetailComponent; Pages.CustomerDetailPage { width: 800; height: 600 } }
    Component { id: filesComponent; Pages.FilesPage { width: 800; height: 600 } }
    Component { id: routerComponent; Pilot.PilotRouter { width: 900; height: 600 } }

    SignalSpy { id: navigationSpy; signalName: "navigateRequested" }

    function test_routePatternsSelectIntendedPages() {
        const router = createTemporaryObject(routerComponent, this)
        verify(router)
        const cases = [
            ["/login", "LoginPage"], ["/dashboard", "DashboardPage"],
            ["/orders", "OrdersPage"], ["/orders/ORD-0001", "OrderDetailPage"],
            ["/orders/ORD-0001/edit", "OrderEditPage"],
            ["/customers", "CustomersPage"],
            ["/customers/CUS-001", "CustomerDetailPage"],
            ["/files", "FilesPage"], ["/settings", "SettingsPage"]
        ]
        for (const entry of cases) {
            router.route = entry[0]
            compare(router.currentPageName, entry[1])
        }
    }

    function test_searchFilterPaginationAndDetailNavigation() {
        const runtime = createTemporaryObject(runtimeComponent, this)
        const page = createTemporaryObject(ordersComponent, this, { runtime: runtime })
        verify(runtime && page)
        navigationSpy.target = page
        page.search("acme", "pending", 2)
        verify(runtime.lastPayload.url.indexOf("query=acme") >= 0)
        verify(runtime.lastPayload.url.indexOf("status=pending") >= 0)
        verify(runtime.lastPayload.url.indexOf("page=2") >= 0)
        runtime.finish({ ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify({ items: [{ id: "ORD-0001", customerName: "Acme" }],
                                                       page: 2, pageSize: 20, total: 21, totalPages: 2 })) } })
        tryCompare(page.model, "ordersState", Models.RuntimeModels.Content)
        compare(page.model.orders.length, 1)
        compare(page.model.orders[0].display, "ORD-0001 — Acme")
        verify(page.openSelectedOrder(0))
        compare(navigationSpy.signalArguments[0][0], "/orders/ORD-0001")
        verify(!page.openSelectedOrder(99))
    }

    function test_customersAndRelatedOrders() {
        const runtime = createTemporaryObject(runtimeComponent, this)
        const list = createTemporaryObject(customersComponent, this, { runtime: runtime })
        verify(runtime && list)
        list.search("Lin", 1)
        verify(runtime.lastPayload.url.indexOf("/api/customers") >= 0)
        runtime.finish({ ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify({ items: [{ id: "CUS-001", name: "Acme" }],
                                                       page: 1, pageSize: 20, total: 1, totalPages: 1 })) } })
        tryCompare(list.model, "customersState", Models.RuntimeModels.Content)
        compare(list.model.customers.length, 1)
        compare(list.model.customers[0].display, "CUS-001 — Acme")
        navigationSpy.target = list
        verify(list.openSelectedCustomer(0))
        compare(navigationSpy.signalArguments[0][0], "/customers/CUS-001")

        const detail = createTemporaryObject(customerDetailComponent, this,
                                             { runtime: runtime, customerId: "CUS-001" })
        detail.refresh()
        runtime.finish({ ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify({ id: "CUS-001", name: "Acme",
                                                orders: [{ id: "ORD-0001" }] })) } })
        tryCompare(detail.model, "customerDetailState", Models.RuntimeModels.Content)
        compare(detail.model.customer.orders.length, 1)
        compare(detail.model.customer.orders[0].display, "ORD-0001")
    }

    function test_brokeredFileCancellationAndMetadata() {
        const runtime = createTemporaryObject(runtimeComponent, this)
        const page = createTemporaryObject(filesComponent, this, { runtime: runtime })
        verify(runtime && page)
        page.openFile()
        compare(runtime.lastCapability, "file")
        compare(runtime.lastOperation, "open")
        runtime.finish({ ok: false, error: { code: "file.cancelled", message: "No file selected" } })
        tryCompare(page.model, "fileState", Models.RuntimeModels.Empty)
        compare(page.model.fileMessage, "No file selected")
        page.openFile()
        runtime.finish({ ok: true, result: { name: "report.txt", size: 4,
                                             contentBase64: Base64.encode("safe") } })
        tryCompare(page.model, "fileState", Models.RuntimeModels.Content)
        compare(page.model.fileMetadata.name, "report.txt")
        compare(page.model.fileMetadata.size, 4)
    }
}
