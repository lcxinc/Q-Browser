#include "Manifest.h"
#include "PilotRoutes.h"
#include "HostPolicy.h"
#include "RouteRegistry.h"
#include "ProtocolMessage.h"
#include "WorkerTestEnvironment.h"

#include <QDir>
#include <QFile>
#include <QDirIterator>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTest>

namespace {
QStringList sourcePolicyViolations(const QByteArray &source)
{
    const QString text = QString::fromUtf8(source);
    QStringList violations;
    const auto addRegex = [&text, &violations](const QString &name,
                                               const QString &pattern,
                                               QRegularExpression::PatternOptions options = {}) {
        const QRegularExpression expression(pattern, options);
        Q_ASSERT(expression.isValid());
        if (expression.match(text).hasMatch())
            violations.push_back(name);
    };
    const auto addLiteral = [&source, &violations](const QString &name,
                                                   const QByteArray &literal) {
        if (source.contains(literal))
            violations.push_back(name);
    };

    addRegex(QStringLiteral("dynamic-import"), QStringLiteral("\\bimport\\s*\\("));
    addRegex(QStringLiteral("dynamic-loader-source"), QStringLiteral("\\bsource\\s*:"));
    addRegex(QStringLiteral("loader-set-source"), QStringLiteral("\\.\\s*setSource\\s*\\("));
    addRegex(QStringLiteral("qml-create-component"),
             QStringLiteral("\\bQt\\s*\\.\\s*createComponent\\s*\\("));
    addRegex(QStringLiteral("qml-create-object"),
             QStringLiteral("\\bQt\\s*\\.\\s*createQmlObject\\s*\\("));
    addRegex(QStringLiteral("qt-include"),
             QStringLiteral("\\bQt\\s*\\.\\s*include\\s*\\("));
    addRegex(QStringLiteral("remote-import"),
             QStringLiteral("^\\s*import\\s+[\\\"'](?:https?|file):"),
             QRegularExpression::MultilineOption);
    addLiteral(QStringLiteral("raw-network"), QByteArrayLiteral("XMLHttpRequest"));
    addLiteral(QStringLiteral("worker-network"), QByteArrayLiteral("WorkerScript"));
    addLiteral(QStringLiteral("file-url"), QByteArrayLiteral("file://"));
    addLiteral(QStringLiteral("native-file"), QByteArrayLiteral("QFile"));
    addLiteral(QStringLiteral("native-file-dialog"), QByteArrayLiteral("FileDialog"));
    addLiteral(QStringLiteral("external-url"), QByteArrayLiteral("Qt.openUrlExternally"));
    addLiteral(QStringLiteral("native-plugin"), QByteArrayLiteral("plugin "));
    return violations;
}
} // namespace

class PilotRoutesTest final : public QObject
{
    Q_OBJECT

private slots:
    void packageManifestDeclaresWorkerInventory();
    void hostInventoryResolvesPatternToPageAndEngine();
    void realWorkerLoadsPilotAndEmitsTypedCapability();
    void sourcePolicyForbidsRawNetworkFilesystemAndNativeCode();
    void sourcePolicyRejectsDynamicQmlConstructionAndLoading();
};

void PilotRoutesTest::sourcePolicyRejectsDynamicQmlConstructionAndLoading()
{
    const QString path = QDir(QStringLiteral(Q_BROWSER_SOURCE_DIR))
                             .filePath(QStringLiteral("tests/fixtures/pilot/source-policy-attack.qml"));
    QFile fixture(path);
    QVERIFY2(fixture.open(QIODevice::ReadOnly), qPrintable(path));
    const QStringList violations = sourcePolicyViolations(fixture.readAll());
    QCOMPARE(violations,
             QStringList({QStringLiteral("dynamic-import"),
                          QStringLiteral("dynamic-loader-source"),
                          QStringLiteral("loader-set-source"),
                          QStringLiteral("qml-create-component"),
                          QStringLiteral("qml-create-object"),
                          QStringLiteral("qt-include"),
                          QStringLiteral("remote-import")}));

    QCOMPARE(sourcePolicyViolations(
                 QByteArrayLiteral("Loader { sourceComponent: safeStaticComponent }")),
             QStringList());
}

