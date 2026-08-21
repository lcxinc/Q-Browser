#include "CapabilityBroker.h"
#include "QmlSourcePolicy.h"
#include "WorkerTestEnvironment.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

namespace {
class RecordingService final : public CapabilityService
{
public:
    BrokerResult invoke(const QString &,
                        const QJsonObject &,
                        const HostRequestContext &) override
    {
        ++invocations;
        return BrokerResult::success();
    }

    int invocations = 0;
};
}

class CapabilityEscapeTest final : public QObject
{
    Q_OBJECT
private slots:
    void undeclaredCapabilitiesNeverReachServices();
    void directNetworkFileAndProcessApisAreDenied();
    void realWorkerCannotReachFilesOrProcessApis();
};

void CapabilityEscapeTest::undeclaredCapabilitiesNeverReachServices()
{
    RecordingService service;
    CapabilityBroker broker(EffectivePolicy{},
                            CapabilityServices{&service, &service, &service, &service});
    const HostRequestContext context{QStringLiteral("company.security"),
                                     QStringLiteral("escape-1")};
    const struct Attempt {
        QString capability;
        QString operation;
    } attempts[]{
        {QStringLiteral("network"), QStringLiteral("request")},
        {QStringLiteral("storage"), QStringLiteral("get")},
        {QStringLiteral("clipboard"), QStringLiteral("read")},
        {QStringLiteral("file"), QStringLiteral("open")},
        {QStringLiteral("process"), QStringLiteral("open")},
    };
    for (const Attempt &attempt : attempts) {
        const BrokerResult result = broker.dispatch(
            attempt.capability, attempt.operation, {}, context);
        QVERIFY2(!result.ok, qPrintable(attempt.capability));
        QCOMPARE(result.errorCode, QStringLiteral("capability.denied"));
    }
    QCOMPARE(service.invocations, 0);
}

void CapabilityEscapeTest::directNetworkFileAndProcessApisAreDenied()
{
    const struct Attack {
        QByteArray source;
        QString violation;
    } attacks[] = {
        {QByteArrayLiteral("import QtQuick\nItem { Component.onCompleted: { var x = new XMLHttpRequest() } }"),
         QStringLiteral("raw-network")},
        {QByteArrayLiteral("import QtQuick\nItem { Loader { source: 'file:///C:/Windows/win.ini' } }"),
         QStringLiteral("unsafe-url-source")},
        {QByteArrayLiteral("import QtQuick\nimport Qt.labs.platform\nItem { FileDialog {} }"),
         QStringLiteral("native-file-dialog")},
        {QByteArrayLiteral("import QtQuick\nItem { Component.onCompleted: Qt.openUrlExternally('file:///C:/Windows/System32/cmd.exe') }"),
         QStringLiteral("external-url")},
    };
    for (const Attack &attack : attacks) {
        const QStringList violations = QmlSourcePolicy::violations(attack.source);
        QVERIFY2(violations.contains(attack.violation), attack.source.constData());
    }

    QTcpServer reachableServer;
    QVERIFY(reachableServer.listen(QHostAddress::LocalHost));
    int connectionAttempts = 0;
    connect(&reachableServer, &QTcpServer::newConnection, &reachableServer, [&] {
        while (QTcpSocket *socket = reachableServer.nextPendingConnection()) {
            ++connectionAttempts;
            socket->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                          "Connection: close\r\n\r\nok");
            socket->disconnectFromHost();
        }
    });
    QByteArray realWorkerAttack = QByteArrayLiteral(R"QML(import QtQuick
import QtQml
Item {
    property bool reported: false
    property var rawRequest: {
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (!reported && xhr.readyState === XMLHttpRequest.DONE) {
                reported = true
                Runtime.invoke("storage", "put", { directNetworkStatus: xhr.status })
            }
        }
        xhr.open("GET", "http://127.0.0.1:__PORT__/escape")
        xhr.send()
        return xhr
    }
}
)QML");
    realWorkerAttack.replace("__PORT__",
                             QByteArray::number(reachableServer.serverPort()));
    WorkerTestEnvironment environment(realWorkerAttack);
    QVERIFY2(environment.isValid(), qPrintable(environment.error()));
    auto launch = environment.launch(QStringLiteral("escape-nonce"),
                                     QStringLiteral("escape-nonce"), 1000);
    QVERIFY2(launch.has_value(), qPrintable(environment.error()));
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Handshake).status,
             SessionStatus::MessageReady);
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::SurfaceReady).status,
             SessionStatus::MessageReady);
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Ready).status,
             SessionStatus::MessageReady);
    const SessionReceiveResult request = receiveUntil(
        launch->hostSession, ProtocolType::Request, 10'000);
    QCOMPARE(request.status, SessionStatus::MessageReady);
    QCOMPARE(request.message->payload().value(QStringLiteral("capability")).toString(),
             QStringLiteral("storage"));
    QCOMPARE(request.message->payload().value(QStringLiteral("payload")).toObject()
                 .value(QStringLiteral("directNetworkStatus")).toInt(), 0);
    QTest::qWait(200);
    QCOMPARE(connectionAttempts, 0);
    const auto shutdown = ProtocolMessage::shutdown(QStringLiteral("security.complete"));
    QVERIFY(shutdown.has_value());
    QVERIFY(launch->hostSession.send(*shutdown, 5000));
    QVERIFY(launch->process.waitForFinished(5000));
    const auto closed = launch->process.close();
    QVERIFY2(closed.value.has_value(), qPrintable(closed.errorCode));
}

