#include "IpcSession.h"

#include <QCoreApplication>
#include <QTest>
#include <QtEndian>

#include <chrono>
#include <future>
#include <type_traits>
#include <string>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {

QByteArray rawJsonFrame(const QByteArray &json)
{
    QByteArray frame(4, '\0');
    qToBigEndian(static_cast<quint32>(json.size()),
                 reinterpret_cast<uchar *>(frame.data()));
    frame.append(json);
    return frame;
}

#ifdef Q_OS_WIN
class ChildProcess final
{
public:
    ~ChildProcess()
    {
        if (process_.hProcess != nullptr) {
            if (WaitForSingleObject(process_.hProcess, 0) == WAIT_TIMEOUT) {
                TerminateProcess(process_.hProcess, 99);
                WaitForSingleObject(process_.hProcess, 5000);
            }
            CloseHandle(process_.hProcess);
        }
        if (process_.hThread != nullptr) {
            CloseHandle(process_.hThread);
        }
    }

    PROCESS_INFORMATION *address() noexcept { return &process_; }
    HANDLE processHandle() const noexcept { return process_.hProcess; }

private:
    PROCESS_INFORMATION process_{};
};

int runIpcChild(const int argc, char **argv)
{
    if (argc != 4) {
        return 10;
    }
    bool readOk = false;
    bool writeOk = false;
    const quintptr readValue = QString::fromLocal8Bit(argv[2]).toULongLong(&readOk);
    const quintptr writeValue = QString::fromLocal8Bit(argv[3]).toULongLong(&writeOk);
    if (!readOk || !writeOk) {
        return 11;
    }
    const HANDLE inheritedRead = reinterpret_cast<HANDLE>(readValue);
    const HANDLE inheritedWrite = reinterpret_cast<HANDLE>(writeValue);
    DWORD readFlags = 0;
    DWORD writeFlags = 0;
    if (!GetHandleInformation(inheritedRead, &readFlags)) {
        return 20;
    }
    if (!GetHandleInformation(inheritedWrite, &writeFlags)) {
        return 21;
    }
    if ((readFlags & HANDLE_FLAG_INHERIT) == 0
        || (writeFlags & HANDLE_FLAG_INHERIT) == 0) {
        return 22;
    }
    if (GetFileType(inheritedRead) != FILE_TYPE_PIPE
        || GetFileType(inheritedWrite) != FILE_TYPE_PIPE) {
        return 23;
    }
    DWORD transferred = 0;
    if (!PeekNamedPipe(inheritedRead, nullptr, 0, nullptr, &transferred, nullptr)) {
        return 24;
    }
    char ignored = 0;
    if (!WriteFile(inheritedWrite, &ignored, 0, &transferred, nullptr)) {
        return 25;
    }
    auto transport = WinPipeTransport::adoptInheritedHandles(
        inheritedRead, inheritedWrite);
    if (!transport.has_value()) {
        return 12;
    }
    IpcSession worker(std::move(*transport), IpcRole::Worker);
    const auto handshake = ProtocolMessage::handshake(QStringLiteral("child-nonce"));
    if (!handshake.has_value() || !worker.send(*handshake)) {
        return 13;
    }
    const SessionReceiveResult acknowledgement = worker.receive(2000);
    if (acknowledgement.status != SessionStatus::MessageReady
        || !worker.isAuthenticated()
        || worker.appIdentity() != QStringLiteral("com.qbrowser.child")) {
        return 14;
    }
    return 0;
}
#endif

} // namespace

