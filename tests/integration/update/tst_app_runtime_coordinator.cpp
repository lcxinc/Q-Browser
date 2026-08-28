#include "AppRuntimeCoordinator.h"
#include "PackageStoreTestHooks.h"
#include "UpdateTestSupport.h"

#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QTest>

#include <algorithm>
#include <memory>

namespace {

struct CoordinatorHarness final
{
    UpdateTemporaryDir temporary;
    SignatureKeyPairResult keys = SignatureVerifier::generateKeyPair();
    std::unique_ptr<PackageStore> store;
    std::unique_ptr<PackageInstaller> installer;
    ManualLifecycleClock clock;
    QVector<AuthorityDrainBatch> drains;
    std::unique_ptr<AppRuntimeCoordinator> coordinator;

    CoordinatorHarness()
    {
        store = std::make_unique<PackageStore>(
            temporary.filePath(QStringLiteral("store")));
        installer = std::make_unique<PackageInstaller>(
            *store, keys.value().publicKeyPem, updateInstallPolicy());
        coordinator = std::make_unique<AppRuntimeCoordinator>(
            QStringLiteral("company.pilot"), *store, *installer,
            WorkerSupervisionPolicy{100, 1'000}, clock.source(), nullptr,
            [this](const AuthorityDrainBatch &batch) {
                drains.push_back(batch);
            });
    }

    QString package(const QString &name, const QString &version,
                    const QString &appId = QStringLiteral("company.pilot"))
    {
        return updateSignedPackage(temporary, name, version,
                                   keys.value().privateKeyPem, false, {},
                                   appId);
    }
};

TabLaunchAuthority tab(const QString &id, const quint64 incarnation = 1)
{
    return {id, incarnation};
}

FullAttemptKey fullKey(const AppRuntimeAction &action)
{
    Q_ASSERT(action.launch.has_value());
    return {{action.tabId, action.runtimeIncarnation},
            action.launch->attempt,
            action.launch->lease.leaseAuthorityEpoch};
}

const AppRuntimeAction &onlyAction(const AppRuntimeResult &result,
                                   const AppRuntimeActionKind kind)
{
    const auto found = std::ranges::find_if(
        result.actions,
        [kind](const AppRuntimeAction &action) { return action.kind == kind; });
    Q_ASSERT(found != result.actions.cend());
    Q_ASSERT(std::ranges::count_if(
                 result.actions,
                 [kind](const AppRuntimeAction &action) {
                     return action.kind == kind;
                 }) == 1);
    return *found;
}

} // namespace

class AppRuntimeCoordinatorTest final : public QObject
{
    Q_OBJECT

private slots:
    void newTabsUseCandidateWhileExistingTabsRemainPinned();
    void pinnedOldTabRestartsItsPinnedVersion();
    void candidateTabsHaveIndependentAttempts();
    void admissionRequiresTheCompleteLaunchRequest();
    void firstHealthyCandidateMarksLkgExactlyOnce();
    void lkgGenerationChangeDoesNotPoisonSiblingAdmission();
    void revokedLeaseCannotPassAdmission();
    void candidateCrashLoopRollsBackOnce();
    void rollbackRevokesEveryFailedCandidateLease();
    void multiTabRollbackReturnsEveryActionInStableRevokeStopRecoverOrder();
    void candidateRollbackDoesNotRevokePinnedOldVersionTab();
    void lateCandidateEventsAreIgnored();
    void rollbackDoesNotAffectAnotherApp();
    void activeNewTabWithInstallPackageStartsZeroWorkers();
    void userReloadUsesCurrentCandidateWhileCrashRestartUsesPinnedLease();
    void drainTimeoutIsFailedClosedAndNeverRecovers();
    void shutdownRetiresEveryTabAndRejectsNewEvents();
    void staleCandidateEventsCannotAffectNewCandidate();
    void lateHeartbeatCannotPromoteCandidate();
    void promotionIssuesLeasesWithNewGeneration();
    void drainCompletionAfterDeadlineRetainsIsolationActions();
    void rollbackFailureStopsAndPermanentlyIsolates();
    void pendingDrainAndShutdownNeverRelauch();
    void revokedTokenCannotPassRepeatedAdmission();
};

void AppRuntimeCoordinatorTest::newTabsUseCandidateWhileExistingTabsRemainPinned()
{
    CoordinatorHarness h;
    QVERIFY(h.temporary.isValid());
    QVERIFY(h.keys.hasValue());
    QVERIFY(h.coordinator->installAndActivate(h.package("one", "1.0.0"), 0)
                .code == AppRuntimeResultCode::Applied);

    const AppRuntimeResult oldLaunch = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("old")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 1);
    const auto &oldAction = onlyAction(oldLaunch, AppRuntimeActionKind::Launch);
    QCOMPARE(oldAction.launch->lease.version, QStringLiteral("1.0.0"));

