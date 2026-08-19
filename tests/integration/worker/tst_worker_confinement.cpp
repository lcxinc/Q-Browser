#include "PendingCapabilityQueue.h"
#include "RuntimeFacade.h"
#include "WorkerTestEnvironment.h"
#include "WorkerNetworkAccess.h"
#include "WorkerWindow.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>

namespace {

bool writeNewFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        && file.write(contents) == contents.size();
}

class EnvironmentGuard final
{
public:
    EnvironmentGuard(const char *name, const QByteArray &value)
        : name_(name), old_(qgetenv(name)), had_(qEnvironmentVariableIsSet(name))
    {
        qputenv(name, value);
    }
    ~EnvironmentGuard()
    {
        if (had_) qputenv(name_.constData(), old_);
        else qunsetenv(name_.constData());
    }
private:
    QByteArray name_;
    QByteArray old_;
    bool had_;
};

} // namespace

class WorkerConfinementTest final : public QObject
{
    Q_OBJECT
private slots:
    void pendingCapabilityQueueIsTypedBoundedAndFifo();
    void runtimeFacadeNavigationIsValidByConstructionAndBounded();
    void rejectsEntryOutsideVerifiedPackage();
    void rejectsPollutedImportAndNativePlugin();
    void loadsApprovedBuiltInDesignModuleFromWorkerClosure();
    void inProcessQmlNetworkIsDeniedBeforeSocketConnect();
    void loadingFacadeCallsAndRawNetworkAreBrokeredOrDenied();
    void loadingQueueOverflowFailsClosedBeforeReady();
};

void WorkerConfinementTest::runtimeFacadeNavigationIsValidByConstructionAndBounded()
{
    RuntimeFacade facade;
    QSignalSpy requested(&facade, &RuntimeFacade::navigationRequested);
    QSignalSpy finished(&facade, &RuntimeFacade::navigationFinished);
    QVERIFY(facade.navigate(QStringLiteral("https://evil.test/orders")).isEmpty());
    QVERIFY(facade.navigate(QStringLiteral("//other-app/orders")).isEmpty());
    QVERIFY(facade.navigate(QStringLiteral("/orders/../settings")).isEmpty());
    const QString requestId = facade.navigate(QStringLiteral("/orders/ORD-0001"));
    QVERIFY(!requestId.isEmpty());
    QCOMPARE(requested.count(), 1);
    QCOMPARE(requested.first().at(1).toString(), QStringLiteral("/orders/ORD-0001"));
    QVERIFY(facade.navigate(QStringLiteral("/settings")).isEmpty());
    facade.complete(requestId, QJsonObject{{QStringLiteral("ok"), true},
                                            {QStringLiteral("result"), QJsonObject{}}});
    QCOMPARE(finished.count(), 1);
    QVERIFY(!facade.navigate(QStringLiteral("/settings")).isEmpty());
}

void WorkerConfinementTest::pendingCapabilityQueueIsTypedBoundedAndFifo()
{
    PendingCapabilityQueue queue(2, 4096);
    QCOMPARE(queue.enqueue({QStringLiteral("one"), QStringLiteral("storage"),
                            QStringLiteral("get"), QJsonObject{}}),
             PendingCapabilityPushResult::Accepted);
    QCOMPARE(queue.enqueue({QStringLiteral("one"), QStringLiteral("storage"),
                            QStringLiteral("get"), QJsonObject{}}),
             PendingCapabilityPushResult::DuplicateRequestId);
    QCOMPARE(queue.enqueue({QStringLiteral("two"), QStringLiteral("network"),
                            QStringLiteral("fetch"), QJsonObject{}}),
             PendingCapabilityPushResult::Accepted);
    QCOMPARE(queue.enqueue({QStringLiteral("three"), QStringLiteral("storage"),
                            QStringLiteral("get"), QJsonObject{}}),
             PendingCapabilityPushResult::LimitExceeded);
    QCOMPARE(queue.enqueue({QStringLiteral("bad"), QStringLiteral("storage"),
                            QString(), QJsonObject{}}),
             PendingCapabilityPushResult::InvalidRequest);
    QCOMPARE(queue.takeNext()->requestId, QStringLiteral("one"));
    QCOMPARE(queue.takeNext()->requestId, QStringLiteral("two"));
    QVERIFY(!queue.takeNext().has_value());

    PendingCapabilityQueue byteBounded(2, 1);
    QCOMPARE(byteBounded.enqueue({QStringLiteral("one"), QStringLiteral("storage"),
                                  QStringLiteral("get"), QJsonObject{}}),
             PendingCapabilityPushResult::LimitExceeded);
}

