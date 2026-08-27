#include "UpdateLifecycleCoordinator.h"

#include "EventRecorder.h"
#include "PackageStore.h"

#include <QFileInfo>
#include <QDateTime>

#include <chrono>
#include <limits>
#include <utility>

namespace
{
UpdateLifecycleResult lifecycleFailure(const UpdateLifecycleError error,
                                       const QString &stableError)
{
    return {error, UpdateLifecycleAction::FailedClosed, stableError, {}, {}, {}};
}
}

LifecycleClock LifecycleClock::system()
{
    return {
        [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        },
        [] { return QDateTime::currentMSecsSinceEpoch(); },
    };
}

bool LifecycleClock::isValid() const noexcept
{
    return static_cast<bool>(steadyNowMilliseconds)
        && static_cast<bool>(utcNowMilliseconds);
}

UpdateLifecycleCoordinator::UpdateLifecycleCoordinator(
    QString appId,
    PackageStore &store,
    PackageInstaller &installer,
    const WorkerSupervisionPolicy supervisionPolicy,
    LaunchCallback launch,
    LifecycleClock clock,
    EventRecorder *recorder,
    QString tabId,
    const quint64 runtimeIncarnation)
    : appId_(std::move(appId))
    , store_(store)
    , installer_(installer)
    , launch_(std::move(launch))
    , recorder_(recorder)
    , clock_(std::move(clock))
    , supervisor_(supervisionPolicy,
                  [this](WorkerActivationId) { restartRequested_ = true; },
                  [this](WorkerActivationId) { rollbackRequested_ = true; })
    , tabId_(std::move(tabId))
    , runtimeIncarnation_(runtimeIncarnation)
{
    if (appId_.isEmpty() || !launch_ || !clock_.isValid()
        || runtimeIncarnation_ == 0) {
        failedClosed_ = true;
    }
}

void UpdateLifecycleCoordinator::setBeforeRelaunchCallback(
    BeforeRelaunchCallback callback)
{
    if (!currentKey_.has_value()) beforeRelaunch_ = std::move(callback);
}

UpdateLifecycleResult UpdateLifecycleCoordinator::installAndLaunch(
    const QString &packagePath)
{
    const qint64 nowMs = clock_.steadyNowMilliseconds();
    if (failedClosed_ || nowMs < 0) {
        return lifecycleFailure(UpdateLifecycleError::InvalidConfiguration,
                                QStringLiteral("update.invalid_configuration"));
    }
    const InstallResult installed = installer_.install(packagePath);
    if (!installed.succeeded() || installed.appId != appId_) {
        record(SafeEventPhase::Install, SafeEventCode::Rejected, 0);
        return lifecycleFailure(UpdateLifecycleError::InstallRejected,
                                installed.stableError.isEmpty()
                                    ? QStringLiteral("update.install_rejected")
                                    : installed.stableError);
    }
    if (!installed.activationBinding.has_value()) {
        (void)enterFailedClosed();
        return lifecycleFailure(UpdateLifecycleError::StateUnavailable,
                                QStringLiteral("update.activation_binding_missing"));
    }
    UpdateLifecycleResult result = beginLaunch(
        installed.version, installed.path, installed.entryPoint,
        installed.permissions,
        *installed.activationBinding, nowMs, false,
        UpdateLifecycleAction::LaunchRequested);
    result.appId = installed.appId;
    result.version = installed.version;
    result.path = installed.path;
    return result;
}