    QVERIFY(h.coordinator->installAndActivate(h.package("two", "1.1.0"), 2)
                .code == AppRuntimeResultCode::Applied);
    const AppRuntimeResult newLaunch = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("new")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 3);
    const auto &newAction = onlyAction(newLaunch, AppRuntimeActionKind::Launch);
    QCOMPARE(newAction.launch->lease.version, QStringLiteral("1.1.0"));
    QCOMPARE(oldAction.launch->lease.version, QStringLiteral("1.0.0"));
}

void AppRuntimeCoordinatorTest::pinnedOldTabRestartsItsPinnedVersion()
{
    CoordinatorHarness h;
    QVERIFY(h.keys.hasValue());
    QVERIFY(h.coordinator->installAndActivate(h.package("one", "1.0.0"), 0)
                .code == AppRuntimeResultCode::Applied);
    const auto launch = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("old")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 1);
    const auto &first = onlyAction(launch, AppRuntimeActionKind::Launch);
    const FullAttemptKey firstKey = fullKey(first);
    QVERIFY(h.coordinator->installAndActivate(h.package("two", "1.1.0"), 2)
                .code == AppRuntimeResultCode::Applied);
    const AppRuntimeResult restarted = h.coordinator->workerExited(
        firstKey, WorkerExitReason::Crashed, 10);
    const auto &restart = onlyAction(restarted, AppRuntimeActionKind::Launch);
    QCOMPARE(restart.launch->revalidationMode,
             PackageRevalidationMode::PinnedLease);
    QCOMPARE(restart.launch->lease.version, QStringLiteral("1.0.0"));
    QVERIFY(restart.launch->recovery);
}

void AppRuntimeCoordinatorTest::candidateTabsHaveIndependentAttempts()
{
    CoordinatorHarness h;
    QVERIFY(h.keys.hasValue());
    QVERIFY(h.coordinator->installAndActivate(h.package("one", "1.0.0"), 0)
                .code == AppRuntimeResultCode::Applied);
    const auto a = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("a")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 1);
    const auto b = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("b")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 2);
    const FullAttemptKey keyA = fullKey(onlyAction(a, AppRuntimeActionKind::Launch));
    const FullAttemptKey keyB = fullKey(onlyAction(b, AppRuntimeActionKind::Launch));
    QVERIFY(keyA.tab.tabId != keyB.tab.tabId);
    QVERIFY(keyA.leaseAuthorityEpoch != keyB.leaseAuthorityEpoch);
    QCOMPARE(h.coordinator->admitAuthenticatedWorker(
                  keyA, a.actions.front().launch->lease)
                  .code,
             AppRuntimeResultCode::Applied);
    QCOMPARE(h.coordinator->admitAuthenticatedWorker(
                  keyB, b.actions.front().launch->lease)
                  .code,
             AppRuntimeResultCode::Applied);
}

void AppRuntimeCoordinatorTest::admissionRequiresTheCompleteLaunchRequest()
{
    CoordinatorHarness h;
    QVERIFY(h.keys.hasValue());
    QVERIFY(h.coordinator->installAndActivate(h.package("one", "1.0.0"), 0)
                .code == AppRuntimeResultCode::Applied);
    const auto launch = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("a")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 1);
    const auto &action = onlyAction(launch, AppRuntimeActionKind::Launch);
    WorkerLaunchRequest altered = *action.launch;
    altered.route = QStringLiteral("/late");
    QCOMPARE(h.coordinator->admitAuthenticatedWorker(altered, 1).code,
             AppRuntimeResultCode::Rejected);
    altered = *action.launch;
    altered.admission = std::make_shared<AuthorityAdmissionToken>();
    QCOMPARE(h.coordinator->admitAuthenticatedWorker(altered, 1).code,
             AppRuntimeResultCode::Rejected);
    QCOMPARE(h.coordinator->admitAuthenticatedWorker(*action.launch, 1).code,
             AppRuntimeResultCode::Applied);
}