void CapabilityEscapeTest::realWorkerCannotReachFilesOrProcessApis()
{
    QTemporaryDir forbidden;
    QVERIFY(forbidden.isValid());
    const QString forbiddenQml = forbidden.filePath(QStringLiteral("Outside.qml"));
    QFile file(forbiddenQml);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(file.write("import QtQuick\nItem {}"), qint64(22));
    file.close();

    QByteArray source = QByteArrayLiteral(R"QML(import QtQuick
Item {
    Loader { id: outside; source: "__FILE_URL__" }
    Timer {
        interval: 500
        running: true
        repeat: false
        onTriggered: Runtime.invoke("storage", "put", {
            fileLoaderStatus: outside.status,
            qfileType: typeof QFile,
            processType: typeof Process
        })
    }
}
)QML");
    source.replace("__FILE_URL__", QUrl::fromLocalFile(forbiddenQml).toEncoded());
    WorkerTestEnvironment environment(source);
    QVERIFY2(environment.isValid(), qPrintable(environment.error()));
    auto launch = environment.launch(QStringLiteral("file-process-nonce"),
                                     QStringLiteral("file-process-nonce"), 1000);
    QVERIFY2(launch.has_value(), qPrintable(environment.error()));
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Handshake).status,
             SessionStatus::MessageReady);
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::SurfaceReady).status,
             SessionStatus::MessageReady);
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Ready).status,
             SessionStatus::MessageReady);
    const SessionReceiveResult request = receiveUntil(
        launch->hostSession, ProtocolType::Request, 10'000);
    QCOMPARE(request.status, SessionStatus::MessageReady);
    const QJsonObject payload = request.message->payload()
                                    .value(QStringLiteral("payload")).toObject();
    QCOMPARE(payload.value(QStringLiteral("fileLoaderStatus")).toInt(), 3);
    QCOMPARE(payload.value(QStringLiteral("qfileType")).toString(),
             QStringLiteral("undefined"));
    QCOMPARE(payload.value(QStringLiteral("processType")).toString(),
             QStringLiteral("undefined"));
    const auto shutdown = ProtocolMessage::shutdown(QStringLiteral("security.complete"));
    QVERIFY(shutdown.has_value());
    QVERIFY(launch->hostSession.send(*shutdown, 5000));
    QVERIFY(launch->process.waitForFinished(5000));
    const auto closed = launch->process.close();
    QVERIFY2(closed.value.has_value(), qPrintable(closed.errorCode));
}

QTEST_MAIN(CapabilityEscapeTest)
#include "tst_capability_escape.moc"
