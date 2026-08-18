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
    readonly property color borderStrong: dark ? "#9ca3af" : "#6b7280"
    readonly property color primary: dark ? "#1d4ed8" : "#2563eb"
    readonly property color primaryPressed: dark ? "#1e40af" : "#1d4ed8"
    readonly property color textOnPrimary: "#ffffff"
    readonly property color focus: dark ? "#fbbf24" : "#b45309"
    readonly property color focusOnPrimary: dark ? "#f9fafb" : "#111827"
    readonly property color textOnFocus: dark ? "#111827" : "#ffffff"
    readonly property color error: dark ? "#fca5a5" : "#b91c1c"
    readonly property color errorSurface: dark ? "#451a1a" : "#fef2f2"
    readonly property color success: dark ? "#86efac" : "#15803d"
    readonly property color disabled: dark ? "#6b7280" : "#9ca3af"
    readonly property color disabledText: dark ? "#d1d5db" : "#4b5563"
    readonly property color statusNeutralBackground: dark ? "#374151" : "#f3f4f6"
    readonly property color statusNeutralText: dark ? "#f9fafb" : "#374151"
    readonly property color statusNeutralBorder: dark ? "#d1d5db" : "#6b7280"
    readonly property color statusSuccessBackground: dark ? "#14532d" : "#dcfce7"
    readonly property color statusSuccessText: dark ? "#dcfce7" : "#166534"
    readonly property color statusSuccessBorder: dark ? "#86efac" : "#15803d"
    readonly property color statusWarningBackground: dark ? "#78350f" : "#fef3c7"
    readonly property color statusWarningText: dark ? "#fef3c7" : "#78350f"
    readonly property color statusWarningBorder: dark ? "#fde68a" : "#92400e"
    readonly property color statusErrorBackground: dark ? "#7f1d1d" : "#fee2e2"
    readonly property color statusErrorText: dark ? "#fee2e2" : "#991b1b"
    readonly property color statusErrorBorder: dark ? "#fca5a5" : "#b91c1c"
    readonly property color scrim: "#80000000"
}