void AppRuntimeCoordinatorTest::firstHealthyCandidateMarksLkgExactlyOnce()
{
    CoordinatorHarness h;
    QVERIFY(h.keys.hasValue());
    QVERIFY(h.coordinator->installAndActivate(h.package("one", "1.0.0"), 0)
                .code == AppRuntimeResultCode::Applied);
    const auto first = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("a")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 1);
    const auto &action = onlyAction(first, AppRuntimeActionKind::Launch);
    const FullAttemptKey key = fullKey(action);
    QVERIFY(h.coordinator->admitAuthenticatedWorker(key,
                                                    action.launch->lease)
                .code == AppRuntimeResultCode::Applied);
    QCOMPARE(h.coordinator->heartbeat(key, 90).code,
             AppRuntimeResultCode::Applied);
    QCOMPARE(h.coordinator->heartbeat(key, 101).code,
             AppRuntimeResultCode::Applied);
    const ActivationState state = h.store->activationState(
        QStringLiteral("company.pilot")).state;
    QCOMPARE(state.lastKnownGood, state.current);
    const qint64 generation = state.generation;
    QCOMPARE(h.coordinator->heartbeat(key, 110).code,
             AppRuntimeResultCode::Applied);
    QCOMPARE(h.store->activationState(QStringLiteral("company.pilot"))
                 .state.generation,
             generation);
}

void AppRuntimeCoordinatorTest::lkgGenerationChangeDoesNotPoisonSiblingAdmission()
{
    CoordinatorHarness h;
    QVERIFY(h.keys.hasValue());
    QVERIFY(h.coordinator->installAndActivate(h.package("one", "1.0.0"), 0)
                .code == AppRuntimeResultCode::Applied);
    const auto a = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("a")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 1);
    const auto b = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("b")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 2);
    const auto &aAction = onlyAction(a, AppRuntimeActionKind::Launch);
    const auto &bAction = onlyAction(b, AppRuntimeActionKind::Launch);
    const FullAttemptKey keyA = fullKey(aAction);
    const FullAttemptKey keyB = fullKey(bAction);
    QVERIFY(h.coordinator->admitAuthenticatedWorker(keyA, aAction.launch->lease)
                .code == AppRuntimeResultCode::Applied);
    QVERIFY(h.coordinator->admitAuthenticatedWorker(keyB, bAction.launch->lease)
                .code == AppRuntimeResultCode::Applied);
    QVERIFY(h.coordinator->heartbeat(keyA, 90).code
            == AppRuntimeResultCode::Applied);
    QVERIFY(h.coordinator->heartbeat(keyB, 91).code
            == AppRuntimeResultCode::Applied);
    QVERIFY(h.coordinator->heartbeat(keyA, 101).code
            == AppRuntimeResultCode::Applied);
    QVERIFY(h.coordinator->heartbeat(keyB, 102).code
            == AppRuntimeResultCode::Applied);
}

void AppRuntimeCoordinatorTest::revokedLeaseCannotPassAdmission()
{
    CoordinatorHarness h;
    QVERIFY(h.keys.hasValue());
    QVERIFY(h.coordinator->installAndActivate(h.package("one", "1.0.0"), 0)
                .code == AppRuntimeResultCode::Applied);
    const auto launch = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("a")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 1);
    const auto &action = onlyAction(launch, AppRuntimeActionKind::Launch);
    const FullAttemptKey key = fullKey(action);
    QVERIFY(h.coordinator->closeTab(tab(QStringLiteral("a"))).code
            == AppRuntimeResultCode::Applied);
    QCOMPARE(h.coordinator->admitAuthenticatedWorker(key, action.launch->lease)
                 .code,
             AppRuntimeResultCode::Rejected);
}

void AppRuntimeCoordinatorTest::candidateCrashLoopRollsBackOnce()
{
    CoordinatorHarness h;
    QVERIFY(h.keys.hasValue());
    QVERIFY(h.coordinator->installAndActivate(h.package("one", "1.0.0"), 0)
                .code == AppRuntimeResultCode::Applied);
    const auto launch = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("a")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 1);
    const auto &first = onlyAction(launch, AppRuntimeActionKind::Launch);
    const FullAttemptKey key = fullKey(first);
    QVERIFY(h.coordinator->installAndActivate(h.package("two", "1.1.0"), 2)
                .code == AppRuntimeResultCode::Applied);
    const auto candidateLaunch = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("a")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 3);
    const auto &candidateAction = onlyAction(candidateLaunch,
                                             AppRuntimeActionKind::Launch);
    const auto restart = h.coordinator->workerExited(
        fullKey(candidateAction), WorkerExitReason::Crashed, 10);
    const auto &second = onlyAction(restart, AppRuntimeActionKind::Launch);
    const FullAttemptKey secondKey = fullKey(second);
    const AppRuntimeResult rollback = h.coordinator->workerExited(
        secondKey, WorkerExitReason::Crashed, 11);
    QVERIFY(rollback.code == AppRuntimeResultCode::Applied);
    QVERIFY(std::ranges::any_of(rollback.actions, [](const auto &action) {
        return action.kind == AppRuntimeActionKind::AwaitAuthorityDrain;
    }));
    const AppRuntimeResult duplicate = h.coordinator->workerExited(
        secondKey, WorkerExitReason::Crashed, 12);
    QVERIFY(duplicate.code == AppRuntimeResultCode::IgnoredStale);
}