UpdateLifecycleResult UpdateLifecycleCoordinator::startOffline()
{
    const qint64 nowMs = clock_.steadyNowMilliseconds();
    if (failedClosed_ || nowMs < 0) {
        return lifecycleFailure(UpdateLifecycleError::InvalidConfiguration,
                                QStringLiteral("update.invalid_configuration"));
    }
    const ActivationStateResult recordedState = store_.recordedActivationState(appId_);
    if (!recordedState.hasValue()) {
        (void)enterFailedClosed();
        return lifecycleFailure(UpdateLifecycleError::StateUnavailable,
                                QStringLiteral("update.state_unavailable"));
    }
    const auto recordedBinding = store_.bindingForState(recordedState.state);
    if (!recordedBinding.has_value()) {
        (void)enterFailedClosed();
        return lifecycleFailure(UpdateLifecycleError::StateUnavailable,
                                QStringLiteral("update.activation_binding_invalid"));
    }
    if (!recordedState.state.current.isEmpty()) {
        const InstallResult current = installer_.verifyInstalled(
            appId_, recordedState.state.current);
        if (current.succeeded()) {
            const PackageStoreResult confirmed = store_.confirmCurrent(
                appId_, *recordedBinding);
            if (!confirmed.succeeded() || !confirmed.activationBinding.has_value()) {
                (void)enterFailedClosed();
                return lifecycleFailure(UpdateLifecycleError::StateCommitFailed,
                                        QStringLiteral("update.current_changed"));
            }
            const InstallResult rebound = installer_.reverifyInstalledVersion(
                appId_, *confirmed.activationBinding);
            if (!rebound.succeeded()) {
                (void)enterFailedClosed();
                return lifecycleFailure(
                    UpdateLifecycleError::PackageVerificationFailed,
                    QStringLiteral("update.current_verification_failed"));
            }
            return beginLaunch(rebound.version, rebound.path,
                               rebound.entryPoint, rebound.permissions,
                               *confirmed.activationBinding,
                               nowMs, false,
                               UpdateLifecycleAction::LaunchRequested);
        }
    }
    if (recordedState.state.lastKnownGood.isEmpty()) {
        (void)enterFailedClosed();
        return lifecycleFailure(UpdateLifecycleError::LkgUnavailable,
                                QStringLiteral("update.lkg_unavailable"));
    }
    const InstallResult lkg = installer_.verifyInstalled(
        appId_, recordedState.state.lastKnownGood);
    if (!lkg.succeeded()) {
        (void)enterFailedClosed();
        return lifecycleFailure(UpdateLifecycleError::PackageVerificationFailed,
                                QStringLiteral("update.lkg_verification_failed"));
    }
    const PackageStoreResult recovered = store_.recoverLastKnownGood(
        appId_, *recordedBinding);
    if (!recovered.succeeded() || !recovered.activationBinding.has_value()) {
        (void)enterFailedClosed();
        return lifecycleFailure(UpdateLifecycleError::StateCommitFailed,
                                QStringLiteral("update.recovery_commit_failed"));
    }
    const InstallResult rebound = installer_.reverifyInstalledVersion(
        appId_, *recovered.activationBinding);
    if (!rebound.succeeded()) {
        (void)enterFailedClosed();
        return lifecycleFailure(UpdateLifecycleError::PackageVerificationFailed,
                                QStringLiteral("update.lkg_verification_failed"));
    }
    return beginLaunch(rebound.version, rebound.path, rebound.entryPoint,
                       rebound.permissions,
                       *recovered.activationBinding, nowMs, true,
                       UpdateLifecycleAction::RecoveredAndLaunched);
}

