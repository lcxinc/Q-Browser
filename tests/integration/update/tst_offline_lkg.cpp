#include "UpdateLifecycleCoordinator.h"
#include "UpdateTestSupport.h"
#include "PackageInstallerTestHooks.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTest>
#include <QScopeGuard>

#include <algorithm>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

class OfflineLkgTest final : public QObject
{
    Q_OBJECT

private slots:
    void restoresVerifiedLkgWhenCurrentIsCorruptAndIgnoresPartialState();
    void stateCommitFailureDoesNotLaunchOrOverwrite();
    void concurrentActivationAfterVerificationNeverLaunchesStalePackage();
    void currentOfflineStartCompareAndCommitsBeforeLaunch();
};

void OfflineLkgTest::currentOfflineStartCompareAndCommitsBeforeLaunch()
{
    UpdateTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem,
                               updateInstallPolicy());
    ManualLifecycleClock clock;
    QVector<qint64> launchGenerations;
    UpdateLifecycleCoordinator coordinator(
        QStringLiteral("company.pilot"), store, installer, {100, 150},
        [&](const UpdateLaunchRequest &) {
            launchGenerations.push_back(
                store.activationState(QStringLiteral("company.pilot"))
                    .state.generation);
            return true;
        }, clock.source());
    clock.set(0);
    QVERIFY(coordinator.installAndLaunch(updateSignedPackage(
                temporary, QStringLiteral("offline-confirm"),
                QStringLiteral("1.0.0"), keys.value().privateKeyPem))
                .succeeded());
    const WorkerAttemptKey key = coordinator.currentAttemptKey().value();
    clock.set(1);
    QCOMPARE(coordinator.authenticatedHandshake(key),
             UpdateLifecycleAction::None);
    clock.set(101);
    QCOMPARE(coordinator.heartbeat(key), UpdateLifecycleAction::MarkedHealthy);
    const qint64 beforeOffline = store.activationState(
        QStringLiteral("company.pilot")).state.generation;

    clock.set(1'000);
    const UpdateLifecycleResult offline = coordinator.startOffline();
    QVERIFY2(offline.succeeded(), qPrintable(offline.stableError));
    QCOMPARE(offline.action, UpdateLifecycleAction::LaunchRequested);
    QCOMPARE(store.activationState(QStringLiteral("company.pilot"))
                 .state.generation,
             beforeOffline + 1);
    QCOMPARE(launchGenerations.back(), beforeOffline + 1);
}

void OfflineLkgTest::concurrentActivationAfterVerificationNeverLaunchesStalePackage()
{
    UpdateTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem,
                               updateInstallPolicy());
    QVector<UpdateLaunchRequest> launches;
    ManualLifecycleClock clock;
    UpdateLifecycleCoordinator coordinator(
        QStringLiteral("company.pilot"), store, installer, {100, 150},
        [&](const UpdateLaunchRequest &request) {
            launches.push_back(request);
            return true;
        }, clock.source());
    clock.set(0);
    const UpdateLifecycleResult first = coordinator.installAndLaunch(
        updateSignedPackage(temporary, QStringLiteral("race-a"),
                            QStringLiteral("1.0.0"), keys.value().privateKeyPem));
    QVERIFY(first.succeeded());
    const WorkerAttemptKey key = coordinator.currentAttemptKey().value();
    clock.set(1);
    (void)coordinator.authenticatedHandshake(key);
    clock.set(101);
    QCOMPARE(coordinator.heartbeat(key),
             UpdateLifecycleAction::MarkedHealthy);
    const QString packageB = updateSignedPackage(
        temporary, QStringLiteral("race-b"), QStringLiteral("1.1.0"),
        keys.value().privateKeyPem);
    PackageStore competingStore(store.root());
    PackageInstaller competingInstaller(
        competingStore, keys.value().publicKeyPem, updateInstallPolicy());
    bool activatedB = false;
    QString activatedDirectory;
    qbrowser_package_installer_testing::PackageInstallerTestHooks hooks;
    hooks.afterVerifyInstalled = [&](const QString &, const QString &) {
        if (activatedB) return;
        activatedB = true;
        const InstallResult installedB = competingInstaller.install(packageB);
        QVERIFY2(installedB.succeeded(), qPrintable(installedB.stableError));
        activatedDirectory = QFileInfo(installedB.path).fileName();
    };
    qbrowser_package_installer_testing::setPackageInstallerTestHooks(
        std::move(hooks));
    const auto resetHooks = qScopeGuard([] {
        qbrowser_package_installer_testing::resetPackageInstallerTestHooks();
    });

    launches.clear();
    clock.set(1'000);
    const UpdateLifecycleResult offline = coordinator.startOffline();
    QVERIFY(activatedB);
    QCOMPARE(offline.error, UpdateLifecycleError::StateCommitFailed);
    QVERIFY(coordinator.failedClosed());
    QVERIFY(launches.isEmpty());
    QCOMPARE(store.activationState(QStringLiteral("company.pilot")).state.current,
             activatedDirectory);
}

