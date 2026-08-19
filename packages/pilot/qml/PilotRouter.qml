pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import Company.Design
import "pages" as Pages

Item {
    id: root
    property var runtime: null
    property string route: "/login"
    readonly property string normalizedRoute: route.split("?")[0].split("#")[0]
    readonly property alias navigationControl: navigation
    readonly property int navigationIndex: {
        if (normalizedRoute === "/dashboard") return 0
        if (normalizedRoute === "/orders" || /^\/orders\/[^/]+(?:\/edit)?$/.test(normalizedRoute)) return 1
        if (normalizedRoute === "/customers" || /^\/customers\/[^/]+$/.test(normalizedRoute)) return 2
        if (normalizedRoute === "/files") return 3
        if (normalizedRoute === "/settings") return 4
        return -1
    }
    onNavigationIndexChanged: {
        if (navigation.currentIndex !== navigationIndex)
            navigation.currentIndex = navigationIndex
    }
    readonly property string orderId: {
        const match = /^\/orders\/([^/]+)(?:\/edit)?$/.exec(normalizedRoute)
        return match ? decodeURIComponent(match[1]) : ""
    }
    readonly property string customerId: {
        const match = /^\/customers\/([^/]+)$/.exec(normalizedRoute)
        return match ? decodeURIComponent(match[1]) : ""
    }
    readonly property string currentPageName: {
        switch (pageKey(normalizedRoute)) {
        case "login": return "LoginPage"
        case "dashboard": return "DashboardPage"
        case "orders": return "OrdersPage"
        case "orderDetail": return "OrderDetailPage"
        case "orderEdit": return "OrderEditPage"
        case "customers": return "CustomersPage"
        case "customerDetail": return "CustomerDetailPage"
        case "files": return "FilesPage"
        case "settings": return "SettingsPage"
        default: return "NotFoundPage"
        }
    }

    function navigate(target) {
        const normalizedTarget = String(target).split("?")[0].split("#")[0]
        if (normalizedTarget === normalizedRoute)
            return false
        if (runtime && typeof runtime.loadRoute === "function")
            runtime.loadRoute(target)
        else
            route = target
        return true
    }

    function pageKey(path) {
        if (path === "/login") return "login"
        if (path === "/dashboard") return "dashboard"
        if (path === "/orders") return "orders"
        if (/^\/orders\/[^/]+\/edit$/.test(path)) return "orderEdit"
        if (/^\/orders\/[^/]+$/.test(path)) return "orderDetail"
        if (path === "/customers") return "customers"
        if (/^\/customers\/[^/]+$/.test(path)) return "customerDetail"
        if (path === "/files") return "files"
        if (path === "/settings") return "settings"
        return "notFound"
    }

    RowLayout {
        anchors.fill: parent
        spacing: Spacing.md
        AppNavigation {
            id: navigation
            Layout.fillHeight: true
            visible: root.normalizedRoute !== "/login"
            currentIndex: root.navigationIndex
            model: [
                { label: "Dashboard", route: "/dashboard" },
                { label: "Orders", route: "/orders" },
                { label: "Customers", route: "/customers" },
                { label: "Files", route: "/files" },
                { label: "Settings", route: "/settings" }
            ]
            accessibleName: "Primary navigation"
            onActivated: (index, value) => root.navigate(value.route)
        }
        Loader {
            id: pageLoader
            Layout.fillWidth: true
            Layout.fillHeight: true
            sourceComponent: {
                switch (root.pageKey(root.normalizedRoute)) {
                case "login": return loginPage
                case "dashboard": return dashboardPage
                case "orders": return ordersPage
                case "orderDetail": return orderDetailPage
                case "orderEdit": return orderEditPage
                case "customers": return customersPage
                case "customerDetail": return customerDetailPage
                case "files": return filesPage
                case "settings": return settingsPage
                default: return notFoundPage
                }
            }
        }
    }

    Component { id: loginPage; Pages.LoginPage { runtime: root.runtime; onNavigateRequested: route => root.navigate(route) } }
    Component { id: dashboardPage; Pages.DashboardPage { runtime: root.runtime; Component.onCompleted: refresh() } }
    Component { id: ordersPage; Pages.OrdersPage { runtime: root.runtime; onNavigateRequested: route => root.navigate(route); Component.onCompleted: search("", "all", 1) } }
    Component { id: orderDetailPage; Pages.OrderDetailPage { runtime: root.runtime; orderId: root.orderId; onNavigateRequested: route => root.navigate(route); Component.onCompleted: refresh() } }
    Component { id: orderEditPage; Pages.OrderEditPage { runtime: root.runtime; orderId: root.orderId; onNavigateRequested: route => root.navigate(route); Component.onCompleted: refresh() } }
    Component { id: customersPage; Pages.CustomersPage { runtime: root.runtime; onNavigateRequested: route => root.navigate(route); Component.onCompleted: search("", 1) } }
    Component { id: customerDetailPage; Pages.CustomerDetailPage { runtime: root.runtime; customerId: root.customerId; onNavigateRequested: route => root.navigate(route); Component.onCompleted: refresh() } }
    Component { id: filesPage; Pages.FilesPage { runtime: root.runtime } }
    Component { id: settingsPage; Pages.SettingsPage { runtime: root.runtime; Component.onCompleted: loadSettings() } }
    Component {
        id: notFoundPage
        StateView {
            viewState: StateView.Error
            errorMessage: "Route not found"
            accessibleName: "Route not found"
        }
    }
}
