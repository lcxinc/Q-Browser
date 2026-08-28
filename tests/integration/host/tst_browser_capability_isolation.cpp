#include "AuthorityAdmissionToken.h"
#include "FileBroker.h"
#include "FileDialogCoordinator.h"
#include "FileDialogTestHooks.h"
#include "HostCapabilityRuntime.h"
#include "HostGestureRouter.h"
#include "HostWorkerSessionController.h"
#include "IpcSession.h"
#include "WorkerRetirementManager.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

static_assert(!std::is_default_constructible_v<FileDialogOperationToken>);
static_assert(!std::is_constructible_v<FileDialogOperationToken, quint64>);
static_assert(std::is_copy_constructible_v<FileDialogOperationToken>);
static_assert(!std::is_default_constructible_v<FileDialogOpenRequest>);

namespace {
PreparedFileRequestResult prepareOpen(FileBroker &broker,
                                      const QString &requestId)
{
    return broker.prepareFileRequest(
        QStringLiteral("open"), {},
        {QStringLiteral("com.qbrowser.owner"), requestId});
}

void writeFile(const QString &path, const QByteArray &content)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(content), content.size());
    file.close();
}

class CompletionProbe final
{
public:
    FileDialogCompletion callback()
    {
        return [this](const FileDialogOperationToken &token,
                      FileDialogSelection selection) {
            {
                const std::scoped_lock lock(mutex_);
                tokens_.push_back(token);
                selections_.push_back(std::move(selection));
                callbackThreads_.push_back(std::this_thread::get_id());
            }
            calls_.fetch_add(1, std::memory_order_release);
            called_.release();
        };
    }