void WorkerConfinementTest::rejectsEntryOutsideVerifiedPackage()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString package = QDir(root.path()).filePath(QStringLiteral("package"));
    QVERIFY(QDir().mkpath(QDir(package).filePath(QStringLiteral("qml"))));
    const QString outside = QDir(root.path()).filePath(QStringLiteral("Outside.qml"));
    QVERIFY(writeNewFile(outside, QByteArrayLiteral("import QtQml\nQtObject {}\n")));
    RuntimeFacade facade;
    WorkerWindow relative;
    QVERIFY(!relative.load(package, QStringLiteral("../Outside.qml"), &facade));
    WorkerWindow absolute;
    QVERIFY(!absolute.load(package, outside, &facade));
}

void WorkerConfinementTest::rejectsPollutedImportAndNativePlugin()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString package = QDir(root.path()).filePath(QStringLiteral("package"));
    const QString packageQml = QDir(package).filePath(QStringLiteral("qml"));
    const QString polluted = QDir(root.path()).filePath(QStringLiteral("polluted"));
    const QString evil = QDir(polluted).filePath(QStringLiteral("Unapproved"));
    QVERIFY(QDir().mkpath(packageQml));
    QVERIFY(QDir().mkpath(evil));
    QVERIFY(writeNewFile(QDir(evil).filePath(QStringLiteral("qmldir")),
                         QByteArrayLiteral("module Unapproved\nThing 1.0 Thing.qml\n")));
    QVERIFY(writeNewFile(QDir(evil).filePath(QStringLiteral("Thing.qml")),
                         QByteArrayLiteral("import QtQml\nQtObject {}\n")));
    QVERIFY(writeNewFile(QDir(packageQml).filePath(QStringLiteral("Main.qml")),
                         QByteArrayLiteral("import Unapproved 1.0\nThing {}\n")));
    EnvironmentGuard pollutedImports("QML2_IMPORT_PATH", polluted.toUtf8());
    RuntimeFacade facade;
    WorkerWindow importWindow;
    QVERIFY(!importWindow.load(package, QStringLiteral("qml/Main.qml"), &facade));

    QVERIFY(QFile::remove(QDir(packageQml).filePath(QStringLiteral("Main.qml"))));
    const QString nativeModule = QDir(packageQml).filePath(QStringLiteral("NativeModule"));
    QVERIFY(QDir().mkpath(nativeModule));
    QVERIFY(writeNewFile(QDir(nativeModule).filePath(QStringLiteral("qmldir")),
                         QByteArrayLiteral("module NativeModule\nplugin\tmalicious_native\n")));
    QVERIFY(writeNewFile(QDir(packageQml).filePath(QStringLiteral("Main.qml")),
                         QByteArrayLiteral("import QtQml\nimport NativeModule\nQtObject {}\n")));
    WorkerWindow pluginWindow;
    QVERIFY(!pluginWindow.load(package, QStringLiteral("qml/Main.qml"), &facade));
    QCOMPARE(pluginWindow.errorString(),
             QStringLiteral("worker.qml.native_plugin_forbidden"));
}

