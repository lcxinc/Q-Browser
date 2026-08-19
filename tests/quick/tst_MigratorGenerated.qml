import QtQuick
import QtTest

TestCase {
    id: testCase
    name: "MigratorGenerated"
    width: 1200
    height: 900
    when: windowShown

    Item {
        anchors.fill: parent

        Loader { id: dashboard; source: "../golden/migrator/dashboard/Main.qml" }
        Loader { id: list; source: "../golden/migrator/list/Main.qml" }
        Loader { id: form; source: "../golden/migrator/form/Main.qml" }
        Loader { id: settings; source: "../golden/migrator/settings/Main.qml" }
    }

    function test_generatedPagesLoadCompanyDesign() {
        compare(dashboard.status, Loader.Ready)
        compare(list.status, Loader.Ready)
        compare(form.status, Loader.Ready)
        compare(settings.status, Loader.Ready)
        compare(dashboard.item.implicitWidth, 1024)
        compare(settings.item.implicitHeight, 720)
    }
}