void AppRuntimeCoordinatorTest::rollbackRevokesEveryFailedCandidateLease()
{
    CoordinatorHarness h;
    QVERIFY(h.keys.hasValue());
    QVERIFY(h.coordinator->installAndActivate(h.package("one", "1.0.0"), 0)
                .code == AppRuntimeResultCode::Applied);
    const auto old = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("old")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 1);
    QVERIFY(h.coordinator->installAndActivate(h.package("two", "1.1.0"), 2)
                .code == AppRuntimeResultCode::Applied);
    const auto failed = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("failed")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 3);
    const auto sibling = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("sibling")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 4);
    const auto &failedAction = onlyAction(failed, AppRuntimeActionKind::Launch);
    const FullAttemptKey failedKey = fullKey(failedAction);
    const auto restart = h.coordinator->workerExited(
        failedKey, WorkerExitReason::Crashed, 10);
    const auto &restartAction = onlyAction(restart, AppRuntimeActionKind::Launch);
    const AppRuntimeResult rollback = h.coordinator->workerExited(
        fullKey(restartAction), WorkerExitReason::Crashed, 11);
    QVERIFY(!rollback.actions.isEmpty());
    QVERIFY(std::ranges::all_of(rollback.actions, [](const auto &action) {
        return action.kind == AppRuntimeActionKind::Revoke
            || action.kind == AppRuntimeActionKind::Stop
            || action.kind == AppRuntimeActionKind::AwaitAuthorityDrain;
    }));
    QVERIFY(std::ranges::none_of(rollback.actions, [&](const auto &action) {
        return action.tabId == old.actions.front().tabId;
    }));
    QVERIFY(std::ranges::any_of(rollback.actions, [&](const auto &action) {
        return action.tabId == sibling.actions.front().tabId;
    }));
}

void AppRuntimeCoordinatorTest::multiTabRollbackReturnsEveryActionInStableRevokeStopRecoverOrder()
{
    CoordinatorHarness h;
    QVERIFY(h.keys.hasValue());
    QVERIFY(h.coordinator->installAndActivate(h.package("one", "1.0.0"), 0)
                .code == AppRuntimeResultCode::Applied);
    QVERIFY(h.coordinator->installAndActivate(h.package("two", "1.1.0"), 1)
                .code == AppRuntimeResultCode::Applied);
    const auto a = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("z")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 2);
    const auto b = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("a")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 3);
    const auto &aLaunch = onlyAction(a, AppRuntimeActionKind::Launch);
    const auto &bLaunch = onlyAction(b, AppRuntimeActionKind::Launch);
    const auto r1 = h.coordinator->workerExited(
        fullKey(aLaunch), WorkerExitReason::Crashed, 10);
    const auto &r1Launch = onlyAction(r1, AppRuntimeActionKind::Launch);
    const auto r2 = h.coordinator->workerExited(
        fullKey(r1Launch), WorkerExitReason::Crashed, 11);
    QVERIFY(!r2.actions.isEmpty());
    QVERIFY(r2.actions.size() >= 5);
    QCOMPARE(r2.actions.at(0).kind, AppRuntimeActionKind::Revoke);
    QCOMPARE(r2.actions.at(0).tabId, QStringLiteral("a"));
    QCOMPARE(r2.actions.at(1).kind, AppRuntimeActionKind::Revoke);
    QCOMPARE(r2.actions.at(1).tabId, QStringLiteral("z"));
    QCOMPARE(r2.actions.at(2).kind, AppRuntimeActionKind::Stop);
    QCOMPARE(r2.actions.at(3).kind, AppRuntimeActionKind::Stop);
    QCOMPARE(r2.actions.at(4).kind, AppRuntimeActionKind::AwaitAuthorityDrain);
    QVERIFY(!h.drains.isEmpty());
    const AppRuntimeResult recovered = h.coordinator->authorityDrainCompleted(
        h.drains.back().id, 12);
    QVERIFY(std::ranges::all_of(recovered.actions, [](const auto &action) {
        return action.kind == AppRuntimeActionKind::RecoverFromLkg
            || action.kind == AppRuntimeActionKind::Launch;
    }));
    Q_UNUSED(bLaunch);
}