    [[nodiscard]] bool wait(const int timeoutMs = 1'000)
    {
        return called_.tryAcquire(1, timeoutMs);
    }

    [[nodiscard]] int calls() const noexcept
    {
        return calls_.load(std::memory_order_acquire);
    }

    [[nodiscard]] FileDialogSelection selection(const qsizetype index = 0) const
    {
        const std::scoped_lock lock(mutex_);
        return selections_.at(index);
    }

    [[nodiscard]] FileDialogOperationToken token(const qsizetype index = 0) const
    {
        const std::scoped_lock lock(mutex_);
        return tokens_.at(index);
    }

    [[nodiscard]] std::thread::id callbackThread(
        const qsizetype index = 0) const
    {
        const std::scoped_lock lock(mutex_);
        return callbackThreads_.at(index);
    }

private:
    mutable std::mutex mutex_;
    std::vector<FileDialogOperationToken> tokens_;
    std::vector<FileDialogSelection> selections_;
    std::vector<std::thread::id> callbackThreads_;
    std::atomic<int> calls_{0};
    QSemaphore called_;
};

qbrowser_broker_testing::FileDialogTestShowResult openedPath(
    const QString &path)
{
    return {FileDialogStatus::Opened, path};
}

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
        HostLaunchContext{QStringLiteral("bounded-final-send"), appIdentity});
    auto worker = std::make_unique<IpcSession>(
        WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds()),
        IpcRole::Worker);
    const auto handshake = ProtocolMessage::handshake(
        QStringLiteral("bounded-final-send"));
    if (!handshake.has_value() || !worker->send(*handshake, 1'000)
        || host->receive(1'000).status != SessionStatus::MessageReady
        || worker->receive(1'000).status != SessionStatus::MessageReady) {
        return std::nullopt;
    }
    return AuthenticatedSessions{std::move(host), std::move(worker)};
}
}

class BrowserCapabilityIsolationTest final : public QObject
{
    Q_OBJECT

private slots:
    void backgroundAdmissionRejectsBeforeDialogCreationAndNeverCallsInline();
    void processWideDialogBusyRejectsSiblingWithItsOwnCallback();
    void switchingOwnerDoesNotRebindPendingOperation();
    void ownerBoundTerminalCompletion_data();
    void ownerBoundTerminalCompletion();
    void cancellationIsExactOnceAndLateResultCannotReachReopenedOwner();
    void guiStaysResponsiveWhileShowAndReadAreBlocked();
    void oversizedStableFileIsRejectedBeforeReadOrEncode();
    void quiescenceCannotExposeANonTerminalOperation();
    void shutdownCannotLoseImmediateCompletion();
    void callbackCanRequestShutdown();
    void shutdownCancelsAllAndWaitsForCallbackQuiescence();
    void boundedFinalSendGateIsDeferred();
};

void BrowserCapabilityIsolationTest::
    backgroundAdmissionRejectsBeforeDialogCreationAndNeverCallsInline()
{
    std::atomic<int> dialogCreations{0};
    std::atomic<int> showCalls{0};
    qbrowser_broker_testing::FileDialogTestHooks hooks;
    hooks.coordinatorDialogCreated = [&] { ++dialogCreations; };
    hooks.coordinatorShow = [&](const qint64,
                                qbrowser_broker_testing::FileDialogTestShowCompletion
                                    complete) {
        ++showCalls;
        complete({FileDialogStatus::Failed, {}});
    };
    qbrowser_broker_testing::setFileDialogTestHooks(std::move(hooks));
    const auto resetHooks = qScopeGuard([] {
        qbrowser_broker_testing::resetFileDialogTestHooks();
    });

    QtFileDialogBackend legacyBackend;
    FileBroker broker(EffectiveFilePolicy{true, 8}, legacyBackend);
    PreparedFileRequestResult preparation = prepareOpen(
        broker, QStringLiteral("background"));
    QVERIFY(preparation.request.has_value());

    const std::thread::id callerThread = std::this_thread::get_id();
    CompletionProbe completion;
    FileDialogCoordinator coordinator;
    std::atomic<int> admissionChecks{0};
    const auto operation = coordinator.openAsync(
        FileDialogOpenRequest(*preparation.request, [&] {
            ++admissionChecks;
            return false;
        }),
        completion.callback());

    QVERIFY(operation.has_value());
    QVERIFY(completion.wait());
    QCOMPARE(admissionChecks.load(), 1);
    QCOMPARE(dialogCreations.load(), 0);
    QCOMPARE(showCalls.load(), 0);
    QCOMPARE(completion.calls(), 1);
    QCOMPARE(completion.selection().status, FileDialogStatus::Denied);
    QCOMPARE(completion.selection().approvedMaximumBytes, 8);
    QVERIFY(completion.token() == operation->token);
    QVERIFY(completion.callbackThread() != callerThread);
    QCOMPARE(broker.completeFileRequest(*preparation.request,
                                        completion.selection()).errorCode,
             QStringLiteral("capability.denied"));
    coordinator.shutdown();
}

void BrowserCapabilityIsolationTest::
    processWideDialogBusyRejectsSiblingWithItsOwnCallback()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString selected = QDir(directory.path()).filePath(
        QStringLiteral("owner.txt"));
    writeFile(selected, QByteArray("12345678"));

    std::mutex showMutex;
    std::vector<qbrowser_broker_testing::FileDialogTestShowCompletion>
        showCompletions;
    QSemaphore showEntered;
    std::atomic<int> dialogCreations{0};
    std::atomic<qint64> shownMaximumBytes{0};
    qbrowser_broker_testing::FileDialogTestHooks hooks;
    hooks.coordinatorDialogCreated = [&] { ++dialogCreations; };
    hooks.coordinatorShow = [&](const qint64 maximumBytes,
                                qbrowser_broker_testing::FileDialogTestShowCompletion
                                    complete) {
        shownMaximumBytes.store(maximumBytes);
        {
            const std::scoped_lock lock(showMutex);
            showCompletions.push_back(std::move(complete));
        }
        showEntered.release();
    };
    qbrowser_broker_testing::setFileDialogTestHooks(std::move(hooks));
    const auto resetHooks = qScopeGuard([] {
        qbrowser_broker_testing::resetFileDialogTestHooks();
    });

    QtFileDialogBackend ownerLegacyBackend;
    QtFileDialogBackend siblingLegacyBackend;
    FileBroker ownerBroker(EffectiveFilePolicy{true, 8}, ownerLegacyBackend);
    FileBroker siblingBroker(EffectiveFilePolicy{true, 8}, siblingLegacyBackend);
    PreparedFileRequestResult ownerPreparation = prepareOpen(
        ownerBroker, QStringLiteral("owner"));
    PreparedFileRequestResult siblingPreparation = prepareOpen(
        siblingBroker, QStringLiteral("sibling"));
    QVERIFY(ownerPreparation.request.has_value());
    QVERIFY(siblingPreparation.request.has_value());

    FileDialogCoordinator coordinator;
    CompletionProbe ownerCompletion;
    CompletionProbe siblingCompletion;
    const auto ownerOperation = coordinator.openAsync(
        FileDialogOpenRequest(*ownerPreparation.request, [] { return true; }),
        ownerCompletion.callback());
    QVERIFY(ownerOperation.has_value());
    QVERIFY(showEntered.tryAcquire(1, 1'000));
    QCOMPARE(shownMaximumBytes.load(), 8);

    const auto siblingOperation = coordinator.openAsync(
        FileDialogOpenRequest(*siblingPreparation.request, [] { return true; }),
        siblingCompletion.callback());
    QVERIFY(siblingOperation.has_value());
    QVERIFY(siblingCompletion.wait());
    QCOMPARE(siblingCompletion.calls(), 1);
    QCOMPARE(siblingCompletion.selection().status, FileDialogStatus::Busy);
    QVERIFY(siblingCompletion.token() == siblingOperation->token);
    QCOMPARE(dialogCreations.load(), 1);
    QCOMPARE(siblingBroker.completeFileRequest(*siblingPreparation.request,
                                               siblingCompletion.selection())
                 .errorCode,
             QStringLiteral("file.busy"));
    QCOMPARE(ownerCompletion.calls(), 0);

    qbrowser_broker_testing::FileDialogTestShowCompletion completeOwner;
    {
        const std::scoped_lock lock(showMutex);
        QCOMPARE(showCompletions.size(), size_t{1});
        completeOwner = showCompletions.front();
    }
    completeOwner(openedPath(selected));
    QVERIFY(ownerCompletion.wait());
    QCOMPARE(ownerCompletion.selection().status, FileDialogStatus::Opened);
    QVERIFY(ownerBroker.completeFileRequest(*ownerPreparation.request,
                                           ownerCompletion.selection()).ok);
    coordinator.shutdown();
}

void BrowserCapabilityIsolationTest::
    switchingOwnerDoesNotRebindPendingOperation()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString selected = QDir(directory.path()).filePath(
        QStringLiteral("original-owner.txt"));
    writeFile(selected, QByteArray("bound"));

    std::mutex showMutex;
    qbrowser_broker_testing::FileDialogTestShowCompletion finishShow;
    QSemaphore showEntered;
    qbrowser_broker_testing::FileDialogTestHooks hooks;
    hooks.coordinatorShow = [&](const qint64,
                                qbrowser_broker_testing::FileDialogTestShowCompletion
                                    complete) {
        {
            const std::scoped_lock lock(showMutex);
            finishShow = std::move(complete);
        }
        showEntered.release();
    };
    qbrowser_broker_testing::setFileDialogTestHooks(std::move(hooks));
    const auto resetHooks = qScopeGuard([] {
        qbrowser_broker_testing::resetFileDialogTestHooks();
    });

    QtFileDialogBackend legacyBackend;
    FileBroker broker(EffectiveFilePolicy{true, 8}, legacyBackend);
    PreparedFileRequestResult preparation = prepareOpen(
        broker, QStringLiteral("switch-owner"));
    QVERIFY(preparation.request.has_value());

    FileDialogCoordinator coordinator;
    CompletionProbe originalOwner;
    CompletionProbe newlyActiveOwner;
    std::atomic<bool> originalIsOwner{true};
    std::atomic<int> admissionChecks{0};
    const auto operation = coordinator.openAsync(
        FileDialogOpenRequest(*preparation.request, [&] {
            ++admissionChecks;
            return originalIsOwner.load();
        }),
        originalOwner.callback());
    QVERIFY(operation.has_value());
    QVERIFY(showEntered.tryAcquire(1, 1'000));

    originalIsOwner.store(false);
    qbrowser_broker_testing::FileDialogTestShowCompletion complete;
    {
        const std::scoped_lock lock(showMutex);
        complete = finishShow;
    }
    complete(openedPath(selected));

    QVERIFY(originalOwner.wait());
    QCOMPARE(admissionChecks.load(), 1);
    QCOMPARE(originalOwner.calls(), 1);
    QCOMPARE(newlyActiveOwner.calls(), 0);
    QVERIFY(originalOwner.token() == operation->token);
    coordinator.shutdown();
}

void BrowserCapabilityIsolationTest::ownerBoundTerminalCompletion_data()
{
    QTest::addColumn<int>("terminalStatus");
    QTest::addColumn<QString>("expectedError");
    QTest::newRow("success") << static_cast<int>(FileDialogStatus::Opened)
                             << QString();
    QTest::newRow("cancelled") << static_cast<int>(FileDialogStatus::Cancelled)
                               << QStringLiteral("file.cancelled");
    QTest::newRow("failed") << static_cast<int>(FileDialogStatus::Failed)
                            << QStringLiteral("file.failed");
}

void BrowserCapabilityIsolationTest::ownerBoundTerminalCompletion()
{
    QFETCH(int, terminalStatus);
    QFETCH(QString, expectedError);
    const auto status = static_cast<FileDialogStatus>(terminalStatus);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString selected = QDir(directory.path()).filePath(
        QStringLiteral("terminal.txt"));
    writeFile(selected, QByteArray("done"));

    std::thread::id showThread;
    qbrowser_broker_testing::FileDialogTestHooks hooks;
    hooks.coordinatorShow = [&](const qint64,
                                qbrowser_broker_testing::FileDialogTestShowCompletion
                                    complete) {
        showThread = std::this_thread::get_id();
        complete({status,
                  status == FileDialogStatus::Opened ? selected : QString()});
    };
    qbrowser_broker_testing::setFileDialogTestHooks(std::move(hooks));
    const auto resetHooks = qScopeGuard([] {
        qbrowser_broker_testing::resetFileDialogTestHooks();
    });

    QtFileDialogBackend legacyBackend;
    FileBroker broker(EffectiveFilePolicy{true, 8}, legacyBackend);
    PreparedFileRequestResult preparation = prepareOpen(
        broker, QStringLiteral("terminal"));
    QVERIFY(preparation.request.has_value());

    const std::thread::id callerThread = std::this_thread::get_id();
    CompletionProbe completion;
    FileDialogCoordinator coordinator;
    const auto operation = coordinator.openAsync(
        FileDialogOpenRequest(*preparation.request, [] { return true; }),
        completion.callback());
    QVERIFY(operation.has_value());
    QVERIFY(completion.wait());
    QCOMPARE(completion.calls(), 1);
    QCOMPARE(completion.selection().status, status);
    QVERIFY(showThread != callerThread);
    QVERIFY(completion.callbackThread() != callerThread);
    const BrokerResult result = broker.completeFileRequest(
        *preparation.request, completion.selection());
    if (expectedError.isEmpty()) {
        QVERIFY(result.ok);
        QCOMPARE(completion.selection().name, QStringLiteral("terminal.txt"));
        QCOMPARE(completion.selection().declaredSize, 4);
#ifdef Q_OS_WIN
        QVERIFY(completion.selection().identityBeforeRead.startsWith(
            QByteArray("win-id128:")));
#else
        QVERIFY(!completion.selection().identityBeforeRead.isEmpty());
#endif
        QCOMPARE(completion.selection().identityBeforeRead,
                 completion.selection().identityAfterRead);
        QCOMPARE(QByteArray::fromBase64(
                     result.value.value(QStringLiteral("contentBase64"))
                         .toString().toLatin1()),
                 QByteArray("done"));
    } else {
        QVERIFY(!result.ok);
        QCOMPARE(result.errorCode, expectedError);
    }
    coordinator.shutdown();
}

void BrowserCapabilityIsolationTest::
    oversizedStableFileIsRejectedBeforeReadOrEncode()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString selected = QDir(directory.path()).filePath(
        QStringLiteral("oversized.txt"));
    writeFile(selected, QByteArray("123456789"));

    std::atomic<int> readCalls{0};
    std::atomic<int> encodeCalls{0};
    qbrowser_broker_testing::FileDialogTestHooks hooks;
    hooks.coordinatorShow = [selected](
                                const qint64,
                                qbrowser_broker_testing::FileDialogTestShowCompletion
                                    complete) {
        complete(openedPath(selected));
    };
    hooks.coordinatorBeforeRead = [&](const qint64) { ++readCalls; };
    hooks.coordinatorBeforeEncode = [&] { ++encodeCalls; };
    qbrowser_broker_testing::setFileDialogTestHooks(std::move(hooks));
    const auto resetHooks = qScopeGuard([] {
        qbrowser_broker_testing::resetFileDialogTestHooks();
    });

    QtFileDialogBackend legacyBackend;
    FileBroker broker(EffectiveFilePolicy{true, 8}, legacyBackend);
    PreparedFileRequestResult preparation = prepareOpen(
        broker, QStringLiteral("oversized"));
    QVERIFY(preparation.request.has_value());

    FileDialogCoordinator coordinator;
    CompletionProbe completion;
    const auto operation = coordinator.openAsync(
        FileDialogOpenRequest(*preparation.request, [] { return true; }),
        completion.callback());
    QVERIFY(operation.has_value());
    QVERIFY(completion.wait());
    QCOMPARE(completion.selection().status, FileDialogStatus::TooLarge);
    QCOMPARE(readCalls.load(), 0);
    QCOMPARE(encodeCalls.load(), 0);
    QCOMPARE(broker.completeFileRequest(*preparation.request,
                                        completion.selection()).errorCode,
             QStringLiteral("file.too_large"));
    coordinator.shutdown();
}

void BrowserCapabilityIsolationTest::
    cancellationIsExactOnceAndLateResultCannotReachReopenedOwner()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString oldPath = QDir(directory.path()).filePath(
        QStringLiteral("old-owner.txt"));
    const QString reopenedPath = QDir(directory.path()).filePath(
        QStringLiteral("reopened-owner.txt"));
    writeFile(oldPath, QByteArray("old"));
    writeFile(reopenedPath, QByteArray("new"));

    std::mutex showMutex;
    std::vector<qbrowser_broker_testing::FileDialogTestShowCompletion>
        showCompletions;
    QSemaphore showEntered;
    QSemaphore cancelObserved;
    QSemaphore allowCancelDelivery;
    QSemaphore operationQuiesced;
    std::atomic<int> cancelCalls{0};
    qbrowser_broker_testing::FileDialogTestHooks hooks;
    hooks.coordinatorShow = [&](const qint64,
                                qbrowser_broker_testing::FileDialogTestShowCompletion
                                    complete) {
        {
            const std::scoped_lock lock(showMutex);
            showCompletions.push_back(std::move(complete));
        }
        showEntered.release();
    };
    hooks.coordinatorCancel = [&] {
        ++cancelCalls;
        cancelObserved.release();
        allowCancelDelivery.acquire();
    };
    hooks.coordinatorOperationQuiesced = [&] { operationQuiesced.release(); };
    qbrowser_broker_testing::setFileDialogTestHooks(std::move(hooks));
    const auto resetHooks = qScopeGuard([] {
        qbrowser_broker_testing::resetFileDialogTestHooks();
    });
    QtFileDialogBackend legacyBackend;
    FileBroker broker(EffectiveFilePolicy{true, 8}, legacyBackend);
    PreparedFileRequestResult oldPreparation = prepareOpen(
        broker, QStringLiteral("old-operation"));
    PreparedFileRequestResult reopenedPreparation = prepareOpen(
        broker, QStringLiteral("reopened-operation"));
    QVERIFY(oldPreparation.request.has_value());
    QVERIFY(reopenedPreparation.request.has_value());

    FileDialogCoordinator coordinator;
    const auto unblockCancellation = qScopeGuard([&] {
        allowCancelDelivery.release();
    });
    CompletionProbe oldCompletion;
    CompletionProbe reopenedCompletion;
    const auto oldOperation = coordinator.openAsync(
        FileDialogOpenRequest(*oldPreparation.request, [] { return true; }),
        oldCompletion.callback());
    QVERIFY(oldOperation.has_value());
    QVERIFY(showEntered.tryAcquire(1, 1'000));

    QVERIFY(oldOperation->cancellation.cancel());
    QVERIFY(!oldOperation->cancellation.cancel());
    QVERIFY(!coordinator.cancel(oldOperation->token));
    QVERIFY(oldCompletion.wait());
    QCOMPARE(oldCompletion.calls(), 1);
    QCOMPARE(oldCompletion.selection().status, FileDialogStatus::Cancelled);
    QVERIFY(cancelObserved.tryAcquire(1, 1'000));
    QCOMPARE(cancelCalls.load(), 1);

    CompletionProbe whileClosingCompletion;
    const auto whileClosingOperation = coordinator.openAsync(
        FileDialogOpenRequest(*reopenedPreparation.request,
                              [] { return true; }),
        whileClosingCompletion.callback());
    QVERIFY(whileClosingOperation.has_value());
    QVERIFY(whileClosingCompletion.wait());
    QCOMPARE(whileClosingCompletion.selection().status,
             FileDialogStatus::Busy);
    {
        const std::scoped_lock lock(showMutex);
        QCOMPARE(showCompletions.size(), size_t{1});
    }

    allowCancelDelivery.release();
    QVERIFY(operationQuiesced.tryAcquire(1, 1'000));

    const auto reopenedOperation = coordinator.openAsync(
        FileDialogOpenRequest(*reopenedPreparation.request,
                              [] { return true; }),
        reopenedCompletion.callback());
    QVERIFY(reopenedOperation.has_value());
    QVERIFY(showEntered.tryAcquire(1, 1'000));

    qbrowser_broker_testing::FileDialogTestShowCompletion lateOld;
    qbrowser_broker_testing::FileDialogTestShowCompletion completeReopened;
    {
        const std::scoped_lock lock(showMutex);
        QCOMPARE(showCompletions.size(), size_t{2});
        lateOld = showCompletions.at(0);
        completeReopened = showCompletions.at(1);
    }
    lateOld(openedPath(oldPath));
    completeReopened(openedPath(reopenedPath));
    QVERIFY(reopenedCompletion.wait());
    QCOMPARE(reopenedCompletion.calls(), 1);
    QCOMPARE(reopenedCompletion.selection().status, FileDialogStatus::Opened);
    QVERIFY(reopenedCompletion.token() == reopenedOperation->token);
    QCOMPARE(oldCompletion.calls(), 1);
    QCOMPARE(cancelCalls.load(), 1);
    const BrokerResult reopened = broker.completeFileRequest(
        *reopenedPreparation.request, reopenedCompletion.selection());
    QVERIFY(reopened.ok);
    QCOMPARE(QByteArray::fromBase64(
                 reopened.value.value(QStringLiteral("contentBase64"))
                     .toString().toLatin1()),
             QByteArray("new"));
    coordinator.shutdown();
}

void BrowserCapabilityIsolationTest::
    guiStaysResponsiveWhileShowAndReadAreBlocked()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString selected = QDir(directory.path()).filePath(
        QStringLiteral("responsive.txt"));
    writeFile(selected, QByteArray("12345678"));

    std::mutex showMutex;
    qbrowser_broker_testing::FileDialogTestShowCompletion finishShow;
    QSemaphore showEntered;
    QSemaphore readEntered;
    QSemaphore allowRead;
    QSemaphore encodeObserved;
    std::atomic<qint64> readCapacity{0};
    std::thread::id showThread;
    std::thread::id readThread;
    std::thread::id encodeThread;
    qbrowser_broker_testing::FileDialogTestHooks hooks;
    hooks.coordinatorShow = [&](const qint64,
                                qbrowser_broker_testing::FileDialogTestShowCompletion
                                    complete) {
        showThread = std::this_thread::get_id();
        {
            const std::scoped_lock lock(showMutex);
            finishShow = std::move(complete);
        }
        showEntered.release();
    };
    hooks.coordinatorBeforeRead = [&](const qint64 capacity) {
        readThread = std::this_thread::get_id();
        readCapacity.store(capacity);
        readEntered.release();
        allowRead.acquire();
    };
    hooks.coordinatorBeforeEncode = [&] {
        encodeThread = std::this_thread::get_id();
        encodeObserved.release();
    };
    qbrowser_broker_testing::setFileDialogTestHooks(std::move(hooks));
    const auto resetHooks = qScopeGuard([] {
        qbrowser_broker_testing::resetFileDialogTestHooks();
    });

    QtFileDialogBackend legacyBackend;
    FileBroker broker(EffectiveFilePolicy{true, 8}, legacyBackend);
    PreparedFileRequestResult preparation = prepareOpen(
        broker, QStringLiteral("responsive"));
    QVERIFY(preparation.request.has_value());

    FileDialogCoordinator coordinator;
    const auto unblockRead = qScopeGuard([&] { allowRead.release(); });
    CompletionProbe completion;
    const auto operation = coordinator.openAsync(
        FileDialogOpenRequest(*preparation.request, [] { return true; }),
        completion.callback());
    QVERIFY(operation.has_value());
    QVERIFY(showEntered.tryAcquire(1, 1'000));

    int guiTicks = 0;
    QTimer timer;
    connect(&timer, &QTimer::timeout, this, [&] { ++guiTicks; });
    timer.start(1);
    QTRY_VERIFY_WITH_TIMEOUT(guiTicks >= 3, 500);
    QCOMPARE(completion.calls(), 0);

    qbrowser_broker_testing::FileDialogTestShowCompletion complete;
    {
        const std::scoped_lock lock(showMutex);
        complete = finishShow;
    }
    complete(openedPath(selected));
    QVERIFY(readEntered.tryAcquire(1, 1'000));
    const int ticksBeforeBlockedRead = guiTicks;
    QTRY_VERIFY_WITH_TIMEOUT(guiTicks >= ticksBeforeBlockedRead + 3, 500);
    QCOMPARE(completion.calls(), 0);

    allowRead.release();
    QVERIFY(encodeObserved.tryAcquire(1, 1'000));
    QVERIFY(completion.wait());
    QCOMPARE(completion.selection().status, FileDialogStatus::Opened);
    QCOMPARE(completion.selection().declaredSize, 8);
    QCOMPARE(readCapacity.load(), 9);
    const std::thread::id callerThread = std::this_thread::get_id();
    QVERIFY(showThread != callerThread);
    QVERIFY(showThread == readThread);
    QVERIFY(readThread == encodeThread);
    QVERIFY(broker.completeFileRequest(*preparation.request,
                                      completion.selection()).ok);
    coordinator.shutdown();
}

void BrowserCapabilityIsolationTest::
    quiescenceCannotExposeANonTerminalOperation()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString selected = QDir(directory.path()).filePath(
        QStringLiteral("quiesced.txt"));
    writeFile(selected, QByteArray("done"));

    std::mutex showMutex;
    qbrowser_broker_testing::FileDialogTestShowCompletion finishShow;
    QSemaphore showEntered;
    QSemaphore quiescenceEntered;
    QSemaphore allowQuiescenceReturn;
    qbrowser_broker_testing::FileDialogTestHooks hooks;
    hooks.coordinatorShow = [&](
                                const qint64,
                                qbrowser_broker_testing::FileDialogTestShowCompletion
                                    complete) {
        {
            const std::scoped_lock lock(showMutex);
            finishShow = std::move(complete);
        }
        showEntered.release();
    };
    hooks.coordinatorOperationQuiesced = [&] {
        quiescenceEntered.release();
        allowQuiescenceReturn.acquire();
    };
    qbrowser_broker_testing::setFileDialogTestHooks(std::move(hooks));
    const auto resetHooks = qScopeGuard([] {
        qbrowser_broker_testing::resetFileDialogTestHooks();
    });

    QtFileDialogBackend legacyBackend;
    FileBroker broker(EffectiveFilePolicy{true, 8}, legacyBackend);
    PreparedFileRequestResult preparation = prepareOpen(
        broker, QStringLiteral("quiescence-terminal-order"));
    QVERIFY(preparation.request.has_value());

    FileDialogCoordinator coordinator;
    const auto unblockQuiescence = qScopeGuard([&] {
        allowQuiescenceReturn.release();
    });
    CompletionProbe completion;
    const auto operation = coordinator.openAsync(
        FileDialogOpenRequest(*preparation.request, [] { return true; }),
        completion.callback());
    QVERIFY(operation.has_value());
    QVERIFY(showEntered.tryAcquire(1, 1'000));

    qbrowser_broker_testing::FileDialogTestShowCompletion complete;
    {
        const std::scoped_lock lock(showMutex);
        complete = finishShow;
    }
    complete(openedPath(selected));
    QVERIFY(quiescenceEntered.tryAcquire(1, 1'000));

    QVERIFY2(completion.wait(200),
             "the old operation was still non-terminal after its physical "
             "reservation became observable as quiesced");
    QCOMPARE(completion.calls(), 1);
    QCOMPARE(completion.selection().status, FileDialogStatus::Opened);
    allowQuiescenceReturn.release();
    coordinator.shutdown();
}

void BrowserCapabilityIsolationTest::
    shutdownCannotLoseImmediateCompletion()
{
    QSemaphore immediateEntered;
    QSemaphore allowImmediateCompletion;
    qbrowser_broker_testing::FileDialogTestHooks hooks;
    hooks.coordinatorBeforeImmediateCompletion = [&] {
        immediateEntered.release();
        allowImmediateCompletion.acquire();
    };
    qbrowser_broker_testing::setFileDialogTestHooks(std::move(hooks));
    const auto resetHooks = qScopeGuard([] {
        qbrowser_broker_testing::resetFileDialogTestHooks();
    });
    QtFileDialogBackend legacyBackend;
    FileBroker broker(EffectiveFilePolicy{true, 8}, legacyBackend);
    PreparedFileRequestResult preparation = prepareOpen(
        broker, QStringLiteral("immediate-shutdown-race"));
    QVERIFY(preparation.request.has_value());

    FileDialogCoordinator coordinator;
    const auto unblockImmediate = qScopeGuard([&] {
        allowImmediateCompletion.release();
    });
    CompletionProbe completion;
    auto opening = std::async(std::launch::async, [&] {
        return coordinator.openAsync(
            FileDialogOpenRequest(*preparation.request, [] { return false; }),
            completion.callback());
    });
    QVERIFY(immediateEntered.tryAcquire(1, 1'000));

    auto stopping = std::async(std::launch::async, [&] {
        coordinator.shutdown();
    });
    const bool shutdownReturnedBeforeEnqueue =
        stopping.wait_for(std::chrono::milliseconds(50))
        == std::future_status::ready;
    allowImmediateCompletion.release();

    const auto operation = opening.get();
    QCOMPARE(stopping.wait_for(std::chrono::seconds(1)),
             std::future_status::ready);
    stopping.get();
    const bool callbackArrived = completion.wait(200);

    QVERIFY2(!shutdownReturnedBeforeEnqueue,
             "shutdown returned while an admitted immediate callback had not "
             "yet been queued");
    QVERIFY(callbackArrived);
    QVERIFY(operation.has_value());
    QCOMPARE(completion.calls(), 1);
    QCOMPARE(completion.selection().status, FileDialogStatus::Denied);
}

void BrowserCapabilityIsolationTest::callbackCanRequestShutdown()
{
    QtFileDialogBackend legacyBackend;
    FileBroker broker(EffectiveFilePolicy{true, 8}, legacyBackend);
    PreparedFileRequestResult preparation = prepareOpen(
        broker, QStringLiteral("callback-shutdown"));
    QVERIFY(preparation.request.has_value());

    FileDialogCoordinator coordinator;
    QSemaphore callbackReturned;
    std::atomic<int> callbackCalls{0};
    std::atomic<int> callbackStatus{
        static_cast<int>(FileDialogStatus::Failed)};
    const auto operation = coordinator.openAsync(
        FileDialogOpenRequest(*preparation.request, [] { return false; }),
        [&](const FileDialogOperationToken &, FileDialogSelection selection) {
            callbackStatus.store(static_cast<int>(selection.status));
            ++callbackCalls;
            coordinator.shutdown();
            callbackReturned.release();
        });

    QVERIFY(operation.has_value());
    QVERIFY(callbackReturned.tryAcquire(1, 1'000));
    QCOMPARE(callbackCalls.load(), 1);
    QCOMPARE(static_cast<FileDialogStatus>(callbackStatus.load()),
             FileDialogStatus::Denied);
    coordinator.shutdown();
}

void BrowserCapabilityIsolationTest::
    shutdownCancelsAllAndWaitsForCallbackQuiescence()
{
    QSemaphore showEntered;
    QSemaphore cancelObserved;
    QSemaphore callbackEntered;
    QSemaphore allowCallbackReturn;
    std::atomic<int> cancelCalls{0};
    std::atomic<int> callbackCalls{0};
    std::atomic<int> callbackStatus{
        static_cast<int>(FileDialogStatus::Failed)};
    qbrowser_broker_testing::FileDialogTestHooks hooks;
    hooks.coordinatorShow = [&](
                                const qint64,
                                qbrowser_broker_testing::FileDialogTestShowCompletion) {
        showEntered.release();
    };
    hooks.coordinatorCancel = [&] {
        ++cancelCalls;
        cancelObserved.release();
    };
    qbrowser_broker_testing::setFileDialogTestHooks(std::move(hooks));
    const auto resetHooks = qScopeGuard([] {
        qbrowser_broker_testing::resetFileDialogTestHooks();
    });

    QtFileDialogBackend legacyBackend;
    FileBroker broker(EffectiveFilePolicy{true, 8}, legacyBackend);
    PreparedFileRequestResult preparation = prepareOpen(
        broker, QStringLiteral("shutdown"));
    QVERIFY(preparation.request.has_value());

    FileDialogCoordinator coordinator;
    const auto unblockCallback = qScopeGuard([&] {
        allowCallbackReturn.release();
    });
    const auto operation = coordinator.openAsync(
        FileDialogOpenRequest(*preparation.request, [] { return true; }),
        [&](const FileDialogOperationToken &, FileDialogSelection selection) {
            callbackStatus.store(static_cast<int>(selection.status));
            ++callbackCalls;
            callbackEntered.release();
            allowCallbackReturn.acquire();
        });
    QVERIFY(operation.has_value());
    QVERIFY(showEntered.tryAcquire(1, 1'000));

    auto shutdown = std::async(std::launch::async, [&] {
        coordinator.shutdown();
    });
    const auto unblockCallbackBeforeFutureJoin = qScopeGuard([&] {
        allowCallbackReturn.release();
    });
    QVERIFY(cancelObserved.tryAcquire(1, 1'000));
    QVERIFY(callbackEntered.tryAcquire(1, 1'000));
    QCOMPARE(shutdown.wait_for(std::chrono::milliseconds(20)),
             std::future_status::timeout);
    allowCallbackReturn.release();
    QCOMPARE(shutdown.wait_for(std::chrono::seconds(1)),
             std::future_status::ready);
    shutdown.get();
    QCOMPARE(cancelCalls.load(), 1);
    QCOMPARE(callbackCalls.load(), 1);
    QCOMPARE(static_cast<FileDialogStatus>(callbackStatus.load()),
             FileDialogStatus::Cancelled);
}

void BrowserCapabilityIsolationTest::
    boundedFinalSendGateIsDeferred()
{
    const QString appIdentity = QStringLiteral("com.qbrowser.final-send");
    auto sessions = authenticatedSessions(appIdentity);
    QVERIFY(sessions.has_value());
    HostWorkerSessionController controller(
        [](const QString &, const QString &) { return true; });
    QTemporaryDir storage;
    QVERIFY(storage.isValid());
    auto router = HostGestureRouter::createForTesting(1);
    QVERIFY(router != nullptr);
    ManifestPermissions permissions;
    permissions.storage = StoragePermission::AppPrivate;
    const TabCapabilityAuthority authority{
        QStringLiteral("tab-final-send"), 1, appIdentity, 91, 901,
        controller.generation() + 1, 17};
    auto admission = std::make_shared<AuthorityAdmissionToken>();
    QString errorCode;
    auto runtime = HostCapabilityRuntime::create(
        authority, admission, router.get(), permissions,
        QUrl(QStringLiteral("http://127.0.0.1:32191/")), storage.path(),
        1, &errorCode, nullptr);
    QVERIFY2(runtime != nullptr, qPrintable(errorCode));
    const auto cleanup = qScopeGuard([&] {
        controller.unbindCapabilityRuntime(authority);
        HostCapabilityRuntime::retire(std::exchange(runtime, {}));
        if (sessions.has_value() && sessions->worker != nullptr) {
            sessions->worker->close();
        }
        (void)WorkerRetirementManager::instance().flush(10'000);
    });

    QVERIFY(controller.attach(std::move(sessions->host), runtime.get()));
    QSignalSpy responseQueued(
        &controller, &HostWorkerSessionController::capabilityResponseQueued);
    QSignalSpy responseSent(
        &controller, &HostWorkerSessionController::capabilityResponseSent);
    QSignalSpy requests(
        &controller, &HostWorkerSessionController::capabilityRequestObserved);
    QSignalSpy heartbeatObserved(
        &controller, &HostWorkerSessionController::heartbeatObserved);
    QVERIFY(responseQueued.isValid());
    QVERIFY(responseSent.isValid());
    QVERIFY(requests.isValid());
    QVERIFY(heartbeatObserved.isValid());

    constexpr int routeCount = 40;
    for (int index = 0; index < routeCount; ++index) {
        const QString route = QStringLiteral("/")
            + QString(2'030, u'r') + QString::number(index);
        QVERIFY(controller.requestRouteLoad(route));
    }
    QTest::qWait(50);

    QVERIFY(sessions->worker->sendRequest(
        QStringLiteral("final-send-request"), QStringLiteral("storage"),
        QStringLiteral("get"),
        QJsonObject{{QStringLiteral("key"), QStringLiteral("bounded")}},
        10'000));
    QVERIFY(sessions->worker->send(ProtocolMessage::heartbeat(), 1'000));
    QTRY_COMPARE_WITH_TIMEOUT(responseQueued.count(), 1, 3'000);
    QTRY_VERIFY_WITH_TIMEOUT(heartbeatObserved.count() > 0, 3'000);
    QVERIFY(sessions->worker->sendRequest(
        QStringLiteral("final-send-busy"), QStringLiteral("storage"),
        QStringLiteral("get"),
        QJsonObject{{QStringLiteral("key"), QStringLiteral("second")}},
        10'000));
    QTRY_COMPARE_WITH_TIMEOUT(requests.count(), 2, 3'000);
    QTRY_COMPARE_WITH_TIMEOUT(responseQueued.count(), 2, 3'000);
    QCOMPARE(responseQueued.at(1).at(0).toString(),
             QStringLiteral("final-send-busy"));
    QCOMPARE(responseQueued.at(1).at(2).toString(),
             QStringLiteral("capability.busy"));
    QCOMPARE(controller.state(), HostWorkerSessionState::Running);

    const AuthorityAdmissionToken::RevocationTicket revocation =
        admission->beginRevoke();
    QVERIFY2(!revocation.isDrained(),
             "UseGuard was released before transport terminal completion");

    int receivedRoutes = 0;
    bool firstResponseObserved = false;
    bool busyResponseObserved = false;
    bool drainObserved = false;
    QElapsedTimer drainTimer;
    QElapsedTimer afterDrainTimer;
    drainTimer.start();
    while (drainTimer.elapsed() < 3'000
           && (receivedRoutes != routeCount || !drainObserved
               || afterDrainTimer.elapsed() < 250)) {
        QCoreApplication::processEvents();
        const SessionReceiveResult received = sessions->worker->poll(0);
        if (received.status == SessionStatus::MessageReady
            && received.message.has_value()) {
            if (received.message->type() == ProtocolType::RouteLoad) {
                ++receivedRoutes;
            } else if (received.message->type() == ProtocolType::Response
                       && received.message->requestId()
                           == QStringLiteral("final-send-request")) {
                firstResponseObserved = true;
            } else if (received.message->type() == ProtocolType::Response
                       && received.message->requestId()
                           == QStringLiteral("final-send-busy")) {
                busyResponseObserved = true;
            }
        }
        if (!drainObserved && revocation.isDrained()) {
            drainObserved = true;
            afterDrainTimer.start();
        }
        QTest::qWait(1);
    }

    QCOMPARE(receivedRoutes, routeCount);
    QVERIFY(drainObserved);
    QTRY_COMPARE_WITH_TIMEOUT(controller.pendingCapabilityCount(), qsizetype(0),
                              1'000);
    QTRY_VERIFY_WITH_TIMEOUT(revocation.isDrained(), 1'000);
    QCOMPARE(responseSent.count(), 0);
    QVERIFY(!firstResponseObserved);
    QVERIFY(!busyResponseObserved);
}

QTEST_GUILESS_MAIN(BrowserCapabilityIsolationTest)

#include "tst_browser_capability_isolation.moc"
