#include "PendingCapabilityQueue.h"
#include "RuntimeFacade.h"
#include "WorkerTestEnvironment.h"
#include "WorkerWindow.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QSet>
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
    void rejectsEntryOutsideVerifiedPackage();
    void rejectsPollutedImportAndNativePlugin();
    void loadingFacadeCallsAndRawNetworkAreBrokeredOrDenied();
    void loadingQueueOverflowFailsClosedBeforeReady();
};

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

void WorkerConfinementTest::loadingFacadeCallsAndRawNetworkAreBrokeredOrDenied()
{
    const QByteArray qml = QByteArrayLiteral(R"QML(import QtQuick
Rectangle {
    width: 320; height: 200
    property bool xhrReported: false
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
        Runtime.invoke("storage", "get", { kind: "startup", identity: Runtime.appIdentity })
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
                                     QStringLiteral("confine-nonce"), 1000);
    QVERIFY2(launch.has_value(), qPrintable(environment.error()));
    QCOMPARE(launch->hostSession.receive(15000).message->type(), ProtocolType::Handshake);
    QCOMPARE(launch->hostSession.receive(15000).message->type(), ProtocolType::SurfaceReady);
    QCOMPARE(launch->hostSession.receive(15000).message->type(), ProtocolType::Ready);

    QSet<QString> kinds;
    for (int index = 0; index < 4; ++index) {
        auto request = launch->hostSession.receive(15000);
        while (request.status == SessionStatus::MessageReady
               && request.message->type() == ProtocolType::Heartbeat) {
            request = launch->hostSession.receive(15000);
        }
        QCOMPARE(request.status, SessionStatus::MessageReady);
        QCOMPARE(request.message->type(), ProtocolType::Request);
        QCOMPARE(request.message->payload().value(QStringLiteral("capability")).toString(),
                 QStringLiteral("storage"));
        const QJsonObject body = request.message->payload()
                                     .value(QStringLiteral("payload")).toObject();
        QCOMPARE(body.value(QStringLiteral("identity")).toString(), environment.appId());
        kinds.insert(body.value(QStringLiteral("kind")).toString());
        const auto response = ProtocolMessage::successResponse(request.message->requestId(), {});
        QVERIFY(response.has_value());
        QVERIFY(launch->hostSession.send(*response, 5000));
    }
    QCOMPARE(kinds, QSet<QString>({QStringLiteral("startup"), QStringLiteral("xhrDenied"),
                                  QStringLiteral("imageDenied"), QStringLiteral("loaderDenied")}));
    QVERIFY(!launch->process.waitForFinished(100));
    QVERIFY(launch->hostSession.send(*ProtocolMessage::shutdown(QStringLiteral("test.done")),
                                    5000));
    QCOMPARE(launch->hostSession.receive(5000).message->type(), ProtocolType::Shutdown);
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
    const auto handshake = launch->hostSession.receive(15000);
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