class IpcSessionTest final : public QObject
{
    Q_OBJECT

private slots:
    void anonymousPipeEndsHaveLeastInheritance();
    void rejectsInvalidInheritedHandles();
    void adoptsInheritedHandlesInRealChildProcess();
    void pipeWriteTimeoutIsBounded();
    void authenticatesNonceAndUsesHostAssignedIdentity();
    void rejectsWrongNonceAndMalformedPeer();
    void correlatesResponsesAndRejectsDuplicateRequestIds();
    void correlatesRouteLoadAcknowledgement();
    void correlatesWorkerNavigationAndRejectsReplay();
    void rejectsUnknownProtocolAndDuplicateInboundRequests();
    void expiresPendingRequests();
    void reportsTimeoutPeerCloseAndHeartbeat();
    void pageMetadataRequiresWorkerReadyAndIsOneWay();
    void prematureMalformedAndUnknownMetadataFailClosed();
    void pageMetadataHandlerDeliveryIsAtMostOnceAndReentrantSafe();
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

void IpcSessionTest::rejectsInvalidInheritedHandles()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    QVERIFY(!WinPipeTransport::adoptInheritedHandles(nullptr, nullptr).has_value());

    WinPipePair pair = WinPipeTransport::createHostPair();
    QVERIFY(pair.isValid());
    QVERIFY(!WinPipeTransport::adoptInheritedHandles(
                 pair.workerEnds().nativeWriteHandle(),
                 pair.workerEnds().nativeReadHandle())
                 .has_value());
    QVERIFY(pair.workerEnds().isValid());

    QVERIFY(SetHandleInformation(pair.workerEnds().nativeReadHandle(),
                                 HANDLE_FLAG_INHERIT, 0));
    QVERIFY(!WinPipeTransport::adoptInheritedHandles(
                 pair.workerEnds().nativeReadHandle(),
                 pair.workerEnds().nativeWriteHandle())
                 .has_value());
    QVERIFY(pair.workerEnds().isValid());
#endif
}

void IpcSessionTest::adoptsInheritedHandlesInRealChildProcess()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    WinPipePair pair = WinPipeTransport::createHostPair();
    QVERIFY(pair.isValid());
    const QString executable = QCoreApplication::applicationFilePath();
    const QString command = QStringLiteral("\"%1\" --ipc-child %2 %3")
                                .arg(executable,
                                     QString::number(reinterpret_cast<quintptr>(
                                         pair.workerEnds().nativeReadHandle())),
                                     QString::number(reinterpret_cast<quintptr>(
                                         pair.workerEnds().nativeWriteHandle())));
    std::wstring mutableCommand = command.toStdWString();
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    ChildProcess child;

    QVERIFY(CreateProcessW(reinterpret_cast<LPCWSTR>(executable.utf16()),
                           mutableCommand.data(),
                           nullptr,
                           nullptr,
                           TRUE,
                           CREATE_NO_WINDOW,
                           nullptr,
                           nullptr,
                           &startup,
                           child.address()));
    WorkerPipeEnds parentCopies = pair.takeWorkerEnds();
    parentCopies.close();
    IpcSession host(pair.takeHost(), IpcRole::Host,
                    HostLaunchContext{QStringLiteral("child-nonce"),
                                      QStringLiteral("com.qbrowser.child")});

    const SessionReceiveResult handshake = host.receive(2000);
    const DWORD waitResult = WaitForSingleObject(child.processHandle(), 5000);
    DWORD exitCode = 0;
    QVERIFY(GetExitCodeProcess(child.processHandle(), &exitCode));
    QCOMPARE(waitResult, DWORD(WAIT_OBJECT_0));
    QCOMPARE(exitCode, DWORD(0));
    QCOMPARE(handshake.status, SessionStatus::MessageReady);
    QCOMPARE(handshake.message->type(), ProtocolType::Handshake);
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
    const auto untrackedRequest = ProtocolMessage::request(QStringLiteral("untracked"),
                                                           QStringLiteral("storage"),
                                                           QStringLiteral("get"), {});
    QVERIFY(!worker.send(*untrackedRequest));
    QCOMPARE(worker.lastErrorCode(), QStringLiteral("ipc.session.tracking_required"));
    QCOMPARE(worker.pendingRequestCount(), qsizetype(0));
    const auto untrackedRoute = ProtocolMessage::routeLoad(QStringLiteral("untracked-route"),
                                                           QStringLiteral("/orders"));
    QVERIFY(!host.send(*untrackedRoute));
    QCOMPARE(host.lastErrorCode(), QStringLiteral("ipc.session.tracking_required"));
    QCOMPARE(host.pendingRequestCount(), qsizetype(0));

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