UpdateLifecycleResult UpdateLifecycleCoordinator::beginLaunch(
    QString version,
    QString path,
    QString entryPoint,
    ManifestPermissions permissions,
    ActivationBinding binding,
    const qint64 nowMs,
    const bool recovery,
    const UpdateLifecycleAction successAction)
{
    stopCurrentAttempt();
    currentVersion_ = std::move(version);
    currentPath_ = std::move(path);
    currentEntryPoint_ = std::move(entryPoint);
    currentPermissions_ = std::move(permissions);
    currentBinding_ = std::move(binding);
    currentHealthy_ = false;
    handshakeAccepted_ = false;
    recoveryLaunch_ = recovery;
    restartRequested_ = false;
    rollbackRequested_ = false;
    hostShuttingDown_ = false;
    const WorkerActivationId activation = supervisor_.beginActivation(nowMs);
    const std::optional<WorkerAttemptId> attempt = supervisor_.beginAttempt(
        activation, nowMs);
    if (!attempt.has_value()) {
        (void)enterFailedClosed();
        return lifecycleFailure(UpdateLifecycleError::LaunchFailed,
                                QStringLiteral("update.attempt_unavailable"));
    }
    currentKey_ = WorkerAttemptKey{activation, *attempt};
    currentAttemptStopped_ = true;
    const auto request = issueLaunchRequest(recovery);
    if (!request.has_value() || !launch_(*request)) {
        const WorkerSupervisionAction action = supervisor_.workerExited(
            *currentKey_, WorkerExitReason::StartupFailure, nowMs);
        const UpdateLifecycleAction recoveryAction = applySupervisionAction(
            action, nowMs);
        if (recoveryAction == UpdateLifecycleAction::RolledBackAndLaunched) {
            return {UpdateLifecycleError::LaunchFailed, recoveryAction,
                    QStringLiteral("update.launch_failed"), appId_,
                    currentVersion_, currentPath_};
        }
        (void)enterFailedClosed();
        return lifecycleFailure(UpdateLifecycleError::LaunchFailed,
                                QStringLiteral("update.launch_failed"));
    }
    currentAttemptStopped_ = false;
    record(recovery ? SafeEventPhase::Rollback : SafeEventPhase::Activate,
           recovery ? SafeEventCode::Recovered : SafeEventCode::Started,
           0, {{SafeMetric::AttemptNumber,
                       static_cast<double>(attempt->value)}});
    return {UpdateLifecycleError::None, successAction, {}, appId_,
            currentVersion_, currentPath_};
}

UpdateLifecycleAction UpdateLifecycleCoordinator::authenticatedHandshake(
    const WorkerAttemptKey key)
{
    const qint64 nowMs = clock_.steadyNowMilliseconds();
    if (!currentKey_.has_value() || key != *currentKey_) {
        return UpdateLifecycleAction::IgnoredStaleAttempt;
    }
    if (handshakeAccepted_) {
        const WorkerSupervisionAction timeout = supervisor_.checkHealth(key, nowMs);
        return timeout == WorkerSupervisionAction::None
            ? UpdateLifecycleAction::None
            : applySupervisionAction(timeout, nowMs);
    }
    if (!currentBinding_.has_value()
        || !installer_.reverifyInstalledVersion(appId_, *currentBinding_)
                .succeeded()) {
        return enterFailedClosed();
    }
    if (!supervisor_.authenticatedHandshake(key, nowMs)) {
        return UpdateLifecycleAction::FailedClosed;
    }
    handshakeAccepted_ = true;
    return UpdateLifecycleAction::None;
}

UpdateLifecycleAction UpdateLifecycleCoordinator::admitAuthenticatedWorker(
    const WorkerAttemptKey key,
    const ActivationBinding &expectedBinding)
{
    if (!currentKey_.has_value() || key != *currentKey_) {
        return UpdateLifecycleAction::IgnoredStaleAttempt;
    }
    if (!currentBinding_.has_value()
        || *currentBinding_ != expectedBinding) {
        return enterFailedClosed();
    }
    return authenticatedHandshake(key);
}

UpdateLifecycleAction UpdateLifecycleCoordinator::admitAuthenticatedWorker(
    const WorkerLaunchRequest &request)
{
    if (!currentKey_.has_value() || request.attempt != *currentKey_) {
        return UpdateLifecycleAction::IgnoredStaleAttempt;
    }
    if (!currentWorkerLaunch_.has_value() || request != *currentWorkerLaunch_
        || request.tabId != tabId_
        || request.runtimeIncarnation != runtimeIncarnation_
        || request.lease.leaseAuthorityEpoch == 0
        || request.lease.leaseAuthorityEpoch
               != currentWorkerLaunch_->lease.leaseAuthorityEpoch) {
        return enterFailedClosed();
    }
    if (request.revalidationMode == PackageRevalidationMode::CurrentActivation) {
        const ActivationBinding expected{
            request.lease.versionDirectory,
            request.lease.digestHex,
            request.lease.activationGenerationAtIssue};
        if (!currentBinding_.has_value() || *currentBinding_ != expected
            || !store_.compareCurrent(appId_, expected).succeeded()) {
            return enterFailedClosed();
        }
    }
    const qint64 nowMs = clock_.steadyNowMilliseconds();
    if (nowMs < 0) return enterFailedClosed();
    if (handshakeAccepted_) {
        const WorkerSupervisionAction timeout =
            supervisor_.checkHealth(request.attempt, nowMs);
        return timeout == WorkerSupervisionAction::None
            ? UpdateLifecycleAction::None
            : applySupervisionAction(timeout, nowMs);
    }
    // The launcher has already performed both exact package validations with
    // one retained immutable guard. Admission independently linearizes only
    // the tab/runtime/epoch authority above; reacquiring the package through
    // a new path-based guard would conflict with the active membership seal.
    if (!supervisor_.authenticatedHandshake(request.attempt, nowMs)) {
        return enterFailedClosed();
    }
    handshakeAccepted_ = true;
    return UpdateLifecycleAction::None;
}

