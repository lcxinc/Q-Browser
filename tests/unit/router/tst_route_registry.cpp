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
    void distinguishesInvalidPatternFromDuplicateShape();
    void rejectsInvalidEngineWithoutChangingState();
    void rejectsInvalidPattern_data();
    void rejectsInvalidPattern();
    void rejectsAmbiguousReservedStaticSegment_data();
    void rejectsAmbiguousReservedStaticSegment();
    void returnsNotFoundForInvalidOrUnknownPath_data();
    void returnsNotFoundForInvalidOrUnknownPath();
    void rejectsUnsafeEncodedPath_data();
    void rejectsUnsafeEncodedPath();
    void invalidMatchHasInvalidEngine();
    void returnsIndependentValueObject();
};

void RouteRegistryTest::matchesExactStaticRoute()
{
    RouteRegistry registry;
    QCOMPARE(registry.add(route(QStringLiteral("/orders"),
                                Engine::TrustedQml,
                                QStringLiteral("com.qbrowser.shell"),
                                QStringLiteral("Orders.qml"))),
             RouteAddResult::Added);

    const auto match = registry.match(QStringLiteral("/orders"));

    QVERIFY(match.isValid());
    QCOMPARE(match.record.pattern, QStringLiteral("/orders"));
    QVERIFY(match.parameters.isEmpty());
}

void RouteRegistryTest::extractsParameterFromRoute()
{
    RouteRegistry registry;
    QCOMPARE(registry.add(route(QStringLiteral("/orders/:id"),
                                Engine::QmlWorker,
                                QStringLiteral("com.qbrowser.pilot"),
                                QStringLiteral("OrderDetail.qml"))),
             RouteAddResult::Added);

    const auto match = registry.match(QStringLiteral("/orders/42"));

    QVERIFY(match.isValid());
    QCOMPARE(match.parameters.value(QStringLiteral("id")), QStringLiteral("42"));
    QCOMPARE(match.record.engine, Engine::QmlWorker);
}

void RouteRegistryTest::matchesMultipleSegmentsAndParameters()
{
    RouteRegistry registry;
    QCOMPARE(registry.add(route(QStringLiteral("/customers/:customerId/orders/:orderId/edit"))),
             RouteAddResult::Added);

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

    QCOMPARE(registry.add(staticFirst ? fixed : parameter), RouteAddResult::Added);
    QCOMPARE(registry.add(staticFirst ? parameter : fixed), RouteAddResult::Added);

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
    QCOMPARE(registry.add(route(path, engine, packageId, entryPoint)), RouteAddResult::Added);

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
    QCOMPARE(registry.add(route(QStringLiteral("/customers/:name"))), RouteAddResult::Added);

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
    QCOMPARE(registry.add(route(QStringLiteral("/customers/:name"))), RouteAddResult::Added);

    QVERIFY(!registry.match(QStringLiteral("/customers/") + encoded).isValid());
}

void RouteRegistryTest::rejectsDuplicateNormalizedPattern()
{
    RouteRegistry registry;

    QCOMPARE(registry.add(route(QStringLiteral("/orders/:id"))), RouteAddResult::Added);
    QCOMPARE(registry.add(route(QStringLiteral("/orders/:id"), Engine::WebEngine)),
             RouteAddResult::DuplicateShape);
}

void RouteRegistryTest::rejectsConflictingParameterNamesAtSameShape()
{
    RouteRegistry registry;

    QCOMPARE(registry.add(route(QStringLiteral("/orders/:id/edit"))), RouteAddResult::Added);
    QCOMPARE(registry.add(route(QStringLiteral("/orders/:orderId/edit"))),
             RouteAddResult::DuplicateShape);
}

void RouteRegistryTest::distinguishesInvalidPatternFromDuplicateShape()
{
    RouteRegistry registry;

    QCOMPARE(registry.add(route(QStringLiteral("orders/:id"))), RouteAddResult::InvalidPattern);
    QCOMPARE(registry.add(route(QStringLiteral("/orders/:id"))), RouteAddResult::Added);
    QCOMPARE(registry.add(route(QStringLiteral("/orders/:orderId"))),
             RouteAddResult::DuplicateShape);
}

void RouteRegistryTest::rejectsInvalidEngineWithoutChangingState()
{
    RouteRegistry registry;
    RouteRecord invalidRecord;
    invalidRecord.pattern = QStringLiteral("/orders");

    QCOMPARE(registry.add(invalidRecord), RouteAddResult::InvalidEngine);
    QVERIFY(!registry.match(QStringLiteral("/orders")).isValid());

    QCOMPARE(registry.add(route(QStringLiteral("/orders"))), RouteAddResult::Added);
    QVERIFY(registry.match(QStringLiteral("/orders")).isValid());
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

    QCOMPARE(registry.add(route(pattern)), RouteAddResult::InvalidPattern);
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

    QCOMPARE(registry.add(route(pattern)), RouteAddResult::InvalidPattern);
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
    QCOMPARE(registry.add(route(QStringLiteral("/orders/:id"))), RouteAddResult::Added);

    QVERIFY(!registry.match(path).isValid());
}

void RouteRegistryTest::rejectsUnsafeEncodedPath_data()
{
    QTest::addColumn<QString>("path");

    QTest::newRow("invalid-utf8") << QStringLiteral("/orders/%FF");
    QTest::newRow("decoded-forward-slash") << QStringLiteral("/orders/acme%2Fadmin");
    QTest::newRow("decoded-backslash") << QStringLiteral("/orders/acme%5Cadmin");
    QTest::newRow("encoded-nul") << QStringLiteral("/orders/%00");
    QTest::newRow("encoded-control") << QStringLiteral("/orders/%1F");
    QTest::newRow("encoded-unicode-control") << QStringLiteral("/orders/%C2%80");
    QTest::newRow("illegal-literal") << QStringLiteral("/orders/[admin]");
    QTest::newRow("trailing-slash") << QStringLiteral("/orders/");

    QString literalControl = QStringLiteral("/orders/");
    literalControl.append(QChar(0x1f));
    QTest::newRow("literal-control") << literalControl;
}

void RouteRegistryTest::rejectsUnsafeEncodedPath()
{
    QFETCH(QString, path);

    RouteRegistry registry;
    QCOMPARE(registry.add(route(QStringLiteral("/orders/:id"))), RouteAddResult::Added);

    QVERIFY(!registry.match(path).isValid());
}

void RouteRegistryTest::invalidMatchHasInvalidEngine()
{
    const RouteRecord defaultRecord;
    QCOMPARE(defaultRecord.engine, Engine::Invalid);

    const RouteRegistry registry;
    const auto match = registry.match(QStringLiteral("/not-found"));

    QVERIFY(!match.isValid());
    QCOMPARE(match.record.engine, Engine::Invalid);
}

void RouteRegistryTest::returnsIndependentValueObject()
{
    RouteRegistry registry;
    QCOMPARE(registry.add(route(QStringLiteral("/orders/:id"),
                                Engine::QmlWorker,
                                QStringLiteral("com.qbrowser.pilot"),
                                QStringLiteral("OrderDetail.qml"))),
             RouteAddResult::Added);

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
