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

    function test_darkThemeStylesActualRowAndSelectionSemantics() {
        const originalDark = Theme.dark
        Theme.dark = true
        const table = createTemporaryObject(tableComponent, this,
                                             { "model": ["One", "Two"] })
        verify(table)
        tryVerify(function() { return table.itemAtIndex(0) !== null })
        verify(table.select(0))
        const row = table.itemAtIndex(0)

        compare(row.contentItem.color.toString(), Theme.textPrimary.toString())
        compare(row.background.color.toString(), Theme.surfaceRaised.toString())
        verify(row.Accessible.selectable)
        verify(row.Accessible.selected)
        Theme.dark = originalDark
    }
}