void WorkerConfinementTest::loadsApprovedBuiltInDesignModuleFromWorkerClosure()
{
    const QByteArray qml = QByteArrayLiteral(R"QML(import QtQuick
import Company.Design
Rectangle {
    width: 320
    height: 200
    color: Theme.background
    AppButton {
        text: "Design closure loaded"
    }
    Component.onCompleted: Runtime.invoke(
        "test", "observe", { kind: "designLoaded", darkSurface: Theme.surface.toString() })
}
)QML");
    WorkerTestEnvironment environment(qml);
    QVERIFY2(environment.isValid(), qPrintable(environment.error()));
    EnvironmentGuard pollutedImports("QML2_IMPORT_PATH",
                                       QByteArrayLiteral("Z:/untrusted/qml"));
    EnvironmentGuard pollutedImportsModern("QML_IMPORT_PATH",
                                             QByteArrayLiteral("Z:/untrusted/qml"));
    auto launch = environment.launch(QStringLiteral("design-nonce"),
                                     QStringLiteral("design-nonce"));
    QVERIFY2(launch.has_value(), qPrintable(environment.error()));
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Handshake).status,
             SessionStatus::MessageReady);
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::SurfaceReady).status,
             SessionStatus::MessageReady);
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Ready).status,
             SessionStatus::MessageReady);
    const auto request = receiveUntil(launch->hostSession, ProtocolType::Request);
    QCOMPARE(request.status, SessionStatus::MessageReady);
    QCOMPARE(request.message->payload().value(QStringLiteral("capability")).toString(),
             QStringLiteral("test"));
    const QJsonObject body = request.message->payload()
                                 .value(QStringLiteral("payload")).toObject();
    QCOMPARE(body.value(QStringLiteral("kind")).toString(),
             QStringLiteral("designLoaded"));
    QVERIFY(!body.value(QStringLiteral("darkSurface")).toString().isEmpty());
    QVERIFY(launch->hostSession.send(
        *ProtocolMessage::successResponse(request.message->requestId(), QJsonObject{}), 5000));
    QVERIFY(launch->hostSession.send(*ProtocolMessage::shutdown(QStringLiteral("test.done")),
                                    5000));
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Shutdown, 5000).status,
             SessionStatus::MessageReady);
    QVERIFY(launch->process.waitForFinished(5000));
    const auto closed = launch->process.close();
    QVERIFY2(closed.value.has_value(), qPrintable(closed.errorCode));
}

void WorkerConfinementTest::inProcessQmlNetworkIsDeniedBeforeSocketConnect()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    int acceptedConnections = 0;
    connect(&server, &QTcpServer::newConnection, this, [&] {
        ++acceptedConnections;
        while (server.hasPendingConnections()) {
            delete server.nextPendingConnection();
        }
    });
    const QString baseUrl = QStringLiteral("http://127.0.0.1:%1")
                                .arg(server.serverPort());

    WorkerNetworkAccessManagerFactory factory;
    std::unique_ptr<QNetworkAccessManager> manager(factory.create(nullptr));
    QNetworkReply *probe = manager->get(
        QNetworkRequest(QUrl(baseUrl + QStringLiteral("/probe"))));
    QSignalSpy probeFinished(probe, &QNetworkReply::finished);
    QVERIFY(probeFinished.wait(1000));
    QCOMPARE(probe->error(), QNetworkReply::ContentAccessDenied);

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString package = QDir(root.path()).filePath(QStringLiteral("package"));
    const QString qmlDirectory = QDir(package).filePath(QStringLiteral("qml"));
    QVERIFY(QDir().mkpath(qmlDirectory));
    const QString qml = QStringLiteral(R"QML(import QtQuick
import QtQml
Rectangle {
    width: 100; height: 100
    property bool xhrReported: false
    property var rawRequest: {
        let xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (!xhrReported && xhr.readyState === XMLHttpRequest.DONE) {
                xhrReported = true
                Runtime.invoke("test", "observe", { kind: "xhrDenied", status: xhr.status })
            }
        }
        xhr.open("GET", "%1/data.json")
        xhr.send()
        return xhr
    }
    Image {
        source: "%1/image.png"
        onStatusChanged: if (status === Image.Error)
            Runtime.invoke("test", "observe", { kind: "imageDenied" })
    }
    Loader {
        source: "%1/Raw.qml"
        onStatusChanged: if (status === Loader.Error)
            Runtime.invoke("test", "observe", { kind: "loaderDenied" })
    }
}
)QML").arg(baseUrl);
    QVERIFY(writeNewFile(QDir(qmlDirectory).filePath(QStringLiteral("Main.qml")),
                         qml.toUtf8()));
    RuntimeFacade facade;
    QSet<QString> deniedKinds;
    bool xhrStatusWasZero = false;
    connect(&facade, &RuntimeFacade::capabilityRequested, this,
            [&](const QString &, const QString &, const QString &,
                const QJsonObject &payload) {
                const QString kind = payload.value(QStringLiteral("kind")).toString();
                deniedKinds.insert(kind);
                if (kind == QStringLiteral("xhrDenied")) {
                    xhrStatusWasZero = payload.value(QStringLiteral("status")).toInt(-1) == 0;
                }
            });
    WorkerWindow window;
    QVERIFY2(window.load(package, QStringLiteral("qml/Main.qml"), &facade),
             qPrintable(window.errorString()));
    QTRY_COMPARE_WITH_TIMEOUT(deniedKinds.size(), 3, 5000);
    QCOMPARE(deniedKinds,
             QSet<QString>({QStringLiteral("xhrDenied"), QStringLiteral("imageDenied"),
                            QStringLiteral("loaderDenied")}));
    QVERIFY(xhrStatusWasZero);
    QTest::qWait(100);
    QCOMPARE(acceptedConnections, 0);
    QVERIFY(!server.hasPendingConnections());
}

