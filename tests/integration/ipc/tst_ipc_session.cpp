#include "IpcSession.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSemaphore>
#include <QTest>
#include <QtEndian>

#include <atomic>
#include <array>
#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

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

std::optional<QByteArray> readExactly(WinPipeTransport &transport,
                                      const qsizetype wantedBytes,
                                      const int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    QByteArray received;
    received.reserve(wantedBytes);
    while (received.size() < wantedBytes) {
        const int remaining = std::max(
            0, timeoutMs - static_cast<int>(timer.elapsed()));
        const PipeReadResult next = transport.readSome(
            wantedBytes - received.size(), remaining);
        if (next.status != PipeIoStatus::Ok || next.bytes.isEmpty()) {
            return std::nullopt;
        }
        received.append(next.bytes);
    }
    return received;
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
    void ipcSessionSubmissionIsPromptWhilePeerIsBlocked();
    void asynchronousSubmissionIsPromptAndOwnsBytes();
    void asynchronousWriterPreservesFifoAndCompletesExactlyOnce();
    void asynchronousWriterEnforcesExactFrameAndByteLimits();
    void asynchronousCloseCancelsEveryAcceptedWriteExactlyOnce();
    void queuedCancellationCompletesExactlyOnceAndPreservesFifo();
    void inFlightCancellationCannotTruncateFollowingFrame();
    void completionCanCloseItsTransportAndQuiescesQueuedWrites();
    void blockedWriterDoesNotDelaySiblingTransport();
    void publicationGateRunsAtDequeueAndCanDiscard();
    void asynchronousRouteDeadlineStartsAfterSuccessfulPublication();
    void asynchronousRouteDeadlineWakesBlockingReceive();
    void cancelledRoutePublicationReleasesItsReservation();
    void asynchronousCompletionCanDestroyItsSession();
    void authenticatesNonceAndUsesHostAssignedIdentity();
    void rejectsWrongNonceAndMalformedPeer();
    void correlatesResponsesAndRejectsDuplicateRequestIds();
    void repeatedRequestIdsDoNotCorrelateAcrossSessions();
    void correlatesRouteLoadAcknowledgement();
    void correlatesWorkerNavigationAndRejectsReplay();
    void rejectsUnknownProtocolAndDuplicateInboundRequests();
    void expiresPendingRequests();
    void reportsTimeoutPeerCloseAndHeartbeat();
    void pageMetadataRequiresWorkerReadyAndIsOneWay();
    void prematureMalformedAndUnknownMetadataFailClosed();
    void pageMetadataHandlerDeliveryIsAtMostOnceAndReentrantSafe();
    void visibilityChangedIsHostOnlyAndAuthenticated();
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

    DWORD bufferedAtReturn = 0;
    QVERIFY(PeekNamedPipe(worker.nativeReadHandle(), nullptr, 0, nullptr,
                          &bufferedAtReturn, nullptr));
    QVERIFY(bufferedAtReturn > 0);
    const PipeReadResult buffered = worker.readSome(
        static_cast<qsizetype>(bufferedAtReturn), 100);
    QCOMPARE(buffered.status, PipeIoStatus::Ok);
    QCOMPARE(buffered.bytes.size(), static_cast<qsizetype>(bufferedAtReturn));
    QTest::qWait(50);
    const PipeReadResult afterReturn = worker.readSome(1, 50);
    QVERIFY(afterReturn.status == PipeIoStatus::PeerClosed
            || afterReturn.status == PipeIoStatus::TimedOut);
    QVERIFY(afterReturn.bytes.isEmpty());
#endif
}

