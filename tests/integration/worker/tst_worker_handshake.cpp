#include "WorkerTestEnvironment.h"
#include "WorkerApplication.h"

#include <QProcess>
#include <QTest>

namespace {

bool authenticateRawPeer(IpcSession &host, WinPipeTransport &peer)
{
    const auto handshake = ProtocolMessage::handshake(QStringLiteral("flood-nonce"));
    return handshake.has_value()
        && peer.writeAll(FrameCodec::encode(handshake->toJson()), 1000)
        && receiveUntil(host, ProtocolType::Handshake, 1000).status
               == SessionStatus::MessageReady;
}

QByteArray heartbeatFrames(const int count)
{
    const QByteArray frame = FrameCodec::encode(ProtocolMessage::heartbeat().toJson());
    QByteArray frames;
    frames.reserve(frame.size() * count);
    for (int index = 0; index < count; ++index) frames.append(frame);
    return frames;
}

} // namespace

class WorkerHandshakeTest final : public QObject
{
    Q_OBJECT
private slots:
    void directLaunchWithoutInheritedHandlesFailsClosed();
    void sandboxedWorkerCompletesLifecycle();
    void sandboxedWorkerNavigationIsTrackedAndRouteLoadedByHost();
    void heartbeatInterleavingIsDispatched();
    void receiveUntilMissingTargetHonorsAbsoluteDeadline();
    void receiveUntilBoundsOverBudgetHeartbeatFlood();
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
    const auto handshake = receiveUntil(launch->hostSession, ProtocolType::Handshake);
    QCOMPARE(handshake.status, SessionStatus::MessageReady);
    QCOMPARE(handshake.message->type(), ProtocolType::Handshake);
    QVERIFY(launch->hostSession.isAuthenticated());
    const auto surface = receiveUntil(launch->hostSession, ProtocolType::SurfaceReady);
    QCOMPARE(surface.status, SessionStatus::MessageReady);
    QCOMPARE(surface.message->type(), ProtocolType::SurfaceReady);
    QVERIFY(!surface.message->payload().value(QStringLiteral("windowHandle")).toString().isEmpty());
    const auto ready = receiveUntil(launch->hostSession, ProtocolType::Ready);
    QCOMPARE(ready.status, SessionStatus::MessageReady);
    QCOMPARE(ready.message->type(), ProtocolType::Ready);
    QCOMPARE(launch->hostSession.appIdentity(), environment.appId());

    QVERIFY(launch->hostSession.sendRouteLoad(QStringLiteral("route-1"),
                                              QStringLiteral("/orders/42"), 5000));
    const auto routeAck = receiveUntil(launch->hostSession, ProtocolType::Response, 5000);
    QCOMPARE(routeAck.status, SessionStatus::MessageReady);
    QCOMPARE(routeAck.message->type(), ProtocolType::Response);
    QCOMPARE(routeAck.message->requestId(), QStringLiteral("route-1"));
    QVERIFY(routeAck.message->payload().value(QStringLiteral("ok")).toBool());

    QVERIFY(launch->hostSession.send(*ProtocolMessage::shutdown(QStringLiteral("host.close")),
                                    5000));
    const auto shutdown = receiveUntil(launch->hostSession, ProtocolType::Shutdown, 5000);
    QCOMPARE(shutdown.status, SessionStatus::MessageReady);
    QCOMPARE(shutdown.message->type(), ProtocolType::Shutdown);
    QVERIFY(launch->process.waitForFinished(5000));
    QCOMPARE(launch->process.exitCode(), DWORD(0));
    const auto closed = launch->process.close();
    QVERIFY2(closed.value.has_value(), qPrintable(closed.errorCode));
}

void WorkerHandshakeTest::sandboxedWorkerNavigationIsTrackedAndRouteLoadedByHost()
{
    const QByteArray qml = QByteArrayLiteral(R"QML(import QtQuick
Rectangle {
    width: 320; height: 200
    Timer {
        interval: 200; running: true; repeat: false
        onTriggered: Runtime.navigate("/orders")
    }
})QML");
    WorkerTestEnvironment environment(qml);
    QVERIFY2(environment.isValid(), qPrintable(environment.error()));
    auto launch = environment.launch(QStringLiteral("navigation-nonce"),
                                     QStringLiteral("navigation-nonce"), 100);
    QVERIFY2(launch.has_value(), qPrintable(environment.error()));
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Handshake).status,
             SessionStatus::MessageReady);
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::SurfaceReady).status,
             SessionStatus::MessageReady);
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Ready).status,
             SessionStatus::MessageReady);
    const SessionReceiveResult request = receiveUntil(
        launch->hostSession, ProtocolType::NavigationRequest, 5000);
    QCOMPARE(request.status, SessionStatus::MessageReady);
    QCOMPARE(request.message->payload().value(QStringLiteral("route")).toString(),
             QStringLiteral("/orders"));
    QVERIFY(launch->hostSession.send(*ProtocolMessage::successResponse(
        request.message->requestId(), QJsonObject{}), 5000));
    QVERIFY(launch->hostSession.sendRouteLoad(QStringLiteral("route-navigation"),
                                              QStringLiteral("/orders"), 5000));
    const SessionReceiveResult routeAck = receiveUntil(
        launch->hostSession, ProtocolType::Response, 5000);
    QCOMPARE(routeAck.status, SessionStatus::MessageReady);
    QCOMPARE(routeAck.message->requestId(), QStringLiteral("route-navigation"));
    QVERIFY(launch->hostSession.send(*ProtocolMessage::shutdown(
        QStringLiteral("navigation.complete")), 5000));
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Shutdown, 5000).status,
             SessionStatus::MessageReady);
    QVERIFY(launch->process.waitForFinished(5000));
    const auto closed = launch->process.close();
    QVERIFY2(closed.value.has_value(), qPrintable(closed.errorCode));
}

