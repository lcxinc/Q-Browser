pragma Singleton

import QtQuick

QtObject {
    property bool dark: false

    readonly property color background: dark ? "#111827" : "#f3f4f6"
    readonly property color surface: dark ? "#1f2937" : "#ffffff"
    readonly property color surfaceRaised: dark ? "#374151" : "#f9fafb"
    readonly property color textPrimary: dark ? "#f9fafb" : "#111827"
    readonly property color textSecondary: dark ? "#d1d5db" : "#4b5563"
    readonly property color border: dark ? "#4b5563" : "#d1d5db"
    readonly property color primary: dark ? "#1d4ed8" : "#2563eb"
    readonly property color primaryPressed: dark ? "#1e40af" : "#1d4ed8"
    readonly property color textOnPrimary: "#ffffff"
    readonly property color focus: dark ? "#fbbf24" : "#b45309"
    readonly property color focusOnPrimary: "#ffffff"
    readonly property color error: dark ? "#fca5a5" : "#b91c1c"
    readonly property color errorSurface: dark ? "#451a1a" : "#fef2f2"
    readonly property color success: dark ? "#86efac" : "#15803d"
    readonly property color disabled: dark ? "#6b7280" : "#9ca3af"
    readonly property color scrim: "#80000000"
}
