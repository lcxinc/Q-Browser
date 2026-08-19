import QtQuick
import Company.Design

Rectangle {
    id: root
    width: 1100
    height: 720
    color: Theme.background
    Accessible.name: "Pilot Console"
    Accessible.role: Accessible.Application

    PilotRouter {
        anchors.fill: parent
        // Runtime is an intentionally injected Worker context property.
        // qmllint disable unqualified
        runtime: typeof Runtime !== "undefined" ? Runtime : null
        // qmllint enable unqualified
        route: runtime && typeof runtime.route === "string" ? runtime.route : "/login"
    }
}
