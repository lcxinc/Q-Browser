#include "UpdateLifecycleCoordinator.h"
#include "UpdateTestSupport.h"
#include "EventRecorder.h"

#include <QFileInfo>
#include <QFile>
#include <QTest>

#include <algorithm>

class UpdateLifecycleTest final : public QObject
{
    Q_OBJECT

private slots:
    void marksOnlyAuthenticatedContinuouslyHealthyVersionsAsLkg();
    void wallClockJumpsDoNotAffectHealthAndHealthyCommitsOnce();
    void staleAdmissionFailureDoesNotPoisonCurrentAttempt();
    void recordsBoundedRouteAcknowledgementWithEmptyPendingQueue();
};

void UpdateLifecycleTest::recordsBoundedRouteAcknowledgementWithEmptyPendingQueue()
{
    UpdateTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem, updateInstallPolicy());
    const QString telemetry = temporary.filePath(QStringLiteral("telemetry"));
    QVERIFY(QDir().mkpath(telemetry));
    EventRecorder recorder({telemetry, QStringLiteral("events.jsonl"), 64 * 1024, 1, 64});
    QVector<UpdateLaunchRequest> launches;
    ManualLifecycleClock clock;
    UpdateLifecycleCoordinator coordinator(
        QStringLiteral("company.pilot"), store, installer, {1'000, 300},
        [&](const UpdateLaunchRequest &request) { launches.push_back(request); return true; },
        clock.source(), &recorder);
    const QString package = updateSignedPackage(
        temporary, QStringLiteral("route-ack"), QStringLiteral("1.0.0"),
        keys.value().privateKeyPem);
    QVERIFY(coordinator.installAndLaunch(package).succeeded());
    coordinator.recordRouteLoadAcknowledged(QStringLiteral("/orders/:id"), 0);
    coordinator.recordRouteLoadAcknowledged(QStringLiteral("/orders/secret-id"), 0);
    coordinator.recordRouteLoadAcknowledged(QStringLiteral("/orders"), 1);
    QVERIFY(recorder.flush(5'000));
    QFile events(QDir(telemetry).filePath(QStringLiteral("events.jsonl")));
    QVERIFY(events.open(QIODevice::ReadOnly));
    const QByteArray data = events.readAll();
    QVERIFY(data.contains("\"phase\":\"worker\",\"code\":\"completed\""));
    QVERIFY(data.contains("\"routeTemplate\":\"/orders/:id\""));
    QVERIFY(data.contains("\"metrics\":{\"queueDepth\":0}"));
    QVERIFY(!data.contains("secret-id"));
    QVERIFY(!data.contains("\"queueDepth\":1"));
}

void UpdateLifecycleTest::staleAdmissionFailureDoesNotPoisonCurrentAttempt()
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
        QStringLiteral("company.pilot"), store, installer, {1'000, 300},
        [&](const UpdateLaunchRequest &request) {
            launches.push_back(request);
            return true;
        }, clock.source());

    const QString packageA = updateSignedPackage(
        temporary, QStringLiteral("stale-admission-a"),
        QStringLiteral("1.0.0"), keys.value().privateKeyPem);
    const QString packageB = updateSignedPackage(
        temporary, QStringLiteral("stale-admission-b"),
        QStringLiteral("1.1.0"), keys.value().privateKeyPem);
    QVERIFY(!packageA.isEmpty());
    QVERIFY(!packageB.isEmpty());

    clock.set(0);
    QVERIFY(coordinator.installAndLaunch(packageA).succeeded());
    const WorkerAttemptKey keyA = launches.back().key;
    clock.set(1);
    QVERIFY(coordinator.installAndLaunch(packageB).succeeded());
    const WorkerAttemptKey keyB = launches.back().key;
    QVERIFY(keyA != keyB);
    const ActivationState stateB = store.activationState(
        QStringLiteral("company.pilot")).state;

    QCOMPARE(coordinator.workerAdmissionFailed(keyA),
             UpdateLifecycleAction::IgnoredStaleAttempt);
    QVERIFY(!coordinator.failedClosed());
    QCOMPARE(coordinator.currentAttemptKey(), std::optional{keyB});
    QCOMPARE(store.activationState(QStringLiteral("company.pilot")).state,
             stateB);
}

void UpdateLifecycleTest::wallClockJumpsDoNotAffectHealthAndHealthyCommitsOnce()
{
    UpdateTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    QVERIFY(keys.hasValue());
    PackageStore store(temporary.filePath(QStringLiteral("store")));
    PackageInstaller installer(store, keys.value().publicKeyPem,
                               updateInstallPolicy());
    const QString telemetryDirectory = temporary.filePath(
        QStringLiteral("telemetry"));
    QVERIFY(QDir().mkpath(telemetryDirectory));
    EventRecorder recorder({telemetryDirectory, QStringLiteral("events.jsonl"),
                            64 * 1024, 1, 64});
    QVERIFY(recorder.isValid());
    qint64 steadyNow = 0;
    qint64 utcNow = 1'900'000'000'000;
    LifecycleClock clock{
        [&] { return steadyNow; },
        [&] { return utcNow; },
    };
    QVector<UpdateLaunchRequest> launches;
    UpdateLifecycleCoordinator coordinator(
        QStringLiteral("company.pilot"), store, installer, {1'000, 300},
        [&](const UpdateLaunchRequest &request) {
            launches.push_back(request);
            return true;
        },
        clock, &recorder);
    const QString package = updateSignedPackage(
        temporary, QStringLiteral("clock"), QStringLiteral("1.0.0"),
        keys.value().privateKeyPem);
    QVERIFY(coordinator.installAndLaunch(package).succeeded());
    const WorkerAttemptKey key = launches.back().key;
    QCOMPARE(coordinator.authenticatedHandshake(key),
             UpdateLifecycleAction::None);
    const qint64 generationBeforeHealthy = store.activationState(
        QStringLiteral("company.pilot")).state.generation;
    for (steadyNow = 200; steadyNow < 1'000; steadyNow += 200) {
        utcNow = (steadyNow == 400) ? 1 : 8'000'000'000'000;
        QCOMPARE(coordinator.heartbeat(key), UpdateLifecycleAction::None);
    }
    steadyNow = 1'000;
    utcNow = 2;
    QCOMPARE(coordinator.heartbeat(key), UpdateLifecycleAction::MarkedHealthy);
    QCOMPARE(coordinator.checkHealth(key), UpdateLifecycleAction::None);
    const ActivationState healthy = store.activationState(
        QStringLiteral("company.pilot")).state;
    QCOMPARE(healthy.generation, generationBeforeHealthy + 1);
    QCOMPARE(coordinator.heartbeat(key), UpdateLifecycleAction::None);
    QCOMPARE(store.activationState(QStringLiteral("company.pilot"))
                 .state.generation,
             healthy.generation);
    QVERIFY(recorder.flush(5'000));
    QFile events(QDir(telemetryDirectory).filePath(QStringLiteral("events.jsonl")));
    QVERIFY(events.open(QIODevice::ReadOnly));
    const QList<QByteArray> lines = events.readAll().split('\n');
    QCOMPARE(std::ranges::count_if(lines, [](const QByteArray &line) {
                 return line.contains(QByteArrayLiteral("\"code\":\"healthy\""));
             }),
             1);
}

void UpdateLifecycleTest::marksOnlyAuthenticatedContinuouslyHealthyVersionsAsLkg()
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
        QStringLiteral("company.pilot"), store, installer,
        {1'000, 300},
        [&](const UpdateLaunchRequest &request) {
            launches.push_back(request);
            return true;
        }, clock.source());

    const QString one = updateSignedPackage(
        temporary, QStringLiteral("one"), QStringLiteral("1.0.0"),
        keys.value().privateKeyPem);
    clock.set(0);
    const UpdateLifecycleResult first = coordinator.installAndLaunch(one);
    QVERIFY2(first.succeeded(), qPrintable(first.stableError));
    QCOMPARE(launches.size(), 1);
    const ActivationState firstState = store.activationState(
        QStringLiteral("company.pilot")).state;
    QCOMPARE(launches.back().expectedActivation.currentDirectory,
             firstState.current);
    QCOMPARE(launches.back().expectedActivation.generation,
             firstState.generation);
    QCOMPARE(launches.back().expectedActivation.versionDigestHex,
             firstState.current.sliced(firstState.current.lastIndexOf(u'-') + 1)
                 .toLatin1());
    const WorkerAttemptKey firstKey = launches.back().key;
    clock.set(100);
    QCOMPARE(coordinator.heartbeat(firstKey),
             UpdateLifecycleAction::IgnoredUntilHandshake);
    QVERIFY(store.activationState(QStringLiteral("company.pilot"))
                .state.lastKnownGood.isEmpty());
    QCOMPARE(coordinator.authenticatedHandshake(firstKey),
             UpdateLifecycleAction::None);
    for (qint64 now = 300; now < 1'100; now += 200) {
        clock.set(now);
        QCOMPARE(coordinator.heartbeat(firstKey),
                 UpdateLifecycleAction::None);
    }
    clock.set(1'100);
    QCOMPARE(coordinator.heartbeat(firstKey),
             UpdateLifecycleAction::MarkedHealthy);
    const QString firstDirectory = QFileInfo(first.path).fileName();
    QCOMPARE(store.activationState(QStringLiteral("company.pilot"))
                 .state.lastKnownGood,
             firstDirectory);

    const QString two = updateSignedPackage(
        temporary, QStringLiteral("two"), QStringLiteral("1.1.0"),
        keys.value().privateKeyPem);
    clock.set(2'000);
    const UpdateLifecycleResult second = coordinator.installAndLaunch(two);
    QVERIFY2(second.succeeded(), qPrintable(second.stableError));
    const WorkerAttemptKey secondKey = launches.back().key;
    clock.set(2'010);
    QCOMPARE(coordinator.authenticatedHandshake(secondKey),
             UpdateLifecycleAction::None);
    for (qint64 now = 2'210; now < 3'010; now += 200) {
        clock.set(now);
        (void)coordinator.heartbeat(secondKey);
    }
    clock.set(3'011);
    QCOMPARE(coordinator.heartbeat(secondKey),
             UpdateLifecycleAction::MarkedHealthy);
    const ActivationState healthy = store.activationState(
        QStringLiteral("company.pilot")).state;
    QCOMPARE(healthy.lastKnownGood, QFileInfo(second.path).fileName());

    const QString tampered = updateSignedPackage(
        temporary, QStringLiteral("tampered"), QStringLiteral("1.2.0"),
        keys.value().privateKeyPem, true);
    clock.set(4'000);
    const UpdateLifecycleResult rejected = coordinator.installAndLaunch(tampered);
    QCOMPARE(rejected.error, UpdateLifecycleError::InstallRejected);
    QCOMPARE(launches.size(), 2);
    QCOMPARE(store.activationState(QStringLiteral("company.pilot")).state.current,
             healthy.current);
    QCOMPARE(store.activationState(QStringLiteral("company.pilot"))
                 .state.lastKnownGood,
             healthy.lastKnownGood);

    clock.set(5'000);
    QCOMPARE(coordinator.heartbeat(firstKey),
             UpdateLifecycleAction::IgnoredStaleAttempt);

    const QString delayed = updateSignedPackage(
        temporary, QStringLiteral("delayed"), QStringLiteral("1.2.0"),
        keys.value().privateKeyPem);
    clock.set(6'000);
    QVERIFY(coordinator.installAndLaunch(delayed).succeeded());
    const WorkerAttemptKey delayedKey = coordinator.currentAttemptKey().value();
    clock.set(6'301);
    QCOMPARE(coordinator.authenticatedHandshake(delayedKey),
             UpdateLifecycleAction::Restarted);
    QVERIFY(coordinator.currentAttemptKey().value() != delayedKey);
    clock.set(6'302);
    QCOMPARE(coordinator.authenticatedHandshake(delayedKey),
             UpdateLifecycleAction::IgnoredStaleAttempt);
}

QTEST_APPLESS_MAIN(UpdateLifecycleTest)

#include "tst_update_lifecycle.moc"
