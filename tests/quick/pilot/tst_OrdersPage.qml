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
            readonly property string appIdentity: "com.qbrowser.pilot"
            readonly property string apiOrigin: "http://127.0.0.1:4173"
            readonly property string route: "/orders/ORD-0001"
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
            signal navigationFinished(string requestId, var response)
            signal navigationRequested(string requestId, string route)
            function navigate(route) {
                ++routeCalls; lastRoute = route
                const requestId = "navigation-" + routeCalls
                navigationRequested(requestId, route)
                return requestId
            }
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
    Component {
        id: customersWindowComponent
        Window {
            id: customersWindow
            property var runtime: null
            readonly property alias page: visibleCustomersPage
            width: 800; height: 600; visible: true
            Pages.CustomersPage {
                id: visibleCustomersPage
                anchors.fill: parent
                runtime: customersWindow.runtime
            }
        }
    }
    Component {
        id: customerDetailWindowComponent
        Window {
            id: detailWindow
            property var runtime: null
            readonly property alias page: customerDetailPage
            width: 800; height: 600; visible: true
            Pages.CustomerDetailPage {
                id: customerDetailPage
                anchors.fill: parent; runtime: detailWindow.runtime; customerId: "CUS-001"
            }
        }
    }
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
        compare(router.route, "/orders/ORD-0001")
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
                                                       page: 2, pageSize: 20, total: 21, totalPages: 2,
                                                       query: "acme", status: "pending" })) } })
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
            bodyBase64: Base64.encode(JSON.stringify({ items: [], page: 3,
                pageSize: 20, total: 41, totalPages: 3,
                query: "acme north", status: "all" })) } })

        tryVerify(function() { return page.statusControl.itemAtIndex(1) !== null })
        const pending = page.statusControl.itemAtIndex(1)
        tryVerify(function() { return pending.visible })
        mouseClick(pending, pending.width / 2, pending.height / 2)
        compare(page.model.ordersStatus, "pending")
        compare(page.model.ordersPage, 1)
        verify(runtime.lastPayload.url.indexOf("query=acme%20north") >= 0)
        verify(runtime.lastPayload.url.indexOf("status=pending") >= 0)
        verify(runtime.lastPayload.url.indexOf("page=1") >= 0)
        compare(page.statusControl.enabled, false)
        const callsWhileBusy = runtime.calls
        keyClick(Qt.Key_Right)
        compare(runtime.calls, callsWhileBusy)
        runtime.finish({ ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify({ items: [], page: 1,
                pageSize: 20, total: 0, totalPages: 0,
                query: "acme north", status: "pending" })) } })
        tryVerify(function() { return page.statusControl.enabled })

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
            bodyBase64: Base64.encode(JSON.stringify({ items: [{ id: "CUS-001", name: "Acme", email: "ops@acme.test" }],
                                                       page: 1, pageSize: 20, total: 1, totalPages: 1,
                                                       query: "lin" })) } })
        tryCompare(list.model, "customersState", Models.RuntimeModels.Content)
        compare(list.model.customers.length, 1)
        compare(list.model.customers[0].display, "CUS-001 — Acme")
        navigationSpy.target = list
        verify(list.openSelectedCustomer(0))
        compare(navigationSpy.signalArguments[0][0], "/customers/CUS-001")

        const detailWindow = createTemporaryObject(customerDetailWindowComponent, this,
                                                   { runtime: runtime })
        verify(detailWindow)
        const detail = detailWindow.page
        detail.refresh()
        runtime.finish({ ok: true, result: { status: 503,
            bodyBase64: Base64.encode(JSON.stringify({ error: { message: "Customer unavailable" } })) } })
        tryCompare(detail.model, "customerDetailState", Models.RuntimeModels.Error)
        tryVerify(function() { return detail.retryControl.visible })
        detailWindow.requestActivate()
        tryVerify(function() { return detailWindow.active })
        detail.retryControl.forceActiveFocus(Qt.TabFocusReason)
        tryVerify(function() { return detail.retryControl.activeFocus })
        keyClick(Qt.Key_Return)
        compare(detail.model.customerDetailState, Models.RuntimeModels.Loading)
        runtime.finish({ ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify({ id: "CUS-001", name: "Acme",
                                                orders: [{ id: "ORD-0001" }] })) } })
        tryCompare(detail.model, "customerDetailState", Models.RuntimeModels.Error)
        detail.refresh()
        runtime.finish({ ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify({
                id: "CUS-001", name: "Acme", email: "ops@acme.test",
                orders: [{ id: "ORD-0001", customerId: "CUS-001",
                    customerName: "Acme", createdAt: "2026-08-01T00:00:00Z",
                    updatedAt: "2026-08-01T00:00:00Z", status: "pending",
                    totalCents: 100, currency: "USD", priority: "normal",
                    shippingAddress: "1 Pilot Way", notes: "" }]
            })) } })
        tryCompare(detail.model, "customerDetailState", Models.RuntimeModels.Content)
        compare(detail.model.customer.orders.length, 1)
        compare(detail.model.customer.orders[0].display, "ORD-0001 — Acme")
    }

    function test_customerPaginationEmptyAndErrorStates() {
        const runtime = createTemporaryObject(runtimeComponent, this)
        const page = createTemporaryObject(customersComponent, this, { runtime: runtime })
        verify(runtime && page)
        page.search("Acme", 1)
        runtime.finish({ ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify({
                items: [{ id: "CUS-001", name: "Acme", email: "ops@acme.test" }], page: 1,
                pageSize: 20, total: 41, totalPages: 3, query: "acme"
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
            bodyBase64: Base64.encode(JSON.stringify({ items: [], page: 2,
                pageSize: 20, total: 41, totalPages: 3, query: "acme" })) } })
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
                items: [{ id: "CUS-003", name: "Acme West", email: "west@acme.test" }], page: 3,
                pageSize: 20, total: 41, totalPages: 3, query: "acme"
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

    function test_emptyMockPaginationAndQueryIdentity() {
        const runtime = createTemporaryObject(runtimeComponent, this)
        const page = createTemporaryObject(ordersComponent, this, { runtime: runtime })
        verify(runtime && page)
        page.search("Missing", "all", 1)
        runtime.finish({ ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify({
                items: [], page: 1, pageSize: 20, total: 0, totalPages: 0,
                query: "missing", status: "all"
            })) } })
        tryCompare(page.model, "ordersState", Models.RuntimeModels.Empty)
        compare(page.model.ordersTotalPages, 0)

        page.search("Expected", "pending", 1)
        runtime.finish({ ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify({
                items: [], page: 1, pageSize: 20, total: 0, totalPages: 0,
                query: "wrong", status: "pending"
            })) } })
        tryCompare(page.model, "ordersState", Models.RuntimeModels.Error)
    }

    function test_pendingSearchClearsAndDisablesSelectedRows() {
        const runtime = createTemporaryObject(runtimeComponent, this)
        const ordersWindow = createTemporaryObject(ordersWindowComponent, this,
                                                    { runtime: runtime })
        verify(runtime && ordersWindow)
        const ordersPage = ordersWindow.page
        navigationSpy.target = ordersPage
        navigationSpy.clear()
        ordersPage.search("", "all", 1)
        runtime.finish({ ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify({
                items: [{ id: "ORD-0001", customerName: "Acme" }],
                page: 1, pageSize: 20, total: 1, totalPages: 1,
                query: "", status: "all"
            })) } })
        tryCompare(ordersPage.model, "ordersState", Models.RuntimeModels.Content)
        verify(ordersPage.tableControl.select(0))
        ordersPage.openControl.forceActiveFocus(Qt.TabFocusReason)
        ordersPage.search("pending", "all", 1)
        compare(ordersPage.tableControl.currentIndex, -1)
        verify(!ordersPage.openControl.enabled)
        verify(!ordersPage.openSelectedOrder(0))
        keyClick(Qt.Key_Return)
        mouseClick(ordersPage.openControl, ordersPage.openControl.width / 2,
                   ordersPage.openControl.height / 2)
        compare(navigationSpy.count, 0)

        const customersWindow = createTemporaryObject(customersWindowComponent, this,
                                                       { runtime: runtime })
        verify(customersWindow)
        const customersPage = customersWindow.page
        navigationSpy.target = customersPage
        navigationSpy.clear()
        runtime.finish({ ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify({
                items: [{ id: "CUS-001", name: "Acme", email: "ops@acme.test" }], page: 1,
                pageSize: 20, total: 1, totalPages: 1, query: ""
            })) } })
        customersPage.search("", 1)
        runtime.finish({ ok: true, result: { status: 200,
            bodyBase64: Base64.encode(JSON.stringify({
                items: [{ id: "CUS-001", name: "Acme", email: "ops@acme.test" }], page: 1,
                pageSize: 20, total: 1, totalPages: 1, query: ""
            })) } })
        tryCompare(customersPage.model, "customersState", Models.RuntimeModels.Content)
        verify(customersPage.tableControl.select(0))
        customersPage.openControl.forceActiveFocus(Qt.TabFocusReason)
        customersPage.search("pending", 1)
        compare(customersPage.tableControl.currentIndex, -1)
        verify(!customersPage.openControl.enabled)
        verify(!customersPage.openSelectedCustomer(0))
        keyClick(Qt.Key_Return)
        mouseClick(customersPage.openControl, customersPage.openControl.width / 2,
                   customersPage.openControl.height / 2)
        compare(navigationSpy.count, 0)
    }
}
