#include "RouteRegistry.h"

#include <QTest>

#include <type_traits>

namespace {

RouteRecord route(const QString &pattern,
                  const Engine engine = Engine::QmlWorker,
                  const QString &packageId = QStringLiteral("com.qbrowser.pilot"),
                  const QString &entryPoint = QStringLiteral("Page.qml"))
{
    return {pattern, engine, packageId, entryPoint};
}

} // namespace

class RouteRegistryTest final : public QObject
{
    Q_OBJECT

private slots:
    void matchesExactStaticRoute();
    void extractsParameterFromRoute();
    void matchesMultipleSegmentsAndParameters();
    void prefersStaticSegmentRegardlessOfRegistrationOrder_data();
    void prefersStaticSegmentRegardlessOfRegistrationOrder();
    void preservesEngineAndTargetMetadata_data();
    void preservesEngineAndTargetMetadata();
    void percentDecodesParameterValues_data();
    void percentDecodesParameterValues();
    void rejectsSlashContainingDecodedParameter_data();
    void rejectsSlashContainingDecodedParameter();
    void rejectsDuplicateNormalizedPattern();
    void rejectsConflictingParameterNamesAtSameShape();
    void rejectsInvalidPattern_data();
    void rejectsInvalidPattern();
    void rejectsAmbiguousReservedStaticSegment_data();
    void rejectsAmbiguousReservedStaticSegment();
    void returnsNotFoundForInvalidOrUnknownPath_data();
    void returnsNotFoundForInvalidOrUnknownPath();
    void returnsIndependentValueObject();
};

void RouteRegistryTest::matchesExactStaticRoute()
{
    RouteRegistry registry;
    QVERIFY(registry.add(route(QStringLiteral("/orders"),
                               Engine::TrustedQml,
                               QStringLiteral("com.qbrowser.shell"),
                               QStringLiteral("Orders.qml"))));

    const auto match = registry.match(QStringLiteral("/orders"));

    QVERIFY(match.isValid());
    QCOMPARE(match.record.pattern, QStringLiteral("/orders"));
    QVERIFY(match.parameters.isEmpty());
}

void RouteRegistryTest::extractsParameterFromRoute()
{
    RouteRegistry registry;
    QVERIFY(registry.add(route(QStringLiteral("/orders/:id"),
                               Engine::QmlWorker,
                               QStringLiteral("com.qbrowser.pilot"),
                               QStringLiteral("OrderDetail.qml"))));

    const auto match = registry.match(QStringLiteral("/orders/42"));

    QVERIFY(match.isValid());
    QCOMPARE(match.parameters.value(QStringLiteral("id")), QStringLiteral("42"));
    QCOMPARE(match.record.engine, Engine::QmlWorker);
}

void RouteRegistryTest::matchesMultipleSegmentsAndParameters()
{
    RouteRegistry registry;
    QVERIFY(registry.add(route(QStringLiteral("/customers/:customerId/orders/:orderId/edit"))));

    const auto match = registry.match(QStringLiteral("/customers/acme/orders/42/edit"));

    QVERIFY(match.isValid());
    QCOMPARE(match.parameters.value(QStringLiteral("customerId")), QStringLiteral("acme"));
    QCOMPARE(match.parameters.value(QStringLiteral("orderId")), QStringLiteral("42"));
}

void RouteRegistryTest::prefersStaticSegmentRegardlessOfRegistrationOrder_data()
{
    QTest::addColumn<bool>("staticFirst");

    QTest::newRow("static-first") << true;
    QTest::newRow("parameter-first") << false;
}

void RouteRegistryTest::prefersStaticSegmentRegardlessOfRegistrationOrder()
{
    QFETCH(bool, staticFirst);

    RouteRegistry registry;
    const auto parameter = route(QStringLiteral("/orders/:id/edit"),
                                 Engine::QmlWorker,
                                 QStringLiteral("com.qbrowser.pilot"),
                                 QStringLiteral("OrderEdit.qml"));
    const auto fixed = route(QStringLiteral("/orders/new/edit"),
                             Engine::TrustedQml,
                             QStringLiteral("com.qbrowser.shell"),
                             QStringLiteral("NewOrder.qml"));

    QVERIFY(registry.add(staticFirst ? fixed : parameter));
    QVERIFY(registry.add(staticFirst ? parameter : fixed));

    const auto match = registry.match(QStringLiteral("/orders/new/edit"));

    QVERIFY(match.isValid());
    QCOMPARE(match.record.entryPoint, QStringLiteral("NewOrder.qml"));
    QVERIFY(match.parameters.isEmpty());
}

