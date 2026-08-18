#include "WorkerSupervisor.h"

#include <limits>
#include <utility>

WorkerSupervisor::WorkerSupervisor(WorkerSupervisionPolicy policy,
                                   Callback restart,
                                   Callback rollback)
    : policy_(policy), restart_(std::move(restart)), rollback_(std::move(rollback))
{
    if (policy_.healthWindowMs <= 0 || policy_.heartbeatTimeoutMs <= 0) {
        policy_ = {};
    }
}

quint64 WorkerSupervisor::beginActivation(const qint64 nowMs)
{
    if (generation_ == std::numeric_limits<quint64>::max()) {
        generation_ = 1;
    } else {
        ++generation_;
    }
    launchTimeMs_ = nowMs;
    lastHeartbeatMs_ = nowMs;
    restartCount_ = 0;
    crashLoop_ = false;
    return generation_;
}

void WorkerSupervisor::heartbeat(const quint64 generation, const qint64 nowMs)
{
    if (generation == generation_ && nowMs >= lastHeartbeatMs_) {
        lastHeartbeatMs_ = nowMs;
    }
}

WorkerSupervisionAction WorkerSupervisor::checkHealth(const quint64 generation,
                                                      const qint64 nowMs)
{
    if (generation != generation_) {
        return WorkerSupervisionAction::IgnoredStaleGeneration;
    }
    if (crashLoop_ || nowMs <= lastHeartbeatMs_ + policy_.heartbeatTimeoutMs) {
        return WorkerSupervisionAction::None;
    }
    return unexpectedFailure(nowMs);
}

WorkerSupervisionAction WorkerSupervisor::workerExited(const quint64 generation,
                                                       const WorkerExitReason reason,
                                                       const qint64 nowMs)
{
    if (generation != generation_) {
        return WorkerSupervisionAction::IgnoredStaleGeneration;
    }
    if (reason == WorkerExitReason::Clean) {
        return WorkerSupervisionAction::None;
    }
    return unexpectedFailure(nowMs);
}

bool WorkerSupervisor::isCrashLoop() const noexcept
{
    return crashLoop_;
}

quint64 WorkerSupervisor::activeGeneration() const noexcept
{
    return generation_;
}

WorkerSupervisionAction WorkerSupervisor::unexpectedFailure(const qint64 nowMs)
{
    if (crashLoop_) {
        return WorkerSupervisionAction::CrashLoopRollback;
    }
    const bool insideHealthWindow = nowMs <= launchTimeMs_ + policy_.healthWindowMs;
    if (restartCount_ == 0) {
        ++restartCount_;
        launchTimeMs_ = nowMs;
        lastHeartbeatMs_ = nowMs;
        if (restart_) restart_();
        return WorkerSupervisionAction::Restart;
    }
    if (insideHealthWindow) {
        crashLoop_ = true;
        if (rollback_) rollback_();
        return WorkerSupervisionAction::CrashLoopRollback;
    }
    restartCount_ = 1;
    launchTimeMs_ = nowMs;
    lastHeartbeatMs_ = nowMs;
    if (restart_) restart_();
    return WorkerSupervisionAction::Restart;
}