void WorkerConfinementTest::loadingFacadeCallsAndRawNetworkAreBrokeredOrDenied()
{
    const QByteArray qml = QByteArrayLiteral(R"QML(import QtQuick
Rectangle {
    width: 320; height: 200
    property bool xhrReported: false
    property string startupRequestId: ""
    Connections {
        target: Runtime
        function onCapabilityFinished(requestId, response) {
            if (requestId === startupRequestId && response.ok === true
                    && response.result.token === "host-result") {
                Runtime.invoke("storage", "put",
                               { kind: "responseConfirmed",
                                 identity: Runtime.appIdentity,
                                 originalRequestId: requestId,
                                 token: response.result.token })
            }
        }
    }
    Image {
        source: "http://127.0.0.1:9/raw-image.png"
        onStatusChanged: if (status === Image.Error)
            Runtime.invoke("storage", "put", { kind: "imageDenied", identity: Runtime.appIdentity })
    }
    Loader {
        source: "http://127.0.0.1:9/Raw.qml"
        onStatusChanged: if (status === Loader.Error)
            Runtime.invoke("storage", "put", { kind: "loaderDenied", identity: Runtime.appIdentity })
    }
    Component.onCompleted: {
        startupRequestId = Runtime.invoke(
            "storage", "get", { kind: "startup", identity: Runtime.appIdentity })
        let xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (!xhrReported && xhr.readyState === XMLHttpRequest.DONE) {
                xhrReported = true
                Runtime.invoke("storage", "put", { kind: "xhrDenied", status: xhr.status,
                                                     identity: Runtime.appIdentity })
            }
        }
        xhr.open("GET", "http://127.0.0.1:9/raw.json")
        xhr.send()
    }
}
)QML");
    WorkerTestEnvironment environment(qml);
    QVERIFY2(environment.isValid(), qPrintable(environment.error()));
    auto launch = environment.launch(QStringLiteral("confine-nonce"),
                                     QStringLiteral("confine-nonce"), 20);
    QVERIFY2(launch.has_value(), qPrintable(environment.error()));
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Handshake).status,
             SessionStatus::MessageReady);
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::SurfaceReady).status,
             SessionStatus::MessageReady);
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Ready).status,
             SessionStatus::MessageReady);
    QTest::qWait(100);

    QSet<QString> deniedAndStartupKinds;
    QString startupRequestId;
    bool responseConfirmed = false;
    for (int index = 0; index < 5; ++index) {
        const auto request = receiveUntil(launch->hostSession,
                                          ProtocolType::Request);
        QCOMPARE(request.status, SessionStatus::MessageReady);
        QCOMPARE(request.message->type(), ProtocolType::Request);
        QCOMPARE(request.message->payload().value(QStringLiteral("capability")).toString(),
                 QStringLiteral("storage"));
        const QJsonObject body = request.message->payload()
                                     .value(QStringLiteral("payload")).toObject();
        QCOMPARE(body.value(QStringLiteral("identity")).toString(), environment.appId());
        const QString kind = body.value(QStringLiteral("kind")).toString();
        QJsonObject result;
        if (kind == QStringLiteral("startup")) {
            startupRequestId = request.message->requestId();
            result.insert(QStringLiteral("token"), QStringLiteral("host-result"));
            deniedAndStartupKinds.insert(kind);
        } else if (kind == QStringLiteral("responseConfirmed")) {
            responseConfirmed = true;
            QCOMPARE(body.value(QStringLiteral("originalRequestId")).toString(),
                     startupRequestId);
            QCOMPARE(body.value(QStringLiteral("token")).toString(),
                     QStringLiteral("host-result"));
        } else {
            deniedAndStartupKinds.insert(kind);
        }
        const auto response = ProtocolMessage::successResponse(
            request.message->requestId(), result);
        QVERIFY(response.has_value());
        QVERIFY(launch->hostSession.send(*response, 5000));
    }
    QCOMPARE(deniedAndStartupKinds,
             QSet<QString>({QStringLiteral("startup"), QStringLiteral("xhrDenied"),
                            QStringLiteral("imageDenied"), QStringLiteral("loaderDenied")}));
    QVERIFY(responseConfirmed);
    QVERIFY(!launch->process.waitForFinished(100));
    QVERIFY(launch->hostSession.send(*ProtocolMessage::shutdown(QStringLiteral("test.done")),
                                    5000));
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Shutdown, 5000).status,
             SessionStatus::MessageReady);
    QVERIFY(launch->process.waitForFinished(5000));
    const auto closed = launch->process.close();
    QVERIFY2(closed.value.has_value(), qPrintable(closed.errorCode));
}

