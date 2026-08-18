#pragma once

#include <QtGlobal>

#include <functional>

struct WorkerSupervisionPolicy final {
    qint64 healthWindowMs = 30'000;
    qint64 heartbeatTimeoutMs = 5'000;
};

enum class WorkerExitReason {
    Clean,
    Crashed,
    StartupFailure,
};

enum class WorkerSupervisionAction {
    None,
    Restart,
    CrashLoopRollback,
    IgnoredStaleGeneration,
};

class WorkerSupervisor final
{
public:
    using Callback = std::function<void()>;

    WorkerSupervisor(WorkerSupervisionPolicy policy,
                     Callback restart,
                     Callback rollback);

    quint64 beginActivation(qint64 nowMs);
    void heartbeat(quint64 generation, qint64 nowMs);
    WorkerSupervisionAction checkHealth(quint64 generation, qint64 nowMs);
    WorkerSupervisionAction workerExited(quint64 generation,
                                         WorkerExitReason reason,
                                         qint64 nowMs);
    bool isCrashLoop() const noexcept;
    quint64 activeGeneration() const noexcept;

private:
    WorkerSupervisionAction unexpectedFailure(qint64 nowMs);

    WorkerSupervisionPolicy policy_;
    Callback restart_;
    Callback rollback_;
    quint64 generation_ = 0;
    qint64 launchTimeMs_ = 0;
    qint64 lastHeartbeatMs_ = 0;
    int restartCount_ = 0;
    bool crashLoop_ = false;
};
