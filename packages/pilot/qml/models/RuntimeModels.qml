import QtQuick
import "../Base64.js" as Base64

QtObject {
    id: root

    enum LoadState { Content, Loading, Empty, Error }

    property var runtime: null
    property string apiOrigin: runtime && typeof runtime.apiOrigin === "string"
                               && runtime.apiOrigin.length > 0
                               ? runtime.apiOrigin : "http://127.0.0.1:4173"
    property var pending: ({})

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
    property var order: ({})
    property int orderEditState: RuntimeModels.Empty
    property var orderEditErrors: ({ status: "", priority: "", shippingAddress: "" })
    property string orderEditServerError: ""

    property int customersState: RuntimeModels.Empty
    property string customersError: ""
    property var customers: []
    property int customerDetailState: RuntimeModels.Empty
    property string customerDetailError: ""
    property var customer: ({})

    property int fileState: RuntimeModels.Empty
    property string fileMessage: "No file selected"
    property var fileMetadata: ({})

    property string themeName: "light"
    property string settingsMessage: ""

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

    function safeError(response, fallback) {
        if (response && response.error && typeof response.error.message === "string")
            return response.error.message
        return fallback
    }

    function responseBody(response) {
        if (!response || response.ok !== true || !response.result
                || typeof response.result.bodyBase64 !== "string")
            return null
        return decodedJson(response.result.bodyBase64)
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

    function begin(kind, capability, operation, payload) {
        if (!runtime || typeof runtime.invoke !== "function")
            return ""
        const requestId = runtime.invoke(capability, operation, payload)
        if (typeof requestId !== "string" || requestId.length === 0)
            return ""
        const next = Object.assign({}, pending)
        next[requestId] = kind
        pending = next
        return requestId
    }

    function network(kind, method, path, body) {
        return begin(kind, "network", "request", {
            method: method,
            url: apiOrigin + path,
            bodyBase64: body === undefined || body === null ? "" : encodedJson(body)
        })
    }

    function login(email, password) {
        const normalizedEmail = email.trim()
        emailError = normalizedEmail.length === 0 ? "Email is required"
                   : !/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(normalizedEmail)
                     ? "Enter a valid email address" : ""
        passwordError = password.length === 0 ? "Password is required"
                      : password.length < 8 ? "Password must contain at least 8 characters" : ""
        serverError = ""
        if (emailError.length > 0 || passwordError.length > 0)
            return false
        authenticated = false
        return network("login", "POST", "/api/login", { email: normalizedEmail, password: password }).length > 0
    }

    function loadDashboard() {
        dashboardState = RuntimeModels.Loading
        dashboardError = ""
        if (network("dashboard", "GET", "/api/dashboard", null).length === 0) {
            dashboardState = RuntimeModels.Error
            dashboardError = "Runtime is unavailable"
        }
    }

    function loadOrders(query, status, page) {
        ordersQuery = query.trim()
        ordersStatus = status.length === 0 ? "all" : status
        ordersPage = Math.max(1, page)
        ordersState = RuntimeModels.Loading
        ordersError = ""
        const path = "/api/orders?page=" + ordersPage + "&pageSize=20&status="
                   + encodeURIComponent(ordersStatus) + "&query=" + encodeURIComponent(ordersQuery)
        if (network("orders", "GET", path, null).length === 0) {
            ordersState = RuntimeModels.Error
            ordersError = "Runtime is unavailable"
        }
    }

    function loadOrder(id) {
        orderDetailState = RuntimeModels.Loading
        orderDetailError = ""
        if (network("orderDetail", "GET", "/api/orders/" + encodeURIComponent(id), null).length === 0) {
            orderDetailState = RuntimeModels.Error
            orderDetailError = "Runtime is unavailable"
        }
    }

    function saveOrder(id, status, priority, shippingAddress, notes) {
        const validStatuses = ["pending", "processing", "shipped", "delivered", "cancelled"]
        orderEditErrors = {
            status: validStatuses.indexOf(status) < 0 ? "Choose a valid status" : "",
            priority: ["normal", "high"].indexOf(priority) < 0 ? "Choose a valid priority" : "",
            shippingAddress: shippingAddress.trim().length === 0 ? "Shipping address is required"
                                                                  : shippingAddress.length > 500 ? "Shipping address is too long" : ""
        }
        orderEditServerError = ""
        if (orderEditErrors.status.length > 0 || orderEditErrors.priority.length > 0
                || orderEditErrors.shippingAddress.length > 0)
            return false
        orderEditState = RuntimeModels.Loading
        const body = { status: status, priority: priority,
                       shippingAddress: shippingAddress.trim(), notes: notes }
        if (network("orderEdit", "PATCH", "/api/orders/" + encodeURIComponent(id), body).length === 0) {
            orderEditState = RuntimeModels.Error
            orderEditServerError = "Runtime is unavailable"
            return false
        }
        return true
    }

    function loadCustomers(query, page) {
        customersState = RuntimeModels.Loading
        customersError = ""
        const path = "/api/customers?page=" + Math.max(1, page)
                   + "&pageSize=20&query=" + encodeURIComponent(query.trim())
        if (network("customers", "GET", path, null).length === 0) {
            customersState = RuntimeModels.Error
            customersError = "Runtime is unavailable"
        }
    }

    function loadCustomer(id) {
        customerDetailState = RuntimeModels.Loading
        customerDetailError = ""
        if (network("customerDetail", "GET", "/api/customers/" + encodeURIComponent(id), null).length === 0) {
            customerDetailState = RuntimeModels.Error
            customerDetailError = "Runtime is unavailable"
        }
    }

    function openFile() {
        fileState = RuntimeModels.Loading
        fileMessage = "Opening file"
        if (begin("file", "file", "open", {}).length === 0) {
            fileState = RuntimeModels.Error
            fileMessage = "Runtime is unavailable"
        }
    }

    function loadSettings() {
        settingsMessage = "Loading settings"
        if (begin("settingsLoad", "storage", "get", { key: "theme" }).length === 0)
            settingsMessage = "Settings unavailable"
    }

    function persistTheme(name) {
        themeName = name === "dark" ? "dark" : "light"
        settingsMessage = "Saving settings"
        if (begin("settingsSave", "storage", "set", { key: "theme", value: themeName }).length === 0)
            settingsMessage = "Settings unavailable"
    }

    function complete(requestId, response) {
        const kind = pending[requestId]
        if (typeof kind !== "string")
            return
        const next = Object.assign({}, pending)
        delete next[requestId]
        pending = next
        const body = responseBody(response)
        const status = response && response.result ? response.result.status : 0
        const httpError = body && body.error && body.error.message ? body.error.message
                                                                     : "Request failed"
        if (kind === "login") {
            if (response && response.ok === true && status >= 200 && status < 300 && body) {
                authenticated = true; user = body.user || ({}); serverError = ""
            } else serverError = response && response.ok === true ? httpError
                                                                   : safeError(response, "Sign in failed")
        } else if (kind === "dashboard") {
            if (response && response.ok === true && status >= 200 && status < 300 && body) {
                const dashboardValue = Object.assign({}, body)
                dashboardValue.recentActivity = displayRows(body.recentActivity || [], "text")
                dashboard = dashboardValue; dashboardState = RuntimeModels.Content
            } else { dashboardError = response && response.ok === true ? httpError
                                                                        : safeError(response, "Dashboard failed")
                     dashboardState = RuntimeModels.Error }
        } else if (kind === "orders") {
            if (response && response.ok === true && status >= 200 && status < 300 && body) {
                orders = displayRows(body.items || [], "customerName"); ordersPage = body.page || 1
                ordersTotalPages = body.totalPages || 1
                ordersState = orders.length === 0 ? RuntimeModels.Empty : RuntimeModels.Content
            } else { ordersError = response && response.ok === true ? httpError
                                                                     : safeError(response, "Orders failed")
                     ordersState = RuntimeModels.Error }
        } else if (kind === "orderDetail") {
            if (response && response.ok === true && status >= 200 && status < 300 && body) {
                order = body; orderDetailState = RuntimeModels.Content
            } else { orderDetailError = response && response.ok === true ? httpError
                                                                          : safeError(response, "Order failed")
                     orderDetailState = RuntimeModels.Error }
        } else if (kind === "orderEdit") {
            if (response && response.ok === true && status >= 200 && status < 300 && body) {
                order = body; orderEditState = RuntimeModels.Content; orderEditServerError = ""
            } else { orderEditServerError = response && response.ok === true ? httpError
                                                                              : safeError(response, "Update failed")
                     orderEditState = RuntimeModels.Error }
        } else if (kind === "customers") {
            if (response && response.ok === true && status >= 200 && status < 300 && body) {
                customers = displayRows(body.items || [], "name")
                customersState = customers.length === 0 ? RuntimeModels.Empty : RuntimeModels.Content
            } else { customersError = response && response.ok === true ? httpError
                                                                        : safeError(response, "Customers failed")
                     customersState = RuntimeModels.Error }
        } else if (kind === "customerDetail") {
            if (response && response.ok === true && status >= 200 && status < 300 && body) {
                const detail = Object.assign({}, body)
                detail.orders = displayRows(body.orders || [], "customerName")
                customer = detail; customerDetailState = RuntimeModels.Content
            } else { customerDetailError = response && response.ok === true ? httpError
                                                                             : safeError(response, "Customer failed")
                     customerDetailState = RuntimeModels.Error }
        } else if (kind === "file") {
            if (response && response.ok === true && response.result) {
                fileMetadata = { name: response.result.name, size: response.result.size }
                fileMessage = response.result.name + " (" + response.result.size + " bytes)"
                fileState = RuntimeModels.Content
            } else if (response && response.error && response.error.code === "file.cancelled") {
                fileMetadata = ({}); fileMessage = response.error.message || "No file selected"
                fileState = RuntimeModels.Empty
            } else { fileMessage = safeError(response, "File open failed"); fileState = RuntimeModels.Error }
        } else if (kind === "settingsLoad") {
            if (response && response.ok === true && response.result) {
                themeName = response.result.value === "dark" ? "dark" : "light"
                settingsMessage = "Settings loaded"
            } else if (response && response.error && response.error.code === "storage.not_found") {
                themeName = "light"; settingsMessage = "Using default settings"
            } else settingsMessage = safeError(response, "Settings unavailable")
        } else if (kind === "settingsSave") {
            settingsMessage = response && response.ok === true ? "Settings saved"
                                                                : safeError(response, "Settings could not be saved")
        }
    }

    property Connections completionConnection: Connections {
        target: root.runtime
        enabled: root.runtime !== null
        function onCapabilityFinished(requestId, response) { root.complete(requestId, response) }
    }
}