void AppRuntimeCoordinatorTest::candidateRollbackDoesNotRevokePinnedOldVersionTab()
{
    CoordinatorHarness h;
    QVERIFY(h.keys.hasValue());
    QVERIFY(h.coordinator->installAndActivate(h.package("one", "1.0.0"), 0)
                .code == AppRuntimeResultCode::Applied);
    const auto old = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("old")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 1);
    QVERIFY(h.coordinator->installAndActivate(h.package("two", "1.1.0"), 2)
                .code == AppRuntimeResultCode::Applied);
    const auto candidate = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("candidate")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 3);
    const auto &candidateAction = onlyAction(candidate, AppRuntimeActionKind::Launch);
    const auto restart = h.coordinator->workerExited(
        fullKey(candidateAction), WorkerExitReason::Crashed, 10);
    const auto &restartAction = onlyAction(restart, AppRuntimeActionKind::Launch);
    const auto rollback = h.coordinator->workerExited(
        fullKey(restartAction), WorkerExitReason::Crashed, 11);
    QVERIFY(std::ranges::none_of(rollback.actions, [&](const auto &action) {
        return action.tabId == old.actions.front().tabId;
    }));
}

void AppRuntimeCoordinatorTest::lateCandidateEventsAreIgnored()
{
    CoordinatorHarness h;
    QVERIFY(h.keys.hasValue());
    QVERIFY(h.coordinator->installAndActivate(h.package("one", "1.0.0"), 0)
                .code == AppRuntimeResultCode::Applied);
    const auto launch = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("a")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 1);
    const auto &action = onlyAction(launch, AppRuntimeActionKind::Launch);
    const FullAttemptKey key = fullKey(action);
    QVERIFY(h.coordinator->closeTab(tab(QStringLiteral("a"))).code
            == AppRuntimeResultCode::Applied);
    QCOMPARE(h.coordinator->heartbeat(key, 2).code,
             AppRuntimeResultCode::IgnoredStale);
    QCOMPARE(h.coordinator->workerAdmissionFailed(key, QStringLiteral("late"))
                 .code,
             AppRuntimeResultCode::IgnoredStale);
}

void AppRuntimeCoordinatorTest::rollbackDoesNotAffectAnotherApp()
{
    CoordinatorHarness h;
    QVERIFY(h.keys.hasValue());
    QVERIFY(h.coordinator->installAndActivate(h.package("one", "1.0.0"), 0)
                .code == AppRuntimeResultCode::Applied);
    const auto launch = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("a")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 1);
    const auto &action = onlyAction(launch, AppRuntimeActionKind::Launch);
    QCOMPARE(h.coordinator->workerExited(fullKey(action),
                                         WorkerExitReason::Clean, 2)
                 .code,
             AppRuntimeResultCode::Applied);
}

void AppRuntimeCoordinatorTest::activeNewTabWithInstallPackageStartsZeroWorkers()
{
    CoordinatorHarness h;
    QVERIFY(h.keys.hasValue());
    const auto installed = h.coordinator->installAndActivate(
        h.package("one", "1.0.0"), 0);
    QVERIFY(installed.code == AppRuntimeResultCode::Applied);
    QCOMPARE(installed.actions.size(), 0);
}

void AppRuntimeCoordinatorTest::userReloadUsesCurrentCandidateWhileCrashRestartUsesPinnedLease()
{
    CoordinatorHarness h;
    QVERIFY(h.keys.hasValue());
    QVERIFY(h.coordinator->installAndActivate(h.package("one", "1.0.0"), 0)
                .code == AppRuntimeResultCode::Applied);
    const auto first = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("a")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 1);
    const auto &firstAction = onlyAction(first, AppRuntimeActionKind::Launch);
    QVERIFY(h.coordinator->installAndActivate(h.package("two", "1.1.0"), 2)
                .code == AppRuntimeResultCode::Applied);
    const auto restart = h.coordinator->workerExited(
        fullKey(firstAction), WorkerExitReason::Crashed, 3);
    const auto &restartAction = onlyAction(restart, AppRuntimeActionKind::Launch);
    QCOMPARE(restartAction.launch->lease.version, QStringLiteral("1.0.0"));
    const auto reload = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("a")), QStringLiteral("/"),
        TabLaunchIntent::ReloadCurrent, 4);
    const auto &reloadAction = onlyAction(reload, AppRuntimeActionKind::Launch);
    QCOMPARE(reloadAction.launch->lease.version, QStringLiteral("1.1.0"));
}

