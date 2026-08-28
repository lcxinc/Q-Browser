#include "AuthorityAdmissionToken.h"
#include "BrowserCommand.h"
#include "FileDialogCoordinator.h"
#include "FileDialogTestHooks.h"
#include "HostApplication.h"
#include "HostCapabilityRuntime.h"
#include "HostGestureRouter.h"
#include "MainWindow.h"
#include "TabCapabilityAuthority.h"
#include "WorkerRetirementManager.h"

#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QGuiApplication>
#include <QHostAddress>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <chrono>
#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

namespace {
ManifestPermissions networkPermission(const quint16 port)
{
    Q_UNUSED(port)
    ManifestPermissions permissions;
    permissions.network.hosts = {QStringLiteral("127.0.0.1")};
    permissions.network.methods = {QStringLiteral("GET")};
    return permissions;
}

ManifestPermissions clipboardPermission()
{
    ManifestPermissions permissions;
    permissions.clipboardRead = ClipboardReadPermission::UserGesture;
    return permissions;
}

ManifestPermissions clipboardAndStoragePermissions()
{
    ManifestPermissions permissions = clipboardPermission();
    permissions.storage = StoragePermission::AppPrivate;
    return permissions;
}

ManifestPermissions filePermission()
{
    ManifestPermissions permissions;
    permissions.fileOpen = FileOpenPermission::UserBrokered;
    return permissions;
}

TabCapabilityAuthority authority(
    const QString &tabId,
    const quint64 runtimeIncarnation,
    const quint32 workerProcessId,
    const quintptr workerWindowId,
    const quint64 sessionGeneration,
    const quint64 leaseAuthorityEpoch,
    const QString &appIdentity = QStringLiteral("com.qbrowser.same"))
{
    return {tabId, runtimeIncarnation, appIdentity, workerProcessId,
            workerWindowId, sessionGeneration, leaseAuthorityEpoch};
}

HostGestureSystemEvidence validEvidence(
    const TabCapabilityAuthority &binding, const quint32 now)
{
    return {now, binding.workerWindowId, binding.workerProcessId,
            binding.workerProcessId, true, true};
}

qint64 monotonicDeadlineAfter(const std::chrono::milliseconds delay)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch() + delay)
        .count();
}

QJsonObject requestPayload(const QUrl &url)
{
    return {{QStringLiteral("method"), QStringLiteral("GET")},
            {QStringLiteral("url"), url.toString(QUrl::FullyEncoded)},
            {QStringLiteral("bodyBase64"), QString{}}};
}

void writeFile(const QString &path, const QByteArray &content)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(content), content.size());
    file.close();
}
}

class HostCapabilityRuntimeTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void rejectsNonCanonicalMockOrigins_data();
    void rejectsNonCanonicalMockOrigins();
    void validatesTrustedWorkerGestureEvidence();
    void clipboardRuntimesCanCoexist();
    void backgroundFileRequestIsDeniedBeforeValidation();
    void backgroundFileRequestNeverCreatesDialog();
    void activeFileRequestCompletesThroughProcessCoordinator();
    void switchingOwnerDoesNotRebindFileCompletion();
    void revokedFileCallbackIsDroppedAndRetireCancelsExactlyOnce();
    void delayedFileCallbackIsDroppedAfterAdmissionRevocation();
    void onlyActiveAuthorityCanIssueAndConsumeGesture();
    void authorityTransitionsRevokeUnconsumedEvidence();
    void mainWindowDeactivationRevokesGestureEvidence();
    void backgroundFocusAndDispatchStateCannotAuthorize();
    void browserCommandIsSuppressedAndNeverBecomesGesture();
    void browserCommandLifecycleReentryDoesNotDeadlock();
    void staleQueuedBrowserCommandIsDropped_data();
    void staleQueuedBrowserCommandIsDropped();
    void queuedBrowserCommandCannotReviveAfterBindingRoundTrip();
    void heldBrowserChordRemainsSuppressedAcrossBindingSwitch();
    void revocationClosesUseAndPublicationUntilGuardDrains();
    void retiringRuntimeLeavesSiblingGestureStorageAndCompletionActive();
    void repeatedRetireClaimsWorkerExactlyOnce();
    void slowRequestRetiresWithoutBlockingOrLateDelivery();
};

void HostCapabilityRuntimeTest::initTestCase()
{
    qRegisterMetaType<BrokerResult>();
    qRegisterMetaType<BrowserCommand>();
    qRegisterMetaType<TabCapabilityAuthority>();
}

void HostCapabilityRuntimeTest::rejectsNonCanonicalMockOrigins_data()
{
    QTest::addColumn<QUrl>("origin");
    QTest::newRow("https") << QUrl(QStringLiteral("https://127.0.0.1:8443/"));
    QTest::newRow("hostname") << QUrl(QStringLiteral("http://localhost:8080/"));
    QTest::newRow("missing-port") << QUrl(QStringLiteral("http://127.0.0.1/"));
    QTest::newRow("path") << QUrl(QStringLiteral("http://127.0.0.1:8080/api"));
    QTest::newRow("query") << QUrl(QStringLiteral("http://127.0.0.1:8080/?x=1"));
    QTest::newRow("fragment") << QUrl(QStringLiteral("http://127.0.0.1:8080/#x"));
    QTest::newRow("userinfo") << QUrl(QStringLiteral("http://user@127.0.0.1:8080/"));
}

void HostCapabilityRuntimeTest::rejectsNonCanonicalMockOrigins()
{
    QFETCH(QUrl, origin);
    QString error;
    const auto runtime = HostCapabilityRuntime::create(
        QStringLiteral("com.qbrowser.test"), {}, origin,
        QStringLiteral("L:/not-opened-for-invalid-config"), 0, 0, 0, &error);
    QVERIFY(runtime == nullptr);
    QCOMPARE(error, QStringLiteral("host.capability.invalid_configuration"));
}

void HostCapabilityRuntimeTest::validatesTrustedWorkerGestureEvidence()
{
    HostWorkerGestureEvidence valid;
    valid.now = 5'000;
    valid.trustedWorkerInput = 4'500;
    valid.lastSystemInput = 4'900;
    valid.workerProcessId = 42;
    valid.focusProcessId = 42;
    valid.foregroundMatchesHostRoot = true;
    valid.focusBelongsToWorkerWindow = true;
    QVERIFY(qbrowser_host_testing::isTrustedWorkerGesture(valid));

    HostWorkerGestureEvidence noGesture = valid;
    noGesture.trustedWorkerInput = 0;
    QVERIFY(!qbrowser_host_testing::isTrustedWorkerGesture(noGesture));
    HostWorkerGestureEvidence unrelatedRecentInput = valid;
    unrelatedRecentInput.trustedWorkerInput = 0;
    unrelatedRecentInput.lastSystemInput = 4'999;
    QVERIFY(!qbrowser_host_testing::isTrustedWorkerGesture(
        unrelatedRecentInput));
    HostWorkerGestureEvidence expired = valid;
    expired.trustedWorkerInput = 3'999;
    QVERIFY(!qbrowser_host_testing::isTrustedWorkerGesture(expired));
    HostWorkerGestureEvidence replay = valid;
    replay.lastGrantedInput = replay.trustedWorkerInput;
    QVERIFY(!qbrowser_host_testing::isTrustedWorkerGesture(replay));
    HostWorkerGestureEvidence crossApp = valid;
    crossApp.focusProcessId = 43;
    QVERIFY(!qbrowser_host_testing::isTrustedWorkerGesture(crossApp));
    HostWorkerGestureEvidence crossWindow = valid;
    crossWindow.focusBelongsToWorkerWindow = false;
    QVERIFY(!qbrowser_host_testing::isTrustedWorkerGesture(crossWindow));
}

