#include "IpcSession.h"

#include <QTest>

#include <chrono>
#include <future>
#include <type_traits>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

class IpcSessionTest final : public QObject
{
    Q_OBJECT

private slots:
    void anonymousPipeEndsHaveLeastInheritance();
    void pipeWriteTimeoutIsBounded();
    void authenticatesNonceAndUsesHostAssignedIdentity();
    void rejectsWrongNonceAndMalformedPeer();
    void correlatesResponsesAndRejectsDuplicateRequestIds();
    void correlatesRouteLoadAcknowledgement();
    void rejectsUnknownProtocolAndDuplicateInboundRequests();
    void expiresPendingRequests();
    void reportsTimeoutPeerCloseAndHeartbeat();
};

void IpcSessionTest::anonymousPipeEndsHaveLeastInheritance()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    WinPipePair pair = WinPipeTransport::createHostPair();
    QVERIFY(pair.isValid());

    DWORD hostReadFlags = 0;
    DWORD hostWriteFlags = 0;
    DWORD workerReadFlags = 0;
    DWORD workerWriteFlags = 0;
    QVERIFY(GetHandleInformation(pair.host().nativeReadHandle(), &hostReadFlags));
    QVERIFY(GetHandleInformation(pair.host().nativeWriteHandle(), &hostWriteFlags));
    QVERIFY(GetHandleInformation(pair.workerEnds().nativeReadHandle(), &workerReadFlags));
    QVERIFY(GetHandleInformation(pair.workerEnds().nativeWriteHandle(), &workerWriteFlags));
    QVERIFY(!(hostReadFlags & HANDLE_FLAG_INHERIT));
    QVERIFY(!(hostWriteFlags & HANDLE_FLAG_INHERIT));
    QVERIFY(workerReadFlags & HANDLE_FLAG_INHERIT);
    QVERIFY(workerWriteFlags & HANDLE_FLAG_INHERIT);

    WorkerPipeEnds ends = pair.takeWorkerEnds();
    static_assert(!std::is_copy_constructible_v<WorkerPipeEnds>);
    QVERIFY(ends.isValid());
    QVERIFY(!pair.workerEnds().isValid());
    WinPipeTransport worker = WinPipeTransport::adoptWorkerEnds(std::move(ends));
    QVERIFY(worker.isValid());
    QVERIFY(!ends.isValid());
    QVERIFY(GetHandleInformation(worker.nativeReadHandle(), &workerReadFlags));
    QVERIFY(GetHandleInformation(worker.nativeWriteHandle(), &workerWriteFlags));
    QVERIFY(!(workerReadFlags & HANDLE_FLAG_INHERIT));
    QVERIFY(!(workerWriteFlags & HANDLE_FLAG_INHERIT));
#endif
}

void IpcSessionTest::pipeWriteTimeoutIsBounded()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    WinPipePair pair = WinPipeTransport::createHostPair();
    WinPipeTransport host = pair.takeHost();
    WinPipeTransport worker = WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds());

    auto write = std::async(std::launch::async, [&host] {
        return host.writeAll(QByteArray(FrameCodec::maximumQueuedBytes(), 'x'), 25);
    });
    const auto completion = write.wait_for(std::chrono::milliseconds(250));
    if (completion != std::future_status::ready) {
        worker.close();
    }
    const bool result = write.get();

    QCOMPARE(completion, std::future_status::ready);
    QVERIFY(!result);
    QCOMPARE(host.lastStatus(), PipeIoStatus::TimedOut);
#endif
}

void IpcSessionTest::authenticatesNonceAndUsesHostAssignedIdentity()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    WinPipePair pair = WinPipeTransport::createHostPair();
    WinPipeTransport workerTransport = WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds());
    IpcSession host(pair.takeHost(), IpcRole::Host,
                    HostLaunchContext{QStringLiteral("launch-nonce"),
                                      QStringLiteral("com.qbrowser.host-assigned")});
    IpcSession worker(std::move(workerTransport), IpcRole::Worker);

    QVERIFY(worker.send(*ProtocolMessage::handshake(QStringLiteral("launch-nonce"))));
    const SessionReceiveResult received = host.receive(1000);

    QCOMPARE(received.status, SessionStatus::MessageReady);
    QCOMPARE(received.message->type(), ProtocolType::Handshake);
    QVERIFY(host.isAuthenticated());
    QCOMPARE(host.appIdentity(), QStringLiteral("com.qbrowser.host-assigned"));
    const SessionReceiveResult acknowledgement = worker.receive(1000);
    QCOMPARE(acknowledgement.status, SessionStatus::MessageReady);
    QCOMPARE(acknowledgement.message->type(), ProtocolType::HandshakeAck);
    QVERIFY(worker.isAuthenticated());
    QCOMPARE(worker.appIdentity(), QStringLiteral("com.qbrowser.host-assigned"));