void AppRuntimeCoordinatorTest::drainTimeoutIsFailedClosedAndNeverRecovers()
{
    CoordinatorHarness h;
    QVERIFY(h.keys.hasValue());
    QVERIFY(h.coordinator->installAndActivate(h.package("one", "1.0.0"), 0)
                .code == AppRuntimeResultCode::Applied);
    const auto launch = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("a")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 1);
    (void)onlyAction(launch, AppRuntimeActionKind::Launch);
    QVERIFY(h.coordinator->installAndActivate(h.package("two", "1.1.0"), 2)
                .code == AppRuntimeResultCode::Applied);
    const auto candidate = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("a")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 3);
    const auto &candidateAction = onlyAction(candidate,
                                             AppRuntimeActionKind::Launch);
    const auto restart = h.coordinator->workerExited(
        fullKey(candidateAction), WorkerExitReason::Crashed, 10);
    const auto &second = onlyAction(restart, AppRuntimeActionKind::Launch);
    const auto pending = h.coordinator->workerExited(
        fullKey(second), WorkerExitReason::Crashed, 11);
    QVERIFY(!h.drains.isEmpty());
    const auto timedOut = h.coordinator->authorityDrainTimedOut(
        h.drains.back().id, h.drains.back().monotonicDeadlineMs + 1);
    QVERIFY(std::ranges::any_of(timedOut.actions, [](const auto &action) {
        return action.kind == AppRuntimeActionKind::FailedClosed;
    }));
    const auto late = h.coordinator->authorityDrainCompleted(
        h.drains.back().id, h.drains.back().monotonicDeadlineMs + 2);
    QVERIFY(std::ranges::none_of(late.actions, [](const auto &action) {
        return action.kind == AppRuntimeActionKind::Launch
            || action.kind == AppRuntimeActionKind::RecoverFromLkg;
    }));
    Q_UNUSED(pending);
}

void AppRuntimeCoordinatorTest::shutdownRetiresEveryTabAndRejectsNewEvents()
{
    CoordinatorHarness h;
    QVERIFY(h.keys.hasValue());
    QVERIFY(h.coordinator->installAndActivate(h.package("one", "1.0.0"), 0)
                .code == AppRuntimeResultCode::Applied);
    const auto launch = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("a")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 1);
    const auto &action = onlyAction(launch, AppRuntimeActionKind::Launch);
    QVERIFY(h.coordinator->beginShutdown().code == AppRuntimeResultCode::Applied);
    QCOMPARE(h.coordinator->requestTabLaunch(
                  tab(QStringLiteral("b")), QStringLiteral("/"),
                  TabLaunchIntent::ActivateCurrent, 2)
                 .code,
             AppRuntimeResultCode::Rejected);
    QCOMPARE(h.coordinator->heartbeat(fullKey(action), 3).code,
             AppRuntimeResultCode::IgnoredStale);
}

void AppRuntimeCoordinatorTest::staleCandidateEventsCannotAffectNewCandidate()
{
    CoordinatorHarness h;
    QVERIFY(h.keys.hasValue());
    QVERIFY(h.coordinator->installAndActivate(h.package("one", "1.0.0"), 0)
                .code == AppRuntimeResultCode::Applied);
    const auto old = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("old")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 1);
    const auto &oldAction = onlyAction(old, AppRuntimeActionKind::Launch);
    const FullAttemptKey oldKey = fullKey(oldAction);
    QVERIFY(h.coordinator->admitAuthenticatedWorker(
                oldKey, oldAction.launch->lease, 1)
                .code == AppRuntimeResultCode::Applied);
    QVERIFY(h.coordinator->installAndActivate(h.package("two", "1.1.0"), 2)
                .code == AppRuntimeResultCode::Applied);

    QVERIFY(h.coordinator->heartbeat(oldKey, 90).code
            == AppRuntimeResultCode::Applied);
    const AppRuntimeResult late = h.coordinator->heartbeat(oldKey, 101);
    QVERIFY(late.code == AppRuntimeResultCode::Applied
            || late.code == AppRuntimeResultCode::IgnoredStale);
    const ActivationState state = h.store->activationState(
        QStringLiteral("company.pilot")).state;
    QVERIFY2(state.lastKnownGood != state.current,
             "an old candidate tab must not promote the new candidate");
}