void IpcSessionTest::correlatesWorkerNavigationAndRejectsReplay()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    WinPipePair pair = WinPipeTransport::createHostPair();
    IpcSession host(pair.takeHost(), IpcRole::Host,
                    HostLaunchContext{QStringLiteral("n"), QStringLiteral("com.qbrowser.pilot")});
    IpcSession worker(WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds()),
                      IpcRole::Worker);
    QVERIFY(worker.send(*ProtocolMessage::handshake(QStringLiteral("n"))));
    QCOMPARE(host.receive(1000).status, SessionStatus::MessageReady);
    QCOMPARE(worker.receive(1000).status, SessionStatus::MessageReady);

    QVERIFY(worker.sendNavigationRequest(QStringLiteral("navigate-1"),
                                         QStringLiteral("/orders/42"), 1000));
    QVERIFY(!worker.sendNavigationRequest(QStringLiteral("navigate-1"),
                                          QStringLiteral("/orders/42"), 1000));
    QCOMPARE(worker.lastErrorCode(), QStringLiteral("ipc.session.duplicate_request_id"));
    const SessionReceiveResult navigation = host.receive(1000);
    QCOMPARE(navigation.status, SessionStatus::MessageReady);
    QCOMPARE(navigation.message->type(), ProtocolType::NavigationRequest);
    QCOMPARE(navigation.message->payload().value(QStringLiteral("route")).toString(),
             QStringLiteral("/orders/42"));
    QVERIFY(host.send(*ProtocolMessage::successResponse(QStringLiteral("navigate-1"),
                                                        QJsonObject{})));
    QCOMPARE(worker.receive(1000).status, SessionStatus::MessageReady);
    QCOMPARE(worker.pendingRequestCount(), qsizetype(0));
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
        WinPipeTransport worker = WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds());
        QVERIFY(worker.writeAll(FrameCodec::encode(
                                    ProtocolMessage::handshake(QStringLiteral("n"))->toJson()),
                                1000));
        QCOMPARE(host.receive(1000).status, SessionStatus::MessageReady);
        const auto request = ProtocolMessage::request(QStringLiteral("same"),
                                                      QStringLiteral("storage"),
                                                      QStringLiteral("get"), {});
        QVERIFY(worker.writeAll(FrameCodec::encode(request->toJson()), 1000));
        QCOMPARE(host.receive(1000).status, SessionStatus::MessageReady);
        QVERIFY(worker.writeAll(FrameCodec::encode(request->toJson()), 1000));
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
                               QStringLiteral("get"), {}, 25));
    QCOMPARE(host.receive(1000).status, SessionStatus::MessageReady);
    QElapsedTimer elapsed;
    elapsed.start();
    QCOMPARE(worker.receive(1000).status, SessionStatus::TimedOut);
    QCOMPARE(worker.lastErrorCode(), QStringLiteral("ipc.session.request_timeout"));
    QVERIFY(elapsed.elapsed() < 250);
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