#endif
}

void IpcSessionTest::rejectsWrongNonceAndMalformedPeer()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    {
        WinPipePair pair = WinPipeTransport::createHostPair();
        IpcSession host(pair.takeHost(), IpcRole::Host,
                        HostLaunchContext{QStringLiteral("right"), QStringLiteral("trusted")});
        IpcSession worker(WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds()),
                          IpcRole::Worker);
        QVERIFY(worker.send(*ProtocolMessage::handshake(QStringLiteral("wrong"))));
        QCOMPARE(host.receive(1000).status, SessionStatus::Failed);
        QCOMPARE(host.lastErrorCode(), QStringLiteral("ipc.session.nonce_mismatch"));
        QVERIFY(host.isClosed());
    }
    {
        WinPipePair pair = WinPipeTransport::createHostPair();
        IpcSession host(pair.takeHost(), IpcRole::Host,
                        HostLaunchContext{QStringLiteral("right"), QStringLiteral("trusted")});
        WinPipeTransport worker = WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds());
        QVERIFY(worker.writeAll(QByteArray(4, '\0'), 1000));
        QCOMPARE(host.receive(1000).status, SessionStatus::Failed);
        QCOMPARE(host.lastErrorCode(), QStringLiteral("ipc.frame.zero_length"));
        QVERIFY(host.isClosed());
    }
    {
        WinPipePair pair = WinPipeTransport::createHostPair();
        IpcSession host(pair.takeHost(), IpcRole::Host,
                        HostLaunchContext{QStringLiteral("right"), QStringLiteral("trusted")});
        WinPipeTransport worker = WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds());
        QVERIFY(worker.writeAll(FrameCodec::encode(
                                    ProtocolMessage::handshake(QStringLiteral("right"))->toJson()),
                                1000));
        QCOMPARE(host.receive(1000).status, SessionStatus::MessageReady);
        const QJsonObject malformedShutdown{
            {QStringLiteral("protocolVersion"), 1},
            {QStringLiteral("type"), QStringLiteral("shutdown")},
            {QStringLiteral("payload"), QJsonObject{}}};
        QVERIFY(worker.writeAll(FrameCodec::encode(malformedShutdown), 1000));
        QCOMPARE(host.receive(1000).status, SessionStatus::Failed);
        QCOMPARE(host.lastErrorCode(), QStringLiteral("ipc.protocol.invalid_payload"));
        QVERIFY(host.isClosed());
    }
#endif
}

void IpcSessionTest::correlatesResponsesAndRejectsDuplicateRequestIds()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    WinPipePair pair = WinPipeTransport::createHostPair();
    IpcSession host(pair.takeHost(), IpcRole::Host,
                    HostLaunchContext{QStringLiteral("n"), QStringLiteral("trusted")});
    IpcSession worker(WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds()),
                      IpcRole::Worker);

    QVERIFY(worker.send(*ProtocolMessage::handshake(QStringLiteral("n"))));
    QCOMPARE(host.receive(1000).status, SessionStatus::MessageReady);
    QCOMPARE(worker.receive(1000).status, SessionStatus::MessageReady);
    QVERIFY(worker.sendRequest(QStringLiteral("req-1"), QStringLiteral("storage"),
                               QStringLiteral("get"), QJsonObject{}, 1000));
    QVERIFY(!worker.sendRequest(QStringLiteral("req-1"), QStringLiteral("storage"),
                                QStringLiteral("get"), QJsonObject{}, 1000));
    QCOMPARE(worker.lastErrorCode(), QStringLiteral("ipc.session.duplicate_request_id"));
    QCOMPARE(host.receive(1000).message->requestId(), QStringLiteral("req-1"));
    QVERIFY(host.send(*ProtocolMessage::successResponse(QStringLiteral("req-1"),
                                                        QJsonObject{})));

    const SessionReceiveResult response = worker.receive(1000);
    QCOMPARE(response.status, SessionStatus::MessageReady);
    QCOMPARE(response.message->requestId(), QStringLiteral("req-1"));
    QCOMPARE(worker.pendingRequestCount(), qsizetype(0));

    QVERIFY(host.send(*ProtocolMessage::successResponse(QStringLiteral("unknown"),
                                                        QJsonObject{})));
    QCOMPARE(worker.receive(1000).status, SessionStatus::Failed);
    QCOMPARE(worker.lastErrorCode(), QStringLiteral("ipc.session.unknown_response"));
