import QtQuick
import "../Base64.js" as Base64

QtObject {
    id: root

    enum LoadState { Content, Loading, Empty, Error }
    enum MutationState { MutationIdle, MutationSaving, MutationFailure, MutationSuccess }

    property var runtime: null
    property string apiOrigin: runtime && typeof runtime.apiOrigin === "string"
                               && runtime.apiOrigin.length > 0
                               ? runtime.apiOrigin : "http://127.0.0.1:4173"
    property var pending: ({})
    property var activeRequestByLane: ({})
    readonly property int pendingCount: Object.keys(pending).length
    readonly property var fixedLanes: ({
        login: true, dashboard: true, orders: true, orderDetailFlow: true,
        orderEditFlow: true, customers: true, customerDetail: true,
        file: true, settings: true
    })
    property var latestGeneration: ({})
    property int requestGeneration: 0

    property string emailError: ""
    property string passwordError: ""
    property string serverError: ""
    property bool authenticated: false
    property var user: ({})

    property int dashboardState: RuntimeModels.Empty
    property string dashboardError: ""
    property var dashboard: ({})

    property int ordersState: RuntimeModels.Empty
    property string ordersError: ""
    property var orders: []
    property int ordersPage: 1
    property int ordersTotalPages: 1
    property string ordersQuery: ""
    property string ordersStatus: "all"

    property int orderDetailState: RuntimeModels.Empty
    property string orderDetailError: ""
    property int orderStatusMutationState: RuntimeModels.MutationIdle
    property string orderStatusError: ""
    property var order: ({})
    property int orderEditState: RuntimeModels.Empty
    property string orderEditLoadError: ""
    property int orderEditMutationState: RuntimeModels.MutationIdle
    property var orderEditErrors: ({ status: "", priority: "", shippingAddress: "" })
    property string orderEditServerError: ""
    property string orderEditMessage: ""
    property string editStatus: ""
    property string editPriority: ""
    property string editShippingAddress: ""
    property string editNotes: ""
    property string orderDetailMessage: ""

    property int customersState: RuntimeModels.Empty
    property string customersError: ""
    property var customers: []
    property int customersPage: 1
    property int customersTotalPages: 1
    property string customersQuery: ""
    property int customerDetailState: RuntimeModels.Empty
    property string customerDetailError: ""
    property var customer: ({})

    property int fileState: RuntimeModels.Empty
    property string fileMessage: "No file selected"
    property var fileMetadata: ({})

    property string themeName: "light"
    property string settingsMessage: ""
    property bool settingsLoading: false
    property bool settingsSaving: false
    property bool settingsDirty: false
    property bool settingsPersisted: false
    property string settingsError: ""

    signal orderEditLoaded()
    signal orderSaved(string orderId)

    function encodedJson(value) {
        return Base64.encode(JSON.stringify(value))
    }

    function decodedJson(value) {
        try {
            return JSON.parse(Base64.decode(value))
        } catch (error) {
            return null
        }
    }

    // QString/QML string lengths are UTF-16 code units. Bound untrusted IPC text
    // before assigning it to any visible string property.
    function boundedErrorText(value) {
        return typeof value === "string" && value.length > 0 && value.length <= 512
                ? value : ""
    }

    function safeError(response, fallback) {
        const fallbackText = boundedErrorText(fallback)
        if (isPlainObject(response) && isPlainObject(response.error)) {
            const message = boundedErrorText(response.error.message)
            if (message.length > 0)
                return message
        }
        return fallbackText.length > 0 ? fallbackText : "Request failed"
    }

    function safeHttpError(body) {
        if (isPlainObject(body) && isPlainObject(body.error)) {
            const message = boundedErrorText(body.error.message)
            if (message.length > 0)
                return message
        }
        return "Request failed"
    }

    function responseBody(response) {
        if (!response || response.ok !== true || !response.result
                || !isPlainObject(response.result)
                || !isInteger(response.result.status, 100, 599)
                || typeof response.result.bodyBase64 !== "string")
            return null
        return decodedJson(response.result.bodyBase64)
    }

    function isPlainObject(value) {
        return value !== null && typeof value === "object" && !Array.isArray(value)
                && Object.prototype.toString.call(value) === "[object Object]"
    }

    function isInteger(value, minimum, maximum) {
        return typeof value === "number" && Number.isFinite(value)
                && Math.floor(value) === value && value >= minimum && value <= maximum
    }

    function isText(value, maximum) {
        return typeof value === "string" && value.length > 0 && value.length <= maximum
    }

    function validRows(items, secondaryKey) {
        if (!Array.isArray(items) || items.length > 100)
            return false
        for (let index = 0; index < items.length; ++index) {
            const item = items[index]
            if (!isPlainObject(item) || !isText(item.id, 128)
                    || (secondaryKey && !isText(item[secondaryKey], 500)))
                return false
        }
        return true
    }

    function validPage(body, context, secondaryKey) {
        if (!isPlainObject(body) || !validRows(body.items, secondaryKey)
                || !isInteger(body.page, 1, 100000)
                || !isInteger(body.pageSize, 1, 100)
                || !isInteger(body.total, 0, Number.MAX_SAFE_INTEGER)
                || !isInteger(body.totalPages, 0, 100000)
                || body.items.length > body.pageSize
                || body.page !== context.expectedPage
                || typeof body.query !== "string" || body.query !== context.expectedQuery
                || (typeof context.expectedStatus === "string"
                    && body.status !== context.expectedStatus))
            return false
        if (body.total === 0)
            return body.items.length === 0 && body.page === 1 && body.totalPages === 0
        return body.total >= body.items.length && body.totalPages >= 1
                && body.page <= body.totalPages
                && body.totalPages === Math.ceil(body.total / body.pageSize)
    }

    function validOrder(body, expectedId) {
        const statuses = ["pending", "processing", "shipped", "delivered", "cancelled"]
        return isPlainObject(body) && isText(body.id, 128) && body.id === expectedId
                && isText(body.customerId, 128) && isText(body.customerName, 500)
                && isText(body.createdAt, 64) && isText(body.updatedAt, 64)
                && statuses.indexOf(body.status) >= 0
                && (body.priority === "normal" || body.priority === "high")
                && isText(body.shippingAddress, 500)
                && typeof body.notes === "string" && body.notes.length <= 500
                && isInteger(body.totalCents, 0, Number.MAX_SAFE_INTEGER)
                && body.currency === "USD"
    }

    function validDashboard(body) {
        if (!isPlainObject(body) || !isPlainObject(body.kpis)
                || !Array.isArray(body.revenueByMonth) || body.revenueByMonth.length > 120
                || !validRows(body.recentActivity, "text"))
            return false
        const keys = ["orderCount", "customerCount", "pendingCount", "revenueCents"]
        for (let index = 0; index < keys.length; ++index) {
            if (!isInteger(body.kpis[keys[index]], 0, Number.MAX_SAFE_INTEGER))
                return false
        }
        for (let row = 0; row < body.revenueByMonth.length; ++row) {
            const item = body.revenueByMonth[row]
            if (!isPlainObject(item) || !isText(item.month, 32)
                    || !isInteger(item.amountCents, 0, Number.MAX_SAFE_INTEGER))
                return false
        }
        return true
    }

    function validCustomerSummary(body) {
        return isPlainObject(body) && isText(body.id, 128)
                && isText(body.name, 500) && isText(body.email, 320)
    }

    function validCustomerDetail(body, expectedId) {
        if (!validCustomerSummary(body) || body.id !== expectedId
                || !Array.isArray(body.orders) || body.orders.length > 100)
            return false
        for (let index = 0; index < body.orders.length; ++index) {
            const related = body.orders[index]
            if (!isPlainObject(related) || !isText(related.id, 128)
                    || !validOrder(related, related.id)
                    || related.customerId !== expectedId)
                return false
        }
        return true
    }

    function displayRows(items, secondaryKey) {
        if (!Array.isArray(items)) return []
        return items.map(function(item) {
            const row = Object.assign({}, item)
            const secondary = item && item[secondaryKey] ? String(item[secondaryKey]) : ""
            row.display = String(item && item.id ? item.id : "")
                        + (secondary.length > 0 ? " — " + secondary : "")
            return row
        })
    }

    function laneBusy(lane) {
        return typeof activeRequestByLane[lane] === "string"
    }

    function begin(kind, capability, operation, payload, lane, expected) {
        if (!runtime || typeof runtime.invoke !== "function")
            return ""
        const requestLane = typeof lane === "string" && lane.length > 0 ? lane : kind
        if (!fixedLanes[requestLane])
            return ""
        const requestId = runtime.invoke(capability, operation, payload)
        if (typeof requestId !== "string" || requestId.length === 0)
            return ""
        const generation = ++requestGeneration
        const next = Object.assign({}, pending)
        const previousRequestId = activeRequestByLane[requestLane]
        if (typeof previousRequestId === "string")
            delete next[previousRequestId]
        next[requestId] = Object.assign({ kind: kind, lane: requestLane,
                                          generation: generation }, expected || ({}))
        pending = next
        const active = Object.assign({}, activeRequestByLane)
        active[requestLane] = requestId
        activeRequestByLane = active
        const newest = Object.assign({}, latestGeneration)
        newest[requestLane] = generation
        latestGeneration = newest
        return requestId
    }

    function network(kind, method, path, body, lane, expected) {
        return begin(kind, "network", "request", {
            method: method,
            url: apiOrigin + path,
            bodyBase64: body === undefined || body === null ? "" : encodedJson(body)
        }, lane, expected)
    }

    function login(email, password) {
        if (typeof email !== "string" || typeof password !== "string") {
            emailError = "Email must be text"
            passwordError = "Password must be text"
            serverError = ""
            return false
        }
        const normalizedEmail = email.trim().toLowerCase()
        emailError = normalizedEmail.length === 0 ? "Email is required"
                   : !/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(normalizedEmail)
                     ? "Enter a valid email address" : ""
        passwordError = password.length === 0 ? "Password is required"
                      : password.length < 8 ? "Password must contain at least 8 characters" : ""
        serverError = ""
        if (emailError.length > 0 || passwordError.length > 0)
            return false
        authenticated = false
        if (laneBusy("login")) return false
        return network("login", "POST", "/api/login", { email: normalizedEmail, password: password },
                       "login", { expectedEmail: normalizedEmail }).length > 0
    }

    function loadDashboard() {
        if (laneBusy("dashboard")) return false
        dashboardState = RuntimeModels.Loading
        dashboardError = ""
        if (network("dashboard", "GET", "/api/dashboard", null, "dashboard").length === 0) {
            dashboardState = RuntimeModels.Error
            dashboardError = "Runtime is unavailable"
            return false
        }
        return true
    }

    function loadOrders(query, status, page) {
        const validStatuses = ["all", "pending", "processing", "shipped", "delivered", "cancelled"]
        if (typeof query !== "string" || typeof status !== "string"
                || validStatuses.indexOf(status) < 0
                || !Number.isInteger(page) || page < 1 || page > 100000) {
            ordersState = RuntimeModels.Error
            ordersError = "Invalid order search"
            return false
        }
        ordersQuery = query.trim()
        ordersStatus = status.length === 0 ? "all" : status
        ordersPage = Math.max(1, page)
        ordersState = RuntimeModels.Loading
        ordersError = ""
        const path = "/api/orders?page=" + ordersPage + "&pageSize=20&status="
                   + encodeURIComponent(ordersStatus) + "&query=" + encodeURIComponent(ordersQuery)
        if (network("orders", "GET", path, null, "orders",
                    { expectedPage: ordersPage, expectedQuery: ordersQuery.toLowerCase(),
                      expectedStatus: ordersStatus }).length === 0) {
            ordersState = RuntimeModels.Error
            ordersError = "Runtime is unavailable"
            return false
        }
        return true
    }

    function loadOrder(id) {
        if (typeof id !== "string" || id.trim().length === 0 || id.length > 128) {
            orderDetailState = RuntimeModels.Error
            orderDetailError = "Invalid order id"
            return false
        }
        orderDetailState = RuntimeModels.Loading
        orderDetailError = ""
        orderStatusMutationState = RuntimeModels.MutationIdle
        orderStatusError = ""
        orderDetailMessage = ""
        if (network("orderDetail", "GET", "/api/orders/" + encodeURIComponent(id), null,
                    "orderDetailFlow", { expectedId: id }).length === 0) {
            orderDetailState = RuntimeModels.Error
            orderDetailError = "Runtime is unavailable"
            return false
        }
        return true
    }

    function loadOrderForEdit(id) {
        if (typeof id !== "string" || id.trim().length === 0 || id.length > 128)
            return false
        orderEditState = RuntimeModels.Loading
        orderEditLoadError = ""
        orderEditMutationState = RuntimeModels.MutationIdle
        orderEditServerError = ""
        orderEditMessage = ""
        if (network("orderEditLoad", "GET", "/api/orders/" + encodeURIComponent(id), null,
                    "orderEditFlow", { expectedId: id }).length === 0) {
            orderEditState = RuntimeModels.Error
            orderEditLoadError = "Runtime is unavailable"
            return false
        }
        return true
    }

    function changeOrderStatus(id, status) {
        const validStatuses = ["pending", "processing", "shipped", "delivered", "cancelled"]
        if (typeof id !== "string" || id.trim().length === 0 || id.length > 128
                || typeof status !== "string" || validStatuses.indexOf(status) < 0)
            return false
        orderStatusMutationState = RuntimeModels.MutationSaving
        orderStatusError = ""
        orderDetailMessage = ""
        if (network("orderStatus", "PATCH", "/api/orders/" + encodeURIComponent(id),
                    { status: status }, "orderDetailFlow",
                    { expectedId: id, expectedStatus: status }).length === 0) {
            orderStatusMutationState = RuntimeModels.MutationFailure
            orderStatusError = "Runtime is unavailable"
            return false
        }
        return true
    }

    function saveOrder(id, status, priority, shippingAddress, notes) {
        const validStatuses = ["pending", "processing", "shipped", "delivered", "cancelled"]
        if (typeof id !== "string" || id.trim().length === 0 || id.length > 128
                || typeof status !== "string" || typeof priority !== "string"
                || typeof shippingAddress !== "string" || typeof notes !== "string") {
            orderEditErrors = { status: "Choose a valid status",
                priority: "Choose a valid priority",
                shippingAddress: "Shipping address must be text" }
            orderEditMutationState = RuntimeModels.MutationIdle
            return false
        }
        orderEditErrors = {
            status: validStatuses.indexOf(status) < 0 ? "Choose a valid status" : "",
            priority: ["normal", "high"].indexOf(priority) < 0 ? "Choose a valid priority" : "",
            shippingAddress: shippingAddress.trim().length === 0 ? "Shipping address is required"
                                                                  : shippingAddress.length > 500 ? "Shipping address is too long" : ""
        }
        orderEditServerError = ""
        orderEditMessage = ""
        if (notes.length > 500) orderEditServerError = "Notes are too long"
        if (orderEditErrors.status.length > 0 || orderEditErrors.priority.length > 0
                || orderEditErrors.shippingAddress.length > 0 || notes.length > 500) {
            orderEditMutationState = RuntimeModels.MutationIdle
            return false
        }
        orderEditMutationState = RuntimeModels.MutationSaving
        const body = { status: status, priority: priority,
                       shippingAddress: shippingAddress.trim(), notes: notes }
        if (network("orderEditSave", "PATCH", "/api/orders/" + encodeURIComponent(id), body,
                    "orderEditFlow", { expectedId: id, expectedStatus: body.status,
                        expectedPriority: body.priority,
                        expectedShippingAddress: body.shippingAddress,
                        expectedNotes: body.notes }).length === 0) {
            orderEditMutationState = RuntimeModels.MutationFailure
            orderEditServerError = "Runtime is unavailable"
            return false
        }
        return true
    }

    function loadCustomers(query, page) {
        if (typeof query !== "string" || !Number.isInteger(page)
                || page < 1 || page > 100000) {
            customersState = RuntimeModels.Error
            customersError = "Invalid customer search"
            return false
        }
        customersQuery = query.trim()
        customersPage = Math.max(1, page)
        customersState = RuntimeModels.Loading
        customersError = ""
        const path = "/api/customers?page=" + customersPage
                   + "&pageSize=20&query=" + encodeURIComponent(customersQuery)
        if (network("customers", "GET", path, null, "customers",
                    { expectedPage: customersPage,
                      expectedQuery: customersQuery.toLowerCase() }).length === 0) {
            customersState = RuntimeModels.Error
            customersError = "Runtime is unavailable"
            return false
        }
        return true
    }

    function loadCustomer(id) {
        if (typeof id !== "string" || id.trim().length === 0 || id.length > 128) {
            customerDetailState = RuntimeModels.Error
            customerDetailError = "Invalid customer id"
            return false
        }
        customerDetailState = RuntimeModels.Loading
        customerDetailError = ""
        if (network("customerDetail", "GET", "/api/customers/" + encodeURIComponent(id), null,
                    "customerDetail", { expectedId: id }).length === 0) {
            customerDetailState = RuntimeModels.Error
            customerDetailError = "Runtime is unavailable"
            return false
        }
        return true
    }

    function openFile() {
        if (laneBusy("file")) return false
        fileState = RuntimeModels.Loading
        fileMessage = "Opening file"
        if (begin("file", "file", "open", {}, "file", { expectedKind: "open" }).length === 0) {
            fileState = RuntimeModels.Error
            fileMessage = "Runtime is unavailable"
            return false
        }
        return true
    }

    function loadSettings() {
        if (laneBusy("settings")) return false
        settingsLoading = true
        settingsError = ""
        settingsMessage = "Loading settings"
        if (begin("settingsLoad", "storage", "get", { key: "theme" }, "settings",
                  { expectedVersion: requestGeneration + 1 }).length === 0) {
            settingsLoading = false
            settingsError = "Settings unavailable"
            settingsMessage = "Settings unavailable"
            return false
        }
        return true
    }

    function persistTheme(name) {
        if (typeof name !== "string" || (name !== "dark" && name !== "light")) {
            settingsError = "Invalid theme"
            settingsMessage = settingsError
            return false
        }
        themeName = name === "dark" ? "dark" : "light"
        settingsLoading = false
        settingsSaving = true
        settingsDirty = true
        settingsPersisted = false
        settingsError = ""
        settingsMessage = "Saving settings"
        if (begin("settingsSave", "storage", "set", { key: "theme", value: themeName },
                  "settings", { expectedValue: themeName,
                                expectedVersion: requestGeneration + 1 }).length === 0) {
            settingsSaving = false
            settingsError = "Settings unavailable"
            settingsMessage = "Settings unavailable"
            return false
        }
        return true
    }

    function complete(requestId, response) {
        if (typeof requestId !== "string" || requestId.length === 0)
            return
        const context = pending[requestId]
        if (!context || typeof context.kind !== "string")
            return
        const next = Object.assign({}, pending)
        delete next[requestId]
        pending = next
        const active = Object.assign({}, activeRequestByLane)
        if (active[context.lane] === requestId)
            delete active[context.lane]
        activeRequestByLane = active
        if (latestGeneration[context.lane] !== context.generation)
            return
        const kind = context.kind
        const body = responseBody(response)
        const status = response && response.result ? response.result.status : 0
        const httpError = safeHttpError(body)
        if (kind === "login") {
            if (response && response.ok === true && status >= 200 && status < 300
                    && isPlainObject(body) && isText(body.token, 4096)
                    && isPlainObject(body.user) && isText(body.user.name, 256)
                    && isText(body.user.email, 320)
                    && isText(body.user.id, 128) && isText(body.user.role, 128)
                    && body.user.email === context.expectedEmail) {
                authenticated = true; user = body.user; serverError = ""
            } else serverError = response && response.ok === true ? httpError
                                                                   : safeError(response, "Sign in failed")
        } else if (kind === "dashboard") {
            if (response && response.ok === true && status >= 200 && status < 300
                    && validDashboard(body)) {
                const dashboardValue = Object.assign({}, body)
                dashboardValue.recentActivity = displayRows(body.recentActivity || [], "text")
                dashboardValue.revenueByMonth = (body.revenueByMonth || []).map(function(item) {
                    const row = Object.assign({}, item)
                    const cents = Number(item && item.amountCents) || 0
                    row.display = String(item && item.month ? item.month : "") + " — $"
                                + (cents / 100).toLocaleString(Qt.locale("en_US"), "f", 2)
                    return row
                })
                dashboard = dashboardValue
                const kpis = body.kpis || ({})
                const hasMetrics = Number(kpis.orderCount) !== 0
                        || Number(kpis.customerCount) !== 0 || Number(kpis.pendingCount) !== 0
                        || Number(kpis.revenueCents) !== 0
                dashboardState = hasMetrics || dashboardValue.revenueByMonth.length > 0
                        || dashboardValue.recentActivity.length > 0
                        ? RuntimeModels.Content : RuntimeModels.Empty
            } else { dashboardError = response && response.ok === true ? httpError
                                                                        : safeError(response, "Dashboard failed")
                     dashboardState = RuntimeModels.Error }
        } else if (kind === "orders") {
            if (response && response.ok === true && status >= 200 && status < 300
                    && validPage(body, context, "customerName")) {
                orders = displayRows(body.items || [], "customerName"); ordersPage = body.page || 1
                ordersTotalPages = body.totalPages
                ordersState = orders.length === 0 ? RuntimeModels.Empty : RuntimeModels.Content
            } else { ordersError = response && response.ok === true ? httpError
                                                                     : safeError(response, "Orders failed")
                     ordersState = RuntimeModels.Error }
        } else if (kind === "orderDetail") {
            if (response && response.ok === true && status >= 200 && status < 300
                    && validOrder(body, context.expectedId)) {
                order = body; orderDetailState = RuntimeModels.Content
            } else { orderDetailError = response && response.ok === true ? httpError
                                                                          : safeError(response, "Order failed")
                     orderDetailState = RuntimeModels.Error }
        } else if (kind === "orderStatus") {
            if (response && response.ok === true && status >= 200 && status < 300
                    && validOrder(body, context.expectedId)
                    && body.status === context.expectedStatus) {
                order = body; orderStatusMutationState = RuntimeModels.MutationSuccess
                orderStatusError = ""; orderDetailMessage = "Order status updated"
            } else { orderStatusError = response && response.ok === true ? httpError
                                                                           : safeError(response, "Update failed")
                     orderStatusMutationState = RuntimeModels.MutationFailure }
        } else if (kind === "orderEditLoad") {
            if (response && response.ok === true && status >= 200 && status < 300
                    && validOrder(body, context.expectedId)) {
                order = body
                editStatus = typeof body.status === "string" ? body.status : ""
                editPriority = typeof body.priority === "string" ? body.priority : ""
                editShippingAddress = typeof body.shippingAddress === "string"
                        ? body.shippingAddress : ""
                editNotes = typeof body.notes === "string" ? body.notes : ""
                orderEditState = RuntimeModels.Content; orderEditLoadError = ""
                orderEditLoaded()
            } else { orderEditLoadError = response && response.ok === true ? httpError
                                                                            : safeError(response, "Order failed")
                     orderEditState = RuntimeModels.Error }
        } else if (kind === "orderEditSave") {
            if (response && response.ok === true && status >= 200 && status < 300
                    && validOrder(body, context.expectedId)
                    && body.status === context.expectedStatus
                    && body.priority === context.expectedPriority
                    && body.shippingAddress === context.expectedShippingAddress
                    && body.notes === context.expectedNotes) {
                order = body; orderEditMutationState = RuntimeModels.MutationSuccess
                orderEditServerError = ""
                orderEditMessage = "Order saved"; orderSaved(String(body.id || ""))
            } else { orderEditServerError = response && response.ok === true ? httpError
                                                                              : safeError(response, "Update failed")
                     orderEditMutationState = RuntimeModels.MutationFailure }
        } else if (kind === "customers") {
            if (response && response.ok === true && status >= 200 && status < 300
                    && validPage(body, context, "name")
                    && body.items.every(function(item) { return validCustomerSummary(item) })) {
                customers = displayRows(body.items || [], "name")
                customersPage = body.page || 1
                customersTotalPages = body.totalPages
                customersState = customers.length === 0 ? RuntimeModels.Empty : RuntimeModels.Content
            } else { customersError = response && response.ok === true ? httpError
                                                                        : safeError(response, "Customers failed")
                     customersState = RuntimeModels.Error }
        } else if (kind === "customerDetail") {
            if (response && response.ok === true && status >= 200 && status < 300
                    && validCustomerDetail(body, context.expectedId)) {
                const detail = Object.assign({}, body)
                detail.orders = displayRows(body.orders || [], "customerName")
                customer = detail; customerDetailState = RuntimeModels.Content
            } else { customerDetailError = response && response.ok === true ? httpError
                                                                             : safeError(response, "Customer failed")
                     customerDetailState = RuntimeModels.Error }
        } else if (kind === "file") {
            if (response && response.ok === true && isPlainObject(response.result)
                    && isText(response.result.name, 500)
                    && isInteger(response.result.size, 0, Number.MAX_SAFE_INTEGER)) {
                fileMetadata = { name: response.result.name, size: response.result.size }
                fileMessage = response.result.name + " (" + response.result.size + " bytes)"
                fileState = RuntimeModels.Content
            } else if (response && response.error && response.error.code === "file.cancelled") {
                fileMetadata = ({}); fileMessage = safeError(response, "No file selected")
                fileState = RuntimeModels.Empty
            } else { fileMessage = safeError(response, "File open failed"); fileState = RuntimeModels.Error }
        } else if (kind === "settingsLoad") {
            settingsLoading = false
            if (response && response.ok === true && isPlainObject(response.result)
                    && (response.result.value === "dark" || response.result.value === "light")) {
                themeName = response.result.value
                settingsPersisted = true; settingsDirty = false; settingsError = ""
                settingsMessage = "Settings loaded"
            } else if (response && response.error && response.error.code === "storage.not_found") {
                themeName = "light"; settingsPersisted = false; settingsDirty = false
                settingsError = ""; settingsMessage = "Using default settings"
            } else { settingsError = safeError(response, "Settings unavailable")
                     settingsMessage = settingsError }
        } else if (kind === "settingsSave") {
            settingsSaving = false
            if (response && response.ok === true && isPlainObject(response.result)) {
                settingsDirty = false; settingsPersisted = true; settingsError = ""
                settingsMessage = "Settings saved"
            } else {
                settingsDirty = true; settingsPersisted = false
                settingsError = safeError(response, "Settings could not be saved")
                settingsMessage = settingsError
            }
        }
    }

    property Connections completionConnection: Connections {
        target: root.runtime
        enabled: root.runtime !== null
        function onCapabilityFinished(requestId, response) { root.complete(requestId, response) }
    }
}