void OfflineLkgTest::restoresVerifiedLkgWhenCurrentIsCorruptAndIgnoresPartialState()
{
    UpdateTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem,
                               updateInstallPolicy());
    QVector<UpdateLaunchRequest> launches;
    QVector<bool> stateCommittedAtLaunch;
    ManualLifecycleClock clock;
    UpdateLifecycleCoordinator coordinator(
        QStringLiteral("company.pilot"), store, installer, {100, 150},
        [&](const UpdateLaunchRequest &request) {
            const ActivationState state = store.activationState(
                QStringLiteral("company.pilot")).state;
            stateCommittedAtLaunch.push_back(
                state.current == QFileInfo(request.packageDirectory).fileName());
            launches.push_back(request);
            return true;
        }, clock.source());

    clock.set(0);
    const UpdateLifecycleResult first = coordinator.installAndLaunch(
        updateSignedPackage(temporary, QStringLiteral("one"),
                            QStringLiteral("1.0.0"), keys.value().privateKeyPem));
    QVERIFY(first.succeeded());
    WorkerAttemptKey key = coordinator.currentAttemptKey().value();
    clock.set(1);
    QCOMPARE(coordinator.authenticatedHandshake(key),
             UpdateLifecycleAction::None);
    clock.set(101);
    QCOMPARE(coordinator.heartbeat(key),
             UpdateLifecycleAction::MarkedHealthy);
    clock.set(1'000);
    const UpdateLifecycleResult stable = coordinator.installAndLaunch(
        updateSignedPackage(temporary, QStringLiteral("two"),
                            QStringLiteral("1.1.0"), keys.value().privateKeyPem));
    QVERIFY(stable.succeeded());
    key = coordinator.currentAttemptKey().value();
    clock.set(1'001);
    QCOMPARE(coordinator.authenticatedHandshake(key),
             UpdateLifecycleAction::None);
    clock.set(1'101);
    QCOMPARE(coordinator.heartbeat(key),
             UpdateLifecycleAction::MarkedHealthy);
    clock.set(2'000);
    const UpdateLifecycleResult candidate = coordinator.installAndLaunch(
        updateSignedPackage(temporary, QStringLiteral("three"),
                            QStringLiteral("1.2.0"), keys.value().privateKeyPem));
    QVERIFY(candidate.succeeded());

    const QString appRoot = store.appRoot(QStringLiteral("company.pilot"));
    const ActivationState beforeInterruption = store.recordedActivationState(
        QStringLiteral("company.pilot")).state;
    const QString missingCurrent = QStringLiteral("1.2.1-")
        + QString(64, QLatin1Char('0'));
    QSaveFile interrupted(appRoot + QStringLiteral("/activation.json"));
    interrupted.setDirectWriteFallback(false);
    QVERIFY(interrupted.open(QIODevice::WriteOnly));
    const QByteArray interruptedBytes = ActivationState{
        missingCurrent, beforeInterruption.current,
        beforeInterruption.lastKnownGood}.toJson();
    QCOMPARE(interrupted.write(interruptedBytes), qint64(interruptedBytes.size()));
    QVERIFY(interrupted.commit());

    QFile partial(appRoot + QStringLiteral("/activation.json.partial"));
    QVERIFY(partial.open(QIODevice::WriteOnly));
    const QByteArray partialBytes = QByteArrayLiteral(
        "{\"current\":\"versions/unverified\"}");
    QCOMPARE(partial.write(partialBytes), qint64(partialBytes.size()));
    partial.close();
    QVERIFY(QDir().mkpath(store.root()
                          + QStringLiteral("/.staging/unverified-candidate/qml")));

    const int beforeRecoveryLaunches = launches.size();
    clock.set(3'000);
    const UpdateLifecycleResult recovered = coordinator.startOffline();
    QVERIFY2(recovered.succeeded(), qPrintable(recovered.stableError));
    QCOMPARE(recovered.action, UpdateLifecycleAction::RecoveredAndLaunched);
    QCOMPARE(launches.size(), beforeRecoveryLaunches + 1);
    QVERIFY(std::ranges::all_of(stateCommittedAtLaunch,
                                [](const bool value) { return value; }));
    QCOMPARE(launches.back().packageVersion, QStringLiteral("1.1.0"));
    QCOMPARE(launches.back().packageDirectory, stable.path);
    const ActivationState state = store.activationState(
        QStringLiteral("company.pilot")).state;
    QCOMPARE(state.current, QFileInfo(stable.path).fileName());
    QCOMPARE(state.lastKnownGood, QFileInfo(stable.path).fileName());
    QVERIFY(state.current != QStringLiteral("unverified"));
}

void OfflineLkgTest::stateCommitFailureDoesNotLaunchOrOverwrite()
{
#ifndef Q_OS_WIN
    QSKIP("Deterministic state replacement sharing violation is Windows-specific");
#else
    UpdateTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem,
                               updateInstallPolicy());
    int launches = 0;
    ManualLifecycleClock clock;
    UpdateLifecycleCoordinator coordinator(
        QStringLiteral("company.pilot"), store, installer, {100, 150},
        [&](const UpdateLaunchRequest &) {
            ++launches;
            return true;
        }, clock.source());
    clock.set(0);
    const UpdateLifecycleResult stable = coordinator.installAndLaunch(
        updateSignedPackage(temporary, QStringLiteral("stable"),
                            QStringLiteral("1.0.0"), keys.value().privateKeyPem));
    QVERIFY(stable.succeeded());
    const WorkerAttemptKey key = coordinator.currentAttemptKey().value();
    clock.set(1);
    (void)coordinator.authenticatedHandshake(key);
    clock.set(101);
    QCOMPARE(coordinator.heartbeat(key),
             UpdateLifecycleAction::MarkedHealthy);

    const QString appRoot = store.appRoot(QStringLiteral("company.pilot"));
    const QString missingCurrent = QStringLiteral("1.1.0-")
        + QString(64, QLatin1Char('0'));
    const ActivationState interruptedState{
        missingCurrent, {}, QFileInfo(stable.path).fileName()};
    const QByteArray interruptedBytes = interruptedState.toJson();
    const QString statePath = appRoot + QStringLiteral("/activation.json");
    QSaveFile interrupted(statePath);
    interrupted.setDirectWriteFallback(false);
    QVERIFY(interrupted.open(QIODevice::WriteOnly));
    QCOMPARE(interrupted.write(interruptedBytes), qint64(interruptedBytes.size()));
    QVERIFY(interrupted.commit());

    const HANDLE blocker = CreateFileW(
        reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(statePath).utf16()),
        GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    QVERIFY(blocker != INVALID_HANDLE_VALUE);
    const int launchesBeforeRecovery = launches;
    clock.set(1'000);
    const UpdateLifecycleResult rejected = coordinator.startOffline();
    CloseHandle(blocker);

    QCOMPARE(rejected.error, UpdateLifecycleError::StateCommitFailed);
    QCOMPARE(launches, launchesBeforeRecovery);
    QFile stateFile(statePath);
    QVERIFY(stateFile.open(QIODevice::ReadOnly));
    QCOMPARE(stateFile.readAll(), interruptedBytes);
#endif
}

QTEST_APPLESS_MAIN(OfflineLkgTest)

#include "tst_offline_lkg.moc"
