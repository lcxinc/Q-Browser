#include "WorkerSupervisor.h"

#include <limits>
#include <utility>

namespace {

quint64 nextId(const quint64 current) noexcept
{
    return current == std::numeric_limits<quint64>::max() ? 1 : current + 1;
}

} // namespace

WorkerSupervisor::WorkerSupervisor(WorkerSupervisionPolicy policy,
                                   Callback restart,
                                   Callback rollback)
    : policy_(policy), restart_(std::move(restart)), rollback_(std::move(rollback))
{
    if (policy_.healthWindowMs <= 0 || policy_.heartbeatTimeoutMs <= 0) {
        policy_ = {};
    }
}

WorkerActivationId WorkerSupervisor::beginActivation(const qint64 nowMs)
{
    activation_.value = nextId(activation_.value);
    state_ = WorkerSupervisorState::Stopped;
    activationStartMs_ = nowMs;
    lastHeartbeatMs_ = nowMs;
    restartUsed_ = false;
    rollbackCalled_ = false;
    hasAttempt_ = false;
    hasLastFailedAttempt_ = false;
    crashLoop_ = false;
    return activation_;
}

std::optional<WorkerAttemptId> WorkerSupervisor::beginAttempt(
    const WorkerActivationId activation,
    const qint64 nowMs)
{
    if (activation != activation_ || state_ != WorkerSupervisorState::Stopped
        || crashLoop_) {
        return std::nullopt;
    }
    attempt_.value = nextId(attempt_.value);
    hasAttempt_ = true;
    state_ = WorkerSupervisorState::Running;
    lastHeartbeatMs_ = nowMs;
    return attempt_;
}

void WorkerSupervisor::heartbeat(const WorkerAttemptKey key, const qint64 nowMs)
{
    if (state_ == WorkerSupervisorState::Running && isCurrent(key)
        && nowMs >= lastHeartbeatMs_) {
        lastHeartbeatMs_ = nowMs;
    }
}

WorkerSupervisionAction WorkerSupervisor::checkHealth(const WorkerAttemptKey key,
                                                      const qint64 nowMs)
{
    if (isDuplicateFailure(key)) {
        return WorkerSupervisionAction::IgnoredDuplicateFailure;
    }
    if (!isCurrent(key)) {
        return WorkerSupervisionAction::IgnoredStaleAttempt;
    }
    if (state_ != WorkerSupervisorState::Running
        || nowMs <= lastHeartbeatMs_ + policy_.heartbeatTimeoutMs) {
        return WorkerSupervisionAction::None;
    }
    return unexpectedFailure(key, nowMs);
}

WorkerSupervisionAction WorkerSupervisor::workerExited(const WorkerAttemptKey key,
                                                       const WorkerExitReason reason,
                                                       const qint64 nowMs)
{
    if (isDuplicateFailure(key)) {
        return WorkerSupervisionAction::IgnoredDuplicateFailure;
    }
    if (!isCurrent(key)) {
        return WorkerSupervisionAction::IgnoredStaleAttempt;
    }
    if (state_ != WorkerSupervisorState::Running) {
        return WorkerSupervisionAction::None;
    }
    if (reason == WorkerExitReason::Clean) {
        state_ = WorkerSupervisorState::Retired;
        return WorkerSupervisionAction::None;
    }
    return unexpectedFailure(key, nowMs);
}

bool WorkerSupervisor::isCrashLoop() const noexcept { return crashLoop_; }
WorkerSupervisorState WorkerSupervisor::state() const noexcept { return state_; }
WorkerActivationId WorkerSupervisor::activeActivation() const noexcept { return activation_; }

std::optional<WorkerAttemptId> WorkerSupervisor::activeAttempt() const noexcept
{
    return hasAttempt_ && state_ == WorkerSupervisorState::Running
        ? std::optional<WorkerAttemptId>(attempt_)
        : std::nullopt;
}

bool WorkerSupervisor::isCurrent(const WorkerAttemptKey key) const noexcept
{
    return hasAttempt_ && key.activation == activation_ && key.attempt == attempt_;
}

bool WorkerSupervisor::isDuplicateFailure(const WorkerAttemptKey key) const noexcept
{
    return hasLastFailedAttempt_ && key == lastFailedAttempt_;
}

WorkerSupervisionAction WorkerSupervisor::unexpectedFailure(
    const WorkerAttemptKey key,
    const qint64 nowMs)
{
    lastFailedAttempt_ = key;
    hasLastFailedAttempt_ = true;
    state_ = WorkerSupervisorState::Stopped;
    if (!restartUsed_) {
        restartUsed_ = true;
        if (restart_) restart_(activation_);
        return WorkerSupervisionAction::Restart;
    }
    if (nowMs <= activationStartMs_ + policy_.healthWindowMs) {
        crashLoop_ = true;
        state_ = WorkerSupervisorState::Retired;
        if (!rollbackCalled_) {
            rollbackCalled_ = true;
            if (rollback_) rollback_(activation_);
        }
        return WorkerSupervisionAction::CrashLoopRollback;
    }
    activationStartMs_ = nowMs;
    restartUsed_ = true;
    if (restart_) restart_(activation_);
    return WorkerSupervisionAction::Restart;
}
