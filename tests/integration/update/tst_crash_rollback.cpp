#include "UpdateLifecycleCoordinator.h"
#include "UpdateTestSupport.h"

#ifdef Q_OS_WIN
#include "WorkerTestEnvironment.h"
#endif

#include <QFileInfo>
#include <QCoreApplication>
#include <QTest>
#include <QUuid>

#include <algorithm>

class CrashRollbackTest final : public QObject
{
    Q_OBJECT

private slots:
    void protectedLongPathTemporaryCleanupLeavesNoResidue();
    void commitsRollbackBeforeLaunchingRecoveredRealLpacWorker();
    void concurrentActivationBeforeRestartFailsClosedWithoutLaunchingStalePackage();
};

void CrashRollbackTest::protectedLongPathTemporaryCleanupLeavesNoResidue()
{
#ifndef Q_OS_WIN
    QSKIP("Protected long-path cleanup is Windows-specific");
#else
    auto temporary = std::make_unique<UpdateTemporaryDir>();
    QVERIFY(temporary->isValid());
    const QString ownedRoot = temporary->path();
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());

    QString storeRelative;
    while (temporary->filePath(storeRelative + QStringLiteral("store")).size()
           < 190) {
        storeRelative += QStringLiteral("cleanup-segment/");
    }
    {
        PackageStore store(
            temporary->filePath(storeRelative + QStringLiteral("store")));
        PackageInstaller installer(store, keys.value().publicKeyPem,
                                   updateInstallPolicy());
        const InstallResult installed = installer.install(updateSignedPackage(
            *temporary, QStringLiteral("cleanup"), QStringLiteral("1.0.0"),
            keys.value().privateKeyPem));
        QVERIFY2(installed.succeeded(), qPrintable(installed.stableError));
        const QString protectedContent = QDir(installed.path).filePath(
            QStringLiteral("metadata/content.sha256"));
        QVERIFY2(protectedContent.size() > MAX_PATH,
                 qPrintable(QStringLiteral("test path was not long enough: %1")
                                .arg(protectedContent.size())));
    }

    temporary.reset();
    QVERIFY2(!QFileInfo::exists(ownedRoot),
             qPrintable(QStringLiteral("protected temporary root remained: %1")
                            .arg(ownedRoot)));
#endif
}

void CrashRollbackTest::concurrentActivationBeforeRestartFailsClosedWithoutLaunchingStalePackage()
{
    UpdateTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem,
                               updateInstallPolicy());
    PackageStore competingStore(store.root());
    PackageInstaller competingInstaller(
        competingStore, keys.value().publicKeyPem, updateInstallPolicy());
    const QString packageB = updateSignedPackage(
        temporary, QStringLiteral("restart-b"), QStringLiteral("1.1.0"),
        keys.value().privateKeyPem);
    QVERIFY(!packageB.isEmpty());
    QVector<UpdateLaunchRequest> launches;
    ManualLifecycleClock clock;
    UpdateLifecycleCoordinator coordinator(
        QStringLiteral("company.pilot"), store, installer, {100, 150},
        [&](const UpdateLaunchRequest &request) {
            launches.push_back(request);
            return true;
        }, clock.source());
    bool activatedB = false;
    coordinator.setBeforeRelaunchCallback([&] {
        const InstallResult installedB = competingInstaller.install(packageB);
        QVERIFY2(installedB.succeeded(), qPrintable(installedB.stableError));
        activatedB = true;
    });

    clock.set(0);
    const UpdateLifecycleResult installedA = coordinator.installAndLaunch(
        updateSignedPackage(temporary, QStringLiteral("restart-a"),
                            QStringLiteral("1.0.0"),
                            keys.value().privateKeyPem));
    QVERIFY(installedA.succeeded());
    QCOMPARE(launches.size(), 1);
    const WorkerAttemptKey keyA = coordinator.currentAttemptKey().value();
    clock.set(1);
    QCOMPARE(coordinator.authenticatedHandshake(keyA),
             UpdateLifecycleAction::None);

    clock.set(10);
    QCOMPARE(coordinator.workerExited(keyA, WorkerExitReason::Crashed),
             UpdateLifecycleAction::FailedClosed);
    QVERIFY(activatedB);
    QVERIFY(coordinator.failedClosed());
    QCOMPARE(launches.size(), 1);
    QCOMPARE(store.activationState(QStringLiteral("company.pilot")).state.current,
             QFileInfo(competingStore.resolveCurrent(
                 QStringLiteral("company.pilot")).path).fileName());
}