void HostCapabilityRuntimeTest::clipboardRuntimesCanCoexist()
{
    QTemporaryDir firstStorage;
    QTemporaryDir secondStorage;
    QVERIFY(firstStorage.isValid());
    QVERIFY(secondStorage.isValid());
    const QUrl origin(QStringLiteral("http://127.0.0.1:8080/"));
    auto router = HostGestureRouter::createForTesting(1);
    QVERIFY(router != nullptr);
    const TabCapabilityAuthority firstAuthority = authority(
        QStringLiteral("tab-a"), 1, 3, 2, 7, 11,
        QStringLiteral("com.qbrowser.first"));
    const TabCapabilityAuthority secondAuthority = authority(
        QStringLiteral("tab-b"), 2, 5, 4, 9, 13,
        QStringLiteral("com.qbrowser.second"));
    auto firstToken = std::make_shared<AuthorityAdmissionToken>();
    auto secondToken = std::make_shared<AuthorityAdmissionToken>();
    QString firstError;
    QString secondError;
    auto first = HostCapabilityRuntime::create(
        firstAuthority, firstToken, router.get(), clipboardPermission(), origin,
        firstStorage.path(), 1, &firstError, nullptr);
    auto second = HostCapabilityRuntime::create(
        secondAuthority, secondToken, router.get(), clipboardPermission(), origin,
        secondStorage.path(), 1, &secondError, nullptr);
    const bool firstCreated = first != nullptr;
    const bool secondCreated = second != nullptr;
    HostCapabilityRuntime::retire(std::exchange(first, {}));
    HostCapabilityRuntime::retire(std::exchange(second, {}));
    QVERIFY(WorkerRetirementManager::instance().flush(10'000));

    QVERIFY2(firstCreated, qPrintable(firstError));
    QVERIFY2(secondCreated, qPrintable(secondError));
}

void HostCapabilityRuntimeTest::backgroundFileRequestIsDeniedBeforeValidation()
{
    QTemporaryDir activeStorage;
    QTemporaryDir backgroundStorage;
    QVERIFY(activeStorage.isValid());
    QVERIFY(backgroundStorage.isValid());
    const QUrl origin(QStringLiteral("http://127.0.0.1:8080/"));
    auto router = HostGestureRouter::createForTesting(100);
    QVERIFY(router != nullptr);
    FileDialogCoordinator coordinator;
    const TabCapabilityAuthority activeAuthority = authority(
        QStringLiteral("tab-active"), 1, 41, 401, 7, 11);
    const TabCapabilityAuthority backgroundAuthority = authority(
        QStringLiteral("tab-background"), 2, 42, 402, 9, 12);
    auto activeToken = std::make_shared<AuthorityAdmissionToken>();
    auto backgroundToken = std::make_shared<AuthorityAdmissionToken>();
    QString activeError;
    QString backgroundError;
    auto active = HostCapabilityRuntime::create(
        activeAuthority, activeToken, router.get(), filePermission(), origin,
        activeStorage.path(), 100, &activeError, &coordinator);
    QVERIFY2(active != nullptr, qPrintable(activeError));
    auto activeCleanup = qScopeGuard([&] {
        HostCapabilityRuntime::retire(std::exchange(active, {}));
        (void)WorkerRetirementManager::instance().flush(10'000);
    });
    auto background = HostCapabilityRuntime::create(
        backgroundAuthority, backgroundToken, router.get(), filePermission(),
        origin, backgroundStorage.path(), 100, &backgroundError, &coordinator);
    QVERIFY2(background != nullptr, qPrintable(backgroundError));
    auto backgroundCleanup = qScopeGuard([&] {
        HostCapabilityRuntime::retire(std::exchange(background, {}));
        (void)WorkerRetirementManager::instance().flush(10'000);
    });
    QSignalSpy completed(background.get(),
                         &HostCapabilityRuntime::authorityCompleted);
    QVERIFY(completed.isValid());
    QVERIFY(router->activateBinding(activeAuthority));

    background->dispatch(
        backgroundAuthority.sessionGeneration,
        QStringLiteral("background-file"), QStringLiteral("file"),
        QStringLiteral("open"),
        {{QStringLiteral("path"), QStringLiteral("C:/untrusted.txt")}});
    QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 2'000);
    const QList<QVariant> completion = completed.takeFirst();
    QCOMPARE(completion.at(0).value<TabCapabilityAuthority>(),
             backgroundAuthority);
    QCOMPARE(completion.at(1).toULongLong(),
             backgroundAuthority.sessionGeneration);
    QCOMPARE(completion.at(2).toString(),
             QStringLiteral("background-file"));
    const BrokerResult result = completion.at(3).value<BrokerResult>();
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("capability.denied"));
}

void HostCapabilityRuntimeTest::backgroundFileRequestNeverCreatesDialog()
{
#ifndef Q_OS_WIN
    QSKIP("TODO(Task16 Task2): inject the portable asynchronous file backend");
#else
    int dialogCalls = 0;
    qbrowser_broker_testing::FileDialogTestHooks hooks;
    hooks.selectedPath = [&dialogCalls] {
        ++dialogCalls;
        return QString{};
    };
    qbrowser_broker_testing::setFileDialogTestHooks(std::move(hooks));
    const auto resetHooks = qScopeGuard([] {
        qbrowser_broker_testing::resetFileDialogTestHooks();
    });

    QTemporaryDir activeStorage;
    QTemporaryDir backgroundStorage;
    QVERIFY(activeStorage.isValid());
    QVERIFY(backgroundStorage.isValid());
    const QUrl origin(QStringLiteral("http://127.0.0.1:8080/"));
    auto router = HostGestureRouter::createForTesting(100);
    QVERIFY(router != nullptr);
    FileDialogCoordinator coordinator;
    const TabCapabilityAuthority activeAuthority = authority(
        QStringLiteral("tab-active"), 1, 41, 401, 7, 11);
    const TabCapabilityAuthority backgroundAuthority = authority(
        QStringLiteral("tab-background"), 2, 42, 402, 9, 12);
    auto activeToken = std::make_shared<AuthorityAdmissionToken>();
    auto backgroundToken = std::make_shared<AuthorityAdmissionToken>();
    QString activeError;
    QString backgroundError;
    auto active = HostCapabilityRuntime::create(
        activeAuthority, activeToken, router.get(), filePermission(), origin,
        activeStorage.path(), 100, &activeError, &coordinator);
    QVERIFY2(active != nullptr, qPrintable(activeError));
    auto activeCleanup = qScopeGuard([&] {
        HostCapabilityRuntime::retire(std::exchange(active, {}));
        (void)WorkerRetirementManager::instance().flush(10'000);
    });
    auto background = HostCapabilityRuntime::create(
        backgroundAuthority, backgroundToken, router.get(), filePermission(),
        origin, backgroundStorage.path(), 100, &backgroundError, &coordinator);
    QVERIFY2(background != nullptr, qPrintable(backgroundError));
    auto backgroundCleanup = qScopeGuard([&] {
        HostCapabilityRuntime::retire(std::exchange(background, {}));
        (void)WorkerRetirementManager::instance().flush(10'000);
    });
    QSignalSpy completed(background.get(),
                         &HostCapabilityRuntime::authorityCompleted);
    QVERIFY(completed.isValid());
    QVERIFY(router->activateBinding(activeAuthority));

    background->dispatch(
        backgroundAuthority.sessionGeneration,
        QStringLiteral("background-file-valid"), QStringLiteral("file"),
        QStringLiteral("open"), {});
    QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 2'000);
    const BrokerResult result =
        completed.takeFirst().at(3).value<BrokerResult>();
    QCOMPARE(dialogCalls, 0);
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("capability.denied"));
#endif
}

void HostCapabilityRuntimeTest::
    activeFileRequestCompletesThroughProcessCoordinator()
{
    QTemporaryDir directory;
    QTemporaryDir storage;
    QVERIFY(directory.isValid());
    QVERIFY(storage.isValid());
    const QString selectedPath =
        QDir(directory.path()).filePath(QStringLiteral("selected.txt"));
    writeFile(selectedPath, QByteArray("owner"));

    std::mutex showMutex;
    qbrowser_broker_testing::FileDialogTestShowCompletion completeShow;
    QSemaphore showEntered;
    qbrowser_broker_testing::FileDialogTestHooks hooks;
    hooks.coordinatorShow =
        [&](const qint64,
            qbrowser_broker_testing::FileDialogTestShowCompletion completion) {
            {
                const std::scoped_lock lock(showMutex);
                completeShow = std::move(completion);
            }
            showEntered.release();
        };
    qbrowser_broker_testing::setFileDialogTestHooks(std::move(hooks));
    const auto resetHooks = qScopeGuard([] {
        qbrowser_broker_testing::resetFileDialogTestHooks();
    });

    FileDialogCoordinator coordinator;
    auto router = HostGestureRouter::createForTesting(100);
    QVERIFY(router != nullptr);
    const TabCapabilityAuthority binding = authority(
        QStringLiteral("tab-owner"), 1, 41, 401, 7, 11);
    auto token = std::make_shared<AuthorityAdmissionToken>();
    QString error;
    auto runtime = HostCapabilityRuntime::create(
        binding, token, router.get(), filePermission(),
        QUrl(QStringLiteral("http://127.0.0.1:8080/")), storage.path(), 100,
        &error, &coordinator);
    QVERIFY2(runtime != nullptr, qPrintable(error));
    auto cleanup = qScopeGuard([&] {
        HostCapabilityRuntime::retire(std::exchange(runtime, {}));
        QVERIFY(WorkerRetirementManager::instance().flush(10'000));
        coordinator.shutdown();
    });
    QVERIFY(router->activateBinding(binding));
    QSignalSpy completed(runtime.get(),
                         &HostCapabilityRuntime::authorityCompleted);
    QVERIFY(completed.isValid());

    runtime->dispatch(binding.sessionGeneration, QStringLiteral("open-owner"),
                      QStringLiteral("file"), QStringLiteral("open"), {});
    QVERIFY(showEntered.tryAcquire(1, 1'000));
    {
        const std::scoped_lock lock(showMutex);
        QVERIFY(completeShow);
        completeShow({FileDialogStatus::Opened, selectedPath});
    }
    QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 2'000);
    const QList<QVariant> completion = completed.takeFirst();
    QCOMPARE(completion.at(0).value<TabCapabilityAuthority>(), binding);
    QCOMPARE(completion.at(1).toULongLong(), binding.sessionGeneration);
    QCOMPARE(completion.at(2).toString(), QStringLiteral("open-owner"));
    const BrokerResult result = completion.at(3).value<BrokerResult>();
    QVERIFY(result.ok);
    QCOMPARE(result.value.value(QStringLiteral("name")).toString(),
             QStringLiteral("selected.txt"));
    QCOMPARE(QByteArray::fromBase64(
                 result.value.value(QStringLiteral("contentBase64"))
                     .toString().toLatin1()),
             QByteArray("owner"));
}

void HostCapabilityRuntimeTest::switchingOwnerDoesNotRebindFileCompletion()
{
    QTemporaryDir directory;
    QTemporaryDir ownerStorage;
    QTemporaryDir siblingStorage;
    QVERIFY(directory.isValid());
    QVERIFY(ownerStorage.isValid());
    QVERIFY(siblingStorage.isValid());
    const QString selectedPath =
        QDir(directory.path()).filePath(QStringLiteral("original.txt"));
    writeFile(selectedPath, QByteArray("original"));

    std::mutex showMutex;
    qbrowser_broker_testing::FileDialogTestShowCompletion completeShow;
    QSemaphore showEntered;
    qbrowser_broker_testing::FileDialogTestHooks hooks;
    hooks.coordinatorShow =
        [&](const qint64,
            qbrowser_broker_testing::FileDialogTestShowCompletion completion) {
            {
                const std::scoped_lock lock(showMutex);
                completeShow = std::move(completion);
            }
            showEntered.release();
        };
    qbrowser_broker_testing::setFileDialogTestHooks(std::move(hooks));
    const auto resetHooks = qScopeGuard([] {
        qbrowser_broker_testing::resetFileDialogTestHooks();
    });

    FileDialogCoordinator coordinator;
    auto router = HostGestureRouter::createForTesting(100);
    QVERIFY(router != nullptr);
    const TabCapabilityAuthority ownerAuthority = authority(
        QStringLiteral("tab-owner"), 1, 41, 401, 7, 11);
    const TabCapabilityAuthority siblingAuthority = authority(
        QStringLiteral("tab-sibling"), 2, 42, 402, 9, 12);
    auto ownerToken = std::make_shared<AuthorityAdmissionToken>();
    auto siblingToken = std::make_shared<AuthorityAdmissionToken>();
    QString ownerError;
    QString siblingError;
    auto owner = HostCapabilityRuntime::create(
        ownerAuthority, ownerToken, router.get(), filePermission(),
        QUrl(QStringLiteral("http://127.0.0.1:8080/")), ownerStorage.path(),
        100, &ownerError, &coordinator);
    auto sibling = HostCapabilityRuntime::create(
        siblingAuthority, siblingToken, router.get(), filePermission(),
        QUrl(QStringLiteral("http://127.0.0.1:8080/")), siblingStorage.path(),
        100, &siblingError, &coordinator);
    QVERIFY2(owner != nullptr, qPrintable(ownerError));
    QVERIFY2(sibling != nullptr, qPrintable(siblingError));
    auto cleanup = qScopeGuard([&] {
        HostCapabilityRuntime::retire(std::exchange(owner, {}));
        HostCapabilityRuntime::retire(std::exchange(sibling, {}));
        QVERIFY(WorkerRetirementManager::instance().flush(10'000));
        coordinator.shutdown();
    });
    QSignalSpy ownerCompleted(owner.get(),
                              &HostCapabilityRuntime::authorityCompleted);
    QSignalSpy siblingCompleted(sibling.get(),
                                &HostCapabilityRuntime::authorityCompleted);
    QVERIFY(ownerCompleted.isValid());
    QVERIFY(siblingCompleted.isValid());
    QVERIFY(router->activateBinding(ownerAuthority));

    owner->dispatch(ownerAuthority.sessionGeneration,
                    QStringLiteral("original-owner"), QStringLiteral("file"),
                    QStringLiteral("open"), {});
    QVERIFY(showEntered.tryAcquire(1, 1'000));
    QVERIFY(router->activateBinding(siblingAuthority));
    {
        const std::scoped_lock lock(showMutex);
        QVERIFY(completeShow);
        completeShow({FileDialogStatus::Opened, selectedPath});
    }
    QTRY_COMPARE_WITH_TIMEOUT(ownerCompleted.count(), 1, 2'000);
    QCOMPARE(siblingCompleted.count(), 0);
    const QList<QVariant> completion = ownerCompleted.takeFirst();
    QCOMPARE(completion.at(0).value<TabCapabilityAuthority>(), ownerAuthority);
    QCOMPARE(completion.at(2).toString(), QStringLiteral("original-owner"));
    const BrokerResult result = completion.at(3).value<BrokerResult>();
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("capability.denied"));
}

void HostCapabilityRuntimeTest::
    revokedFileCallbackIsDroppedAndRetireCancelsExactlyOnce()
{
    QTemporaryDir directory;
    QTemporaryDir storage;
    QTemporaryDir reopenedStorage;
    QVERIFY(directory.isValid());
    QVERIFY(storage.isValid());
    QVERIFY(reopenedStorage.isValid());
    const QString selectedPath =
        QDir(directory.path()).filePath(QStringLiteral("late.txt"));
    const QString reopenedPath =
        QDir(directory.path()).filePath(QStringLiteral("reopened.txt"));
    writeFile(selectedPath, QByteArray("late"));
    writeFile(reopenedPath, QByteArray("new-owner"));

    std::mutex showMutex;
    qbrowser_broker_testing::FileDialogTestShowCompletion completeShow;
    QSemaphore showEntered;
    QSemaphore operationQuiesced;
    std::atomic<int> cancelCalls{0};
    qbrowser_broker_testing::FileDialogTestHooks hooks;
    hooks.coordinatorShow =
        [&](const qint64,
            qbrowser_broker_testing::FileDialogTestShowCompletion completion) {
            {
                const std::scoped_lock lock(showMutex);
                completeShow = std::move(completion);
            }
            showEntered.release();
        };
    hooks.coordinatorCancel = [&] { ++cancelCalls; };
    hooks.coordinatorOperationQuiesced = [&] { operationQuiesced.release(); };
    qbrowser_broker_testing::setFileDialogTestHooks(std::move(hooks));
    const auto resetHooks = qScopeGuard([] {
        qbrowser_broker_testing::resetFileDialogTestHooks();
    });

    FileDialogCoordinator coordinator;
    auto router = HostGestureRouter::createForTesting(100);
    QVERIFY(router != nullptr);
    const TabCapabilityAuthority binding = authority(
        QStringLiteral("tab-retired"), 1, 41, 401, 7, 11);
    auto token = std::make_shared<AuthorityAdmissionToken>();
    QString error;
    auto runtime = HostCapabilityRuntime::create(
        binding, token, router.get(), filePermission(),
        QUrl(QStringLiteral("http://127.0.0.1:8080/")), storage.path(), 100,
        &error, &coordinator);
    QVERIFY2(runtime != nullptr, qPrintable(error));
    QVERIFY(router->activateBinding(binding));
    QSignalSpy completed(runtime.get(),
                         &HostCapabilityRuntime::authorityCompleted);
    QVERIFY(completed.isValid());

    runtime->dispatch(binding.sessionGeneration, QStringLiteral("retire-open"),
                      QStringLiteral("file"), QStringLiteral("open"), {});
    QVERIFY(showEntered.tryAcquire(1, 1'000));
    qbrowser_broker_testing::FileDialogTestShowCompletion lateOld;
    {
        const std::scoped_lock lock(showMutex);
        lateOld = completeShow;
    }
    HostCapabilityRuntime::retire(std::exchange(runtime, {}));
    QVERIFY(operationQuiesced.tryAcquire(1, 1'000));
    QVERIFY(WorkerRetirementManager::instance().flush(10'000));
    QCoreApplication::processEvents();
    QCOMPARE(cancelCalls.load(), 1);
    QCOMPARE(completed.count(), 0);

    const TabCapabilityAuthority reopenedBinding = authority(
        QStringLiteral("tab-retired"), 2, 51, 501, 8, 12);
    auto reopenedToken = std::make_shared<AuthorityAdmissionToken>();
    QString reopenedError;
    auto reopened = HostCapabilityRuntime::create(
        reopenedBinding, reopenedToken, router.get(), filePermission(),
        QUrl(QStringLiteral("http://127.0.0.1:8080/")),
        reopenedStorage.path(), 100, &reopenedError, &coordinator);
    QVERIFY2(reopened != nullptr, qPrintable(reopenedError));
    auto reopenedCleanup = qScopeGuard([&] {
        HostCapabilityRuntime::retire(std::exchange(reopened, {}));
        QVERIFY(WorkerRetirementManager::instance().flush(10'000));
    });
    QVERIFY(router->activateBinding(reopenedBinding));
    QSignalSpy reopenedCompleted(reopened.get(),
                                 &HostCapabilityRuntime::authorityCompleted);
    QVERIFY(reopenedCompleted.isValid());
    reopened->dispatch(reopenedBinding.sessionGeneration,
                       QStringLiteral("reopened-open"), QStringLiteral("file"),
                       QStringLiteral("open"), {});
    QVERIFY(showEntered.tryAcquire(1, 1'000));
    qbrowser_broker_testing::FileDialogTestShowCompletion completeReopened;
    {
        const std::scoped_lock lock(showMutex);
        QVERIFY(lateOld);
        QVERIFY(completeShow);
        completeReopened = completeShow;
    }
    lateOld({FileDialogStatus::Opened, selectedPath});
    completeReopened({FileDialogStatus::Opened, reopenedPath});
    QTRY_COMPARE_WITH_TIMEOUT(reopenedCompleted.count(), 1, 2'000);
    QCoreApplication::processEvents();
    QCOMPARE(cancelCalls.load(), 1);
    QCOMPARE(completed.count(), 0);
    const QList<QVariant> reopenedCompletion = reopenedCompleted.takeFirst();
    QCOMPARE(reopenedCompletion.at(0).value<TabCapabilityAuthority>(),
             reopenedBinding);
    QCOMPARE(reopenedCompletion.at(2).toString(),
             QStringLiteral("reopened-open"));
    const BrokerResult reopenedResult =
        reopenedCompletion.at(3).value<BrokerResult>();
    QVERIFY(reopenedResult.ok);
    QCOMPARE(QByteArray::fromBase64(
                 reopenedResult.value.value(QStringLiteral("contentBase64"))
                     .toString().toLatin1()),
             QByteArray("new-owner"));
    coordinator.shutdown();
}

void HostCapabilityRuntimeTest::
    delayedFileCallbackIsDroppedAfterAdmissionRevocation()
{
    QTemporaryDir directory;
    QTemporaryDir storage;
    QVERIFY(directory.isValid());
    QVERIFY(storage.isValid());
    const QString selectedPath =
        QDir(directory.path()).filePath(QStringLiteral("revoked.txt"));
    writeFile(selectedPath, QByteArray("revoked"));

    std::mutex showMutex;
    qbrowser_broker_testing::FileDialogTestShowCompletion completeShow;
    QSemaphore showEntered;
    QSemaphore operationQuiesced;
    qbrowser_broker_testing::FileDialogTestHooks hooks;
    hooks.coordinatorShow = [&] (
                                const qint64,
                                qbrowser_broker_testing::
                                    FileDialogTestShowCompletion completion) {
        {
            const std::scoped_lock lock(showMutex);
            completeShow = std::move(completion);
        }
        showEntered.release();
    };
    hooks.coordinatorOperationQuiesced = [&] { operationQuiesced.release(); };
    qbrowser_broker_testing::setFileDialogTestHooks(std::move(hooks));
    const auto resetHooks = qScopeGuard([] {
        qbrowser_broker_testing::resetFileDialogTestHooks();
    });

    FileDialogCoordinator coordinator;
    auto router = HostGestureRouter::createForTesting(100);
    QVERIFY(router != nullptr);
    const TabCapabilityAuthority binding = authority(
        QStringLiteral("tab-revoked"), 1, 61, 601, 13, 17);
    auto token = std::make_shared<AuthorityAdmissionToken>();
    QString error;
    auto runtime = HostCapabilityRuntime::create(
        binding, token, router.get(), filePermission(),
        QUrl(QStringLiteral("http://127.0.0.1:8080/")), storage.path(), 100,
        &error, &coordinator);
    QVERIFY2(runtime != nullptr, qPrintable(error));
    auto cleanup = qScopeGuard([&] {
        HostCapabilityRuntime::retire(std::exchange(runtime, {}));
        QVERIFY(WorkerRetirementManager::instance().flush(10'000));
        coordinator.shutdown();
    });
    QVERIFY(router->activateBinding(binding));
    QSignalSpy completed(runtime.get(),
                         &HostCapabilityRuntime::authorityCompleted);
    QVERIFY(completed.isValid());

    runtime->dispatch(binding.sessionGeneration,
                      QStringLiteral("revoked-open"), QStringLiteral("file"),
                      QStringLiteral("open"), {});
    QVERIFY(showEntered.tryAcquire(1, 1'000));
    {
        const std::scoped_lock lock(showMutex);
        QVERIFY(completeShow);
        completeShow({FileDialogStatus::Opened, selectedPath});
    }
    QVERIFY(operationQuiesced.tryAcquire(1, 1'000));
    (void)token->beginRevoke();
    QCoreApplication::processEvents();
    QTest::qWait(50);
    QCOMPARE(completed.count(), 0);
}

void HostCapabilityRuntimeTest::onlyActiveAuthorityCanIssueAndConsumeGesture()
{
    auto router = HostGestureRouter::createForTesting(100);
    QVERIFY(router != nullptr);
    const TabCapabilityAuthority first = authority(
        QStringLiteral("tab-a"), 1, 41, 401, 7, 11);
    const TabCapabilityAuthority sibling = authority(
        QStringLiteral("tab-b"), 2, 42, 402, 7, 12);
    auto firstToken = std::make_shared<AuthorityAdmissionToken>();
    auto siblingToken = std::make_shared<AuthorityAdmissionToken>();
    auto firstStore = std::make_shared<UserGestureGrantStore>();
    auto siblingStore = std::make_shared<UserGestureGrantStore>();
    auto firstSession = firstStore->openSession(first.appIdentity);
    auto siblingSession = siblingStore->openSession(sibling.appIdentity);
    QVERIFY(firstSession.has_value());
    QVERIFY(siblingSession.has_value());
    QVERIFY(router->registerBinding(
        first, firstToken, firstStore, std::move(*firstSession)));
    QVERIFY(router->registerBinding(
        sibling, siblingToken, siblingStore, std::move(*siblingSession)));
    QVERIFY(router->activateBinding(first));
    router->setSystemEvidenceForTesting(validEvidence(first, 1'000));
    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_A), true, 900));

    QVERIFY(!router->issueGrant(sibling, QStringLiteral("same-request"), 250)
                 .has_value());
    auto firstGrant = router->issueGrant(
        first, QStringLiteral("same-request"), 250);
    QVERIFY(firstGrant.has_value());
    QVERIFY(!siblingStore->consume(*firstGrant,
                                   sibling.appIdentity,
                                   QStringLiteral("same-request")));
    QVERIFY(firstStore->consume(*firstGrant,
                                first.appIdentity,
                                QStringLiteral("same-request")));
    QVERIFY(!firstStore->consume(*firstGrant,
                                 first.appIdentity,
                                 QStringLiteral("same-request")));

    QVERIFY(router->activateBinding(sibling));
    router->setSystemEvidenceForTesting(validEvidence(sibling, 1'100));
    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_A), true, 1'050));
    auto siblingGrant = router->issueGrant(
        sibling, QStringLiteral("same-request"), 250);
    QVERIFY(siblingGrant.has_value());
    QVERIFY(siblingStore->consume(*siblingGrant,
                                  sibling.appIdentity,
                                  QStringLiteral("same-request")));
}

void HostCapabilityRuntimeTest::authorityTransitionsRevokeUnconsumedEvidence()
{
    const auto switchOrDeactivate = [](const bool switchTabs) {
        auto router = HostGestureRouter::createForTesting(100);
        QVERIFY(router != nullptr);
        const TabCapabilityAuthority first = authority(
            QStringLiteral("tab-a"), 1, 41, 401, 7, 11);
        const TabCapabilityAuthority sibling = authority(
            QStringLiteral("tab-b"), 2, 42, 402, 7, 12);
        auto firstToken = std::make_shared<AuthorityAdmissionToken>();
        auto siblingToken = std::make_shared<AuthorityAdmissionToken>();
        auto firstStore = std::make_shared<UserGestureGrantStore>();
        auto siblingStore = std::make_shared<UserGestureGrantStore>();
        auto firstSession = firstStore->openSession(first.appIdentity);
        auto siblingSession = siblingStore->openSession(sibling.appIdentity);
        QVERIFY(firstSession.has_value());
        QVERIFY(siblingSession.has_value());
        QVERIFY(router->registerBinding(
            first, firstToken, firstStore, std::move(*firstSession)));
        QVERIFY(router->registerBinding(
            sibling, siblingToken, siblingStore, std::move(*siblingSession)));
        QVERIFY(router->activateBinding(first));
        router->setSystemEvidenceForTesting(validEvidence(first, 1'000));
        QVERIFY(!router->routeKeyboardForTesting(
            QKeyCombination(Qt::NoModifier, Qt::Key_A), true, 900));
        if (switchTabs) {
            QVERIFY(router->activateBinding(sibling));
        } else {
            router->hostDeactivated();
        }
        QVERIFY(router->activateBinding(first));
        router->setSystemEvidenceForTesting(validEvidence(first, 1'001));
        QVERIFY(!router->issueGrant(first, QStringLiteral("after-transition"), 250)
                     .has_value());
    };
    switchOrDeactivate(true);
    switchOrDeactivate(false);

    const auto closeBinding = [] {
        auto router = HostGestureRouter::createForTesting(100);
        QVERIFY(router != nullptr);
        const TabCapabilityAuthority first = authority(
            QStringLiteral("tab-a"), 1, 41, 401, 7, 11);
        auto token = std::make_shared<AuthorityAdmissionToken>();
        auto store = std::make_shared<UserGestureGrantStore>();
        auto session = store->openSession(first.appIdentity);
        QVERIFY(session.has_value());
        QVERIFY(router->registerBinding(
            first, token, store, std::move(*session)));
        QVERIFY(router->activateBinding(first));
        router->setSystemEvidenceForTesting(validEvidence(first, 1'000));
        QVERIFY(!router->routeKeyboardForTesting(
            QKeyCombination(Qt::NoModifier, Qt::Key_A), true, 900));
        auto grant = router->issueGrant(first, QStringLiteral("unconsumed"), 250);
        QVERIFY(grant.has_value());
        router->unregisterBinding(first);
        QVERIFY(!store->consume(*grant, first.appIdentity,
                                QStringLiteral("unconsumed")));
        QVERIFY(!token->tryAcquireUse().has_value());
    };
    closeBinding();

    const TabCapabilityAuthority original = authority(
        QStringLiteral("tab-a"), 1, 41, 401, 7, 11);
    const QList<TabCapabilityAuthority> replacements{
        authority(QStringLiteral("tab-a"), 2, 41, 401, 7, 11),
        authority(QStringLiteral("tab-a"), 1, 43, 401, 7, 11),
        authority(QStringLiteral("tab-a"), 1, 41, 403, 7, 11),
        authority(QStringLiteral("tab-a"), 1, 41, 401, 8, 11),
        authority(QStringLiteral("tab-a"), 1, 41, 401, 7, 12),
        authority(QStringLiteral("tab-a"), 1, 41, 401, 7, 11,
                  QStringLiteral("com.qbrowser.replacement")),
    };
    for (const TabCapabilityAuthority &replacement : replacements) {
        auto router = HostGestureRouter::createForTesting(100);
        QVERIFY(router != nullptr);
        auto originalToken = std::make_shared<AuthorityAdmissionToken>();
        auto replacementToken = std::make_shared<AuthorityAdmissionToken>();
        auto originalStore = std::make_shared<UserGestureGrantStore>();
        auto replacementStore = std::make_shared<UserGestureGrantStore>();
        auto originalSession = originalStore->openSession(original.appIdentity);
        auto replacementSession = replacementStore->openSession(
            replacement.appIdentity);
        QVERIFY(originalSession.has_value());
        QVERIFY(replacementSession.has_value());
        QVERIFY(router->registerBinding(
            original, originalToken, originalStore,
            std::move(*originalSession)));
        QVERIFY(router->activateBinding(original));
        router->setSystemEvidenceForTesting(validEvidence(original, 1'000));
        QVERIFY(!router->routeKeyboardForTesting(
            QKeyCombination(Qt::NoModifier, Qt::Key_A), true, 900));
        auto grant = router->issueGrant(
            original, QStringLiteral("unconsumed"), 250);
        QVERIFY(grant.has_value());

        QVERIFY(router->registerBinding(
            replacement, replacementToken,
            replacementStore, std::move(*replacementSession)));
        QVERIFY(!originalStore->consume(*grant, original.appIdentity,
                                        QStringLiteral("unconsumed")));
        QVERIFY(!originalToken->tryAcquireUse().has_value());
        QVERIFY(router->activateBinding(replacement));
        router->setSystemEvidenceForTesting(validEvidence(replacement, 1'001));
        QVERIFY(!router->issueGrant(
                         replacement, QStringLiteral("no-reused-evidence"), 250)
                     .has_value());
    }
}

void HostCapabilityRuntimeTest::mainWindowDeactivationRevokesGestureEvidence()
{
    HostApplication host(QUrl(QStringLiteral("http://127.0.0.1:8080/")));
    QVERIFY(host.start());
    QVERIFY(host.mainWindow() != nullptr);
    HostGestureRouter *const router = host.gestureRouterForTesting();
    QVERIFY(router != nullptr);

    const TabCapabilityAuthority binding = authority(
        host.mainWindow()->tabModel()->activeId(), 1, 41, 401, 7, 11);
    auto token = std::make_shared<AuthorityAdmissionToken>();
    auto store = std::make_shared<UserGestureGrantStore>();
    auto session = store->openSession(binding.appIdentity);
    QVERIFY(session.has_value());
    QVERIFY(router->registerBinding(
        binding, token, store, std::move(*session)));
    QVERIFY(router->activateBinding(binding));
    router->setSystemEvidenceForTesting(validEvidence(binding, 1'000));
    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_A), true, 900));
    auto grant = router->issueGrant(
        binding, QStringLiteral("before-window-deactivate"), 5'000);
    QVERIFY(grant.has_value());

    QEvent deactivate(QEvent::WindowDeactivate);
    QVERIFY(QCoreApplication::sendEvent(host.mainWindow(), &deactivate));
    QVERIFY(!store->consume(*grant, binding.appIdentity,
                            QStringLiteral("before-window-deactivate")));

    QVERIFY(router->activateBinding(binding));
    router->setSystemEvidenceForTesting(validEvidence(binding, 1'100));
    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_B), true, 1'050));
    QEvent activate(QEvent::WindowActivate);
    QVERIFY(QCoreApplication::sendEvent(host.mainWindow(), &activate));
    QVERIFY(!store->consume(*grant, binding.appIdentity,
                            QStringLiteral("before-window-deactivate")));
    QVERIFY(!router->issueGrant(
        binding, QStringLiteral("after-window-activate"), 250).has_value());
}

void HostCapabilityRuntimeTest::backgroundFocusAndDispatchStateCannotAuthorize()
{
    auto router = HostGestureRouter::createForTesting(100);
    QVERIFY(router != nullptr);
    const TabCapabilityAuthority binding = authority(
        QStringLiteral("tab-a"), 1, 41, 401, 7, 11);
    auto token = std::make_shared<AuthorityAdmissionToken>();
    auto store = std::make_shared<UserGestureGrantStore>();
    auto session = store->openSession(binding.appIdentity);
    QVERIFY(session.has_value());
    QVERIFY(router->registerBinding(
        binding, token, store, std::move(*session)));
    QVERIFY(router->activateBinding(binding));

    HostGestureSystemEvidence background = validEvidence(binding, 1'000);
    background.foregroundMatchesHostRoot = false;
    router->setSystemEvidenceForTesting(background);
    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_A), true, 900));
    router->setSystemEvidenceForTesting(validEvidence(binding, 1'001));
    QVERIFY(!router->issueGrant(binding, QStringLiteral("background"), 250)
                 .has_value());

    router->setSystemEvidenceForTesting(validEvidence(binding, 2'000));
    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_A), true, 1'900));
    HostGestureSystemEvidence stolen = validEvidence(binding, 2'001);
    stolen.foregroundMatchesHostRoot = false;
    router->setSystemEvidenceForTesting(stolen);
    QVERIFY(!router->issueGrant(binding, QStringLiteral("stolen"), 250)
                 .has_value());
    router->setSystemEvidenceForTesting(validEvidence(binding, 2'002));
    QVERIFY(!router->issueGrant(binding, QStringLiteral("stale-after-stolen"), 250)
                 .has_value());

    router->setSystemEvidenceForTesting(validEvidence(binding, 3'000));
    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_A), true, 2'900));
    HostGestureSystemEvidence wrongPid = validEvidence(binding, 3'001);
    wrongPid.workerWindowProcessId = binding.workerProcessId + 1;
    router->setSystemEvidenceForTesting(wrongPid);
    QVERIFY(!router->issueGrant(binding, QStringLiteral("wrong-pid"), 250)
                 .has_value());
    router->setSystemEvidenceForTesting(validEvidence(binding, 3'002));
    QVERIFY(!router->issueGrant(binding, QStringLiteral("pid-evidence-revoked"), 250)
                 .has_value());

    router->setSystemEvidenceForTesting(validEvidence(binding, 4'000));
    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_A), true, 3'900));
    HostGestureSystemEvidence wrongWindow = validEvidence(binding, 4'001);
    wrongWindow.workerWindowId = binding.workerWindowId + 1;
    router->setSystemEvidenceForTesting(wrongWindow);
    QVERIFY(!router->issueGrant(binding, QStringLiteral("wrong-window"), 250)
                 .has_value());

    router->setSystemEvidenceForTesting(validEvidence(binding, 5'000));
    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_A), true, 4'900, true));
    QVERIFY(!router->issueGrant(
                     binding, QStringLiteral("lower-il-keyboard"), 250)
                 .has_value());
    router->setSystemEvidenceForTesting(validEvidence(binding, 5'100));
    QVERIFY(!router->observeMouseForTesting(
        binding.workerWindowId, binding.workerProcessId, true, 5'000, true));
    QVERIFY(!router->issueGrant(
                     binding, QStringLiteral("lower-il-mouse"), 250)
                 .has_value());
}

void HostCapabilityRuntimeTest::browserCommandIsSuppressedAndNeverBecomesGesture()
{
    auto router = HostGestureRouter::createForTesting(100);
    QVERIFY(router != nullptr);
    const TabCapabilityAuthority binding = authority(
        QStringLiteral("tab-a"), 1, 41, 401, 7, 11);
    auto token = std::make_shared<AuthorityAdmissionToken>();
    auto store = std::make_shared<UserGestureGrantStore>();
    auto session = store->openSession(binding.appIdentity);
    QVERIFY(session.has_value());
    QVERIFY(router->registerBinding(
        binding, token, store, std::move(*session)));
    QVERIFY(router->activateBinding(binding));
    router->setSystemEvidenceForTesting(validEvidence(binding, 1'000));
    QSignalSpy commands(router.get(), &HostGestureRouter::browserCommandRequested);
    QVERIFY(commands.isValid());
    const QKeyCombination ctrlTab(Qt::ControlModifier, Qt::Key_Tab);

    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::ControlModifier, Qt::Key_Control), true, 890));
    QVERIFY(router->routeKeyboardForTesting(ctrlTab, true, 900));
    QVERIFY(router->routeKeyboardForTesting(ctrlTab, true, 901));
    QVERIFY(router->routeKeyboardForTesting(ctrlTab, false, 902));
    QTRY_COMPARE(commands.count(), 1);
    QCOMPARE(commands.takeFirst().at(0).value<BrowserCommand>(),
             BrowserCommand::NextTab);
    QVERIFY(!router->issueGrant(binding, QStringLiteral("ctrl-tab"), 250)
                 .has_value());

    (void)token->beginRevoke();
    router->setSystemEvidenceForTesting(validEvidence(binding, 1'100));
    QVERIFY(router->routeKeyboardForTesting(ctrlTab, true, 1'050));
    QVERIFY(router->routeKeyboardForTesting(ctrlTab, false, 1'051));
    QCoreApplication::processEvents();
    QCOMPARE(commands.count(), 0);
    QVERIFY(!router->activateBinding(binding));
}

void HostCapabilityRuntimeTest::browserCommandLifecycleReentryDoesNotDeadlock()
{
    auto router = HostGestureRouter::createForTesting(100);
    QVERIFY(router != nullptr);
    const TabCapabilityAuthority binding = authority(
        QStringLiteral("tab-a"), 1, 41, 401, 7, 11);
    auto token = std::make_shared<AuthorityAdmissionToken>();
    auto store = std::make_shared<UserGestureGrantStore>();
    auto session = store->openSession(binding.appIdentity);
    QVERIFY(session.has_value());
    QVERIFY(router->registerBinding(
        binding, token, store, std::move(*session)));
    QVERIFY(router->activateBinding(binding));
    router->setSystemEvidenceForTesting(validEvidence(binding, 1'000));

    std::promise<void> revokeReturned;
    std::future<void> revokeFinished = revokeReturned.get_future();
    std::thread revokeThread;
    bool revokeReturnedInsideSignal = false;
    bool lifecycleCompleted = false;
    const QMetaObject::Connection connection = connect(
        router.get(), &HostGestureRouter::browserCommandRequested,
        router.get(),
        [router = router.get(), binding, token, &revokeReturned,
         &revokeFinished, &revokeThread, &revokeReturnedInsideSignal,
         &lifecycleCompleted](const BrowserCommand command) {
            if (command != BrowserCommand::CloseTab) return;
            revokeThread = std::thread([token, &revokeReturned] {
                (void)token->beginRevoke();
                revokeReturned.set_value();
            });
            revokeReturnedInsideSignal =
                revokeFinished.wait_for(std::chrono::seconds(1))
                == std::future_status::ready;
            if (revokeReturnedInsideSignal) {
                router->unregisterBinding(binding);
                lifecycleCompleted = true;
            }
        },
        Qt::DirectConnection);
    QVERIFY(connection);

    QVERIFY(router->routeKeyboardForTesting(
        QKeyCombination(Qt::ControlModifier, Qt::Key_W), true, 900));
    QCoreApplication::sendPostedEvents(router.get(), QEvent::MetaCall);
    if (revokeThread.joinable()) revokeThread.join();

    QVERIFY2(revokeReturnedInsideSignal,
             "beginRevoke was blocked by the browser-command publication gate");
    QVERIFY(lifecycleCompleted);
    QVERIFY(!router->activateBinding(binding));
}

void HostCapabilityRuntimeTest::staleQueuedBrowserCommandIsDropped_data()
{
    QTest::addColumn<bool>("retireOriginal");
    QTest::newRow("switch") << false;
    QTest::newRow("retire-and-switch") << true;
}

void HostCapabilityRuntimeTest::staleQueuedBrowserCommandIsDropped()
{
    QFETCH(bool, retireOriginal);
    auto router = HostGestureRouter::createForTesting(100);
    QVERIFY(router != nullptr);
    const TabCapabilityAuthority first = authority(
        QStringLiteral("tab-a"), 1, 41, 401, 7, 11);
    const TabCapabilityAuthority second = authority(
        QStringLiteral("tab-b"), 2, 42, 402, 9, 12);
    auto firstToken = std::make_shared<AuthorityAdmissionToken>();
    auto secondToken = std::make_shared<AuthorityAdmissionToken>();
    auto firstStore = std::make_shared<UserGestureGrantStore>();
    auto secondStore = std::make_shared<UserGestureGrantStore>();
    auto firstSession = firstStore->openSession(first.appIdentity);
    auto secondSession = secondStore->openSession(second.appIdentity);
    QVERIFY(firstSession.has_value());
    QVERIFY(secondSession.has_value());
    QVERIFY(router->registerBinding(
        first, firstToken, firstStore, std::move(*firstSession)));
    QVERIFY(router->registerBinding(
        second, secondToken, secondStore, std::move(*secondSession)));
    QVERIFY(router->activateBinding(first));
    router->setSystemEvidenceForTesting(validEvidence(first, 1'000));
    QSignalSpy commands(router.get(),
                        &HostGestureRouter::browserCommandRequested);
    QVERIFY(commands.isValid());
    const QKeyCombination ctrlTab(Qt::ControlModifier, Qt::Key_Tab);

    QVERIFY(router->routeKeyboardForTesting(ctrlTab, true, 900));
    QCOMPARE(commands.count(), 0);
    if (retireOriginal) router->unregisterBinding(first);
    QVERIFY(router->activateBinding(second));
    router->setSystemEvidenceForTesting(validEvidence(second, 1'100));
    QCoreApplication::sendPostedEvents(router.get(), QEvent::MetaCall);

    QCOMPARE(commands.count(), 0);
    QVERIFY(router->routeKeyboardForTesting(ctrlTab, false, 1'101));
}

void HostCapabilityRuntimeTest::queuedBrowserCommandCannotReviveAfterBindingRoundTrip()
{
    auto router = HostGestureRouter::createForTesting(100);
    QVERIFY(router != nullptr);
    const TabCapabilityAuthority first = authority(
        QStringLiteral("tab-a"), 1, 41, 401, 7, 11);
    const TabCapabilityAuthority second = authority(
        QStringLiteral("tab-b"), 2, 42, 402, 9, 12);
    auto firstToken = std::make_shared<AuthorityAdmissionToken>();
    auto secondToken = std::make_shared<AuthorityAdmissionToken>();
    auto firstStore = std::make_shared<UserGestureGrantStore>();
    auto secondStore = std::make_shared<UserGestureGrantStore>();
    auto firstSession = firstStore->openSession(first.appIdentity);
    auto secondSession = secondStore->openSession(second.appIdentity);
    QVERIFY(firstSession.has_value());
    QVERIFY(secondSession.has_value());
    QVERIFY(router->registerBinding(
        first, firstToken, firstStore, std::move(*firstSession)));
    QVERIFY(router->registerBinding(
        second, secondToken, secondStore, std::move(*secondSession)));
    QVERIFY(router->activateBinding(first));
    router->setSystemEvidenceForTesting(validEvidence(first, 1'000));
    QSignalSpy commands(router.get(),
                        &HostGestureRouter::browserCommandRequested);
    QVERIFY(commands.isValid());
    const QKeyCombination ctrlTab(Qt::ControlModifier, Qt::Key_Tab);

    QVERIFY(router->routeKeyboardForTesting(ctrlTab, true, 900));
    QCOMPARE(commands.count(), 0);
    QVERIFY(router->activateBinding(second));
    router->setSystemEvidenceForTesting(validEvidence(second, 1'100));
    QVERIFY(router->activateBinding(first));
    router->setSystemEvidenceForTesting(validEvidence(first, 1'200));
    QCoreApplication::sendPostedEvents(router.get(), QEvent::MetaCall);

    QCOMPARE(commands.count(), 0);
    QVERIFY(router->routeKeyboardForTesting(ctrlTab, false, 1'201));
}

void HostCapabilityRuntimeTest::heldBrowserChordRemainsSuppressedAcrossBindingSwitch()
{
    auto router = HostGestureRouter::createForTesting(100);
    QVERIFY(router != nullptr);
    const TabCapabilityAuthority first = authority(
        QStringLiteral("tab-a"), 1, 41, 401, 7, 11);
    const TabCapabilityAuthority second = authority(
        QStringLiteral("tab-b"), 2, 42, 402, 9, 12);
    auto firstToken = std::make_shared<AuthorityAdmissionToken>();
    auto secondToken = std::make_shared<AuthorityAdmissionToken>();
    auto firstStore = std::make_shared<UserGestureGrantStore>();
    auto secondStore = std::make_shared<UserGestureGrantStore>();
    auto firstSession = firstStore->openSession(first.appIdentity);
    auto secondSession = secondStore->openSession(second.appIdentity);
    QVERIFY(firstSession.has_value());
    QVERIFY(secondSession.has_value());
    QVERIFY(router->registerBinding(
        first, firstToken, firstStore, std::move(*firstSession)));
    QVERIFY(router->registerBinding(
        second, secondToken, secondStore, std::move(*secondSession)));
    QVERIFY(router->activateBinding(first));
    router->setSystemEvidenceForTesting(validEvidence(first, 1'000));
    QSignalSpy commands(router.get(),
                        &HostGestureRouter::browserCommandRequested);
    QVERIFY(commands.isValid());

    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::ControlModifier, Qt::Key_Control), true, 890));
    QVERIFY(router->routeKeyboardForTesting(
        QKeyCombination(Qt::ControlModifier, Qt::Key_Tab), true, 900));
    QVERIFY(router->activateBinding(second));
    router->setSystemEvidenceForTesting(validEvidence(second, 1'100));
    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_Control), false, 1'040));

    QVERIFY(router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_Tab), true, 1'050));
    QVERIFY(!router->issueGrant(
        second, QStringLiteral("held-tab-repeat"), 250).has_value());
    QVERIFY(router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_Tab), false, 1'051));
    QCoreApplication::sendPostedEvents(router.get(), QEvent::MetaCall);
    QCOMPARE(commands.count(), 0);

    router->setSystemEvidenceForTesting(validEvidence(second, 1'200));
    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_Tab), true, 1'150));
    QVERIFY(router->issueGrant(
        second, QStringLiteral("new-tab-keydown"), 250).has_value());
}

void HostCapabilityRuntimeTest::revocationClosesUseAndPublicationUntilGuardDrains()
{
    auto token = std::make_shared<AuthorityAdmissionToken>();
    auto guard = token->tryAcquireUse();
    QVERIFY(guard.has_value());
    int publications = 0;
    QVERIFY(guard->publishIfStillAdmitted([&publications] {
        ++publications;
        return true;
    }));
    QCOMPARE(publications, 1);

    const AuthorityAdmissionToken::RevocationTicket ticket = token->beginRevoke();
    QVERIFY(!ticket.isDrained());
    QVERIFY(!ticket.waitAllowedOnCurrentThread());
    QVERIFY(!token->tryAcquireUse().has_value());
    QVERIFY(!guard->publishIfStillAdmitted([&publications] {
        ++publications;
        return true;
    }));
    QCOMPARE(publications, 1);
    auto repeatedRevoker = std::async(std::launch::async, [token] {
        const auto repeatedTicket = token->beginRevoke();
        return repeatedTicket.waitAllowedOnCurrentThread();
    });
    QVERIFY(!repeatedRevoker.get());

    auto timed = std::async(std::launch::async, [ticket] {
        return ticket.waitUntil(monotonicDeadlineAfter(
            std::chrono::milliseconds(20)));
    });
    QVERIFY(!timed.get());
    QVERIFY(!ticket.isDrained());
    bool publicationAfterDeadlineRan = false;
    QVERIFY(!guard->publishIfStillAdmitted([&publicationAfterDeadlineRan] {
        publicationAfterDeadlineRan = true;
        return true;
    }));
    QVERIFY(!publicationAfterDeadlineRan);
    auto drained = std::async(std::launch::async, [ticket] {
        ticket.waitUntilDrained();
        return ticket.isDrained();
    });
    QCOMPARE(drained.wait_for(std::chrono::milliseconds(20)),
             std::future_status::timeout);
    guard.reset();
    QCOMPARE(drained.wait_for(std::chrono::seconds(1)),
             std::future_status::ready);
    QVERIFY(drained.get());
    QVERIFY(ticket.isDrained());
}

void HostCapabilityRuntimeTest::retiringRuntimeLeavesSiblingGestureStorageAndCompletionActive()
{
    QTemporaryDir firstStorage;
    QTemporaryDir siblingStorage;
    QVERIFY(firstStorage.isValid());
    QVERIFY(siblingStorage.isValid());
    const QUrl origin(QStringLiteral("http://127.0.0.1:8080/"));
    auto router = HostGestureRouter::createForTesting(100);
    QVERIFY(router != nullptr);
    const TabCapabilityAuthority firstAuthority = authority(
        QStringLiteral("tab-a"), 1, 41, 401, 7, 11);
    const TabCapabilityAuthority siblingAuthority = authority(
        QStringLiteral("tab-b"), 2, 42, 402, 9, 12);
    auto firstToken = std::make_shared<AuthorityAdmissionToken>();
    auto siblingToken = std::make_shared<AuthorityAdmissionToken>();
    QString firstError;
    QString siblingError;
    auto first = HostCapabilityRuntime::create(
        firstAuthority, firstToken, router.get(),
        clipboardAndStoragePermissions(), origin, firstStorage.path(), 100,
        &firstError, nullptr);
    QVERIFY2(first != nullptr, qPrintable(firstError));
    auto firstCleanup = qScopeGuard([&] {
        HostCapabilityRuntime::retire(std::exchange(first, {}));
        (void)WorkerRetirementManager::instance().flush(10'000);
    });
    auto sibling = HostCapabilityRuntime::create(
        siblingAuthority, siblingToken, router.get(),
        clipboardAndStoragePermissions(), origin, siblingStorage.path(), 100,
        &siblingError, nullptr);
    QVERIFY2(sibling != nullptr, qPrintable(siblingError));
    auto siblingCleanup = qScopeGuard([&] {
        HostCapabilityRuntime::retire(std::exchange(sibling, {}));
        (void)WorkerRetirementManager::instance().flush(10'000);
    });
    QSignalSpy completed(sibling.get(),
                         &HostCapabilityRuntime::authorityCompleted);
    QVERIFY(completed.isValid());
    QVERIFY(router->activateBinding(firstAuthority));
    router->setSystemEvidenceForTesting(validEvidence(firstAuthority, 800));
    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_A), true, 700));

    HostCapabilityRuntime::retire(std::exchange(first, {}));
    QVERIFY(WorkerRetirementManager::instance().flush(10'000));
    QVERIFY(!firstToken->tryAcquireUse().has_value());
    QVERIFY(siblingToken->tryAcquireUse().has_value());
    QVERIFY(router->activateBinding(siblingAuthority));
    sibling->dispatch(
        siblingAuthority.sessionGeneration + 1,
        QStringLiteral("wrong-generation"), QStringLiteral("storage"),
        QStringLiteral("get"),
        {{QStringLiteral("key"), QStringLiteral("must-not-dispatch")}});
    QTest::qWait(50);
    QCOMPARE(completed.count(), 0);
    router->setSystemEvidenceForTesting(validEvidence(siblingAuthority, 1'000));
    QVERIFY(!router->routeKeyboardForTesting(
        QKeyCombination(Qt::NoModifier, Qt::Key_A), true, 900));
    QGuiApplication::clipboard()->setText(QStringLiteral("sibling-value"));
    sibling->dispatch(siblingAuthority.sessionGeneration,
                      QStringLiteral("same-request"),
                      QStringLiteral("clipboard"), QStringLiteral("read"), {});
    QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 2'000);
    QCOMPARE(completed.at(0).at(0).value<TabCapabilityAuthority>(),
             siblingAuthority);
    QCOMPARE(completed.at(0).at(1).toULongLong(),
             siblingAuthority.sessionGeneration);
    QCOMPARE(completed.at(0).at(2).toString(), QStringLiteral("same-request"));
    const BrokerResult clipboardResult =
        completed.at(0).at(3).value<BrokerResult>();
    QVERIFY(clipboardResult.ok
            || clipboardResult.errorCode == QStringLiteral("clipboard.failed"));
    QVERIFY(clipboardResult.errorCode
            != QStringLiteral("clipboard.gesture_required"));

    sibling->dispatch(
        siblingAuthority.sessionGeneration, QStringLiteral("storage-request"),
        QStringLiteral("storage"), QStringLiteral("get"),
        {{QStringLiteral("key"), QStringLiteral("survives")}});
    QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 2, 2'000);
    QCOMPARE(completed.at(1).at(0).value<TabCapabilityAuthority>(),
             siblingAuthority);
    QCOMPARE(completed.at(1).at(2).toString(),
             QStringLiteral("storage-request"));
    QCOMPARE(completed.at(1).at(3).value<BrokerResult>().errorCode,
             QStringLiteral("storage.not_found"));
}

void HostCapabilityRuntimeTest::repeatedRetireClaimsWorkerExactlyOnce()
{
    QTemporaryDir storage;
    QVERIFY(storage.isValid());
    QString error;
    auto runtime = HostCapabilityRuntime::create(
        QStringLiteral("com.qbrowser.test"), {},
        QUrl(QStringLiteral("http://127.0.0.1:8080/")), storage.path(),
        0, 0, 0, &error);
    QVERIFY2(runtime != nullptr, qPrintable(error));
    auto alias = runtime;

    HostCapabilityRuntime::retire(std::exchange(runtime, {}));
    HostCapabilityRuntime::retire(std::exchange(alias, {}));
    QVERIFY(WorkerRetirementManager::instance().flush(10'000));
    QCoreApplication::processEvents();
    QVERIFY(WorkerRetirementManager::instance().status().isIdle());
}

void HostCapabilityRuntimeTest::slowRequestRetiresWithoutBlockingOrLateDelivery()
{
    QTcpServer silentServer;
    QVERIFY(silentServer.listen(QHostAddress::LocalHost));
    const QUrl origin(QStringLiteral("http://127.0.0.1:%1/")
                          .arg(silentServer.serverPort()));
    QTemporaryDir storage;
    QVERIFY(storage.isValid());
    QString error;
    auto runtime = HostCapabilityRuntime::create(
        QStringLiteral("com.qbrowser.test"),
        networkPermission(silentServer.serverPort()), origin, storage.path(),
        0, 0, 0, &error);
    QVERIFY2(runtime != nullptr, qPrintable(error));
    int delivered = 0;
    connect(runtime.get(), &HostCapabilityRuntime::completed,
            this, [&delivered] { ++delivered; });
    runtime->dispatch(
        7, QStringLiteral("slow-request"), QStringLiteral("network"),
        QStringLiteral("request"),
        requestPayload(QUrl(origin.toString() + QStringLiteral("api/dashboard"))));

    QElapsedTimer destruction;
    destruction.start();
    HostCapabilityRuntime::retire(std::exchange(runtime, {}));
    QVERIFY2(destruction.elapsed() < 100,
             qPrintable(QStringLiteral("destruction blocked %1 ms")
                            .arg(destruction.elapsed())));
    QVERIFY(WorkerRetirementManager::instance().flush(10'000));
    QVERIFY(WorkerRetirementManager::instance().status().isIdle());
    QCoreApplication::processEvents();
    QCOMPARE(delivered, 0);
}

QTEST_MAIN(HostCapabilityRuntimeTest)

#include "tst_host_capability_runtime.moc"