void PilotRoutesTest::packageManifestDeclaresWorkerInventory()
{
    const QString path = QDir(QStringLiteral(Q_BROWSER_SOURCE_DIR))
                             .filePath(QStringLiteral("packages/pilot/manifest.json"));
    QFile file(path);
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(path));
    const ManifestParseResult parsed = Manifest::parse(file.readAll());
    QVERIFY(parsed.hasValue());
    QCOMPARE(parsed.value().routes(),
             QStringList({QStringLiteral("/login"),
                          QStringLiteral("/dashboard"),
                          QStringLiteral("/orders"),
                          QStringLiteral("/orders/:id"),
                          QStringLiteral("/orders/:id/edit"),
                          QStringLiteral("/customers"),
                          QStringLiteral("/customers/:id"),
                          QStringLiteral("/files"),
                          QStringLiteral("/settings")}));
    QCOMPARE(parsed.value().permissions().network.methods,
             QStringList({QStringLiteral("GET"), QStringLiteral("POST"), QStringLiteral("PATCH")}));
    const auto patch = parseHttpMethod(QStringLiteral("PATCH"));
    QVERIFY(patch.has_value());
    QCOMPARE(httpMethodName(*patch), QStringLiteral("PATCH"));
}

void PilotRoutesTest::sourcePolicyForbidsRawNetworkFilesystemAndNativeCode()
{
    const QString qmlRoot = QDir(QStringLiteral(Q_BROWSER_SOURCE_DIR))
                                .filePath(QStringLiteral("packages/pilot/qml"));
    const QStringList requiredPages{
        QStringLiteral("LoginPage.qml"), QStringLiteral("DashboardPage.qml"),
        QStringLiteral("OrdersPage.qml"), QStringLiteral("OrderDetailPage.qml"),
        QStringLiteral("OrderEditPage.qml"), QStringLiteral("CustomersPage.qml"),
        QStringLiteral("CustomerDetailPage.qml"), QStringLiteral("FilesPage.qml"),
        QStringLiteral("SettingsPage.qml")};
    for (const QString &page : requiredPages) {
        QVERIFY2(QFileInfo(QDir(qmlRoot).filePath(QStringLiteral("pages/") + page)).isFile(),
                 qPrintable(page));
    }
    int runtimeInvocations = 0;
    QDirIterator sources(qmlRoot, {QStringLiteral("*.qml"), QStringLiteral("*.js")},
                         QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (sources.hasNext()) {
        QFile source(sources.next());
        QVERIFY(source.open(QIODevice::ReadOnly));
        const QByteArray contents = source.readAll();
        const QStringList violations = sourcePolicyViolations(contents);
        QVERIFY2(violations.isEmpty(),
                 qPrintable(source.fileName() + QStringLiteral(": ")
                            + violations.join(QStringLiteral(", "))));
        qsizetype offset = 0;
        while ((offset = contents.indexOf("runtime.invoke", offset)) >= 0) {
            ++runtimeInvocations;
            offset += 14;
            QVERIFY2(source.fileName().endsWith(QStringLiteral("RuntimeModels.qml")),
                     qPrintable(source.fileName()));
        }
    }
    QVERIFY(runtimeInvocations > 0);
}

void PilotRoutesTest::realWorkerLoadsPilotAndEmitsTypedCapability()
{
    const QString package = QDir(QStringLiteral(Q_BROWSER_SOURCE_DIR))
                                .filePath(QStringLiteral("packages/pilot"));
    WorkerTestEnvironment environment({}, package);
    QVERIFY2(environment.isValid(), qPrintable(environment.error()));
    auto launch = environment.launch(QStringLiteral("pilot-worker-nonce"),
                                     QStringLiteral("pilot-worker-nonce"), 100,
                                     QStringLiteral("http://127.0.0.1:49321/"));
    QVERIFY2(launch.has_value(), qPrintable(environment.error()));
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Handshake).status,
             SessionStatus::MessageReady);
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::SurfaceReady).status,
             SessionStatus::MessageReady);
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Ready).status,
             SessionStatus::MessageReady);
    QVERIFY(launch->hostSession.sendRouteLoad(QStringLiteral("route-dashboard"),
                                              QStringLiteral("/dashboard"), 5000));
    const SessionReceiveResult request = receiveUntil(launch->hostSession,
                                                       ProtocolType::Request, 10000);
    QCOMPARE(request.status, SessionStatus::MessageReady);
    QVERIFY(request.message.has_value());
    QCOMPARE(request.message->payload().value(QStringLiteral("capability")).toString(),
             QStringLiteral("network"));
    QCOMPARE(request.message->payload().value(QStringLiteral("operation")).toString(),
             QStringLiteral("request"));
    const QJsonObject payload = request.message->payload().value(
        QStringLiteral("payload")).toObject();
    QCOMPARE(payload.value(QStringLiteral("method")).toString(), QStringLiteral("GET"));
    QCOMPARE(payload.value(QStringLiteral("url")).toString(),
             QStringLiteral("http://127.0.0.1:49321/api/dashboard"));
    const auto completion = ProtocolMessage::errorResponse(
        request.message->requestId(), QStringLiteral("network.timeout"),
        QStringLiteral("Network request timed out."));
    QVERIFY(completion.has_value());
    QVERIFY(launch->hostSession.send(*completion, 5000));
    const auto shutdown = ProtocolMessage::shutdown(QStringLiteral("pilot.test.complete"));
    QVERIFY(shutdown.has_value());
    QVERIFY(launch->hostSession.send(*shutdown, 5000));
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Shutdown, 5000).status,
             SessionStatus::MessageReady);
    QVERIFY(launch->process.waitForFinished(5000));
    const auto closed = launch->process.close();
    QVERIFY2(closed.value.has_value(), qPrintable(closed.errorCode));

    auto invalidOrigin = environment.launch(
        QStringLiteral("pilot-invalid-origin"), QStringLiteral("pilot-invalid-origin"), 100,
        QStringLiteral("http://127.0.0.1:49321/api"));
    QVERIFY2(invalidOrigin.has_value(), qPrintable(environment.error()));
    const SessionReceiveResult rejected = invalidOrigin->hostSession.receive(5000);
    QVERIFY(rejected.status == SessionStatus::PeerClosed
            || rejected.status == SessionStatus::Failed);
    QVERIFY(invalidOrigin->process.waitForFinished(5000));
    QVERIFY(invalidOrigin->process.exitCode() != DWORD(0));
    const auto invalidClosed = invalidOrigin->process.close();
    QVERIFY2(invalidClosed.value.has_value(), qPrintable(invalidClosed.errorCode));
}