void AppRuntimeCoordinatorTest::lateHeartbeatCannotPromoteCandidate()
{
    CoordinatorHarness h;
    QVERIFY(h.keys.hasValue());
    QVERIFY(h.coordinator->installAndActivate(h.package("one", "1.0.0"), 0)
                .code == AppRuntimeResultCode::Applied);
    const auto launch = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("late")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 1);
    const auto &action = onlyAction(launch, AppRuntimeActionKind::Launch);
    const FullAttemptKey key = fullKey(action);
    QVERIFY(h.coordinator->admitAuthenticatedWorker(key,
                                                    action.launch->lease, 1)
                .code == AppRuntimeResultCode::Applied);
    const AppRuntimeResult late = h.coordinator->heartbeat(key, 1'002);
    QVERIFY(std::ranges::any_of(late.actions, [](const auto &value) {
        return value.kind == AppRuntimeActionKind::Launch;
    }));
    const ActivationState state = h.store->activationState(
        QStringLiteral("company.pilot")).state;
    QVERIFY2(state.lastKnownGood != state.current,
             "a heartbeat after the timeout must not promote a candidate");
}

void AppRuntimeCoordinatorTest::promotionIssuesLeasesWithNewGeneration()
{
    CoordinatorHarness h;
    QVERIFY(h.keys.hasValue());
    QVERIFY(h.coordinator->installAndActivate(h.package("one", "1.0.0"), 0)
                .code == AppRuntimeResultCode::Applied);
    QVERIFY(h.coordinator->installAndActivate(h.package("two", "1.1.0"), 1)
                .code == AppRuntimeResultCode::Applied);
    const auto first = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("first")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 2);
    const auto &firstAction = onlyAction(first, AppRuntimeActionKind::Launch);
    const FullAttemptKey key = fullKey(firstAction);
    QVERIFY(h.coordinator->admitAuthenticatedWorker(
                key, firstAction.launch->lease, 2)
                .code == AppRuntimeResultCode::Applied);
    QVERIFY(h.coordinator->heartbeat(key, 90).code
            == AppRuntimeResultCode::Applied);
    QVERIFY(h.coordinator->heartbeat(key, 102).code
            == AppRuntimeResultCode::Applied);
    const ActivationState state = h.store->activationState(
        QStringLiteral("company.pilot")).state;
    const auto second = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("second")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 103);
    const auto &secondAction = onlyAction(second, AppRuntimeActionKind::Launch);
    QCOMPARE(secondAction.launch->lease.activationGenerationAtIssue,
             state.generation);
}

void AppRuntimeCoordinatorTest::drainCompletionAfterDeadlineRetainsIsolationActions()
{
    CoordinatorHarness h;
    QVERIFY(h.keys.hasValue());
    QVERIFY(h.coordinator->installAndActivate(h.package("one", "1.0.0"), 0)
                .code == AppRuntimeResultCode::Applied);
    const auto first = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("a")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 1);
    QVERIFY(h.coordinator->installAndActivate(h.package("two", "1.1.0"), 2)
                .code == AppRuntimeResultCode::Applied);
    const auto candidate = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("a")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 3);
    const auto &candidateAction = onlyAction(candidate,
                                             AppRuntimeActionKind::Launch);
    const auto restart = h.coordinator->workerExited(
        fullKey(candidateAction), WorkerExitReason::Crashed, 10);
    const auto &restartAction = onlyAction(restart, AppRuntimeActionKind::Launch);
    const auto rollback = h.coordinator->workerExited(
        fullKey(restartAction), WorkerExitReason::Crashed, 11);
    QVERIFY(!rollback.actions.isEmpty());
    QVERIFY(!h.drains.isEmpty());
    const AppRuntimeResult completed = h.coordinator->authorityDrainCompleted(
        h.drains.back().id, h.drains.back().monotonicDeadlineMs + 1);
    QCOMPARE(completed.code, AppRuntimeResultCode::FailedClosed);
    QVERIFY(std::ranges::any_of(completed.actions, [](const auto &value) {
        return value.kind == AppRuntimeActionKind::IsolateSession;
    }));
    Q_UNUSED(first);
}