#endif
}

void IpcSessionTest::correlatesRouteLoadAcknowledgement()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    WinPipePair pair = WinPipeTransport::createHostPair();
    IpcSession host(pair.takeHost(), IpcRole::Host,
                    HostLaunchContext{QStringLiteral("n"), QStringLiteral("trusted")});
    IpcSession worker(WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds()),
                      IpcRole::Worker);
    QVERIFY(worker.send(*ProtocolMessage::handshake(QStringLiteral("n"))));
    QCOMPARE(host.receive(1000).status, SessionStatus::MessageReady);
    QCOMPARE(worker.receive(1000).status, SessionStatus::MessageReady);

    QVERIFY(host.sendRouteLoad(QStringLiteral("route-1"), QStringLiteral("/orders/42"),
                               1000));
    const SessionReceiveResult route = worker.receive(1000);
    QCOMPARE(route.status, SessionStatus::MessageReady);
    QCOMPARE(route.message->type(), ProtocolType::RouteLoad);
    QCOMPARE(route.message->payload().value(QStringLiteral("route")).toString(),
             QStringLiteral("/orders/42"));
    QVERIFY(worker.send(*ProtocolMessage::successResponse(QStringLiteral("route-1"), {})));
    QCOMPARE(host.receive(1000).status, SessionStatus::MessageReady);
    QCOMPARE(host.pendingRequestCount(), qsizetype(0));
#endif
}

void IpcSessionTest::rejectsUnknownProtocolAndDuplicateInboundRequests()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    {
        WinPipePair pair = WinPipeTransport::createHostPair();
        IpcSession host(pair.takeHost(), IpcRole::Host,
                        HostLaunchContext{QStringLiteral("n"), QStringLiteral("trusted")});
        WinPipeTransport worker = WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds());
        QJsonObject unknown{{QStringLiteral("protocolVersion"), 7},
                            {QStringLiteral("type"), QStringLiteral("heartbeat")},
                            {QStringLiteral("payload"), QJsonObject{}}};
        QVERIFY(worker.writeAll(FrameCodec::encode(unknown), 1000));
        QCOMPARE(host.receive(1000).status, SessionStatus::Failed);
        QCOMPARE(host.lastErrorCode(), QStringLiteral("ipc.protocol.unsupported_version"));
    }
    {
        WinPipePair pair = WinPipeTransport::createHostPair();
        IpcSession host(pair.takeHost(), IpcRole::Host,
                        HostLaunchContext{QStringLiteral("n"), QStringLiteral("trusted")});
        IpcSession worker(WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds()),
                          IpcRole::Worker);
        QVERIFY(worker.send(*ProtocolMessage::handshake(QStringLiteral("n"))));
        QCOMPARE(host.receive(1000).status, SessionStatus::MessageReady);
        QCOMPARE(worker.receive(1000).status, SessionStatus::MessageReady);
        const auto request = ProtocolMessage::request(QStringLiteral("same"),
                                                      QStringLiteral("storage"),
                                                      QStringLiteral("get"), {});
        QVERIFY(worker.send(*request));
        QCOMPARE(host.receive(1000).status, SessionStatus::MessageReady);
        QVERIFY(worker.send(*request));
        QCOMPARE(host.receive(1000).status, SessionStatus::Failed);
        QCOMPARE(host.lastErrorCode(), QStringLiteral("ipc.session.duplicate_request_id"));
    }
    {
        WinPipePair pair = WinPipeTransport::createHostPair();
        IpcSession host(pair.takeHost(), IpcRole::Host,
                        HostLaunchContext{QStringLiteral("n"), QStringLiteral("trusted")});
        WinPipeTransport worker = WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds());
        QVERIFY(worker.writeAll(FrameCodec::encode(
                                    ProtocolMessage::handshake(QStringLiteral("n"))->toJson()),
                                1000));
        QCOMPARE(host.receive(1000).status, SessionStatus::MessageReady);
        QVERIFY(worker.writeAll(FrameCodec::encode(
                                    ProtocolMessage::routeLoad(QStringLiteral("route"),
                                                               QStringLiteral("/forged"))
                                        ->toJson()),
                                1000));
        QCOMPARE(host.receive(1000).status, SessionStatus::Failed);
        QCOMPARE(host.lastErrorCode(),
                 QStringLiteral("ipc.session.unexpected_message_direction"));
    }