void WorkerConfinementTest::loadingQueueOverflowFailsClosedBeforeReady()
{
    const QByteArray qml = QByteArrayLiteral(R"QML(import QtQuick
Rectangle {
    width: 100; height: 100
    Component.onCompleted: {
        for (let index = 0; index < 65; ++index)
            Runtime.invoke("storage", "get", { sequence: index })
    }
}
)QML");
    WorkerTestEnvironment environment(qml);
    QVERIFY2(environment.isValid(), qPrintable(environment.error()));
    auto launch = environment.launch(QStringLiteral("overflow-nonce"),
                                     QStringLiteral("overflow-nonce"), 1000);
    QVERIFY2(launch.has_value(), qPrintable(environment.error()));
    const auto handshake = receiveUntil(launch->hostSession, ProtocolType::Handshake);
    QCOMPARE(handshake.status, SessionStatus::MessageReady);
    QCOMPARE(handshake.message->type(), ProtocolType::Handshake);
    const auto afterHandshake = launch->hostSession.receive(15000);
    QVERIFY(afterHandshake.status == SessionStatus::PeerClosed
            || afterHandshake.status == SessionStatus::Failed);
    QVERIFY(launch->process.waitForFinished(5000));
    QVERIFY(launch->process.exitCode() != DWORD(0));
    const auto closed = launch->process.close();
    QVERIFY2(closed.value.has_value(), qPrintable(closed.errorCode));
}

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    WorkerConfinementTest test;
    if (argc == 1) {
        char outputOption[] = "-o";
        char outputTarget[] = "-,txt";
        char *arguments[] = {argv[0], outputOption, outputTarget};
        return QTest::qExec(&test, 3, arguments);
    }
    return QTest::qExec(&test, argc, argv);
}

#include "tst_worker_confinement.moc"