void AppRuntimeCoordinatorTest::rollbackFailureStopsAndPermanentlyIsolates()
{
    CoordinatorHarness h;
    QVERIFY(h.keys.hasValue());
    QVERIFY(h.coordinator->installAndActivate(h.package("one", "1.0.0"), 0)
                .code == AppRuntimeResultCode::Applied);
    const auto old = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("a")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 1);
    QVERIFY(!old.actions.isEmpty());
    QVERIFY(h.coordinator->installAndActivate(h.package("two", "1.1.0"), 2)
                .code == AppRuntimeResultCode::Applied);
    qbrowser_package_store_testing::PackageStoreTestHooks hooks;
    hooks.afterActivationLockAcquired =
        [&h](const QString &appId, const QString &operation) {
            if (appId == QStringLiteral("company.pilot")
                && operation == QStringLiteral("rollback")) {
                QVERIFY(QFile::remove(h.store->appRoot(appId)
                                      + QStringLiteral("/activation.json")));
            }
        };
    qbrowser_package_store_testing::setPackageStoreTestHooks(std::move(hooks));
    const auto candidate = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("a")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 3);
    const auto &candidateAction = onlyAction(candidate,
                                             AppRuntimeActionKind::Launch);
    const auto restart = h.coordinator->workerExited(
        fullKey(candidateAction), WorkerExitReason::Crashed, 10);
    const auto &restartAction = onlyAction(restart, AppRuntimeActionKind::Launch);
    const AppRuntimeResult failed = h.coordinator->workerExited(
        fullKey(restartAction), WorkerExitReason::Crashed, 11);
    qbrowser_package_store_testing::resetPackageStoreTestHooks();
    QCOMPARE(failed.code, AppRuntimeResultCode::FailedClosed);
    QVERIFY(std::ranges::any_of(failed.actions, [](const auto &value) {
        return value.kind == AppRuntimeActionKind::Stop;
    }));
    QVERIFY(std::ranges::any_of(failed.actions, [](const auto &value) {
        return value.kind == AppRuntimeActionKind::IsolateSession;
    }));
    QCOMPARE(h.coordinator->requestTabLaunch(
                  tab(QStringLiteral("new")), QStringLiteral("/"),
                  TabLaunchIntent::ActivateCurrent, 12)
                 .code,
             AppRuntimeResultCode::FailedClosed);
}

void AppRuntimeCoordinatorTest::pendingDrainAndShutdownNeverRelauch()
{
    CoordinatorHarness h;
    QVERIFY(h.keys.hasValue());
    QVERIFY(h.coordinator->installAndActivate(h.package("one", "1.0.0"), 0)
                .code == AppRuntimeResultCode::Applied);
    QVERIFY(h.coordinator->installAndActivate(h.package("two", "1.1.0"), 1)
                .code == AppRuntimeResultCode::Applied);
    const auto launch = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("a")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 2);
    const auto &action = onlyAction(launch, AppRuntimeActionKind::Launch);
    const auto restart = h.coordinator->workerExited(
        fullKey(action), WorkerExitReason::Crashed, 10);
    const auto &restartAction = onlyAction(restart, AppRuntimeActionKind::Launch);
    const auto rollback = h.coordinator->workerExited(
        fullKey(restartAction), WorkerExitReason::Crashed, 11);
    QVERIFY(!h.drains.isEmpty());
    QCOMPARE(h.coordinator->requestTabLaunch(
                  tab(QStringLiteral("b")), QStringLiteral("/"),
                  TabLaunchIntent::ActivateCurrent, 12)
                 .code,
             AppRuntimeResultCode::Rejected);
    QVERIFY(h.coordinator->beginShutdown(13).code
            == AppRuntimeResultCode::Applied);
    const auto done = h.coordinator->authorityDrainCompleted(
        h.drains.back().id, 14);
    QVERIFY(std::ranges::none_of(done.actions, [](const auto &value) {
        return value.kind == AppRuntimeActionKind::Launch
            || value.kind == AppRuntimeActionKind::RecoverFromLkg;
    }));
    Q_UNUSED(rollback);
}

void AppRuntimeCoordinatorTest::revokedTokenCannotPassRepeatedAdmission()
{
    CoordinatorHarness h;
    QVERIFY(h.keys.hasValue());
    QVERIFY(h.coordinator->installAndActivate(h.package("one", "1.0.0"), 0)
                .code == AppRuntimeResultCode::Applied);
    const auto launch = h.coordinator->requestTabLaunch(
        tab(QStringLiteral("a")), QStringLiteral("/"),
        TabLaunchIntent::ActivateCurrent, 1);
    const auto &action = onlyAction(launch, AppRuntimeActionKind::Launch);
    const FullAttemptKey key = fullKey(action);
    QVERIFY(h.coordinator->admitAuthenticatedWorker(
                key, action.launch->lease, 1)
                .code == AppRuntimeResultCode::Applied);
    const auto ticket = action.launch->admission->beginRevoke();
    QVERIFY(!ticket.isDrained() || ticket.isDrained());
    QCOMPARE(h.coordinator->admitAuthenticatedWorker(
                  key, action.launch->lease, 2)
                 .code,
             AppRuntimeResultCode::Rejected);
}

QTEST_MAIN(AppRuntimeCoordinatorTest)
#include "tst_app_runtime_coordinator.moc"
