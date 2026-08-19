#include "UpdateLifecycleCoordinator.h"
#include "UpdateTestSupport.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTest>

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
};

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
    UpdateLifecycleCoordinator coordinator(
        QStringLiteral("company.pilot"), store, installer, {100, 150},
        [&](const UpdateLaunchRequest &request) {
            const ActivationState state = store.activationState(
                QStringLiteral("company.pilot")).state;
            stateCommittedAtLaunch.push_back(
                state.current == QFileInfo(request.packageDirectory).fileName());
            launches.push_back(request);
            return true;
        });

    const UpdateLifecycleResult first = coordinator.installAndLaunch(
        updateSignedPackage(temporary, QStringLiteral("one"),
                            QStringLiteral("1.0.0"), keys.value().privateKeyPem), 0);
    QVERIFY(first.succeeded());
    WorkerAttemptKey key = coordinator.currentAttemptKey().value();
    QCOMPARE(coordinator.authenticatedHandshake(key, 1),
             UpdateLifecycleAction::None);
    QCOMPARE(coordinator.heartbeat(key, 101),
             UpdateLifecycleAction::MarkedHealthy);
    const UpdateLifecycleResult stable = coordinator.installAndLaunch(
        updateSignedPackage(temporary, QStringLiteral("two"),
                            QStringLiteral("1.1.0"), keys.value().privateKeyPem),
        1'000);
    QVERIFY(stable.succeeded());
    key = coordinator.currentAttemptKey().value();
    QCOMPARE(coordinator.authenticatedHandshake(key, 1'001),
             UpdateLifecycleAction::None);
    QCOMPARE(coordinator.heartbeat(key, 1'101),
             UpdateLifecycleAction::MarkedHealthy);
    const UpdateLifecycleResult candidate = coordinator.installAndLaunch(
        updateSignedPackage(temporary, QStringLiteral("three"),
                            QStringLiteral("1.2.0"), keys.value().privateKeyPem),
        2'000);
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
    const UpdateLifecycleResult recovered = coordinator.startOffline(3'000);
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
    UpdateLifecycleCoordinator coordinator(
        QStringLiteral("company.pilot"), store, installer, {100, 150},
        [&](const UpdateLaunchRequest &) {
            ++launches;
            return true;
        });
    const UpdateLifecycleResult stable = coordinator.installAndLaunch(
        updateSignedPackage(temporary, QStringLiteral("stable"),
                            QStringLiteral("1.0.0"), keys.value().privateKeyPem), 0);
    QVERIFY(stable.succeeded());
    const WorkerAttemptKey key = coordinator.currentAttemptKey().value();
    (void)coordinator.authenticatedHandshake(key, 1);
    QCOMPARE(coordinator.heartbeat(key, 101),
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
    const UpdateLifecycleResult rejected = coordinator.startOffline(1'000);
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