void IpcSessionTest::pageMetadataRequiresWorkerReadyAndIsOneWay()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    WinPipePair pair = WinPipeTransport::createHostPair();
    IpcSession host(pair.takeHost(), IpcRole::Host,
                    HostLaunchContext{QStringLiteral("metadata-nonce"),
                                      QStringLiteral("com.qbrowser.metadata")});
    IpcSession worker(WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds()),
                      IpcRole::Worker);
    QVERIFY(worker.send(*ProtocolMessage::handshake(QStringLiteral("metadata-nonce"))));
    QCOMPARE(host.receive(1000).status, SessionStatus::MessageReady);
    QCOMPARE(worker.receive(1000).status, SessionStatus::MessageReady);

    const auto metadata = ProtocolMessage::pageMetadata(
        QStringLiteral("Orders"), QStringLiteral("ready"));
    QVERIFY(metadata.has_value());
    QVERIFY(!host.send(*metadata));
    QCOMPARE(host.lastErrorCode(),
             QStringLiteral("ipc.session.unexpected_message_direction"));
    QVERIFY(!worker.send(*metadata));
    QCOMPARE(worker.lastErrorCode(), QStringLiteral("ipc.session.ready_required"));
    QCOMPARE(host.poll().status, SessionStatus::TimedOut);
    QVERIFY(!host.isClosed());

    int observed = 0;
    QString observedTitle;
    QString observedStatus;
    host.setPageMetadataHandler([&](const QString &title, const QString &status) {
        ++observed;
        observedTitle = title;
        observedStatus = status;
    });
    QVERIFY(worker.send(ProtocolMessage::ready()));
    const SessionReceiveResult ready = host.receive(1000);
    QCOMPARE(ready.status, SessionStatus::MessageReady);
    QCOMPARE(ready.message->type(), ProtocolType::Ready);
    QVERIFY(worker.send(*metadata));
    const SessionReceiveResult received = host.receive(1000);
    QCOMPARE(received.status, SessionStatus::MessageReady);
    QCOMPARE(received.message->type(), ProtocolType::PageMetadata);
    QVERIFY(received.message->requestId().isEmpty());
    QCOMPARE(observed, 1);
    QCOMPARE(observedTitle, QStringLiteral("Orders"));
    QCOMPARE(observedStatus, QStringLiteral("ready"));
#endif
}

void IpcSessionTest::prematureMalformedAndUnknownMetadataFailClosed()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    const auto authenticatedRaw = [] {
        WinPipePair pair = WinPipeTransport::createHostPair();
        auto host = std::make_unique<IpcSession>(
            pair.takeHost(), IpcRole::Host,
            HostLaunchContext{QStringLiteral("raw-metadata"),
                              QStringLiteral("com.qbrowser.metadata")});
        WinPipeTransport peer = WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds());
        const auto handshake = ProtocolMessage::handshake(QStringLiteral("raw-metadata"));
        if (!handshake.has_value()
            || !peer.writeAll(FrameCodec::encode(handshake->toJson()), 1000)
            || host->receive(1000).status != SessionStatus::MessageReady) {
            return std::pair<std::unique_ptr<IpcSession>, WinPipeTransport>{};
        }
        return std::pair{std::move(host), std::move(peer)};
    };

    {
        auto [host, peer] = authenticatedRaw();
        QVERIFY(host != nullptr);
        const auto metadata = ProtocolMessage::pageMetadata(QStringLiteral("Too soon"));
        QVERIFY(metadata.has_value());
        QVERIFY(peer.writeAll(FrameCodec::encode(metadata->toJson()), 1000));
        QCOMPARE(host->receive(1000).status, SessionStatus::Failed);
        QCOMPARE(host->lastErrorCode(), QStringLiteral("ipc.session.ready_required"));
    }
    {
        auto [host, peer] = authenticatedRaw();
        QVERIFY(host != nullptr);
        const QJsonObject future{{QStringLiteral("protocolVersion"), 1},
                                 {QStringLiteral("type"),
                                  QStringLiteral("futurePageMetadata")},
                                 {QStringLiteral("payload"), QJsonObject{}}};
        QVERIFY(peer.writeAll(FrameCodec::encode(future), 1000));
        QCOMPARE(host->receive(1000).status, SessionStatus::Failed);
        QCOMPARE(host->lastErrorCode(), QStringLiteral("ipc.protocol.unknown_type"));
        QVERIFY(host->isClosed());
    }
    {
        auto [host, peer] = authenticatedRaw();
        QVERIFY(host != nullptr);
        QVERIFY(peer.writeAll(FrameCodec::encode(ProtocolMessage::ready().toJson()), 1000));
        QCOMPARE(host->receive(1000).status, SessionStatus::MessageReady);
        const QByteArray invalid = QByteArrayLiteral(
            R"({"protocolVersion":1,"type":"pageMetadata","payload":{"title":"\uD800"}})");
        QVERIFY(peer.writeAll(rawJsonFrame(invalid), 1000));
        QCOMPARE(host->receive(1000).status, SessionStatus::Failed);
        QCOMPARE(host->lastErrorCode(), QStringLiteral("ipc.frame.invalid_json"));
    }
    {
        auto [host, peer] = authenticatedRaw();
        QVERIFY(host != nullptr);
        const auto metadata = ProtocolMessage::pageMetadata(
            QStringLiteral("Buffered title"), QStringLiteral("ready"));
        QVERIFY(metadata.has_value());
        const QByteArray readyAndMetadata =
            FrameCodec::encode(ProtocolMessage::ready().toJson())
            + FrameCodec::encode(metadata->toJson());
        QVERIFY(peer.writeAll(readyAndMetadata, 1000));
        const SessionReceiveResult ready = host->receive(1000);
        QCOMPARE(ready.status, SessionStatus::MessageReady);
        QCOMPARE(ready.message->type(), ProtocolType::Ready);

        int observed = 0;
        host->setPageMetadataHandler(
            [&](const QString &title, const QString &status) {
                ++observed;
                QCOMPARE(title, QStringLiteral("Buffered title"));
                QCOMPARE(status, QStringLiteral("ready"));
            });
        QCOMPARE(observed, 1);
        const SessionReceiveResult queued = host->receive(1000);
        QCOMPARE(queued.status, SessionStatus::MessageReady);
        QCOMPARE(queued.message->type(), ProtocolType::PageMetadata);
        QCOMPARE(observed, 1);
    }
