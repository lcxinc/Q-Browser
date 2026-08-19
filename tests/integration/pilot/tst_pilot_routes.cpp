#include "Manifest.h"
#include "PilotRoutes.h"
#include "HostPolicy.h"
#include "RouteRegistry.h"
#include "ProtocolMessage.h"
#include "QmlSourcePolicy.h"
#include "WorkerTestEnvironment.h"

#include <QDir>
#include <QFile>
#include <QDirIterator>
#include <QJsonDocument>
#include <QSet>
#include <QTest>

class PilotRoutesTest final : public QObject
{
    Q_OBJECT

private slots:
    void packageManifestDeclaresWorkerInventory();
    void manifestDeclarationsMatchDirectSourceUsage();
    void hostInventoryResolvesPatternToPageAndEngine();
    void realWorkerLoadsPilotAndEmitsTypedCapability();
    void sourcePolicyForbidsRawNetworkFilesystemAndNativeCode();
    void sourcePolicyRejectsDynamicQmlConstructionAndLoading();
};

void PilotRoutesTest::manifestDeclarationsMatchDirectSourceUsage()
{
    const QDir sourceRoot(QDir(QStringLiteral(Q_BROWSER_SOURCE_DIR))
                              .filePath(QStringLiteral("packages/pilot/qml")));
    QSet<QString> directImports;
    bool usesNetwork = false;
    QSet<QString> networkMethods;
    bool usesStorage = false;
    bool usesFile = false;
    QDirIterator sources(sourceRoot.path(), {QStringLiteral("*.qml"), QStringLiteral("*.js")},
                         QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (sources.hasNext()) {
        QFile source(sources.next());
        QVERIFY(source.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(source.readAll());
        for (const QString &line : text.split(u'\n')) {
            const QString trimmed = line.trimmed();
            if (!trimmed.startsWith(QStringLiteral("import "))) continue;
            const QString name = trimmed.sliced(7).section(u' ', 0, 0);
            if (!name.startsWith(u'\"') && !name.startsWith(u'\'')) directImports.insert(name);
        }
        usesNetwork = usesNetwork || text.contains(QStringLiteral("network(\""));
        for (const QString &method : {QStringLiteral("GET"), QStringLiteral("POST"),
                                      QStringLiteral("PATCH")}) {
            if (text.contains(u'"' + method + u'"')) networkMethods.insert(method);
        }
        usesStorage = usesStorage || text.contains(QStringLiteral("\"storage\""));
        usesFile = usesFile || text.contains(QStringLiteral("\"file\", \"open\""));
    }
    const QString manifestPath = QDir(QStringLiteral(Q_BROWSER_SOURCE_DIR))
                                     .filePath(QStringLiteral("packages/pilot/manifest.json"));
    QFile manifestFile(manifestPath);
    QVERIFY(manifestFile.open(QIODevice::ReadOnly));
    const ManifestParseResult parsed = Manifest::parse(manifestFile.readAll());
    QVERIFY(parsed.hasValue());
    QCOMPARE(directImports, QSet<QString>(parsed.value().imports().cbegin(),
                                          parsed.value().imports().cend()));
    QCOMPARE(!parsed.value().permissions().network.hosts.isEmpty(), usesNetwork);
    QCOMPARE(networkMethods,
             QSet<QString>(parsed.value().permissions().network.methods.cbegin(),
                           parsed.value().permissions().network.methods.cend()));
    QCOMPARE(parsed.value().permissions().storage == StoragePermission::AppPrivate, usesStorage);
    QCOMPARE(parsed.value().permissions().fileOpen == FileOpenPermission::UserBrokered, usesFile);
    QVERIFY(!parsed.value().permissions().clipboardWrite);
    QCOMPARE(parsed.value().permissions().clipboardRead, ClipboardReadPermission::Disabled);
}

void PilotRoutesTest::sourcePolicyRejectsDynamicQmlConstructionAndLoading()
{
    const QString path = QDir(QStringLiteral(Q_BROWSER_SOURCE_DIR))
                             .filePath(QStringLiteral("tests/fixtures/pilot/source-policy-attack.qml"));
    QFile fixture(path);
    QVERIFY2(fixture.open(QIODevice::ReadOnly), qPrintable(path));
    const QStringList violations = QmlSourcePolicy::violations(fixture.readAll());
    for (const QString &expected : {QStringLiteral("dynamic-import"),
                                    QStringLiteral("dynamic-loader-source"),
                                    QStringLiteral("loader-set-source"),
                                    QStringLiteral("qml-create-component"),
                                    QStringLiteral("qml-create-object"),
                                    QStringLiteral("qt-include"),
                                    QStringLiteral("remote-import")}) {
        QVERIFY2(violations.contains(expected), qPrintable(expected));
    }

    QCOMPARE(QmlSourcePolicy::violations(
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
    QCOMPARE(parsed.value().imports(),
             QStringList({QStringLiteral("QtQuick"),
                          QStringLiteral("QtQuick.Layouts"),
                          QStringLiteral("Company.Design")}));
    QCOMPARE(parsed.value().permissions().clipboardRead,
             ClipboardReadPermission::Disabled);
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
        const QStringList violations = QmlSourcePolicy::violations(contents);
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