void RouteRegistryTest::preservesEngineAndTargetMetadata_data()
{
    QTest::addColumn<QString>("path");
    QTest::addColumn<Engine>("engine");
    QTest::addColumn<QString>("packageId");
    QTest::addColumn<QString>("entryPoint");

    QTest::newRow("trusted-qml") << QStringLiteral("/login") << Engine::TrustedQml
                                  << QStringLiteral("com.qbrowser.shell")
                                  << QStringLiteral("Login.qml");
    QTest::newRow("worker-qml") << QStringLiteral("/orders") << Engine::QmlWorker
                                 << QStringLiteral("com.qbrowser.pilot")
                                 << QStringLiteral("Orders.qml");
    QTest::newRow("web-engine") << QStringLiteral("/legacy") << Engine::WebEngine
                                 << QStringLiteral("com.qbrowser.web")
                                 << QStringLiteral("legacy/index.html");
}

void RouteRegistryTest::preservesEngineAndTargetMetadata()
{
    QFETCH(QString, path);
    QFETCH(Engine, engine);
    QFETCH(QString, packageId);
    QFETCH(QString, entryPoint);

    RouteRegistry registry;
    QVERIFY(registry.add(route(path, engine, packageId, entryPoint)));

    const auto match = registry.match(path);

    QVERIFY(match.isValid());
    QCOMPARE(match.record.engine, engine);
    QCOMPARE(match.record.packageId, packageId);
    QCOMPARE(match.record.entryPoint, entryPoint);
}

void RouteRegistryTest::percentDecodesParameterValues_data()
{
    QTest::addColumn<QString>("encoded");
    QTest::addColumn<QString>("decoded");

    QTest::newRow("space") << QStringLiteral("alice%20smith") << QStringLiteral("alice smith");
    QTest::newRow("unicode") << QStringLiteral("%E2%9C%93") << QString::fromUtf8("✓");
    QTest::newRow("literal-percent") << QStringLiteral("100%25") << QStringLiteral("100%");
}

void RouteRegistryTest::percentDecodesParameterValues()
{
    QFETCH(QString, encoded);
    QFETCH(QString, decoded);

    RouteRegistry registry;
    QVERIFY(registry.add(route(QStringLiteral("/customers/:name"))));

    const auto match = registry.match(QStringLiteral("/customers/") + encoded);

    QVERIFY(match.isValid());
    QCOMPARE(match.parameters.value(QStringLiteral("name")), decoded);
}

void RouteRegistryTest::rejectsSlashContainingDecodedParameter_data()
{
    QTest::addColumn<QString>("encoded");

    QTest::newRow("forward-slash") << QStringLiteral("acme%2Fadmin");
    QTest::newRow("backslash") << QStringLiteral("acme%5Cadmin");
}

void RouteRegistryTest::rejectsSlashContainingDecodedParameter()
{
    QFETCH(QString, encoded);

    RouteRegistry registry;
    QVERIFY(registry.add(route(QStringLiteral("/customers/:name"))));

    QVERIFY(!registry.match(QStringLiteral("/customers/") + encoded).isValid());
}

void RouteRegistryTest::rejectsDuplicateNormalizedPattern()
{
    RouteRegistry registry;

    QVERIFY(registry.add(route(QStringLiteral("/orders/:id"))));
    QVERIFY(!registry.add(route(QStringLiteral("/orders/:id"), Engine::WebEngine)));
}

void RouteRegistryTest::rejectsConflictingParameterNamesAtSameShape()
{
    RouteRegistry registry;

    QVERIFY(registry.add(route(QStringLiteral("/orders/:id/edit"))));
    QVERIFY(!registry.add(route(QStringLiteral("/orders/:orderId/edit"))));
}