#endif
}

void IpcSessionTest::expiresPendingRequests()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    WinPipePair pair = WinPipeTransport::createHostPair();
    IpcSession host(pair.takeHost(), IpcRole::Host,
                    HostLaunchContext{QStringLiteral("n"), QStringLiteral("trusted")});
    IpcSession worker(WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds()),
                      IpcRole::Worker);
    QVERIFY(worker.send(*ProtocolMessage::handshake(QStringLiteral("n"))));
    QCOMPARE(host.receive(1000).status, SessionStatus::MessageReady);
    QCOMPARE(worker.receive(1000).status, SessionStatus::MessageReady);
    QVERIFY(worker.sendRequest(QStringLiteral("expires"), QStringLiteral("storage"),
                               QStringLiteral("get"), {}, 10));
    QCOMPARE(host.receive(1000).status, SessionStatus::MessageReady);
    QTest::qWait(20);
    QCOMPARE(worker.receive(1000).status, SessionStatus::TimedOut);
    QCOMPARE(worker.lastErrorCode(), QStringLiteral("ipc.session.request_timeout"));
#endif
}

void IpcSessionTest::reportsTimeoutPeerCloseAndHeartbeat()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    {
        WinPipePair pair = WinPipeTransport::createHostPair();
        IpcSession host(pair.takeHost(), IpcRole::Host,
                        HostLaunchContext{QStringLiteral("n"), QStringLiteral("trusted")});
        IpcSession worker(WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds()),
                          IpcRole::Worker);
        QVERIFY(worker.send(*ProtocolMessage::handshake(QStringLiteral("n"))));
        QCOMPARE(host.receive(1000).status, SessionStatus::MessageReady);
        QCOMPARE(worker.receive(1000).status, SessionStatus::MessageReady);
        QVERIFY(worker.send(ProtocolMessage::heartbeat()));
        const SessionReceiveResult heartbeat = host.receive(1000);
        QCOMPARE(heartbeat.status, SessionStatus::MessageReady);
        QCOMPARE(heartbeat.message->type(), ProtocolType::Heartbeat);
        QVERIFY(host.lastPeerActivityMonotonicMs() > 0);
    }
    {
        WinPipePair pair = WinPipeTransport::createHostPair();
        IpcSession host(pair.takeHost(), IpcRole::Host,
                        HostLaunchContext{QStringLiteral("n"), QStringLiteral("trusted")});
        QCOMPARE(host.receive(20).status, SessionStatus::TimedOut);
        QCOMPARE(host.lastErrorCode(), QStringLiteral("ipc.session.timeout"));
        QVERIFY(host.isClosed());
    }
    {
        WinPipePair pair = WinPipeTransport::createHostPair();
        IpcSession host(pair.takeHost(), IpcRole::Host,
                        HostLaunchContext{QStringLiteral("n"), QStringLiteral("trusted")});
        {
            IpcSession worker(WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds()),
                              IpcRole::Worker);
        }
        QCOMPARE(host.receive(1000).status, SessionStatus::PeerClosed);
        QCOMPARE(host.lastErrorCode(), QStringLiteral("ipc.session.peer_closed"));
        QVERIFY(host.isClosed());
    }
#endif
}

QTEST_MAIN(IpcSessionTest)

#include "tst_ipc_session.moc"
