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
    void commitsRollbackBeforeLaunchingRecoveredRealLpacWorker();
};

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
        });
    coordinatorPointer = &coordinator;
    coordinator.setBeforeRelaunchCallback([&] { stopObserved = true; });

    const UpdateLifecycleResult first = coordinator.installAndLaunch(
        updateSignedPackage(temporary, QStringLiteral("one"),
                            QStringLiteral("1.0.0"), keys.value().privateKeyPem), 0);
    QVERIFY(first.succeeded());
    WorkerAttemptKey healthyKey = coordinator.currentAttemptKey().value();
    (void)coordinator.authenticatedHandshake(healthyKey, 1);
    for (qint64 now = 401; now < 2'001; now += 400) {
        QCOMPARE(coordinator.heartbeat(healthyKey, now),
                 UpdateLifecycleAction::None);
    }
    QCOMPARE(coordinator.heartbeat(healthyKey, 2'001),
             UpdateLifecycleAction::MarkedHealthy);
    const UpdateLifecycleResult stable = coordinator.installAndLaunch(
        updateSignedPackage(temporary, QStringLiteral("two"),
                            QStringLiteral("1.1.0"), keys.value().privateKeyPem),
        3'000);
    QVERIFY(stable.succeeded());
    healthyKey = coordinator.currentAttemptKey().value();
    (void)coordinator.authenticatedHandshake(healthyKey, 3'001);
    for (qint64 now = 3'401; now < 5'001; now += 400) {
        QCOMPARE(coordinator.heartbeat(healthyKey, now),
                 UpdateLifecycleAction::None);
    }
    QCOMPARE(coordinator.heartbeat(healthyKey, 5'001),
             UpdateLifecycleAction::MarkedHealthy);

    const UpdateLifecycleResult crashing = coordinator.installAndLaunch(
        updateSignedPackage(temporary, QStringLiteral("crashing"),
                            QStringLiteral("1.2.0"), keys.value().privateKeyPem,
                            false,
                            QByteArrayLiteral(
                                "import QtQuick\nItem { Timer { interval: 500; running: true; "
                                "onTriggered: Qt.quit() } }")),
        6'000);
    QVERIFY2(crashing.succeeded(), qPrintable(crashing.stableError));
    QVERIFY(workerLaunch != nullptr);
    const WorkerAttemptKey firstKey = coordinator.currentAttemptKey().value();
    const SessionReceiveResult handshake = receiveUntil(
        workerLaunch->hostSession, ProtocolType::Handshake);
    QCOMPARE(handshake.status, SessionStatus::MessageReady);
    QCOMPARE(coordinator.authenticatedHandshake(firstKey, 6'010),
             UpdateLifecycleAction::None);
    workerLaunch->process.terminate(ERROR_PROCESS_ABORTED);
    QVERIFY(workerLaunch->process.waitForFinished(10'000));
    workerLaunch->hostSession.close();
    QVERIFY(workerLaunch->process.close().value.has_value());

    QCOMPARE(coordinator.workerExited(firstKey, WorkerExitReason::Crashed, 6'600),
             UpdateLifecycleAction::Restarted);
    QVERIFY(workerLaunch != nullptr);
    const WorkerAttemptKey secondKey = coordinator.currentAttemptKey().value();
    QCOMPARE(receiveUntil(workerLaunch->hostSession, ProtocolType::Handshake).status,
             SessionStatus::MessageReady);
    QCOMPARE(coordinator.authenticatedHandshake(secondKey, 6'610),
             UpdateLifecycleAction::None);
    workerLaunch->process.terminate(ERROR_PROCESS_ABORTED);
    QVERIFY(workerLaunch->process.waitForFinished(10'000));
    workerLaunch->hostSession.close();
    QVERIFY(workerLaunch->process.close().value.has_value());

    QCOMPARE(coordinator.workerExited(secondKey, WorkerExitReason::Crashed, 7'200),
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
    QCOMPARE(coordinator.authenticatedHandshake(recoveredKey, 7'210),
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

    QCOMPARE(coordinator.workerExited(secondKey, WorkerExitReason::Crashed, 7'201),
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
