#pragma once

#include <QtGlobal>

#include <compare>
#include <functional>
#include <optional>

struct WorkerSupervisionPolicy final {
    qint64 healthWindowMs = 30'000;
    qint64 heartbeatTimeoutMs = 5'000;
};

struct WorkerActivationId final {
    quint64 value = 0;
    auto operator<=>(const WorkerActivationId &) const = default;
};

struct WorkerAttemptId final {
    quint64 value = 0;
    auto operator<=>(const WorkerAttemptId &) const = default;
};

struct WorkerAttemptKey final {
    WorkerActivationId activation;
    WorkerAttemptId attempt;
    auto operator<=>(const WorkerAttemptKey &) const = default;
};

enum class WorkerExitReason {
    Clean,
    Crashed,
    StartupFailure,
};

enum class WorkerSupervisorState {
    Stopped,
    Running,
    Retired,
};

enum class WorkerSupervisionAction {
    None,
    Restart,
    StartupRollback,
    CrashLoopRollback,
    IgnoredStaleAttempt,
    IgnoredDuplicateFailure,
};

class WorkerSupervisor final
{
public:
    using Callback = std::function<void(WorkerActivationId)>;

    WorkerSupervisor(WorkerSupervisionPolicy policy,
                     Callback restart,
                     Callback rollback);

    WorkerActivationId beginActivation(qint64 nowMs);
    std::optional<WorkerAttemptId> beginAttempt(WorkerActivationId activation,
                                                qint64 nowMs);
    bool authenticatedHandshake(WorkerAttemptKey key, qint64 nowMs);
    void heartbeat(WorkerAttemptKey key, qint64 nowMs);
    [[nodiscard]] bool isHealthy(WorkerAttemptKey key, qint64 nowMs) const noexcept;
    WorkerSupervisionAction checkHealth(WorkerAttemptKey key, qint64 nowMs);
    WorkerSupervisionAction workerExited(WorkerAttemptKey key,
                                         WorkerExitReason reason,
                                         qint64 nowMs);
    bool isCrashLoop() const noexcept;
    WorkerSupervisorState state() const noexcept;
    WorkerActivationId activeActivation() const noexcept;
    std::optional<WorkerAttemptId> activeAttempt() const noexcept;

private:
    bool isCurrent(WorkerAttemptKey key) const noexcept;
    bool isDuplicateFailure(WorkerAttemptKey key) const noexcept;
    WorkerSupervisionAction unexpectedFailure(WorkerAttemptKey key,
                                               qint64 nowMs);

    WorkerSupervisionPolicy policy_;
    Callback restart_;
    Callback rollback_;
    WorkerActivationId activation_;
    WorkerAttemptId attempt_;
    WorkerAttemptKey lastFailedAttempt_;
    WorkerSupervisorState state_ = WorkerSupervisorState::Retired;
    qint64 attemptStartMs_ = 0;
    qint64 handshakeMs_ = 0;
    qint64 lastHeartbeatMs_ = 0;
    bool restartUsed_ = false;
    bool rollbackCalled_ = false;
    bool hasAttempt_ = false;
    bool hasLastFailedAttempt_ = false;
    bool crashLoop_ = false;
    bool hasAuthenticatedHandshake_ = false;
};
