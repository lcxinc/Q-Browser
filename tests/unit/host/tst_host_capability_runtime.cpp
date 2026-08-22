#include "HostCapabilityRuntime.h"
#include "WorkerRetirementManager.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonObject>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <utility>

namespace {
ManifestPermissions networkPermission(const quint16 port)
{
    Q_UNUSED(port)
    ManifestPermissions permissions;
    permissions.network.hosts = {QStringLiteral("127.0.0.1")};
    permissions.network.methods = {QStringLiteral("GET")};
    return permissions;
}

QJsonObject requestPayload(const QUrl &url)
{
    return {{QStringLiteral("method"), QStringLiteral("GET")},
            {QStringLiteral("url"), url.toString(QUrl::FullyEncoded)},
            {QStringLiteral("bodyBase64"), QString{}}};
}
}

class HostCapabilityRuntimeTest final : public QObject
{
    Q_OBJECT

private slots:
    void rejectsNonCanonicalMockOrigins_data();
    void rejectsNonCanonicalMockOrigins();
    void validatesTrustedWorkerGestureEvidence();
    void slowRequestRetiresWithoutBlockingOrLateDelivery();
};

void HostCapabilityRuntimeTest::rejectsNonCanonicalMockOrigins_data()
{
    QTest::addColumn<QUrl>("origin");
    QTest::newRow("https") << QUrl(QStringLiteral("https://127.0.0.1:8443/"));
    QTest::newRow("hostname") << QUrl(QStringLiteral("http://localhost:8080/"));
    QTest::newRow("missing-port") << QUrl(QStringLiteral("http://127.0.0.1/"));
    QTest::newRow("path") << QUrl(QStringLiteral("http://127.0.0.1:8080/api"));
    QTest::newRow("query") << QUrl(QStringLiteral("http://127.0.0.1:8080/?x=1"));
    QTest::newRow("fragment") << QUrl(QStringLiteral("http://127.0.0.1:8080/#x"));
    QTest::newRow("userinfo") << QUrl(QStringLiteral("http://user@127.0.0.1:8080/"));
}

void HostCapabilityRuntimeTest::rejectsNonCanonicalMockOrigins()
{
    QFETCH(QUrl, origin);
    QString error;
    const auto runtime = HostCapabilityRuntime::create(
        QStringLiteral("com.qbrowser.test"), {}, origin,
        QStringLiteral("L:/not-opened-for-invalid-config"), 0, 0, 0, &error);
    QVERIFY(runtime == nullptr);
    QCOMPARE(error, QStringLiteral("host.capability.invalid_configuration"));
}

void HostCapabilityRuntimeTest::validatesTrustedWorkerGestureEvidence()
{
    HostWorkerGestureEvidence valid;
    valid.now = 5'000;
    valid.lastInput = 4'500;
    valid.workerProcessId = 42;
    valid.focusProcessId = 42;
    valid.foregroundMatchesHostRoot = true;
    valid.focusBelongsToWorkerWindow = true;
    QVERIFY(qbrowser_host_testing::isTrustedWorkerGesture(valid));

    HostWorkerGestureEvidence noGesture = valid;
    noGesture.lastInput = 0;
    QVERIFY(!qbrowser_host_testing::isTrustedWorkerGesture(noGesture));
    HostWorkerGestureEvidence expired = valid;
    expired.lastInput = 3'999;
    QVERIFY(!qbrowser_host_testing::isTrustedWorkerGesture(expired));
    HostWorkerGestureEvidence replay = valid;
    replay.lastGrantedInput = replay.lastInput;
    QVERIFY(!qbrowser_host_testing::isTrustedWorkerGesture(replay));
    HostWorkerGestureEvidence crossApp = valid;
    crossApp.focusProcessId = 43;
    QVERIFY(!qbrowser_host_testing::isTrustedWorkerGesture(crossApp));
    HostWorkerGestureEvidence crossWindow = valid;
    crossWindow.focusBelongsToWorkerWindow = false;
    QVERIFY(!qbrowser_host_testing::isTrustedWorkerGesture(crossWindow));
}

void HostCapabilityRuntimeTest::slowRequestRetiresWithoutBlockingOrLateDelivery()
{
    QTcpServer silentServer;
    QVERIFY(silentServer.listen(QHostAddress::LocalHost));
    const QUrl origin(QStringLiteral("http://127.0.0.1:%1/")
                          .arg(silentServer.serverPort()));
    QTemporaryDir storage;
    QVERIFY(storage.isValid());
    QString error;
    auto runtime = HostCapabilityRuntime::create(
        QStringLiteral("com.qbrowser.test"),
        networkPermission(silentServer.serverPort()), origin, storage.path(),
        0, 0, 0, &error);
    QVERIFY2(runtime != nullptr, qPrintable(error));
    int delivered = 0;
    connect(runtime.get(), &HostCapabilityRuntime::completed,
            this, [&delivered] { ++delivered; });
    runtime->dispatch(
        7, QStringLiteral("slow-request"), QStringLiteral("network"),
        QStringLiteral("request"),
        requestPayload(QUrl(origin.toString() + QStringLiteral("api/dashboard"))));

    QElapsedTimer destruction;
    destruction.start();
    HostCapabilityRuntime::retire(std::exchange(runtime, {}));
    QVERIFY2(destruction.elapsed() < 100,
             qPrintable(QStringLiteral("destruction blocked %1 ms")
                            .arg(destruction.elapsed())));
    QVERIFY(WorkerRetirementManager::instance().flush(10'000));
    QVERIFY(WorkerRetirementManager::instance().status().isIdle());
    QCoreApplication::processEvents();
    QCOMPARE(delivered, 0);
}

QTEST_MAIN(HostCapabilityRuntimeTest)

#include "tst_host_capability_runtime.moc"