UpdateLifecycleAction UpdateLifecycleCoordinator::heartbeat(
    const WorkerAttemptKey key)
{
    const qint64 nowMs = clock_.steadyNowMilliseconds();
    if (!currentKey_.has_value() || key != *currentKey_) {
        return UpdateLifecycleAction::IgnoredStaleAttempt;
    }
    const WorkerSupervisionAction timeout = supervisor_.checkHealth(key, nowMs);
    if (timeout != WorkerSupervisionAction::None) {
        return applySupervisionAction(timeout, nowMs);
    }
    if (!handshakeAccepted_) return UpdateLifecycleAction::IgnoredUntilHandshake;
    supervisor_.heartbeat(key, nowMs);
    return transitionToHealthy(key, nowMs);
}

UpdateLifecycleAction UpdateLifecycleCoordinator::checkHealth(
    const WorkerAttemptKey key)
{
    const qint64 nowMs = clock_.steadyNowMilliseconds();
    if (!currentKey_.has_value() || key != *currentKey_) {
        return UpdateLifecycleAction::IgnoredStaleAttempt;
    }
    const WorkerSupervisionAction action = supervisor_.checkHealth(key, nowMs);
    if (action != WorkerSupervisionAction::None) {
        return applySupervisionAction(action, nowMs);
    }
    if (!handshakeAccepted_) return UpdateLifecycleAction::None;
    return transitionToHealthy(key, nowMs);
}

UpdateLifecycleAction UpdateLifecycleCoordinator::workerExited(
    const WorkerAttemptKey key,
    WorkerExitReason reason)
{
    const qint64 nowMs = clock_.steadyNowMilliseconds();
    if (hostShuttingDown_) reason = WorkerExitReason::Clean;
    return applySupervisionAction(supervisor_.workerExited(key, reason, nowMs),
                                  nowMs);
}

UpdateLifecycleAction UpdateLifecycleCoordinator::workerCleanupFailed(
    const WorkerAttemptKey key)
{
    Q_UNUSED(key);
    return enterFailedClosed();
}

UpdateLifecycleAction UpdateLifecycleCoordinator::workerAdmissionFailed(
    const WorkerAttemptKey key)
{
    if (!currentKey_.has_value() || key != *currentKey_) {
        return UpdateLifecycleAction::IgnoredStaleAttempt;
    }
    return enterFailedClosed();
}

UpdateLifecycleAction UpdateLifecycleCoordinator::transitionToHealthy(
    const WorkerAttemptKey key,
    const qint64 steadyNowMs)
{
    if (currentHealthy_ || !supervisor_.isHealthy(key, steadyNowMs)) {
        return UpdateLifecycleAction::None;
    }
    if (!currentBinding_.has_value()) return enterFailedClosed();
    const PackageStoreResult marked = store_.markCurrentLastKnownGood(
        appId_, *currentBinding_);
    if (!marked.succeeded() || !marked.activationBinding.has_value()) {
        return enterFailedClosed();
    }
    currentBinding_ = marked.activationBinding;
    currentHealthy_ = true;
    record(SafeEventPhase::Health, SafeEventCode::Healthy, 0);
    return UpdateLifecycleAction::MarkedHealthy;
}

