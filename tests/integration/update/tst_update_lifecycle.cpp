#include "UpdateLifecycleCoordinator.h"
#include "UpdateTestSupport.h"

#include <QFileInfo>
#include <QTest>

class UpdateLifecycleTest final : public QObject
{
    Q_OBJECT

private slots:
    void marksOnlyAuthenticatedContinuouslyHealthyVersionsAsLkg();
};

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
    UpdateLifecycleCoordinator coordinator(
        QStringLiteral("company.pilot"), store, installer,
        {1'000, 300},
        [&](const UpdateLaunchRequest &request) {
            launches.push_back(request);
            return true;
        });

    const QString one = updateSignedPackage(
        temporary, QStringLiteral("one"), QStringLiteral("1.0.0"),
        keys.value().privateKeyPem);
    const UpdateLifecycleResult first = coordinator.installAndLaunch(one, 0);
    QVERIFY2(first.succeeded(), qPrintable(first.stableError));
    QCOMPARE(launches.size(), 1);
    const WorkerAttemptKey firstKey = launches.back().key;
    QCOMPARE(coordinator.heartbeat(firstKey, 100),
             UpdateLifecycleAction::IgnoredUntilHandshake);
    QVERIFY(store.activationState(QStringLiteral("company.pilot"))
                .state.lastKnownGood.isEmpty());
    QCOMPARE(coordinator.authenticatedHandshake(firstKey, 100),
             UpdateLifecycleAction::None);
    for (qint64 now = 300; now < 1'100; now += 200) {
        QCOMPARE(coordinator.heartbeat(firstKey, now),
                 UpdateLifecycleAction::None);
    }
    QCOMPARE(coordinator.heartbeat(firstKey, 1'100),
             UpdateLifecycleAction::MarkedHealthy);
    const QString firstDirectory = QFileInfo(first.path).fileName();
    QCOMPARE(store.activationState(QStringLiteral("company.pilot"))
                 .state.lastKnownGood,
             firstDirectory);

    const QString two = updateSignedPackage(
        temporary, QStringLiteral("two"), QStringLiteral("1.1.0"),
        keys.value().privateKeyPem);
    const UpdateLifecycleResult second = coordinator.installAndLaunch(two, 2'000);
    QVERIFY2(second.succeeded(), qPrintable(second.stableError));
    const WorkerAttemptKey secondKey = launches.back().key;
    QCOMPARE(coordinator.authenticatedHandshake(secondKey, 2'010),
             UpdateLifecycleAction::None);
    for (qint64 now = 2'210; now < 3'010; now += 200) {
        (void)coordinator.heartbeat(secondKey, now);
    }
    QCOMPARE(coordinator.heartbeat(secondKey, 3'011),
             UpdateLifecycleAction::MarkedHealthy);
    const ActivationState healthy = store.activationState(
        QStringLiteral("company.pilot")).state;
    QCOMPARE(healthy.lastKnownGood, QFileInfo(second.path).fileName());

    const QString tampered = updateSignedPackage(
        temporary, QStringLiteral("tampered"), QStringLiteral("1.2.0"),
        keys.value().privateKeyPem, true);
    const UpdateLifecycleResult rejected = coordinator.installAndLaunch(
        tampered, 4'000);
    QCOMPARE(rejected.error, UpdateLifecycleError::InstallRejected);
    QCOMPARE(launches.size(), 2);
    QCOMPARE(store.activationState(QStringLiteral("company.pilot")).state.current,
             healthy.current);
    QCOMPARE(store.activationState(QStringLiteral("company.pilot"))
                 .state.lastKnownGood,
             healthy.lastKnownGood);

    QCOMPARE(coordinator.heartbeat(firstKey, 5'000),
             UpdateLifecycleAction::IgnoredStaleAttempt);

    const QString delayed = updateSignedPackage(
        temporary, QStringLiteral("delayed"), QStringLiteral("1.2.0"),
        keys.value().privateKeyPem);
    QVERIFY(coordinator.installAndLaunch(delayed, 6'000).succeeded());
    const WorkerAttemptKey delayedKey = coordinator.currentAttemptKey().value();
    QCOMPARE(coordinator.authenticatedHandshake(delayedKey, 6'301),
             UpdateLifecycleAction::Restarted);
    QVERIFY(coordinator.currentAttemptKey().value() != delayedKey);
    QCOMPARE(coordinator.authenticatedHandshake(delayedKey, 6'302),
             UpdateLifecycleAction::IgnoredStaleAttempt);
}

QTEST_APPLESS_MAIN(UpdateLifecycleTest)

#include "tst_update_lifecycle.moc"