#endif
}

void IpcSessionTest::pageMetadataHandlerDeliveryIsAtMostOnceAndReentrantSafe()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    const auto authenticatedRaw = [](const QString &nonce) {
        WinPipePair pair = WinPipeTransport::createHostPair();
        auto host = std::make_unique<IpcSession>(
            pair.takeHost(), IpcRole::Host,
            HostLaunchContext{nonce, QStringLiteral("com.qbrowser.metadata")});
        WinPipeTransport peer = WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds());
        const auto handshake = ProtocolMessage::handshake(nonce);
        if (!handshake.has_value()
            || !peer.writeAll(FrameCodec::encode(handshake->toJson()), 1000)
            || host->receive(1000).status != SessionStatus::MessageReady) {
            return std::pair<std::unique_ptr<IpcSession>, WinPipeTransport>{};
        }
        return std::pair{std::move(host), std::move(peer)};
    };

    {
        auto [host, peer] = authenticatedRaw(QStringLiteral("metadata-at-most-once"));
        QVERIFY(host != nullptr);
        const auto buffered = ProtocolMessage::pageMetadata(
            QStringLiteral("Buffered title"), QStringLiteral("ready"));
        QVERIFY(buffered.has_value());
        QVERIFY(peer.writeAll(FrameCodec::encode(ProtocolMessage::ready().toJson())
                                  + FrameCodec::encode(buffered->toJson()),
                              1000));
        const SessionReceiveResult ready = host->receive(1000);
        QCOMPARE(ready.status, SessionStatus::MessageReady);
        QCOMPARE(ready.message->type(), ProtocolType::Ready);

        int handlerACalls = 0;
        int handlerBCalls = 0;
        IpcSession::PageMetadataHandler handlerA =
            [&](const QString &, const QString &) { ++handlerACalls; };
        IpcSession::PageMetadataHandler handlerB =
            [&](const QString &title, const QString &status) {
                ++handlerBCalls;
                QCOMPARE(title, QStringLiteral("Later title"));
                QCOMPARE(status, QStringLiteral("loading"));
            };
        host->setPageMetadataHandler(handlerA);
        QCOMPARE(handlerACalls, 1);
        host->setPageMetadataHandler(handlerA);
        QCOMPARE(handlerACalls, 1);
        host->setPageMetadataHandler(handlerB);
        QCOMPARE(handlerBCalls, 0);

        const SessionReceiveResult queued = host->receive(1000);
        QCOMPARE(queued.status, SessionStatus::MessageReady);
        QCOMPARE(queued.message->type(), ProtocolType::PageMetadata);
        QCOMPARE(queued.message->payload().value(QStringLiteral("title")).toString(),
                 QStringLiteral("Buffered title"));
        QCOMPARE(handlerACalls, 1);
        QCOMPARE(handlerBCalls, 0);

        const auto later = ProtocolMessage::pageMetadata(
            QStringLiteral("Later title"), QStringLiteral("loading"));
        QVERIFY(later.has_value());
        QVERIFY(peer.writeAll(FrameCodec::encode(later->toJson()), 1000));
        const SessionReceiveResult received = host->receive(1000);
        QCOMPARE(received.status, SessionStatus::MessageReady);
        QCOMPARE(received.message->type(), ProtocolType::PageMetadata);
        QCOMPARE(handlerBCalls, 1);
    }

    {
        auto [host, peer] = authenticatedRaw(QStringLiteral("metadata-clear-handler"));
        QVERIFY(host != nullptr);
        const auto first = ProtocolMessage::pageMetadata(QStringLiteral("First"));
        const auto second = ProtocolMessage::pageMetadata(QStringLiteral("Second"));
        QVERIFY(first.has_value());
        QVERIFY(second.has_value());
        QVERIFY(peer.writeAll(FrameCodec::encode(ProtocolMessage::ready().toJson())
                                  + FrameCodec::encode(first->toJson())
                                  + FrameCodec::encode(second->toJson()),
                              1000));
        QCOMPARE(host->receive(1000).message->type(), ProtocolType::Ready);

        int handlerCalls = 0;
        bool callbackThrew = false;
        try {
            host->setPageMetadataHandler(
                [&](const QString &, const QString &) {
                    ++handlerCalls;
                    host->setPageMetadataHandler({});
                });
        } catch (...) {
            callbackThrew = true;
        }
        QVERIFY(!callbackThrew);
        QCOMPARE(handlerCalls, 1);
        const SessionReceiveResult queuedFirst = host->receive(1000);
        const SessionReceiveResult queuedSecond = host->receive(1000);
        QCOMPARE(queuedFirst.status, SessionStatus::MessageReady);
        QCOMPARE(queuedSecond.status, SessionStatus::MessageReady);
        QCOMPARE(queuedFirst.message->payload().value(QStringLiteral("title")).toString(),
                 QStringLiteral("First"));
        QCOMPARE(queuedSecond.message->payload().value(QStringLiteral("title")).toString(),
                 QStringLiteral("Second"));
    }

    {
        auto [host, peer] = authenticatedRaw(QStringLiteral("metadata-close-handler"));
        QVERIFY(host != nullptr);
        const auto first = ProtocolMessage::pageMetadata(QStringLiteral("First"));
        const auto second = ProtocolMessage::pageMetadata(QStringLiteral("Second"));
        QVERIFY(first.has_value());
        QVERIFY(second.has_value());
        QVERIFY(peer.writeAll(FrameCodec::encode(ProtocolMessage::ready().toJson())
                                  + FrameCodec::encode(first->toJson())
                                  + FrameCodec::encode(second->toJson()),
                              1000));
        QCOMPARE(host->receive(1000).message->type(), ProtocolType::Ready);

        int handlerCalls = 0;
        bool callbackThrew = false;
        try {
            host->setPageMetadataHandler(
                [&](const QString &, const QString &) {
                    ++handlerCalls;
                    host->close();
                });
        } catch (...) {
            callbackThrew = true;
        }
        QVERIFY(!callbackThrew);
        QCOMPARE(handlerCalls, 1);
        QVERIFY(host->isClosed());
        QCOMPARE(host->receive(0).status, SessionStatus::Failed);
    }
#endif
}

int main(int argc, char **argv)
{
#ifdef Q_OS_WIN
    if (argc > 1 && QByteArray(argv[1]) == QByteArrayLiteral("--ipc-child")) {
        return runIpcChild(argc, argv);
    }
#endif
    QCoreApplication application(argc, argv);
    IpcSessionTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_ipc_session.moc"
