import QtQuick
import QtTest
import Company.Design

TestCase {
    name: "AppTable"
    when: windowShown

    Component {
        id: tableComponent

        AppTable {
            width: 400
            height: 120
            accessibleName: "Orders"
        }
    }

    function linearChannel(channel) {
        const normalized = channel / 255.0
        return normalized <= 0.04045 ? normalized / 12.92
                                       : Math.pow((normalized + 0.055) / 1.055, 2.4)
    }

    function luminance(color) {
        const hex = color.toString().substring(1)
        return 0.2126 * linearChannel(parseInt(hex.substring(0, 2), 16))
                + 0.7152 * linearChannel(parseInt(hex.substring(2, 4), 16))
                + 0.0722 * linearChannel(parseInt(hex.substring(4, 6), 16))
    }

    function contrast(first, second) {
        const firstLuminance = luminance(first)
        const secondLuminance = luminance(second)
        return (Math.max(firstLuminance, secondLuminance) + 0.05)
                / (Math.min(firstLuminance, secondLuminance) + 0.05)
    }

    function test_emptyState() {
        const table = createTemporaryObject(tableComponent, this, { "model": [] })
        verify(table)

        verify(table.empty)
        verify(table.emptyVisible)
        compare(table.currentIndex, -1)
    }

    function test_selectionIsBoundedAndAccessible() {
        const table = createTemporaryObject(tableComponent, this,
                                             { "model": ["One", "Two", "Three"] })
        verify(table)

        verify(table.select(1))
        compare(table.currentIndex, 1)
        verify(!table.select(99))
        compare(table.currentIndex, 1)
        compare(table.Accessible.name, "Orders")
        compare(table.Accessible.role, Accessible.Table)
    }

    function test_largeModelsUseVirtualizedDelegates() {
        const rows = []
        for (let index = 0; index < 2000; ++index)
            rows.push("Row " + index)

        const table = createTemporaryObject(tableComponent, this, { "model": rows })
        verify(table)
        tryVerify(function() { return table.instantiatedItemCount > 0 })

        verify(table.virtualizationEnabled)
        verify(table.instantiatedItemCount < rows.length)
        verify(table.itemAtIndex(1999) === null)
    }

    function test_integerModelUsesListViewCount() {
        const table = createTemporaryObject(tableComponent, this, { "model": 3 })
        verify(table)

        tryCompare(table, "count", 3)
        verify(!table.empty)
        verify(table.select(2))
        compare(table.currentIndex, 2)
    }

    function test_bothThemesStyleActualRowAndVisibleSelection() {
        const originalDark = Theme.dark
        for (const dark of [false, true]) {
            Theme.dark = dark
            const table = createTemporaryObject(tableComponent, this,
                                                 { "model": ["One", "Two"] })
            verify(table)
            tryVerify(function() { return table.itemAtIndex(0) !== null })
            verify(table.select(0))
            const row = table.itemAtIndex(0)

            compare(row.contentItem.color.toString(), Theme.textPrimary.toString())
            compare(row.background.color.toString(), Theme.surfaceRaised.toString())
            verify(row.background.border.width >= Spacing.focusRing)
            verify(contrast(row.background.border.color,
                            row.background.color) >= 3.0)
            verify(row.Accessible.selectable)
            verify(row.Accessible.selected)
        }
        Theme.dark = originalDark
    }
}
