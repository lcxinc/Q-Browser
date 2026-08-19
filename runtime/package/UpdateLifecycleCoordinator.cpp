#include "UpdateLifecycleCoordinator.h"

#include "EventRecorder.h"
#include "PackageStore.h"

#include <QFileInfo>

#include <utility>

namespace
{
UpdateLifecycleResult lifecycleFailure(const UpdateLifecycleError error,
                                       const QString &stableError)
{
    return {error, UpdateLifecycleAction::FailedClosed, stableError, {}, {}, {}};
}
}

UpdateLifecycleCoordinator::UpdateLifecycleCoordinator(
    QString appId,
    PackageStore &store,
    PackageInstaller &installer,
    const WorkerSupervisionPolicy supervisionPolicy,
    LaunchCallback launch,
    EventRecorder *recorder)
    : appId_(std::move(appId))
    , store_(store)
    , installer_(installer)
    , launch_(std::move(launch))
    , recorder_(recorder)
    , supervisor_(supervisionPolicy,
                  [this](WorkerActivationId) { restartRequested_ = true; },
                  [this](WorkerActivationId) { rollbackRequested_ = true; })
{
    if (appId_.isEmpty() || !launch_) failedClosed_ = true;
}

void UpdateLifecycleCoordinator::setBeforeRelaunchCallback(
    BeforeRelaunchCallback callback)
{
    if (!currentKey_.has_value()) beforeRelaunch_ = std::move(callback);
}

UpdateLifecycleResult UpdateLifecycleCoordinator::installAndLaunch(
    const QString &packagePath,
    const qint64 nowMs)
{
    if (failedClosed_ || nowMs < 0) {
        return lifecycleFailure(UpdateLifecycleError::InvalidConfiguration,
                                QStringLiteral("update.invalid_configuration"));
    }
    const InstallResult installed = installer_.install(packagePath);
    if (!installed.succeeded() || installed.appId != appId_) {
        record(SafeEventPhase::Install, SafeEventCode::Rejected, nowMs, 0);
        return lifecycleFailure(UpdateLifecycleError::InstallRejected,
                                QStringLiteral("update.install_rejected"));
    }
    UpdateLifecycleResult result = beginLaunch(
        installed.version, installed.path, nowMs, false,
        UpdateLifecycleAction::LaunchRequested);
    result.appId = installed.appId;
    result.version = installed.version;
    result.path = installed.path;
    return result;
}

