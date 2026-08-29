#include "HostWorkerSessionIo.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include <memory>
#include <optional>
#include <utility>

namespace {

struct AuthenticatedSessions final
{
    std::unique_ptr<IpcSession> host;
    std::unique_ptr<IpcSession> worker;
};

std::optional<AuthenticatedSessions> authenticatedSessions(
    const QString &appIdentity)
{
    WinPipePair pair = WinPipeTransport::createHostPair();
    if (!pair.isValid()) return std::nullopt;
    auto host = std::make_unique<IpcSession>(
        pair.takeHost(), IpcRole::Host,
        HostLaunchContext{QStringLiteral("host-io-review"), appIdentity});
    auto worker = std::make_unique<IpcSession>(
        WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds()),
        IpcRole::Worker);
    const auto handshake = ProtocolMessage::handshake(
        QStringLiteral("host-io-review"));
    if (!handshake.has_value() || !worker->send(*handshake, 1'000)
        || host->receive(1'000).status != SessionStatus::MessageReady
        || worker->receive(1'000).status != SessionStatus::MessageReady) {
        return std::nullopt;
    }
    return AuthenticatedSessions{std::move(host), std::move(worker)};
}

bool waitForSignal(QSignalSpy &spy, const int timeoutMs)
{
    return !spy.isEmpty() || spy.wait(timeoutMs);
}

} // namespace

class HostWorkerSessionIoTest final : public QObject
{
    Q_OBJECT

private slots:
    void blockedSendFailsOnItsIoDeadline();
    void silentShutdownPeerFailsOnItsIoDeadline();
    void retiredBindingRejectsQueuedCapabilityPublication();
};

void HostWorkerSessionIoTest::blockedSendFailsOnItsIoDeadline()
{
    auto sessions = authenticatedSessions(
        QStringLiteral("com.qbrowser.blocked-host-send"));
    QVERIFY(sessions.has_value());
    QVERIFY(sessions->worker->sendRequest(
        QStringLiteral("blocked-host-send"), QStringLiteral("storage"),
        QStringLiteral("get"), QJsonObject{}, 10'000));
    QCOMPARE(sessions->host->receive(1'000).status,
             SessionStatus::MessageReady);
    const auto response = ProtocolMessage::successResponse(
        QStringLiteral("blocked-host-send"),
        QJsonObject{{QStringLiteral("content"), QString(512 * 1024, u'x')}});
    QVERIFY(response.has_value());

    HostWorkerSessionIo io(std::move(sessions->host), 71,
                           QThread::currentThread());
    QSignalSpy failed(&io, &HostWorkerSessionIo::sessionFailed);
    QVERIFY(failed.isValid());
    QElapsedTimer elapsed;
    elapsed.start();
    io.sendMessage(71, 1, *response, false, QString{});

    QVERIFY2(waitForSignal(failed, 6'000),
             "blocked Host send did not reach a terminal deadline");
    QVERIFY2(elapsed.elapsed() < 5'750,
             qPrintable(QStringLiteral("blocked Host send remained active for %1 ms")
                            .arg(elapsed.elapsed())));
    QCOMPARE(failed.at(0).at(1).toString(),
             QStringLiteral("host.worker_session.send_timeout"));
    sessions->worker->close();
}

void HostWorkerSessionIoTest::silentShutdownPeerFailsOnItsIoDeadline()
{
    auto sessions = authenticatedSessions(
        QStringLiteral("com.qbrowser.silent-shutdown"));
    QVERIFY(sessions.has_value());
    HostWorkerSessionIo io(std::move(sessions->host), 72,
                           QThread::currentThread());
    QSignalSpy failed(&io, &HostWorkerSessionIo::sessionFailed);
    QVERIFY(failed.isValid());
    QElapsedTimer elapsed;
    elapsed.start();
    io.beginShutdown(72, QStringLiteral("test.silent_peer"));

    QVERIFY2(waitForSignal(failed, 6'000),
             "silent shutdown peer left Host IO non-terminal");
    QVERIFY2(elapsed.elapsed() < 5'750,
             qPrintable(QStringLiteral("silent shutdown remained active for %1 ms")
                            .arg(elapsed.elapsed())));
    QCOMPARE(failed.at(0).at(1).toString(),
             QStringLiteral("host.worker_session.shutdown_timeout"));
    sessions->worker->close();
}

void HostWorkerSessionIoTest::retiredBindingRejectsQueuedCapabilityPublication()
{
    const QString appIdentity =
        QStringLiteral("com.qbrowser.retired-io-binding");
    auto sessions = authenticatedSessions(appIdentity);
    QVERIFY(sessions.has_value());
    QVERIFY(sessions->worker->sendRequest(
        QStringLiteral("binding-blocker"), QStringLiteral("storage"),
        QStringLiteral("get"), QJsonObject{}, 10'000));
    QCOMPARE(sessions->host->receive(1'000).status,
             SessionStatus::MessageReady);
    QVERIFY(sessions->worker->sendRequest(
        QStringLiteral("binding-gated"), QStringLiteral("storage"),
        QStringLiteral("get"), QJsonObject{}, 10'000));
    QCOMPARE(sessions->host->receive(1'000).status,
             SessionStatus::MessageReady);

    const auto blocker = ProtocolMessage::successResponse(
        QStringLiteral("binding-blocker"),
        QJsonObject{{QStringLiteral("content"), QString(512 * 1024, u'x')}});
    const auto gated = ProtocolMessage::successResponse(
        QStringLiteral("binding-gated"),
        QJsonObject{{QStringLiteral("published"), true}});
    QVERIFY(blocker.has_value());
    QVERIFY(gated.has_value());
    QVERIFY(sessions->host->submitSend(*blocker).accepted);

    const TabCapabilityAuthority authority{
        QStringLiteral("retired-io-tab"), 9, appIdentity, 91, 901, 73, 17};
    auto admission = std::make_shared<AuthorityAdmissionToken>();
    std::optional<AuthorityAdmissionToken::UseGuard> acquired =
        admission->tryAcquireUse();
    QVERIFY(acquired.has_value());
    auto retainedUse =
        std::make_shared<AuthorityAdmissionToken::UseGuard>(
            std::move(*acquired));

    HostWorkerSessionIo io(std::move(sessions->host), 73,
                           QThread::currentThread(), authority);
    QSignalSpy shutdownFinished(&io,
                                &HostWorkerSessionIo::shutdownFinished);
    QVERIFY(shutdownFinished.isValid());
    io.start();
    io.sendMessage(73, 2, *gated, false, QString{}, authority,
                   retainedUse);
    io.beginShutdown(73, QStringLiteral("test.retire_binding"));

    const SessionReceiveResult blockingResponse =
        sessions->worker->receive(2'000);
    QCOMPARE(blockingResponse.status, SessionStatus::MessageReady);
    QCOMPARE(blockingResponse.message->requestId(),
             QStringLiteral("binding-blocker"));
    const SessionReceiveResult afterRetirement =
        sessions->worker->receive(2'000);
    QCOMPARE(afterRetirement.status, SessionStatus::MessageReady);
    QCOMPARE(afterRetirement.message->type(), ProtocolType::Shutdown);
    const auto acknowledgement = ProtocolMessage::shutdown(
        QStringLiteral("worker.ack"));
    QVERIFY(acknowledgement.has_value());
    QVERIFY(sessions->worker->send(*acknowledgement, 1'000));
    QVERIFY(waitForSignal(shutdownFinished, 1'000));
    sessions->worker->close();
}

QTEST_GUILESS_MAIN(HostWorkerSessionIoTest)

#include "tst_host_worker_session_io.moc"
