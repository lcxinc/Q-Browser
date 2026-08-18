#include "WorkerTestEnvironment.h"
#include "WorkerApplication.h"

#include <QProcess>
#include <QTest>

class WorkerHandshakeTest final : public QObject
{
    Q_OBJECT
private slots:
    void directLaunchWithoutInheritedHandlesFailsClosed();
    void sandboxedWorkerCompletesLifecycle();
    void wrongNonceFailsClosed();
};

void WorkerHandshakeTest::directLaunchWithoutInheritedHandlesFailsClosed()
{
    QProcess worker;
    worker.start(QString::fromUtf8(Q_BROWSER_WORKER_PATH), {});
    QVERIFY(worker.waitForStarted(5000));
    QVERIFY(worker.waitForFinished(5000));
    QCOMPARE(worker.exitStatus(), QProcess::NormalExit);
    QCOMPARE(worker.exitCode(), WorkerApplication::invalidLaunchExitCode());
}

void WorkerHandshakeTest::sandboxedWorkerCompletesLifecycle()
{
    WorkerTestEnvironment environment;
    QVERIFY2(environment.isValid(), qPrintable(environment.error()));
    auto launch = environment.launch(QStringLiteral("launch-nonce"),
                                     QStringLiteral("launch-nonce"));
    QVERIFY2(launch.has_value(), qPrintable(environment.error()));
    const auto handshake = launch->hostSession.receive(15000);
    QCOMPARE(handshake.status, SessionStatus::MessageReady);
    QCOMPARE(handshake.message->type(), ProtocolType::Handshake);
    QVERIFY(launch->hostSession.isAuthenticated());
    const auto surface = launch->hostSession.receive(15000);
    QCOMPARE(surface.status, SessionStatus::MessageReady);
    QCOMPARE(surface.message->type(), ProtocolType::SurfaceReady);
    QVERIFY(!surface.message->payload().value(QStringLiteral("windowHandle")).toString().isEmpty());
    const auto ready = launch->hostSession.receive(15000);
    QCOMPARE(ready.status, SessionStatus::MessageReady);
    QCOMPARE(ready.message->type(), ProtocolType::Ready);
    QCOMPARE(launch->hostSession.appIdentity(), environment.appId());

    QVERIFY(launch->hostSession.sendRouteLoad(QStringLiteral("route-1"),
                                              QStringLiteral("/orders/42"), 5000));
    bool sawHeartbeat = false;
    auto routeAck = launch->hostSession.receive(5000);
    while (routeAck.status == SessionStatus::MessageReady
           && routeAck.message->type() == ProtocolType::Heartbeat) {
        sawHeartbeat = true;
        routeAck = launch->hostSession.receive(5000);
    }
    QCOMPARE(routeAck.status, SessionStatus::MessageReady);
    QCOMPARE(routeAck.message->type(), ProtocolType::Response);
    QCOMPARE(routeAck.message->requestId(), QStringLiteral("route-1"));
    QVERIFY(routeAck.message->payload().value(QStringLiteral("ok")).toBool());

    if (!sawHeartbeat) {
        const auto heartbeat = launch->hostSession.receive(5000);
        QCOMPARE(heartbeat.status, SessionStatus::MessageReady);
        QCOMPARE(heartbeat.message->type(), ProtocolType::Heartbeat);
    }
    QVERIFY(launch->hostSession.send(*ProtocolMessage::shutdown(QStringLiteral("host.close")),
                                    5000));
    const auto shutdown = launch->hostSession.receive(5000);
    QCOMPARE(shutdown.status, SessionStatus::MessageReady);
    QCOMPARE(shutdown.message->type(), ProtocolType::Shutdown);
    QVERIFY(launch->process.waitForFinished(5000));
    QCOMPARE(launch->process.exitCode(), DWORD(0));
    const auto closed = launch->process.close();
    QVERIFY2(closed.value.has_value(), qPrintable(closed.errorCode));
}

void WorkerHandshakeTest::wrongNonceFailsClosed()
{
    WorkerTestEnvironment environment;
    QVERIFY2(environment.isValid(), qPrintable(environment.error()));
    auto launch = environment.launch(QStringLiteral("wrong-nonce"),
                                     QStringLiteral("expected-nonce"));
    QVERIFY2(launch.has_value(), qPrintable(environment.error()));
    const auto result = launch->hostSession.receive(15000);
    QCOMPARE(result.status, SessionStatus::Failed);
    QCOMPARE(result.errorCode, QStringLiteral("ipc.session.nonce_mismatch"));
    QVERIFY(launch->process.waitForFinished(5000));
    QVERIFY(launch->process.exitCode() != DWORD(0));
    const auto closed = launch->process.close();
    QVERIFY2(closed.value.has_value(), qPrintable(closed.errorCode));
}

QTEST_GUILESS_MAIN(WorkerHandshakeTest)
#include "tst_worker_handshake.moc"
