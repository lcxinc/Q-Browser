import QtQuick
import QtQuick.Window
import QtTest
import "../../../packages/pilot/qml/pages" as Pages
import "../../../packages/pilot/qml/models" as Models
import "../../../packages/pilot/qml/Base64.js" as Base64
import "../../../packages/pilot/qml" as Pilot

TestCase {
    name: "PilotOrdersCustomersFiles"
    when: windowShown

    Component {
        id: runtimeComponent
        QtObject {
            property int calls: 0
            property string lastRequestId: ""
            property string lastCapability: ""
            property string lastOperation: ""
            property var lastPayload: ({})
            property int routeCalls: 0
            property string lastRoute: ""
            signal capabilityFinished(string requestId, var response)
            function invoke(capability, operation, payload) {
                ++calls; lastRequestId = "r" + calls; lastCapability = capability
                lastOperation = operation; lastPayload = payload; return lastRequestId
            }
            function finish(response) { capabilityFinished(lastRequestId, response) }
            function loadRoute(route) { ++routeCalls; lastRoute = route }
        }
    }
    Component { id: ordersComponent; Pages.OrdersPage { width: 800; height: 600 } }
    Component {
        id: ordersWindowComponent
        Window {
            id: ordersWindow
            property var runtime: null
            readonly property alias page: visibleOrdersPage
            width: 800; height: 600; visible: true
            Pages.OrdersPage {
                id: visibleOrdersPage
                anchors.fill: parent
                runtime: ordersWindow.runtime
            }
        }
    }
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

    function test_navigationSelectionTracksRouteFamiliesWithoutDuplicateLoads() {
        const runtime = createTemporaryObject(runtimeComponent, this)
        const router = createTemporaryObject(routerComponent, this, { runtime: runtime })
        verify(runtime && router)
        const cases = [
            ["/dashboard", 0], ["/orders", 1], ["/orders/ORD-0001", 1],
            ["/orders/ORD-0001/edit?from=detail", 1], ["/customers", 2],
            ["/customers/CUS-001#orders", 2], ["/files", 3], ["/settings", 4]
        ]
        for (const entry of cases) {
            router.route = entry[0]
            compare(router.navigationIndex, entry[1])
            compare(router.navigationControl.currentIndex, entry[1])
        }
        router.route = "/orders/ORD-0001"
        verify(!router.navigate("/orders/ORD-0001"))
        compare(runtime.routeCalls, 0)
        verify(router.navigate("/customers"))
        compare(runtime.routeCalls, 1)
        compare(runtime.lastRoute, "/customers")
        router.route = "/orders"
        compare(router.navigationControl.currentIndex, 1)

        router.route = "/dashboard"
        verify(router.navigationControl.navigate(2))
        compare(router.navigationControl.currentIndex, 2)
        router.route = "/orders/ORD-0002"
        compare(router.navigationControl.currentIndex, 1)
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

    function test_statusActivationReloadsCurrentQueryFromFirstPageByMouseAndKeyboard() {
        const runtime = createTemporaryObject(runtimeComponent, this)
        const window = createTemporaryObject(ordersWindowComponent, this, { runtime: runtime })
        verify(runtime && window)
        tryVerify(function() { return window.visible })
        const page = window.page
        verify(page)
        page.search("acme north", "all", 3)
        runtime.finish({ ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify({ items: [], page: 3, totalPages: 3 })) } })

        tryVerify(function() { return page.statusControl.itemAtIndex(1) !== null })
        const pending = page.statusControl.itemAtIndex(1)
        tryVerify(function() { return pending.visible })
        mouseClick(pending, pending.width / 2, pending.height / 2)
        compare(page.model.ordersStatus, "pending")
        compare(page.model.ordersPage, 1)
        verify(runtime.lastPayload.url.indexOf("query=acme%20north") >= 0)
        verify(runtime.lastPayload.url.indexOf("status=pending") >= 0)
        verify(runtime.lastPayload.url.indexOf("page=1") >= 0)

        window.requestActivate()
        tryVerify(function() { return window.active })
        page.statusControl.forceActiveFocus(Qt.TabFocusReason)
        tryVerify(function() { return page.statusControl.activeFocus })
        keyClick(Qt.Key_Right)
        compare(page.model.ordersStatus, "processing")
        compare(page.model.ordersPage, 1)
        verify(runtime.lastPayload.url.indexOf("query=acme%20north") >= 0)
        verify(runtime.lastPayload.url.indexOf("status=processing") >= 0)
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

    function test_customerPaginationEmptyAndErrorStates() {
        const runtime = createTemporaryObject(runtimeComponent, this)
        const page = createTemporaryObject(customersComponent, this, { runtime: runtime })
        verify(runtime && page)
        page.search("Acme", 1)
        runtime.finish({ ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify({
                items: [{ id: "CUS-001", name: "Acme" }], page: 1, totalPages: 3
            })) } })
        tryCompare(page.model, "customersState", Models.RuntimeModels.Content)
        compare(page.model.customersPage, 1)
        compare(page.model.customersTotalPages, 3)
        verify(!page.previousEnabled)
        verify(page.nextEnabled)
        verify(page.nextPage())
        verify(runtime.lastPayload.url.indexOf("query=Acme") >= 0)
        verify(runtime.lastPayload.url.indexOf("page=2") >= 0)
        runtime.finish({ ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify({ items: [], page: 2, totalPages: 3 })) } })
        tryCompare(page.model, "customersState", Models.RuntimeModels.Empty)
        compare(page.model.customersPage, 2)
        verify(page.previousEnabled)
        verify(page.nextEnabled)

        verify(page.nextPage())
        runtime.finish({ ok: true, result: { status: 503,
            bodyBase64: Base64.encode(JSON.stringify({ error: { message: "Customers unavailable" } })) } })
        tryCompare(page.model, "customersState", Models.RuntimeModels.Error)
        compare(page.model.customersError, "Customers unavailable")

        page.search("Acme", 3)
        runtime.finish({ ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify({
                items: [{ id: "CUS-003", name: "Acme West" }], page: 3, totalPages: 3
            })) } })
        tryCompare(page.model, "customersPage", 3)
        verify(page.previousEnabled)
        verify(!page.nextEnabled)
        verify(!page.nextPage())
        verify(page.previousPage())
        verify(runtime.lastPayload.url.indexOf("page=2") >= 0)
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