UpdateLifecycleAction UpdateLifecycleCoordinator::applySupervisionAction(
    const WorkerSupervisionAction action,
    const qint64 nowMs)
{
    switch (action) {
    case WorkerSupervisionAction::None:
        return UpdateLifecycleAction::None;
    case WorkerSupervisionAction::IgnoredStaleAttempt:
        return UpdateLifecycleAction::IgnoredStaleAttempt;
    case WorkerSupervisionAction::IgnoredDuplicateFailure:
        return UpdateLifecycleAction::IgnoredDuplicateFailure;
    case WorkerSupervisionAction::Restart:
        return restartRequested_ ? restart(nowMs)
                                 : enterFailedClosed();
    case WorkerSupervisionAction::StartupRollback:
    case WorkerSupervisionAction::CrashLoopRollback:
        return rollbackRequested_ ? rollbackAndRecover(nowMs)
                                  : enterFailedClosed();
    }
    return enterFailedClosed();
}

UpdateLifecycleAction UpdateLifecycleCoordinator::restart(const qint64 nowMs)
{
    restartRequested_ = false;
    handshakeAccepted_ = false;
    stopCurrentAttempt();
    if (!currentBinding_.has_value()) return enterFailedClosed();
    const InstallResult rebound = installer_.reverifyInstalledVersion(
        appId_, *currentBinding_);
    if (!rebound.succeeded()) return enterFailedClosed();
    currentVersion_ = rebound.version;
    currentPath_ = rebound.path;
    currentEntryPoint_ = rebound.entryPoint;
    currentPermissions_ = rebound.permissions;
    const WorkerActivationId activation = supervisor_.activeActivation();
    const std::optional<WorkerAttemptId> attempt = supervisor_.beginAttempt(
        activation, nowMs);
    if (!attempt.has_value()) {
        return enterFailedClosed();
    }
    currentKey_ = WorkerAttemptKey{activation, *attempt};
    currentAttemptStopped_ = true;
    const auto request = issueLaunchRequest(recoveryLaunch_);
    if (!request.has_value() || !launch_(*request)) {
        const WorkerSupervisionAction failed = supervisor_.workerExited(
            *currentKey_, WorkerExitReason::StartupFailure, nowMs);
        return applySupervisionAction(failed, nowMs);
    }
    currentAttemptStopped_ = false;
    record(SafeEventPhase::Worker, SafeEventCode::Restarted, 0,
           {{SafeMetric::RestartCount, 1.0},
            {SafeMetric::AttemptNumber, static_cast<double>(attempt->value)}});
    return UpdateLifecycleAction::Restarted;
}

UpdateLifecycleAction UpdateLifecycleCoordinator::rollbackAndRecover(
    const qint64 nowMs)
{
    rollbackRequested_ = false;
    stopCurrentAttempt();
    if (!currentBinding_.has_value()) return enterFailedClosed();
    const PackageStoreResult rolledBack = store_.rollback(appId_, *currentBinding_);
    if (!rolledBack.succeeded() || !rolledBack.activationBinding.has_value()) {
        return enterFailedClosed();
    }
    const ActivationStateResult state = store_.activationState(appId_);
    if (!state.hasValue()) {
        return enterFailedClosed();
    }
    const InstallResult verified = installer_.reverifyInstalledVersion(
        appId_, *rolledBack.activationBinding);
    if (!verified.succeeded()) {
        return enterFailedClosed();
    }
    const UpdateLifecycleResult launched = beginLaunch(
        verified.version, verified.path, verified.entryPoint,
        verified.permissions,
        *rolledBack.activationBinding, nowMs, true,
        UpdateLifecycleAction::RolledBackAndLaunched);
    if (!launched.succeeded()) {
        return enterFailedClosed();
    }
    return UpdateLifecycleAction::RolledBackAndLaunched;
}