void PilotRoutesTest::hostInventoryResolvesPatternToPageAndEngine()
{
    const auto routes = createPilotRouteRegistry(QUrl(QStringLiteral("http://127.0.0.1:4180/")));
    QVERIFY(routes.has_value());
    const struct Expected {
        QString path;
        Engine engine;
        QString page;
    } expected[] = {
        {QStringLiteral("/login"), Engine::QmlWorker, QStringLiteral("qml/Main.qml")},
        {QStringLiteral("/dashboard"), Engine::QmlWorker, QStringLiteral("qml/Main.qml")},
        {QStringLiteral("/orders"), Engine::QmlWorker, QStringLiteral("qml/Main.qml")},
        {QStringLiteral("/orders/ORD-0001"), Engine::QmlWorker, QStringLiteral("qml/Main.qml")},
        {QStringLiteral("/orders/ORD-0001/edit"), Engine::QmlWorker, QStringLiteral("qml/Main.qml")},
        {QStringLiteral("/customers"), Engine::QmlWorker, QStringLiteral("qml/Main.qml")},
        {QStringLiteral("/customers/CUS-001"), Engine::QmlWorker, QStringLiteral("qml/Main.qml")},
        {QStringLiteral("/files"), Engine::QmlWorker, QStringLiteral("qml/Main.qml")},
        {QStringLiteral("/settings"), Engine::QmlWorker, QStringLiteral("qml/Main.qml")},
        {QStringLiteral("/web/help"), Engine::WebEngine,
         QStringLiteral("http://127.0.0.1:4180/help")},
    };
    for (const Expected &route : expected) {
        const RouteMatch match = routes->match(route.path);
        QVERIFY2(match.isValid(), qPrintable(route.path));
        QCOMPARE(match.record.engine, route.engine);
        QCOMPARE(match.record.entryPoint, route.page);
    }
    QCOMPARE(routes->match(QStringLiteral("/orders/ORD-0001/edit")).parameters.value(
                 QStringLiteral("id")),
             QStringLiteral("ORD-0001"));
}

QTEST_MAIN(PilotRoutesTest)
#include "tst_pilot_routes.moc"