void CrashRollbackTest::commitsRollbackBeforeLaunchingRecoveredRealLpacWorker()
{
#ifndef Q_OS_WIN
    QSKIP("LPAC worker lifecycle is Windows-specific");
#else
    UpdateTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem,
                               updateInstallPolicy());

    QVector<QString> launchVersions;
    QVector<bool> stateCommittedAtLaunch;
    QVector<bool> stoppedBeforeRelaunch;
    bool stopObserved = false;
    std::unique_ptr<WorkerTestEnvironment> workerEnvironment;
    std::unique_ptr<WorkerTestEnvironment::Launch> workerLaunch;
    ManualLifecycleClock clock;
    UpdateLifecycleCoordinator *coordinatorPointer = nullptr;
    UpdateLifecycleCoordinator coordinator(
        QStringLiteral("company.pilot"), store, installer, {2'000, 500},
        [&](const UpdateLaunchRequest &request) {
            launchVersions.push_back(request.packageVersion);
            const ActivationState state = store.activationState(
                QStringLiteral("company.pilot")).state;
            stateCommittedAtLaunch.push_back(
                state.current == QFileInfo(request.packageDirectory).fileName());
            if (launchVersions.size() > 1) {
                stoppedBeforeRelaunch.push_back(stopObserved);
            }
            stopObserved = false;
            if (request.packageVersion != QStringLiteral("1.2.0")
                && !request.recovery) {
                return true;
            }
            const QByteArray qml = request.packageVersion == QStringLiteral("1.2.0")
                ? QByteArrayLiteral(
                      "import QtQuick\nItem { Timer { interval: 500; running: true; "
                      "onTriggered: Qt.quit() } }")
                : QByteArrayLiteral(
                      "import QtQuick\nItem { width: 320; height: 200; "
                      "Component.onCompleted: Runtime.invoke(\"storage\", \"get\", "
                      "{ kind: \"lkgRecovered\" }) }");
            workerEnvironment = std::make_unique<WorkerTestEnvironment>(qml);
            if (!workerEnvironment->isValid()) return false;
            const QString nonce = QUuid::createUuid().toString(QUuid::Id128);
            auto launched = workerEnvironment->launch(nonce, nonce, 50);
            if (!launched.has_value()) return false;
            workerLaunch = std::make_unique<WorkerTestEnvironment::Launch>(
                std::move(*launched));
            return coordinatorPointer != nullptr;
        }, clock.source());
    coordinatorPointer = &coordinator;
    coordinator.setBeforeRelaunchCallback([&] { stopObserved = true; });

    clock.set(0);
    const UpdateLifecycleResult first = coordinator.installAndLaunch(
        updateSignedPackage(temporary, QStringLiteral("one"),
                            QStringLiteral("1.0.0"), keys.value().privateKeyPem));
    QVERIFY(first.succeeded());
    WorkerAttemptKey healthyKey = coordinator.currentAttemptKey().value();
    clock.set(1);
    (void)coordinator.authenticatedHandshake(healthyKey);
    for (qint64 now = 401; now < 2'001; now += 400) {
        clock.set(now);
        QCOMPARE(coordinator.heartbeat(healthyKey),
                 UpdateLifecycleAction::None);
    }
    clock.set(2'001);
    QCOMPARE(coordinator.heartbeat(healthyKey),
             UpdateLifecycleAction::MarkedHealthy);
    clock.set(3'000);
    const UpdateLifecycleResult stable = coordinator.installAndLaunch(
        updateSignedPackage(temporary, QStringLiteral("two"),
                            QStringLiteral("1.1.0"), keys.value().privateKeyPem));
    QVERIFY(stable.succeeded());
    healthyKey = coordinator.currentAttemptKey().value();
    clock.set(3'001);
    (void)coordinator.authenticatedHandshake(healthyKey);
    for (qint64 now = 3'401; now < 5'001; now += 400) {
        clock.set(now);
        QCOMPARE(coordinator.heartbeat(healthyKey),
                 UpdateLifecycleAction::None);
    }
    clock.set(5'001);
    QCOMPARE(coordinator.heartbeat(healthyKey),
             UpdateLifecycleAction::MarkedHealthy);

    clock.set(6'000);
    const UpdateLifecycleResult crashing = coordinator.installAndLaunch(
        updateSignedPackage(temporary, QStringLiteral("crashing"),
                            QStringLiteral("1.2.0"), keys.value().privateKeyPem,
                            false,
                            QByteArrayLiteral(
                                "import QtQuick\nItem { Timer { interval: 500; running: true; "
                                "onTriggered: Qt.quit() } }")));
    QVERIFY2(crashing.succeeded(), qPrintable(crashing.stableError));
    QVERIFY(workerLaunch != nullptr);
    const WorkerAttemptKey firstKey = coordinator.currentAttemptKey().value();
    const SessionReceiveResult handshake = receiveUntil(
        workerLaunch->hostSession, ProtocolType::Handshake);
    QCOMPARE(handshake.status, SessionStatus::MessageReady);
    clock.set(6'010);
    QCOMPARE(coordinator.authenticatedHandshake(firstKey),
             UpdateLifecycleAction::None);
    workerLaunch->process.terminate(ERROR_PROCESS_ABORTED);
    QVERIFY(workerLaunch->process.waitForFinished(10'000));
    workerLaunch->hostSession.close();
    QVERIFY(workerLaunch->process.close().value.has_value());

    clock.set(6'600);
    QCOMPARE(coordinator.workerExited(firstKey, WorkerExitReason::Crashed),
             UpdateLifecycleAction::Restarted);
    QVERIFY(workerLaunch != nullptr);
    const WorkerAttemptKey secondKey = coordinator.currentAttemptKey().value();
    QCOMPARE(receiveUntil(workerLaunch->hostSession, ProtocolType::Handshake).status,
             SessionStatus::MessageReady);
    clock.set(6'610);
    QCOMPARE(coordinator.authenticatedHandshake(secondKey),
             UpdateLifecycleAction::None);
    workerLaunch->process.terminate(ERROR_PROCESS_ABORTED);
    QVERIFY(workerLaunch->process.waitForFinished(10'000));
    workerLaunch->hostSession.close();
    QVERIFY(workerLaunch->process.close().value.has_value());

    clock.set(7'200);
    QCOMPARE(coordinator.workerExited(secondKey, WorkerExitReason::Crashed),
             UpdateLifecycleAction::RolledBackAndLaunched);
    QCOMPARE(launchVersions.back(), QStringLiteral("1.1.0"));
    QVERIFY(stateCommittedAtLaunch.back());
    QVERIFY(std::ranges::all_of(stoppedBeforeRelaunch,
                                [](const bool value) { return value; }));
    const ActivationState rolledBack = store.activationState(
        QStringLiteral("company.pilot")).state;
    QCOMPARE(rolledBack.current, QFileInfo(stable.path).fileName());
    QCOMPARE(rolledBack.lastKnownGood, QFileInfo(stable.path).fileName());
    QVERIFY(workerLaunch != nullptr);
    const WorkerAttemptKey recoveredKey = coordinator.currentAttemptKey().value();
    QCOMPARE(receiveUntil(workerLaunch->hostSession, ProtocolType::Handshake).status,
             SessionStatus::MessageReady);
    clock.set(7'210);
    QCOMPARE(coordinator.authenticatedHandshake(recoveredKey),
             UpdateLifecycleAction::None);
    QCOMPARE(receiveUntil(workerLaunch->hostSession, ProtocolType::SurfaceReady).status,
             SessionStatus::MessageReady);
    QCOMPARE(receiveUntil(workerLaunch->hostSession, ProtocolType::Ready).status,
             SessionStatus::MessageReady);
    const SessionReceiveResult capability = receiveUntil(
        workerLaunch->hostSession, ProtocolType::Request, 5'000);
    QCOMPARE(capability.status, SessionStatus::MessageReady);
    QCOMPARE(capability.message->payload().value(QStringLiteral("capability")).toString(),
             QStringLiteral("storage"));
    QCOMPARE(capability.message->payload().value(QStringLiteral("payload")).toObject()
                 .value(QStringLiteral("kind")).toString(),
             QStringLiteral("lkgRecovered"));
    const auto response = ProtocolMessage::successResponse(
        capability.message->requestId(), {});
    QVERIFY(response.has_value());
    QVERIFY(workerLaunch->hostSession.send(*response, 5'000));

    clock.set(7'211);
    QCOMPARE(coordinator.workerExited(secondKey, WorkerExitReason::Crashed),
             UpdateLifecycleAction::IgnoredStaleAttempt);
    QCOMPARE(launchVersions.count(QStringLiteral("1.1.0")), 2);
    workerLaunch->hostSession.close();
    workerLaunch->process.terminate(ERROR_PROCESS_ABORTED);
    QVERIFY(workerLaunch->process.waitForFinished(5'000));
    QVERIFY(workerLaunch->process.close().value.has_value());
#endif
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    CrashRollbackTest test;
    if (argc == 1) {
        char outputOption[] = "-o";
        char outputTarget[] = "-,txt";
        char *arguments[] = {argv[0], outputOption, outputTarget};
        return QTest::qExec(&test, 3, arguments);
    }
    return QTest::qExec(&test, argc, argv);
}

#include "tst_crash_rollback.moc"