std::optional<UpdateLaunchRequest>
UpdateLifecycleCoordinator::issueLaunchRequest(const bool recovery)
{
    if (!currentBinding_.has_value() || !currentKey_.has_value()
        || nextLeaseAuthorityEpoch_ == 0) {
        return std::nullopt;
    }
    revokeCurrentLaunchAuthority();
    const quint64 authorityEpoch = nextLeaseAuthorityEpoch_;
    nextLeaseAuthorityEpoch_ = authorityEpoch
            == std::numeric_limits<quint64>::max()
        ? 0
        : authorityEpoch + 1;
    VerifiedPackageLease lease{
        appId_,
        currentVersion_,
        currentBinding_->currentDirectory,
        currentPath_,
        currentEntryPoint_,
        currentPermissions_,
        currentBinding_->versionDigestHex,
        currentBinding_->generation,
        authorityEpoch};
    WorkerLaunchRequest worker{
        tabId_,
        runtimeIncarnation_,
        QStringLiteral("/"),
        std::move(lease),
        std::make_shared<AuthorityAdmissionToken>(),
        *currentKey_,
        recovery ? PackageRevalidationMode::PinnedLease
                 : PackageRevalidationMode::CurrentActivation,
        recovery};
    currentWorkerLaunch_ = worker;
    return UpdateLaunchRequest{
        appId_, currentVersion_, currentPath_, currentEntryPoint_,
        *currentBinding_, *currentKey_, recovery, std::move(worker)};
}

void UpdateLifecycleCoordinator::revokeCurrentLaunchAuthority() noexcept
{
    if (currentWorkerLaunch_.has_value()
        && currentWorkerLaunch_->admission != nullptr) {
        (void)currentWorkerLaunch_->admission->beginRevoke();
    }
    currentWorkerLaunch_.reset();
}

void UpdateLifecycleCoordinator::stopCurrentAttempt()
{
    revokeCurrentLaunchAuthority();
    if (!currentKey_.has_value() || currentAttemptStopped_) return;
    currentAttemptStopped_ = true;
    if (beforeRelaunch_) beforeRelaunch_();
}

UpdateLifecycleAction UpdateLifecycleCoordinator::enterFailedClosed()
{
    failedClosed_ = true;
    stopCurrentAttempt();
    return UpdateLifecycleAction::FailedClosed;
}

void UpdateLifecycleCoordinator::beginHostShutdown() noexcept
{
    hostShuttingDown_ = true;
    revokeCurrentLaunchAuthority();
}

std::optional<WorkerAttemptKey>
UpdateLifecycleCoordinator::currentAttemptKey() const noexcept
{
    return currentKey_;
}

bool UpdateLifecycleCoordinator::failedClosed() const noexcept
{
    return failedClosed_;
}

void UpdateLifecycleCoordinator::recordRouteLoadAcknowledged(
    const QString &routeTemplate, const qsizetype pendingRouteLoads) const
{
    static const QStringList allowedTemplates{
        QStringLiteral("/login"), QStringLiteral("/dashboard"),
        QStringLiteral("/orders"), QStringLiteral("/orders/:id"),
        QStringLiteral("/orders/:id/edit"), QStringLiteral("/customers"),
        QStringLiteral("/customers/:id"), QStringLiteral("/files"),
        QStringLiteral("/settings")};
    if (pendingRouteLoads != 0 || !allowedTemplates.contains(routeTemplate)) return;
    record(SafeEventPhase::Worker, SafeEventCode::Completed, 0,
           {{SafeMetric::QueueDepth, static_cast<double>(pendingRouteLoads)}},
           routeTemplate);
}

void UpdateLifecycleCoordinator::record(
    const SafeEventPhase phase,
    const SafeEventCode code,
    const qint64 durationMs,
    const SafeMetrics &metrics,
    const QString &routeTemplate) const
{
    if (recorder_ == nullptr || currentVersion_.isEmpty()) return;
    const SafeEventResult event = SafeEvent::create(
        clock_.utcNowMilliseconds(), appId_, currentVersion_, phase, code, durationMs,
        routeTemplate, metrics);
    if (event.hasValue()) (void)recorder_->record(event.value());
}