void WorkerHandshakeTest::heartbeatInterleavingIsDispatched()
{
    WorkerTestEnvironment environment;
    QVERIFY2(environment.isValid(), qPrintable(environment.error()));
    auto launch = environment.launch(QStringLiteral("dispatch-nonce"),
                                     QStringLiteral("dispatch-nonce"), 20);
    QVERIFY2(launch.has_value(), qPrintable(environment.error()));
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Handshake).status,
             SessionStatus::MessageReady);
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::SurfaceReady).status,
             SessionStatus::MessageReady);
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Ready).status,
             SessionStatus::MessageReady);

    for (int index = 0; index < 10; ++index) {
        QTest::qWait(30);
        const QString requestId = QStringLiteral("route-%1").arg(index);
        QVERIFY(launch->hostSession.sendRouteLoad(
            requestId, QStringLiteral("/stress/%1").arg(index), 5000));
        const auto response = receiveUntil(launch->hostSession,
                                           ProtocolType::Response, 5000);
        QCOMPARE(response.status, SessionStatus::MessageReady);
        QCOMPARE(response.message->requestId(), requestId);
    }
    QTest::qWait(100);
    QVERIFY(launch->hostSession.send(
        *ProtocolMessage::shutdown(QStringLiteral("stress.done")), 5000));
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Shutdown, 5000).status,
             SessionStatus::MessageReady);
    QVERIFY(launch->process.waitForFinished(5000));
    const auto closed = launch->process.close();
    QVERIFY2(closed.value.has_value(), qPrintable(closed.errorCode));
}

void WorkerHandshakeTest::receiveUntilMissingTargetHonorsAbsoluteDeadline()
{
    constexpr int timeoutMs = 100;
    constexpr int prequeuedHeartbeats = 5;
    WinPipePair pair = WinPipeTransport::createHostPair();
    QVERIFY(pair.isValid());
    WinPipeTransport peer = WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds());
    IpcSession host(pair.takeHost(), IpcRole::Host,
                    HostLaunchContext{QStringLiteral("flood-nonce"),
                                      QStringLiteral("com.qbrowser.flood")});
    QVERIFY(authenticateRawPeer(host, peer));
    QVERIFY(peer.writeAll(heartbeatFrames(prequeuedHeartbeats), 1000));

    qint64 elapsedMs = 0;
    int receiveCalls = 0;

    QElapsedTimer wall;
    wall.start();
    const SessionReceiveResult missing = receiveUntilWithClock(
        [&](const int remainingMs) {
            ++receiveCalls;
            const SessionReceiveResult result = host.receive(remainingMs);
            if (receiveCalls == prequeuedHeartbeats) elapsedMs = timeoutMs;
            return result;
        },
        [&] { return elapsedMs; }, ProtocolType::Ready, timeoutMs);

    QCOMPARE(missing.status, SessionStatus::TimedOut);
    QCOMPARE(missing.errorCode, QStringLiteral("worker.test.receive_timeout"));
    QCOMPARE(receiveCalls, prequeuedHeartbeats);
    QVERIFY(!host.isClosed());
    QVERIFY2(wall.elapsed() < 50,
             qPrintable(QStringLiteral("receiveUntil took %1ms").arg(wall.elapsed())));
}

void WorkerHandshakeTest::receiveUntilBoundsOverBudgetHeartbeatFlood()
{
    constexpr int timeoutMs = 100;
    constexpr int expectedHeartbeatBudget = 37;
    WinPipePair pair = WinPipeTransport::createHostPair();
    QVERIFY(pair.isValid());
    WinPipeTransport peer = WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds());
    IpcSession host(pair.takeHost(), IpcRole::Host,
                    HostLaunchContext{QStringLiteral("flood-nonce"),
                                      QStringLiteral("com.qbrowser.flood")});
    QVERIFY(authenticateRawPeer(host, peer));
    QVERIFY(peer.writeAll(heartbeatFrames(expectedHeartbeatBudget), 1000));

    int receiveCalls = 0;

    QElapsedTimer wall;
    wall.start();
    const SessionReceiveResult missing = receiveUntilWithClock(
        [&](const int remainingMs) {
            ++receiveCalls;
            return host.receive(remainingMs);
        },
        [] { return qint64(0); }, ProtocolType::Ready, timeoutMs);

    QCOMPARE(missing.status, SessionStatus::TimedOut);
    QCOMPARE(missing.errorCode, QStringLiteral("worker.test.heartbeat_limit"));
    QCOMPARE(receiveCalls, expectedHeartbeatBudget);
    QVERIFY(!host.isClosed());
    QVERIFY2(wall.elapsed() < 50,
             qPrintable(QStringLiteral("receiveUntil took %1ms").arg(wall.elapsed())));
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