void IpcSessionTest::ipcSessionSubmissionIsPromptWhilePeerIsBlocked()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    WinPipePair pair = WinPipeTransport::createHostPair();
    QVERIFY(pair.isValid());
    IpcSession host(pair.takeHost(), IpcRole::Host,
                    HostLaunchContext{QStringLiteral("async-nonce"),
                                      QStringLiteral("com.qbrowser.async")});
    IpcSession worker(
        WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds()),
        IpcRole::Worker);
    QVERIFY(worker.send(*ProtocolMessage::handshake(
        QStringLiteral("async-nonce"))));
    QCOMPARE(host.receive(1'000).status, SessionStatus::MessageReady);
    QCOMPARE(worker.receive(1'000).status, SessionStatus::MessageReady);

    const auto message = ProtocolMessage::successResponse(
        QStringLiteral("async-response"),
        QJsonObject{{QStringLiteral("content"),
                     QString(512 * 1024, u'x')}});
    QVERIFY(message.has_value());
    QSemaphore completed;
    std::atomic<int> completionCalls{0};
    std::atomic<PipeIoStatus> completionStatus{PipeIoStatus::Failed};
    QElapsedTimer timer;
    timer.start();
    IpcSendSubmission submission = host.submitSend(
        *message,
        IpcSendWork{
            {},
            [&](const PipeWriteResult &result) {
                completionStatus.store(result.status,
                                       std::memory_order_release);
                completionCalls.fetch_add(1, std::memory_order_relaxed);
                completed.release();
            }});
    const qint64 elapsedMs = timer.elapsed();

    QVERIFY(submission.accepted);
    QVERIFY2(elapsedMs < 50,
             qPrintable(QStringLiteral("IpcSession::submitSend blocked for %1 ms")
                            .arg(elapsedMs)));
    QCOMPARE(completionCalls.load(std::memory_order_relaxed), 0);
    host.close();
    QVERIFY(completed.tryAcquire(1, 1'000));
    QCOMPARE(completionStatus.load(std::memory_order_acquire),
             PipeIoStatus::Cancelled);
    QCOMPARE(completionCalls.load(std::memory_order_relaxed), 1);
#endif
}

void IpcSessionTest::asynchronousSubmissionIsPromptAndOwnsBytes()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    WinPipePair pair = WinPipeTransport::createHostPair();
    QVERIFY(pair.isValid());
    WinPipeTransport sender = pair.takeHost();
    WinPipeTransport receiver =
        WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds());

    QByteArray ownedCandidate(256 * 1024, 'a');
    QSemaphore completed;
    std::atomic<int> completionCalls{0};
    std::atomic<PipeIoStatus> completionStatus{PipeIoStatus::Failed};
    QElapsedTimer submitTimer;
    submitTimer.start();
    PipeWriteSubmission submission = sender.submitWrite(PipeWriteWork{
        ownedCandidate,
        {},
        [&](const PipeWriteResult &result) {
            completionStatus.store(result.status, std::memory_order_release);
            completionCalls.fetch_add(1, std::memory_order_relaxed);
            completed.release();
        }});
    const qint64 submitElapsedMs = submitTimer.elapsed();

    QVERIFY(submission.accepted);
    QVERIFY2(submitElapsedMs < 50,
             qPrintable(QStringLiteral("submission blocked for %1 ms")
                            .arg(submitElapsedMs)));
    ownedCandidate.fill('b');
    const std::optional<QByteArray> received =
        readExactly(receiver, 256 * 1024, 2'000);
    QVERIFY(received.has_value());
    QCOMPARE(*received, QByteArray(256 * 1024, 'a'));
    QVERIFY(completed.tryAcquire(1, 1'000));
    QCOMPARE(completionStatus.load(std::memory_order_acquire), PipeIoStatus::Ok);
    QCOMPARE(completionCalls.load(std::memory_order_relaxed), 1);

    sender.close();
    QCOMPARE(completionCalls.load(std::memory_order_relaxed), 1);
#endif
}

void IpcSessionTest::asynchronousWriterPreservesFifoAndCompletesExactlyOnce()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    WinPipePair pair = WinPipeTransport::createHostPair();
    QVERIFY(pair.isValid());
    WinPipeTransport sender = pair.takeHost();
    WinPipeTransport receiver =
        WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds());

    const QList<QByteArray> frames{
        QByteArray(48 * 1024, '1'),
        QByteArray(48 * 1024, '2'),
        QByteArray(48 * 1024, '3')};
    QByteArray expected;
    QSemaphore completed;
    std::mutex completionMutex;
    std::vector<int> completionOrder;
    std::array<std::atomic<int>, 3> completionCalls{};
    for (int index = 0; index < 3; ++index) {
        expected.append(frames.at(index));
        PipeWriteSubmission submission = sender.submitWrite(PipeWriteWork{
            frames.at(index),
            {},
            [&, index](const PipeWriteResult &result) {
                QCOMPARE(result.status, PipeIoStatus::Ok);
                completionCalls.at(static_cast<std::size_t>(index))
                    .fetch_add(1, std::memory_order_relaxed);
                {
                    const std::lock_guard lock(completionMutex);
                    completionOrder.push_back(index);
                }
                completed.release();
            }});
        QVERIFY(submission.accepted);
    }

    const std::optional<QByteArray> received =
        readExactly(receiver, expected.size(), 2'000);
    QVERIFY(received.has_value());
    QCOMPARE(*received, expected);
    QVERIFY(completed.tryAcquire(3, 1'000));
    {
        const std::lock_guard lock(completionMutex);
        QCOMPARE(completionOrder, std::vector<int>({0, 1, 2}));
    }
    for (const std::atomic<int> &calls : completionCalls) {
        QCOMPARE(calls.load(std::memory_order_relaxed), 1);
    }
    sender.close();
    for (const std::atomic<int> &calls : completionCalls) {
        QCOMPARE(calls.load(std::memory_order_relaxed), 1);
    }
#endif
}

void IpcSessionTest::asynchronousWriterEnforcesExactFrameAndByteLimits()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    QCOMPARE(WinPipeTransport::maximumPendingWriteCount(), qsizetype(64));
    QCOMPARE(WinPipeTransport::maximumPendingWriteBytes(),
             qsizetype(4 * 1024 * 1024));

    {
        WinPipePair pair = WinPipeTransport::createHostPair();
        QVERIFY(pair.isValid());
        WinPipeTransport sender = pair.takeHost();
        WinPipeTransport receiver =
            WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds());
        std::vector<PipeWriteSubmission> accepted;
        accepted.reserve(64);
        accepted.push_back(sender.submitWrite(
            PipeWriteWork{QByteArray(1024 * 1024, 'x'), {}, {}}));
        QVERIFY(accepted.back().accepted);
        for (qsizetype index = 1;
             index < WinPipeTransport::maximumPendingWriteCount(); ++index) {
            accepted.push_back(sender.submitWrite(
                PipeWriteWork{QByteArray(1, 'x'), {}, {}}));
            QVERIFY(accepted.back().accepted);
        }
        PipeWriteSubmission rejected = sender.submitWrite(
            PipeWriteWork{QByteArray(1, 'x'), {}, {}});
        QVERIFY(!rejected.accepted);
        QCOMPARE(rejected.errorCode, QStringLiteral("ipc.send_queue_full"));
        sender.close();
    }

    {
        WinPipePair pair = WinPipeTransport::createHostPair();
        QVERIFY(pair.isValid());
        WinPipeTransport sender = pair.takeHost();
        WinPipeTransport receiver =
            WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds());
        std::vector<PipeWriteSubmission> accepted;
        accepted.reserve(4);
        for (int index = 0; index < 4; ++index) {
            accepted.push_back(sender.submitWrite(
                PipeWriteWork{QByteArray(1024 * 1024, 'x'), {}, {}}));
            QVERIFY(accepted.back().accepted);
        }
        PipeWriteSubmission rejected = sender.submitWrite(
            PipeWriteWork{QByteArray(1, 'x'), {}, {}});
        QVERIFY(!rejected.accepted);
        QCOMPARE(rejected.errorCode, QStringLiteral("ipc.send_queue_full"));
        sender.close();
    }
#endif
}

void IpcSessionTest::asynchronousCloseCancelsEveryAcceptedWriteExactlyOnce()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    WinPipePair pair = WinPipeTransport::createHostPair();
    QVERIFY(pair.isValid());
    WinPipeTransport sender = pair.takeHost();
    WinPipeTransport receiver =
        WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds());
    constexpr int writeCount = 4;
    QSemaphore completed;
    std::array<std::atomic<int>, writeCount> completionCalls{};
    std::array<std::atomic<PipeIoStatus>, writeCount> completionStatuses{};
    for (int index = 0; index < writeCount; ++index) {
        completionStatuses.at(static_cast<std::size_t>(index))
            .store(PipeIoStatus::Failed, std::memory_order_relaxed);
        PipeWriteSubmission submission = sender.submitWrite(PipeWriteWork{
            QByteArray(1024 * 1024, static_cast<char>('a' + index)),
            {},
            [&, index](const PipeWriteResult &result) {
                completionStatuses.at(static_cast<std::size_t>(index))
                    .store(result.status, std::memory_order_release);
                completionCalls.at(static_cast<std::size_t>(index))
                    .fetch_add(1, std::memory_order_relaxed);
                completed.release();
            }});
        QVERIFY(submission.accepted);
    }

    sender.close();
    QVERIFY(completed.tryAcquire(writeCount, 1'000));
    for (int index = 0; index < writeCount; ++index) {
        QCOMPARE(completionCalls.at(static_cast<std::size_t>(index))
                     .load(std::memory_order_relaxed),
                 1);
        QCOMPARE(completionStatuses.at(static_cast<std::size_t>(index))
                     .load(std::memory_order_acquire),
                 PipeIoStatus::Cancelled);
    }
    sender.close();
    QTest::qWait(10);
    for (const std::atomic<int> &calls : completionCalls) {
        QCOMPARE(calls.load(std::memory_order_relaxed), 1);
    }
#endif
}

void IpcSessionTest::queuedCancellationCompletesExactlyOnceAndPreservesFifo()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    WinPipePair pair = WinPipeTransport::createHostPair();
    QVERIFY(pair.isValid());
    WinPipeTransport sender = pair.takeHost();
    WinPipeTransport receiver =
        WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds());

    const QByteArray firstFrame(256 * 1024, 'f');
    const QByteArray cancelledFrame = QByteArrayLiteral("cancelled");
    const QByteArray survivingFrame = QByteArrayLiteral("surviving");
    QSemaphore firstCompleted;
    QSemaphore cancelledCompleted;
    QSemaphore survivingCompleted;
    std::atomic<int> firstCalls{0};
    std::atomic<int> cancelledCalls{0};
    std::atomic<int> survivingCalls{0};
    std::atomic<PipeIoStatus> cancelledStatus{PipeIoStatus::Failed};

    PipeWriteSubmission first = sender.submitWrite(PipeWriteWork{
        firstFrame,
        {},
        [&](const PipeWriteResult &result) {
            if (result.status == PipeIoStatus::Ok) {
                firstCalls.fetch_add(1, std::memory_order_relaxed);
            }
            firstCompleted.release();
        }});
    PipeWriteSubmission cancelled = sender.submitWrite(PipeWriteWork{
        cancelledFrame,
        {},
        [&](const PipeWriteResult &result) {
            cancelledStatus.store(result.status, std::memory_order_release);
            cancelledCalls.fetch_add(1, std::memory_order_relaxed);
            cancelledCompleted.release();
        }});
    PipeWriteSubmission surviving = sender.submitWrite(PipeWriteWork{
        survivingFrame,
        {},
        [&](const PipeWriteResult &result) {
            if (result.status == PipeIoStatus::Ok) {
                survivingCalls.fetch_add(1, std::memory_order_relaxed);
            }
            survivingCompleted.release();
        }});
    QVERIFY(first.accepted);
    QVERIFY(cancelled.accepted);
    QVERIFY(surviving.accepted);
    QVERIFY(cancelled.cancellation.cancel());
    QVERIFY(cancelledCompleted.tryAcquire(1, 1'000));
    QCOMPARE(cancelledStatus.load(std::memory_order_acquire),
             PipeIoStatus::Cancelled);
    QCOMPARE(cancelledCalls.load(std::memory_order_relaxed), 1);

    const std::optional<QByteArray> received = readExactly(
        receiver, firstFrame.size() + survivingFrame.size(), 2'000);
    QVERIFY(received.has_value());
    QCOMPARE(*received, firstFrame + survivingFrame);
    QVERIFY(firstCompleted.tryAcquire(1, 1'000));
    QVERIFY(survivingCompleted.tryAcquire(1, 1'000));
    QCOMPARE(firstCalls.load(std::memory_order_relaxed), 1);
    QCOMPARE(survivingCalls.load(std::memory_order_relaxed), 1);

    sender.close();
    QCOMPARE(cancelledCalls.load(std::memory_order_relaxed), 1);
#endif
}

void IpcSessionTest::inFlightCancellationCannotTruncateFollowingFrame()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    WinPipePair pair = WinPipeTransport::createHostPair();
    QVERIFY(pair.isValid());
    WinPipeTransport sender = pair.takeHost();
    WinPipeTransport receiver =
        WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds());

    const QByteArray firstFrame(256 * 1024, 'i');
    const QByteArray followingFrame = QByteArrayLiteral("following");
    QSemaphore firstCompleted;
    QSemaphore followingCompleted;
    std::atomic<int> firstCalls{0};
    std::atomic<int> followingCalls{0};
    std::atomic<PipeIoStatus> firstStatus{PipeIoStatus::Failed};

    PipeWriteSubmission first = sender.submitWrite(PipeWriteWork{
        firstFrame,
        {},
        [&](const PipeWriteResult &result) {
            firstStatus.store(result.status, std::memory_order_release);
            firstCalls.fetch_add(1, std::memory_order_relaxed);
            firstCompleted.release();
        }});
    QVERIFY(first.accepted);
    QVERIFY(first.cancellation.isValid());

    const PipeReadResult prefix = receiver.readSome(64 * 1024, 1'000);
    QCOMPARE(prefix.status, PipeIoStatus::Ok);
    QVERIFY(!prefix.bytes.isEmpty());
    QVERIFY(prefix.bytes.size() < firstFrame.size());
    QCOMPARE(prefix.bytes, QByteArray(prefix.bytes.size(), 'i'));

    QVERIFY2(!first.cancellation.cancel(),
             "An in-flight frame cannot be cancelled without truncating the stream");
    PipeWriteSubmission following = sender.submitWrite(PipeWriteWork{
        followingFrame,
        {},
        [&](const PipeWriteResult &result) {
            if (result.status == PipeIoStatus::Ok) {
                followingCalls.fetch_add(1, std::memory_order_relaxed);
            }
            followingCompleted.release();
        }});
    QVERIFY(following.accepted);

    const std::optional<QByteArray> remainder = readExactly(
        receiver, firstFrame.size() - prefix.bytes.size(), 2'000);
    QVERIFY(remainder.has_value());
    QCOMPARE(prefix.bytes + *remainder, firstFrame);
    QVERIFY(firstCompleted.tryAcquire(1, 1'000));
    QCOMPARE(firstStatus.load(std::memory_order_acquire), PipeIoStatus::Ok);
    QCOMPARE(firstCalls.load(std::memory_order_relaxed), 1);

    const std::optional<QByteArray> receivedFollowing = readExactly(
        receiver, followingFrame.size(), 1'000);
    QVERIFY(receivedFollowing.has_value());
    QCOMPARE(*receivedFollowing, followingFrame);
    QVERIFY(followingCompleted.tryAcquire(1, 1'000));
    QCOMPARE(followingCalls.load(std::memory_order_relaxed), 1);

    sender.close();
    QCOMPARE(firstCalls.load(std::memory_order_relaxed), 1);
    QCOMPARE(followingCalls.load(std::memory_order_relaxed), 1);
#endif
}

void IpcSessionTest::completionCanCloseItsTransportAndQuiescesQueuedWrites()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    WinPipePair pair = WinPipeTransport::createHostPair();
    QVERIFY(pair.isValid());
    WinPipeTransport sender = pair.takeHost();
    WinPipeTransport receiver =
        WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds());
    QSemaphore completed;
    std::atomic<int> firstCalls{0};
    std::atomic<int> secondCalls{0};
    std::atomic_bool queuedQuiescedBeforeCloseReturned{false};

    PipeWriteSubmission first = sender.submitWrite(PipeWriteWork{
        QByteArray(256 * 1024, 'f'),
        {},
        [&](const PipeWriteResult &result) {
            if (result.status == PipeIoStatus::Ok) {
                firstCalls.fetch_add(1, std::memory_order_relaxed);
            }
            sender.close();
            queuedQuiescedBeforeCloseReturned.store(
                secondCalls.load(std::memory_order_acquire) == 1,
                std::memory_order_release);
            completed.release();
        }});
    PipeWriteSubmission second = sender.submitWrite(PipeWriteWork{
        QByteArrayLiteral("queued"),
        {},
        [&](const PipeWriteResult &result) {
            if (result.status == PipeIoStatus::Cancelled) {
                secondCalls.fetch_add(1, std::memory_order_release);
            }
            completed.release();
        }});
    QVERIFY(first.accepted);
    QVERIFY(second.accepted);
    QVERIFY(readExactly(receiver, 256 * 1024, 2'000).has_value());
    QVERIFY(completed.tryAcquire(2, 1'000));
    QCOMPARE(firstCalls.load(std::memory_order_relaxed), 1);
    QCOMPARE(secondCalls.load(std::memory_order_acquire), 1);
    QVERIFY(queuedQuiescedBeforeCloseReturned.load(std::memory_order_acquire));
    sender.close();
    QCOMPARE(firstCalls.load(std::memory_order_relaxed), 1);
    QCOMPARE(secondCalls.load(std::memory_order_acquire), 1);
#endif
}

void IpcSessionTest::blockedWriterDoesNotDelaySiblingTransport()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    WinPipePair blockedPair = WinPipeTransport::createHostPair();
    WinPipePair siblingPair = WinPipeTransport::createHostPair();
    QVERIFY(blockedPair.isValid());
    QVERIFY(siblingPair.isValid());
    WinPipeTransport blockedSender = blockedPair.takeHost();
    WinPipeTransport blockedReceiver =
        WinPipeTransport::adoptWorkerEnds(blockedPair.takeWorkerEnds());
    WinPipeTransport siblingSender = siblingPair.takeHost();
    WinPipeTransport siblingReceiver =
        WinPipeTransport::adoptWorkerEnds(siblingPair.takeWorkerEnds());

    PipeWriteSubmission blocked = blockedSender.submitWrite(
        PipeWriteWork{QByteArray(1024 * 1024, 'b'), {}, {}});
    QVERIFY(blocked.accepted);

    QSemaphore siblingCompleted;
    QElapsedTimer siblingTimer;
    siblingTimer.start();
    PipeWriteSubmission sibling = siblingSender.submitWrite(PipeWriteWork{
        QByteArrayLiteral("sibling"),
        {},
        [&](const PipeWriteResult &result) {
            QCOMPARE(result.status, PipeIoStatus::Ok);
            siblingCompleted.release();
        }});
    QVERIFY(sibling.accepted);
    const std::optional<QByteArray> received =
        readExactly(siblingReceiver, 7, 250);
    QVERIFY(received.has_value());
    QCOMPARE(*received, QByteArrayLiteral("sibling"));
    QVERIFY(siblingCompleted.tryAcquire(1, 250));
    QVERIFY2(siblingTimer.elapsed() < 250,
             qPrintable(QStringLiteral("sibling stalled for %1 ms")
                            .arg(siblingTimer.elapsed())));

    auto blockedClose = std::async(std::launch::async,
                                   [&blockedSender] { blockedSender.close(); });
    PipeWriteSubmission secondSibling = siblingSender.submitWrite(
        PipeWriteWork{QByteArrayLiteral("still-responsive"), {}, {}});
    QVERIFY(secondSibling.accepted);
    QVERIFY(readExactly(siblingReceiver, 16, 250).has_value());
    QCOMPARE(blockedClose.wait_for(std::chrono::seconds(1)),
             std::future_status::ready);
    blockedClose.get();
#endif
}

void IpcSessionTest::publicationGateRunsAtDequeueAndCanDiscard()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    WinPipePair pair = WinPipeTransport::createHostPair();
    QVERIFY(pair.isValid());
    WinPipeTransport sender = pair.takeHost();
    WinPipeTransport receiver =
        WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds());

    QSemaphore firstCompleted;
    PipeWriteSubmission first = sender.submitWrite(PipeWriteWork{
        QByteArray(1024 * 1024, 'x'),
        {},
        [&](const PipeWriteResult &) { firstCompleted.release(); }});
    QVERIFY(first.accepted);

    std::atomic_bool admitted{true};
    std::atomic<int> gateCalls{0};
    std::atomic<int> completionCalls{0};
    QSemaphore discarded;
    PipeWriteSubmission gated = sender.submitWrite(PipeWriteWork{
        QByteArrayLiteral("must-not-publish"),
        [&] {
            gateCalls.fetch_add(1, std::memory_order_relaxed);
            return admitted.load(std::memory_order_acquire);
        },
        [&](const PipeWriteResult &result) {
            QCOMPARE(result.status, PipeIoStatus::Cancelled);
            completionCalls.fetch_add(1, std::memory_order_relaxed);
            discarded.release();
        }});
    QVERIFY(gated.accepted);
    QCOMPARE(gateCalls.load(std::memory_order_relaxed), 0);
    admitted.store(false, std::memory_order_release);

    QVERIFY(readExactly(receiver, 1024 * 1024, 2'000).has_value());
    QVERIFY(firstCompleted.tryAcquire(1, 1'000));
    QVERIFY(discarded.tryAcquire(1, 1'000));
    QCOMPARE(gateCalls.load(std::memory_order_relaxed), 1);
    QCOMPARE(completionCalls.load(std::memory_order_relaxed), 1);
    QCOMPARE(receiver.readSome(64, 20).status, PipeIoStatus::TimedOut);
    sender.close();
    QCOMPARE(completionCalls.load(std::memory_order_relaxed), 1);
#endif
}

void IpcSessionTest::asynchronousRouteDeadlineStartsAfterSuccessfulPublication()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    WinPipePair pair = WinPipeTransport::createHostPair();
    QVERIFY(pair.isValid());
    IpcSession host(
        pair.takeHost(), IpcRole::Host,
        HostLaunchContext{QStringLiteral("deferred-route-deadline"),
                          QStringLiteral("com.qbrowser.deferred-route")});
    IpcSession worker(
        WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds()),
        IpcRole::Worker);
    QVERIFY(worker.send(*ProtocolMessage::handshake(
        QStringLiteral("deferred-route-deadline"))));
    QCOMPARE(host.receive(1'000).status, SessionStatus::MessageReady);
    QCOMPARE(worker.receive(1'000).status, SessionStatus::MessageReady);

    QVERIFY(worker.sendRequest(
        QStringLiteral("blocking-response"), QStringLiteral("storage"),
        QStringLiteral("get"), QJsonObject{}, 1'000));
    QCOMPARE(host.receive(1'000).status, SessionStatus::MessageReady);
    const auto blockingResponse = ProtocolMessage::successResponse(
        QStringLiteral("blocking-response"),
        QJsonObject{{QStringLiteral("content"), QString(512 * 1024, u'x')}});
    QVERIFY(blockingResponse.has_value());
    IpcSendSubmission blocker = host.submitSend(*blockingResponse);
    QVERIFY(blocker.accepted);

    QSemaphore published;
    std::atomic<PipeIoStatus> publicationStatus{PipeIoStatus::Failed};
    IpcSendSubmission route = host.submitRouteLoad(
        QStringLiteral("deferred-route"), QStringLiteral("/orders"), 25,
        IpcSendWork{
            {},
            [&](const PipeWriteResult &result) {
                publicationStatus.store(result.status,
                                        std::memory_order_release);
                published.release();
            }});
    QVERIFY(route.accepted);
    QCOMPARE(host.pendingRequestCount(), qsizetype(1));

    QTest::qWait(75);
    const SessionReceiveResult beforePublication = host.poll(0);
    QCOMPARE(beforePublication.status, SessionStatus::TimedOut);
    QVERIFY(!host.isClosed());
    QCOMPARE(host.pendingRequestCount(), qsizetype(1));

    const SessionReceiveResult blocking = worker.receive(2'000);
    QCOMPARE(blocking.status, SessionStatus::MessageReady);
    QCOMPARE(blocking.message->requestId(),
             QStringLiteral("blocking-response"));
    const SessionReceiveResult routed = worker.receive(2'000);
    QCOMPARE(routed.status, SessionStatus::MessageReady);
    QCOMPARE(routed.message->requestId(), QStringLiteral("deferred-route"));
    QVERIFY(published.tryAcquire(1, 1'000));
    QCOMPARE(publicationStatus.load(std::memory_order_acquire),
             PipeIoStatus::Ok);

    QTest::qWait(50);
    QCOMPARE(host.poll(0).status, SessionStatus::TimedOut);
    QCOMPARE(host.lastErrorCode(),
             QStringLiteral("ipc.session.request_timeout"));
    QVERIFY(host.isClosed());
#endif
}

void IpcSessionTest::asynchronousRouteDeadlineWakesBlockingReceive()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    WinPipePair pair = WinPipeTransport::createHostPair();
    QVERIFY(pair.isValid());
    IpcSession host(
        pair.takeHost(), IpcRole::Host,
        HostLaunchContext{QStringLiteral("published-route-wakeup"),
                          QStringLiteral("com.qbrowser.route-wakeup")});
    IpcSession worker(
        WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds()),
        IpcRole::Worker);
    QVERIFY(worker.send(*ProtocolMessage::handshake(
        QStringLiteral("published-route-wakeup"))));
    QCOMPARE(host.receive(1'000).status, SessionStatus::MessageReady);
    QCOMPARE(worker.receive(1'000).status, SessionStatus::MessageReady);

    QSemaphore existingPublished;
    QVERIFY(host.submitRouteLoad(
                    QStringLiteral("existing-long-route"),
                    QStringLiteral("/existing"), 5'000,
                    IpcSendWork{
                        {},
                        [&existingPublished](const PipeWriteResult &result) {
                            if (result.status == PipeIoStatus::Ok)
                                existingPublished.release();
                        }})
                .accepted);
    const SessionReceiveResult existing = worker.receive(1'000);
    QCOMPARE(existing.status, SessionStatus::MessageReady);
    QCOMPARE(existing.message->requestId(),
             QStringLiteral("existing-long-route"));
    QVERIFY(existingPublished.tryAcquire(1, 1'000));

    QVERIFY(worker.sendRequest(
        QStringLiteral("wakeup-blocker"), QStringLiteral("storage"),
        QStringLiteral("get"), QJsonObject{}, 1'000));
    QCOMPARE(host.receive(1'000).status, SessionStatus::MessageReady);
    const auto blockingResponse = ProtocolMessage::successResponse(
        QStringLiteral("wakeup-blocker"),
        QJsonObject{{QStringLiteral("content"), QString(512 * 1024, u'x')}});
    QVERIFY(blockingResponse.has_value());
    QVERIFY(host.submitSend(*blockingResponse).accepted);
    QVERIFY(host.submitRouteLoad(
                    QStringLiteral("wakeup-route"), QStringLiteral("/orders"),
                    25)
                .accepted);

    auto drainWorker = std::async(std::launch::async, [&worker] {
        QTest::qSleep(75);
        const SessionReceiveResult blocking = worker.receive(2'000);
        const SessionReceiveResult routed = worker.receive(2'000);
        return std::pair{blocking, routed};
    });
    QElapsedTimer elapsed;
    elapsed.start();
    const SessionReceiveResult expired = host.receive(1'000);
    const qint64 expiryElapsedMs = elapsed.elapsed();

    QCOMPARE(expired.status, SessionStatus::TimedOut);
    QCOMPARE(expired.errorCode,
             QStringLiteral("ipc.session.request_timeout"));
    QVERIFY2(expiryElapsedMs < 500,
             qPrintable(QStringLiteral(
                            "published request deadline was enforced after %1 ms")
                            .arg(expiryElapsedMs)));
    const auto drained = drainWorker.get();
    QCOMPARE(drained.first.status, SessionStatus::MessageReady);
    QCOMPARE(drained.first.message->requestId(),
             QStringLiteral("wakeup-blocker"));
    QCOMPARE(drained.second.status, SessionStatus::MessageReady);
    QCOMPARE(drained.second.message->requestId(),
             QStringLiteral("wakeup-route"));
#endif
}

void IpcSessionTest::cancelledRoutePublicationReleasesItsReservation()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    WinPipePair pair = WinPipeTransport::createHostPair();
    QVERIFY(pair.isValid());
    IpcSession host(
        pair.takeHost(), IpcRole::Host,
        HostLaunchContext{QStringLiteral("cancelled-route"),
                          QStringLiteral("com.qbrowser.cancelled-route")});
    IpcSession worker(
        WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds()),
        IpcRole::Worker);
    QVERIFY(worker.send(*ProtocolMessage::handshake(
        QStringLiteral("cancelled-route"))));
    QCOMPARE(host.receive(1'000).status, SessionStatus::MessageReady);
    QCOMPARE(worker.receive(1'000).status, SessionStatus::MessageReady);

    QVERIFY(worker.sendRequest(
        QStringLiteral("blocking-cancel"), QStringLiteral("storage"),
        QStringLiteral("get"), QJsonObject{}, 1'000));
    QCOMPARE(host.receive(1'000).status, SessionStatus::MessageReady);
    const auto blockingResponse = ProtocolMessage::successResponse(
        QStringLiteral("blocking-cancel"),
        QJsonObject{{QStringLiteral("content"), QString(512 * 1024, u'x')}});
    QVERIFY(blockingResponse.has_value());
    QVERIFY(host.submitSend(*blockingResponse).accepted);

    QSemaphore completed;
    std::atomic<PipeIoStatus> completionStatus{PipeIoStatus::Failed};
    IpcSendSubmission cancelled = host.submitRouteLoad(
        QStringLiteral("reusable-route"), QStringLiteral("/first"), 5'000,
        IpcSendWork{
            [] { return false; },
            [&](const PipeWriteResult &result) {
                completionStatus.store(result.status,
                                       std::memory_order_release);
                completed.release();
            }});
    QVERIFY(cancelled.accepted);
    QCOMPARE(host.pendingRequestCount(), qsizetype(1));

    QCOMPARE(worker.receive(2'000).status, SessionStatus::MessageReady);
    QVERIFY(completed.tryAcquire(1, 1'000));
    QCOMPARE(completionStatus.load(std::memory_order_acquire),
             PipeIoStatus::Cancelled);
    QCOMPARE(host.pendingRequestCount(), qsizetype(0));
    QVERIFY(!host.isClosed());

    IpcSendSubmission reused = host.submitRouteLoad(
        QStringLiteral("reusable-route"), QStringLiteral("/second"), 5'000);
    QVERIFY2(reused.accepted, qPrintable(reused.errorCode));
    host.close();
#endif
}

void IpcSessionTest::asynchronousCompletionCanDestroyItsSession()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    WinPipePair pair = WinPipeTransport::createHostPair();
    QVERIFY(pair.isValid());
    auto host = std::make_unique<IpcSession>(
        pair.takeHost(), IpcRole::Host,
        HostLaunchContext{QStringLiteral("destroying-completion"),
                          QStringLiteral("com.qbrowser.destroying-completion")});
    IpcSession worker(
        WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds()),
        IpcRole::Worker);
    QVERIFY(worker.send(*ProtocolMessage::handshake(
        QStringLiteral("destroying-completion"))));
    QCOMPARE(host->receive(1'000).status, SessionStatus::MessageReady);
    QCOMPARE(worker.receive(1'000).status, SessionStatus::MessageReady);

    QSemaphore completed;
    std::atomic<int> completionCalls{0};
    IpcSendSubmission submission = host->submitSend(
        ProtocolMessage::heartbeat(),
        IpcSendWork{
            {},
            [&](const PipeWriteResult &result) {
                QCOMPARE(result.status, PipeIoStatus::Ok);
                ++completionCalls;
                host.reset();
                completed.release();
            }});
    QVERIFY(submission.accepted);
    QCOMPARE(worker.receive(1'000).status, SessionStatus::MessageReady);
    QVERIFY(completed.tryAcquire(1, 1'000));
    QCOMPARE(completionCalls.load(), 1);
    QVERIFY(host == nullptr);
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

void IpcSessionTest::repeatedRequestIdsDoNotCorrelateAcrossSessions()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    WinPipePair firstPair = WinPipeTransport::createHostPair();
    WinPipePair secondPair = WinPipeTransport::createHostPair();
    QVERIFY(firstPair.isValid());
    QVERIFY(secondPair.isValid());
    IpcSession firstHost(
        firstPair.takeHost(), IpcRole::Host,
        HostLaunchContext{QStringLiteral("first-session"),
                          QStringLiteral("com.qbrowser.same-app")});
    IpcSession firstWorker(
        WinPipeTransport::adoptWorkerEnds(firstPair.takeWorkerEnds()),
        IpcRole::Worker);
    IpcSession secondHost(
        secondPair.takeHost(), IpcRole::Host,
        HostLaunchContext{QStringLiteral("second-session"),
                          QStringLiteral("com.qbrowser.same-app")});
    IpcSession secondWorker(
        WinPipeTransport::adoptWorkerEnds(secondPair.takeWorkerEnds()),
        IpcRole::Worker);

    QVERIFY(firstWorker.send(*ProtocolMessage::handshake(
        QStringLiteral("first-session"))));
    QVERIFY(secondWorker.send(*ProtocolMessage::handshake(
        QStringLiteral("second-session"))));
    QCOMPARE(firstHost.receive(1000).status, SessionStatus::MessageReady);
    QCOMPARE(secondHost.receive(1000).status, SessionStatus::MessageReady);
    QCOMPARE(firstWorker.receive(1000).status, SessionStatus::MessageReady);
    QCOMPARE(secondWorker.receive(1000).status, SessionStatus::MessageReady);

    const QString repeatedId = QStringLiteral("same-request-id");
    QVERIFY(firstWorker.sendRequest(
        repeatedId, QStringLiteral("storage"), QStringLiteral("get"),
        QJsonObject{{QStringLiteral("key"), QStringLiteral("first")}},
        1000));
    QVERIFY(secondWorker.sendRequest(
        repeatedId, QStringLiteral("storage"), QStringLiteral("get"),
        QJsonObject{{QStringLiteral("key"), QStringLiteral("second")}},
        1000));
    QCOMPARE(firstHost.receive(1000).message->requestId(), repeatedId);
    QCOMPARE(secondHost.receive(1000).message->requestId(), repeatedId);
    QCOMPARE(firstWorker.pendingRequestCount(), qsizetype(1));
    QCOMPARE(secondWorker.pendingRequestCount(), qsizetype(1));

    const auto firstResponse = ProtocolMessage::successResponse(
        repeatedId,
        QJsonObject{{QStringLiteral("owner"), QStringLiteral("first")}});
    QVERIFY(firstResponse.has_value());
    QVERIFY(firstHost.send(*firstResponse));
    const SessionReceiveResult firstReceived = firstWorker.receive(1000);
    QCOMPARE(firstReceived.status, SessionStatus::MessageReady);
    QCOMPARE(firstReceived.message->payload()
                 .value(QStringLiteral("result")).toObject()
                 .value(QStringLiteral("owner")).toString(),
             QStringLiteral("first"));
    QCOMPARE(firstWorker.pendingRequestCount(), qsizetype(0));
    QCOMPARE(secondWorker.pendingRequestCount(), qsizetype(1));
    QCOMPARE(secondWorker.poll(0).status, SessionStatus::TimedOut);
    QVERIFY(!secondWorker.isClosed());

    const auto secondResponse = ProtocolMessage::successResponse(
        repeatedId,
        QJsonObject{{QStringLiteral("owner"), QStringLiteral("second")}});
    QVERIFY(secondResponse.has_value());
    QVERIFY(secondHost.send(*secondResponse));
    const SessionReceiveResult secondReceived = secondWorker.receive(1000);
    QCOMPARE(secondReceived.status, SessionStatus::MessageReady);
    QCOMPARE(secondReceived.message->payload()
                 .value(QStringLiteral("result")).toObject()
                 .value(QStringLiteral("owner")).toString(),
             QStringLiteral("second"));
    QCOMPARE(secondWorker.pendingRequestCount(), qsizetype(0));

    firstHost.close();
    QVERIFY(secondWorker.send(ProtocolMessage::heartbeat(), 1000));
    const SessionReceiveResult secondHeartbeat = secondHost.receive(1000);
    QCOMPARE(secondHeartbeat.status, SessionStatus::MessageReady);
    QCOMPARE(secondHeartbeat.message->type(), ProtocolType::Heartbeat);
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

void IpcSessionTest::visibilityChangedIsHostOnlyAndAuthenticated()
{
#ifndef Q_OS_WIN
    QSKIP("Windows anonymous pipe contract");
#else
    WinPipePair pair = WinPipeTransport::createHostPair();
    IpcSession host(pair.takeHost(), IpcRole::Host,
                    HostLaunchContext{QStringLiteral("visibility-nonce"),
                                      QStringLiteral("com.qbrowser.visibility")});
    IpcSession worker(WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds()),
                      IpcRole::Worker);

    QVERIFY(!host.send(ProtocolMessage::visibilityChanged(true).value()));
    QCOMPARE(host.lastErrorCode(), QStringLiteral("ipc.session.authentication_required"));
    QVERIFY(!worker.send(ProtocolMessage::visibilityChanged(true).value()));
    QCOMPARE(worker.lastErrorCode(), QStringLiteral("ipc.session.authentication_required"));

    QVERIFY(worker.send(*ProtocolMessage::handshake(QStringLiteral("visibility-nonce"))));
    QCOMPARE(host.receive(1000).status, SessionStatus::MessageReady);
    QCOMPARE(worker.receive(1000).status, SessionStatus::MessageReady);

    QVERIFY(host.sendVisibilityChanged(true));
    const SessionReceiveResult active = worker.receive(1000);
    QCOMPARE(active.status, SessionStatus::MessageReady);
    QVERIFY(active.message.has_value());
    QCOMPARE(active.message->type(), ProtocolType::VisibilityChanged);
    QCOMPARE(active.message->payload().value(QStringLiteral("active")).toBool(), true);

    QVERIFY(!worker.send(ProtocolMessage::visibilityChanged(false).value()));
    QCOMPARE(worker.lastErrorCode(), QStringLiteral("ipc.session.unexpected_message_direction"));
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