void RouteRegistryTest::rejectsInvalidPattern_data()
{
    QTest::addColumn<QString>("pattern");

    QTest::newRow("empty") << QString();
    QTest::newRow("relative") << QStringLiteral("orders/:id");
    QTest::newRow("duplicate-slash") << QStringLiteral("/orders//:id");
    QTest::newRow("trailing-slash") << QStringLiteral("/orders/");
    QTest::newRow("dot") << QStringLiteral("/orders/./edit");
    QTest::newRow("dot-dot") << QStringLiteral("/orders/../edit");
    QTest::newRow("query") << QStringLiteral("/orders?tab=open");
    QTest::newRow("fragment") << QStringLiteral("/orders#open");
    QTest::newRow("empty-parameter") << QStringLiteral("/orders/:");
    QTest::newRow("invalid-parameter-name") << QStringLiteral("/orders/:9id");
    QTest::newRow("duplicate-parameter-name") << QStringLiteral("/orders/:id/:id");
    QTest::newRow("wildcard") << QStringLiteral("/orders/*");
    QTest::newRow("optional") << QStringLiteral("/orders/:id?");
}

void RouteRegistryTest::rejectsInvalidPattern()
{
    QFETCH(QString, pattern);

    RouteRegistry registry;

    QVERIFY(!registry.add(route(pattern)));
}

void RouteRegistryTest::rejectsAmbiguousReservedStaticSegment_data()
{
    QTest::addColumn<QString>("pattern");

    QTest::newRow("encoded-colon") << QStringLiteral("/orders/%3Aid");
    QTest::newRow("colon-inside-static") << QStringLiteral("/orders/id:edit");
    QTest::newRow("encoded-slash") << QStringLiteral("/orders%2Farchive");
}

void RouteRegistryTest::rejectsAmbiguousReservedStaticSegment()
{
    QFETCH(QString, pattern);

    RouteRegistry registry;

    QVERIFY(!registry.add(route(pattern)));
}

void RouteRegistryTest::returnsNotFoundForInvalidOrUnknownPath_data()
{
    QTest::addColumn<QString>("path");

    QTest::newRow("unknown") << QStringLiteral("/customers");
    QTest::newRow("relative") << QStringLiteral("orders/42");
    QTest::newRow("duplicate-slash") << QStringLiteral("/orders//42");
    QTest::newRow("dot-segment") << QStringLiteral("/orders/../42");
    QTest::newRow("malformed-percent") << QStringLiteral("/orders/%GG");
    QTest::newRow("encoded-unreserved") << QStringLiteral("/orders/%34%32");
    QTest::newRow("lowercase-escape") << QStringLiteral("/orders/alice%2fadmin");
    QTest::newRow("query") << QStringLiteral("/orders/42?tab=history");
}

void RouteRegistryTest::returnsNotFoundForInvalidOrUnknownPath()
{
    QFETCH(QString, path);

    RouteRegistry registry;
    QVERIFY(registry.add(route(QStringLiteral("/orders/:id"))));

    QVERIFY(!registry.match(path).isValid());
}

void RouteRegistryTest::returnsIndependentValueObject()
{
    RouteRegistry registry;
    QVERIFY(registry.add(route(QStringLiteral("/orders/:id"),
                               Engine::QmlWorker,
                               QStringLiteral("com.qbrowser.pilot"),
                               QStringLiteral("OrderDetail.qml"))));

    auto first = registry.match(QStringLiteral("/orders/42"));
    static_assert(!std::is_pointer_v<decltype(first)>);
    first.record.entryPoint = QStringLiteral("Changed.qml");
    first.parameters.insert(QStringLiteral("id"), QStringLiteral("changed"));

    const auto second = registry.match(QStringLiteral("/orders/42"));
    QCOMPARE(second.record.entryPoint, QStringLiteral("OrderDetail.qml"));
    QCOMPARE(second.parameters.value(QStringLiteral("id")), QStringLiteral("42"));
}

QTEST_MAIN(RouteRegistryTest)

#include "tst_route_registry.moc"