UpdateLifecycleResult UpdateLifecycleCoordinator::startOffline(const qint64 nowMs)
{
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
    if (!recordedState.state.current.isEmpty()) {
        const InstallResult current = installer_.verifyInstalled(
            appId_, recordedState.state.current);
        if (current.succeeded()) {
            return beginLaunch(current.version, current.path, nowMs, false,
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
    const PackageStoreResult recovered = store_.recoverLastKnownGood(appId_);
    if (!recovered.succeeded()) {
        (void)enterFailedClosed();
        return lifecycleFailure(UpdateLifecycleError::StateCommitFailed,
                                QStringLiteral("update.recovery_commit_failed"));
    }
    return beginLaunch(lkg.version, lkg.path, nowMs, true,
                       UpdateLifecycleAction::RecoveredAndLaunched);
}

UpdateLifecycleResult UpdateLifecycleCoordinator::beginLaunch(
    QString version,
    QString path,
    const qint64 nowMs,
    const bool recovery,
    const UpdateLifecycleAction successAction)
{
    stopCurrentAttempt();
    currentVersion_ = std::move(version);
    currentPath_ = std::move(path);
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
    const UpdateLaunchRequest request{appId_, currentVersion_, currentPath_,
                                      *currentKey_, recovery};
    if (!launch_(request)) {
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
           nowMs, 0, {{SafeMetric::AttemptNumber,
                       static_cast<double>(attempt->value)}});
    return {UpdateLifecycleError::None, successAction, {}, appId_,
            currentVersion_, currentPath_};
}

UpdateLifecycleAction UpdateLifecycleCoordinator::authenticatedHandshake(
    const WorkerAttemptKey key,
    const qint64 nowMs)
{
    if (!currentKey_.has_value() || key != *currentKey_) {
        return UpdateLifecycleAction::IgnoredStaleAttempt;
    }
    const WorkerSupervisionAction timeout = supervisor_.checkHealth(key, nowMs);
    if (timeout != WorkerSupervisionAction::None) {
        return applySupervisionAction(timeout, nowMs);
    }
    if (handshakeAccepted_) return UpdateLifecycleAction::None;
    if (!supervisor_.authenticatedHandshake(key, nowMs)) {
        return UpdateLifecycleAction::FailedClosed;
    }
    handshakeAccepted_ = true;
    return UpdateLifecycleAction::None;
}

UpdateLifecycleAction UpdateLifecycleCoordinator::heartbeat(
    const WorkerAttemptKey key,
    const qint64 nowMs)
{
    if (!currentKey_.has_value() || key != *currentKey_) {
        return UpdateLifecycleAction::IgnoredStaleAttempt;
    }
    const WorkerSupervisionAction timeout = supervisor_.checkHealth(key, nowMs);
    if (timeout != WorkerSupervisionAction::None) {
        return applySupervisionAction(timeout, nowMs);
    }
    if (!handshakeAccepted_) return UpdateLifecycleAction::IgnoredUntilHandshake;
    supervisor_.heartbeat(key, nowMs);
    if (!currentHealthy_ && supervisor_.isHealthy(key, nowMs)) {
        const PackageStoreResult marked = store_.markCurrentLastKnownGood(appId_);
        if (!marked.succeeded()) {
            return enterFailedClosed();
        }
        currentHealthy_ = true;
        record(SafeEventPhase::Health, SafeEventCode::Healthy, nowMs, 0);
        return UpdateLifecycleAction::MarkedHealthy;
    }
    return UpdateLifecycleAction::None;
}

UpdateLifecycleAction UpdateLifecycleCoordinator::checkHealth(
    const WorkerAttemptKey key,
    const qint64 nowMs)
{
    if (!currentKey_.has_value() || key != *currentKey_) {
        return UpdateLifecycleAction::IgnoredStaleAttempt;
    }
    const WorkerSupervisionAction action = supervisor_.checkHealth(key, nowMs);
    if (action != WorkerSupervisionAction::None) {
        return applySupervisionAction(action, nowMs);
    }
    if (handshakeAccepted_ && !currentHealthy_
        && supervisor_.isHealthy(key, nowMs)) {
        const PackageStoreResult marked = store_.markCurrentLastKnownGood(appId_);
        if (!marked.succeeded()) {
            return enterFailedClosed();
        }
        currentHealthy_ = true;
        return UpdateLifecycleAction::MarkedHealthy;
    }
    return UpdateLifecycleAction::None;
}

UpdateLifecycleAction UpdateLifecycleCoordinator::workerExited(
    const WorkerAttemptKey key,
    WorkerExitReason reason,
    const qint64 nowMs)
{
    if (hostShuttingDown_) reason = WorkerExitReason::Clean;
    return applySupervisionAction(supervisor_.workerExited(key, reason, nowMs),
                                  nowMs);
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
    const WorkerActivationId activation = supervisor_.activeActivation();
    const std::optional<WorkerAttemptId> attempt = supervisor_.beginAttempt(
        activation, nowMs);
    if (!attempt.has_value()) {
        return enterFailedClosed();
    }
    currentKey_ = WorkerAttemptKey{activation, *attempt};
    currentAttemptStopped_ = true;
    if (!launch_({appId_, currentVersion_, currentPath_, *currentKey_,
                  recoveryLaunch_})) {
        const WorkerSupervisionAction failed = supervisor_.workerExited(
            *currentKey_, WorkerExitReason::StartupFailure, nowMs);
        return applySupervisionAction(failed, nowMs);
    }
    currentAttemptStopped_ = false;
    record(SafeEventPhase::Worker, SafeEventCode::Restarted, nowMs, 0,
           {{SafeMetric::RestartCount, 1.0},
            {SafeMetric::AttemptNumber, static_cast<double>(attempt->value)}});
    return UpdateLifecycleAction::Restarted;
}

UpdateLifecycleAction UpdateLifecycleCoordinator::rollbackAndRecover(
    const qint64 nowMs)
{
    rollbackRequested_ = false;
    stopCurrentAttempt();
    const PackageStoreResult rolledBack = store_.rollback(appId_);
    if (!rolledBack.succeeded()) {
        return enterFailedClosed();
    }
    const ActivationStateResult state = store_.activationState(appId_);
    if (!state.hasValue()) {
        return enterFailedClosed();
    }
    const InstallResult verified = installer_.verifyInstalled(appId_, state.state.current);
    if (!verified.succeeded()) {
        return enterFailedClosed();
    }
    const UpdateLifecycleResult launched = beginLaunch(
        verified.version, verified.path, nowMs, true,
        UpdateLifecycleAction::RolledBackAndLaunched);
    if (!launched.succeeded()) {
        return enterFailedClosed();
    }
    return UpdateLifecycleAction::RolledBackAndLaunched;
}

void UpdateLifecycleCoordinator::stopCurrentAttempt()
{
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

void UpdateLifecycleCoordinator::record(
    const SafeEventPhase phase,
    const SafeEventCode code,
    const qint64 nowMs,
    const qint64 durationMs,
    const SafeMetrics &metrics) const
{
    if (recorder_ == nullptr || currentVersion_.isEmpty()) return;
    const SafeEventResult event = SafeEvent::create(
        nowMs, appId_, currentVersion_, phase, code, durationMs,
        QStringLiteral("/"), metrics);
    if (event.hasValue()) (void)recorder_->record(event.value());
}
